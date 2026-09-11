# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed
- `ota/rollback` while a firmware is awaiting confirmation now invalidates the running image and lets the bootloader return to the app that is already valid, so the restored firmware boots **confirmed**. It previously only moved the boot pointer, which brought the restored app up unverified: the dashboard kept showing *Firmware confirmation required*, and the next reboot could bounce the device back into the image you just rejected.

## [0.10.0] - 2026-09-11

### Added
- **Boot identity is connector-owned.** `heapwatch_initialize()` now advances the NVS boot counter, applies `ESP_RST_PANIC` crash attribution, and queues `STARTING_UP` plus deep-sleep wake telemetry (`wake_cause_code`, `sleep_duration_ms`). The server boot-cycle resume — for devices whose NVS was erased but which still hold fleet credentials — runs on `hw_bootstrap` after MQTT activation. Host apps no longer write any of this; a client `app_main` is `heapwatch_initialize()` plus `heapwatch_notify_app_started()`.
- `heapwatch_notify_app_started()` queues `STARTED_UP`, so the event marks the end of the *host app's* own start-up instead of being emitted back-to-back with `STARTING_UP`.

### Removed
- **BREAKING:** Removed public `heapwatch_set_boot_cycle_context()`. The connector reads and advances the boot counter itself, so there is nothing for the host to feed in. Clients that called it should delete the call along with their own boot-counter, `STARTING_UP` and deep-sleep-wake wiring.

### Fixed
- Telemetry no longer reports `boot_cycle=0` for the first payloads of a boot. The counter was previously set by the host app *after* `heapwatch_initialize()` returned, so the MQTT-up boot sync could publish specs and events before it landed. This also removes the compensating duplicate `specs` publish that ran on every boot — specs are now re-queued only if the counter actually changes (server resume).

## [0.9.0] - 2026-09-11

### Added
- MQTT `specs` payloads include `boot_cycle` (same counter as events). Standard events always include `boot_cycle` (defaults to the connector’s current boot).

### Fixed
- `config/set` accepts JSON types from the field catalog (int/enum numbers, bools, strings) instead of rejecting every non-number/bool before `setValue`.

## [0.8.0] - 2026-09-11

### Added
- Auto-mounts every flash `data/spiffs`, `data/littlefs`, and `data/fat` partition at `/<label>` (Kconfig **Auto-mount all storage partitions**, default on). Already-mounted volumes are treated as success and stay host-owned.
- Kconfig **Auto-format blank storage partitions** (default on) formats only when the connector itself performs the mount.
- Public `heapwatch_register_fs_volume(label, mount_path)` before `heapwatch_initialize()`: custom mount path when auto-mount is on, or required mapping when auto-mount is off.
- File browser and `storage.free_bytes` / `storage.used_bytes` cover every mounted SPIFFS, LittleFS, and FatFS volume.

### Fixed
- Partition table `total_bytes` is the flash partition size again (not SPIFFS/FAT usable capacity, which was drawing fake unused-flash gaps).
- LittleFS (`0x83`) is recognized as a filesystem: named in the partition dump, auto-mounted, and listed as a file-browser volume.
- File-browser `modified_at` is JSON `null` when the filesystem has no mtime (`stat` epoch 0), instead of `1970-01-01`.

## [0.7.0] - 2026-09-10

### Changed
- **BREAKING:** `heapwatch_deinit()` is now a real teardown instead of a partial stop. It stops `hw_bootstrap`, `hw_log_forward`, `hw_dispatcher`, `hw_metrics`, `hw_command`, `hw_going_dark`, `hw_ota`, `hw_streaming`, `hw_ble_report` and `hw_enroll_led`, restores the ESP log `vprintf` hook, unregisters the IP/Wi-Fi event handlers, stops managed Wi-Fi and SNTP, and clears every once-per-boot flag (catalogs, config snapshot, boot static sync, claimed identities, coredump upload). `initialize() → deinit() → initialize() → deinit()` now cycles cleanly. NVS, the default event loop, and host-owned netifs are deliberately left intact.

### Removed
- **BREAKING:** Removed unused public API `heapwatch_config_t` / `heapwatch_config_default` / `HEAPWATCH_CONFIG_DEFAULT`, deprecated alias `heapwatch_get_firmware_version`, and `heapwatch_update_cellular_signal` (use `heapwatch_update_parameter_int` for custom cellular slugs if needed). Use `heapwatch_get_app_source_version()` for app git-describe.
- **BREAKING:** Removed public `heapwatch_queue_specs()` — device specs are queued internally on every MQTT connect, on `specs_refresh`, and after OTA.
- **BREAKING:** Removed public `heapwatch_sync_claimed_identities()` — the claimed MAC/UID backfill already runs once per boot from the dispatcher as soon as the network is up.

## [0.6.1] - 2026-09-10

### Changed
- Connector ESP_LOG tags are grouped by subsystem (`HwLife`, `HwMqtt`, `HwEnroll`, `HwCmd`, `HwFs`, `HwOta`, `HwStream`, `HwNet`, `HwBle`, `HwMetrics`, `HwStorage`). All connector-owned tags start with `Hw`.

### Security
- Log forwarding redacts `Authorization: Bearer` tokens as `(REDACTED)` so device API keys are not uploaded as logs.

## [0.6.0] - 2026-09-10

### Added
- Per-netif monotonic RX/TX byte counters since boot: `dynamic.net.{seg}.rx_bytes` / `.tx_bytes` on `/parameter`, and `rx_bytes` / `tx_bytes` on each `/network` netif. Counted from lwIP `input` / `linkoutput` `pbuf` lengths (ESP-IDF 5.3 has no `IP_EVENT_TX_RX`).
- Kconfig Features option **Collect Network Interface Bytes Transmitted/Received**.
- Dynamic `/parameter` keys use `dynamic.{family}.{segment}.{metric}` for task, cpu, net, and power, with a shared segment sanitizer.

### Fixed
- Traffic reporting no longer forces `MIB2_STATS` on lwIP (that crashed in `esp_netif_next_unsafe` about 7s after boot). WiFi STA MIB2 octet counters were also stuck at a bogus ~1 GiB TX value.
