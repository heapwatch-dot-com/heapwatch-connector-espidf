// Copyright (c) 2026 Heapwatch. All rights reserved.

#include "heapwatch/Radio/BluetoothLe/IRadioBleProvider.h"

#include <cstdio>
#include <cstring>

#include "heapwatch/Radio/BleDelegate.h"
#include "heapwatch/Radio/BluetoothLe/NimBleProvider.h"
#include "heapwatch/Radio/BluetoothLe/NullBleProvider.h"
#include "sdkconfig.h"

namespace Heapwatch::Radio::BluetoothLe {
namespace {

const heapwatch_ble_delegate_vtable_t* g_delegate_vtable = nullptr;

ScannedBleDeviceCallback g_scan_peer_cb = nullptr;
ScanCompleteCallback g_scan_done_cb = nullptr;
void* g_scan_ctx = nullptr;

void delegateScannedBleDevice(const char* peer_addr, int8_t rssi, const char* name, void* /*ctx*/) {
    if (g_scan_peer_cb == nullptr) {
        return;
    }
    ScannedBleDevice peer = {};
    if (peer_addr != nullptr) {
        snprintf(peer.addr, sizeof(peer.addr), "%s", peer_addr);
    }
    peer.rssi = rssi;
    if (name != nullptr) {
        snprintf(peer.name, sizeof(peer.name), "%s", name);
    }
    g_scan_peer_cb(peer, g_scan_ctx);
}

void delegateScanDone(bool success, const char* message, void* /*ctx*/) {
    if (g_scan_done_cb != nullptr) {
        g_scan_done_cb(success, message, g_scan_ctx);
    }
    g_scan_peer_cb = nullptr;
    g_scan_done_cb = nullptr;
    g_scan_ctx = nullptr;
}

class HostAppBleProvider final : public IRadioBleProvider {
public:
    bool initialize() override { return true; }
    bool isAvailable() const override { return g_delegate_vtable != nullptr; }

    void fillSnapshot(RadioBleSnapshot& out) const override {
        std::memset(&out, 0, sizeof(out));
        out.bt_enabled = true;
        if (g_delegate_vtable == nullptr || g_delegate_vtable->fill_snapshot == nullptr) {
            return;
        }

        heapwatch_ble_delegate_snapshot_t snap = {};
        g_delegate_vtable->fill_snapshot(&snap);
        out.stack_ready = snap.stack_ready;
        out.advertising = snap.advertising;
        out.scanning = snap.scanning;
        if (snap.mac != nullptr) {
            snprintf(out.mac, sizeof(out.mac), "%s", snap.mac);
        }
    }

    bool startScan(bool active, uint32_t duration_seconds, ScannedBleDeviceCallback on_peer,
                   ScanCompleteCallback on_complete, void* ctx) override {
        if (g_delegate_vtable == nullptr || g_delegate_vtable->start_scan == nullptr) {
            if (on_complete != nullptr) {
                on_complete(false, "no_delegate", ctx);
            }
            return false;
        }

        g_scan_peer_cb = on_peer;
        g_scan_done_cb = on_complete;
        g_scan_ctx = ctx;
        return g_delegate_vtable->start_scan(active, duration_seconds, delegateScannedBleDevice,
                                             delegateScanDone, nullptr);
    }

    void stopScan() override {
        if (g_delegate_vtable != nullptr && g_delegate_vtable->stop_scan != nullptr) {
            g_delegate_vtable->stop_scan();
        }
    }

    bool isScanning() const override {
        if (g_delegate_vtable == nullptr || g_delegate_vtable->is_scanning == nullptr) {
            return false;
        }
        return g_delegate_vtable->is_scanning();
    }
};

}  // namespace

IRadioBleProvider& getBleProvider() {
    if (g_delegate_vtable != nullptr) {
        static HostAppBleProvider delegate_provider;
        return delegate_provider;
    }
#if CONFIG_HEAPWATCH_ENABLE_BLE_REPORTING && CONFIG_BT_ENABLED
    static NimBleProvider nimble_provider;
    return nimble_provider;
#else
    static NullBleProvider null_provider;
    return null_provider;
#endif
}

void notifyDelegateRegistered(const heapwatch_ble_delegate_vtable_t* vtable) {
    g_delegate_vtable = vtable;
}

void notifyDelegateUnregistered() { g_delegate_vtable = nullptr; }

}  // namespace Heapwatch::Radio::BluetoothLe
