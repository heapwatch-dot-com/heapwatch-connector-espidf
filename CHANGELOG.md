# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.18.1] - 2026-09-15

### Changed
- Internal style pass (CLAUDE.md): FreeRTOS worker entry points renamed to `task…` functions (`taskDispatchMqttQueue`, `taskForwardEspLogs`, `taskBootstrapConnector`, `taskCollectMetrics`, `taskStreamIngress`, `taskPerformOta`, `taskReportBluetooth`). FreeRTOS task **name strings** stay `hw_*`.
- Internal enum values use `TYPE_SCREAMING` (e.g. `Enrollment::Status::STATUS_PENDING`, `Tls::Server::SERVER_API`, `InboundKind::INBOUND_MQTT_COMMAND`).
- Large connector sources split by concern into hyphenated `.cpp` files (`StreamIngressClient-Worker`, `OtaService-Worker`, `MetricsCollector-Worker`, `ConnectorBootstrap-LogForward`, `DeviceFilesystemOperations-ReadWrite` / `-StreamTransfer`, `FsVolumeManager-Mount` / `-Sd`).
- Removed lowercase `heapwatch/radio/BleDelegate.h` shim; include `heapwatch/Radio/BleDelegate.h` directly. Public `heapwatch.h` is unchanged.

## [0.18.0] - 2026-09-15

### Changed
- Commanded-offline FreeRTOS task is `hw_offline` (was `hw_going_dark`). Host task snapshots and CPU charts show the new name.
- **BREAKING (host sdkconfig):** `CONFIG_HEAPWATCH_OFFLINE_TASK_STACK_BYTES` replaces `CONFIG_HEAPWATCH_GOING_DARK_TASK_STACK_BYTES` (same default 10 KiB). Re-pin in host `sdkconfig` / `sdkconfig.defaults`.
- Internal clear-naming pass: clearer C++ identifiers and PascalCase paths (`MqttTelemetryClient`, `ConnectorMemoryBlock`, `DeviceCommandTask`, `CommandedOffline`, `DeviceFilesystemOperations`, `OncePerBootTelemetry`, `PublicCApi`, `IngressTimestamps`, `FreeRtosAlloc`). Public `heapwatch.h` is unchanged.

## [0.17.1] - 2026-09-15

### Fixed
- Short-lived tasks such as `hw_bootstrap` are remembered again. Task stats are seeded during connector activate (while bootstrap is still running) and kept refreshed while waiting for MQTT, so they still appear on TASKS with `exists: false` after that task exits.

## [0.17.0] - 2026-09-15

### Added
- TASKS MQTT rows include `exists` (boolean). Every FreeRTOS task name seen since connector boot is published each metrics tick: live tasks have `exists: true`, deleted tasks stay in the table with frozen last Now/Min/Max/Peak/CPU and `exists: false`.

### Changed
- When the task-stats table is full (`HEAPWATCH_MAX_TASK_ROWS`), a new live task is logged as an **error** (dropped from tracking) instead of a one-shot warning.

## [0.16.3] - 2026-09-15

### Security
- Stream ingress and Heapwatch REST now use TLS in lab and production. The connector dials with `esp_tls` / HTTPS (SNI `stream.heapwatch.local` / `api.heapwatch.local` in development) instead of plaintext TCP or HTTP.

### Changed
- Post-build firmware upload uses Caddy HTTPS. Lab `HEAPWATCH_API_BASE=https://api.heapwatch.local` trusts the bundled Caddy CA (`certs/heapwatch_local_ca.pem`); production still uses the default public CA store.

## [0.16.2] - 2026-09-15

### Changed
- Wi-Fi and IP `sys_evt` callbacks only record what happened (event-group bits). `esp_wifi_connect()`, MQTT events, LED, and phase updates run on Heapwatch tasks via `handleNotificationsFromSystemEventLoop()`.

## [0.16.1] - 2026-09-14

