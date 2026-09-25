// Copyright 2026 Heapwatch
// SPDX-License-Identifier: Apache-2.0

#ifndef HEAPWATCH_DEVICE_RADIO_BLUETOOTH_REPORTER_H
#define HEAPWATCH_DEVICE_RADIO_BLUETOOTH_REPORTER_H

#include <stddef.h>
#include <stdint.h>

namespace Heapwatch::Radio::BluetoothLe {

/** Start BLE provider + optional telemetry task (idempotent). */
void start();

void stop();

/** Emit delta events and enqueue a full `/bluetooth` MQTT publish. */
void publishFullSync();

/** Build `{"ble":{...}}` into out (including braces). Returns false on overflow. */
bool buildPayload(char* out, size_t out_len);

/** Queue a BLE lifecycle event and mirror to ble_events stream when active. */
void emitBleEvent(int32_t event_id, const char* detail, const char* peer_addr);

/**
 * Run bluetooth/scan: ensure TCP, stream peers to ble_scan, publish completion marker.
 * Returns false when BLE is unavailable (caller should nak).
 */
bool startCommandScan(const char* command_id, bool active, uint32_t duration_seconds);

/** Handle bluetooth/get — publishFullSync. */
void handleGetCommand();

bool isBleReportingEnabled();

}  // namespace Heapwatch::Radio::BluetoothLe

#endif
