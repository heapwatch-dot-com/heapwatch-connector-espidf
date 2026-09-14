// Copyright (c) 2026 Heapwatch. All rights reserved.

#include "device_network/DeviceNetworkReporter.h"
#include "device_network/NetifTraffic.h"

#include <cstdarg>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "heapwatch_config.h"
#include "heapwatch_connector/HeapwatchConnector.h"
#include "lwip/ip4_addr.h"
#if CONFIG_LWIP_IPV6
#include "lwip/ip6_addr.h"
#endif

namespace heapwatch::device_network {
namespace {

constexpr const char* TAG = "HwNet";
constexpr size_t MAX_TRACKED_KEYS = 8;
constexpr size_t KEY_LEN = 32;

bool g_started = false;
volatile bool g_topology_dirty = false;
char g_prev_default_key[KEY_LEN] = {};
char g_prev_keys[MAX_TRACKED_KEYS][KEY_LEN] = {};
size_t g_prev_key_count = 0;
bool g_have_prev = false;

void ip4_to_string(const esp_ip4_addr_t& addr, char* out, size_t out_len) {
    snprintf(out, out_len, IPSTR, IP2STR(&addr));
}

bool append(char* out, size_t out_len, size_t* offset, const char* fmt, ...) {
    if (*offset >= out_len) {
        return false;
    }
    va_list args;
    va_start(args, fmt);
    const int32_t written = vsnprintf(out + *offset, out_len - *offset, fmt, args);
    va_end(args);
    if (written < 0 || static_cast<size_t>(written) >= out_len - *offset) {
        return false;
    }
    *offset += static_cast<size_t>(written);
    return true;
}

/** Append a JSON string value (quotes included), escaping control chars. */
bool append_json_string(char* out, size_t out_len, size_t* offset, const char* value) {
    if (value == nullptr) {
        value = "";
    }
    if (!append(out, out_len, offset, "\"")) {
        return false;
    }
    for (const uint8_t* p = reinterpret_cast<const uint8_t*>(value); *p != '\0'; ++p) {
        const uint8_t c = *p;
        if (c == '"' || c == '\\') {
            if (!append(out, out_len, offset, "\\%c", c)) {
                return false;
            }
        } else if (c < 0x20) {
            if (!append(out, out_len, offset, "\\u%04x", c)) {
                return false;
            }
        } else if (!append(out, out_len, offset, "%c", c)) {
            return false;
        }
    }
    return append(out, out_len, offset, "\"");
}

void copy_key(char* dest, size_t dest_len, const char* src) {
    if (src == nullptr) {
        dest[0] = '\0';
        return;
    }
    strncpy(dest, src, dest_len - 1);
    dest[dest_len - 1] = '\0';
}

bool key_in_prev(const char* key) {
    for (size_t i = 0; i < g_prev_key_count; ++i) {
        if (strncmp(g_prev_keys[i], key, KEY_LEN) == 0) {
            return true;
        }
    }
    return false;
}

bool append_netif_object(esp_netif_t* netif, esp_netif_t* default_netif, char* out, size_t out_len,
                         size_t* offset, bool* first) {
    const char* key = esp_netif_get_ifkey(netif);
    const char* desc = esp_netif_get_desc(netif);
    if (key == nullptr) {
        key = "";
    }
    if (desc == nullptr) {
        desc = "";
    }

    const uint32_t flags = static_cast<uint32_t>(esp_netif_get_flags(netif));
    const bool up = esp_netif_is_netif_up(netif);
    const bool is_default = (default_netif != nullptr && netif == default_netif);
    const int32_t route_prio = esp_netif_get_route_prio(netif);

    if (!append(out, out_len, offset, "%s{\"key\":", *first ? "" : ",")) {
        return false;
    }
    if (!append_json_string(out, out_len, offset, key)) {
        return false;
    }
    if (!append(out, out_len, offset, ",\"desc\":")) {
        return false;
    }
    if (!append_json_string(out, out_len, offset, desc)) {
        return false;
    }
    if (!append(out, out_len, offset, ",\"flags\":%" PRIu32 ",\"up\":%s,\"is_default\":%s,\"route_prio\":%" PRId32,
                flags, up ? "true" : "false", is_default ? "true" : "false", route_prio)) {
        return false;
    }
    *first = false;

    uint8_t mac[6] = {};
    if (esp_netif_get_mac(netif, mac) == ESP_OK) {
        if (!append(out, out_len, offset, ",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"", mac[0],
                    mac[1], mac[2], mac[3], mac[4], mac[5])) {
            return false;
        }
    }

    esp_netif_ip_info_t ip_info = {};
    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        char ip[16] = {};
        char mask[16] = {};
        char gw[16] = {};
        ip4_to_string(ip_info.ip, ip, sizeof(ip));
        ip4_to_string(ip_info.netmask, mask, sizeof(mask));
        ip4_to_string(ip_info.gw, gw, sizeof(gw));
        if (!append(out, out_len, offset, ",\"ip4\":{\"ip\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\"}",
                    ip, mask, gw)) {
            return false;
        }
    }

#if CONFIG_LWIP_IPV6
    esp_ip6_addr_t ip6[3];
    int32_t ip6_count = esp_netif_get_all_ip6(netif, ip6);
    if (ip6_count > 3) {
        ip6_count = 3;
    }
    if (ip6_count > 0) {
        if (!append(out, out_len, offset, ",\"ip6\":[")) {
            return false;
        }
        for (int32_t i = 0; i < ip6_count; ++i) {
            char ip6_str[48] = {};
            ip6addr_ntoa_r(reinterpret_cast<const ip6_addr_t*>(&ip6[i]), ip6_str, sizeof(ip6_str));
            if (!append(out, out_len, offset, "%s\"%s\"", i == 0 ? "" : ",", ip6_str)) {
                return false;
            }
        }
        if (!append(out, out_len, offset, "]")) {
            return false;
        }
    }
#endif

