// Copyright (c) 2026 Viktor Jamrich Enterprises s.r.o. All rights reserved.
//
// Public C API for customer-owned BLE stacks. Register a vtable to override the
// built-in NimBLE report-only provider.

#ifndef HEAPWATCH_RADIO_BLE_DELEGATE_H
#define HEAPWATCH_RADIO_BLE_DELEGATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool stack_ready;
    bool advertising;
    bool scanning;
    const char* mac;
} heapwatch_ble_delegate_snapshot_t;

typedef void (*heapwatch_ble_delegate_scan_peer_cb)(const char* peer_addr, int8_t rssi,
                                                    const char* name, void* ctx);
typedef void (*heapwatch_ble_delegate_scan_done_cb)(bool success, const char* message, void* ctx);

typedef struct {
    void (*fill_snapshot)(heapwatch_ble_delegate_snapshot_t* out);
    bool (*start_scan)(bool active, uint32_t duration_seconds,
                       heapwatch_ble_delegate_scan_peer_cb on_peer,
                       heapwatch_ble_delegate_scan_done_cb on_done, void* ctx);
    void (*stop_scan)(void);
    bool (*is_scanning)(void);
} heapwatch_ble_delegate_vtable_t;

void heapwatch_ble_register_delegate(const heapwatch_ble_delegate_vtable_t* vtable);
void heapwatch_ble_unregister_delegate(void);
bool heapwatch_ble_has_delegate(void);

#ifdef __cplusplus
}
#endif

#endif
