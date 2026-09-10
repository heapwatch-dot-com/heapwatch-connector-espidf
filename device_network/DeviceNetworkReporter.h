// Copyright (c) 2026 Heapwatch. All rights reserved.

#ifndef HEAPWATCH_DEVICE_NETWORK_REPORTER_H
#define HEAPWATCH_DEVICE_NETWORK_REPORTER_H

#include <stddef.h>

namespace heapwatch::device_network {

/** Start listening for IP/WiFi netif lifecycle events (idempotent). */
void start();

void stop();

/**
 * Walk all esp_netifs, emit NETIF_* / DEFAULT_ROUTE_CHANGED events as needed,
 * and enqueue a full `/network` MQTT publish.
 */
void publishFullSync();

/** Mark that netif topology changed; collector publishes when MQTT is up. */
void markTopologyDirty();

/** Returns true once per mark — clears the flag. */
bool consumeTopologyDirty();

/**
 * Build `{"netifs":[...]}` into out (including braces). Returns false on overflow.
 */
bool buildPayload(char* out, size_t out_len);

}  // namespace heapwatch::device_network

#endif
