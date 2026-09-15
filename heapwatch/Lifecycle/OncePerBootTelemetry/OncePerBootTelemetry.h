#pragma once

#include <stdbool.h>

/**
 * Pure edge-detection for once-per-boot static payload enqueue.
 * Kept header-only so host-side Python tests can mirror the contract.
 */
static inline bool heapwatch_should_enqueue_once_per_boot_static_telemetry(bool prev_mqtt_connected, bool mqtt_connected,
                                                        bool boot_static_enqueued) {
    return !prev_mqtt_connected && mqtt_connected && !boot_static_enqueued;
}

namespace Heapwatch::Lifecycle::OncePerBootTelemetry {

/** Run deterministic boot one-offs (specs, bounds, network, command catalog request, …). */
void enqueueOncePerBootStaticTelemetry();

void resetOncePerBootStaticTelemetry();

}  // namespace Heapwatch::Lifecycle::OncePerBootTelemetry
