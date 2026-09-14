// Copyright (c) 2026 Heapwatch. All rights reserved.

#include "heapwatch/Radio/BleDelegate.h"

#include "heapwatch/Radio/BluetoothLe/RadioDelegate.h"

namespace {

const heapwatch_ble_delegate_vtable_t* g_registered_vtable = nullptr;

}  // namespace

void heapwatch_ble_register_delegate(const heapwatch_ble_delegate_vtable_t* vtable) {
    g_registered_vtable = vtable;
    Heapwatch::Radio::BluetoothLe::notifyDelegateRegistered(vtable);
}

void heapwatch_ble_unregister_delegate(void) {
    g_registered_vtable = nullptr;
    Heapwatch::Radio::BluetoothLe::notifyDelegateUnregistered();
}

bool heapwatch_ble_has_delegate(void) { return g_registered_vtable != nullptr; }
