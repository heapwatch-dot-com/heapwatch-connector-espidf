# Heapwatch Connector for ESP-IDF

ESP-IDF component that connects ESP32 devices to [Heapwatch](https://heapwatch.com) — enrollment, MQTT telemetry, logs, metrics, remote commands, OTA, and coredumps.

**Requires ESP-IDF ≥ 5.3** (tested on 5.3.x).

**Full integration guide:** [docs.heapwatch.com/integration](https://docs.heapwatch.com/integration/getting-started/overview)

## Quick add

`main/idf_component.yml`:

```yaml
dependencies:
  idf:
    version: ">=5.3.0"
  heapwatch_connector_espidf:
    git: git@github.com:heapwatch-dot-com/heapwatch-connector-espidf.git
    version: main
```

`main/CMakeLists.txt` — add `REQUIRES heapwatch_connector_espidf`, then `idf.py reconfigure`.

## Minimal firmware

```c
#include "heapwatch.h"

void app_main(void) {
    ESP_ERROR_CHECK(heapwatch_initialize());
    // ... your application ...
    heapwatch_notify_app_started();
}
```

That is the whole integration. NVS, the event loop, Wi-Fi, SNTP, enrollment, MQTT, the log
relay, the boot counter and crash attribution are all owned by the connector and run on its
own `hw_*` tasks — `heapwatch_initialize()` returns immediately and never blocks your boot.

Configure once:

- Project-root `.env` — build token (firmware upload on `idf.py build`)
- `sdkconfig.defaults.secrets` — `CONFIG_HEAPWATCH_ENROLLMENT_KEY="sk_enroll_…"`
- menuconfig → **Heapwatch Connector** — Wi‑Fi and features

Do **not** put MQTT passwords or device API keys in Kconfig. After admin **Accept**, credentials are stored in NVS.

The in-app wizard [**Connect your first device**](https://app.heapwatch.com/dashboard/connect-first-device) walks through the same steps with copyable snippets.

## Task stack telemetry

Every FreeRTOS task — yours, the connector's and the system's — reports three stack figures
against its configured depth. They answer different questions, so none of them replaces another:

| Figure | Wire | What it measures |
|---|---|---|
| **Now** | `sc` | Stack pointer depth at the moment of the sample. |
| **Max** | `sm` | Deepest stack pointer ever sampled — stack *reserved* by a call frame, whether or not it was written to. |
| **Peak** | `sp` | Deepest stack ever *written*, from the FreeRTOS high-water mark (`0xa5` fill-byte scan). |
| Total | `st` | The task's configured stack depth. |

Max can sit above Peak: a large call frame moves the stack pointer down immediately, but the
high-water mark only moves once those bytes are actually stored to. Peak is the honest
overflow-risk number; Max tells you how little headroom was left even on the frames the task
never filled.

Two cadences, one `hw_metrics` task:

- **Telemetry interval** (`CONFIG_HEAPWATCH_METRICS_INTERVAL_MS`, default 2000 ms) — when the
  figures are published, and the only time the high-water mark is scanned.
- **Task sample interval** (`CONFIG_HEAPWATCH_TASK_SAMPLE_INTERVAL_MS`, default 500 ms) — how
  often the stack pointer is read in between. Max only moves when a sample is taken, so this is
  what decides how short a burst is still caught. Reads three task-control-block fields per task,
  allocates nothing, and is clamped to the telemetry interval.

Both are patchable at runtime from the Heapwatch UI (`tel_int_ms`, `task_smp_ms`).

Known, accepted imprecision:

- Stack geometry comes from the task control block (`pxStackBase` … `pxEndOfStack`), never from
  the heap allocator, so tasks created with `xTaskCreateStatic()` measure exactly like
  heap-allocated ones. `pxEndOfStack` marks the last aligned usable word, so the reported total
  is a few bytes (11 on Xtensa) below the depth you passed to `xTaskCreate*` — that is the true
  usable extent.
- For a task other than the sampler, the stack pointer read is the one saved at its last context
  switch, so a task running on another core is one switch behind, and the saved pointer sits
  below the context-save frame. Both err high, never low.

## License

Copyright © Heapwatch. See file headers. Licensing: info@heapwatch.com
