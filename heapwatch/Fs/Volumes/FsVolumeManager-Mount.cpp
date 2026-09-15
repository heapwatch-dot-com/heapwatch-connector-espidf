#include "heapwatch/Fs/Volumes/FsVolumeManager.h"
#include "heapwatch/Fs/Volumes/FsVolumeManager-Internal.h"

#include <cstdio>
#include <cstring>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "esp_vfs_fat.h"
#include "sdkconfig.h"

#include "heapwatch/Fs/HeapwatchFsPartition.h"
#include "heapwatch.h"

namespace Heapwatch::Fs::Volumes {

namespace Internal {

static constexpr char TAG[] = "HwFs";

namespace {

bool formatFilesystemIfMountFailed() {
#if CONFIG_HEAPWATCH_AUTO_FORMAT_BLANK_STORAGE_PARTITIONS
    return true;
#else
    return false;
#endif
}

bool autoMountEnabled() {
#if CONFIG_HEAPWATCH_AUTO_MOUNT_STORAGE_PARTITIONS
    return true;
#else
    return false;
#endif
}

bool kindFromPartition(const esp_partition_t* part, Kind* kind) {
    if (part == nullptr || kind == nullptr || part->type != ESP_PARTITION_TYPE_DATA) {
        return false;
    }
    if (part->subtype == ESP_PARTITION_SUBTYPE_DATA_SPIFFS) {
        *kind = Kind::KIND_SPIFFS;
        return true;
    }
    if (heapwatchIsLittlefsSubtype(part->subtype)) {
        *kind = Kind::KIND_LITTLEFS;
        return true;
    }
    if (part->subtype == ESP_PARTITION_SUBTYPE_DATA_FAT) {
        *kind = Kind::KIND_FAT;
        return true;
    }
    return false;
}

bool driverSaysMounted(Kind kind, const char* label) {
    size_t total = 0;
    size_t used = 0;
    switch (kind) {
        case Kind::KIND_SPIFFS:
            return esp_spiffs_info(label, &total, &used) == ESP_OK;
        case Kind::KIND_LITTLEFS:
            return esp_littlefs_info(label, &total, &used) == ESP_OK;
        case Kind::KIND_FAT:
            return false;
    }
    return false;
}

enum class MountOutcome : uint8_t { Failed, ConnectorOwned, HostOwned };

MountOutcome mountSpiffs(const char* label, const char* mount_path) {
    esp_vfs_spiffs_conf_t conf = {};
    conf.base_path = mount_path;
    conf.partition_label = label;
    conf.max_files = 4;
    conf.format_if_mount_failed = formatFilesystemIfMountFailed();
    const esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_OK) {
        return MountOutcome::ConnectorOwned;
    }
    if (err == ESP_ERR_INVALID_STATE && dirExists(mount_path)) {
        return MountOutcome::HostOwned;
    }
    ESP_LOGW(TAG, "SPIFFS mount '%s' at '%s' failed: %s", label, mount_path, esp_err_to_name(err));
    return MountOutcome::Failed;
}

MountOutcome mountLittlefs(const char* label, const char* mount_path) {
    const esp_partition_t* part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = mount_path;
    conf.partition_label = label;
    conf.partition = part;
    conf.format_if_mount_failed = formatFilesystemIfMountFailed();
    conf.dont_mount = false;
    const esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err == ESP_OK) {
        return MountOutcome::ConnectorOwned;
    }
    if (err == ESP_ERR_INVALID_STATE && dirExists(mount_path)) {
        return MountOutcome::HostOwned;
    }
    ESP_LOGW(TAG, "LittleFS mount '%s' at '%s' failed: %s", label, mount_path, esp_err_to_name(err));
    return MountOutcome::Failed;
}

MountOutcome mountFat(Volume* volume) {
    if (volume == nullptr) {
        return MountOutcome::Failed;
    }
    if (dirExists(volume->mount_path)) {
        return MountOutcome::HostOwned;
    }
    esp_vfs_fat_mount_config_t cfg = {};
    cfg.format_if_mount_failed = formatFilesystemIfMountFailed();
    cfg.max_files = 4;
    cfg.allocation_unit_size = 4096;
    const esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(volume->mount_path, volume->label, &cfg,
                                                            &volume->fat_wl);
    if (err == ESP_OK) {
        return MountOutcome::ConnectorOwned;
    }
    if ((err == ESP_ERR_INVALID_STATE || err == ESP_ERR_INVALID_ARG) && dirExists(volume->mount_path)) {
        volume->fat_wl = WL_INVALID_HANDLE;
        return MountOutcome::HostOwned;
    }
    ESP_LOGW(TAG, "FatFS mount '%s' at '%s' failed: %s", volume->label, volume->mount_path,
             esp_err_to_name(err));
    return MountOutcome::Failed;
}

MountOutcome doMount(Volume* volume) {
    if (volume == nullptr) {
        return MountOutcome::Failed;
    }
    switch (volume->kind) {
        case Kind::KIND_SPIFFS:
            return mountSpiffs(volume->label, volume->mount_path);
        case Kind::KIND_LITTLEFS:
            return mountLittlefs(volume->label, volume->mount_path);
        case Kind::KIND_FAT:
            return mountFat(volume);
    }
    return MountOutcome::Failed;
}

void processPartition(const esp_partition_t* part) {
    Kind kind = Kind::KIND_SPIFFS;
    if (!kindFromPartition(part, &kind)) {
        return;
    }
    const char* label = part->label[0] != '\0' ? part->label : nullptr;
    if (!validLabel(label)) {
        return;
    }

    char intended[MOUNT_PATH_MAX] = {};
    const HostMapping* mapping = findHostMapping(label);
    if (mapping != nullptr) {
        snprintf(intended, sizeof(intended), "%s", mapping->mount_path);
    } else {
        defaultMountPath(label, intended, sizeof(intended));
    }

    if (!autoMountEnabled()) {
        if (mapping == nullptr) {
            return;
        }
        if (dirExists(intended)) {
            registerAlreadyMountedVolume(kind, label, intended, false, WL_INVALID_HANDLE, true);
        } else {
            ESP_LOGW(TAG, "Registered volume '%s' at '%s' is not mounted", label, intended);
        }
        return;
    }

    const bool already = driverSaysMounted(kind, label) || dirExists(intended);
    if (already) {
        const bool browsable = dirExists(intended);
        if (!browsable) {
            ESP_LOGW(TAG,
                     "Partition '%s' is mounted by the host but '%s' is not a VFS root — "
                     "call heapwatch_register_fs_volume() with the real mount path",
                     label, intended);
        }
        registerAlreadyMountedVolume(kind, label, intended, false, WL_INVALID_HANDLE, browsable);
        return;
    }

    Volume pending = {};
    copyLabel(pending.label, sizeof(pending.label), label);
    snprintf(pending.mount_path, sizeof(pending.mount_path), "%s", intended);
    pending.kind = kind;
    pending.fat_wl = WL_INVALID_HANDLE;
    const MountOutcome outcome = doMount(&pending);
    if (outcome == MountOutcome::Failed) {
        return;
    }
    registerAlreadyMountedVolume(kind, label, intended, outcome == MountOutcome::ConnectorOwned, pending.fat_wl,
               dirExists(intended));
}

}  // namespace

