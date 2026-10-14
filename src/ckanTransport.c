//
// FILE            ckanTransport.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// The CKAN transport - a reader thread and a queue of resources to read. See ckanTransport.h.
//
// HTTP is the broker's own client (corRest, resolved from the running broker like corLog and corJson):
// HTTPS included, nothing linked into the plugin.
//
#include <ctype.h>                                    // isdigit
#include <errno.h>                                    // ETIMEDOUT
#include <pthread.h>                                  // pthread_*
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // malloc, free, getenv
#include <string.h>                                   // strdup, strcmp, strlen, memcpy
#include <time.h>                                     // clock_gettime, timegm

#include "corLog/corLog.h"                            // COR_E, COR_W, COR_I
#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAllocBufferInit.h"              // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"             // corAllocBufferReset
#include "corJson/CorJson.h"                          // CorJson
#include "corJson/corJsonCreate.h"                    // corJsonCreate
#include "corJson/corJsonParse.h"                     // corJsonParse
#include "corJson/corJsonRender.h"                    // corJsonFastRender
#include "corJson/corJsonRenderSize.h"                // corJsonFastRenderSize
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corRest/CorRestVerb.h"                      // CorVerbGet
#include "corRest/corRestClient.h"                    // corRestClient*

#include "corBridge/BridgeBroker.h"                   // BRIDGE_*

#include "ckanTransport.h"                            // Own interface



// -----------------------------------------------------------------------------
//
// The portal, from "server" - kept from open until close
//
static char*            portalUrl  = NULL;            // no trailing '/'
static char*            apiKey     = NULL;            // the Authorization header - NEVER logged
static int              timeoutMs  = 30000;

static CkanReceiveFunc  receiveF   = NULL;
static CkanStatusFunc   statusF    = NULL;



// -----------------------------------------------------------------------------
//
// Job - one resource to read
//
typedef struct Job
{
  char*         endpoint;
  CkanEndpoint  config;                               // config.timeColumn malloc'd
  bool          cancelled;                            // ckanTransportEndpointDel
  struct Job*   next;
} Job;



// -----------------------------------------------------------------------------
//
// The reader thread and its queue - every member under mutex
//
static pthread_mutex_t  mutex       = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   cond;                         // CLOCK_MONOTONIC - see ckanTransportOpen
static Job*             queueHead   = NULL;
static Job*             queueTail   = NULL;
static Job*             current     = NULL;           // the job being read
static bool             stopping    = false;
static bool             threadUp    = false;
static pthread_t        thread;



// -----------------------------------------------------------------------------
//
// The retries of one page: a portal that cannot be reached, a timeout, a 5xx. A 4xx is the answer.
//
static const int        retryDelayMsV[] = { 1000, 3000 };
#define RETRIES         ((int) (sizeof(retryDelayMsV) / sizeof(retryDelayMsV[0])))



// -----------------------------------------------------------------------------
//
// nowNs - CLOCK_REALTIME, nanoseconds
//
static int64_t nowNs(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_REALTIME, &ts);
  return (int64_t) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}



// -----------------------------------------------------------------------------
//
// jobFree -
//
static void jobFree(Job* jobP)
{
  free(jobP->endpoint);
  free((char*) jobP->config.timeColumn);
  free(jobP);
}



// -----------------------------------------------------------------------------
//
// shouldStop - the broker is going down, or the endpoint was taken away
//
static bool shouldStop(Job* jobP)
{
  pthread_mutex_lock(&mutex);
  bool stop = (stopping == true) || (jobP->cancelled == true);
  pthread_mutex_unlock(&mutex);

  return stop;
}



// -----------------------------------------------------------------------------
//
// waitMs - wait ms, or until close() says stop; false when it did
//
static bool waitMs(int ms)
{
  struct timespec until;

  clock_gettime(CLOCK_MONOTONIC, &until);
  until.tv_sec  += ms / 1000;
  until.tv_nsec += (ms % 1000) * 1000000L;
  if (until.tv_nsec >= 1000000000L)
  {
    until.tv_sec  += 1;
    until.tv_nsec -= 1000000000L;
  }

  pthread_mutex_lock(&mutex);
  while ((stopping == false) && (pthread_cond_timedwait(&cond, &mutex, &until) != ETIMEDOUT))
    ;
  bool go = (stopping == false);
  pthread_mutex_unlock(&mutex);

  return go;
}



