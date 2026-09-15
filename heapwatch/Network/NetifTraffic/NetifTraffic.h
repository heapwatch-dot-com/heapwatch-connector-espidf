// Copyright (c) 2026 Heapwatch. All rights reserved.

#ifndef HEAPWATCH_NETIF_TRAFFIC_H
#define HEAPWATCH_NETIF_TRAFFIC_H

#include <stddef.h>

namespace Heapwatch::Network::NetifTraffic {

/** Register IP_EVENT_TX_RX (idempotent). No-op if traffic reporting is disabled. */
void start();

/** Bind known if_keys and default netif; install lwIP input/linkoutput byte counters. */
void installNetifRxTxHooksIfMissing();

/**
 * Append `dynamic.net.{seg}.rx_bytes` / `.tx_bytes` fields for every tracked netif.
 * JSON: leading commas + `"slug":value`. Flat: leading `|slug=value`.
 * Returns bytes written (not including a trailing NUL).
 */
size_t appendParameterFields(char* out, size_t out_len, bool json);

/** Append `"rx_bytes":N,"tx_bytes":N` for one if_key (including leading comma). */
bool appendRxTxJsonFields(char* out, size_t out_len, size_t* offset, const char* if_key);

}  // namespace Heapwatch::Network::NetifTraffic

#endif
