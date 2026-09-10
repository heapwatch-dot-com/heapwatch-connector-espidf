# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Per-netif monotonic RX/TX byte counters (`dynamic.net.{seg}.rx_bytes` / `.tx_bytes` on `/parameter`, `rx_bytes` / `tx_bytes` on `/network`), sampled from LWIP MIB2 octet counters.

### Fixed
- Disabled netif RX/TX sampling and removed the `MIB2_STATS=1` lwip compile override after a LoadProhibited crash in `esp_netif_next_unsafe` (~7s boot loop).
