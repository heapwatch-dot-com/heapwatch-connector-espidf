// Copyright (c) 2026 Heapwatch. All rights reserved.

#include "heapwatch/Radio/BleDelegate.h"

namespace Heapwatch::Radio::BluetoothLe {

void notifyDelegateRegistered(const heapwatch_ble_delegate_vtable_t* vtable);
void notifyDelegateUnregistered();

}  // namespace Heapwatch::Radio::BluetoothLe
