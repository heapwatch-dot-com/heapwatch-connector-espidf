#pragma once

#include <cstddef>
#include <cstdint>

namespace Heapwatch::DeviceCommand::CommandedOffline {

enum class SleepKind : uint8_t {
    SLEEP_KIND_LIGHT,
    SLEEP_KIND_DEEP,
};

/** Start the commanded-offline pipeline for restart. Returns false if busy. */
bool scheduleRestart(const char* suffix, const char* command_id);

/** Start the commanded-offline pipeline for shutdown (indefinite deep sleep). Returns false if busy. */
bool scheduleShutdown(const char* suffix, const char* command_id);

/** Start the commanded-offline pipeline for sleep_light / sleep_deep. Returns false if busy. */
bool scheduleSleep(const char* suffix, const char* command_id, SleepKind kind, uint32_t duration_s);

/** Start the commanded-offline pipeline for wifi_reconnect. Returns false if busy. */
bool scheduleWifiReconnect(const char* suffix, const char* command_id, uint32_t duration_s);

/** True while a commanded-offline pipeline is active (reject other commands). */
bool isActive();

void init();

void shutdown();

}  // namespace Heapwatch::DeviceCommand::CommandedOffline
