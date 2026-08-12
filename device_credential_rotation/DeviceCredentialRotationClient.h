#pragma once

#include "device_credentials/DeviceCredentialsStore.h"

namespace heapwatch::device_credential_rotation {

bool fetchAndConfirm(const char* api_base, const heapwatch::device_credentials::Credentials* current,
                      heapwatch::device_credentials::Credentials* updated);

}  // namespace heapwatch::device_credential_rotation
