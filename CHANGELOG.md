# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
