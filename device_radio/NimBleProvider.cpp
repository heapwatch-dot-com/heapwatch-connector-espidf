// Copyright (c) 2026 Viktor Jamrich Enterprises s.r.o. All rights reserved.

#include "device_radio/NimBleProvider.h"

#include <cstdio>
#include <cstring>

#include "device_radio/BluetoothReporter.h"
#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_HEAPWATCH_ENABLE_BLE_REPORTING && CONFIG_BT_ENABLED

#include "esp_bt.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

namespace heapwatch::device_radio {
namespace {

constexpr const char* kTag = "HwBleNim";

NimBleProvider* g_provider = nullptr;
bool g_host_synced = false;
bool g_advertising = false;
bool g_scanning = false;
char g_mac[kBleAddrLen] = {};

struct ScanSession {
    bool active = false;
    ScanPeerCallback on_peer = nullptr;
    ScanCompleteCallback on_complete = nullptr;
    void* ctx = nullptr;
};

ScanSession g_scan;

void format_addr(const uint8_t addr[6], char* out, size_t out_len) {
    snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x", addr[5], addr[4], addr[3], addr[2],
             addr[1], addr[0]);
}

void hostTask(void* /*param*/) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void onReset(int reason) {
    ESP_LOGE(kTag, "NimBLE reset, reason=%d", reason);
    g_host_synced = false;
}

void onSync() {
    if (g_provider != nullptr) {
        g_provider->onHostSync();
    }
}

int gapEvent(struct ble_gap_event* event, void* /*arg*/) {
    if (g_provider != nullptr) {
        g_provider->onGapEvent(event->type, event);
    }
    return 0;
}

}  // namespace

bool NimBleProvider::initialize() {
    g_provider = this;

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&bt_cfg) != ESP_OK) {
        ESP_LOGE(kTag, "esp_bt_controller_init failed");
        return false;
    }
    if (esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK) {
        ESP_LOGE(kTag, "esp_bt_controller_enable failed");
        return false;
    }

    nimble_port_init();
    ble_hs_cfg.reset_cb = onReset;
    ble_hs_cfg.sync_cb = onSync;
    ble_svc_gap_device_name_set("heapwatch");
    ble_svc_gap_init();
    nimble_port_freertos_init(hostTask);

    ESP_LOGI(kTag, "NimBLE report-only stack starting");
    return true;
}

bool NimBleProvider::isAvailable() const { return g_host_synced; }

void NimBleProvider::updateMacFromStack() {
    uint8_t addr[6] = {};
    if (ble_hs_id_infer_auto(0, &addr[0]) == 0) {
        format_addr(addr, g_mac, sizeof(g_mac));
    }
}

void NimBleProvider::onHostSync() {
    g_host_synced = true;
    updateMacFromStack();
    ESP_LOGI(kTag, "NimBLE host synced, mac=%s", g_mac);
    heapwatch::device_radio::publishFullSync();
}

void NimBleProvider::fillSnapshot(RadioBleSnapshot& out) const {
    std::memset(&out, 0, sizeof(out));
    out.bt_enabled = true;
    out.stack_ready = g_host_synced;
    out.advertising = g_advertising;
    out.scanning = g_scanning;
    snprintf(out.mac, sizeof(out.mac), "%s", g_mac);
}

bool NimBleProvider::startScan(bool active, uint32_t duration_seconds, ScanPeerCallback on_peer,
                               ScanCompleteCallback on_complete, void* ctx) {
    if (!g_host_synced) {
        if (on_complete != nullptr) {
            on_complete(false, "stack_not_ready", ctx);
        }
        return false;
    }
    if (g_scanning) {
        if (on_complete != nullptr) {
            on_complete(false, "scan_busy", ctx);
        }
        return false;
    }

    g_scan.active = true;
    g_scan.on_peer = on_peer;
    g_scan.on_complete = on_complete;
    g_scan.ctx = ctx;
    g_scanning = true;

    struct ble_gap_disc_params params = {};
    params.passive = active ? 0 : 1;
    params.itvl = 0;
    params.window = 0;
    params.filter_policy = 0;
    params.limited = 0;

    uint8_t own_addr_type = 0;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        g_scanning = false;
        g_scan.active = false;
        if (on_complete != nullptr) {
            on_complete(false, "addr_type_failed", ctx);
        }
        return false;
    }

    const uint32_t duration_ms = duration_seconds * 1000U;
    const int rc = ble_gap_disc(own_addr_type, duration_ms, &params, gapEvent, nullptr);
    if (rc != 0) {
        g_scanning = false;
        g_scan.active = false;
        if (on_complete != nullptr) {
            on_complete(false, "gap_disc_failed", ctx);
        }
        return false;
    }

    return true;
}

void NimBleProvider::stopScan() {
    if (!g_scanning) {
        return;
    }
    ble_gap_disc_cancel();
}

bool NimBleProvider::isScanning() const { return g_scanning; }

void NimBleProvider::onGapEvent(int event_type, void* event_ptr) {
    auto* event = static_cast<struct ble_gap_event*>(event_ptr);

    switch (event_type) {
        case BLE_GAP_EVENT_ADV_COMPLETE:
            g_advertising = false;
            break;
        case BLE_GAP_EVENT_DISC: {
            if (!g_scan.active || g_scan.on_peer == nullptr) {
                break;
            }
            ScanPeer peer = {};
            format_addr(event->disc.addr.val, peer.addr, sizeof(peer.addr));
            peer.rssi = event->disc.rssi;
            if (event->disc.length_data > 0 && event->disc.data != nullptr) {
                ble_hs_adv_fields fields = {};
                if (ble_hs_adv_parse_fields(&fields, event->disc.data,
                                            event->disc.length_data) == 0 &&
                    fields.name != nullptr && fields.name_len > 0) {
                    const size_t copy_len = fields.name_len < 31
                                                ? fields.name_len
                                                : static_cast<uint8_t>(31);
                    memcpy(peer.name, fields.name, copy_len);
                    peer.name[copy_len] = '\0';
                }
            }
            g_scan.on_peer(peer, g_scan.ctx);
            break;
        }
        case BLE_GAP_EVENT_DISC_COMPLETE:
            g_scanning = false;
            if (g_scan.active && g_scan.on_complete != nullptr) {
                g_scan.on_complete(true, "scan_complete", g_scan.ctx);
            }
            g_scan.active = false;
            g_scan.on_peer = nullptr;
            g_scan.on_complete = nullptr;
            g_scan.ctx = nullptr;
            break;
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                // Connection tracking is report-only v1; snapshot lists stay empty.
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            break;
        default:
            break;
    }
}

}  // namespace heapwatch::device_radio

#else  // !CONFIG_HEAPWATCH_ENABLE_BLE_REPORTING || !CONFIG_BT_ENABLED

namespace heapwatch::device_radio {

bool NimBleProvider::initialize() { return false; }
bool NimBleProvider::isAvailable() const { return false; }
void NimBleProvider::fillSnapshot(RadioBleSnapshot& out) const {
    std::memset(&out, 0, sizeof(out));
}
bool NimBleProvider::startScan(bool, uint32_t, ScanPeerCallback, ScanCompleteCallback on_complete,
                               void* ctx) {
    if (on_complete != nullptr) {
        on_complete(false, "bluetooth_disabled", ctx);
    }
    return false;
}
void NimBleProvider::stopScan() {}
bool NimBleProvider::isScanning() const { return false; }
void NimBleProvider::onHostSync() {}
void NimBleProvider::onGapEvent(int, void*) {}

}  // namespace heapwatch::device_radio

#endif
