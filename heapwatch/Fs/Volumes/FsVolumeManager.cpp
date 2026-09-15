#include "heapwatch/Fs/Volumes/FsVolumeManager.h"
#include "heapwatch/Fs/Volumes/FsVolumeManager-Internal.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <dirent.h>

#include "esp_log.h"
#include "esp_littlefs.h"
#include "esp_spiffs.h"
#include "esp_vfs_fat.h"
#include "sdkconfig.h"

#include "heapwatch/Helpers/DynamicParameterSegment.h"
#include "heapwatch/Helpers/MemoryManager/HeapwatchMemoryManager.h"
#include "heapwatch.h"
#include "heapwatch_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace Heapwatch::Fs::Volumes {

namespace Internal {

static constexpr char TAG[] = "HwFs";

HostMapping* g_host_map = nullptr;
HostMapping* g_sd_map = nullptr;
Volume* g_volumes = nullptr;
size_t g_volume_count = 0;
bool g_mount_all_done = false;
SemaphoreHandle_t g_mutex = nullptr;

void bindVolumeTables() {
    if (g_volumes != nullptr) {
        return;
    }
    Heapwatch::Helpers::MemoryManager::init();
    Heapwatch::Helpers::ConnectorMemoryBlock* ws = Heapwatch::Helpers::MemoryManager::get();
    if (ws == nullptr) {
        return;
    }
    g_host_map = ws->fs_host_map;
    g_sd_map = ws->fs_sd_map;
    g_volumes = ws->fs_volumes;
    for (size_t i = 0; i < MAX_VOLUMES; ++i) {
        g_volumes[i] = Volume{};
    }
}

SemaphoreHandle_t mutexHandle() {
    bindVolumeTables();
    if (g_mutex == nullptr) {
        g_mutex = xSemaphoreCreateMutex();
    }
    return g_mutex;
}

void lockTable() {
    SemaphoreHandle_t mutex = mutexHandle();
    if (mutex != nullptr) {
        (void)xSemaphoreTake(mutex, portMAX_DELAY);
    }
}

void unlockTable() {
    if (g_mutex != nullptr) {
        (void)xSemaphoreGive(g_mutex);
    }
}


void copyLabel(char* dest, size_t dest_len, const char* src) {
    if (dest == nullptr || dest_len == 0) {
        return;
    }
    if (src == nullptr) {
        dest[0] = '\0';
        return;
    }
    strncpy(dest, src, dest_len - 1);
    dest[dest_len - 1] = '\0';
}

bool containsDotDot(const char* path) {
    if (path == nullptr) {
        return true;
    }
    return strstr(path, "/..") != nullptr || strstr(path, "../") != nullptr || strcmp(path, "..") == 0;
}

bool validMountPath(const char* path) {
    if (path == nullptr || path[0] != '/' || path[1] == '\0') {
        return false;
    }
    if (containsDotDot(path)) {
        return false;
    }
    return strlen(path) < MOUNT_PATH_MAX;
}

bool validLabel(const char* label) {
    return label != nullptr && label[0] != '\0' && strlen(label) < LABEL_MAX;
}

bool pathStartsWithMount(const char* path, const char* mount) {
    if (path == nullptr || mount == nullptr) {
        return false;
    }
    const size_t mount_len = strlen(mount);
    if (strncmp(path, mount, mount_len) != 0) {
        return false;
    }
    return path[mount_len] == '\0' || path[mount_len] == '/';
}

bool dirExists(const char* path) {
    if (path == nullptr) {
        return false;
    }
    DIR* dir = opendir(path);
    if (dir == nullptr) {
        return false;
    }
    closedir(dir);
    return true;
}

const HostMapping* findHostMapping(const char* label) {
    if (label == nullptr || g_host_map == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < MAX_FLASH_VOLUMES; ++i) {
        if (g_host_map[i].used && strcmp(g_host_map[i].label, label) == 0) {
            return &g_host_map[i];
        }
    }
    return nullptr;
}

/** Reserve or reuse a slot for `label`; nullptr when the table is full. */
HostMapping* claimMappingSlot(HostMapping* table, size_t table_len, const char* label) {
    if (table == nullptr || label == nullptr) {
        return nullptr;
    }
    HostMapping* empty = nullptr;
    for (size_t i = 0; i < table_len; ++i) {
        if (table[i].used && strcmp(table[i].label, label) == 0) {
            return &table[i];
        }
        if (!table[i].used && empty == nullptr) {
            empty = &table[i];
        }
    }
    return empty;
}

Volume* findVolumeByLabelLocked(const char* label) {
    if (label == nullptr || g_volumes == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < g_volume_count; ++i) {
        if (strcmp(g_volumes[i].label, label) == 0) {
            return &g_volumes[i];
        }
    }
    return nullptr;
}

void defaultMountPath(const char* label, char* out, size_t out_len) {
    snprintf(out, out_len, "/%s", label);
}

void registerAlreadyMountedVolume(Kind kind, const char* label, const char* mount_path, bool connector_mounted,
                                  wl_handle_t fat_wl, bool browsable, bool external) {
    Volume* existing = findVolumeByLabelLocked(label);
    if (existing != nullptr) {
        snprintf(existing->mount_path, sizeof(existing->mount_path), "%s", mount_path);
        existing->kind = kind;
        existing->mounted = true;
        existing->browsable = browsable;
        existing->connector_mounted = connector_mounted;
        existing->external = external;
        existing->fat_wl = fat_wl;
        return;
    }
    if (g_volume_count >= MAX_VOLUMES) {
        ESP_LOGW(TAG, "FS volume table full; skipping '%s'", label);
        return;
    }
    Volume& vol = g_volumes[g_volume_count++];
    vol = {};
    copyLabel(vol.label, sizeof(vol.label), label);
    snprintf(vol.mount_path, sizeof(vol.mount_path), "%s", mount_path);
    vol.kind = kind;
    vol.mounted = true;
    vol.browsable = browsable;
    vol.connector_mounted = connector_mounted;
    vol.external = external;
    vol.fat_wl = fat_wl;
    ESP_LOGI(TAG, "%s volume '%s' at '%s' (%s)", kindName(kind), label, mount_path,
             connector_mounted ? "connector-mounted" : "host-owned");
}

void tryBindRegisteredVolumesNowMountedLocked() {
    if (!g_mount_all_done) {
        return;
    }
    for (size_t i = 0; i < MAX_SD_VOLUMES; ++i) {
        if (g_sd_map[i].used) {
            (void)applySdMappingLocked(g_sd_map[i]);
        }
    }
    for (size_t i = 0; i < MAX_FLASH_VOLUMES; ++i) {
        if (g_host_map[i].used) {
            (void)applyFlashMappingLocked(g_host_map[i]);
        }
    }
}

}  // namespace Internal

