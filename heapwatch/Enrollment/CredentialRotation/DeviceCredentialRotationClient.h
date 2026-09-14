#pragma once

#include "heapwatch/Enrollment/Credentials/DeviceCredentialsStore.h"

namespace Heapwatch::Enrollment::CredentialRotation {

bool fetchAndConfirm(const char* api_base, const Heapwatch::Enrollment::Credentials::Credentials* current,
                      Heapwatch::Enrollment::Credentials::Credentials* updated);

}  // namespace Heapwatch::Enrollment::CredentialRotation
