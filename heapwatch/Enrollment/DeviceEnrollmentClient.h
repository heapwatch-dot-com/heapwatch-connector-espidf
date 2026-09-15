#pragma once

#include <cstddef>
#include <cstdint>

#include "heapwatch/Enrollment/Credentials/DeviceCredentialsStore.h"

namespace Heapwatch::Enrollment {

enum class Status {
    STATUS_PENDING = 0,
    STATUS_ACCEPTED_AWAITING_PROVISION = 1,
    STATUS_PROVISIONED = 2,
    STATUS_REJECTED = 3,
    STATUS_RE_ENROLL_BLOCKED = 4,
    STATUS_BLOCKED_FOREVER = 5,
    STATUS_KEY_EXPIRED = 6,
    STATUS_ERROR = 7,
};

/** POST /api/device-enrollment/enroll — server mints device_uid. */
bool enroll(const char* api_base, const char* enrollment_key, const char* claimed_mac,
            const char* claimed_uid128, const char* build_identifier, const char* build_environment,
            char* device_uid_out, size_t device_uid_out_len, Status* status_out);

/** GET /api/device-enrollment/status */
Status pollStatus(const char* api_base, const char* enrollment_key, const char* device_uid);

/** POST /api/device-enrollment/provisioning/fetch */
bool fetchProvisioning(const char* api_base, const char* enrollment_key, const char* device_uid,
                       Heapwatch::Enrollment::Credentials::Credentials* out);

/** POST /api/devices/self/provisioning/confirm */
bool confirmProvisioning(const char* api_base, const char* device_api_key);

/** POST /api/devices/self/identities */
bool postClaimedIdentities(const char* api_base, const char* device_api_key,
                           const char* claimed_mac, const char* claimed_uid128,
                           const char* claimed_customer_hardware_id);

bool runUntilProvisioned(const char* api_base, const char* enrollment_key,
                         const char* claimed_mac, const char* claimed_uid128,
                         const char* build_identifier, const char* build_environment,
                         uint32_t poll_interval_ms,
                         Heapwatch::Enrollment::Credentials::Credentials* out);

}  // namespace Heapwatch::Enrollment
