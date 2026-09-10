// Copyright (c) 2026 Heapwatch. All rights reserved.

#include "device_network/NetifTraffic.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "heapwatch_config.h"
#include "lwip/netif.h"
#include "DynamicParameterSegment.h"

namespace heapwatch::netif_traffic {
namespace {

constexpr const char* kTag = "HwNetifTraffic";
constexpr size_t kMaxTrackedKeys = 8;
constexpr size_t kKeyLen = 32;

struct Slot {
    char key[kKeyLen];
    uint32_t last_rx32;
    uint32_t last_tx32;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    bool primed;
};

Slot g_slots[kMaxTrackedKeys] = {};
size_t g_slot_count = 0;
bool g_started = false;

#if defined(CONFIG_HEAPWATCH_ENABLE_NETIF_TRAFFIC) && CONFIG_HEAPWATCH_ENABLE_NETIF_TRAFFIC
#define HEAPWATCH_NETIF_TRAFFIC_ACTIVE 1
#endif
// Goal 1: sampling is off until the device is confirmed stable. Do not re-enable here.
#undef HEAPWATCH_NETIF_TRAFFIC_ACTIVE

#ifdef HEAPWATCH_NETIF_TRAFFIC_ACTIVE

Slot* find_slot_by_key(const char* key) {
    for (size_t i = 0; i < g_slot_count; ++i) {
        if (strncmp(g_slots[i].key, key, kKeyLen) == 0) {
            return &g_slots[i];
        }
    }
    return nullptr;
}

void copy_key(char* dest, const char* src) {
    strncpy(dest, src, kKeyLen - 1);
    dest[kKeyLen - 1] = '\0';
}

uint64_t advance_counter(uint64_t accumulated, uint32_t previous, uint32_t now) {
    if (now >= previous) {
        return accumulated + static_cast<uint64_t>(now - previous);
    }
    return accumulated + (0x100000000ULL - static_cast<uint64_t>(previous)) + static_cast<uint64_t>(now);
}

void sample_netif(esp_netif_t* netif) {
    const char* key = esp_netif_get_ifkey(netif);
    if (key == nullptr || key[0] == '\0') {
        ESP_LOGI(kTag, "sample: netif with empty if_key");
        return;
    }

#if !MIB2_STATS
    ESP_LOGI(kTag, "sample: key=%s MIB2_STATS=0, skip", key);
    (void)netif;
    return;
#else
    auto* lwip_netif = static_cast<struct netif*>(esp_netif_get_netif_impl(netif));
    if (lwip_netif == nullptr) {
        ESP_LOGI(kTag, "sample: key=%s lwip_impl=null", key);
        return;
    }

    Slot* slot = find_slot_by_key(key);
    if (slot == nullptr) {
        if (g_slot_count >= kMaxTrackedKeys) {
            ESP_LOGI(kTag, "sample: key=%s slot table full (%u)", key,
                     static_cast<unsigned>(g_slot_count));
            return;
        }
        slot = &g_slots[g_slot_count++];
        copy_key(slot->key, key);
        slot->rx_bytes = 0;
        slot->tx_bytes = 0;
        slot->last_rx32 = 0;
        slot->last_tx32 = 0;
        slot->primed = false;
    }

    const uint32_t now_rx = lwip_netif->mib2_counters.ifinoctets;
    const uint32_t now_tx = lwip_netif->mib2_counters.ifoutoctets;
    if (!slot->primed) {
        slot->last_rx32 = now_rx;
        slot->last_tx32 = now_tx;
        slot->rx_bytes = now_rx;
        slot->tx_bytes = now_tx;
        slot->primed = true;
        ESP_LOGI(kTag, "sample PRIME key=%s mib2_rx=%" PRIu32 " mib2_tx=%" PRIu32
                       " acc_rx=%" PRIu64 " acc_tx=%" PRIu64,
                 key, now_rx, now_tx, slot->rx_bytes, slot->tx_bytes);
        return;
    }

    slot->rx_bytes = advance_counter(slot->rx_bytes, slot->last_rx32, now_rx);
    slot->tx_bytes = advance_counter(slot->tx_bytes, slot->last_tx32, now_tx);
    slot->last_rx32 = now_rx;
    slot->last_tx32 = now_tx;
    ESP_LOGI(kTag, "sample key=%s mib2_rx=%" PRIu32 " mib2_tx=%" PRIu32
                   " acc_rx=%" PRIu64 " acc_tx=%" PRIu64,
             key, now_rx, now_tx, slot->rx_bytes, slot->tx_bytes);
#endif
}

void sample_all() {
    size_t walked = 0;
    for (esp_netif_t* netif = esp_netif_next(nullptr); netif != nullptr;
         netif = esp_netif_next(netif)) {
        ++walked;
        sample_netif(netif);
    }
    ESP_LOGI(kTag, "sample_all walked=%u slots=%u", static_cast<unsigned>(walked),
             static_cast<unsigned>(g_slot_count));
}

#endif  // HEAPWATCH_NETIF_TRAFFIC_ACTIVE

}  // namespace

void start() {
#ifdef HEAPWATCH_NETIF_TRAFFIC_ACTIVE
    if (g_started) {
        return;
    }
    g_started = true;
    ESP_LOGI(kTag, "start: NETIF_TRAFFIC compiled in, MIB2_STATS=%d",
#if MIB2_STATS
             1
#else
             0
#endif
    );
    sample_all();
#else
    ESP_LOGI(kTag, "start: NETIF_TRAFFIC forced off (stability)");
    (void)g_started;
#endif
}

void ensureTracking() {
#ifdef HEAPWATCH_NETIF_TRAFFIC_ACTIVE
    sample_all();
#else
    (void)g_slot_count;
#endif
}

size_t appendParameterFields(char* out, size_t out_len, bool json) {
    if (out == nullptr || out_len == 0) {
        return 0;
    }

#ifdef HEAPWATCH_NETIF_TRAFFIC_ACTIVE
    sample_all();
    size_t pos = 0;
    for (size_t i = 0; i < g_slot_count && pos < out_len; ++i) {
        const Slot& slot = g_slots[i];
        int written = 0;
        if (json) {
            char slug_seg[kKeyLen];
            if (!heapwatch::normalizeDynamicSegment(slot.key, slug_seg, sizeof(slug_seg))) {
                ESP_LOGI(kTag, "param skip key=%s (segment normalize failed)", slot.key);
                continue;
            }
            written = snprintf(out + pos, out_len - pos,
                               ",\"dynamic.net.%s.rx_bytes\":%" PRIu64 ",\"dynamic.net.%s.tx_bytes\":%" PRIu64,
                               slug_seg, slot.rx_bytes, slug_seg, slot.tx_bytes);
            ESP_LOGI(kTag, "param json key=%s slug=%s rx=%" PRIu64 " tx=%" PRIu64 " written=%d",
                     slot.key, slug_seg, slot.rx_bytes, slot.tx_bytes, written);
        } else {
            char slug_seg[kKeyLen];
            if (!heapwatch::normalizeDynamicSegment(slot.key, slug_seg, sizeof(slug_seg))) {
                ESP_LOGI(kTag, "param skip key=%s (segment normalize failed)", slot.key);
                continue;
            }
            written = snprintf(out + pos, out_len - pos,
                               "%sdynamic.net.%s.rx_bytes=%" PRIu64 "%sdynamic.net.%s.tx_bytes=%" PRIu64,
                               HEAPWATCH_FLAT_DELIMITER, slug_seg, slot.rx_bytes,
                               HEAPWATCH_FLAT_DELIMITER, slug_seg, slot.tx_bytes);
            ESP_LOGI(kTag, "param flat key=%s slug=%s rx=%" PRIu64 " tx=%" PRIu64 " written=%d",
                     slot.key, slug_seg, slot.rx_bytes, slot.tx_bytes, written);
        }
        if (written < 0 || static_cast<size_t>(written) >= out_len - pos) {
            ESP_LOGI(kTag, "param buffer full pos=%u out_len=%u", static_cast<unsigned>(pos),
                     static_cast<unsigned>(out_len));
            out[pos] = '\0';
            break;
        }
        pos += static_cast<size_t>(written);
    }
    ESP_LOGI(kTag, "param append slots=%u bytes=%u", static_cast<unsigned>(g_slot_count),
             static_cast<unsigned>(pos));
    return pos;
#else
    (void)json;
    ESP_LOGI(kTag, "param append: NETIF_TRAFFIC compiled OUT");
    if (out_len > 0) {
        out[0] = '\0';
    }
    return 0;
#endif
}

bool appendRxTxJsonFields(char* out, size_t out_len, size_t* offset, const char* if_key) {
#ifdef HEAPWATCH_NETIF_TRAFFIC_ACTIVE
    if (out == nullptr || offset == nullptr || if_key == nullptr) {
        return false;
    }
    sample_all();
    const Slot* slot = find_slot_by_key(if_key);
    const uint64_t rx = slot != nullptr ? slot->rx_bytes : 0;
    const uint64_t tx = slot != nullptr ? slot->tx_bytes : 0;
    ESP_LOGI(kTag, "network json if_key=%s slot=%s rx=%" PRIu64 " tx=%" PRIu64,
             if_key, slot != nullptr ? "yes" : "NO", rx, tx);
    if (*offset >= out_len) {
        return false;
    }
    const int written =
        snprintf(out + *offset, out_len - *offset, ",\"rx_bytes\":%" PRIu64 ",\"tx_bytes\":%" PRIu64,
                 rx, tx);
    if (written < 0 || static_cast<size_t>(written) >= out_len - *offset) {
        return false;
    }
    *offset += static_cast<size_t>(written);
    return true;
#else
    ESP_LOGI(kTag, "network json: NETIF_TRAFFIC compiled OUT if_key=%s",
             if_key != nullptr ? if_key : "(null)");
    (void)out;
    (void)out_len;
    (void)offset;
    (void)if_key;
    return true;
#endif
}

}  // namespace heapwatch::netif_traffic
