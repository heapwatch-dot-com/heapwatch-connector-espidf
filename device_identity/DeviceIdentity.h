#pragma once

#include <cstddef>

namespace heapwatch::device_identity {

/** Normalized Wi-Fi STA MAC for enrollment identity claims (`aa:bb:cc:dd:ee:ff`). */
void getClaimedMac(char* out, size_t out_len);

void getMacUid(char* out, size_t out_len);
void getChipId(char* out, size_t out_len);

}  // namespace heapwatch::device_identity
