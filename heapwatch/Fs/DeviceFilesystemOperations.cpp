#include "heapwatch/Fs/DeviceFilesystemOperations.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_flash.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"
#include "sdkconfig.h"

#include "heapwatch/Fs/Volumes/FsVolumeManager.h"
#include "heapwatch/Fs/HeapwatchFsPartition.h"

namespace Heapwatch::Fs {

namespace {

/** ESP-IDF: partition table always occupies one 4 KiB flash sector. */
constexpr uint32_t PARTITION_TABLE_SECTOR_BYTES = 0x1000;
/** Secure Boot V1 digest at flash offset 0 (one sector). */
constexpr uint32_t SECURE_BOOT_DIGEST_BYTES = 0x1000;
/** ESP-IDF partition types (esp_partition_type_t) — stable since IDF 5.x CSV tooling. */
constexpr int32_t PARTITION_TYPE_BOOTLOADER = 0x02;
constexpr int32_t PARTITION_TYPE_PARTITION_TABLE = 0x03;
constexpr int32_t PARTITION_SUBTYPE_PRIMARY = 0x00;

bool containsDotDot(const char* path) {
    if (path == nullptr) {
        return true;
    }
    return strstr(path, "/..") != nullptr || strstr(path, "../") != nullptr ||
           strcmp(path, "..") == 0;
}

}  // namespace

bool isAllowedPath(const char* path) {
    if (path == nullptr || path[0] != '/') {
        return false;
    }
    if (containsDotDot(path)) {
        return false;
    }
    return Heapwatch::Fs::Volumes::pathIsAllowed(path);
}

namespace {

const char* espTypeName(esp_partition_type_t type) {
    switch (type) {
        case ESP_PARTITION_TYPE_APP:
            return "app";
        case ESP_PARTITION_TYPE_DATA:
            return "data";
        default:
            return nullptr;
    }
}

/** ESP-IDF subtype CSV name when known; nullptr if unknown (do not invent hex here). */
const char* espSubtypeName(esp_partition_type_t type, esp_partition_subtype_t subtype) {
    if (type == ESP_PARTITION_TYPE_APP) {
        if (subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
            return "factory";
        }
        if (subtype == ESP_PARTITION_SUBTYPE_APP_TEST) {
            return "test";
        }
        // ota_N is formatted by the caller (needs a writable buffer).
        if (subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
            subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
            return nullptr;
        }
        return nullptr;
    }

    if (type == ESP_PARTITION_TYPE_DATA) {
        switch (subtype) {
            case ESP_PARTITION_SUBTYPE_DATA_OTA:
                return "ota";
            case ESP_PARTITION_SUBTYPE_DATA_PHY:
                return "phy";
            case ESP_PARTITION_SUBTYPE_DATA_NVS:
                return "nvs";
            case ESP_PARTITION_SUBTYPE_DATA_COREDUMP:
                return "coredump";
            case ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS:
                return "nvs_keys";
            case ESP_PARTITION_SUBTYPE_DATA_EFUSE_EM:
                return "efuse_em";
            case ESP_PARTITION_SUBTYPE_DATA_UNDEFINED:
                return "undefined";
            case ESP_PARTITION_SUBTYPE_DATA_FAT:
                return "fat";
            case ESP_PARTITION_SUBTYPE_DATA_SPIFFS:
                return "spiffs";
            default:
                if (heapwatchIsLittlefsSubtype(subtype)) {
                    return "littlefs";
                }
                return nullptr;
        }
    }

    return nullptr;
}

bool formatOtaSubtypeName(esp_partition_type_t type, esp_partition_subtype_t subtype, char* buf, size_t buflen) {
    if (buf == nullptr || buflen == 0) {
        return false;
    }
    if (type != ESP_PARTITION_TYPE_APP) {
        return false;
    }
    if (subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN || subtype > ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
        return false;
    }
    const int32_t slot = static_cast<int32_t>(subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN);
    snprintf(buf, buflen, "ota_%" PRId32, slot);
    return true;
}

bool tryFsUsedBytes(const char* label, Heapwatch::Fs::Volumes::Kind kind, int64_t* used_out, int64_t* total_out) {
    if (label == nullptr || used_out == nullptr || total_out == nullptr) {
        return false;
    }
    size_t total = 0;
    size_t used = 0;
    if (kind == Heapwatch::Fs::Volumes::Kind::KIND_FAT) {
        const Heapwatch::Fs::Volumes::Volume* vol = nullptr;
        for (size_t i = 0; i < Heapwatch::Fs::Volumes::mountedCount(); ++i) {
            const Heapwatch::Fs::Volumes::Volume* candidate = Heapwatch::Fs::Volumes::volumeAt(i);
            if (candidate != nullptr && !candidate->external &&
                strcmp(candidate->label, label) == 0) {
                vol = candidate;
                break;
            }
        }
        if (vol == nullptr || !Heapwatch::Fs::Volumes::queryUsage(*vol, &total, &used)) {
            return false;
        }
    } else if (!Heapwatch::Fs::Volumes::queryUsageByLabel(label, kind, &total, &used)) {
        return false;
    }
    *total_out = static_cast<int64_t>(total);
    *used_out = static_cast<int64_t>(used);
    return true;
}

uint32_t bootloaderFlashOffset() {
#if defined(CONFIG_BOOTLOADER_OFFSET_IN_FLASH)
    return static_cast<uint32_t>(CONFIG_BOOTLOADER_OFFSET_IN_FLASH);
#else
    return static_cast<uint32_t>(ESP_BOOTLOADER_OFFSET);
#endif
}

cJSON* buildSyntheticPartitionObject(const char* id, const char* label, const char* type,
                                     const char* subtype, int32_t type_id, int32_t subtype_id,
                                     uint32_t address, uint32_t size_bytes) {
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", id);
    cJSON_AddStringToObject(obj, "label", label);
    cJSON_AddStringToObject(obj, "type", type);
    cJSON_AddStringToObject(obj, "subtype", subtype);
    cJSON_AddNumberToObject(obj, "type_id", static_cast<double>(type_id));
    cJSON_AddNumberToObject(obj, "subtype_id", static_cast<double>(subtype_id));
    cJSON_AddNumberToObject(obj, "address", static_cast<double>(address));
    cJSON_AddNumberToObject(obj, "total_bytes", static_cast<double>(size_bytes));
    cJSON_AddNullToObject(obj, "used_bytes");
    cJSON_AddBoolToObject(obj, "browsable", false);
    cJSON_AddNullToObject(obj, "volume_id");
    cJSON* flags = cJSON_CreateArray();
    cJSON_AddItemToObject(obj, "flags", flags);
    return obj;
}

void appendHiddenFlashPartitions(cJSON* partitions) {
    if (partitions == nullptr) {
        return;
    }

    const uint32_t table_offset = static_cast<uint32_t>(ESP_PARTITION_TABLE_OFFSET);
    const uint32_t boot_offset = bootloaderFlashOffset();

#if CONFIG_SECURE_BOOT
    cJSON_AddItemToArray(
        partitions,
        buildSyntheticPartitionObject("secure_boot_digest", "Reserved", "reserved", "reserved", 0, 0,
                                      0, SECURE_BOOT_DIGEST_BYTES));
#endif

    if (table_offset > boot_offset) {
        cJSON_AddItemToArray(
            partitions,
            buildSyntheticPartitionObject("bootloader", "Bootloader", "bootloader", "primary",
                                      PARTITION_TYPE_BOOTLOADER, PARTITION_SUBTYPE_PRIMARY,
                                      boot_offset, table_offset - boot_offset));
    }

    cJSON_AddItemToArray(
        partitions,
        buildSyntheticPartitionObject("partition_table", "Partition Table", "partition_table",
                                      "primary", PARTITION_TYPE_PARTITION_TABLE,
                                      PARTITION_SUBTYPE_PRIMARY, table_offset,
                                      PARTITION_TABLE_SECTOR_BYTES));
}

cJSON* buildPartitionObject(const esp_partition_t* part) {
    if (part == nullptr) {
        return nullptr;
    }

    const char* label = part->label[0] != '\0' ? part->label : "unnamed";
    const char* type_name = espTypeName(part->type);
    char ota_subtype_buf[8];
    const char* subtype_name = espSubtypeName(part->type, part->subtype);
    if (subtype_name == nullptr &&
        formatOtaSubtypeName(part->type, part->subtype, ota_subtype_buf, sizeof(ota_subtype_buf))) {
        subtype_name = ota_subtype_buf;
    }

    bool browsable = false;
    const char* volume_id = nullptr;
    int64_t used = -1;
    int64_t total_bytes = static_cast<int64_t>(part->size);

    Heapwatch::Fs::Volumes::Kind kind = Heapwatch::Fs::Volumes::Kind::KIND_SPIFFS;
    bool is_fs = false;
    if (part->type == ESP_PARTITION_TYPE_DATA &&
        part->subtype == ESP_PARTITION_SUBTYPE_DATA_SPIFFS) {
        kind = Heapwatch::Fs::Volumes::Kind::KIND_SPIFFS;
        is_fs = true;
    } else if (part->type == ESP_PARTITION_TYPE_DATA &&
               heapwatchIsLittlefsSubtype(part->subtype)) {
        kind = Heapwatch::Fs::Volumes::Kind::KIND_LITTLEFS;
        is_fs = true;
    } else if (part->type == ESP_PARTITION_TYPE_DATA &&
               part->subtype == ESP_PARTITION_SUBTYPE_DATA_FAT) {
        kind = Heapwatch::Fs::Volumes::Kind::KIND_FAT;
        is_fs = true;
    }

    if (is_fs) {
        int64_t fs_used = 0;
        int64_t fs_total = 0;
        if (tryFsUsedBytes(label, kind, &fs_used, &fs_total)) {
            used = fs_used;
            (void)fs_total;
        }
        for (size_t i = 0; i < Heapwatch::Fs::Volumes::mountedCount(); ++i) {
            const Heapwatch::Fs::Volumes::Volume* vol = Heapwatch::Fs::Volumes::volumeAt(i);
            if (vol != nullptr && vol->browsable && !vol->external &&
                strcmp(vol->label, label) == 0) {
                browsable = true;
                volume_id = vol->label;
                break;
            }
        }
    }

    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", label);
    cJSON_AddStringToObject(obj, "label", label);
    if (type_name != nullptr) {
        cJSON_AddStringToObject(obj, "type", type_name);
    } else {
        cJSON_AddNullToObject(obj, "type");
    }
    if (subtype_name != nullptr) {
        cJSON_AddStringToObject(obj, "subtype", subtype_name);
    } else {
        cJSON_AddNullToObject(obj, "subtype");
    }
    cJSON_AddNumberToObject(obj, "type_id", static_cast<double>(part->type));
    cJSON_AddNumberToObject(obj, "subtype_id", static_cast<double>(part->subtype));
    cJSON_AddNumberToObject(obj, "address", static_cast<double>(part->address));
    cJSON_AddNumberToObject(obj, "total_bytes", static_cast<double>(total_bytes));
    if (used >= 0) {
        cJSON_AddNumberToObject(obj, "used_bytes", static_cast<double>(used));
    } else {
        cJSON_AddNullToObject(obj, "used_bytes");
    }
    cJSON_AddBoolToObject(obj, "browsable", browsable);
    if (volume_id != nullptr) {
        cJSON_AddStringToObject(obj, "volume_id", volume_id);
    } else {
        cJSON_AddNullToObject(obj, "volume_id");
    }

    cJSON* flags = cJSON_CreateArray();
    if (part->encrypted) {
        cJSON_AddItemToArray(flags, cJSON_CreateString("encrypted"));
    }
    cJSON_AddItemToObject(obj, "flags", flags);

    return obj;
}

cJSON* buildPartitionsArray() {
    cJSON* partitions = cJSON_CreateArray();
    appendHiddenFlashPartitions(partitions);

    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    while (it != nullptr) {
        const esp_partition_t* part = esp_partition_get(it);
        cJSON* obj = buildPartitionObject(part);
        if (obj != nullptr) {
            cJSON_AddItemToArray(partitions, obj);
        }
        it = esp_partition_next(it);
    }
    // esp_partition_next(NULL terminator) already releases the iterator.
    return partitions;
}

}  // namespace

cJSON* buildVolumesResult() {
    cJSON* volumes = cJSON_CreateArray();

    for (size_t i = 0; i < Heapwatch::Fs::Volumes::mountedCount(); ++i) {
        const Heapwatch::Fs::Volumes::Volume* vol = Heapwatch::Fs::Volumes::volumeAt(i);
        if (vol == nullptr || !vol->browsable) {
            continue;
        }
        cJSON* obj = Heapwatch::Fs::Volumes::buildVolumeObject(*vol);
        if (obj != nullptr) {
            cJSON_AddItemToArray(volumes, obj);
        }
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "volumes", volumes);
    return result;
}

cJSON* buildStorageResult() {
    cJSON* result = cJSON_CreateObject();

    uint32_t flash_size = 0;
    if (esp_flash_get_physical_size(nullptr, &flash_size) == ESP_OK && flash_size > 0) {
        cJSON_AddNumberToObject(result, "flash_size_bytes", static_cast<double>(flash_size));
    } else {
        cJSON_AddNullToObject(result, "flash_size_bytes");
    }

    const uint32_t table_offset = static_cast<uint32_t>(ESP_PARTITION_TABLE_OFFSET);
    const uint32_t boot_offset = bootloaderFlashOffset();
    cJSON_AddNumberToObject(result, "partition_table_offset", static_cast<double>(table_offset));
    cJSON_AddNumberToObject(result, "bootloader_offset", static_cast<double>(boot_offset));

    cJSON_AddItemToObject(result, "partitions", buildPartitionsArray());

    cJSON* volumesWrapper = buildVolumesResult();
    cJSON* volumes = cJSON_DetachItemFromObject(volumesWrapper, "volumes");
    cJSON_Delete(volumesWrapper);
    if (volumes == nullptr) {
        volumes = cJSON_CreateArray();
    }
    cJSON_AddItemToObject(result, "volumes", volumes);
    return result;
}

}  // namespace Heapwatch::Fs
