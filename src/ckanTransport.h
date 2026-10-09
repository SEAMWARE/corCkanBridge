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
// The TRANSPORT - the part of the bridge that speaks the foreign protocol, and the part a real bridge
// replaces. ckanRegister.c is the seam to the broker and stays as it is; nothing in this interface
// knows about the broker, entities or Channels: an endpoint, a JSON text and a time, both ways.
//
// The example transport is a directory of files. An endpoint is a file path under the directory:
//
//   in   a file whose content changed becomes a sample - polled every pollMs
//   out  a value published to an endpoint is written into its file
//
#include <stdbool.h>                                  // bool
#include <stdint.h>                                   // int64_t

#include "corTree/CorNode.h"                          // CorNode

#include "corBridge/BridgeDriver.h"                   // BridgeDirection



// -----------------------------------------------------------------------------
//
// CkanReceiveFunc - what the transport calls when a value arrives on an endpoint
//
// Called on the TRANSPORT's own thread, never on a broker thread. json is the transport's: borrowed
// for the duration of the call.
//
// @param timeNs  when the value was produced, nanoseconds since the epoch, as the transport says -
//                0 when it does not say
//
typedef void (*CkanReceiveFunc)(const char* endpoint, const char* json, int64_t timeNs);



// -----------------------------------------------------------------------------
//
// ckanTransportOpen - bring the transport up
//
// @param serverP    the "server" object of this bridge's member of the --bridgeConfig file, or NULL
//                   when there is none. Read during the call and not kept.
// @param receiveF   where arriving values go
//
// @return BRIDGE_OK or BRIDGE_ERR, the reason logged
//
extern int ckanTransportOpen(CorNode* serverP, CkanReceiveFunc receiveF);



// -----------------------------------------------------------------------------
//
// ckanTransportClose - take it down; returns once no thread of the transport can call receiveF
//
extern void ckanTransportClose(void);



// -----------------------------------------------------------------------------
//
// ckanTransportEndpointAdd - start carrying an endpoint
//
// @param text  the endpoint carries plain text, not JSON: an arriving value becomes a JSON string
//
// @return BRIDGE_OK, BRIDGE_BAD_INPUT (not an endpoint of this transport) or BRIDGE_ERR
//
extern int ckanTransportEndpointAdd(const char* endpoint, BridgeDirection direction, bool text);



// -----------------------------------------------------------------------------
//
// ckanTransportEndpointDel - stop carrying an endpoint; BRIDGE_OK or BRIDGE_NOT_FOUND
//
extern int ckanTransportEndpointDel(const char* endpoint);



// -----------------------------------------------------------------------------
//
// ckanTransportSend - send a value out on an endpoint
//
// Called on a BROKER thread, inside the request that wrote the attribute: it must not block on
// anything slow.
//
// @return BRIDGE_OK, BRIDGE_NOT_FOUND (not an outbound endpoint) or BRIDGE_ERR
//
extern int ckanTransportSend(const char* endpoint, const char* json);

#endif  // CORCKANBRIDGE_CKANTRANSPORT_H_