// -----------------------------------------------------------------------------
//
// httpGet - GET a URL; the body, malloc'd and NUL-terminated, or NULL (*whyP, *transientP)
//
// ⚠ The body is copied the moment the call returns, and nothing else of the response is read after
// it. Up to corRest#35, the response's body pointed into the connection's receive buffer, which the
// client releases before corRestClientSend returns (freed outright on 'Connection: close'): a broker
// built before that fix leaves a window here that the copy narrows and cannot close.
//
// ⚠ The response is the broker's struct, filled in by the broker's corRest: built against older corRest
// headers than the broker's, this plugin's CorRestClientResponse would be shorter than what the broker
// writes into it (corRest#35 appended two members). The padding keeps a broker's newer members inside
// memory of this frame.
//
static char* httpGet(const char* url, int* statusP, char* why, int whySize, bool* transientP)
{
  CorRestClientRequest  req;
  union
  {
    CorRestClientResponse resp;
    char                  pad[sizeof(CorRestClientResponse) + 256];
  } respU;
  CorRestClientResponse* respP = &respU.resp;

  memset(&respU, 0, sizeof(respU));
  *transientP = false;

  corRestClientRequestInit(&req, CorVerbGet, url, NULL);
  corRestClientRequestHeader(&req, "Accept", "application/json");

  if (apiKey != NULL)
    corRestClientRequestHeader(&req, "Authorization", apiKey);

  corRestClientRequestTimeout(&req, timeoutMs, timeoutMs);

  int   rc   = corRestClientSend(&req, respP);
  char* body = NULL;

  if (rc == CORR_OK)
  {
    int len = (respP->body != NULL) ? respP->bodyLen : 0;

    body = (char*) malloc(len + 1);

    if (body != NULL)
    {
      if (len > 0)
        memcpy(body, respP->body, len);
      body[len] = 0;
    }

    *statusP = respP->statusCode;
  }
  else
  {
    snprintf(why, whySize, "the portal cannot be reached: %s (%d)", (respP->errorDetail[0] != 0) ? respP->errorDetail : "no answer", rc);
    *transientP = true;
  }

  corRestClientResponseCleanup(respP);
  corRestClientRequestCleanup(&req);

  return body;
}



// -----------------------------------------------------------------------------
//
// ckanError - the "error" of a CKAN Action API answer as one line: "Not Found Error: Not found: ..."
//
static void ckanError(CorNode* answerP, int status, char* why, int whySize)
{
  CorNode*    errorP   = (answerP != NULL) ? corTreeLookup(answerP, "error") : NULL;
  CorNode*    typeP    = (errorP  != NULL) ? corTreeLookup(errorP, "__type")  : NULL;
  CorNode*    messageP = (errorP  != NULL) ? corTreeLookup(errorP, "message") : NULL;
  const char* type     = ((typeP    != NULL) && (typeP->type    == CorString)) ? typeP->value.s    : NULL;
  const char* message  = ((messageP != NULL) && (messageP->type == CorString)) ? messageP->value.s : NULL;

  //
  // A validation error has no "message": its members are the fields, each an array of complaints
  //
  if ((message == NULL) && (errorP != NULL) && (errorP->type == CorObject))
  {
    for (CorNode* fieldP = errorP->value.head; fieldP != NULL; fieldP = fieldP->next)
    {
      if ((fieldP->type == CorArray) && (fieldP->value.head != NULL) && (fieldP->value.head->type == CorString))
      {
        message = fieldP->value.head->value.s;
        break;
      }
    }
  }

  if ((type != NULL) && (message != NULL))
    snprintf(why, whySize, "the portal answered %d - %s: %s", status, type, message);
  else if ((type != NULL) || (message != NULL))
    snprintf(why, whySize, "the portal answered %d - %s", status, (type != NULL) ? type : message);
  else
    snprintf(why, whySize, "the portal answered %d, and not as CKAN's Action API does", status);
}



