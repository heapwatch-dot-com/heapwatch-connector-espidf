// Copyright (c) 2026 Viktor Jamrich Enterprises s.r.o. All rights reserved.
//
// This software is part of the Heapwatch firmware project.
// Unauthorized copying, modification, or distribution of this software,
// via any medium, is strictly prohibited unless otherwise permitted.
//
// This software is provided "as is", without warranty of any kind, express or implied,
// including but not limited to the warranties of merchantability, fitness for a particular purpose,
// and noninfringement. In no event shall the authors or copyright holders be liable for any claim,
// damages, or other liability, whether in an action of contract, tort, or otherwise, arising from,
// out of, or in connection with the software or the use or other dealings in the software.
//
// For licensing inquiries, please contact: admin@heapwatch.com
//

#pragma once

#include <stdint.h>

#include "esp_sleep.h"
#include "heapwatch_config.h"

// Reusable sleep-tracking helpers, shared between the real boot-time wake handling in
// main.cpp and the sleep_tracking test harness. Kept framework-portable at the
// heapwatch_wake_cause_t boundary: only this translation function ever touches raw
// esp_sleep_wakeup_cause_t values (see PORT1 in the connector-hardening planning docs).

#ifdef __cplusplus
extern "C" {
#endif

// Translates the current esp_sleep_get_wakeup_cause() value into a Heapwatch-owned,
// framework-agnostic wake cause code safe to send to the backend.
heapwatch_wake_cause_t heapwatch_translate_current_wake_cause(void);

// Call immediately before esp_deep_sleep_start(). Stashes the current epoch time in
// RTC memory (survives deep sleep, lost on power-on reset) so the duration can be
// recovered after reboot.
void heapwatch_sleep_tracking_mark_entering_deep_sleep(void);

// Call once early after boot when esp_reset_reason() == ESP_RST_DEEPSLEEP.
// Returns the elapsed time in milliseconds since heapwatch_sleep_tracking_mark_entering_deep_sleep()
// was called, or -1 if no mark is pending (e.g. the reset was not caused by our own
// tracked deep sleep, or the RTC clock was not running).
// Clears the pending mark so a subsequent call returns -1 until the next deep sleep.
int32_t heapwatch_sleep_tracking_consume_deep_sleep_duration_ms(void);

#ifdef __cplusplus
}
#endif