    esp_netif_dns_info_t dns = {};
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        char dns_str[40] = {};
        if (dns.ip.type == ESP_IPADDR_TYPE_V4 && dns.ip.u_addr.ip4.addr != 0) {
            ip4_to_string(dns.ip.u_addr.ip4, dns_str, sizeof(dns_str));
            if (!append(out, out_len, offset, ",\"dns\":[\"%s\"]", dns_str)) {
                return false;
            }
        }
    }

    // Shallow WiFi STA extras when this looks like the default STA netif.
    if (strstr(key, "STA") != nullptr || strcmp(desc, "sta") == 0) {
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            if (!append(out, out_len, offset, ",\"wifi\":{\"ssid\":")) {
                return false;
            }
            if (!append_json_string(out, out_len, offset, reinterpret_cast<const char*>(ap.ssid))) {
                return false;
            }
            if (!append(out, out_len, offset, ",\"rssi_dbm\":%" PRId32 ",\"channel\":%" PRIu32 "}",
                        static_cast<int32_t>(ap.rssi), static_cast<uint32_t>(ap.primary))) {
                return false;
            }
        }
    }

    if (!heapwatch::netif_traffic::appendRxTxJsonFields(out, out_len, offset, key)) {
        return false;
    }

    return append(out, out_len, offset, "}");
}

void emit_events_for_scan(esp_netif_t* default_netif) {
    char current_keys[MAX_TRACKED_KEYS][KEY_LEN] = {};
    size_t current_count = 0;

    // Use locked esp_netif_next — next_unsafe from MQTT/event tasks can see an empty list.
    for (esp_netif_t* netif = esp_netif_next(nullptr); netif != nullptr;
         netif = esp_netif_next(netif)) {
        const char* key = esp_netif_get_ifkey(netif);
        if (key == nullptr || key[0] == '\0' || current_count >= MAX_TRACKED_KEYS) {
            continue;
        }
        copy_key(current_keys[current_count], KEY_LEN, key);
        if (g_have_prev && !key_in_prev(key) && esp_netif_is_netif_up(netif)) {
            HeapwatchConnector::getInstance().queueEvent(HEAPWATCH_EVENT_NETIF_UP);
        }
        current_count++;
    }

    if (g_have_prev) {
        for (size_t i = 0; i < g_prev_key_count; ++i) {
            bool still_present = false;
            for (size_t j = 0; j < current_count; ++j) {
                if (strncmp(g_prev_keys[i], current_keys[j], KEY_LEN) == 0) {
                    still_present = true;
                    break;
                }
            }
            if (!still_present) {
                HeapwatchConnector::getInstance().queueEvent(HEAPWATCH_EVENT_NETIF_DOWN);
            }
        }
    }

    const char* default_key = default_netif != nullptr ? esp_netif_get_ifkey(default_netif) : "";
    if (default_key == nullptr) {
        default_key = "";
    }
    if (g_have_prev && strncmp(g_prev_default_key, default_key, KEY_LEN) != 0) {
        HeapwatchConnector::getInstance().queueEvent(HEAPWATCH_EVENT_DEFAULT_ROUTE_CHANGED);
    }

    g_prev_key_count = current_count;
    for (size_t i = 0; i < current_count; ++i) {
        copy_key(g_prev_keys[i], KEY_LEN, current_keys[i]);
    }
    copy_key(g_prev_default_key, KEY_LEN, default_key);
    g_have_prev = true;
}

