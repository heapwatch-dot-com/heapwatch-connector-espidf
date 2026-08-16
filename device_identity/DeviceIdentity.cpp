#include "DeviceIdentity.h"

#include <cstdio>

#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_mac.h"
#include "esp_system.h"

namespace heapwatch::device_identity {

namespace {

bool is_all_zero(const uint8_t* bytes, size_t len) {
    for (size_t index = 0; index < len; index += 1) {
        if (bytes[index] != 0) {
            return false;
        }
    }

    return true;
}

void format_hex_lower(const uint8_t* bytes, size_t byte_count, char* out, size_t out_len) {
    if (out == nullptr || out_len == 0 || byte_count == 0) {
        return;
    }

    const size_t required = byte_count * 2U + 1U;
    if (out_len < required) {
        out[0] = '\0';
        return;
    }

    for (size_t index = 0; index < byte_count; index += 1) {
        std::snprintf(out + (index * 2U), 3, "%02x", bytes[index]);
    }
    out[byte_count * 2U] = '\0';
}

}  // namespace

void getClaimedMac(char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return;
    }

    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    std::snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
                  mac[4], mac[5]);
}

void getClaimedUid128(char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return;
    }

    out[0] = '\0';

#if !CONFIG_IDF_TARGET_ESP32
    uint8_t uid[16] = {};
    if (esp_efuse_read_field_blob(ESP_EFUSE_OPTIONAL_UNIQUE_ID, uid, 128) != ESP_OK) {
        return;
    }

    if (is_all_zero(uid, sizeof(uid))) {
        return;
    }

    format_hex_lower(uid, sizeof(uid), out, out_len);
#endif
}

void getMacUid(char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return;
    }

    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    std::snprintf(out, out_len, "esp-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3],
                  mac[4], mac[5]);
}

void getChipId(char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return;
    }

    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    std::snprintf(out, out_len, "chip-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3],
                  mac[4], mac[5]);
}

}  // namespace heapwatch::device_identity
