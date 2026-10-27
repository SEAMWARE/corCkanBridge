//
// FILE            ckanRegister.c
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
// The ckan bridge - the SEAM: what the broker calls, and the one symbol it looks for (bridgeRegister).
// The contract is corBridge's BridgeDriver.h (broker -> plugin) and BridgeBroker.h (plugin -> broker).
//
// A bridge knows nothing about entities. This one carries (endpoint, json, time) one way, in:
//
//   endpoint  a CKAN DataStore resource id
//   json      one record (row) of it
//   time      the record's time column; 0 (no time - no observedAt) without one
//
// Which entities a record becomes is the broker's: a DATACHANNEL, from the "dataChannels" section of
// this bridge's member of the --bridgeConfig file (coraine: COR_FEATURE_DATA_CHANNELS). The plugin
// reads the rest of its member ("server": the portal) and each DataChannel's channelInfo (how to read
// the resource). The transport - CKAN's Action API - is in ckanTransport.c.
//
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // fopen, fread, fseek, ftell, snprintf
#include <stdlib.h>                                   // malloc, free, strtol
#include <string.h>                                   // strdup, strcmp

#include "corLog/corLog.h"                            // COR_E, COR_W
#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAllocBufferInit.h"              // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"             // corAllocBufferReset
#include "corJson/CorJson.h"                          // CorJson
#include "corJson/corJsonCreate.h"                    // corJsonCreate
#include "corJson/corJsonParse.h"                     // corJsonParse
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "corBridge/BridgeDriver.h"                   // BridgeDriver, BridgeChannelKind, BridgeDirection
#include "corBridge/BridgeBroker.h"                   // BridgeBroker, BRIDGE_*

#include "ckanTransport.h"                            // ckanTransport*



// -----------------------------------------------------------------------------
//
// The broker's side of the seam - kept from init() until close()
//
static const BridgeBroker* brokerP = NULL;



// -----------------------------------------------------------------------------
//
// ckanReceive - a record: hand it to the broker
//
// On the transport's thread. The broker sets up its own per-request state inside sampleIn and takes
// its own locks - the plugin prepares nothing. "ckan" is this bridge's alias: the broker finds the
// Channel by (alias, endpoint).
//
// BRIDGE_BAD_INPUT is a record the Channel's mapping makes no entity of (a column an id needs is
// empty): the broker has said so in its log, and the read goes on.
//
static bool ckanReceive(const char* endpoint, const char* json, int64_t timeNs)
{
  if (brokerP == NULL)
    return false;

  int r = brokerP->sampleIn("ckan", endpoint, json, timeNs);

  if ((r != BRIDGE_OK) && (r != BRIDGE_BAD_INPUT))
    COR_W("ckan: a record of '%s' was not taken (%d)", endpoint, r);

  return (r == BRIDGE_OK);
}



// -----------------------------------------------------------------------------
//
// channelStatus - tell the broker whether a Channel carries (ABI 12), else say it in the log only
//
// Built against corBridge headers from before ABI 12, the plugin has no channelStatusIn to call: the
// log is then the only place a reason goes.
//
static void channelStatus(const char* endpoint, bool carrying, const char* reason)
{
#if BRIDGE_ABI_VERSION >= 12
  if ((brokerP != NULL) && (brokerP->abiVersion >= 12) && (brokerP->channelStatusIn != NULL))
  {
    brokerP->channelStatusIn("ckan", endpoint, (carrying == true) ? BridgeChannelAvailable : BridgeChannelDormant, reason);
    return;
  }
#endif

  if (carrying == false)
    COR_E("ckan: '%s' is not carried: %s (the broker shows no reason on the Channel: it has no channelStatusIn, bridge ABI 12)", endpoint, reason);
}



// -----------------------------------------------------------------------------
//
// configLoad - the whole --bridgeConfig file, NUL-terminated; malloc'd. NULL: not readable
//
// All of it: the file holds every bridge's Channels, and the broker takes up to 4 MiB.
//
static char* configLoad(const char* path)
{
  FILE* fP = fopen(path, "r");
  if (fP == NULL)
    return NULL;

  char* text = NULL;
  long  size = (fseek(fP, 0, SEEK_END) == 0) ? ftell(fP) : -1;

  if ((size >= 0) && (fseek(fP, 0, SEEK_SET) == 0) && ((text = malloc(size + 1)) != NULL))
  {
    if (fread(text, 1, size, fP) == (size_t) size)
      text[size] = 0;
    else
    {
      free(text);
      text = NULL;
    }
  }

  fclose(fP);
  return text;
}



