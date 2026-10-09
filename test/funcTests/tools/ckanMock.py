#!/usr/bin/env python3
#
# FILE            ckanMock.py
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# ckanMock.py <port> <fixture dir> [api key] - a CKAN portal's datastore_search, from recorded answers
#
# The functional tests do not reach the internet: this serves what data.boston.gov answered, recorded in
# the fixture directory (see its README).
#
#   GET /api/3/action/datastore_search?resource_id=<id>&limit=<n>&offset=<o>[&sort=_id]
#
#     44c931aa-4577-4caa-a8f2-ed1e38263e70   the 50 recorded Blue Bike stations, sliced by limit/offset as
#                                            CKAN does - total 50, the envelope of the first recorded page
#     edge-cases                             edge-cases.json (written for the tests, not recorded)
#     retry-503                              503 to the first request, then the stations
#     anything else                          the recorded 404 (not-found.json, its id replaced)
#
#   GET /mock/requests   one line per datastore_search request so far: its query, sorted, and
#                        "auth=key" (the Authorization header was the key given), "auth=other" or
#                        "auth=none". The key itself is never written anywhere.
#
import json
import os
import sys
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

port       = int(sys.argv[1])
fixtureDir = sys.argv[2]
apiKey     = sys.argv[3] if len(sys.argv) > 3 else None

STATIONS   = "44c931aa-4577-4caa-a8f2-ed1e38263e70"


def load(name):
    with open(os.path.join(fixtureDir, name)) as f:
        return json.load(f)


pages      = [load("boston-bike-stations-offset%d.json" % o) for o in (0, 20, 40)]
stations   = [record for page in pages for record in page["result"]["records"]]
envelope   = pages[0]
edgeCases  = load("edge-cases.json")
notFound   = load("not-found.json")

lock       = threading.Lock()
requests   = []
retried    = set()


def answer(records, resourceId, limit, offset, base):
    page = json.loads(json.dumps(base))
    result = page["result"]
    result["resource_id"] = resourceId
    result["limit"]       = limit
    result["offset"]      = offset
    result["total"]       = len(records)
    result["records"]     = records[offset:offset + limit]
    result.pop("_links", None)
    return page


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def send(self, status, body):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json;charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        url   = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(url.query)

        if url.path == "/mock/requests":
            with lock:
                text = "".join(line + "\n" for line in requests).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(text)))
            self.end_headers()
            self.wfile.write(text)
            return

        if url.path != "/api/3/action/datastore_search":
            self.send(404, {"success": False, "error": {"__type": "Not Found Error", "message": "Not found"}})
            return

        auth = self.headers.get("Authorization")
        authSaid = "none" if auth is None else ("key" if auth == apiKey else "other")

        with lock:
            requests.append("%s auth=%s" % ("&".join(sorted(url.query.split("&"))), authSaid))

        resourceId = query.get("resource_id", [""])[0]
        limit      = int(query.get("limit", ["100"])[0])
        offset     = int(query.get("offset", ["0"])[0])

        if resourceId == "retry-503":
            with lock:
                first = resourceId not in retried
                retried.add(resourceId)
            if first:
                self.send(503, {"success": False, "error": {"__type": "Service Unavailable", "message": "try again"}})
                return
            self.send(200, answer(stations, resourceId, limit, offset, envelope))
        elif resourceId == STATIONS:
            self.send(200, answer(stations, resourceId, limit, offset, envelope))
        elif resourceId == "edge-cases":
            self.send(200, answer(edgeCases["records"], resourceId, limit, offset, envelope))
        else:
            body = json.loads(json.dumps(notFound).replace("00000000-0000-0000-0000-000000000000", resourceId))
            self.send(404, body)


ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
