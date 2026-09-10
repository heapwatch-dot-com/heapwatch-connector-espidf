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
 * Typed power helpers over HeapwatchConnector::updateParameter*().
 * Also exposed as heapwatch_update_power_* in heapwatch.h.
 * The client owns ADC/GPIO/fuel-gauge wiring; the connector never reads peripherals here.
 */
namespace heapwatch::parameter_api {

/**
 * Named, multi-source power API. `source_name` is conventionally "battery"/"usb"
 * but any name is valid (e.g. "solar") — it becomes part of
 * `dynamic.power.{source_name}.mv` / `.percent` / `.state`.
 */
void updatePowerMv(const char* source_name, int64_t mv);
void updatePowerMvRanged(const char* source_name, int64_t mv, int64_t min_mv, int64_t max_mv);
void updatePowerPercent(const char* source_name, double percent);
void updatePowerState(const char* source_name, heapwatch_power_state_t state);

}  // namespace heapwatch::parameter_api
