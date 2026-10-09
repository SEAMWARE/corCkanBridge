# corCkanBridge

A **bridge plugin** for the coraine NGSI-LD broker: `ckan.so`, a working bridge with a tiny example
transport, to be turned into a real one - OPC UA, CKAN, Philips Hue, anything the broker does not
speak. It builds, loads into a broker, and carries a value each way before a line of its own is
written.


## What a bridge is

A bridge connects the broker to a foreign transport. It **knows nothing about entities**: it carries
`(endpoint, json, time)` in both directions, and nothing else.

    device <-> transport <-> ckan.so <-> broker <-> Channel <-> entity attribute
                                     sampleIn()  ->            (in)
                                     publish()   <-            (out)

- **Endpoint** - the transport's own name for a value: an MQTT topic, a Modbus register, an OPC UA
  node id. Exact, as the wire spells it.
- **Channel** - *this endpoint <-> this entity attribute, this direction*. Channels live in the
  broker, which configures them from the `--bridgeConfig` file and shows them on
  `GET /ngsi-ld/v1/channels`. A Channel the bridge refuses stays, `dormant`, with the reason.
- **Bridge** - one transport instance (one connection, one server), `GET /ngsi-ld/v1/bridges`.
  `--bridges ckan` loads the plugin; it is never loaded at runtime or inferred from configuration.
- **In** - a value arrives on an endpoint; the plugin calls `brokerP->sampleIn(alias, endpoint, json,
  timeNs)` from its own thread; the broker writes it into the Channel's attribute (`observedAt` =
  the time), notifies, and records the temporal event.
- **Out** - an attribute a Channel carries is written; the broker calls `publish(endpoint, json)`
  inside that request; the plugin sends it. A value that came in from a transport is never published
  back out (the broker sees to that); the plugin keeps its own writes from coming back in as samples.

## The contract

`corBridge/BridgeDriver.h` (what the plugin fills in) and `corBridge/BridgeBroker.h` (what the broker
offers). In the order the broker uses them:

| entry point | thread | what the plugin does |
|---|---|---|
| `bridgeRegister(BridgeDriver*)` | broker, at load | the one exported symbol: fills in the driver - alias, version, the entry points it has |
| `init(configFile, brokerP)` | broker, late in startup | keeps `brokerP`; reads its member of the `--bridgeConfig` file; brings the transport up |
| `channelAddInfo(endpoint, kind, direction, info)` | broker | per Channel, after `init`: checks the endpoint and `channelInfo`, subscribes |
| `brokerP->sampleIn(...)` | the plugin's own | a value arrived |
| `publish(endpoint, json)` | broker, inside the request | hands the value to the transport and returns - must not block |
| `channelDel(endpoint)` | broker | stops carrying an endpoint (not called yet: Channels come from the file only) |
| `close()` | broker, at shutdown | returns only when no plugin thread can call `sampleIn` any more |
| `versionInfo()` | broker | one line for `GET /version` |

