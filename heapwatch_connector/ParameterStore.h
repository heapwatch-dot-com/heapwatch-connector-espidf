// Copyright (c) 2026 Viktor Jamrich Enterprises s.r.o. All rights reserved.
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
// For licensing inquiries, please contact: admin@heapwatch.com
//

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
 * HeapwatchConnector::flushParameters() and serializes the *entire* current
 * map as one flat batched JSON object -- this is what the backend's
 * ProcessParameter job actually expects on the wire (one publish = one
 * `{slug: value, ...}` object, not one message per value), and it delivers
 * "coalesced, last-value-wins" for free.
 *
 * PARAM1b: `_min`/`_max` bounds are dirty-flag-gated -- only re-included in a
 * flush when they differ from what was last actually sent, or when the rare
 * safety-net resend interval has elapsed (see HEAPWATCH_PARAMETER_BOUNDS_RESEND_INTERVAL_MS).
 * Live values are included every flush that has at least one valued slug.
 * Limits-only slots (max/min set with no live value) are valid: a flush may
 * contain only `slug_max` / `slug_min` keys.
 */
namespace heapwatch::parameter_store {

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
 * (`{"tsms":...,"utms":...,"slug":value,...}` plus due `_min`/`_max` keys),
 * clearing bound dirty-flags for any bounds included in this flush. Returns
 * false (and leaves `out` untouched) if there is nothing to publish — empty
 * map, or only limits-only slots whose bounds are not due.
 */
bool buildFlushPayload(char* out, size_t out_size);

}  // namespace heapwatch::parameter_store
