# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.12.0] - 2026-09-12

### Removed
- **BREAKING:** the task stack **max** figure (`sm`) and its menuconfig option *Task Stack Sampling Interval (ms)* (`CONFIG_HEAPWATCH_TASK_SAMPLE_INTERVAL_MS`) and remote setting `task_smp_ms`, both added in 0.11.0. The figure was meant to show the deepest stack a task ever *reserved*, but it could not: FreeRTOS records a task's stack pointer only when that task is switched out, so reading it from the collector only ever sees the depth at the task's blocking call — never the deep call frames it reaches while running. On hardware, 19 of 21 tasks reported max equal to now while only the high-water mark moved. Sampling more often cannot help, because the recorded pointer does not change between context switches. **Use Peak** (`sp`, the high-water mark): overflow is a *write* past the end of the stack, and Peak records exactly that. Task payloads are back to `st` / `sc` / `sp` / `cpu`, and the metrics task is back to one cadence.

### Fixed
- A task whose stack pointer fails the range check now keeps its last known **Now** value instead of reporting `0`. Two system tasks were charted as using no stack at all when the read failed.

## [0.11.0] - 2026-09-12

### Added
- **`heapwatch_register_sd_volume(label, mount_path)`** — hand the connector an SD card the host app already mounted. A card has no partition-table entry, so it cannot be discovered the way flash volumes are; the host names it once, before `heapwatch_initialize()`, and it then appears in the file browser and the `fs/storage` report exactly like a flash volume (with a null `partition_id`, because no partition backs it). Read-only adoption: the connector queries used/free with `esp_vfs_fat_info()` and never initializes the SPI/SDMMC bus or unmounts the card. Up to 4 cards, in a table separate from the 8 flash volume slots.
- Task telemetry now reports **current** stack usage next to the **peak** (high-water) figure, so a task's stack chart shows what it is using right now instead of only the worst case it ever reached. Peak usage is still reported and is still the number to watch for overflow risk.
- Task telemetry adds a third stack figure, **max**: the deepest stack pointer the connector has sampled for that task. Peak counts only stack a task actually *wrote* to, so a burst that reserves a large call frame without filling it never moves the peak. Max counts reserved stack too, which is what tells you how close the task came to running off the end.
- New menuconfig option **Task Stack Sampling Interval (ms)** (`CONFIG_HEAPWATCH_TASK_SAMPLE_INTERVAL_MS`, default 500, range 100–5000) and matching remotely patchable connector setting `task_smp_ms`. Max only moves when a sample is taken, so this decides how short a burst is still caught; the telemetry interval only decides how often the figures are published. A sample reads three task-control-block fields per task — no stack scan, no allocation — and is clamped to the telemetry interval so the connector never samples slower than it publishes.

### Removed
- **BREAKING:** the `storage.used_bytes` / `storage.free_bytes` parameters and the Kconfig option that produced them (`HEAPWATCH_ENABLE_STORAGE_STATS`, formerly *Report internal-flash filesystem stats (STORAGE1)*). They published one lumped total across every mounted volume, which duplicated the per-volume `fs/storage` report that the dashboard actually reads — and being a single pair, they could never describe a device with several volumes. Storage is now reported only through `fs/storage`. Devices lose storage history and the offline last-known storage value; no dashboard surface used either.
- The SD card no longer has its own code path through the file browser. It is a volume like any other, so path validation, listing, reads, writes, uploads, and free-space checks all flow through `FsVolumeManager` instead of a parallel `/sd` special case.

### Fixed
- **Tasks created with `xTaskCreateStatic()` no longer crash the device.** Stack sizes were read with `heap_caps_get_allocated_size()`, which asserts when handed an address that belongs to no heap — exactly the case for a static task whose stack lives in `.bss`. Stack geometry now comes from the task control block, so static and heap-allocated tasks are measured the same way.
- Reported stack size is now the task's exact configured depth. It previously came from the heap block backing the stack, so it read slightly high by whatever the allocator rounded up.

### Changed
- The *Also Mount And Take-Over SD Card Over SPI* option (renamed from "Also mount and report an SD card over SPI") moved into the **Filesystem Volumes** menu, since it mounts a volume rather than collecting stats. Its help now states plainly that the connector initializes the SPI bus itself, so it must stay off when the host owns the card — register the card instead. The symbol and its GPIO settings are unchanged.
- Menuconfig prompts in the Filesystem Volumes menu now use the file's Title Case convention: *Auto-Mount All Storage Partitions*, *Auto-Format Blank Storage Partitions*.
- Filesystem volumes are mounted from connector bootstrap instead of from the storage-stats module, so the volume lifecycle no longer depends on a telemetry feature being enabled.
- **BREAKING:** `tasks` payloads replace the `su` field with `sc` (current stack bytes), `sm` (max sampled stack bytes) and `sp` (peak stack bytes); `st` (total) is unchanged. Needs Heapwatch backend support for the new fields.
- **BREAKING:** Minimum ESP-IDF is now **5.3.0** (was 5.1.0) — 5.3 is the oldest release Espressif still maintains.

## [0.10.1] - 2026-09-11

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
