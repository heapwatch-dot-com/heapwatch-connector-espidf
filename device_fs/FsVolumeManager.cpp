#include "FsVolumeManager.h"

#include <cstdio>
#include <cstring>
#include <dirent.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "esp_vfs_fat.h"
#include "sdkconfig.h"

#if CONFIG_HEAPWATCH_ENABLE_SD_CARD_STATS
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#endif

#include "HeapwatchFsPartition.h"
#include "heapwatch.h"
#include "heapwatch_connector/ConnectorLifecycle.h"

namespace heapwatch::fs_volumes {

namespace {

constexpr char kTag[] = "HwFs";

struct HostMapping {
    char label[kLabelMax] = {};
    char mount_path[kMountPathMax] = {};
    bool used = false;
};

HostMapping g_host_map[kMaxFlashVolumes] = {};
HostMapping g_sd_map[kMaxSdVolumes] = {};
Volume g_volumes[kMaxVolumes] = {};
size_t g_volume_count = 0;

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
    return strlen(path) < kMountPathMax;
}

bool validLabel(const char* label) {
    return label != nullptr && label[0] != '\0' && strlen(label) < kLabelMax;
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
    if (label == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < kMaxFlashVolumes; ++i) {
        if (g_host_map[i].used && strcmp(g_host_map[i].label, label) == 0) {
            return &g_host_map[i];
        }
    }
    return nullptr;
}

/** Reserve or reuse a slot for `label`; nullptr when the table is full. */
HostMapping* claimMappingSlot(HostMapping* table, size_t table_len, const char* label) {
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

void defaultMountPath(const char* label, char* out, size_t out_len) {
    snprintf(out, out_len, "/%s", label);
}

bool formatIfFailed() {
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
        *kind = Kind::Spiffs;
        return true;
    }
    if (heapwatchIsLittlefsSubtype(part->subtype)) {
        *kind = Kind::Littlefs;
        return true;
    }
    if (part->subtype == ESP_PARTITION_SUBTYPE_DATA_FAT) {
        *kind = Kind::Fat;
        return true;
    }
    return false;
}

bool driverSaysMounted(Kind kind, const char* label) {
    size_t total = 0;
    size_t used = 0;
    switch (kind) {
        case Kind::Spiffs:
            return esp_spiffs_info(label, &total, &used) == ESP_OK;
        case Kind::Littlefs:
            return esp_littlefs_info(label, &total, &used) == ESP_OK;
        case Kind::Fat:
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
    conf.format_if_mount_failed = formatIfFailed();
    const esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_OK) {
        return MountOutcome::ConnectorOwned;
    }
    if (err == ESP_ERR_INVALID_STATE && dirExists(mount_path)) {
        return MountOutcome::HostOwned;
    }
    ESP_LOGW(kTag, "SPIFFS mount '%s' at '%s' failed: %s", label, mount_path, esp_err_to_name(err));
    return MountOutcome::Failed;
}

MountOutcome mountLittlefs(const char* label, const char* mount_path) {
    const esp_partition_t* part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = mount_path;
    conf.partition_label = label;
    conf.partition = part;
    conf.format_if_mount_failed = formatIfFailed();
    conf.dont_mount = false;
    const esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err == ESP_OK) {
        return MountOutcome::ConnectorOwned;
    }
    if (err == ESP_ERR_INVALID_STATE && dirExists(mount_path)) {
        return MountOutcome::HostOwned;
    }
    ESP_LOGW(kTag, "LittleFS mount '%s' at '%s' failed: %s", label, mount_path, esp_err_to_name(err));
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
    cfg.format_if_mount_failed = formatIfFailed();
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
    ESP_LOGW(kTag, "FatFS mount '%s' at '%s' failed: %s", volume->label, volume->mount_path,
             esp_err_to_name(err));
    return MountOutcome::Failed;
}

MountOutcome doMount(Volume* volume) {
    if (volume == nullptr) {
        return MountOutcome::Failed;
    }
    switch (volume->kind) {
        case Kind::Spiffs:
            return mountSpiffs(volume->label, volume->mount_path);
        case Kind::Littlefs:
            return mountLittlefs(volume->label, volume->mount_path);
        case Kind::Fat:
            return mountFat(volume);
    }
    return MountOutcome::Failed;
}

