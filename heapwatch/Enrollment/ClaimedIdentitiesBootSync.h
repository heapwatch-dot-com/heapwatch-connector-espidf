#pragma once

namespace Heapwatch::Enrollment {

/** POST /api/devices/self/identities — stable JSON shape, nulls for unknown fields. */
bool postClaimedIdentities(const char* api_base, const char* device_api_key,
                           const char* claimed_mac, const char* claimed_uid128,
                           const char* claimed_customer_hardware_id);

/**
 * Once-per-boot backfill when network is up. Retries with backoff until HTTP 2xx.
 * No-op when device API key is not stored (factory enrollment not finished).
 */
bool serviceClaimedIdentitiesBootPostIfDue();

/** Optional host trigger; does not block. Dispatcher also calls serviceClaimedIdentitiesBootPostIfDue(). */
void requestClaimedIdentitiesBootPostNow();

void resetClaimedIdentitiesBootSync();

}  // namespace Heapwatch::Enrollment
