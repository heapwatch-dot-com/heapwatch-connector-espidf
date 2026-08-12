#pragma once

#include <cstddef>
#include <cstdint>

#include "device_credentials/DeviceCredentialsStore.h"

namespace heapwatch::device_enrollment {

enum class Status {
    kPending = 0,
    kAcceptedAwaitingProvision = 1,
    kProvisioned = 2,
    kRejected = 3,
    kReEnrollBlocked = 4,
    kBlockedForever = 5,
    kKeyExpired = 6,
    kError = 7,
};

/** POST /api/device-enrollment/enroll — server mints device_uid. */
bool enroll(const char* api_base, const char* enrollment_key, const char* claimed_mac,
            const char* build_identifier, const char* build_environment, char* device_uid_out,
            size_t device_uid_out_len, Status* status_out);

/** GET /api/device-enrollment/status */
Status pollStatus(const char* api_base, const char* enrollment_key, const char* device_uid);

/** POST /api/device-enrollment/provisioning/fetch */
bool fetchProvisioning(const char* api_base, const char* enrollment_key, const char* device_uid,
                       heapwatch::device_credentials::Credentials* out);

/** POST /api/devices/self/provisioning/confirm */
bool confirmProvisioning(const char* api_base, const char* device_api_key);

bool runUntilProvisioned(const char* api_base, const char* enrollment_key,
                         const char* claimed_mac, const char* build_identifier,
                         const char* build_environment, uint32_t poll_interval_ms,
                         heapwatch::device_credentials::Credentials* out);

}  // namespace heapwatch::device_enrollment
