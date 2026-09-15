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

### Types

- never use old school C types that have no size specification that also vary between architectures like
"unsigned", "long" - you have no idea how this will switch between devices and architectures
- ALWAYS use fixed size types like "uint8_t" or "int64_t" etc. UNLESS a third-party (not our code) consumer
already expects these old-school types - in that case we MUST match it to be compatible

### Consts

- any const or constexpr MUST be named as FOOBAR_THINGY (uppercase)

### Namespaces

- namespaces MUST use naming like this - THE ARE ONLY EXAMPLES - "Heapwatch" NOT "heapwatch", "Heapwatch::Connectors" NOT "heapwatch::connectors", "Heapwatch::Connectors::BluetoothLe" NOT "heapwatch::connectors::bluetooth_le", "Heapwatch::Helpers" NOT "heapwatch::helpers"
- the root namespace is Heapwatch
- sub-namespaces differ by domain

### Naming

- you MUST give things PROPER CLEAR AND MEANINGFUL names
- do NOT try to be clever and make up fancy names nobody understands
- bad example: function name for handling messages from system event loop (should be workload easy) "serveNetMailbox" => NOBODY HAS A CLUE WHAT THAT MEANS! "mailbox" ?! everybody will think it has something to
do with POP3 or IMAP! you MUST NEVER make confusing naming like this
- good example: "handleNotificationsFromSystemEventLoop()" => yes, it's long and IT'S SUPER CLEAR! that's exactly how it should be!
- name things in SUPER CLEAR why - it does not mater it's longer - CLEAR is MORE IMPORTANT than confusing and short.

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
| `HwCmd` | Commands, catalogs, config catalog/snapshot, commanded-offline, sleep hooks |
| `HwFs` | Device filesystem |
| `HwOta` | OTA (FreeRTOS task name stays `hw_ota`) |
| `HwStream` | Stream ingress TCP/mux |
| `HwNet` | `/network` reporter + netif RX/TX |
| `HwBle` | BLE reporter + NimBLE provider |
| `HwMetrics` | Metrics collector |
| `HwStorage` | SPIFFS/SD stats |

- ESP-IDF component tags (`wifi`, `HTTP_CLIENT`, `mqtt`, …) are not renamed

### Class, file, and architecture style

These rules apply to **new** connector C++. They are the same house style as Remoted Node GPS firmware. Existing sections still win on their own topics: Naming (clear long names), Namespaces (PascalCase), Consts (uppercase), Types (fixed-width), Tasks (`hw_` + under 16 chars), Telemetry (queue then dispatch).

Do not invent a parallel naming scheme. Do not rewrite the whole tree to match this in one go — follow it when you add or touch a type.

#### Domain layers

Keep a type in the layer it belongs to. Do not mash a hardware driver, a protocol client, and a command handler into one class.

- **Helpers** — reusable utilities with no product workflow
- **Adapters / drivers** — talk to a chip, netif, radio, filesystem, or other hardware/OS surface
- **Clients** — a protocol on top of an adapter (MQTT, HTTP, stream, …)
- **Services / reporters / collectors** — long-running connector subsystems
- **Commands** — one command (or a tight family) per type when that command has its own execute path
- **Data** — DTOs and message structs

Folder path and namespace stay aligned (see Namespaces). The public C API in `heapwatch.h` stays C — these rules are for internal C++.

#### Classes: `I` / `Abstract` / concrete

When you add a C++ type that could have **more than one implementation**, **or** a **lifecycle** (start / stop / sleep / bootstrap / setup), use this triad:

| Kind | Prefix | Rule |
|---|---|---|
| Interface | `I` | Pure virtuals + virtual destructor. Tiny surface. Example: `IMqttClient`, `IRadioBleProvider` |
| Abstract base | `Abstract` | Public lifecycle methods are `final`. The subclass only implements `onX()` hooks. Example: `AbstractSystemService` |
| Concrete | domain noun | Real work. PascalCase, clear, not clever. Example: `SimMqttClient`, `NimBleProvider` |

Do **not** wrap every helper, POD, or C API in `IFoo`. If there is one implementation, no lifecycle and for that class it does not make sure to have multiple implementations, then a single concrete class is enough.

Single concrete classes are good for say singletons, data models, etc.

Any component where the point is the INTERFACE - so that you can easily connect it to other parts of the system - there you must use the "I" (interface) layer, Abstract layer and particular class layer.

#### Template method (`onX` hooks)