esp_err_t applyFlashMappingLocked(const HostMapping& mapping) {
    Volume* existing = findVolumeByLabelLocked(mapping.label);
    if (existing != nullptr) {
        if (strcmp(existing->mount_path, mapping.mount_path) == 0) {
            if (dirExists(mapping.mount_path)) {
                existing->browsable = true;
                existing->mounted = true;
            }
            return ESP_OK;
        }
        if (existing->connector_mounted) {
            ESP_LOGE(TAG,
                     "Cannot remount connector-owned flash volume '%s' from '%s' to '%s' — "
                     "register the custom path before the connector mounts it, or disable auto-mount",
                     mapping.label, existing->mount_path, mapping.mount_path);
            return ESP_ERR_HEAPWATCH_INVALID_ARG;
        }
        if (!dirExists(mapping.mount_path)) {
            return ESP_OK;
        }
        snprintf(existing->mount_path, sizeof(existing->mount_path), "%s", mapping.mount_path);
        existing->browsable = true;
        existing->mounted = true;
        return ESP_OK;
    }

    if (!g_mount_all_done) {
        return ESP_OK;
    }

    const esp_partition_t* part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, mapping.label);
    if (part == nullptr) {
        ESP_LOGW(TAG, "No flash data partition labelled '%s'", mapping.label);
        return ESP_ERR_HEAPWATCH_INVALID_ARG;
    }
    processPartition(part);
    return ESP_OK;
}

