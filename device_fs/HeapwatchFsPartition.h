#pragma once

#include "esp_partition.h"

// ESP-IDF 5.3 may omit ESP_PARTITION_SUBTYPE_DATA_LITTLEFS; CSV/on-wire value is 0x83.
#ifndef HEAPWATCH_PARTITION_SUBTYPE_LITTLEFS
#ifdef ESP_PARTITION_SUBTYPE_DATA_LITTLEFS
#define HEAPWATCH_PARTITION_SUBTYPE_LITTLEFS ESP_PARTITION_SUBTYPE_DATA_LITTLEFS
#else
#define HEAPWATCH_PARTITION_SUBTYPE_LITTLEFS static_cast<esp_partition_subtype_t>(0x83)
#endif
#endif

inline bool heapwatchIsLittlefsSubtype(esp_partition_subtype_t subtype) {
    return subtype == HEAPWATCH_PARTITION_SUBTYPE_LITTLEFS ||
           subtype == static_cast<esp_partition_subtype_t>(0x83);
}
