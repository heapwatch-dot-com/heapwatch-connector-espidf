#include "ClaimedIdentitiesBootSync.h"

#include "device_credentials/DeviceCredentialsStore.h"
#include "device_enrollment/DeviceEnrollmentClient.h"
#include "device_identity/DeviceIdentity.h"
#include "freertos/FreeRTOS.h"
#include "heapwatch_config.h"

namespace heapwatch::device_enrollment {

namespace {

bool g_claimed_identities_post_done = false;
TickType_t g_claimed_identities_next_attempt_tick = 0;

enum class BootPostResult {
    kSuccess,
    kSkipped,
    kRetryableFailure,
};

BootPostResult tryPostFromStoredCredentials() {
    heapwatch::device_credentials::Credentials stored = {};
    if (!heapwatch::device_credentials::load(&stored) || stored.device_api_key[0] == '\0') {
        return BootPostResult::kSkipped;
    }

    char claimed_mac[32] = {};
    char claimed_uid128[33] = {};
    heapwatch::device_identity::getClaimedMac(claimed_mac, sizeof(claimed_mac));
    heapwatch::device_identity::getClaimedUid128(claimed_uid128, sizeof(claimed_uid128));

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

}  // namespace heapwatch::device_enrollment