// -----------------------------------------------------------------------------
//
// recordTime - a record's time: its time column; 0 - no time, and the broker writes no observedAt - when the
//              DataChannel names none, or the record's is not a date-time. The read time is not the time of
//              the data, and is not made to look like it
//
static int64_t recordTime(Job* jobP, CorNode* recordP, bool* warnedP)
{
  if (jobP->config.timeColumn == NULL)
    return 0;

  CorNode* timeP = corTreeLookup(recordP, jobP->config.timeColumn);
  int64_t  ns    = ((timeP != NULL) && (timeP->type == CorString)) ? ckanTimeParse(timeP->value.s) : -1;

  if (ns > 0)
    return ns;

  if (*warnedP == false)
  {
    COR_W("ckan: '%s': a record's '%s' is not an ISO 8601 date-time - it has no time (said once per read)",
          jobP->endpoint, jobP->config.timeColumn);
    *warnedP = true;
  }

  return 0;
}



// -----------------------------------------------------------------------------
//
// PageResult - what one page told
//
typedef struct PageResult
{
  int        records;                                 // on this page
  int        taken;                                   // of them, taken by the broker
  long long  total;                                   // the resource's, -1 when the answer did not say
} PageResult;



// -----------------------------------------------------------------------------
//
// pageRead - one datastore_search call, its records handed over; false (and why) when it failed
//
static bool pageRead(Job* jobP, int offset, PageResult* resultP, bool* warnedP, char* why, int whySize)
{
  char url[2048];

  snprintf(url, sizeof(url), "%s/api/3/action/datastore_search?resource_id=%s&limit=%d&offset=%d&sort=_id",
           portalUrl, jobP->endpoint, jobP->config.pageSize, offset);

  char* body      = NULL;
  int   status    = 0;
  bool  transient = false;

  for (int attempt = 0; attempt <= RETRIES; attempt++)
  {
    body = httpGet(url, &status, why, whySize, &transient);

    if ((body != NULL) && (status >= 500))
    {
      snprintf(why, whySize, "the portal answered %d", status);
      transient = true;
      free(body);
      body = NULL;
    }

    if ((body != NULL) || (transient == false) || (attempt == RETRIES))
      break;

    COR_W("ckan: '%s', offset %d: %s - again in %d ms", jobP->endpoint, offset, why, retryDelayMsV[attempt]);

    if (waitMs(retryDelayMsV[attempt]) == false)
      return false;
  }

  if (body == NULL)
    return false;

  char     buf[16 * 1024];
  CorAlloc ka;
  CorJson  cj;

  corAllocBufferInit(&ka, buf, sizeof(buf), 256 * 1024, NULL, "ckan page");
  corJsonCreate(&cj, &ka);

  CorNode* answerP  = corJsonParse(&cj, body);
  CorNode* successP = (answerP  != NULL) ? corTreeLookup(answerP, "success") : NULL;
  CorNode* answerResultP = (answerP != NULL) ? corTreeLookup(answerP, "result") : NULL;
  CorNode* recordsP = (answerResultP != NULL) ? corTreeLookup(answerResultP, "records") : NULL;
  CorNode* totalP   = (answerResultP != NULL) ? corTreeLookup(answerResultP, "total")   : NULL;
  bool     ok       = false;

  if (answerP == NULL)
    snprintf(why, whySize, "the portal answered %d, and not with JSON", status);
  else if ((status != 200) || (successP == NULL) || (successP->type != CorBoolean) || (successP->value.b == false))
    ckanError(answerP, status, why, whySize);
  else if ((recordsP == NULL) || (recordsP->type != CorArray))
    snprintf(why, whySize, "the portal's answer has no result.records - not a DataStore resource?");
  else
    ok = true;

  if (ok == true)
  {
    resultP->records = 0;
    resultP->taken   = 0;
    resultP->total   = ((totalP != NULL) && (totalP->type == CorInt)) ? totalP->value.i : -1;

    for (CorNode* recordP = recordsP->value.head; recordP != NULL; recordP = recordP->next)
    {
      resultP->records += 1;

      if (recordP->type != CorObject)
        continue;

      if (shouldStop(jobP) == true)
        break;

      //
      // Rendered detached - corJsonFastRender follows the sibling chain and renders a name
      //
      char*    name = recordP->name;
      CorNode* next = recordP->next;

      recordP->name = NULL;
      recordP->next = NULL;

      int   size = corJsonFastRenderSize(recordP);
      char* text = (char*) malloc(size + 1);

      if (text != NULL)
      {
        corJsonFastRender(recordP, text);

        if (receiveF(jobP->endpoint, text, recordTime(jobP, recordP, warnedP)) == true)
          resultP->taken += 1;

        free(text);
      }

      recordP->name = name;
      recordP->next = next;
    }
  }

  corAllocBufferReset(&ka, false);
  free(body);                                         // after the tree: its strings point into it

  return ok;
}



