#include "EnrollStateLedIndicator.h"

#include "DeviceEnrollmentClient.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace heapwatch::device_enrollment {

#if CONFIG_HEAPWATCH_ENROLL_STATE_LED_INDICATOR

namespace {

constexpr char kTag[] = "EnrollStateLed";

volatile EnrollStateLedPhase g_phase = EnrollStateLedPhase::kWifiConnecting;

bool g_started = false;

void driveLed(bool logical_on) {
    const bool active_low = CONFIG_HEAPWATCH_ENROLL_STATE_LED_ACTIVE_LOW;
    const int level = (logical_on != active_low) ? 1 : 0;
    gpio_set_level(static_cast<gpio_num_t>(CONFIG_HEAPWATCH_ENROLL_STATE_LED_GPIO), level);
}

void sleepMs(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

bool samplePhaseMatches(EnrollStateLedPhase expected) {
    return g_phase == expected;
}

void blinkEqualOnOff(uint32_t on_ms, uint32_t off_ms, EnrollStateLedPhase phase) {
    while (samplePhaseMatches(phase)) {
        driveLed(true);
        sleepMs(on_ms);
        if (!samplePhaseMatches(phase)) {
            break;
        }
        driveLed(false);
        sleepMs(off_ms);
    }
}

void runDoubleBlink(EnrollStateLedPhase phase) {
    while (samplePhaseMatches(phase)) {
        for (int i = 0; i < 2; ++i) {
            driveLed(true);
            sleepMs(150);
            if (!samplePhaseMatches(phase)) {
                return;
            }
            driveLed(false);
            sleepMs(150);
            if (!samplePhaseMatches(phase)) {
                return;
            }
        }
        driveLed(false);
        sleepMs(800);
    }
}

void runTripleBlink(EnrollStateLedPhase phase) {
    while (samplePhaseMatches(phase)) {
        for (int i = 0; i < 3; ++i) {
            driveLed(true);
            sleepMs(120);
            if (!samplePhaseMatches(phase)) {
                return;
            }
            driveLed(false);
            sleepMs(120);
            if (!samplePhaseMatches(phase)) {
                return;
            }
        }
        driveLed(false);
        sleepMs(700);
    }
}

void runRejectedPattern(EnrollStateLedPhase phase) {
    while (samplePhaseMatches(phase)) {
        driveLed(false);
        sleepMs(1200);
        for (int i = 0; i < 3; ++i) {
            if (!samplePhaseMatches(phase)) {
                return;
            }
            driveLed(true);
            sleepMs(120);
            driveLed(false);
            sleepMs(120);
        }
    }
}

void runSosPattern(EnrollStateLedPhase phase) {
    while (samplePhaseMatches(phase)) {
        for (int i = 0; i < 3; ++i) {
            driveLed(true);
            sleepMs(120);
            driveLed(false);
            sleepMs(120);
        }
        sleepMs(200);
        driveLed(true);
        sleepMs(400);
        driveLed(false);
        sleepMs(200);
        for (int i = 0; i < 3; ++i) {
            driveLed(true);
            sleepMs(120);
            driveLed(false);
            sleepMs(120);
        }
        sleepMs(900);
    }
}

void runKeyExpiredPattern(EnrollStateLedPhase phase) {
    while (samplePhaseMatches(phase)) {
        driveLed(true);
        sleepMs(200);
        driveLed(false);
        sleepMs(2800);
    }
}

void indicatorTaskEntry(void* /*param*/) {
    while (true) {
        const EnrollStateLedPhase phase = g_phase;

        switch (phase) {
            case EnrollStateLedPhase::kOff:
                driveLed(false);
                sleepMs(200);
                break;
            case EnrollStateLedPhase::kWifiConnecting:
                blinkEqualOnOff(500, 500, phase);
                break;
            case EnrollStateLedPhase::kWifiFailed:
            case EnrollStateLedPhase::kNetworkError:
                blinkEqualOnOff(100, 100, phase);
                break;
            case EnrollStateLedPhase::kEnrolling:
                blinkEqualOnOff(250, 250, phase);
                break;
            case EnrollStateLedPhase::kPending:
                runDoubleBlink(phase);
                break;
            case EnrollStateLedPhase::kAcceptedAwaitingProvision:
                runTripleBlink(phase);
                break;
            case EnrollStateLedPhase::kProvisioned:
                driveLed(true);
                sleepMs(500);
                break;
            case EnrollStateLedPhase::kRejected:
                runRejectedPattern(phase);
                break;
            case EnrollStateLedPhase::kReEnrollBlocked:
                runSosPattern(phase);
                break;
            case EnrollStateLedPhase::kBlockedForever:
                blinkEqualOnOff(50, 50, phase);
                break;
            case EnrollStateLedPhase::kKeyExpired:
                runKeyExpiredPattern(phase);
                break;
        }
    }
}

}  // namespace

void startIndicator() {
    if (g_started) {
        return;
    }

    if (CONFIG_HEAPWATCH_ENROLL_STATE_LED_GPIO < 0) {
        ESP_LOGW(kTag, "Enroll state LED enabled but GPIO is unset");
        return;
    }

    gpio_reset_pin(static_cast<gpio_num_t>(CONFIG_HEAPWATCH_ENROLL_STATE_LED_GPIO));
    gpio_set_direction(static_cast<gpio_num_t>(CONFIG_HEAPWATCH_ENROLL_STATE_LED_GPIO),
                       GPIO_MODE_OUTPUT);
    driveLed(false);

    if (xTaskCreate(indicatorTaskEntry, "hw_enroll_led", 2048, nullptr, tskIDLE_PRIORITY + 1,
                    nullptr) != pdPASS) {
        ESP_LOGE(kTag, "Failed to start enroll state LED task");
        return;
    }

    g_started = true;
    ESP_LOGI(kTag, "Enroll state LED on GPIO %d (active_low=%d)",
             CONFIG_HEAPWATCH_ENROLL_STATE_LED_GPIO,
             CONFIG_HEAPWATCH_ENROLL_STATE_LED_ACTIVE_LOW ? 1 : 0);
}

void setIndicatorPhase(EnrollStateLedPhase phase) { g_phase = phase; }

EnrollStateLedPhase indicatorPhaseFromEnrollmentStatus(Status status) {
    switch (status) {
        case Status::kPending:
            return EnrollStateLedPhase::kPending;
        case Status::kAcceptedAwaitingProvision:
            return EnrollStateLedPhase::kAcceptedAwaitingProvision;
        case Status::kProvisioned:
            return EnrollStateLedPhase::kProvisioned;
        case Status::kRejected:
            return EnrollStateLedPhase::kRejected;
        case Status::kReEnrollBlocked:
            return EnrollStateLedPhase::kReEnrollBlocked;
        case Status::kBlockedForever:
            return EnrollStateLedPhase::kBlockedForever;
        case Status::kKeyExpired:
            return EnrollStateLedPhase::kKeyExpired;
        case Status::kError:
            return EnrollStateLedPhase::kNetworkError;
    }

    return EnrollStateLedPhase::kNetworkError;
}

#else

void startIndicator() {}

void setIndicatorPhase(EnrollStateLedPhase /*phase*/) {}

EnrollStateLedPhase indicatorPhaseFromEnrollmentStatus(Status /*status*/) {
    return EnrollStateLedPhase::kOff;
}

#endif

}  // namespace heapwatch::device_enrollment