void unbindMemoryBlock() {
    Internal::g_host_map = nullptr;
    Internal::g_sd_map = nullptr;
    Internal::g_volumes = nullptr;
    Internal::g_volume_count = 0;
    Internal::g_mount_all_done = false;
}

void tryBindRegisteredVolumesNowMounted() {
    Internal::TableLock lock;
    Internal::tryBindRegisteredVolumesNowMountedLocked();
}

size_t mountedCount() {
    Internal::TableLock lock;
    Internal::tryBindRegisteredVolumesNowMountedLocked();
    return Internal::g_volume_count;
}

const Volume* volumeAt(size_t index) {
    Internal::TableLock lock;
    Internal::tryBindRegisteredVolumesNowMountedLocked();
    if (index >= Internal::g_volume_count) {
        return nullptr;
    }
    return &Internal::g_volumes[index];
}

const Volume* findByPath(const char* path) {
    Internal::TableLock lock;
    Internal::tryBindRegisteredVolumesNowMountedLocked();
    const Volume* best = nullptr;
    size_t best_len = 0;
    for (size_t i = 0; i < Internal::g_volume_count; ++i) {
        const Volume& vol = Internal::g_volumes[i];
        if (!vol.browsable) {
            continue;
        }
        if (!Internal::pathStartsWithMount(path, vol.mount_path)) {
            continue;
        }
        const size_t len = strlen(vol.mount_path);
        if (len > best_len) {
            best_len = len;
            best = &vol;
        }
    }
    return best;
}

bool pathIsAllowed(const char* path) {
    return findByPath(path) != nullptr;
}

bool queryUsage(const Volume& volume, size_t* total_bytes, size_t* used_bytes) {
    if (volume.kind == Kind::KIND_FAT) {
        uint64_t total = 0;
        uint64_t free_bytes = 0;
        if (esp_vfs_fat_info(volume.mount_path, &total, &free_bytes) != ESP_OK) {
            return false;
        }
        if (total_bytes != nullptr) {
            *total_bytes = static_cast<size_t>(total);
        }
        if (used_bytes != nullptr) {
            *used_bytes = static_cast<size_t>(total >= free_bytes ? total - free_bytes : 0);
        }
        return true;
    }
    return queryUsageByLabel(volume.label, volume.kind, total_bytes, used_bytes);
}

