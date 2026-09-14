# Heapwatch Connector (ESP-IDF)

This is a "connector" for Heapwatch - an ESP-IDF component that connects to Heapwatch servers
and automatically collects, queues and dispatches telemetry data so that Heapwatch can show them in UI or
present via MCP.

## Mandatory Rules for Development

- this connector will be used by thousands of Heapwatch clients - they will add this component to their ESP32
device firmware and use it as development analytics/observability tool during development
- this means that when creating implementation plans, you strategize and think long-term
- the connector must evolve so that it is backwards compatible, unless approved by the user
- the mqtt topic, REST API endpoints and payload, TCP streaming channel protocol - all of these must be
architected in a way so that it is easy to extend WITHOUT breaking backwards compatbility
- you can be sure that at any point in time MANY VERSIONS of this connector will be running at the same time
connected to the backend... and ALL OF THEM must work unless we made breaking changes and bumped MAJOR semver
version

### Tasks

- every new task/FreeRTOS thread has quite some memory overhead
- because of that use tasks sparingly and strategize to use as little memory as possible BUT at the same
time you MUST NOT create significant risk of stack overflow / OOM by task stack sizes that are too low
- if we can reasonably avoid adding a new task, do so - or ask the user which way to go
- every single task in Heapwatch Connector MUST use naming like "hw_log_forward"
- every single task in Heapwatch Connector MUST start with "hw_"
- every single task in Heapwatch Connector MUST have name shorter than 16 chars



### Telemetry

- this connector uses MQTTS and raw TCP over TLS channel to send telemetry data to Heapwatch backend
- because this connector is a part of an IoT device, we MUST assume the internet connection will be
intermittent and can fail at any time
- the default mindset is "connection lost" and if get the connection again, we must take that opportunity
- because of this "no connection by default" idea, we MUST use queues to queue up our data
- without queues, we would NOT be able to report any events that happened during a connection lost window
- so we queue data and then have a separate dispatcher task that once the connection is established, sends
the data to Heapwatch
- if that particular message dispatch failed, we DO NOT discard it - we keep it on the queue
- so the dispatch mechanism must work to kinda peek one item from the queue and send it
- if dispatch fails, we keep it on the queue, if it succeeds, we pop it from the queue

#### Queue Optimization

- because we send out different data payload, the final size of the message can vary
- the queue has a fixed size of N elements with compile-time knows maximum payload size
- to accommodate the largest needed payload, each queue element must be pretty big, which allows us
to store very few entries in the queue
- with the default telemetry collection interval of 1000ms, this would fill up very quickly
- so we split the queues into THREE sizes - small, medium and large
- and we created THREE queues - one for each size
- which queue can be used here depends on whether the payload can fit into the limits of the queue

#### MQTT vs. TCP over TLS Split

- we now have TWO channels to send telemetry to the backend
- the question then is when to use which one
- we use MQTT by default - its easy to use for any messages that fit within MQTT maximum payload
limit that we have set by our queues' limits
- for anything can could shoot ABOVE these limits, we MUST use a different transport
- MQTT needs to "whole thing" to be ready at once - which means it will occupy significant memory
for large payloads
- that's why we added TCP over TLS Streaming Channel to literally stream the data into the backend using
just a small buffer
- TCP over TLS MUST be used for all "unknown size but might be big" transfers - like storage data download/
upload, large command manifests, large config manifests, etc.
- if you are not sure which one to use - ask the user

Workspace context: `heapwatch-firmware/CLAUDE.md` (Lysos `ws`/`rws`, build rules, task naming).

### Public API

- the users will include "heapwatch.h" in their firmware apps and use it to interact with our library
- you MUST keep in SYNC an .md file called PUBLIC_API.md - which contains a TABLE of every single
function if the public API and its description

### Consts

- any const or constexpr MUST be named as FOOBAR_THINGY (uppercase)

### ESP-IDF Log Tags

- multiple files can use the SAME log tag if needed
- log tags should be used for logical grouping
- do not use one log tag = one file - use logical grouping that makes sense
- every connector-owned tag MUST start with `Hw` and use PascalCase (e.g. `HwBle`, not `hw_ota` or `connector_bootstrap`)
- reuse an existing tag from this table; do not invent a new one unless a new subsystem is added:

| Tag | Domain |
|---|---|
| `HwLife` | Bootstrap, lifecycle, C API, settings, boot cycle, MQTT-up boot sync |
| `HwMqtt` | MQTT queues, coredump HTTP, dispatcher |
| `HwEnroll` | Enrollment, LED, self-intel, NVS credentials, credential rotation |
| `HwCmd` | Commands, catalogs, config catalog/snapshot, going-dark, sleep hooks |
| `HwFs` | Device filesystem |
| `HwOta` | OTA (FreeRTOS task name stays `hw_ota`) |
| `HwStream` | Stream ingress TCP/mux |
| `HwNet` | `/network` reporter + netif RX/TX |
| `HwBle` | BLE reporter + NimBLE provider |
| `HwMetrics` | Metrics collector |
| `HwStorage` | SPIFFS/SD stats |

- ESP-IDF component tags (`wifi`, `HTTP_CLIENT`, `mqtt`, …) are not renamed

## Changelog (mandatory)

[`CHANGELOG.md`](CHANGELOG.md) — [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) + **SemVer**. Firmware keeps SemVer; do not tag the connector with cloud CalVer (`v2026.37.4`).

When you ship any **notable** connector change (new/changed MQTT commands, config, OTA behaviour, breaking API), **add a user-facing bullet under `[Unreleased]` in the same change**. Do not paste commit logs. On **git tag**, promote `[Unreleased]` to `[X.Y.Z] - YYYY-MM-DD`, then add a fresh empty `[Unreleased]`. Commit to `heapwatch-dot-com/heapwatch-connector-espidf`. Follow the `changelog` skill in the umbrella repo (`.claude/skills/changelog/SKILL.md`).
