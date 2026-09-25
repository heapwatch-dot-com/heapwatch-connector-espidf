// Copyright 2026 Heapwatch
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * PARAM1 (Phase 5c): the real generic parameter mechanism.
 *
 * An in-RAM sparse map, `slug -> {value, min?, max?}`, fixed capacity
 * (HEAPWATCH_MAX_PARAMETER_SLOTS), no malloc/free. `update*()` calls from
 * anywhere in the connector or client app code (metrics collector, POW1
 * power API, CELL1 cellular API, ...) just update the current in-place value
 * for a slug. `buildFlushPayload()` is called once per telemetry round by
 * MqttTelemetryClient::flushParameters() and serializes the *entire* current
 * map as one flat batched JSON object -- this is what the backend's
 * ProcessParameter job actually expects on the wire (one publish = one
 * `{slug: value, ...}` object, not one message per value), and it delivers
 * "coalesced, last-value-wins" for free.
 *
 * PARAM1b: `_min`/`_max` bounds are dirty-flag-gated — only re-included when
 * `SLOT_BOUNDS_DIRTY` is set. Dirty clears only after successful outbound queue accept.
 * Live values are included every flush that has at least one valued slug.
 * Limits-only slots (max/min set with no live value) are valid: a flush may
 * contain only `slug_max` / `slug_min` keys.
 */
namespace Heapwatch::Parameters::Store {

constexpr size_t MAX_SLUG_LEN = 48;

/** In-RAM parameter row. Lives in the memory block, not .bss. */
struct Slot {
    char slug[MAX_SLUG_LEN] = {0};
    int64_t int_value = 0;
    double double_value = 0.0;
    int64_t int_min = 0;
    int64_t int_max = 0;
    double double_min = 0.0;
    double double_max = 0.0;
    uint32_t last_bounds_sent_tick = 0;
    uint8_t flags = 0;
};

/** Drop the working-set pointer after MemoryManager::deinit(). */
void unbindMemoryBlock();

/** Sets/overwrites a slug's live integer value. No bounds. */
void updateInt(const char* slug, int64_t value);

/** Sets/overwrites a slug's live double value. No bounds. */
void updateDouble(const char* slug, double value);

/** Sets/overwrites a slug's live integer value plus its (typically boot-constant) bounds. */
void updateIntRanged(const char* slug, int64_t value, int64_t min_value, int64_t max_value);

/** Sets/overwrites a slug's live double value plus its (typically boot-constant) bounds. */
void updateDoubleRanged(const char* slug, double value, double min_value, double max_value);

/**
 * Sets a slug's max (or min) without requiring a live value. Used for connector
 * gauge limits (boot + change) and app custom-metric caps. Marks bounds dirty
 * when the cap changes so the next flush is a valid limits-only `/parameter`
 * message if no value has been written yet.
 */
void setIntMax(const char* slug, int64_t max_value);
void setIntMin(const char* slug, int64_t min_value);
void setDoubleMax(const char* slug, double max_value);
void setDoubleMin(const char* slug, double min_value);

/**
 * Serializes the current map into `out` as one flat JSON object
 * (`{"tsms":...,"utms":...,"boot_cycle":...,"slug":value,...}` plus dirty `_min`/`_max` keys). Returns
 * false (and leaves `out` untouched) if there is nothing to publish.
 */
bool buildFlushPayload(char* out, size_t out_size);

/** Clears bounds dirty flags after the payload built above was accepted by the outbound queue. */
void acknowledgeBoundsSent();

}  // namespace Heapwatch::Parameters::Store
