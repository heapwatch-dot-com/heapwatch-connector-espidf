#include "SleepTracking.h"

#include <sys/time.h>

#include "esp_attr.h"

namespace {

// RTC memory survives deep sleep (unlike normal .bss/.data) but is cleared on power-on
// reset, so a value left over from a previous boot can never be mistaken for a fresh mark.
RTC_DATA_ATTR int64_t g_deep_sleep_entry_epoch_ms = -1;

int64_t epoch_ms_now(void) {
    struct timeval tv = {};
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000 + (tv.tv_usec / 1000);
}

}  // namespace

extern "C" heapwatch_wake_cause_t heapwatch_translate_current_wake_cause(void) {
    switch (esp_sleep_get_wakeup_cause()) {
        case ESP_SLEEP_WAKEUP_TIMER:
            return HEAPWATCH_WAKE_CAUSE_TIMER;
        case ESP_SLEEP_WAKEUP_EXT0:
            return HEAPWATCH_WAKE_CAUSE_EXT0;
        case ESP_SLEEP_WAKEUP_EXT1:
            return HEAPWATCH_WAKE_CAUSE_EXT1;
        case ESP_SLEEP_WAKEUP_TOUCHPAD:
            return HEAPWATCH_WAKE_CAUSE_TOUCHPAD;
        case ESP_SLEEP_WAKEUP_ULP:
            return HEAPWATCH_WAKE_CAUSE_ULP;
        case ESP_SLEEP_WAKEUP_GPIO:
            return HEAPWATCH_WAKE_CAUSE_GPIO;
        case ESP_SLEEP_WAKEUP_UART:
            return HEAPWATCH_WAKE_CAUSE_UART;
        case ESP_SLEEP_WAKEUP_WIFI:
            return HEAPWATCH_WAKE_CAUSE_WIFI;
        case ESP_SLEEP_WAKEUP_BT:
            return HEAPWATCH_WAKE_CAUSE_BT;
        case ESP_SLEEP_WAKEUP_COCPU:
            return HEAPWATCH_WAKE_CAUSE_COCPU;
        case ESP_SLEEP_WAKEUP_UNDEFINED:
            return HEAPWATCH_WAKE_CAUSE_UNDEFINED;
        default:
            // Covers ESP_SLEEP_WAKEUP_ALL, per-target UART1/UART2/COCPU_TRAP_TRIG variants,
            // and any wakeup source introduced by an ESP-IDF version we don't know about yet.
            return HEAPWATCH_WAKE_CAUSE_OTHER;
    }
}

extern "C" void heapwatch_sleep_tracking_mark_entering_deep_sleep(void) {
    g_deep_sleep_entry_epoch_ms = epoch_ms_now();
}

extern "C" int32_t heapwatch_sleep_tracking_consume_deep_sleep_duration_ms(void) {
    if (g_deep_sleep_entry_epoch_ms < 0) {
        return -1;
    }

    const int64_t entry_ms = g_deep_sleep_entry_epoch_ms;
    g_deep_sleep_entry_epoch_ms = -1;

    const int64_t delta_ms = epoch_ms_now() - entry_ms;
    if (delta_ms < 0) {
        return -1;
    }
    return delta_ms > INT32_MAX ? INT32_MAX : static_cast<int32_t>(delta_ms);
}
