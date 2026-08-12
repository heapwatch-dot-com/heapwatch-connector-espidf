// Copyright (c) 2026 Viktor Jamrich Enterprises s.r.o. All rights reserved.

#ifndef HEAPWATCH_DEVICE_RADIO_I_RADIO_BLE_PROVIDER_H
#define HEAPWATCH_DEVICE_RADIO_I_RADIO_BLE_PROVIDER_H

#include <stddef.h>
#include <stdint.h>

#include "device_radio/RadioBleSnapshot.h"

namespace heapwatch::device_radio {

struct ScanPeer {
    char addr[kBleAddrLen];
    int8_t rssi;
    char name[32];
};

using ScanPeerCallback = void (*)(const ScanPeer& peer, void* ctx);
using ScanCompleteCallback = void (*)(bool success, const char* message, void* ctx);

class IRadioBleProvider {
public:
    virtual ~IRadioBleProvider() = default;

    virtual bool initialize() = 0;
    virtual bool isAvailable() const = 0;
    virtual void fillSnapshot(RadioBleSnapshot& out) const = 0;
    virtual bool startScan(bool active, uint32_t duration_seconds, ScanPeerCallback on_peer,
                           ScanCompleteCallback on_complete, void* ctx) = 0;
    virtual void stopScan() = 0;
    virtual bool isScanning() const = 0;
};

IRadioBleProvider& getBleProvider();

}  // namespace heapwatch::device_radio

#endif
