#pragma once

#include <stdbool.h>

/**
 * Pure edge-detection for once-per-boot static payload enqueue.
 * Kept header-only so host-side Python tests can mirror the contract.
 */
static inline bool heapwatch_should_enqueue_boot_static(bool prev_mqtt_connected, bool mqtt_connected,
                                                        bool boot_static_enqueued) {
    return !prev_mqtt_connected && mqtt_connected && !boot_static_enqueued;
}

namespace heapwatch::connector_boot_sync {

/** Run deterministic boot one-offs (specs, bounds, network, command catalog request, …). */
void runBootStaticSync();

void resetBootStaticSync();

}  // namespace heapwatch::connector_boot_sync
