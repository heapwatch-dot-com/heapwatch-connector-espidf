// Copyright (c) 2026 Viktor Jamrich Enterprises s.r.o. All rights reserved.

#ifndef HEAPWATCH_DEVICE_RADIO_NULL_BLE_PROVIDER_H
#define HEAPWATCH_DEVICE_RADIO_NULL_BLE_PROVIDER_H

#include "device_radio/IRadioBleProvider.h"

namespace heapwatch::device_radio {

class NullBleProvider final : public IRadioBleProvider {
public:
    bool initialize() override;
    bool isAvailable() const override;
    void fillSnapshot(RadioBleSnapshot& out) const override;
    bool startScan(bool active, uint32_t duration_seconds, ScanPeerCallback on_peer,
                   ScanCompleteCallback on_complete, void* ctx) override;
    void stopScan() override;
    bool isScanning() const override;
};

}  // namespace heapwatch::device_radio

#endif
