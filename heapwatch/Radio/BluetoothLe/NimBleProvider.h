// Copyright 2026 Heapwatch
// SPDX-License-Identifier: Apache-2.0

#ifndef HEAPWATCH_DEVICE_RADIO_NIM_BLE_PROVIDER_H
#define HEAPWATCH_DEVICE_RADIO_NIM_BLE_PROVIDER_H

#include "heapwatch/Radio/BluetoothLe/IRadioBleProvider.h"

namespace Heapwatch::Radio::BluetoothLe {

class NimBleProvider final : public IRadioBleProvider {
public:
    bool initialize() override;
    bool isAvailable() const override;
    void fillSnapshot(RadioBleSnapshot& out) const override;
    bool startScan(bool active, uint32_t duration_seconds, ScannedBleDeviceCallback on_peer,
                   ScanCompleteCallback on_complete, void* ctx) override;
    void stopScan() override;
    bool isScanning() const override;

    void onHostSync();
    void onGapEvent(uint8_t event_type, void* event);

private:
    void updateMacFromStack();
};

}  // namespace Heapwatch::Radio::BluetoothLe

#endif
