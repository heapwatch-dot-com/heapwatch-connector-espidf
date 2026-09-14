#include "heapwatch/Enrollment/ClaimedIdentitiesBootSync.h"

#include "heapwatch/Enrollment/Credentials/DeviceCredentialsStore.h"
#include "heapwatch/Enrollment/DeviceEnrollmentClient.h"
#include "heapwatch/Enrollment/Identity/DeviceIdentity.h"
#include "freertos/FreeRTOS.h"
#include "heapwatch_config.h"
#include "heapwatch_endpoints.h"

namespace Heapwatch::Enrollment {

namespace {

bool g_claimed_identities_post_done = false;
TickType_t g_claimed_identities_next_attempt_tick = 0;

enum class BootPostResult {
    kSuccess,
    kSkipped,
    kRetryableFailure,
};

BootPostResult tryPostFromStoredCredentials() {
    Heapwatch::Enrollment::Credentials::Credentials stored = {};
    if (!Heapwatch::Enrollment::Credentials::load(&stored) || stored.device_api_key[0] == '\0') {
        return BootPostResult::kSkipped;
    }

    char claimed_mac[32] = {};
    char claimed_uid128[33] = {};
    Heapwatch::Enrollment::Identity::getClaimedMac(claimed_mac, sizeof(claimed_mac));
    Heapwatch::Enrollment::Identity::getClaimedUid128(claimed_uid128, sizeof(claimed_uid128));

    return postClaimedIdentities(HEAPWATCH_API_BASE_URL, stored.device_api_key, claimed_mac,
                                 claimed_uid128, nullptr)
               ? BootPostResult::kSuccess
               : BootPostResult::kRetryableFailure;
}

}  // namespace

bool serviceClaimedIdentitiesBootPostIfDue() {
    if (g_claimed_identities_post_done) {
        return true;
    }

    const TickType_t now = xTaskGetTickCount();
    if (g_claimed_identities_next_attempt_tick != 0 &&
        now < g_claimed_identities_next_attempt_tick) {
        return false;
    }

    const BootPostResult result = tryPostFromStoredCredentials();
    if (result == BootPostResult::kSuccess) {
        g_claimed_identities_post_done = true;
        return true;
    }
    if (result == BootPostResult::kSkipped) {
        return false;
    }

    g_claimed_identities_next_attempt_tick =
        now + pdMS_TO_TICKS(HEAPWATCH_IDENTITIES_RETRY_INTERVAL_MS);
    return false;
}

void requestClaimedIdentitiesBootPostNow() {
    g_claimed_identities_next_attempt_tick = 0;
    (void)serviceClaimedIdentitiesBootPostIfDue();
}

void resetClaimedIdentitiesBootSync() {
    g_claimed_identities_post_done = false;
    g_claimed_identities_next_attempt_tick = 0;
}

}  // namespace Heapwatch::Enrollment
