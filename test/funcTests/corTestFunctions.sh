# SPDX-License-Identifier: Apache-2.0
#
# corTestFunctions.sh - the verbs this repo's tests use; corTest sources it into every section
#
# A test starts a CKAN portal of its own (tools/ckanMock.py: recorded answers, no internet) and a broker
# that loads THIS build of the plugin (by path - nothing is installed), each on a port of its own, with
# ramDB as the broker's store: no database server is needed. A test waits on and asserts on what the API
# says, never on the broker's log - the log is kept for reading when a test fails.
#
#   CKAN_BROKER     the broker binary (default: coraine on PATH - the coraine package, or
#                   `. <dir>/install/env` after coraine-build)
#   CKAN_PORT       its port (default 19026 - clear of a broker on 1026 and of coraine's own test ports)
#   CKAN_MOCK_PORT  the mock portal's port (default CKAN_PORT + 1)
#   CKAN_PLUGIN     the plugin under test (default: ckan.so in this repo)
#   CKAN_TEST_DIR   scratch directory: the bridge configuration, the pids, the broker's and the mock's log
#
CKAN_REPO="${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
CKAN_BROKER="${CKAN_BROKER:-coraine}"
CKAN_PORT="${CKAN_PORT:-19026}"
CKAN_MOCK_PORT="${CKAN_MOCK_PORT:-$((CKAN_PORT + 1))}"
CKAN_PLUGIN="${CKAN_PLUGIN:-$CKAN_REPO/ckan.so}"
CKAN_TEST_DIR="${CKAN_TEST_DIR:-/tmp/ckan-bridge-test}"
CKAN_FIXTURES="$CKAN_REPO/test/funcTests/fixtures"
CKAN_STATIONS="44c931aa-4577-4caa-a8f2-ed1e38263e70"


# -----------------------------------------------------------------------------
#
# pidStop <pid file> - stop what the pid file names (never by name), and wait for it to go
#
pidStop() {
  [ -f "$1" ] || return 0

  local pid i
  pid=$(cat "$1")
  kill "$pid" 2>/dev/null
  for ((i = 0; i < 50; i++)); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  kill -9 "$pid" 2>/dev/null
  rm -f "$1"
}


# -----------------------------------------------------------------------------
#
# mockStart [api key] - the CKAN portal: tools/ckanMock.py on $CKAN_MOCK_PORT
#
mockStart() {
  mockStop
  mkdir -p "$CKAN_TEST_DIR"

  python3 "$CKAN_REPO/test/funcTests/tools/ckanMock.py" "$CKAN_MOCK_PORT" "$CKAN_FIXTURES" "$@" > "$CKAN_TEST_DIR/mock.log" 2>&1 &
  echo $! > "$CKAN_TEST_DIR/mock.pid"

  local i
  for ((i = 0; i < 50; i++)); do
    curl -s -o /dev/null "localhost:$CKAN_MOCK_PORT/mock/requests" && return 0
    sleep 0.1
  done

  echo "mockStart: no answer on port $CKAN_MOCK_PORT - $CKAN_TEST_DIR/mock.log:" >&2
  tail -5 "$CKAN_TEST_DIR/mock.log" >&2
  return 1
}


# -----------------------------------------------------------------------------
#
# mockStop - stop the portal mockStart started
#
mockStop() {
  pidStop "$CKAN_TEST_DIR/mock.pid"
}


# -----------------------------------------------------------------------------
#
# mockRequests - the datastore_search requests the portal was sent, in order
#
mockRequests() {
  curl -s "localhost:$CKAN_MOCK_PORT/mock/requests"
}


# -----------------------------------------------------------------------------
#
# brokerStart [broker options...] - start the broker with the plugin and $CKAN_TEST_DIR/bridges.json
#
# Returns once the port answers; 1 (with the log's tail on stderr) if the broker died on the way.
#
brokerStart() {
  brokerStop

  "$CKAN_BROKER" --port "$CKAN_PORT" --foreground --pretty-print 2 --httpEndpoint "http://localhost:$CKAN_PORT" \
                 --database ramDB --bridges "$CKAN_PLUGIN" --bridgeConfig "$CKAN_TEST_DIR/bridges.json" "$@" \
                 > "$CKAN_TEST_DIR/broker.log" 2>&1 &
  echo $! > "$CKAN_TEST_DIR/broker.pid"

  local i
  for ((i = 0; i < 100; i++)); do
    curl -s -o /dev/null "localhost:$CKAN_PORT/ngsi-ld/v1/types" && return 0
    if ! kill -0 "$(cat "$CKAN_TEST_DIR/broker.pid")" 2>/dev/null; then
      echo "brokerStart: the broker died - $CKAN_TEST_DIR/broker.log:" >&2
      tail -20 "$CKAN_TEST_DIR/broker.log" >&2
      return 1
    fi
    sleep 0.1
  done

  echo "brokerStart: no answer on port $CKAN_PORT after 10s" >&2
  return 1
}


