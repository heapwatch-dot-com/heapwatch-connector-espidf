#pragma once

#include <cstdint>

#include "sdkconfig.h"

namespace heapwatch::device_enrollment {

enum class Status;

enum class EnrollStateLedPhase : std::uint8_t {
    kOff = 0,
    kWifiConnecting,
    kWifiFailed,
    kNetworkError,
    kEnrolling,
    kPending,
    kAcceptedAwaitingProvision,
    kProvisioned,
    kRejected,
    kReEnrollBlocked,
    kBlockedForever,
    kKeyExpired,
};

#if CONFIG_HEAPWATCH_ENROLL_STATE_LED_INDICATOR

void startIndicator();

void setIndicatorPhase(EnrollStateLedPhase phase);

EnrollStateLedPhase indicatorPhaseFromEnrollmentStatus(Status status);

#else

inline void startIndicator() {}

inline void setIndicatorPhase(EnrollStateLedPhase /*phase*/) {}

inline EnrollStateLedPhase indicatorPhaseFromEnrollmentStatus(Status /*status*/) {
    return EnrollStateLedPhase::kOff;
}

#endif

}  // namespace heapwatch::device_enrollment