The public method owns the invariant (status, locking, “do not start twice”). The hook owns the work.

| Public API | Subclass hook |
|---|---|
| `start()` / `stop()` / `sleep()` / `bootstrap()` / `setup()` | `onStart()` / `onStop()` / `onSleep()` / `onBootstrap()` / `onSetup()` |
| queueing entry (`logMessage`, enqueue, …) | `dispatch…` / handle hook that actually sends or processes |

- Event / callback methods: `on` + what happened — `onIncomingMqttCommand()`, `onMqttMessageReceived`
- Task **functions**: `task` + verb phrase — `taskProcessQueuedMessages()`, `taskHandleAsyncRx` (the FreeRTOS **name string** is still `hw_…` under 16 chars — see Tasks)
- ISR: `isrOn…`

Boolean methods: `is` / `has` / `set`. Enum **type** is PascalCase (`MqttConnectionState`). Enum **value** is `TYPE_SCREAMING` (`MQTT_CONNECTION_DISCONNECTED`).

#### Files

- One class → one header named after the class, PascalCase: `DeviceCommandTask.h`
- You MAY split the `.cpp` by concern. The hyphen is the **concern**, not a kebab-case class name:
  - `ClassName.cpp` — constructor, shared members, start/stop of tasks
  - `ClassName-Concern.cpp` — one protocol or feature (`Sim7080Adapter-Mqtt.cpp`)
  - `ClassName-Worker-Role.cpp` — the FreeRTOS task body for that role (`SimMqttClient-Worker-Queued.cpp`)
- Split when one `.cpp` would mix a worker loop with setup, or mix two protocols. Do not split a small helper into five files.

Producer / ISR / system callback **only enqueues**. A worker task does the heavy work. That is the same rule as Telemetry — it also applies to logs, commands, and radio.

#### Hungarian member prefixes — IGNORE (not in force)

**IGNORE this subsection. It is NOT a rule. Do not apply it.**

Do not name new members this way. Do not rename existing members to this scheme. The table is commented out on purpose: it is a preservation copy so we can turn it on later without reconstructing it. Until the user explicitly enables it, member names follow Naming (clear, meaningful) and existing connector practice — not Hungarian.

<!--
INACTIVE — DO NOT APPLY. Hungarian member prefixes (preserved only).

Prefix | Meaning                              | Example
m      | member object / string / enum        | mDeviceId, mMqttState
mb     | member bool                          | mbDebugEnabled, mbPreventAnySuspend
mu     | member unsigned numeric              | muStackSize, muMqttTransmitErrorCount
mp     | member pointer / shared_ptr          | mpLog, mpAdapter
mh     | member FreeRTOS handle               | mhAsyncRxTask, mhProcessingTask
mmx    | member mutex / semaphore             | mmxReadWriteMutex, mmxStorageMutex
mmap   | member map                           | mmapServices
g / gp | global / global pointer              | gApp, gpModem, gpLog
pThis  | local "this" after task-arg cast     | pThis

-->

## Changelog (mandatory)

[`CHANGELOG.md`](CHANGELOG.md) — [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) + **SemVer**. Firmware keeps SemVer; do not tag the connector with cloud CalVer (`v2026.37.4`).

When you ship any **notable** connector change (new/changed MQTT commands, config, OTA behaviour, breaking API), **add a user-facing bullet under `[Unreleased]` in the same change**. Do not paste commit logs. On **git tag**, promote `[Unreleased]` to `[X.Y.Z] - YYYY-MM-DD`, then add a fresh empty `[Unreleased]`. Commit to `heapwatch-dot-com/heapwatch-connector-espidf`. Follow the `changelog` skill in the umbrella repo (`.claude/skills/changelog/SKILL.md`).

## CLAUDE.md Update

- CLAUDE.md file is an instruction/rule set for AI agents
- because of this, it is an asset that need long lifetime
- we update it / improve it when needed to make the AI agent's output even better
- because of this you MUST NOT put garbage into CLAUDE.md - this file MUST be super clear
- CLAUDE.md MUST be super clear - which means you MUST never put garbage and junk into it
- you must also NEVER put short-lived DETAILS into it, that will require constant synchronization - you can be sure it would DRIFT!
- what are "short-lived details" - precise listing of parts that will constantly change and you will need to update it over and over again like "Folder FooBar contains graphics files, icons, logos, banners, sounds, music..." => this is GARBAGE, because the moment we change anything you must rewrite this!