void adoptVolume(Kind kind, const char* label, const char* mount_path, bool connector_mounted,
                 wl_handle_t fat_wl, bool browsable, bool external = false) {
    if (g_volume_count >= kMaxVolumes) {
        ESP_LOGW(kTag, "FS volume table full; skipping '%s'", label);
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
    ESP_LOGI(kTag, "%s volume '%s' at '%s' (%s)", kindName(kind), label, mount_path,
             connector_mounted ? "connector-mounted" : "host-owned");
}

#if CONFIG_HEAPWATCH_ENABLE_SD_CARD_STATS
constexpr char kConnectorSdLabel[] = "sd";
constexpr char kConnectorSdMountPath[] = "/sd";
sdmmc_card_t* g_connector_sd_card = nullptr;

/**
 * Connector-owned SD mount: only for hosts that do not use the card themselves. Takes over the
 * SPI bus, so it must stay off whenever the host already mounted the card (register it instead).
 */
void mountConnectorSdCard() {
    if (CONFIG_HEAPWATCH_SD_CARD_CS_GPIO < 0 || CONFIG_HEAPWATCH_SD_CARD_MOSI_GPIO < 0 ||
        CONFIG_HEAPWATCH_SD_CARD_MISO_GPIO < 0 || CONFIG_HEAPWATCH_SD_CARD_SCK_GPIO < 0) {
        ESP_LOGI(kTag, "SD take-over enabled but pins unconfigured (-1) -- skipping mount");
        return;
    }
    if (dirExists(kConnectorSdMountPath)) {
        ESP_LOGW(kTag,
                 "'%s' is already mounted -- disable the SD take-over option and call "
                 "heapwatch_register_sd_volume() instead",
                 kConnectorSdMountPath);
        adoptVolume(Kind::Fat, kConnectorSdLabel, kConnectorSdMountPath, false, WL_INVALID_HANDLE,
                    true, true);
        return;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = CONFIG_HEAPWATCH_SD_CARD_MOSI_GPIO;
    bus_cfg.miso_io_num = CONFIG_HEAPWATCH_SD_CARD_MISO_GPIO;
    bus_cfg.sclk_io_num = CONFIG_HEAPWATCH_SD_CARD_SCK_GPIO;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;

    esp_err_t err = spi_bus_initialize(static_cast<spi_host_device_t>(host.slot), &bus_cfg,
                                       SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "SD card SPI bus init failed: %s -- no card mounted", esp_err_to_name(err));
        return;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = static_cast<gpio_num_t>(CONFIG_HEAPWATCH_SD_CARD_CS_GPIO);
    slot_cfg.host_id = static_cast<spi_host_device_t>(host.slot);

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;
    mount_cfg.max_files = 4;
    mount_cfg.allocation_unit_size = 16 * 1024;

    err = esp_vfs_fat_sdspi_mount(kConnectorSdMountPath, &host, &slot_cfg, &mount_cfg,
                                  &g_connector_sd_card);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "SD card mount failed (no card present?): %s -- skipping",
                 esp_err_to_name(err));
        spi_bus_free(static_cast<spi_host_device_t>(host.slot));
        g_connector_sd_card = nullptr;
        return;
    }

    adoptVolume(Kind::Fat, kConnectorSdLabel, kConnectorSdMountPath, true, WL_INVALID_HANDLE, true,
                true);
}

void unmountConnectorSdCard(const char* mount_path) {
    if (g_connector_sd_card == nullptr) {
        return;
    }
    const sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    const esp_err_t err = esp_vfs_fat_sdcard_unmount(mount_path, g_connector_sd_card);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "SD card unmount failed: %s", esp_err_to_name(err));
    }
    spi_bus_free(static_cast<spi_host_device_t>(host.slot));
    g_connector_sd_card = nullptr;
}
#endif  // CONFIG_HEAPWATCH_ENABLE_SD_CARD_STATS

/** Adopt host-mounted SD cards. They are never discovered — only registered. */
void adoptSdVolumes() {
#if CONFIG_HEAPWATCH_ENABLE_SD_CARD_STATS
    mountConnectorSdCard();
#endif

    for (size_t i = 0; i < kMaxSdVolumes; ++i) {
        const HostMapping& mapping = g_sd_map[i];
        if (!mapping.used) {
            continue;
        }
        if (!dirExists(mapping.mount_path)) {
            ESP_LOGW(kTag, "SD volume '%s' at '%s' is not mounted -- skipping", mapping.label,
                     mapping.mount_path);
            continue;
        }
        adoptVolume(Kind::Fat, mapping.label, mapping.mount_path, false, WL_INVALID_HANDLE, true,
                    true);
    }
}

void processPartition(const esp_partition_t* part) {
    Kind kind = Kind::Spiffs;
    if (!kindFromPartition(part, &kind)) {
        return;
    }
    const char* label = part->label[0] != '\0' ? part->label : nullptr;
    if (!validLabel(label)) {
        return;
    }

    char intended[kMountPathMax] = {};
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
            adoptVolume(kind, label, intended, false, WL_INVALID_HANDLE, true);
        } else {
            ESP_LOGW(kTag, "Registered volume '%s' at '%s' is not mounted", label, intended);
        }
        return;
    }

    const bool already = driverSaysMounted(kind, label) || dirExists(intended);
    if (already) {
        const bool browsable = dirExists(intended);
        if (!browsable) {
            ESP_LOGW(kTag,
                     "Partition '%s' is mounted by the host but '%s' is not a VFS root — "
                     "call heapwatch_register_fs_volume() with the real mount path",
                     label, intended);
        }
        adoptVolume(kind, label, intended, false, WL_INVALID_HANDLE, browsable);
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
    adoptVolume(kind, label, intended, outcome == MountOutcome::ConnectorOwned, pending.fat_wl,
               dirExists(intended));
}

}  // namespace

