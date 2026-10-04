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
    git: https://github.com/heapwatch-dot-com/heapwatch-connector-espidf.git
    version: v0.22.2
```

`main/CMakeLists.txt` — add `REQUIRES heapwatch_connector_espidf`, then `idf.py reconfigure`.

## Minimal firmware

```c
#include "heapwatch.h"

void app_main(void) {
    ESP_ERROR_CHECK(heapwatch_initialize());
    // ... your application (mount storage, then heapwatch_register_sd_volume, etc.) ...
    heapwatch_notify_app_started();
}
```

That is the whole integration. NVS, the event loop, Wi-Fi, SNTP, enrollment, MQTT, the log
relay, the boot counter and crash attribution are all owned by the connector and run on its
own `hw_*` tasks — `heapwatch_initialize()` returns immediately and never blocks your boot.

Configure once:

- Project-root `.env` — optional. When the build-upload keys are missing, `idf.py build` prints a warning and skips firmware upload
- `sdkconfig.defaults.secrets` — `CONFIG_HEAPWATCH_ENROLLMENT_KEY="sk_enroll_…"`
- menuconfig → **Heapwatch Connector** — Wi‑Fi and features

Do **not** put MQTT passwords or device API keys in Kconfig. After admin **Accept**, credentials are stored in NVS.

The in-app wizard [**Connect your first device**](https://app.heapwatch.com/dashboard/connect-first-device) walks through the same steps with copyable snippets.

## Task stack telemetry

Every FreeRTOS task — yours, the connector's and the system's — reports stack figures against its
configured depth. They answer different questions, so none replaces another:

| Figure | Wire | What it measures |
|---|---|---|
| Name | `name` | FreeRTOS task name. |
| Total | `total` | The task's configured stack depth. |
| **Now** | `now` | Stack pointer depth at this telemetry sample. |
| **Min** | `min` | Lowest **Now** seen for this task instance. |
| **Max** | `max` | Highest **Now** seen for this task instance. |
| **Peak** | `peak` | Deepest stack ever *written*, from the FreeRTOS high-water mark (`0xa5` fill-byte scan). |
| CPU | `cpu` | CPU percent over the telemetry interval. |

Min and Max are a running range of Now: the first valid sample seeds both; later valid samples
ratchet `min` down and `max` up. A failed stack-pointer read keeps the last good Now and does
not feed 0 into the range. A new task instance (same name, new handle) resets the range.

**Peak is the overflow-risk number.** Overflow is a *write* past the end of the stack, and the
high-water mark records exactly that, whenever it happened — no sampling luck required.

Figures are collected and published on the telemetry interval
(`CONFIG_HEAPWATCH_METRICS_INTERVAL_MS`, default 2000 ms, patchable at runtime as `tel_int_ms`).
When the task list does not fit one large MQTT slot (`HEAPWATCH_LARGE_PAYLOAD_SIZE`), it is
split across several `TASKS` publishes. Each message is a complete JSON subset; ingest is
per-task, so sending A+B then C+D is the same as one message with all four.

Known, accepted imprecision:

- Stack geometry comes from the task control block (`pxStackBase` … `pxEndOfStack`), never from
  the heap allocator, so tasks created with `xTaskCreateStatic()` measure exactly like
  heap-allocated ones. `pxEndOfStack` marks the last aligned usable word, so the reported total
  is a few bytes (11 on Xtensa) below the depth you passed to `xTaskCreate*` — that is the true
  usable extent.
- **Now is a blocking-point reading for every task but one.** FreeRTOS writes `pxTopOfStack` only
  when a task is switched out, so the depth read from outside is the one frozen at the task's last
  context switch — in practice the depth at its blocking call (`xQueueReceive`, `vTaskDelay`).
  While a task actually runs, its stack pointer lives in a CPU register and is not readable from
  another task. The collector reads its own live stack pointer, and a task running on another core
  is one switch behind; the saved pointer sits below the context-save frame, so all of these err
  high, never low. Min and Max are still the observed range of those Now samples.

## License

Apache License 2.0. See [LICENSE](LICENSE).
