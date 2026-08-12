#pragma once

#include <cstddef>
#include <cstdint>

namespace heapwatch::going_dark {

enum class SleepKind : uint8_t {
    Light,
    Deep,
};

/** Start the going-dark pipeline for restart. Returns false if busy. */
bool scheduleRestart(const char* suffix, const char* command_id);

/** Start the going-dark pipeline for sleep_light / sleep_deep. Returns false if busy. */
bool scheduleSleep(const char* suffix, const char* command_id, SleepKind kind, uint32_t duration_s);

/** Start the going-dark pipeline for wifi_reconnect. Returns false if busy. */
bool scheduleWifiReconnect(const char* suffix, const char* command_id, uint32_t duration_s);

/** True while a going-dark pipeline is active (reject other commands). */
bool isActive();

void init();

}  // namespace heapwatch::going_dark
