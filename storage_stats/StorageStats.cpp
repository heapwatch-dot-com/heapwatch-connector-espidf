#include "StorageStats.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdkconfig.h"

#include "../device_fs/FsVolumeManager.h"
#include "../heapwatch_connector/HeapwatchConnector.h"

#if CONFIG_HEAPWATCH_ENABLE_SD_CARD_STATS
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#endif

namespace heapwatch::storage_stats {

namespace {
constexpr char kTag[] = "HwStorage";

#if CONFIG_HEAPWATCH_ENABLE_SD_CARD_STATS
constexpr char kSdMountPoint[] = "/sd";
sdmmc_card_t* g_sd_card = nullptr;
bool g_sd_mounted = false;
#endif

#if CONFIG_HEAPWATCH_ENABLE_SD_CARD_STATS
bool mountSdCardIfConfigured() {
    if (CONFIG_HEAPWATCH_SD_CARD_CS_GPIO < 0 || CONFIG_HEAPWATCH_SD_CARD_MOSI_GPIO < 0 ||
        CONFIG_HEAPWATCH_SD_CARD_MISO_GPIO < 0 || CONFIG_HEAPWATCH_SD_CARD_SCK_GPIO < 0) {
        ESP_LOGI(kTag, "SD card stats enabled but pins unconfigured (-1) -- skipping mount");
        return false;
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
        return false;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = static_cast<gpio_num_t>(CONFIG_HEAPWATCH_SD_CARD_CS_GPIO);
    slot_cfg.host_id = static_cast<spi_host_device_t>(host.slot);

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;
    mount_cfg.max_files = 4;
    mount_cfg.allocation_unit_size = 16 * 1024;

    err = esp_vfs_fat_sdspi_mount(kSdMountPoint, &host, &slot_cfg, &mount_cfg, &g_sd_card);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "SD card mount failed (no card present?): %s -- skipping",
                 esp_err_to_name(err));
        spi_bus_free(static_cast<spi_host_device_t>(host.slot));
        g_sd_card = nullptr;
        return false;
    }

    ESP_LOGI(kTag, "SD card mounted at '%s'", kSdMountPoint);
    return true;
}
#endif  // CONFIG_HEAPWATCH_ENABLE_SD_CARD_STATS

}  // namespace

void init() {
    heapwatch::fs_volumes::mountAll();
#if CONFIG_HEAPWATCH_ENABLE_SD_CARD_STATS
    g_sd_mounted = mountSdCardIfConfigured();
#endif
}

void collectAndReport() {
#if CONFIG_HEAPWATCH_ENABLE_STORAGE_STATS
    bool have_any = false;
    uint64_t total_free_bytes = 0;
    uint64_t total_used_bytes = 0;
    uint64_t mount_total_bytes = 0;

    for (size_t i = 0; i < heapwatch::fs_volumes::mountedCount(); ++i) {
        const auto* vol = heapwatch::fs_volumes::volumeAt(i);
        if (vol == nullptr || !vol->mounted) {
            continue;
        }
        size_t total_bytes = 0;
        size_t used_bytes = 0;
        if (!heapwatch::fs_volumes::queryUsage(*vol, &total_bytes, &used_bytes)) {
            continue;
        }
        total_used_bytes += used_bytes;
        total_free_bytes += (total_bytes >= used_bytes) ? (total_bytes - used_bytes) : 0;
        mount_total_bytes += total_bytes;
        have_any = true;
    }

#if CONFIG_HEAPWATCH_ENABLE_SD_CARD_STATS
    if (g_sd_mounted) {
        uint64_t sd_total_bytes = 0;
        uint64_t sd_free_bytes = 0;
        if (esp_vfs_fat_info(kSdMountPoint, &sd_total_bytes, &sd_free_bytes) == ESP_OK) {
            total_free_bytes += sd_free_bytes;
            total_used_bytes +=
                (sd_total_bytes >= sd_free_bytes) ? (sd_total_bytes - sd_free_bytes) : 0;
            mount_total_bytes += sd_total_bytes;
            have_any = true;
        }
    }
#endif

    if (!have_any) {
        return;
    }

    auto& connector = HeapwatchConnector::getInstance();
    connector.updateParameterInt("storage.free_bytes", static_cast<int64_t>(total_free_bytes));
    connector.updateParameterInt("storage.used_bytes", static_cast<int64_t>(total_used_bytes));
    if (mount_total_bytes > 0) {
        connector.setParameterIntMin("storage.free_bytes", 0);
        connector.setParameterIntMax("storage.free_bytes", static_cast<int64_t>(mount_total_bytes));
        connector.setParameterIntMin("storage.used_bytes", 0);
        connector.setParameterIntMax("storage.used_bytes", static_cast<int64_t>(mount_total_bytes));
    }
#endif  // CONFIG_HEAPWATCH_ENABLE_STORAGE_STATS
}

}  // namespace heapwatch::storage_stats