# -----------------------------------------------------------------------------
#
# brokerStop - stop the broker brokerStart started
#
brokerStop() {
  pidStop "$CKAN_TEST_DIR/broker.pid"
}


# -----------------------------------------------------------------------------
#
# brokerAlive - "broker up" when the broker process lives and answers, else what is wrong
#
brokerAlive() {
  if ! kill -0 "$(cat "$CKAN_TEST_DIR/broker.pid" 2>/dev/null)" 2>/dev/null; then
    echo "broker DEAD"
    return 1
  fi
  echo "broker up, GET /version: $(curl -s -o /dev/null -w '%{http_code}' "localhost:$CKAN_PORT/version")"
}


# -----------------------------------------------------------------------------
#
# channels - one line per Channel: endpoint, status, samplesIn - and why, when it says why
#
channels() {
  curl -s "localhost:$CKAN_PORT/ngsi-ld/v1/channels" | python3 -c 'import sys, json
for c in sorted(json.load(sys.stdin), key=lambda c: c["channelTarget"]):
  line = "%s %s samplesIn=%s" % (c["channelTarget"], c["status"], c.get("samplesIn"))
  print(line if c.get("statusReason") is None else "%s - %s" % (line, c["statusReason"]))'
}


# -----------------------------------------------------------------------------
#
# channelAwait <endpoint> <python condition on c> [seconds] - wait until GET /channels shows the
#                                                             endpoint's Channel c meeting it
#
#   channelAwait $CKAN_STATIONS 'c["samplesIn"] == 50'
#   channelAwait nope 'c["status"] == "dormant"'
#
channelAwait() {
  local secs="${3:-20}" i

  for ((i = 0; i < secs * 10; i++)); do
    curl -s "localhost:$CKAN_PORT/ngsi-ld/v1/channels" | python3 -c 'import sys, json
found = [c for c in json.load(sys.stdin) if c["channelTarget"] == sys.argv[1]]
c = found[0] if found else None
sys.exit(0 if (c is not None) and ('"$2"') else 1)' "$1" 2>/dev/null && return 0
    sleep 0.1
  done

  echo "channelAwait: '$1' does not meet $2 after ${secs}s:" >&2
  channels >&2
  return 1
}


# -----------------------------------------------------------------------------
#
# entityGet <entityId> [attrs] - the entity, JSON with sorted members, without the times the broker
#                                stamps (createdAt, modifiedAt) and with observedAt replaced by "<read time>"
#
entityGet() {
  curl -s "localhost:$CKAN_PORT/ngsi-ld/v1/entities/$1${2:+?attrs=$2}" | python3 -c 'import sys, json
def clean(n):
  if isinstance(n, dict):
    return {k: ("<read time>" if k == "observedAt" else clean(v)) for k, v in n.items() if k not in ("createdAt", "modifiedAt")}
  if isinstance(n, list):
    return [clean(v) for v in n]
  return n
print(json.dumps(clean(json.load(sys.stdin)), indent=2, sort_keys=True))'
}


# -----------------------------------------------------------------------------
#
# entityIds <type> - the ids of every entity of a type, sorted, one a line
#
entityIds() {
  curl -s "localhost:$CKAN_PORT/ngsi-ld/v1/entities?type=$1&limit=1000" | python3 -c 'import sys, json
for i in sorted(e["id"] for e in json.load(sys.stdin)):
  print(i)'
}


# -----------------------------------------------------------------------------
#
# entityCount <type> - how many entities of a type there are
#
entityCount() {
  curl -s -o /dev/null -D - "localhost:$CKAN_PORT/ngsi-ld/v1/entities?type=$1&count=true&limit=0" | tr -d '\r' \
    | sed -n 's/^NGSILD-Results-Count: //Ip'
}
