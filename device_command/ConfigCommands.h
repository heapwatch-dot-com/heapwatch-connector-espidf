#pragma once

#include <cstddef>

namespace heapwatch::device_command {

void handleConfigGet(const char* suffix, char* command_id, const char* payload, size_t payload_len);

void handleConfigSet(const char* suffix, char* command_id, const char* payload, size_t payload_len);

void handleConfigPersist(const char* suffix, char* command_id, const char* payload, size_t payload_len);

void handleConfigRevert(const char* suffix, char* command_id, const char* payload, size_t payload_len);

void handleWifiReconnect(const char* suffix, char* command_id, const char* payload,
                         size_t payload_len);

}  // namespace heapwatch::device_command
