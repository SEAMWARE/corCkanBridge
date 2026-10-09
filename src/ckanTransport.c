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
// The example transport - a directory of files, standing in for a device. See ckanTransport.h.
//
//   "ckan": { "server": { "dir": "/var/lib/ckan", "pollMs": 1000 }, "ngsild": { ... } }
//
// An endpoint is a file path relative to dir ("tank1/level"). One thread polls every inbound
// endpoint's file and reports its content when it changed - a value is reported once, not once per
// poll. A published value is written into the file (to a temporary file, then renamed, so the poller
// never reads half of one) and remembered as the file's last content, so it does not come back in as
// a sample.
//
// TODO: replace this file with your protocol: connect in ckanTransportOpen, subscribe in
//       ckanTransportEndpointAdd, send in ckanTransportSend, and call receiveF from the thread your
//       protocol library delivers on.
//
#include <errno.h>                                    // errno
#include <limits.h>                                   // PATH_MAX
#include <pthread.h>                                  // pthread_*
#include <stdio.h>                                    // snprintf, fopen, fread, fwrite, rename
#include <stdlib.h>                                   // malloc, free
#include <string.h>                                   // strcmp, strdup, strstr, strspn, strrchr, memcpy
#include <sys/stat.h>                                 // stat, mkdir
#include <time.h>                                     // nanosleep
#include <unistd.h>                                   // unlink

#include "corLog/corLog.h"                            // COR_E, COR_W, COR_V
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "corBridge/BridgeBroker.h"                   // BRIDGE_*

#include "ckanTransport.h"                            // Own interface



// -----------------------------------------------------------------------------
//
// CkanEndpoint - one endpoint the transport carries
//
// TODO: what your protocol needs per endpoint - a subscription handle, a node id, a register address
//
typedef struct CkanEndpoint
{
  bool             inUse;
  char*            endpoint;                          // as the broker names it
  char*            path;                              // <dir>/<endpoint>
  BridgeDirection  direction;
  bool             text;                              // plain text: a value arrives as a JSON string
  char*            last;                              // the file's content when last read or written - NULL: none yet
} CkanEndpoint;



// -----------------------------------------------------------------------------
//
// Module state - the endpoints are under mtx: the broker adds them on its threads, the poller reads them
//
static CkanEndpoint     endpoints[64];
static pthread_mutex_t  mtx         = PTHREAD_MUTEX_INITIALIZER;
static char             dir[PATH_MAX];
static int              pollMs      = 1000;
static CkanReceiveFunc  receiveF    = NULL;
static pthread_t        poller;
static volatile bool    running     = false;



// -----------------------------------------------------------------------------
//
// fileRead - a file's content, trailing whitespace removed; malloc'd. NULL: not there, or empty
//
static char* fileRead(const char* path)
{
  FILE* fP = fopen(path, "r");
  if (fP == NULL)
    return NULL;

  char   buf[16 * 1024];
  size_t n = fread(buf, 1, sizeof(buf) - 1, fP);
  fclose(fP);

  while ((n > 0) && ((buf[n - 1] == '\n') || (buf[n - 1] == '\r') || (buf[n - 1] == ' ') || (buf[n - 1] == '\t')))
    n--;

  if (n == 0)                                         // empty - or caught between truncate and write
    return NULL;

  char* content = malloc(n + 1);
  if (content != NULL)
  {
    memcpy(content, buf, n);
    content[n] = 0;
  }

  return content;
}



// -----------------------------------------------------------------------------
//
// jsonString - text as a JSON string, escaped; malloc'd
//
static char* jsonString(const char* text)
{
  char* out = malloc(strlen(text) * 6 + 3);
  char* oP  = out;

  if (out == NULL)
    return NULL;

  *oP++ = '"';
  for (const unsigned char* cP = (const unsigned char*) text; *cP != 0; cP++)
  {
    if      (*cP == '"')   { *oP++ = '\\'; *oP++ = '"';  }
    else if (*cP == '\\')  { *oP++ = '\\'; *oP++ = '\\'; }
    else if (*cP == '\n')  { *oP++ = '\\'; *oP++ = 'n';  }
    else if (*cP == '\t')  { *oP++ = '\\'; *oP++ = 't';  }
    else if (*cP < 0x20)   oP += sprintf(oP, "\\u%04x", *cP);
    else                   *oP++ = *cP;
  }
  *oP++ = '"';
  *oP   = 0;

  return out;
}



