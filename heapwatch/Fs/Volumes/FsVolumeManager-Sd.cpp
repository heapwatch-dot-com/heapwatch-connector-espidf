#include "heapwatch/Fs/Volumes/FsVolumeManager.h"
#include "heapwatch/Fs/Volumes/FsVolumeManager-Internal.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdkconfig.h"

#if CONFIG_HEAPWATCH_ENABLE_SD_CARD_STATS
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#endif

#include "heapwatch.h"

namespace Heapwatch::Fs::Volumes {

namespace Internal {

static constexpr char TAG[] = "HwFs";

#if CONFIG_HEAPWATCH_ENABLE_SD_CARD_STATS
constexpr char CONNECTOR_SD_LABEL[] = "sd";
constexpr char CONNECTOR_SD_MOUNT_PATH[] = "/sd";
sdmmc_card_t* g_connector_sd_card = nullptr;

/**
 * Connector-owned SD mount: only for hosts that do not use the card themselves. Takes over the
 * SPI bus, so it must stay off whenever the host already mounted the card (register it instead).
 */
void mountConnectorSdCard() {
    if (CONFIG_HEAPWATCH_SD_CARD_CS_GPIO < 0 || CONFIG_HEAPWATCH_SD_CARD_MOSI_GPIO < 0 ||
        CONFIG_HEAPWATCH_SD_CARD_MISO_GPIO < 0 || CONFIG_HEAPWATCH_SD_CARD_SCK_GPIO < 0) {
        ESP_LOGI(TAG, "SD take-over enabled but pins unconfigured (-1) -- skipping mount");
        return;
    }
    if (dirExists(CONNECTOR_SD_MOUNT_PATH)) {
        ESP_LOGW(TAG,
                 "'%s' is already mounted -- disable the SD take-over option and call "
                 "heapwatch_register_sd_volume() instead",
                 CONNECTOR_SD_MOUNT_PATH);
        registerAlreadyMountedVolume(Kind::KIND_FAT, CONNECTOR_SD_LABEL, CONNECTOR_SD_MOUNT_PATH, false, WL_INVALID_HANDLE,
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
        ESP_LOGW(TAG, "SD card SPI bus init failed: %s -- no card mounted", esp_err_to_name(err));
        return;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = static_cast<gpio_num_t>(CONFIG_HEAPWATCH_SD_CARD_CS_GPIO);
    slot_cfg.host_id = static_cast<spi_host_device_t>(host.slot);

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;
    mount_cfg.max_files = 4;
    mount_cfg.allocation_unit_size = 16 * 1024;

    err = esp_vfs_fat_sdspi_mount(CONNECTOR_SD_MOUNT_PATH, &host, &slot_cfg, &mount_cfg,
                                  &g_connector_sd_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed (no card present?): %s -- skipping",
                 esp_err_to_name(err));
        spi_bus_free(static_cast<spi_host_device_t>(host.slot));
        g_connector_sd_card = nullptr;
        return;
    }

    registerAlreadyMountedVolume(Kind::KIND_FAT, CONNECTOR_SD_LABEL, CONNECTOR_SD_MOUNT_PATH, true, WL_INVALID_HANDLE, true,
                true);
}

void unmountConnectorSdCard(const char* mount_path) {
    if (g_connector_sd_card == nullptr) {
        return;
    }
    const sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    const esp_err_t err = esp_vfs_fat_sdcard_unmount(mount_path, g_connector_sd_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD card unmount failed: %s", esp_err_to_name(err));
    }
    spi_bus_free(static_cast<spi_host_device_t>(host.slot));
    g_connector_sd_card = nullptr;
}

#endif  // CONFIG_HEAPWATCH_ENABLE_SD_CARD_STATS

void registerAlreadyMountedSdVolumes() {
#if CONFIG_HEAPWATCH_ENABLE_SD_CARD_STATS
    mountConnectorSdCard();
#endif

    for (size_t i = 0; i < MAX_SD_VOLUMES; ++i) {
        const HostMapping& mapping = g_sd_map[i];
        if (!mapping.used) {
            continue;
        }
        if (!dirExists(mapping.mount_path)) {
            ESP_LOGW(TAG, "SD volume '%s' at '%s' is not mounted -- skipping", mapping.label,
                     mapping.mount_path);
            continue;
        }
        registerAlreadyMountedVolume(Kind::KIND_FAT, mapping.label, mapping.mount_path, false, WL_INVALID_HANDLE, true,
                    true);
    }
}

esp_err_t applySdMappingLocked(const HostMapping& mapping) {
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
                     "Cannot move connector-owned volume '%s' from '%s' to '%s'",
                     mapping.label, existing->mount_path, mapping.mount_path);
            return ESP_ERR_HEAPWATCH_INVALID_ARG;
        }
        if (!dirExists(mapping.mount_path)) {
            return ESP_OK;
        }
        snprintf(existing->mount_path, sizeof(existing->mount_path), "%s", mapping.mount_path);
        existing->browsable = true;
        existing->mounted = true;
        existing->external = true;
        return ESP_OK;
    }

    if (!g_mount_all_done) {
        return ESP_OK;
    }
    if (!dirExists(mapping.mount_path)) {
        ESP_LOGI(TAG, "SD volume '%s' at '%s' is not mounted yet — will register when present",
                 mapping.label, mapping.mount_path);
        return ESP_OK;
    }
    registerAlreadyMountedVolume(Kind::KIND_FAT, mapping.label, mapping.mount_path, false, WL_INVALID_HANDLE, true,
                true);
    return ESP_OK;
}

}  // namespace Internal

esp_err_t registerSdVolume(const char* label, const char* mount_path) {
    if (!Internal::validLabel(label) || !Internal::validMountPath(mount_path)) {
        return ESP_ERR_HEAPWATCH_INVALID_ARG;
    }

    Internal::TableLock lock;
    HostMapping* slot = Internal::claimMappingSlot(Internal::g_sd_map, MAX_SD_VOLUMES, label);
    if (slot == nullptr) {
        return ESP_ERR_HEAPWATCH_NO_SPACE;
    }
    slot->used = true;
    Internal::copyLabel(slot->label, sizeof(slot->label), label);
    snprintf(slot->mount_path, sizeof(slot->mount_path), "%s", mount_path);
    return Internal::applySdMappingLocked(*slot);
}

}  // namespace Heapwatch::Fs::Volumes
