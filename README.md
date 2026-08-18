# Heapwatch Connector for ESP-IDF

ESP-IDF component that connects ESP32 devices to [Heapwatch](https://heapwatch.com) for crash analytics, logs, metrics, remote commands, OTA, filesystem access, and device specs.

Requires **ESP-IDF ≥ 5.1** (tested on 5.3.x).

## Add to your project

### Component Manager (git dependency)

```yaml
# main/idf_component.yml
dependencies:
  idf:
    version: ">=5.1.0"
  heapwatch_connector_espidf:
    git: git@github.com:viktorjamrich/heapwatch-connector-espidf.git
    version: main
```

### Local path (development)

```yaml
dependencies:
  heapwatch_connector_espidf:
    path: ../heapwatch-connector-espidf
```

### CMake

```cmake
# main/CMakeLists.txt
idf_component_register(
    SRCS "main.cpp"
    REQUIRES heapwatch_connector_espidf
)
```

Then:

```bash
idf.py menuconfig   # Component options → "Heapwatch Connector"
idf.py build
```

## Minimal `app_main`

```c
#include "heapwatch.h"

void app_main(void) {
    ESP_ERROR_CHECK(heapwatch_initialize());
    // ... your application code runs immediately; enrollment/MQTT are sidecar background tasks
}
```

Build metadata (app version, connector version, build fingerprint) is generated automatically — do **not** pass version fields into `heapwatch_initialize()`.

Credentials are **never** passed in code or Kconfig. On first boot the device enrolls with `CONFIG_HEAPWATCH_ENROLLMENT_KEY`; after operator Accept + confirm, credentials live in NVS for subsequent boots.

See the [heapwatch-firmware-crasher](https://github.com/viktorjamrich/heapwatch-firmware-crasher) lab app for a full integration example (enrollment, OTA, commands, crash harness).

## Production endpoints (default)

Out of the box, firmware talks to **production** Heapwatch. These are fixed in `heapwatch_endpoints.h` (not Kconfig):

| Service | URL |
|---------|-----|
| REST API | `https://api.heapwatch.com` (port **443**) |
| MQTT (MQTTS) | `mqtts://mqtts.mqtt.heapwatch.com:8883` |
| Stream ingress (device TCP/TLS) | `stream.heapwatch.com:8910` (Caddy L4 TLS → Go mux) |
| Stream ingress (browser WSS) | `wss://stream.heapwatch.com:443` (dashboard; not used by firmware) |

`CONFIG_HEAPWATCH_BUILD_ENV` (`development` / `testing` / `staging` / `production`) is a **reporting label** sent during enrollment and firmware upload. It does **not** change which servers the device connects to.

### Local dev against a LAN / docker stack

For **internal Heapwatch development only**, compile with `HEAPWATCH_DEV_LOCAL_NETWORK` in your **app** project (never ship this to customers):

```cmake
# app CMakeLists.txt — before project()
add_compile_definitions(HEAPWATCH_DEV_LOCAL_NETWORK)
```

That swaps API/MQTT/stream hosts to the lab LAN endpoints defined in `heapwatch_endpoints.h`. Edit those literals for your machine if needed.

## Kconfig (`menuconfig` → Heapwatch Connector)

| Area | What to set |
|------|-------------|
| **Device enrollment** | `HEAPWATCH_ENROLLMENT_KEY` (required per firmware batch), poll interval |
| **Wi-Fi** | `HEAPWATCH_MANAGE_WIFI`, SSID/password (or let the host app manage networking) |
| **Build environment** | `HEAPWATCH_BUILD_ENV` — reporting label only |
| **Features** | log relay, specs, OTA, coredump upload, stream ingress, BLE, storage stats, … |
| **Queues / timing** | metrics interval, log batch size, queue depths |

Enabling **task snapshots** and/or **CPU snapshots** auto-selects FreeRTOS trace + runtime stats (`FREERTOS_USE_TRACE_FACILITY`, `FREERTOS_GENERATE_RUN_TIME_STATS`, ESP Timer clock, `uint32_t` counter). You do not need to paste those flags into `sdkconfig` by hand.

Choosing **Debug** as the minimum relayed log severity implies ESP-IDF `LOG_DEFAULT_LEVEL_DEBUG` so debug lines are compiled in.

If log collection is on and `CONFIG_FREERTOS_MAX_TASK_NAME_LEN` is below 16, CMake prints a warning at configure time (the default is 16).

### Enrollment workflow

1. In Heapwatch UI: **Enrollment → Enrollment keys** → create a key (copy the plain `sk_enroll_…` once).
2. `menuconfig` → **Heapwatch Connector** → **Device enrollment** → paste the key.
3. Flash one firmware image to many devices (same key per batch).
4. Each device POSTs enroll on first boot → appears in **Enrollment → Pending devices**.
5. Operator **Accept** → device polls until provisioned → fetch/confirm → credentials saved to NVS.

There is no Integration-tab credential path, no manual device creation, and no Kconfig login fallback.

## Build metadata & firmware upload (automatic)

When this component is a dependency of your app, ESP-IDF runs the build pipeline on every `idf.py build` — **no extra CMake in your app**.

| Step | Output |
|------|--------|
| Pre-build | `build/generated/heapwatch_build_metadata.h` |
| Post-link | Final `HEAPWATCH_BUILD_IDENTIFIER` (sha256 fingerprint), optional relink |
| Post-build | Upload to Heapwatch API (`POST /api/applications/{id}/firmwares/ci-upload`) |

Generated macros:

| Macro | Source |
|-------|--------|
| `HEAPWATCH_APP_SOURCE_VERSION` | `git describe` in **your firmware repo**, or empty when no VCS |
| `HEAPWATCH_CONNECTOR_SOURCE_VERSION` | `git describe` in **this component** checkout, or empty when no VCS |
| `HEAPWATCH_BUILD_IDENTIFIER` | `sha256(elf + partition table + sdkconfig)` after link |

Device specs MQTT payload includes `app_version` and `connector_source_version` from these strings.

`heapwatch_init()` fails if the build ID is still the pre-link placeholder (i.e. the post-link step did not run). Always flash a binary from a completed `idf.py build`.

### Upload credentials (`.env` in your firmware project root)

Pre-build metadata generation and post-build upload both require a `.env` file next to `sdkconfig` (gitignored). If any required key is missing, the build fails immediately with a list of what to set. Required keys:

```env
HEAPWATCH_API_BASE=https://api.heapwatch.com
HEAPWATCH_BUILD_TOKEN=sk_build_...
HEAPWATCH_TENANT_UUID=<tenant-uuid>
HEAPWATCH_APPLICATION_ID=<application-id>
```

Optional:

```env
HEAPWATCH_BUILD_ENV=development
HEAPWATCH_FIRMWARE_DESCRIPTION=My nightly build
HEAPWATCH_UPLOAD_STRICT=1          # fail the build if upload fails (default: warn only)
```

For local API during dev, set `HEAPWATCH_API_BASE` to your stack (e.g. `http://localhost:8090`) — this only affects the **upload script**, not the firmware runtime endpoints (unless you also use `HEAPWATCH_DEV_LOCAL_NETWORK`).

Create a build token in the Heapwatch UI: **Application → Build tokens**.

### Other upload requirements

- `partitions.csv` in the firmware project root
- **Git is optional.** ZIP downloads or copied folders without `.git` build fine — version macros are empty and the upload sends `null` for `source_version` / `connector_source_version`. `build_identifier` (sha256 fingerprint) is always required and is what coredumps/OTA use.
- Optional env overrides without git: `HEAPWATCH_APP_SOURCE_VERSION`, `HEAPWATCH_CONNECTOR_SOURCE_VERSION` (also read at pre-build).
- Backend must accept nullable `connector_source_version` on firmware upload (Heapwatch app ≥ 2026-08)

Scripts live in `scripts/`; wiring is in `CMakeLists.txt` (pre-build) and `project_include.cmake` (post-build upload target `heapwatch_firmware_upload`).

## Public C API

Single header: `heapwatch.h`

- Lifecycle: `heapwatch_init`, `heapwatch_set_network_ready`, `heapwatch_deinit`
- Telemetry: `heapwatch_queue_event`, `heapwatch_queue_log`, `heapwatch_update_parameter_*`, `heapwatch_queue_specs`
- Commands: `heapwatch_register_command`, sleep hooks, config catalogs
- Metadata getters: `heapwatch_get_build_identifier()`, `heapwatch_get_app_source_version()`, `heapwatch_get_connector_source_version()`

## Repository layout

```
heapwatch.h                 Public C API
heapwatch_endpoints.h       Production vs dev network targets
heapwatch_config.h          Shared types / constants
Kconfig.projbuild           menuconfig options
CMakeLists.txt              Component sources + pre-build metadata
project_include.cmake       Post-build fingerprint + upload (client projects only)
scripts/                    generate_build_metadata, compute_build_identifier, upload
certs/                      TLS trust (local dev CA when needed)
```

## License

Copyright © Viktor Jamrich Enterprises s.r.o. See file headers and contact admin@heapwatch.com for licensing.
