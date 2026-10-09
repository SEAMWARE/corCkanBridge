# corCkanBridge

A **bridge plugin** for the coraine NGSI-LD broker: `ckan.so` reads open data from a
[CKAN](https://ckan.org) portal - the data portal behind many city and government open-data sites -
and the broker makes entities of it. Each row of a DataStore resource is one record; a **record
Channel** in the broker says which entities a record becomes: which column is the entity id, which
columns are Properties, which two make a `location` Point, which one names another entity
(a Relationship).

    CKAN portal  --datastore_search-->  ckan.so  --(resource id, row, time)-->  broker  --record Channel-->  entities

The bridge **knows nothing about entities**: it carries `(endpoint, json, time)` - the resource id,
one row as a JSON object, the row's time - and the mapping is the broker's.

## What it does

**Mode `once`**: when the broker starts, every configured resource is read whole, page by page
(`datastore_search`, `limit`/`offset`, sorted by `_id`), each row handed to the broker, and the
resource is done. A restart reads it again; the broker's write is an upsert, so the same rows leave the
same entities.

- **HTTP(S)** is the broker's own client (corRest, resolved from the running broker): nothing is linked
  into the plugin.
- **The time** of a row is its `timeColumn` (ISO 8601), if the Channel names one; else when its page
  was read. It becomes `observedAt` on every attribute the row writes.
- **Failures**: a portal that cannot be reached, a timeout or a 5xx is tried three times (again after
  1 s, then 3 s); a 4xx is the answer. A resource that cannot be read leaves its Channel **dormant**,
  with the portal's own reason, on `GET /ngsi-ld/v1/channels` - `the portal answered 404 - Not Found
  Error: Not found: Resource "..." was not found.` The broker starts and stays up whatever the portal
  does; the portal is not contacted before the broker is up.
- **The API key** is sent as the `Authorization` header and appears nowhere else: not in a log line,
  not on `GET /ngsi-ld/v1/channels`.

`poll` (follow the dataset's activity stream, then read the new rows by `_id`, or the whole resource
when it was replaced) and `push` (a webhook, where the portal has `ckanext-webhooks`) are reserved
mode names, refused today with the reason "not built yet".

## Configuration

One `--bridgeConfig` file serves every bridge: one member per alias. `server` is this plugin's; the
`ngsild` section is the broker's - its `records` make the Channels, one per resource.

```json
{ "ckan": {
    "server": { "url": "https://data.boston.gov", "timeoutMs": 30000 },
    "ngsild": {
      "defaultEntity": false,
      "records": {
        "44c931aa-4577-4caa-a8f2-ed1e38263e70": {
          "channelInfo": [ { "key": "mode",     "value": "once" },
                           { "key": "pageSize", "value": "100"  } ],
          "entities": [
            { "type": "BikeStation", "id": "urn:ngsi-ld:BikeStation:{Number}",
              "attributes": {
                "name":       { "column": "Name" },
                "totalDocks": { "column": "Total_docks", "as": "number" },
                "public":     { "column": "Public_", "as": "boolean" },
                "location":   { "point": { "longitude": "Longitude", "latitude": "Latitude" } },
                "inDistrict": { "relationship": "urn:ngsi-ld:District:{District}" } } },
            { "type": "District", "id": "urn:ngsi-ld:District:{District}",
              "attributes": { "name": { "column": "District" } } } ] } } } } }
```

That is Boston's Blue Bike stations (data.boston.gov, licence ODC PDDL): 572 rows become 572
`BikeStation`s, each with a Relationship to one of 13 `District`s.

### `server` - the portal

| member | | |
|---|---|---|
| `url` | mandatory | the portal, `https://...` or `http://...` |
| `apiKey` | optional | a CKAN API token, sent as `Authorization` - for a private resource |
| `apiKeyEnv` | optional | the name of an environment variable holding the token, instead of `apiKey` |
| `timeoutMs` | optional | one HTTP request, 100-600000, default 30000 |

### `channelInfo` - how to read one resource

| key | | |
|---|---|---|
| `mode` | `once` | the default and the one built; `poll` and `push` are refused, "not built yet" |
| `pageSize` | `1`-`32000` | rows per `datastore_search` call, default 100 |
| `timeColumn` | a column | each row's time, ISO 8601 (`2026-10-09T12:00:00Z`, `2026-10-09 12:00:00`, `2026-10-09`); a row without a readable one gets its read time |

An API key in `channelInfo` is refused (the Channel is dormant, and says why): `channelInfo` is shown
on `GET /ngsi-ld/v1/channels`. Any other key is refused as unknown.

### `records` - the broker's record Channels

The endpoint (the member's name) is the resource id: a UUID, or a DataStore alias (letters, digits,
`.`, `_`, `-`). `entities` lists what each row makes:

- `id` - a template: `{Column}` is the column's value, each character outside `[A-Za-z0-9._-]`
  replaced by `_` (`South Boston` -> `South_Boston`). A row without a value in a column the id names
  makes no entity of this kind.
- `attributes` - `{ "column": ... }` a Property (`"as": "number" | "boolean" | "string"` converts
  `"15"` to 15, `"Yes"` to true); `{ "point": { "longitude": ..., "latitude": ... } }` a GeoProperty
  Point; `{ "relationship": <template> }` a Relationship. A row without a value for one - an absent,
  null or empty column, a value that does not convert - leaves that attribute out.

All the entities of one row are written as one batch upsert with `options=update`: created when they
are not there, their mapped attributes replaced, any other attribute left alone. Record Channels are
coraine's (`COR_FEATURE_BRIDGE_RECORDS`): see coraine's
[Bridges and Channels](https://github.com/SEAMWARE/coraine/blob/main/doc/bridge-channels.md#record-channels).

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

It needs a broker with record Channels (coraine, `COR_FEATURE_BRIDGE_RECORDS`). With corBridge
headers of ABI 12 and a broker that has `channelStatusIn` (`COR_FEATURE_CHANNEL_STATUS_IN`), a
Channel that cannot be read says why on `GET /ngsi-ld/v1/channels`; without, only the broker's log
does.

## Testing

    make test

No internet: `test/funcTests/tools/ckanMock.py` is the portal, answering from
`test/funcTests/fixtures/` - three pages of data.boston.gov's Blue Bike stations (resource
`44c931aa-4577-4caa-a8f2-ed1e38263e70`, rows 1-50, recorded 2026-10-09, licence
[ODC PDDL](https://opendatacommons.org/licenses/pddl/)), its answer for a resource it does not have,
and `edge-cases.json` (written for the tests, not recorded: empty, null and unreadable columns).

Starts `coraine` (from `PATH`: the coraine package, or `. <dir>/install/env` after `coraine-build`) on
port 19026 with ramDB - no database server - and the mock on 19027, loads `./ckan.so` by path, and runs
`test/funcTests/cases/*.test` with corTest (`PATH`, or `CORTEST=`). `CKAN_BROKER`, `CKAN_PORT`,
`CKAN_MOCK_PORT` and `CKAN_TEST_DIR` override; the logs are `/tmp/ckan-bridge-test/broker.log` and
`mock.log`.

| test | |
|---|---|
| `ckan_records` | 50 rows in three pages of 20 -> 50 BikeStations (Properties, location, Relationship) and 2 Districts; the API key sent, and shown nowhere |
| `ckan_record_edges` | empty, null and unreadable columns: what each row still makes; a row that makes nothing is not taken |
| `ckan_channel_status` | a resource the portal does not have, a 503 then the rows, refused `channelInfo`: dormant with the reason, the broker up |
| `ckan_portal_unreachable` | nothing listening: the broker starts, the Channel goes dormant after three tries |

## Running

    coraine --bridges ckan --bridgeConfig bridges.json            # installed in <plugin dir>/bridge
    coraine --bridges $PWD/ckan.so --bridgeConfig bridges.json    # a path: used as it is

The plugin directory is `/opt/seamware/plugins`, or `$SEAMWARE_PLUGIN_DIR`. With the configuration
above, against data.boston.gov: 572 rows in 6 pages, 572 BikeStations and 13 Districts, the read
taking 0.96 s (a debug build, on 2026-10-09).

## Further reading

- [corBridge](https://github.com/SEAMWARE/corBridge) - the contract; every entry point is documented
  in `BridgeDriver.h` and `BridgeBroker.h`
- coraine: [Bridges and Channels](https://github.com/SEAMWARE/coraine/blob/main/doc/bridge-channels.md),
  [the full reference](https://github.com/SEAMWARE/coraine/blob/main/doc/bridge-channels-details.md)
- CKAN's [DataStore API](https://docs.ckan.org/en/latest/maintaining/datastore.html#the-datastore-api)

---

Copyright 2026 Seamware · Apache-2.0