void scanAndMountFlashPartitions() {
    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    while (it != nullptr) {
        const esp_partition_t* part = esp_partition_get(it);
        processPartition(part);
        it = esp_partition_next(it);
    }
}

}  // namespace Internal

esp_err_t registerVolume(const char* partition_label, const char* mount_path) {
    if (!Internal::validLabel(partition_label) || !Internal::validMountPath(mount_path)) {
        return ESP_ERR_HEAPWATCH_INVALID_ARG;
    }

    Internal::TableLock lock;
    HostMapping* slot = Internal::claimMappingSlot(Internal::g_host_map, MAX_FLASH_VOLUMES, partition_label);
    if (slot == nullptr) {
        return ESP_ERR_HEAPWATCH_NO_SPACE;
    }
    slot->used = true;
    Internal::copyLabel(slot->label, sizeof(slot->label), partition_label);
    snprintf(slot->mount_path, sizeof(slot->mount_path), "%s", mount_path);
    return Internal::applyFlashMappingLocked(*slot);
}

void mountAll() {
    Internal::TableLock lock;
    Internal::g_volume_count = 0;
    for (size_t i = 0; i < MAX_VOLUMES; ++i) {
        Internal::g_volumes[i] = {};
    }

    Internal::scanAndMountFlashPartitions();
    Internal::registerAlreadyMountedSdVolumes();
    Internal::g_mount_all_done = true;
    Internal::tryBindRegisteredVolumesNowMountedLocked();
}

void unmountConnectorOwned() {
    Internal::TableLock lock;
    for (size_t i = 0; i < Internal::g_volume_count; ++i) {
        Volume& vol = Internal::g_volumes[i];
        if (!vol.connector_mounted) {
            continue;
        }
        esp_err_t err = ESP_OK;
#if CONFIG_HEAPWATCH_ENABLE_SD_CARD_STATS
        if (vol.external) {
            Internal::unmountConnectorSdCard(vol.mount_path);
            ESP_LOGI("HwFs", "Unmounted connector volume '%s'", vol.label);
            continue;
        }
#endif
        switch (vol.kind) {
            case Kind::KIND_SPIFFS:
                err = esp_vfs_spiffs_unregister(vol.label);
                break;
            case Kind::KIND_LITTLEFS:
                err = esp_vfs_littlefs_unregister(vol.label);
                break;
            case Kind::KIND_FAT:
                if (vol.fat_wl != WL_INVALID_HANDLE) {
                    err = esp_vfs_fat_spiflash_unmount_rw_wl(vol.mount_path, vol.fat_wl);
                } else {
                    ESP_LOGW("HwFs", "Skipping FatFS unmount of '%s' (no wear-levelling handle)",
                             vol.label);
                    err = ESP_ERR_INVALID_STATE;
                }
                break;
        }
        if (err != ESP_OK) {
            ESP_LOGW("HwFs", "Unmount '%s' failed: %s", vol.label, esp_err_to_name(err));
        } else {
            ESP_LOGI("HwFs", "Unmounted connector volume '%s'", vol.label);
        }
    }
    Internal::g_volume_count = 0;
    for (size_t i = 0; i < MAX_VOLUMES; ++i) {
        Internal::g_volumes[i] = {};
    }
    Internal::g_mount_all_done = false;
}

}  // namespace Heapwatch::Fs::Volumes