- **NULL means "not supported".** A slot the plugin leaves NULL is answered `BRIDGE_UNSUPPORTED` by
  the broker. Optional slots: `args` (command-line options of the plugin's own), services and actions
  (ABI 2-4), `notifySchemes` + `notify` (notifications to a URI scheme - corMqttBridge), Service
  Execution (ABI 11).
- **The ABI handshake.** The broker zeroes the driver, writes *its* `BRIDGE_ABI_VERSION` into
  `abiVersion` and calls `bridgeRegister`. The struct is the broker's, at the size of its header: the
  plugin fills in only the slots that version has (`channelAddInfo` from ABI 9), then writes its own
  version back. On the broker side (`BridgeBroker`), an upcall newer than ABI 1 is called only after
  checking `brokerP->abiVersion` and that the pointer is not NULL.
- **Plain data across the seam**: `const char*` and `int64_t`. `json` passed to the plugin is
  borrowed: copy what is needed after the call. Return codes: `BRIDGE_OK`, `BRIDGE_ERR`,
  `BRIDGE_NOT_FOUND`, `BRIDGE_UNSUPPORTED`, `BRIDGE_BAD_INPUT`.
- **Built against the broker's headers.** The broker is linked `-rdynamic`: `corLog`, `corJson`,
  `corTree` and `corAlloc` resolve from the running broker at `dlopen`, and nothing of the cor stack is
  linked into the plugin - so it is built against the headers of the broker it is loaded into.
- **Logging**: `COR_E` / `COR_W` / `COR_V` (corLog) for the plugin's own lines;
  `brokerP->logFunction` to forward the transport library's own log sink.

## Where each piece goes

| file | what | in a real bridge |
|---|---|---|
| `src/ckanRegister.c` | the seam: `bridgeRegister`, the driver entry points, the bridge's configuration | stays; the TODOs: channelInfo keys, version line |
| `src/ckanTransport.h` | the transport's interface: open, close, add/remove an endpoint, send; a receive callback | stays, or grows |
| `src/ckanTransport.c` | the example transport: a directory of files, polled | replaced by the protocol |
| `test/funcTests/cases/bridge_ckan.test` | a broker with this plugin: a value in, a value out, no echo, a refused endpoint | the device becomes the protocol's test peer |
| `test/funcTests/corTestFunctions.sh` | `brokerStart`, `deviceWrite`, `entityAwait`, `channels`, ... | `deviceWrite` / `deviceAwait` talk to the peer |
| `makefile` | `debug` / `release`, `install`, `contract`, `test` | add the transport library to `LIBS` |
| `.github/workflows/ci.yml` | contract check, both builds, the tests, against the cor stack | install the transport library |

## Configuration

One `--bridgeConfig` file serves every bridge: one member per alias. The `ngsild` section is the
broker's - it makes the Channels; everything else in the member is the plugin's.

```json
{ "ckan": { "server": { "dir": "/var/lib/ckan", "pollMs": 1000 },
            "ngsild": { "topics": {
              "tank1/level": { "entityId": "urn:ngsi-ld:Tank:T1", "entityType": "Tank", "attribute": "level" },
              "tank1/state": { "entityId": "urn:ngsi-ld:Tank:T1", "entityType": "Tank", "attribute": "state",
                               "channelInfo": [ { "key": "format", "value": "text" } ] } } } } }
```

- A Channel's direction is `both` and every configured attribute exists from startup, with the value
  `"uninitialized"` until the first sample.
- `channelInfo` - how to read or carry one endpoint - is checked by the broker for its shape (an array
  of `{"key", "value"}` strings) and never for its keys: an unknown key is the plugin's to refuse
  (`BRIDGE_BAD_INPUT`).
- The example transport: an endpoint is a file path under `server.dir`; a file whose content changes
  is a sample, its modification time the sample's time; a published value is written into the file.
  `format: text` makes the content a JSON string; the default, `json`, takes it as JSON.

## Building

    make                    # ckan.so, debug (traces compiled in)
    make BUILD=release
    make contract           # does src/ckanRegister.c still satisfy BridgeDriver.h?
    make install            # to /opt/seamware/plugins/bridge (PLUGIN_DIR=...)

The cor-lib headers are found in `COR_LIBS` (default `..`: the sibling repos, as every cor repo builds).
Without siblings and with the **coraine-dev** package installed, the source of the installed broker
(`/usr/share/coraine/src/coraine-src.tar.xz`) is unpacked into `.coraine-src/` and used instead. For
a broker built with `coraine-build <dir>`: `make COR_LIBS=<dir>`, and
`make install PLUGIN_DIR=<dir>/install/plugins/bridge`.

## Testing

    make test

Starts `coraine` (from `PATH`: the coraine package, or `. <dir>/install/env` after `coraine-build`) on
port 19026 with ramDB - no database server - loads `./ckan.so` by path, and runs
`test/funcTests/cases/*.test` with corTest (`PATH`, or `CORTEST=`). `CKAN_BROKER`, `CKAN_PORT` and
`CKAN_TEST_DIR` override; the broker's log is `/tmp/ckan-bridge-test/broker.log`.

## Running

    coraine --bridges ckan --bridgeConfig bridges.json            # installed in <plugin dir>/bridge
    coraine --bridges $PWD/ckan.so --bridgeConfig bridges.json    # a path: used as it is

The plugin directory is `/opt/seamware/plugins`, or `$SEAMWARE_PLUGIN_DIR`.

## Further reading

- [corBridge](https://github.com/SEAMWARE/corBridge) - the contract; every entry point is documented
  in `BridgeDriver.h` and `BridgeBroker.h`
- coraine: [Bridges and Channels](https://github.com/SEAMWARE/coraine/blob/main/doc/bridge-channels.md),
  [the full reference](https://github.com/SEAMWARE/coraine/blob/main/doc/bridge-channels-details.md),
  [plugin architecture](https://github.com/SEAMWARE/coraine/blob/main/doc/plugin-architecture.md)
- Real bridges: [corMqttBridge](https://github.com/SEAMWARE/corMqttBridge) (a library's own thread,
  notifications), [corModbusBridge](https://github.com/SEAMWARE/corModbusBridge) (polling, a write
  queue), the broker's `src/plugins/bridge/loopback` (every entry point, services and actions)
- [corTest](https://github.com/SEAMWARE/corTest) - the test runner

---

Copyright 2026 Seamware · Apache-2.0