// -----------------------------------------------------------------------------
//
// resourceRead - mode once: the whole resource, page by page
//
static void resourceRead(Job* jobP)
{
  char     why[512];
  int      offset  = 0;
  int      pages   = 0;
  int      taken   = 0;
  bool     warned  = false;
  int64_t  startNs = nowNs();

  COR_I("ckan: reading '%s' from %s, %d records a page", jobP->endpoint, portalUrl, jobP->config.pageSize);

  while (shouldStop(jobP) == false)
  {
    PageResult page;

    if (pageRead(jobP, offset, &page, &warned, why, sizeof(why)) == false)
    {
      if (shouldStop(jobP) == true)
        return;

      COR_E("ckan: '%s', offset %d: %s", jobP->endpoint, offset, why);
      statusF(jobP->endpoint, false, why);
      return;
    }

    pages  += 1;
    offset += page.records;
    taken  += page.taken;

    if ((page.records < jobP->config.pageSize) || ((page.total >= 0) && (offset >= page.total)))
      break;
  }

  if (shouldStop(jobP) == true)
    return;

  COR_I("ckan: '%s' read: %d records in %d page%s, %d taken, %.3f s", jobP->endpoint, offset, pages, (pages == 1) ? "" : "s",
        taken, (double) (nowNs() - startNs) / 1e9);
}



// -----------------------------------------------------------------------------
//
// readerThread - the queue, one resource at a time
//
static void* readerThread(void* arg)
{
  (void) arg;

  pthread_mutex_lock(&mutex);

  while (stopping == false)
  {
    if (queueHead == NULL)
    {
      pthread_cond_wait(&cond, &mutex);
      continue;
    }

    Job* jobP = queueHead;

    queueHead = jobP->next;
    if (queueHead == NULL)
      queueTail = NULL;

    current = jobP;
    pthread_mutex_unlock(&mutex);

    if (jobP->cancelled == false)
      resourceRead(jobP);

    pthread_mutex_lock(&mutex);
    current = NULL;
    jobFree(jobP);
  }

  pthread_mutex_unlock(&mutex);
  return NULL;
}



