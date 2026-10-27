#ifndef CORCKANBRIDGE_CKANTRANSPORT_H_
#define CORCKANBRIDGE_CKANTRANSPORT_H_

//
// FILE            ckanTransport.h
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
// The TRANSPORT - CKAN's Action API, as a client. ckanRegister.c is the seam to the broker; nothing in
// this interface knows about the broker, entities or Channels: an endpoint, a JSON text and a time.
//
//   endpoint  a DataStore resource id ("44c931aa-4577-4caa-a8f2-ed1e38263e70")
//   json      one record (row) of the resource - a JSON object, one member per column, "_id" included
//   time      the record's time column, if the endpoint names one; else 0 - no time, no observedAt
//
// How an endpoint is followed is its MODE. Only "once" is built: the resource is read whole, page by
// page (datastore_search, limit/offset, sorted by _id), every record handed over, and the endpoint is
// done. "poll" (the dataset's activity stream, then new rows by _id or the whole resource again) and
// "push" (a webhook) are names reserved for what comes later; the configuration keeps room for them.
//
#include <stdbool.h>                                  // bool
#include <stdint.h>                                   // int64_t

#include "corTree/CorNode.h"                          // CorNode



// -----------------------------------------------------------------------------
//
// CkanMode - how an endpoint follows its resource
//
typedef enum CkanMode
{
  CkanModeOnce = 0                                    // read it whole at start, then stop
} CkanMode;



// -----------------------------------------------------------------------------
//
// CkanEndpoint - how to read one resource, from its Channel's channelInfo
//
typedef struct CkanEndpoint
{
  CkanMode     mode;
  int          pageSize;                              // records per datastore_search call, 1-32000
  const char*  timeColumn;                            // the column holding a record's time (ISO 8601), or NULL
} CkanEndpoint;



// -----------------------------------------------------------------------------
//
// CkanReceiveFunc - a record arrived
//
// On the transport's own thread. json is borrowed for the duration of the call.
//
// @return true when the record was taken - for the count in the log line closing a read
//
typedef bool (*CkanReceiveFunc)(const char* endpoint, const char* json, int64_t timeNs);



// -----------------------------------------------------------------------------
//
// CkanStatusFunc - whether an endpoint can be read, and why not
//
// On the transport's own thread. reason is one line for a person, NULL with carrying true.
//
typedef void (*CkanStatusFunc)(const char* endpoint, bool carrying, const char* reason);



// -----------------------------------------------------------------------------
//
// ckanTransportOpen - read the portal's settings and start the reader thread
//
// @param serverP  the "server" object of this bridge's member of the --bridgeConfig file:
//                   url        the portal, "https://data.boston.gov" - mandatory
//                   apiKey     a CKAN API token, sent as the Authorization header - optional, never logged
//                   apiKeyEnv  the name of an environment variable holding it, instead
//                   timeoutMs  one HTTP request, default 30000
//                 Read during the call and not kept.
//
// The portal is not contacted here: a portal that is down does not keep the broker from starting.
//
// @return BRIDGE_OK or BRIDGE_ERR, the reason logged
//
extern int ckanTransportOpen(CorNode* serverP, CkanReceiveFunc receiveF, CkanStatusFunc statusF);



// -----------------------------------------------------------------------------
//
// ckanTransportClose - stop the reader; returns once it can call neither function any more
//
// A read in progress stops after the request in flight (at most timeoutMs).
//
extern void ckanTransportClose(void);



// -----------------------------------------------------------------------------
//
// ckanTransportEndpointAdd - start following a resource
//
// Queued for the reader thread, which reads the endpoints in the order they were added.
//
// @return BRIDGE_OK, BRIDGE_BAD_INPUT (not a resource id - *whyP says why) or BRIDGE_ERR
//
extern int ckanTransportEndpointAdd(const char* endpoint, const CkanEndpoint* configP, const char** whyP);



// -----------------------------------------------------------------------------
//
// ckanTransportEndpointDel - stop following a resource; BRIDGE_OK or BRIDGE_NOT_FOUND
//
extern int ckanTransportEndpointDel(const char* endpoint);



// -----------------------------------------------------------------------------
//
// ckanTimeParse - an ISO 8601 date-time as nanoseconds since the epoch; -1 when it is not one
//
// "2026-10-09T12:00:00Z", "2026-10-09T12:00:00.250+02:00", "2026-10-09 12:00:00" (no zone: UTC),
// "2026-10-09" (midnight UTC).
//
extern int64_t ckanTimeParse(const char* text);

#endif  // CORCKANBRIDGE_CKANTRANSPORT_H_
