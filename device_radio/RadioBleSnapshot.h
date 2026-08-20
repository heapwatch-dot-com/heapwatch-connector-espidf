// Copyright (c) 2026 Heapwatch. All rights reserved.

#ifndef HEAPWATCH_DEVICE_RADIO_BLE_SNAPSHOT_H
#define HEAPWATCH_DEVICE_RADIO_BLE_SNAPSHOT_H

#include <stddef.h>
#include <stdint.h>

namespace heapwatch::device_radio {

constexpr size_t kMaxBleConnections = 4;
constexpr size_t kMaxBleBonds = 8;
constexpr size_t kBleAddrLen = 18;

struct RadioBleConnection {
    char peer_addr[kBleAddrLen];
    uint16_t conn_handle;
    bool is_central;
};

struct RadioBleBond {
    char peer_addr[kBleAddrLen];
};

struct RadioBleSnapshot {
    bool stack_ready;
    bool advertising;
    bool scanning;
    bool bt_enabled;
    char mac[kBleAddrLen];
    RadioBleConnection connections[kMaxBleConnections];
    size_t connection_count;
    RadioBleBond bonds[kMaxBleBonds];
    size_t bond_count;
};

}  // namespace heapwatch::device_radio

#endif