// -----------------------------------------------------------------------------
//
// ckanTransportOpen -
//
int ckanTransportOpen(CorNode* serverP, CkanReceiveFunc _receiveF, CkanStatusFunc _statusF)
{
  CorNode*    urlP       = (serverP != NULL) ? corTreeLookup(serverP, "url")       : NULL;
  CorNode*    keyP       = (serverP != NULL) ? corTreeLookup(serverP, "apiKey")    : NULL;
  CorNode*    keyEnvP    = (serverP != NULL) ? corTreeLookup(serverP, "apiKeyEnv") : NULL;
  CorNode*    timeoutP   = (serverP != NULL) ? corTreeLookup(serverP, "timeoutMs") : NULL;
  const char* url        = ((urlP != NULL) && (urlP->type == CorString)) ? urlP->value.s : NULL;

  if ((url == NULL) || ((strncmp(url, "http://", 7) != 0) && (strncmp(url, "https://", 8) != 0)))
  {
    COR_E("ckan: server.url must be the portal's http(s) URL, e.g. \"https://data.boston.gov\"");
    return BRIDGE_ERR;
  }

  if ((keyP != NULL) && (keyEnvP != NULL))
  {
    COR_E("ckan: server.apiKey and server.apiKeyEnv - one or the other");
    return BRIDGE_ERR;
  }

  if (keyP != NULL)
  {
    if ((keyP->type != CorString) || (keyP->value.s[0] == 0))
    {
      COR_E("ckan: server.apiKey must be a non-empty string");
      return BRIDGE_ERR;
    }
    apiKey = strdup(keyP->value.s);
  }
  else if (keyEnvP != NULL)
  {
    const char* key = ((keyEnvP->type == CorString) && (keyEnvP->value.s[0] != 0)) ? getenv(keyEnvP->value.s) : NULL;

    if ((key == NULL) || (*key == 0))
    {
      COR_E("ckan: server.apiKeyEnv names no environment variable that holds a key");
      return BRIDGE_ERR;
    }
    apiKey = strdup(key);
  }

  if (timeoutP != NULL)
  {
    if ((timeoutP->type != CorInt) || (timeoutP->value.i < 100) || (timeoutP->value.i > 600000))
    {
      COR_E("ckan: server.timeoutMs must be an integer from 100 to 600000");
      free(apiKey);
      apiKey = NULL;
      return BRIDGE_ERR;
    }
    timeoutMs = (int) timeoutP->value.i;
  }

  for (CorNode* memberP = (serverP != NULL) ? serverP->value.head : NULL; memberP != NULL; memberP = memberP->next)
  {
    if ((strcmp(memberP->name, "url") != 0) && (strcmp(memberP->name, "apiKey") != 0) &&
        (strcmp(memberP->name, "apiKeyEnv") != 0) && (strcmp(memberP->name, "timeoutMs") != 0))
      COR_W("ckan: server.%s - not a setting of this bridge (url, apiKey, apiKeyEnv, timeoutMs) - ignored", memberP->name);
  }

  portalUrl = strdup(url);

  int len = strlen(portalUrl);
  while ((len > 0) && (portalUrl[len - 1] == '/'))
    portalUrl[--len] = 0;

  receiveF = _receiveF;
  statusF  = _statusF;
  stopping = false;

  pthread_condattr_t attr;

  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  pthread_cond_init(&cond, &attr);
  pthread_condattr_destroy(&attr);

  if (pthread_create(&thread, NULL, readerThread, NULL) != 0)
  {
    COR_E("ckan: cannot start the reader thread");
    return BRIDGE_ERR;
  }

  threadUp = true;

  COR_I("ckan: portal %s%s", portalUrl, (apiKey != NULL) ? ", with an API key" : "");
  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// ckanTransportClose -
//
void ckanTransportClose(void)
{
  pthread_mutex_lock(&mutex);
  stopping = true;
  pthread_cond_broadcast(&cond);
  pthread_mutex_unlock(&mutex);

  if (threadUp == true)
  {
    pthread_join(thread, NULL);
    threadUp = false;
  }

  while (queueHead != NULL)
  {
    Job* nextP = queueHead->next;
    jobFree(queueHead);
    queueHead = nextP;
  }
  queueTail = NULL;

  free(portalUrl);
  free(apiKey);
  portalUrl = NULL;
  apiKey    = NULL;
}



// -----------------------------------------------------------------------------
//
// resourceIdCheck - what a resource id may hold: a UUID, or a DataStore alias - [A-Za-z0-9._-]
//
// Checked rather than percent-encoded: the id goes into the URL as it is, and a character outside
// these is a mistake in the configuration, not a resource.
//
static bool resourceIdCheck(const char* endpoint)
{
  if ((endpoint == NULL) || (*endpoint == 0) || (strlen(endpoint) > 200))
    return false;

  for (const char* cP = endpoint; *cP != 0; cP++)
  {
    char c = *cP;

    if (!(((c >= 'A') && (c <= 'Z')) || ((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9')) || (c == '.') || (c == '_') || (c == '-')))
      return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// ckanTransportEndpointAdd -
//
int ckanTransportEndpointAdd(const char* endpoint, const CkanEndpoint* configP, const char** whyP)
{
  if (resourceIdCheck(endpoint) == false)
  {
    *whyP = "not a CKAN resource id - a UUID or a DataStore alias: letters, digits, '.', '_', '-'";
    return BRIDGE_BAD_INPUT;
  }

  Job* jobP = (Job*) calloc(1, sizeof(Job));

  if (jobP == NULL)
    return BRIDGE_ERR;

  jobP->endpoint          = strdup(endpoint);
  jobP->config            = *configP;
  jobP->config.timeColumn = (configP->timeColumn != NULL) ? strdup(configP->timeColumn) : NULL;

  pthread_mutex_lock(&mutex);

  if (queueTail == NULL)
    queueHead = jobP;
  else
    queueTail->next = jobP;
  queueTail = jobP;

  pthread_cond_broadcast(&cond);
  pthread_mutex_unlock(&mutex);

  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// ckanTransportEndpointDel -
//
int ckanTransportEndpointDel(const char* endpoint)
{
  int r = BRIDGE_NOT_FOUND;

  pthread_mutex_lock(&mutex);

  if ((current != NULL) && (strcmp(current->endpoint, endpoint) == 0))
  {
    current->cancelled = true;
    r = BRIDGE_OK;
  }

  for (Job* jobP = queueHead; jobP != NULL; jobP = jobP->next)
  {
    if (strcmp(jobP->endpoint, endpoint) == 0)
    {
      jobP->cancelled = true;
      r = BRIDGE_OK;
    }
  }

  pthread_mutex_unlock(&mutex);

  return r;
}



// -----------------------------------------------------------------------------
//
// digits - n digits at *sP as a number; -1 when they are not all there
//
static int digits(const char** sP, int n)
{
  int v = 0;

  for (int ix = 0; ix < n; ix++)
  {
    if (isdigit((unsigned char) (*sP)[ix]) == 0)
      return -1;
    v = v * 10 + ((*sP)[ix] - '0');
  }

  *sP += n;
  return v;
}



// -----------------------------------------------------------------------------
//
// ckanTimeParse -
//
int64_t ckanTimeParse(const char* text)
{
  const char* s = text;
  struct tm   tm;
  int64_t     fracNs = 0;
  int         offsetS = 0;

  memset(&tm, 0, sizeof(tm));

  int year  = digits(&s, 4);  if ((year  < 0) || (*s++ != '-')) return -1;
  int month = digits(&s, 2);  if ((month < 1) || (month > 12) || (*s++ != '-')) return -1;
  int day   = digits(&s, 2);  if ((day   < 1) || (day   > 31)) return -1;

  tm.tm_year = year - 1900;
  tm.tm_mon  = month - 1;
  tm.tm_mday = day;

  if ((*s == 'T') || (*s == ' '))
  {
    s++;

    int hour = digits(&s, 2);  if ((hour < 0) || (hour > 23) || (*s++ != ':')) return -1;
    int min  = digits(&s, 2);  if ((min  < 0) || (min  > 59)) return -1;
    int sec  = 0;

    if (*s == ':')
    {
      s++;
      sec = digits(&s, 2);
      if ((sec < 0) || (sec > 60))
        return -1;
    }

    if ((*s == '.') || (*s == ','))
    {
      int64_t scale = 100000000;

      s++;
      if (isdigit((unsigned char) *s) == 0)
        return -1;

      while (isdigit((unsigned char) *s))
      {
        fracNs += (*s - '0') * scale;
        scale  /= 10;
        s++;
      }
    }

    tm.tm_hour = hour;
    tm.tm_min  = min;
    tm.tm_sec  = sec;

    if (*s == 'Z')
      s++;
    else if ((*s == '+') || (*s == '-'))
    {
      int sign = (*s == '-') ? -1 : 1;

      s++;
      int oh = digits(&s, 2);
      if (oh < 0)
        return -1;
      if (*s == ':')
        s++;
      int om = digits(&s, 2);
      if (om < 0)
        return -1;

      offsetS = sign * (oh * 3600 + om * 60);
    }
  }

  if (*s != 0)
    return -1;

  time_t t = timegm(&tm);

  if (t == (time_t) -1)
    return -1;

  return ((int64_t) t - offsetS) * 1000000000LL + fracNs;
}
