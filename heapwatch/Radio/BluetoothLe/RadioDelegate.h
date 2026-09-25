// Copyright 2026 Heapwatch
// SPDX-License-Identifier: Apache-2.0

#include "heapwatch/Radio/BleDelegate.h"

namespace Heapwatch::Radio::BluetoothLe {

void notifyDelegateRegistered(const heapwatch_ble_delegate_vtable_t* vtable);
void notifyDelegateUnregistered();

}  // namespace Heapwatch::Radio::BluetoothLe
