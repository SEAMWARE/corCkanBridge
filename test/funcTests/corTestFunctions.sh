# SPDX-License-Identifier: Apache-2.0
#
# corTestFunctions.sh - the verbs this repo's tests use; corTest sources it into every section
#
# A test starts a broker that loads THIS build of the plugin (by path - nothing is installed), on a port
# of its own, with ramDB as its store: no database server is needed. A test waits on and asserts on what
# the API says, never on the broker's log - the log is kept for reading when a test fails.
#
#   CKAN_BROKER   the broker binary (default: coraine on PATH - the coraine package, or
#                 `. <dir>/install/env` after coraine-build)
#   CKAN_PORT     its port (default 19026 - clear of a broker on 1026 and of coraine's own test ports)
#   CKAN_PLUGIN   the plugin under test (default: ckan.so in this repo)
#   CKAN_TEST_DIR scratch directory: the bridge configuration, the device files, the broker's pid and log
#
CKAN_BROKER="${CKAN_BROKER:-coraine}"
CKAN_PORT="${CKAN_PORT:-19026}"
CKAN_PLUGIN="${CKAN_PLUGIN:-${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}/ckan.so}"
CKAN_TEST_DIR="${CKAN_TEST_DIR:-/tmp/ckan-bridge-test}"


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
# brokerStop - stop the broker brokerStart started (by its pid, never by name)
#
brokerStop() {
  local pidFile="$CKAN_TEST_DIR/broker.pid"
  [ -f "$pidFile" ] || return 0

  local pid i
  pid=$(cat "$pidFile")
  kill "$pid" 2>/dev/null
  for ((i = 0; i < 50; i++)); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  kill -9 "$pid" 2>/dev/null
  rm -f "$pidFile"
}


# -----------------------------------------------------------------------------
#
# deviceWrite <endpoint> <content> [time] - the device produces a value: its file, written whole
#
# Written beside it and renamed, so the bridge never reads half a value. [time] (ISO 8601) sets the
# file's modification time, which the bridge reports as the value's time - observedAt.
#
deviceWrite() {
  local file="$CKAN_TEST_DIR/device/$1"

  mkdir -p "$(dirname "$file")"
  echo "$2" > "$file.new"
  [ -z "$3" ] || touch -d "$3" "$file.new"
  mv "$file.new" "$file"
}


# -----------------------------------------------------------------------------
#
# deviceAwait <endpoint> <content> [seconds] - wait until the device's file holds <content>, then print it
#
deviceAwait() {
  local file="$CKAN_TEST_DIR/device/$1" secs="${3:-10}" i

  for ((i = 0; i < secs * 10; i++)); do
    [ "$(cat "$file" 2>/dev/null)" == "$2" ] && { cat "$file"; return 0; }
    sleep 0.1
  done

  echo "deviceAwait: '$1' holds '$(cat "$file" 2>/dev/null)', not '$2', after ${secs}s" >&2
  cat "$file" 2>/dev/null
  return 1
}


# -----------------------------------------------------------------------------
#
# entityAwait <entityId> <attr> <text> [seconds] - wait until GET the attribute (keyValues) contains <text>
#
entityAwait() {
  local url="localhost:$CKAN_PORT/ngsi-ld/v1/entities/$1?options=keyValues&attrs=$2" secs="${4:-10}" i

  for ((i = 0; i < secs * 10; i++)); do
    curl -s "$url" | grep -qF -- "$3" && return 0
    sleep 0.1
  done

  echo "entityAwait: $1/$2 is $(curl -s "$url"), without '$3', after ${secs}s" >&2
  return 1
}


# -----------------------------------------------------------------------------
#
# entityGet <entityId> <attrs> [options] - the entity's attributes, JSON with sorted members
#
entityGet() {
  curl -s "localhost:$CKAN_PORT/ngsi-ld/v1/entities/$1?attrs=$2${3:+&options=$3}" \
    | python3 -c 'import sys, json; print(json.dumps(json.load(sys.stdin), indent=2, sort_keys=True))'
}


# -----------------------------------------------------------------------------
#
# attrPatch <entityId> <attr> <body> - PATCH one attribute; prints the HTTP status
#
attrPatch() {
  curl -s -o /dev/null -w "%{http_code}\n" -X PATCH "localhost:$CKAN_PORT/ngsi-ld/v1/entities/$1/attrs/$2" \
       -H 'Content-Type: application/json' -d "$3"
}


# -----------------------------------------------------------------------------
#
# channels - one line per Channel: endpoint, status, counters - and why, when it is dormant
#
channels() {
  curl -s "localhost:$CKAN_PORT/ngsi-ld/v1/channels" | python3 -c 'import sys, json
for c in sorted(json.load(sys.stdin), key=lambda c: c["channelTarget"]):
  line = "%s %s samplesIn=%s samplesOut=%s" % (c["channelTarget"], c["status"], c.get("samplesIn"), c.get("samplesOut"))
  print(line if c["status"] == "available" else "%s - %s" % (line, c.get("statusReason")))'
}
