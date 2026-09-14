#pragma once

#include <cstddef>
#include <cstdint>

namespace Heapwatch::DeviceCommand {

void handleSleepLight(const char* suffix, char* command_id, const char* payload, size_t payload_len);

void handleSleepDeep(const char* suffix, char* command_id, const char* payload, size_t payload_len);

}  // namespace Heapwatch::DeviceCommand
