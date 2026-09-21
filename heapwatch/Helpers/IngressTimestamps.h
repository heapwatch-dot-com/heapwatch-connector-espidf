#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Wall clocks below this (2025-01-01T00:00:00Z) are the pre-SNTP/GNSS 1970 clock, not a real
 * time. Same bar the backend uses, so both sides agree on what "device had no clock" means.
 */
#define HEAPWATCH_MIN_TRUSTED_WALL_CLOCK_MS 1735689600000ULL

/** Buffer size for heapwatch_format_wall_clock_or_null() output ("null" or epoch-ms digits). */
#define HEAPWATCH_WALL_CLOCK_VALUE_BUFFER_SIZE 24

void heapwatch_get_wall_clock_and_uptime_ms(uint64_t* wall_clock_ms_out, uint64_t* uptime_ms_out);

/** True once the wall clock is a real date (time synced), false while it is still the 1970 clock. */
bool heapwatch_is_wall_clock_trusted(void);

/**
 * Renders the `tsms` value for a payload: the epoch-ms digits when the clock is trusted,
 * otherwise the literal `null`. The key is always emitted — never omitted — so the backend can
 * tell "no clock yet" from "field missing".
 */
void heapwatch_format_wall_clock_or_null(uint64_t wall_clock_ms, char* out, size_t out_size);

/**
 * Rewrites a queued payload's null `tsms` into a real timestamp derived from its own `utms` and
 * the current clock, so data buffered before the first time sync still reaches the broker with a
 * wall clock. In-place; returns false when the clock is still untrusted, the payload has no null
 * `tsms`/`utms` pair, or the rewritten payload would not fit in payload_capacity.
 */
bool heapwatch_restamp_payload_wall_clock(char* payload, size_t payload_capacity);

#ifdef __cplusplus
}
#endif
