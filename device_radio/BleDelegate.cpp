// Copyright (c) 2026 Heapwatch. All rights reserved.

#include "heapwatch/radio/BleDelegate.h"

#include "device_radio/RadioDelegate.h"

namespace {

const heapwatch_ble_delegate_vtable_t* g_registered_vtable = nullptr;

}  // namespace

void heapwatch_ble_register_delegate(const heapwatch_ble_delegate_vtable_t* vtable) {
    g_registered_vtable = vtable;
    heapwatch::device_radio::notifyDelegateRegistered(vtable);
}

void heapwatch_ble_unregister_delegate(void) {
    g_registered_vtable = nullptr;
    heapwatch::device_radio::notifyDelegateUnregistered();
}

bool heapwatch_ble_has_delegate(void) { return g_registered_vtable != nullptr; }
