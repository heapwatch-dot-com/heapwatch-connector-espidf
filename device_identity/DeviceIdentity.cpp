#include "DeviceIdentity.h"

#include <cstdio>

#include "esp_mac.h"
#include "esp_system.h"

namespace heapwatch::device_identity {

void getClaimedMac(char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return;
    }

    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    std::snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
                  mac[4], mac[5]);
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
