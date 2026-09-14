// Copyright (c) 2026 Heapwatch. All rights reserved.

#ifndef HEAPWATCH_DEVICE_RADIO_BLE_SNAPSHOT_H
#define HEAPWATCH_DEVICE_RADIO_BLE_SNAPSHOT_H

#include <stddef.h>
#include <stdint.h>

namespace heapwatch::device_radio {

constexpr size_t MAX_BLE_CONNECTIONS = 4;
constexpr size_t MAX_BLE_BONDS = 8;
constexpr size_t BLE_ADDR_LEN = 18;

struct RadioBleConnection {
    char peer_addr[BLE_ADDR_LEN];
    uint16_t conn_handle;
    bool is_central;
};

struct RadioBleBond {
    char peer_addr[BLE_ADDR_LEN];
};

struct RadioBleSnapshot {
    bool stack_ready;
    bool advertising;
    bool scanning;
    bool bt_enabled;
    char mac[BLE_ADDR_LEN];
    RadioBleConnection connections[MAX_BLE_CONNECTIONS];
    size_t connection_count;
    RadioBleBond bonds[MAX_BLE_BONDS];
    size_t bond_count;
};

}  // namespace heapwatch::device_radio

#endif