### Changed
- Parameter slots, OTA job URLs, and FS volume/host-map tables live in the working set (PSRAM when `CONFIG_HEAPWATCH_ALLOCATE_DATA_IN_PSRAM` is on) instead of internal `.bss`. `heapwatch_register_fs_volume` before `heapwatch_initialize()` still works: it allocates the working set early. Log ring, OTA download chunk, and FS sha256/download buffers stay in DRAM (flash-cache / vprintf).
- Log capture ring is **16** slots (was 64). Slot text length is `HEAPWATCH_MAX_LOG_LINE_LEN` (default 160, was a hardcoded 256). Lines are truncated at the ESP log hook, matching MQTT. At defaults this is about 3.3 KiB DRAM instead of 19.7 KiB; a burst larger than 16 lines in ~100 ms can drop (`g_log_lines_dropped_total`).

### Fixed
- Restored `heapwatch/Radio/BleDelegate.h` (0.16.0 left a shim that included itself, so the public BLE delegate C API failed to compile).

## [0.16.0] - 2026-09-14

### Changed
- Internal C++ namespaces are PascalCase under root `Heapwatch`, nested by domain (`Heapwatch::Radio::BluetoothLe`, `Heapwatch::Helpers`, `Heapwatch::DeviceCommand`, …). Source lives under `heapwatch/<Domain>/…` (top-level folder lowercase). Public C API (`heapwatch.h`, `heapwatch_*`) is unchanged. `#include "heapwatch/radio/BleDelegate.h"` still resolves via a shim.

## [0.15.0] - 2026-09-14

### Changed
- Connector integers use fixed-width types (`int32_t`, `uint8_t`, `uint64_t`, …) instead of unsized `int` / `unsigned` / `long`. Public `heapwatch_queue_event`, log-context `core`, and config catalog min/max are `int32_t` (same 32-bit ABI on ESP32). Libc/IDF/NimBLE/POSIX signatures that require `int` (log vprintf hook, sockets, NimBLE callbacks) are unchanged.
- Connector long-lived buffers, MQTT/command/stream queues, the coredump slot, and metrics task-scan scratch are one typed working set (`memory_manager/`), allocated once at init (PSRAM when `CONFIG_HEAPWATCH_ALLOCATE_DATA_IN_PSRAM` is on). Alloc failure still aborts. `heapwatch_deinit()` frees the block only after dispatcher, metrics, command, and stream workers have stopped; otherwise it leaks the block, same as the old queue policy. Metrics no longer `malloc`s a `TaskStatus_t` table every telemetry tick. Menuconfig **Connector JSON/FS scratch arena** (`CONFIG_HEAPWATCH_MEM_SCRATCH_BYTES`, default 8 KiB) reserves a resettable bump buffer in that working set.
- Features-menu Kconfig `n` now **compiles the feature out** (`#if CONFIG_HEAPWATCH_ENABLE_*`): no log ring / `hw_log_forward`, no `hw_ota` or OTA download buffer, no coredump queue/HTTPS upload, no task-stats table, and heap/CPU keys are omitted from the metrics blob rather than sent as zeros. Command catalog omits `ota/*` when OTA is off. Default-on builds stay wire-identical.

## [0.14.1] - 2026-09-14

### Added
- Per-volume filesystem used/total on the metrics interval (`dynamic.fs.<label>.used_bytes` and `_max`), gated by `CONFIG_HEAPWATCH_ENABLE_FS_VOLUME_STATS` (default on). Watch STORAGE follows these over Echo. This is not the retired lumped `storage.used_bytes` pair.
- Kconfig `CONFIG_HEAPWATCH_GOING_DARK_TASK_STACK_BYTES` (default **10 KiB**, was hard-coded 6 KiB). Pin in host `sdkconfig.defaults` on GPS/modem images.

### Fixed
- `hw_going_dark` stack overflow on remoted-node-gps (peak **6109/6141 B**, 99.5% high-water).

## [0.14.0] - 2026-09-14

### Added
- Kconfig **Allocate Heapwatch Connector stacks in PSRAM (Recommended)** (`CONFIG_HEAPWATCH_STACKS_IN_PSRAM`, default off, requires SPIRAM). Flash-safe tasks (`hw_metrics`, `hw_log_forward`, `hw_going_dark`, `hw_enroll_led`, `hw_ble_report`) use PSRAM stacks. `hw_bootstrap`, `hw_command`, `hw_ota`, `hw_streaming`, and `hw_dispatcher` stay in DRAM (`xTaskCreate`, not WithCaps) because they touch NVS/OTA/coredump/FS while SPI cache is off. Implies `SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY`.

