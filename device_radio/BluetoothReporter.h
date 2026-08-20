// Copyright (c) 2026 Heapwatch. All rights reserved.

#ifndef HEAPWATCH_DEVICE_RADIO_BLUETOOTH_REPORTER_H
#define HEAPWATCH_DEVICE_RADIO_BLUETOOTH_REPORTER_H

#include <stddef.h>
#include <stdint.h>

namespace heapwatch::device_radio {

/** Start BLE provider + optional telemetry task (idempotent). */
void start();

/** Emit delta events and enqueue a full `/bluetooth` MQTT publish. */
void publishFullSync();

/** Build `{"ble":{...}}` into out (including braces). Returns false on overflow. */
bool buildPayload(char* out, size_t out_len);

/** Queue a BLE lifecycle event and mirror to ble_events stream when active. */
void emitBleEvent(int event_id, const char* detail, const char* peer_addr);

/**
 * Run bluetooth/scan: ensure TCP, stream peers to ble_scan, publish completion marker.
 * Returns false when BLE is unavailable (caller should nak).
 */
bool startCommandScan(const char* command_id, bool active, uint32_t duration_seconds);

/** Handle bluetooth/get — publishFullSync. */
void handleGetCommand();

bool isBleReportingEnabled();

}  // namespace heapwatch::device_radio

#endif
