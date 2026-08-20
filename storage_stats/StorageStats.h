// Copyright (c) 2026 Heapwatch. All rights reserved.
//
// This software is part of the Heapwatch Connector for ESP-IDF project.
// Unauthorized copying, modification, or distribution of this software,
// via any medium, is strictly prohibited unless otherwise permitted.
//
// This software is provided "as is", without warranty of any kind, express or implied,
// including but not limited to the warranties of merchantability, fitness for a particular purpose,
// and noninfringement. In no event shall the authors or copyright holders be liable for any claim,
// damages, or other liability, whether in an action of contract, tort, or otherwise, arising from,
// out of, or in connection with the software or the use or other dealings in the software.
//
// For licensing inquiries, please contact: admin@heapwatch.com
//

#pragma once

// STORAGE1: internal-flash filesystem stats, deferred from Phase 4 (no mounted
// filesystem existed to measure), now implemented. Mounts the dedicated "storage"
// SPIFFS partition (see partitions.csv) at boot and, if HEAPWATCH_ENABLE_SD_CARD_STATS
// is on, also attempts an SD-over-SPI mount using the Kconfig-configured pins --
// ported from remoted-node-gps's StorageService.cpp/pin-mapping.h (proven SPIFFS.begin()
// + esp_vfs_fat_sdspi_mount() usage), simplified down to just the free/used-space
// query this connector needs (no SQLite storage layer).
//
// Neither mount is fatal on failure: a fresh/unformatted SPIFFS partition is
// auto-formatted on first boot (format_if_mount_failed=true); a missing/unwired SD
// card just logs a warning and is skipped, and its stats are omitted from the
// reported totals for that boot. Reports storage.free_bytes/storage.used_bytes via
// the generic parameter mechanism (PARAM1) -- see PARAM2 for the fixed-slug design.
namespace heapwatch::storage_stats {

/**
 * Mounts the SPIFFS "storage" partition, and (if HEAPWATCH_ENABLE_SD_CARD_STATS=y)
 * attempts to mount an SD card over SPI using the Kconfig-configured pins. Call once,
 * early in app_main -- safe to call even when HEAPWATCH_ENABLE_STORAGE_STATS=n (no-op).
 */
void init();

/**
 * Queries free/used bytes across every successfully-mounted medium (SPIFFS always
 * attempted; SD card only if enabled and its mount succeeded) and reports the
 * aggregate via HeapwatchConnector::updateParameterInt("storage.free_bytes"/
 * "storage.used_bytes", ...). No-op if HEAPWATCH_ENABLE_STORAGE_STATS=n or nothing
 * is mounted. Call once per telemetry round from MetricsCollector.
 */
void collectAndReport();

}  // namespace heapwatch::storage_stats
