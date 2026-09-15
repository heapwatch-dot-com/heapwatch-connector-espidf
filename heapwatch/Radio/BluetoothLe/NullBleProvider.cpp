// Copyright (c) 2026 Heapwatch. All rights reserved.

#include "heapwatch/Radio/BluetoothLe/NullBleProvider.h"

#include <cstring>

namespace Heapwatch::Radio::BluetoothLe {

bool NullBleProvider::initialize() { return true; }

bool NullBleProvider::isAvailable() const { return false; }

void NullBleProvider::fillSnapshot(RadioBleSnapshot& out) const {
    std::memset(&out, 0, sizeof(out));
    out.bt_enabled = false;
}

bool NullBleProvider::startScan(bool /*active*/, uint32_t /*duration_seconds*/,
                                ScannedBleDeviceCallback /*on_peer*/, ScanCompleteCallback on_complete,
                                void* ctx) {
    if (on_complete != nullptr) {
        on_complete(false, "bluetooth_disabled", ctx);
    }
    return false;
}

void NullBleProvider::stopScan() {}

bool NullBleProvider::isScanning() const { return false; }

}  // namespace Heapwatch::Radio::BluetoothLe