// -----------------------------------------------------------------------------
//
// pollOnce - every inbound endpoint whose file changed, handed to receiveF
//
// receiveF is called WITHOUT mtx held: the broker takes its own locks inside sampleIn, and a broker
// thread inside channelAdd or publish may be waiting for mtx at that moment.
//
static void pollOnce(void)
{
  for (int ix = 0; ix < (int) (sizeof(endpoints) / sizeof(endpoints[0])); ix++)
  {
    char*   endpoint = NULL;
    char*   json     = NULL;
    int64_t timeNs   = 0;

    pthread_mutex_lock(&mtx);

    CkanEndpoint* eP = &endpoints[ix];

    if ((eP->inUse == true) && (eP->direction != BridgeDirectionOut))
    {
      char* content = fileRead(eP->path);

      if ((content != NULL) && ((eP->last == NULL) || (strcmp(content, eP->last) != 0)))
      {
        struct stat st;

        if (stat(eP->path, &st) == 0)                 // the transport's own time: when the file was written
          timeNs = (int64_t) st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;

        endpoint = strdup(eP->endpoint);
        json     = (eP->text == true) ? jsonString(content) : strdup(content);

        free(eP->last);
        eP->last = content;
      }
      else
        free(content);
    }

    pthread_mutex_unlock(&mtx);

    if ((endpoint != NULL) && (json != NULL))
      receiveF(endpoint, json, timeNs);

    free(endpoint);
    free(json);
  }
}



