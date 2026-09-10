#pragma once

#include <cstddef>

namespace heapwatch::device_ota {

constexpr const char* kLogTag = "HwOta";

void init();

/**
 * Handle MQTT command suffixes: ota/start, ota/abort, ota/rollback, ota/confirm.
 * Returns true when suffix starts with "ota/".
 *
 * Partition-agnostic: uses esp_ota_get_next_update_partition / running partition
 * at runtime. Never hardcodes a client's partitions.csv layout.
 */
bool handleMqttCommand(const char* suffix, const char* command_id, const char* payload,
                       size_t payload_len);

bool isInProgress();

}  // namespace heapwatch::device_ota