### Changed
- `heapwatch_register_fs_volume` and `heapwatch_register_sd_volume` are valid **after** `heapwatch_initialize()`. Host apps can start the connector first so telemetry queues immediately, then register volumes when storage comes up. If the VFS path is not mounted yet, the mapping is kept and adopted on the next `fs/storage`. A connector-owned flash volume cannot be remounted to a different path live.
- `heapwatch_register_config_catalog` after MQTT-up re-uploads the catalog so a late host catalog is not stuck as the first-boot snapshot.
- `hw_bootstrap` DRAM stack is **10 KiB** (was 16 KiB). The task now exits after handoff instead of sitting in a 1-second poll loop.
- `hw_bootstrap` exits after enroll or NVS credentials, MQTT/dispatcher start, and the optional one-shot HTTP that realigns the boot number if NVS was wiped. Credential rotation stays on `hw_command`. Enroll LED after that handoff is driven from IP and MQTT events (not a 1-second poll loop).
- Kconfig **Put as much data as possible in PSRAM (Recommended)** is `CONFIG_HEAPWATCH_ALLOCATE_DATA_IN_PSRAM` (was `CONFIG_HEAPWATCH_ALLOCATE_QUEUES_IN_PSRAM`). Besides MQTT outbound queues it also puts the command inbound queue, stream TCP buffers, and metrics/publish scratch in PSRAM. Allocation failure aborts (no silent DRAM fallback). OTA/FS flash dest buffers stay in DRAM. `idf.py reconfigure` remaps the old symbol.

### Fixed
- File Browser still lists a mounted volume when used/total cannot be read (Arduino SPIFFS with a NULL partition label used to omit the volume entirely). SPIFFS usage also falls back to the unnamed mount if the labelled query fails.
- `HwOta` no longer logs “OTA service ready” when `hw_ota` failed to start. The worker-running line is only printed if the DRAM task was created.
- File-browser upload rename failures now log errno. `EINVAL` (22) is the usual FatFS-without-long-names case (target is not 8.3). Enable `CONFIG_FATFS_LFN_HEAP` on the host.

## [0.13.0] - 2026-09-13

### Fixed
- Post-build firmware upload now takes the partition table CSV that ESP-IDF already resolved (`PARTITION_CSV_PATH`) instead of requiring a file named `partitions.csv` in the project root. Custom names such as `partition_unsecured_16MB.csv` upload correctly.

### Changed
- `hw_dispatcher` stack default is **12 KiB** (`CONFIG_HEAPWATCH_DISPATCHER_TASK_STACK_BYTES`, was a hard-coded 8 KiB). That thread runs claimed-identities HTTPS and streamed coredump upload; 8 KiB overflows the mbedTLS handshake (stack canary on `hw_dispatcher`).
- **BREAKING:** Post-build firmware upload fails the ESP-IDF build when the Heapwatch API call fails. The switch is `HEAPWATCH_FAIL_ON_UPLOAD_ERROR` (default on). Set `HEAPWATCH_FAIL_ON_UPLOAD_ERROR=0` to warn and continue. `HEAPWATCH_UPLOAD_STRICT` is gone.
- **BREAKING:** `TASKS` JSON uses English keys matching the dashboard: `name`, `total`, `now`,
  `min`, `max`, `peak`, `cpu`. The compact `n` / `st` / `sc` / `sp` keys are gone.
- When the task list does not fit one `HEAPWATCH_LARGE_PAYLOAD_SIZE` MQTT slot, it is split
  across multiple `TASKS` publishes. Each message is a complete subset (ingest is per-task).
  Stack-limit PARAMETER fields (`stack_{current,min,max,peak}_bytes_max`) split the same way.

### Added
- Per-task **min** and **max** of **Now** (lowest / highest stack-pointer depth this instance has
  shown on a telemetry tick). First valid sample seeds both; a new handle resets the range. Peak
  (FreeRTOS high-water mark) is unchanged.

## [0.12.1] - 2026-09-12

### Fixed
- `/network` reporter now compiles when the host firmware has lwIP IPv6 disabled. IPv6 address formatting used `ip6_addr_t` even if `CONFIG_LWIP_IPV6` was off, so IPv4-only clients (typical Arduino / cellular products) failed to build.

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
