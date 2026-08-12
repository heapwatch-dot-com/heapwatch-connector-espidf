// Copyright (c) 2026 Viktor Jamrich Enterprises s.r.o. All rights reserved.

#include "heapwatch/radio/BleDelegate.h"

namespace heapwatch::device_radio {

void notifyDelegateRegistered(const heapwatch_ble_delegate_vtable_t* vtable);
void notifyDelegateUnregistered();

}  // namespace heapwatch::device_radio
