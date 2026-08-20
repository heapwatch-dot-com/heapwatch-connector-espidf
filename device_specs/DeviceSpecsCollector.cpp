#include "DeviceSpecsCollector.h"

#include <cstdio>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "heapwatch.h"

namespace heapwatch::device_specs {

namespace {
const char* chip_model_name(esp_chip_model_t model) {
    switch (model) {
        case CHIP_ESP32: return "ESP32";
        case CHIP_ESP32S2: return "ESP32-S2";
        case CHIP_ESP32S3: return "ESP32-S3";
        case CHIP_ESP32C3: return "ESP32-C3";
        case CHIP_ESP32C2: return "ESP32-C2";
        case CHIP_ESP32C6: return "ESP32-C6";
        case CHIP_ESP32H2: return "ESP32-H2";
        case CHIP_ESP32P4: return "ESP32-P4";
        default: return "UNKNOWN";
    }
}

const char* reset_reason_name(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_UNKNOWN: return "unknown";
        case ESP_RST_POWERON: return "power_on";
        case ESP_RST_EXT: return "external";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "int_wdt";
        case ESP_RST_TASK_WDT: return "task_wdt";
        case ESP_RST_WDT: return "wdt";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "sdio";
        default: return "unknown";
    }
}

void append_chip_features(char* output, size_t output_len, uint32_t features) {
    size_t offset = strlen(output);
    bool first = true;

    struct FeatureFlag {
        uint32_t flag;
        const char* name;
    };

    static const FeatureFlag flags[] = {
        {CHIP_FEATURE_EMB_FLASH, "embed_flash"},
        {CHIP_FEATURE_WIFI_BGN, "wifi"},
        {CHIP_FEATURE_BLE, "ble"},
        {CHIP_FEATURE_BT, "bt"},
        {CHIP_FEATURE_IEEE802154, "ieee802154"},
        {CHIP_FEATURE_EMB_PSRAM, "embed_psram"},
    };

    offset += snprintf(output + offset, output_len - offset, ",\"chip_features\":[");
    for (const FeatureFlag& feature : flags) {
        if ((features & feature.flag) == 0) {
            continue;
        }

        if (!first) {
            offset += snprintf(output + offset, output_len - offset, ",");
        }
        offset += snprintf(output + offset, output_len - offset, "\"%s\"", feature.name);
        first = false;
    }
    snprintf(output + offset, output_len - offset, "]");
}

void append_mac_field(char* output, size_t output_len, const char* field_name, esp_mac_type_t type) {
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, type) != ESP_OK) {
        return;
    }

    char mac_string[32] = {};
    snprintf(mac_string, sizeof(mac_string), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
    const size_t offset = strlen(output);
    snprintf(output + offset, output_len - offset, ",\"%s\":\"%s\"", field_name, mac_string);
}

void append_ble_specs(char* output, size_t output_len) {
    uint8_t mac[6] = {};
    char mac_string[32] = {};
    if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
        snprintf(mac_string, sizeof(mac_string), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
                 mac[2], mac[3], mac[4], mac[5]);
    }

    const size_t offset = strlen(output);
#if CONFIG_HEAPWATCH_ENABLE_BLE_REPORTING && CONFIG_BT_ENABLED
    snprintf(output + offset, output_len - offset,
             ",\"ble\":{\"mac\":\"%s\"},\"bt\":{\"enabled\":true}", mac_string);
#else
    snprintf(output + offset, output_len - offset,
             ",\"ble\":{\"mac\":\"%s\"},\"bt\":{\"enabled\":false}", mac_string);
#endif
}