void on_ip_or_wifi_event(void* /*arg*/, esp_event_base_t event_base, int32_t event_id,
                         void* /*event_data*/) {
#ifdef IP_EVENT_TX_RX
    if (event_base == IP_EVENT && event_id == IP_EVENT_TX_RX) {
        return;
    }
#endif
    if (event_base == IP_EVENT) {
        switch (event_id) {
            case IP_EVENT_STA_GOT_IP:
#ifdef IP_EVENT_ETH_GOT_IP
            case IP_EVENT_ETH_GOT_IP:
#endif
#ifdef IP_EVENT_PPP_GOT_IP
            case IP_EVENT_PPP_GOT_IP:
#endif
                HeapwatchConnector::getInstance().queueEvent(HEAPWATCH_EVENT_NETIF_GOT_IP);
                break;
            case IP_EVENT_STA_LOST_IP:
#ifdef IP_EVENT_ETH_LOST_IP
            case IP_EVENT_ETH_LOST_IP:
#endif
#ifdef IP_EVENT_PPP_LOST_IP
            case IP_EVENT_PPP_LOST_IP:
#endif
                HeapwatchConnector::getInstance().queueEvent(HEAPWATCH_EVENT_NETIF_LOST_IP);
                break;
#ifdef IP_EVENT_NETIF_UP
            case IP_EVENT_NETIF_UP:
                HeapwatchConnector::getInstance().queueEvent(HEAPWATCH_EVENT_NETIF_UP);
                break;
#endif
#ifdef IP_EVENT_NETIF_DOWN
            case IP_EVENT_NETIF_DOWN:
                HeapwatchConnector::getInstance().queueEvent(HEAPWATCH_EVENT_NETIF_DOWN);
                break;
#endif
            default:
                break;
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        HeapwatchConnector::getInstance().queueEvent(HEAPWATCH_EVENT_NETIF_DOWN);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        HeapwatchConnector::getInstance().queueEvent(HEAPWATCH_EVENT_NETIF_UP);
    }

    markTopologyDirty();
}

}  // namespace

bool buildPayload(char* out, size_t out_len) {
    if (out == nullptr || out_len < 16) {
        return false;
    }

    heapwatch::netif_traffic::ensureTracking();

    size_t offset = 0;
    if (!append(out, out_len, &offset, "{\"netifs\":[")) {
        return false;
    }

    esp_netif_t* default_netif = esp_netif_get_default_netif();
    bool first = true;
    size_t netif_count = 0;
    for (esp_netif_t* netif = esp_netif_next(nullptr); netif != nullptr;
         netif = esp_netif_next(netif)) {
        if (!append_netif_object(netif, default_netif, out, out_len, &offset, &first)) {
            return false;
        }
        ++netif_count;
    }

    if (!append(out, out_len, &offset, "]}")) {
        return false;
    }

    ESP_LOGI(TAG, "Built network payload with %" PRIu32 " netif(s)", static_cast<uint32_t>(netif_count));
    return true;
}

void publishFullSync() {
    emit_events_for_scan(esp_netif_get_default_netif());
    HeapwatchConnector::getInstance().queueNetwork();
    ESP_LOGI(TAG, "Queued full /network sync");
}

void markTopologyDirty() { g_topology_dirty = true; }

bool consumeTopologyDirty() {
    if (!g_topology_dirty) {
        return false;
    }
    g_topology_dirty = false;
    return true;
}

void start() {
    if (g_started) {
        return;
    }
    g_started = true;

    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_or_wifi_event, nullptr));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED,
                                                             &on_ip_or_wifi_event, nullptr));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_handler_register(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &on_ip_or_wifi_event, nullptr));
    heapwatch::netif_traffic::start();
    ESP_LOGI(TAG, "Network reporter started");
}

void stop() {
    if (!g_started) {
        return;
    }
    (void)esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_or_wifi_event);
    (void)esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &on_ip_or_wifi_event);
    (void)esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &on_ip_or_wifi_event);
    g_started = false;
}

}  // namespace heapwatch::device_network