// -----------------------------------------------------------------------------
//
// pollThread - the transport's own thread: the one every sample arrives on
//
static void* pollThread(void* arg)
{
  (void) arg;

  struct timespec nap = { pollMs / 1000, (pollMs % 1000) * 1000000L };

  while (running == true)
  {
    pollOnce();
    nanosleep(&nap, NULL);
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// dirsMake - every directory above a file, as mkdir -p
//
static void dirsMake(const char* path)
{
  char copy[PATH_MAX];

  snprintf(copy, sizeof(copy), "%s", path);

  for (char* slashP = strchr(&copy[1], '/'); slashP != NULL; slashP = strchr(&slashP[1], '/'))
  {
    *slashP = 0;
    if ((mkdir(copy, 0755) != 0) && (errno != EEXIST))
      COR_W("ckan: cannot create '%s': %s", copy, strerror(errno));
    *slashP = '/';
  }
}



// -----------------------------------------------------------------------------
//
// ckanTransportOpen -
//
int ckanTransportOpen(CorNode* serverP, CkanReceiveFunc _receiveF)
{
  CorNode* dirP    = (serverP != NULL) ? corTreeLookup(serverP, "dir")    : NULL;
  CorNode* pollMsP = (serverP != NULL) ? corTreeLookup(serverP, "pollMs") : NULL;

  if ((dirP == NULL) || (dirP->type != CorString) || (dirP->value.s[0] != '/'))
  {
    COR_E("ckan: server.dir is missing, or not an absolute path, in the bridge configuration");
    return BRIDGE_ERR;
  }

  if ((pollMsP != NULL) && ((pollMsP->type != CorInt) || (pollMsP->value.i < 10) || (pollMsP->value.i > 3600000)))
  {
    COR_E("ckan: server.pollMs must be an integer from 10 to 3600000");
    return BRIDGE_ERR;
  }

  snprintf(dir, sizeof(dir), "%s", dirP->value.s);
  pollMs   = (pollMsP != NULL) ? (int) pollMsP->value.i : 1000;
  receiveF = _receiveF;

  //
  // The thread starts now and polls nothing until the broker adds an endpoint - it adds them after init
  //
  running = true;
  if (pthread_create(&poller, NULL, pollThread, NULL) != 0)
  {
    running = false;
    COR_E("ckan: cannot start the poll thread");
    return BRIDGE_ERR;
  }

  COR_V("ckan: polling '%s' every %d ms", dir, pollMs);
  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// ckanTransportClose -
//
void ckanTransportClose(void)
{
  if (running == true)
  {
    running = false;
    pthread_join(poller, NULL);                       // the one thread that calls receiveF - gone when this returns
  }

  pthread_mutex_lock(&mtx);
  for (int ix = 0; ix < (int) (sizeof(endpoints) / sizeof(endpoints[0])); ix++)
  {
    CkanEndpoint* eP = &endpoints[ix];

    free(eP->endpoint);
    free(eP->path);
    free(eP->last);
    memset(eP, 0, sizeof(*eP));
  }
  pthread_mutex_unlock(&mtx);
}



// -----------------------------------------------------------------------------
//
// ckanTransportEndpointAdd -
//
// An endpoint is a relative path of letters, digits, '_', '-', '.' and '/', with no '..' - a Channel
// cannot reach out of the directory.
//
int ckanTransportEndpointAdd(const char* endpoint, BridgeDirection direction, bool text)
{
  const char* allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-./";

  if ((endpoint[0] == 0) || (endpoint[0] == '/') || (endpoint[strspn(endpoint, allowed)] != 0) || (strstr(endpoint, "..") != NULL))
  {
    COR_E("ckan: '%s' is not an endpoint - a relative file path, no '..'", endpoint);
    return BRIDGE_BAD_INPUT;
  }

  char path[PATH_MAX];
  if (snprintf(path, sizeof(path), "%s/%s", dir, endpoint) >= (int) sizeof(path))
  {
    COR_E("ckan: '%s': the path is too long", endpoint);
    return BRIDGE_BAD_INPUT;
  }

  dirsMake(path);

  pthread_mutex_lock(&mtx);

  CkanEndpoint* freeP = NULL;
  for (int ix = 0; ix < (int) (sizeof(endpoints) / sizeof(endpoints[0])); ix++)
  {
    if ((endpoints[ix].inUse == true) && (strcmp(endpoints[ix].endpoint, endpoint) == 0))
    {
      pthread_mutex_unlock(&mtx);
      return BRIDGE_OK;                               // carried already
    }

    if ((endpoints[ix].inUse == false) && (freeP == NULL))
      freeP = &endpoints[ix];
  }

  if (freeP == NULL)
  {
    pthread_mutex_unlock(&mtx);
    COR_E("ckan: '%s': no room for another endpoint", endpoint);
    return BRIDGE_ERR;
  }

  freeP->endpoint  = strdup(endpoint);
  freeP->path      = strdup(path);
  freeP->direction = direction;
  freeP->text      = text;
  freeP->last      = fileRead(path);                  // what is there already is not news
  freeP->inUse     = true;

  pthread_mutex_unlock(&mtx);

  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// ckanTransportEndpointDel -
//
int ckanTransportEndpointDel(const char* endpoint)
{
  pthread_mutex_lock(&mtx);

  for (int ix = 0; ix < (int) (sizeof(endpoints) / sizeof(endpoints[0])); ix++)
  {
    CkanEndpoint* eP = &endpoints[ix];

    if ((eP->inUse == true) && (strcmp(eP->endpoint, endpoint) == 0))
    {
      free(eP->endpoint);
      free(eP->path);
      free(eP->last);
      memset(eP, 0, sizeof(*eP));

      pthread_mutex_unlock(&mtx);
      return BRIDGE_OK;
    }
  }

  pthread_mutex_unlock(&mtx);
  return BRIDGE_NOT_FOUND;
}



// -----------------------------------------------------------------------------
//
// ckanTransportSend -
//
// A small local file is written at once. A transport with a slow round trip queues the value here and
// sends it from a thread of its own (corModbusBridge does).
//
int ckanTransportSend(const char* endpoint, const char* json)
{
  pthread_mutex_lock(&mtx);

  CkanEndpoint* eP = NULL;
  for (int ix = 0; (ix < (int) (sizeof(endpoints) / sizeof(endpoints[0]))) && (eP == NULL); ix++)
  {
    if ((endpoints[ix].inUse == true) && (strcmp(endpoints[ix].endpoint, endpoint) == 0))
      eP = &endpoints[ix];
  }

  if ((eP == NULL) || (eP->direction == BridgeDirectionIn))
  {
    pthread_mutex_unlock(&mtx);
    return BRIDGE_NOT_FOUND;
  }

  char tmpPath[PATH_MAX + 8];
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", eP->path);

  FILE* fP = fopen(tmpPath, "w");
  bool  ok = (fP != NULL) && (fprintf(fP, "%s\n", json) > 0);

  if (fP != NULL)
    ok = (fclose(fP) == 0) && ok;

  if ((ok == true) && (rename(tmpPath, eP->path) == 0))
  {
    free(eP->last);
    eP->last = strdup(json);                          // ours - not to be read back in as a sample
  }
  else
  {
    unlink(tmpPath);
    ok = false;
  }

  pthread_mutex_unlock(&mtx);

  if (ok == false)
  {
    COR_W("ckan: cannot write '%s': %s", endpoint, strerror(errno));
    return BRIDGE_ERR;
  }

  return BRIDGE_OK;
}