// -----------------------------------------------------------------------------
//
// ckanInit - the portal, and the reader thread
//
// The --bridgeConfig file is one JSON object, one member per bridge alias. The broker reads every
// member's "ngsild" section (the Channels); the rest of the member is the plugin's:
//
//   { "ckan": { "server": { "url": "https://data.boston.gov" },
//               "ngsild": { "dataChannels": { "<resource id>": { "channelInfo": [...], "entities": [...] } } } } }
//
// Called once, late in the broker's startup. The portal is not contacted here - each resource is read
// on the reader thread, once the broker has handed it the DataChannel's endpoint (channelAddInfo).
//
static int ckanInit(const char* configFile, const BridgeBroker* _brokerP)
{
  CorAlloc  ka;
  CorJson   cj;
  char      buf[16 * 1024];
  CorNode*  serverP = NULL;
  char*     text    = (configFile != NULL) ? configLoad(configFile) : NULL;

  brokerP = _brokerP;

  corAllocBufferInit(&ka, buf, sizeof(buf), 16 * 1024, NULL, "ckan config");
  corJsonCreate(&cj, &ka);

  if (text != NULL)
  {
    CorNode* rootP   = corJsonParse(&cj, text);       // valid JSON: the broker has parsed the same file
    CorNode* bridgeP = (rootP != NULL) ? corTreeLookup(rootP, "ckan") : NULL;

    serverP = (bridgeP != NULL) ? corTreeLookup(bridgeP, "server") : NULL;
  }

  int r = ckanTransportOpen(serverP, ckanReceive, channelStatus);

  corAllocBufferReset(&ka, false);                    // nothing parsed here is kept
  free(text);                                         // after the tree: its strings point into it
  return r;
}



// -----------------------------------------------------------------------------
//
// ckanClose - must not return until no thread of the plugin can call sampleIn
//
static void ckanClose(void)
{
  ckanTransportClose();
  brokerP = NULL;
}



// -----------------------------------------------------------------------------
//
// channelInfoRead - a Channel's channelInfo -> how to read its resource; false (and *whyP) for anything wrong
//
// channelInfo is JSON text, an array of {"key": "...", "value": "..."}, both strings, or NULL. The broker
// checks that shape and never a key: a key the plugin does not know is the plugin's to refuse.
//
//   mode        once (the default) - read the resource at start, then stop
//   pageSize    records per datastore_search call, 1-32000 (default 100)
//   timeColumn  a column holding each record's time, ISO 8601; without it, a record has no time (no observedAt)
//
// The text is parsed in a copy; configP->timeColumn points into textBuf, the caller's, and lives as
// long as it.
//
static bool channelInfoRead(const char* info, CkanEndpoint* configP, char* textBuf, int textBufSize, char* why, int whySize)
{
  configP->mode       = CkanModeOnce;
  configP->pageSize   = 100;
  configP->timeColumn = NULL;

  if (info == NULL)
    return true;

  CorAlloc ka;
  CorJson  cj;
  char     buf[4096];
  char*    copy = strdup(info);                       // corJsonParse works in place, and info is the broker's
  bool     ok;

  if (copy == NULL)
  {
    snprintf(why, whySize, "out of memory");
    return false;
  }

  corAllocBufferInit(&ka, buf, sizeof(buf), 4096, NULL, "ckan channelInfo");
  corJsonCreate(&cj, &ka);

  CorNode* arrayP = corJsonParse(&cj, copy);
  ok = (arrayP != NULL) && (arrayP->type == CorArray);

  if (ok == false)
    snprintf(why, whySize, "channelInfo is not an array of {key, value}");

  for (CorNode* pairP = (ok == true) ? arrayP->value.head : NULL; (pairP != NULL) && (ok == true); pairP = pairP->next)
  {
    CorNode*    keyP   = corTreeLookup(pairP, "key");
    CorNode*    valueP = corTreeLookup(pairP, "value");
    const char* key    = ((keyP   != NULL) && (keyP->type   == CorString)) ? keyP->value.s   : "";
    const char* value  = ((valueP != NULL) && (valueP->type == CorString)) ? valueP->value.s : "";

    if (strcmp(key, "mode") == 0)
    {
      if ((strcmp(value, "poll") == 0) || (strcmp(value, "push") == 0))
      {
        snprintf(why, whySize, "mode '%s' is not built yet - 'once' is", value);
        ok = false;
      }
      else if (strcmp(value, "once") != 0)
      {
        snprintf(why, whySize, "mode '%s' - the modes are once, poll and push, and once is the one built", value);
        ok = false;
      }
    }
    else if (strcmp(key, "pageSize") == 0)
    {
      char* endP;
      long  n = strtol(value, &endP, 10);

      if ((*value == 0) || (*endP != 0) || (n < 1) || (n > 32000))
      {
        snprintf(why, whySize, "pageSize '%s' - an integer from 1 to 32000", value);
        ok = false;
      }
      else
        configP->pageSize = (int) n;
    }
    else if (strcmp(key, "timeColumn") == 0)
    {
      int len = strlen(value);

      if ((len == 0) || (len >= textBufSize))
      {
        snprintf(why, whySize, "timeColumn - the name of a column");
        ok = false;
      }
      else
      {
        memcpy(textBuf, value, len + 1);
        configP->timeColumn = textBuf;
      }
    }
    else if ((strcmp(key, "apiKey") == 0) || (strcmp(key, "Authorization") == 0))
    {
      snprintf(why, whySize, "'%s' in channelInfo - an API key goes in server.apiKey (or server.apiKeyEnv): channelInfo is shown on GET /ngsi-ld/v1/dataChannels", key);
      ok = false;
    }
    else
    {
      snprintf(why, whySize, "unknown channelInfo key '%s' (mode, pageSize, timeColumn)", key);
      ok = false;
    }
  }

  corAllocBufferReset(&ka, false);
  free(copy);

  return ok;
}



