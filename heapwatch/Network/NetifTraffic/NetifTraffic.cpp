// Copyright (c) 2026 Heapwatch. All rights reserved.

#include "heapwatch/Network/NetifTraffic/NetifTraffic.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "heapwatch_config.h"
#include "lwip/err.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "heapwatch/Helpers/DynamicParameterSegment.h"

namespace Heapwatch::Network::NetifTraffic {
namespace {

constexpr const char* TAG = "HwNet";
constexpr size_t MAX_TRACKED_KEYS = 8;
constexpr size_t KEY_LEN = 32;

const char* KNOWN_IF_KEYS[] = {"WIFI_STA_DEF", "WIFI_AP_DEF", "ETH_DEF", "PPP_DEF"};

struct Slot {
    char key[KEY_LEN];
    struct netif* lwip;
    netif_input_fn orig_input;
    netif_linkoutput_fn orig_linkoutput;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    bool hooked;
};

Slot g_slots[MAX_TRACKED_KEYS] = {};
size_t g_slot_count = 0;
bool g_started = false;

#if defined(CONFIG_HEAPWATCH_ENABLE_NETIF_TRAFFIC) && CONFIG_HEAPWATCH_ENABLE_NETIF_TRAFFIC
#define HEAPWATCH_NETIF_TRAFFIC_ACTIVE 1
#endif

#ifdef HEAPWATCH_NETIF_TRAFFIC_ACTIVE

Slot* find_slot_by_key(const char* key) {
    for (size_t i = 0; i < g_slot_count; ++i) {
        if (strncmp(g_slots[i].key, key, KEY_LEN) == 0) {
            return &g_slots[i];
        }
    }
    return nullptr;
}

Slot* find_slot_by_lwip(struct netif* lwip) {
    if (lwip == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < g_slot_count; ++i) {
        if (g_slots[i].lwip == lwip) {
            return &g_slots[i];
        }
    }
    return nullptr;
}

void copy_key(char* dest, const char* src) {
    strncpy(dest, src, KEY_LEN - 1);
    dest[KEY_LEN - 1] = '\0';
}

err_t hook_input(struct pbuf* p, struct netif* inp) {
    Slot* slot = find_slot_by_lwip(inp);
    if (slot != nullptr && p != nullptr) {
        slot->rx_bytes += static_cast<uint64_t>(p->tot_len);
    }
    if (slot != nullptr && slot->orig_input != nullptr) {
        return slot->orig_input(p, inp);
    }
    return ERR_IF;
}

err_t hook_linkoutput(struct netif* netif, struct pbuf* p) {
    Slot* slot = find_slot_by_lwip(netif);
    if (slot != nullptr && p != nullptr) {
        slot->tx_bytes += static_cast<uint64_t>(p->tot_len);
    }
    if (slot != nullptr && slot->orig_linkoutput != nullptr) {
        return slot->orig_linkoutput(netif, p);
    }
    return ERR_IF;
}

Slot* alloc_slot(const char* key, struct netif* lwip) {
    Slot* slot = find_slot_by_key(key);
    if (slot != nullptr) {
        return slot;
    }
    if (g_slot_count >= MAX_TRACKED_KEYS) {
        return nullptr;
    }
    slot = &g_slots[g_slot_count++];
    copy_key(slot->key, key);
    slot->lwip = lwip;
    slot->orig_input = nullptr;
    slot->orig_linkoutput = nullptr;
    slot->rx_bytes = 0;
    slot->tx_bytes = 0;
    slot->hooked = false;
    return slot;
}

void hook_esp_netif(esp_netif_t* netif) {
    if (netif == nullptr) {
        return;
    }
    const char* key = esp_netif_get_ifkey(netif);
    if (key == nullptr || key[0] == '\0') {
        return;
    }
    auto* lwip = static_cast<struct netif*>(esp_netif_get_netif_impl(netif));
    if (lwip == nullptr) {
        return;
    }
    if (lwip->input == &hook_input) {
        return;
    }

    Slot* slot = alloc_slot(key, lwip);
    if (slot == nullptr) {
        return;
    }
    slot->lwip = lwip;
    slot->orig_input = lwip->input;
    slot->orig_linkoutput = lwip->linkoutput;
    lwip->input = &hook_input;
    if (lwip->linkoutput != nullptr) {
        lwip->linkoutput = &hook_linkoutput;
    }
    slot->hooked = true;
    ESP_LOGI(TAG, "hooked if_key=%s input=%" PRId32 " linkoutput=%" PRId32, key,
             static_cast<int32_t>(slot->orig_input != nullptr),
             static_cast<int32_t>(slot->orig_linkoutput != nullptr));
}

void hook_known_netifs() {
    for (const char* key : KNOWN_IF_KEYS) {
        hook_esp_netif(esp_netif_get_handle_from_ifkey(key));
    }
    hook_esp_netif(esp_netif_get_default_netif());
}

#endif  // HEAPWATCH_NETIF_TRAFFIC_ACTIVE

}  // namespace

void start() {
#ifdef HEAPWATCH_NETIF_TRAFFIC_ACTIVE
    if (g_started) {
        return;
    }
    g_started = true;
    hook_known_netifs();
    ESP_LOGI(TAG, "start: input/linkoutput hooks (slots=%" PRIu32 ")", static_cast<uint32_t>(g_slot_count));
#else
    ESP_LOGI(TAG, "start: NETIF_TRAFFIC compiled OUT");
    (void)g_started;
#endif
}

void installNetifRxTxHooksIfMissing() {
#ifdef HEAPWATCH_NETIF_TRAFFIC_ACTIVE
    hook_known_netifs();
#else
    (void)g_slot_count;
#endif
}

size_t appendParameterFields(char* out, size_t out_len, bool json) {
    if (out == nullptr || out_len == 0) {
        return 0;
    }

#ifdef HEAPWATCH_NETIF_TRAFFIC_ACTIVE
    hook_known_netifs();
    size_t pos = 0;
    for (size_t i = 0; i < g_slot_count && pos < out_len; ++i) {
        const Slot& slot = g_slots[i];
        if (!slot.hooked) {
            continue;
        }
        char slug_seg[KEY_LEN];
        if (!Heapwatch::Helpers::normalizeDynamicSegment(slot.key, slug_seg, sizeof(slug_seg))) {
            continue;
        }
        int32_t written = 0;
        if (json) {
            written = snprintf(out + pos, out_len - pos,
                               ",\"dynamic.net.%s.rx_bytes\":%" PRIu64 ",\"dynamic.net.%s.tx_bytes\":%" PRIu64,
                               slug_seg, slot.rx_bytes, slug_seg, slot.tx_bytes);
        } else {
            written = snprintf(out + pos, out_len - pos,
                               "%sdynamic.net.%s.rx_bytes=%" PRIu64 "%sdynamic.net.%s.tx_bytes=%" PRIu64,
                               HEAPWATCH_FLAT_DELIMITER, slug_seg, slot.rx_bytes,
                               HEAPWATCH_FLAT_DELIMITER, slug_seg, slot.tx_bytes);
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
    hook_known_netifs();
    const Slot* slot = find_slot_by_key(if_key);
    const uint64_t rx = slot != nullptr ? slot->rx_bytes : 0;
    const uint64_t tx = slot != nullptr ? slot->tx_bytes : 0;
    if (*offset >= out_len) {
        return false;
    }
    const int32_t written =
        snprintf(out + *offset, out_len - *offset, ",\"rx_bytes\":%" PRIu64 ",\"tx_bytes\":%" PRIu64,
                 rx, tx);
    if (written < 0 || static_cast<size_t>(written) >= out_len - *offset) {
        return false;
    }
    *offset += static_cast<size_t>(written);
    return true;
#else
    (void)out;
    (void)out_len;
    (void)offset;
    (void)if_key;
    return true;
#endif
}

}  // namespace Heapwatch::Network::NetifTraffic