const char* partition_subtype_name(esp_partition_type_t type, esp_partition_subtype_t subtype) {
    if (type == ESP_PARTITION_TYPE_APP) {
        switch (subtype) {
            case ESP_PARTITION_SUBTYPE_APP_FACTORY: return "factory";
            case ESP_PARTITION_SUBTYPE_APP_OTA_0: return "ota_0";
            case ESP_PARTITION_SUBTYPE_APP_OTA_1: return "ota_1";
            case ESP_PARTITION_SUBTYPE_APP_OTA_2: return "ota_2";
            case ESP_PARTITION_SUBTYPE_APP_OTA_3: return "ota_3";
            case ESP_PARTITION_SUBTYPE_APP_OTA_4: return "ota_4";
            case ESP_PARTITION_SUBTYPE_APP_OTA_5: return "ota_5";
            case ESP_PARTITION_SUBTYPE_APP_TEST: return "test";
            default: return "app";
        }
    }

    if (type == ESP_PARTITION_TYPE_DATA) {
        switch (subtype) {
            case ESP_PARTITION_SUBTYPE_DATA_OTA: return "ota";
            case ESP_PARTITION_SUBTYPE_DATA_NVS: return "nvs";
            case ESP_PARTITION_SUBTYPE_DATA_PHY: return "phy";
            case ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS: return "nvs_keys";
            case ESP_PARTITION_SUBTYPE_DATA_EFUSE_EM: return "efuse_em";
            case ESP_PARTITION_SUBTYPE_DATA_UNDEFINED: return "undefined";
            case ESP_PARTITION_SUBTYPE_DATA_ESPHTTPD: return "esphttpd";
            case ESP_PARTITION_SUBTYPE_DATA_FAT: return "fat";
            case ESP_PARTITION_SUBTYPE_DATA_SPIFFS: return "spiffs";
            case ESP_PARTITION_SUBTYPE_DATA_COREDUMP: return "coredump";
#ifdef ESP_PARTITION_SUBTYPE_DATA_LITTLEFS
            case ESP_PARTITION_SUBTYPE_DATA_LITTLEFS: return "littlefs";
#endif
            default: return "data";
        }
    }

    return "unknown";
}

const char* partition_type_name(esp_partition_type_t type) {
    switch (type) {
        case ESP_PARTITION_TYPE_APP: return "app";
        case ESP_PARTITION_TYPE_DATA: return "data";
        default: return "unknown";
    }
}

/**
 * Partition-agnostic OTA capability: discover layout from the running table.
 * Reports has_ota_partitions + ota_app_slot_bytes + a CSV dump of what is actually
 * on flash (for backend layout compare). Never hardcodes a client partition map.
 */
bool append_ota_capability(char* output, size_t output_len) {
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    bool has_ota = next != nullptr;
    size_t slot_bytes = next != nullptr ? next->size : 0;

    // Prefer the smallest OTA app slot as the fit gate (any slot we might write into).
    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    while (it != nullptr) {
        const esp_partition_t* part = esp_partition_get(it);
        if (part != nullptr && part->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
            part->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
            has_ota = true;
            if (slot_bytes == 0 || part->size < slot_bytes) {
                slot_bytes = part->size;
            }
        }
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);

    size_t offset = strlen(output);
    offset += snprintf(output + offset, output_len - offset,
                       ",\"has_ota_partitions\":%s,\"ota_app_slot_bytes\":%lu,\"partitions_csv\":\"",
                       has_ota ? "true" : "false", static_cast<unsigned long>(slot_bytes));

    bool first = true;
    it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    while (it != nullptr) {
        const esp_partition_t* part = esp_partition_get(it);
        if (part != nullptr) {
            if (!first) {
                offset += snprintf(output + offset, output_len - offset, "\\n");
            }
            first = false;
            offset += snprintf(output + offset, output_len - offset, "%s, %s, %s, 0x%lx, 0x%lx,",
                               part->label, partition_type_name(part->type),
                               partition_subtype_name(part->type, part->subtype),
                               static_cast<unsigned long>(part->address),
                               static_cast<unsigned long>(part->size));
            if (offset + 8 >= output_len) {
                esp_partition_iterator_release(it);
                return false;
            }
        }
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);

    offset += snprintf(output + offset, output_len - offset, "\"");
    if (offset >= output_len) {
        return false;
    }

    // Bootloader-app-rollback state (independent of CONFIG_HEAPWATCH_ENABLE_OTA).
    bool pending_confirm = false;
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running != nullptr) {
        esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
        if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
            pending_confirm = (state == ESP_OTA_IMG_PENDING_VERIFY);
        }
    }

    bool rollback_capable = false;
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    rollback_capable = true;
#endif

    offset += snprintf(output + offset, output_len - offset,
                       ",\"ota_pending_confirm\":%s,\"ota_rollback_capable\":%s",
                       pending_confirm ? "true" : "false",
                       rollback_capable ? "true" : "false");
    return offset < output_len;
}
}  // namespace

