#pragma once

#include <cstdint>

#include "sdkconfig.h"

namespace Heapwatch::Enrollment {

enum class Status;

enum class EnrollStateLedPhase : std::uint8_t {
    ENROLL_LED_OFF = 0,
    ENROLL_LED_WIFI_CONNECTING,
    ENROLL_LED_WIFI_FAILED,
    ENROLL_LED_NETWORK_ERROR,
    ENROLL_LED_ENROLLING,
    ENROLL_LED_PENDING,
    ENROLL_LED_ACCEPTED_AWAITING_PROVISION,
    ENROLL_LED_PROVISIONED,
    ENROLL_LED_REJECTED,
    ENROLL_LED_RE_ENROLL_BLOCKED,
    ENROLL_LED_BLOCKED_FOREVER,
    ENROLL_LED_KEY_EXPIRED,
};

#if CONFIG_HEAPWATCH_ENROLL_STATE_LED_INDICATOR

void startIndicator();

void stopIndicator();

void setIndicatorPhase(EnrollStateLedPhase phase);

EnrollStateLedPhase indicatorPhaseFromEnrollmentStatus(Status status);

#else

inline void startIndicator() {}

inline void stopIndicator() {}

inline void setIndicatorPhase(EnrollStateLedPhase /*phase*/) {}

inline EnrollStateLedPhase indicatorPhaseFromEnrollmentStatus(Status /*status*/) {
    return EnrollStateLedPhase::ENROLL_LED_OFF;
}

#endif

}  // namespace Heapwatch::Enrollment
