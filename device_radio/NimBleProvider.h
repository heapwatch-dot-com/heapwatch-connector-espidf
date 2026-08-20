// Copyright (c) 2026 Heapwatch. All rights reserved.

#ifndef HEAPWATCH_DEVICE_RADIO_NIM_BLE_PROVIDER_H
#define HEAPWATCH_DEVICE_RADIO_NIM_BLE_PROVIDER_H

#include "device_radio/IRadioBleProvider.h"

namespace heapwatch::device_radio {

class NimBleProvider final : public IRadioBleProvider {
public:
    bool initialize() override;
    bool isAvailable() const override;
    void fillSnapshot(RadioBleSnapshot& out) const override;
    bool startScan(bool active, uint32_t duration_seconds, ScanPeerCallback on_peer,
                   ScanCompleteCallback on_complete, void* ctx) override;
    void stopScan() override;
    bool isScanning() const override;

    void onHostSync();
    void onGapEvent(int event_type, void* event);

private:
    void updateMacFromStack();
};

}  // namespace heapwatch::device_radio

#endif
