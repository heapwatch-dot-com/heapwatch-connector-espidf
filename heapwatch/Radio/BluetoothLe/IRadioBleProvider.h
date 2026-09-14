// Copyright (c) 2026 Heapwatch. All rights reserved.

#ifndef HEAPWATCH_DEVICE_RADIO_I_RADIO_BLE_PROVIDER_H
#define HEAPWATCH_DEVICE_RADIO_I_RADIO_BLE_PROVIDER_H

#include <stddef.h>
#include <stdint.h>

#include "heapwatch/Radio/BluetoothLe/RadioBleSnapshot.h"

namespace Heapwatch::Radio::BluetoothLe {

struct ScanPeer {
    char addr[BLE_ADDR_LEN];
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

}  // namespace Heapwatch::Radio::BluetoothLe

#endif