esp_err_t registerVolume(const char* partition_label, const char* mount_path) {
    if (heapwatch::connector_lifecycle::initializeCalled()) {
        return ESP_ERR_HEAPWATCH_ALREADY_INITIALIZED;
    }
    if (!validLabel(partition_label) || !validMountPath(mount_path)) {
        return ESP_ERR_HEAPWATCH_INVALID_ARG;
    }

    HostMapping* slot = claimMappingSlot(g_host_map, kMaxFlashVolumes, partition_label);
    if (slot == nullptr) {
        return ESP_ERR_HEAPWATCH_NO_SPACE;
    }
    slot->used = true;
    copyLabel(slot->label, sizeof(slot->label), partition_label);
    snprintf(slot->mount_path, sizeof(slot->mount_path), "%s", mount_path);
    return ESP_OK;
}

esp_err_t registerSdVolume(const char* label, const char* mount_path) {
    if (heapwatch::connector_lifecycle::initializeCalled()) {
        return ESP_ERR_HEAPWATCH_ALREADY_INITIALIZED;
    }
    if (!validLabel(label) || !validMountPath(mount_path)) {
        return ESP_ERR_HEAPWATCH_INVALID_ARG;
    }

    HostMapping* slot = claimMappingSlot(g_sd_map, kMaxSdVolumes, label);
    if (slot == nullptr) {
        return ESP_ERR_HEAPWATCH_NO_SPACE;
    }
    slot->used = true;
    copyLabel(slot->label, sizeof(slot->label), label);
    snprintf(slot->mount_path, sizeof(slot->mount_path), "%s", mount_path);
    return ESP_OK;
}

void mountAll() {
    g_volume_count = 0;
    for (size_t i = 0; i < kMaxVolumes; ++i) {
        g_volumes[i] = {};
    }

    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    while (it != nullptr) {
        const esp_partition_t* part = esp_partition_get(it);
        processPartition(part);
        it = esp_partition_next(it);
    }

    adoptSdVolumes();
}

void unmountConnectorOwned() {
    for (size_t i = 0; i < g_volume_count; ++i) {
        Volume& vol = g_volumes[i];
        if (!vol.connector_mounted) {
            continue;
        }
        esp_err_t err = ESP_OK;
#if CONFIG_HEAPWATCH_ENABLE_SD_CARD_STATS
        if (vol.external) {
            unmountConnectorSdCard(vol.mount_path);
            ESP_LOGI(kTag, "Unmounted connector volume '%s'", vol.label);
            continue;
        }
#endif
        switch (vol.kind) {
            case Kind::Spiffs:
                err = esp_vfs_spiffs_unregister(vol.label);
                break;
            case Kind::Littlefs:
                err = esp_vfs_littlefs_unregister(vol.label);
                break;
            case Kind::Fat:
                if (vol.fat_wl != WL_INVALID_HANDLE) {
                    err = esp_vfs_fat_spiflash_unmount_rw_wl(vol.mount_path, vol.fat_wl);
                } else {
                    ESP_LOGW(kTag, "Skipping FatFS unmount of '%s' (no wear-levelling handle)",
                             vol.label);
                    err = ESP_ERR_INVALID_STATE;
                }
                break;
        }
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "Unmount '%s' failed: %s", vol.label, esp_err_to_name(err));
        } else {
            ESP_LOGI(kTag, "Unmounted connector volume '%s'", vol.label);
        }
    }
    g_volume_count = 0;
    for (size_t i = 0; i < kMaxVolumes; ++i) {
        g_volumes[i] = {};
    }
}

size_t mountedCount() {
    return g_volume_count;
}

const Volume* volumeAt(size_t index) {
    if (index >= g_volume_count) {
        return nullptr;
    }
    return &g_volumes[index];
}

const Volume* findByPath(const char* path) {
    const Volume* best = nullptr;
    size_t best_len = 0;
    for (size_t i = 0; i < g_volume_count; ++i) {
        const Volume& vol = g_volumes[i];
        if (!vol.browsable) {
            continue;
        }
        if (!pathStartsWithMount(path, vol.mount_path)) {
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
    if (volume.kind == Kind::Fat) {
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
        case Kind::Spiffs:
            err = esp_spiffs_info(label, &total, &used);
            break;
        case Kind::Littlefs:
            err = esp_littlefs_info(label, &total, &used);
            break;
        case Kind::Fat:
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
        case Kind::Spiffs:
            return "spiffs";
        case Kind::Littlefs:
            return "littlefs";
        case Kind::Fat:
            return "fat";
    }
    return "unknown";
}

cJSON* buildVolumeObject(const Volume& volume) {
    size_t total = 0;
    size_t used = 0;
    if (!queryUsage(volume, &total, &used)) {
        return nullptr;
    }
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
    cJSON_AddNumberToObject(vol, "total_bytes", static_cast<double>(total));
    cJSON_AddNumberToObject(vol, "used_bytes", static_cast<double>(used));
    return vol;
}

}  // namespace heapwatch::fs_volumes
