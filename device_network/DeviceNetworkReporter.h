// Copyright (c) 2026 Viktor Jamrich Enterprises s.r.o. All rights reserved.

#ifndef HEAPWATCH_DEVICE_NETWORK_REPORTER_H
#define HEAPWATCH_DEVICE_NETWORK_REPORTER_H

#include <stddef.h>

namespace heapwatch::device_network {

/** Start listening for IP/WiFi netif lifecycle events (idempotent). */
void start();

/**
 * Walk all esp_netifs, emit NETIF_* / DEFAULT_ROUTE_CHANGED events as needed,
 * and enqueue a full `/network` MQTT publish.
 */
void publishFullSync();

/**
 * Build `{"netifs":[...]}` into out (including braces). Returns false on overflow.
 */
bool buildPayload(char* out, size_t out_len);

}  // namespace heapwatch::device_network

#endif
