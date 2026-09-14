#pragma once

#include <cstddef>
#include <cstdint>

namespace Heapwatch::Ota {

constexpr const char* TAG = "HwOta";

constexpr size_t URL_MAX = 512;
constexpr size_t SHA_HEX_MAX = 65;
constexpr size_t MAX_DATA_PARTITIONS = 4;
constexpr size_t PARTITION_NAME_MAX = 32;

struct DataPartitionJob {
    char name[PARTITION_NAME_MAX] = {};
    char download_url[URL_MAX] = {};
    char sha256[SHA_HEX_MAX] = {};
    int64_t size = 0;
};

struct OtaJob {
    int64_t ota_session_id = 0;
    char code_download_url[URL_MAX] = {};
    char code_checksum[SHA_HEX_MAX] = {};
    int64_t code_size = 0;
    bool include_data = false;
    size_t data_count = 0;
    DataPartitionJob data[MAX_DATA_PARTITIONS] = {};
};

void init();

void shutdown();

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

}  // namespace Heapwatch::Ota
