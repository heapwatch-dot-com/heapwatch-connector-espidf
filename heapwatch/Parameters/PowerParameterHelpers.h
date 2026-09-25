// Copyright 2026 Heapwatch
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "heapwatch_config.h"

/**
 * Typed power helpers over MqttTelemetryClient::updateParameter*().
 * Also exposed as heapwatch_update_power_* in heapwatch.h.
 * The client owns ADC/GPIO/fuel-gauge wiring; the connector never reads peripherals here.
 */
namespace Heapwatch::Parameters {

/**
 * Named, multi-source power API. `source_name` is conventionally "battery"/"usb"
 * but any name is valid (e.g. "solar") — it becomes part of
 * `dynamic.power.{source_name}.mv` / `.percent` / `.state`.
 */
void updatePowerMv(const char* source_name, int64_t mv);
void updatePowerMvRanged(const char* source_name, int64_t mv, int64_t min_mv, int64_t max_mv);
void updatePowerPercent(const char* source_name, double percent);
void updatePowerState(const char* source_name, heapwatch_power_state_t state);

}  // namespace Heapwatch::Parameters
