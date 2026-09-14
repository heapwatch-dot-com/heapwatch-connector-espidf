#include "heapwatch/Enrollment/EnrollStateLedIndicator.h"
#include <cinttypes>

#include "heapwatch/Enrollment/DeviceEnrollmentClient.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "heapwatch_config.h"
#include "heapwatch/Helpers/heapwatch_freertos_alloc.h"

namespace Heapwatch::Enrollment {

#if CONFIG_HEAPWATCH_ENROLL_STATE_LED_INDICATOR

namespace {

constexpr char TAG[] = "HwEnroll";

volatile EnrollStateLedPhase g_phase = EnrollStateLedPhase::kWifiConnecting;

bool g_started = false;
volatile bool g_stop_requested = false;
volatile bool g_task_exited = true;

void driveLed(bool logical_on) {
#if defined(CONFIG_HEAPWATCH_ENROLL_STATE_LED_ACTIVE_LOW) && CONFIG_HEAPWATCH_ENROLL_STATE_LED_ACTIVE_LOW
    const bool active_low = true;
#else
    const bool active_low = false;
#endif
    const uint32_t level = (logical_on != active_low) ? 1u : 0u;
    gpio_set_level(static_cast<gpio_num_t>(CONFIG_HEAPWATCH_ENROLL_STATE_LED_GPIO), level);
}

// Blink patterns need their waits to be exact, so only ones longer than the stop
// slice get sliced — slicing a 50 ms pulse would visibly skew it.
void sleepMs(uint32_t ms) {
    if (ms <= HEAPWATCH_STOP_CHECK_SLICE_MS) {
        vTaskDelay(pdMS_TO_TICKS(ms));
        return;
    }
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    while (!g_stop_requested && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(HEAPWATCH_STOP_CHECK_SLICE_MS));
    }
}

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
        for (int32_t i = 0; i < 2; ++i) {
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
        for (int32_t i = 0; i < 3; ++i) {
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
        for (int32_t i = 0; i < 3; ++i) {
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
        for (int32_t i = 0; i < 3; ++i) {
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
        for (int32_t i = 0; i < 3; ++i) {
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
    while (!g_stop_requested) {
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
    driveLed(false);
    g_task_exited = true;
    heapwatch_task_delete(nullptr);
}

}  // namespace

void startIndicator() {
    if (g_started) {
        return;
    }

    if (CONFIG_HEAPWATCH_ENROLL_STATE_LED_GPIO < 0) {
        ESP_LOGW(TAG, "Enroll state LED enabled but GPIO is unset");
        return;
    }

    gpio_reset_pin(static_cast<gpio_num_t>(CONFIG_HEAPWATCH_ENROLL_STATE_LED_GPIO));
    gpio_set_direction(static_cast<gpio_num_t>(CONFIG_HEAPWATCH_ENROLL_STATE_LED_GPIO),
                       GPIO_MODE_OUTPUT);
    driveLed(false);

    g_stop_requested = false;
    g_task_exited = false;
    if (heapwatch_task_create(indicatorTaskEntry, "hw_enroll_led", 2048, nullptr, tskIDLE_PRIORITY + 1,
                              nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start enroll state LED task");
        g_task_exited = true;
        return;
    }

    g_started = true;
    ESP_LOGI(TAG, "Enroll state LED on GPIO %" PRId32 " (active_low=%" PRId32 ")",
             static_cast<int32_t>(CONFIG_HEAPWATCH_ENROLL_STATE_LED_GPIO),
#if defined(CONFIG_HEAPWATCH_ENROLL_STATE_LED_ACTIVE_LOW) && CONFIG_HEAPWATCH_ENROLL_STATE_LED_ACTIVE_LOW
             static_cast<int32_t>(1));
#else
             static_cast<int32_t>(0));
#endif
}

void stopIndicator() {
    if (!g_started) {
        return;
    }
    g_stop_requested = true;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
    while (!g_task_exited && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    driveLed(false);
    g_started = false;
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

#endif

}  // namespace Heapwatch::Enrollment