// -----------------------------------------------------------------------------
//
// ckanChannelAddInfo - start reading a resource, ABI 9
//
// Once per Channel, after init. Inbound topics only: a resource is read, nothing is written to the
// portal (yet). A Channel refused here stays, dormant, with the reason - told to the broker
// (channelStatusIn, ABI 12) before this returns, so GET /ngsi-ld/v1/dataChannels shows it.
//
static int ckanChannelAddInfo(const char* endpoint, BridgeChannelKind kind, BridgeDirection direction, const char* info)
{
  char         why[256];
  char         timeColumn[256];
  CkanEndpoint config;

  if (kind != BridgeChannelTopic)
  {
    channelStatus(endpoint, false, "this bridge reads resources - a service or an action is not one");
    return BRIDGE_UNSUPPORTED;
  }

  if (direction != BridgeDirectionIn)
  {
    channelStatus(endpoint, false, "this bridge reads resources, and writes none - a DataChannel is inbound");
    return BRIDGE_UNSUPPORTED;
  }

  if (channelInfoRead(info, &config, timeColumn, sizeof(timeColumn), why, sizeof(why)) == false)
  {
    COR_E("ckan: '%s': %s", endpoint, why);
    channelStatus(endpoint, false, why);
    return BRIDGE_BAD_INPUT;
  }

  const char* reason = NULL;
  int         r      = ckanTransportEndpointAdd(endpoint, &config, &reason);

  if (r != BRIDGE_OK)
  {
    COR_E("ckan: '%s': %s", endpoint, (reason != NULL) ? reason : "cannot be read");
    channelStatus(endpoint, false, (reason != NULL) ? reason : "cannot be read");
  }

  return r;
}



// -----------------------------------------------------------------------------
//
// ckanChannelAdd - a host from before ABI 9, which has no channelInfo
//
static int ckanChannelAdd(const char* endpoint, BridgeChannelKind kind, BridgeDirection direction)
{
  return ckanChannelAddInfo(endpoint, kind, direction, NULL);
}



// -----------------------------------------------------------------------------
//
// ckanChannelDel - stop reading a resource
//
static int ckanChannelDel(const char* endpoint)
{
  return ckanTransportEndpointDel(endpoint);
}



// -----------------------------------------------------------------------------
//
// ckanVersionInfo - one line for GET /version
//
static const char* ckanVersionInfo(void)
{
  return "ckan 0.1.0 (CKAN Action API: datastore_search; mode once)";
}



// -----------------------------------------------------------------------------
//
// bridgeRegister - the one symbol the broker looks for
//
// The .so exports it and corNodeLayout - weak, defined by corTree/CorNode.h, which the broker checks when
// it opens the plugin. Everything else is hidden (-fvisibility=hidden in the makefile).
//
// The broker zeroes the struct, writes ITS BRIDGE_ABI_VERSION into abiVersion, and calls this. The struct
// is the broker's, allocated at the size of ITS header: a slot newer than the broker is not there, so a
// slot is filled only when the broker's abiVersion has it. A slot left NULL means "not supported" -
// publish among them: nothing is written to a portal.
//
__attribute__((visibility("default"))) void bridgeRegister(BridgeDriver* driverP)
{
  if (driverP->abiVersion < 1)
    driverP->abiVersion = 1;                          // a host from before the handshake

  driverP->alias       = "ckan";                      // --bridges ckan, and its member of --bridgeConfig
  driverP->version     = "0.1.0";
  driverP->args        = NULL;
  driverP->init        = ckanInit;
  driverP->close       = ckanClose;
  driverP->versionInfo = ckanVersionInfo;
  driverP->channelAdd  = ckanChannelAdd;
  driverP->channelDel  = ckanChannelDel;
  driverP->publish     = NULL;

  if (driverP->abiVersion >= 9)
    driverP->channelAddInfo = ckanChannelAddInfo;

  driverP->abiVersion  = BRIDGE_ABI_VERSION;
}