bool buildPayload(char* output, size_t output_len) {
    if (output == nullptr || output_len == 0) {
        return false;
    }

    esp_chip_info_t chip_info = {};
    esp_chip_info(&chip_info);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(nullptr, &flash_size) != ESP_OK) {
        flash_size = 0;
    }

    const size_t internal_heap_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const size_t heap_total = internal_heap_total + psram_total;
    const size_t min_free_heap = esp_get_minimum_free_heap_size();

    const esp_app_desc_t* app_desc = esp_app_get_description();
    const esp_partition_t* running_partition = esp_ota_get_running_partition();
    uint8_t elf_sha256[32] = {};
    char elf_sha256_hex[65] = {};
    if (running_partition != nullptr &&
        esp_partition_get_sha256(running_partition, elf_sha256) == ESP_OK) {
        for (size_t i = 0; i < sizeof(elf_sha256); ++i) {
            snprintf(elf_sha256_hex + (i * 2), sizeof(elf_sha256_hex) - (i * 2), "%02x", elf_sha256[i]);
        }
    }

    char build_date[64] = {};
    if (app_desc != nullptr) {
        snprintf(build_date, sizeof(build_date), "%s %s", app_desc->date, app_desc->time);
    }

    const int written = snprintf(
        output,
        output_len,
        "{\"idf_target\":\"%s\",\"chip_model\":\"%s\",\"chip_revision\":%u,\"chip_cores\":%u,"
        "\"flash_size_bytes\":%lu,\"psram_size_bytes\":%lu,\"heap_total_bytes\":%lu,"
        "\"heap_internal_total_bytes\":%lu,\"min_free_heap_bytes\":%lu,"
        "\"sdk_version\":\"%s\",\"app_version\":\"%s\",\"connector_source_version\":\"%s\","
        "\"build_identifier\":\"%s\","
        "\"build_date\":\"%s\",\"cpu_freq_mhz\":%u,\"reset_reason\":\"%s\","
        "\"secure_boot_enabled\":%s,\"flash_encryption_enabled\":%s",
        CONFIG_IDF_TARGET,
        chip_model_name(chip_info.model),
        chip_info.revision,
        chip_info.cores,
        static_cast<unsigned long>(flash_size),
        static_cast<unsigned long>(psram_total),
        static_cast<unsigned long>(heap_total),
        static_cast<unsigned long>(internal_heap_total),
        static_cast<unsigned long>(min_free_heap),
        esp_get_idf_version(),
        (heapwatch_get_app_source_version()[0] != '\0')
            ? heapwatch_get_app_source_version()
            : (app_desc != nullptr ? app_desc->version : ""),
        heapwatch_get_connector_source_version(),
        heapwatch_get_build_identifier(),
        build_date,
        CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        reset_reason_name(esp_reset_reason()),
#if CONFIG_SECURE_BOOT
        "true",
#else
        "false",
#endif
#if CONFIG_SECURE_FLASH_ENC_ENABLED
        "true"
#else
        "false"
#endif
    );

    if (written < 0 || static_cast<size_t>(written) >= output_len) {
        return false;
    }

    append_chip_features(output, output_len, chip_info.features);
    append_mac_field(output, output_len, "mac_wifi_sta", ESP_MAC_WIFI_STA);
    append_ble_specs(output, output_len);

    size_t offset = strlen(output);
    if (elf_sha256_hex[0] != '\0') {
        offset += snprintf(output + offset, output_len - offset, ",\"app_elf_sha256\":\"%s\"",
                           elf_sha256_hex);
    }

    if (!append_ota_capability(output, output_len)) {
        return false;
    }

    const size_t closing_offset = strlen(output);
    if (closing_offset + 2 >= output_len) {
        return false;
    }

    output[closing_offset] = '}';
    output[closing_offset + 1] = '\0';
    return true;
}

}  // namespace heapwatch::device_specs
