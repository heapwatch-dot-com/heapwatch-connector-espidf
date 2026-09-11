# Heapwatch Connector for ESP-IDF

ESP-IDF component that connects ESP32 devices to [Heapwatch](https://heapwatch.com) — enrollment, MQTT telemetry, logs, metrics, remote commands, OTA, and coredumps.

**Requires ESP-IDF ≥ 5.1** (tested on 5.3.x).

**Full integration guide:** [docs.heapwatch.com/integration](https://docs.heapwatch.com/integration/getting-started/overview)

## Quick add

`main/idf_component.yml`:

```yaml
dependencies:
  idf:
    version: ">=5.1.0"
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

## License

Copyright © Heapwatch. See file headers. Licensing: info@heapwatch.com
