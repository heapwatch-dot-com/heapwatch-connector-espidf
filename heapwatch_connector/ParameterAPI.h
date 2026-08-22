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
// For licensing inquiries, please contact: info@heapwatch.com
//

#pragma once

#include "heapwatch_config.h"

/**
 * POW1 / CELL1 (Phase 5c): typed, "nice" wrappers over the generic
 * HeapwatchConnector::updateParameter*() escape hatch (PARAM1). These are
 * client-facing app-code helpers -- the client owns its own board-specific
 * ADC/GPIO/fuel-gauge/modem wiring and pushes readings in; the connector
 * never reads a peripheral itself here.
 *
 * Client-facing helpers — also exposed via the public C ABI in heapwatch.h
 * (`heapwatch_update_power_*` / `heapwatch_update_cellular_signal`, Phase 5b).
 * The client owns its own board-specific ADC/GPIO/fuel-gauge/modem wiring and
 * pushes readings in; the connector never reads a peripheral itself here.
 */
namespace heapwatch::parameter_api {

/**
 * POW1: named, multi-source power API -- mirrors the already-proven
 * per-FreeRTOS-task auto-provisioning pattern backend-side (see task_plan.md
 * POW1). `source_name` is conventionally "battery"/"usb"/... but any name is
 * valid (e.g. "solar", "backup_battery") -- it becomes part of the emitted
 * slug (`power.{source_name}.mv` / `.percent` / `.state`).
 */
void updatePowerMv(const char* source_name, int64_t mv);
void updatePowerMvRanged(const char* source_name, int64_t mv, int64_t min_mv, int64_t max_mv);
void updatePowerPercent(const char* source_name, double percent);
void updatePowerState(const char* source_name, heapwatch_power_state_t state);

/**
 * CELL1: cellular signal quality, client-push only, fixed slugs (not
 * multi-source like power -- single-modem assumed, see task_plan.md CELL1).
 */
void updateCellularSignal(int rssi_dbm, int ber);

}  // namespace heapwatch::parameter_api