bool queryUsageByLabel(const char* label, Kind kind, size_t* total_bytes, size_t* used_bytes) {
    size_t total = 0;
    size_t used = 0;
    esp_err_t err = ESP_ERR_NOT_FOUND;
    switch (kind) {
        case Kind::KIND_SPIFFS:
            err = esp_spiffs_info(label, &total, &used);
            if (err != ESP_OK) {
                // Arduino SPIFFS.begin() defaults to a NULL partition label.
                err = esp_spiffs_info(nullptr, &total, &used);
            }
            break;
        case Kind::KIND_LITTLEFS:
            err = esp_littlefs_info(label, &total, &used);
            break;
        case Kind::KIND_FAT:
            return false;
    }
    if (err != ESP_OK) {
        return false;
    }
    if (total_bytes != nullptr) {
        *total_bytes = total;
    }
    if (used_bytes != nullptr) {
        *used_bytes = used;
    }
    return true;
}

const char* kindName(Kind kind) {
    switch (kind) {
        case Kind::KIND_SPIFFS:
            return "spiffs";
        case Kind::KIND_LITTLEFS:
            return "littlefs";
        case Kind::KIND_FAT:
            return "fat";
    }
    return "unknown";
}

cJSON* buildVolumeObject(const Volume& volume) {
    cJSON* vol = cJSON_CreateObject();
    cJSON_AddStringToObject(vol, "id", volume.label);
    cJSON_AddStringToObject(vol, "label", volume.label);
    cJSON_AddStringToObject(vol, "path", volume.mount_path);
    cJSON_AddStringToObject(vol, "filesystem", kindName(volume.kind));
    if (volume.external) {
        cJSON_AddNullToObject(vol, "partition_id");
    } else {
        cJSON_AddStringToObject(vol, "partition_id", volume.label);
    }
    size_t total = 0;
    size_t used = 0;
    if (queryUsage(volume, &total, &used)) {
        cJSON_AddNumberToObject(vol, "total_bytes", static_cast<double>(total));
        cJSON_AddNumberToObject(vol, "used_bytes", static_cast<double>(used));
    } else {
        cJSON_AddNullToObject(vol, "total_bytes");
        cJSON_AddNullToObject(vol, "used_bytes");
    }
    return vol;
}

size_t appendParameterFields(char* out, size_t out_len, bool json) {
    if (out == nullptr || out_len == 0) {
        return 0;
    }

#if defined(CONFIG_HEAPWATCH_ENABLE_FS_VOLUME_STATS) && CONFIG_HEAPWATCH_ENABLE_FS_VOLUME_STATS
    Volume copies[MAX_VOLUMES];
    size_t count = 0;
    {
        Internal::TableLock lock;
        Internal::tryBindRegisteredVolumesNowMountedLocked();
        count = Internal::g_volume_count;
        if (count > MAX_VOLUMES) {
            count = MAX_VOLUMES;
        }
        for (size_t i = 0; i < count; ++i) {
            copies[i] = Internal::g_volumes[i];
        }
    }

    size_t pos = 0;
    for (size_t i = 0; i < count && pos < out_len; ++i) {
        const Volume& vol = copies[i];
        if (!vol.browsable) {
            continue;
        }
        size_t total = 0;
        size_t used = 0;
        if (!queryUsage(vol, &total, &used)) {
            continue;
        }
        char slug_seg[LABEL_MAX];
        if (!Heapwatch::Helpers::normalizeDynamicSegment(vol.label, slug_seg, sizeof(slug_seg))) {
            continue;
        }
        int32_t written = 0;
        if (json) {
            written = snprintf(out + pos, out_len - pos,
                               ",\"dynamic.fs.%s.used_bytes\":%" PRIu64
                               ",\"dynamic.fs.%s.used_bytes_max\":%" PRIu64,
                               slug_seg, static_cast<uint64_t>(used), slug_seg,
                               static_cast<uint64_t>(total));
        } else {
            written = snprintf(out + pos, out_len - pos,
                               "%sdynamic.fs.%s.used_bytes=%" PRIu64
                               "%sdynamic.fs.%s.used_bytes_max=%" PRIu64,
                               HEAPWATCH_FLAT_DELIMITER, slug_seg, static_cast<uint64_t>(used),
                               HEAPWATCH_FLAT_DELIMITER, slug_seg, static_cast<uint64_t>(total));
        }
        if (written < 0 || static_cast<size_t>(written) >= out_len - pos) {
            out[pos] = '\0';
            break;
        }
        pos += static_cast<size_t>(written);
    }
    return pos;
#else
    (void)json;
    out[0] = '\0';
    return 0;
#endif
}

}  // namespace Heapwatch::Fs::Volumes
