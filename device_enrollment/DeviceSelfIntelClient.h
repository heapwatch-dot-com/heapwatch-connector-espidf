#pragma once

#include <cstdint>

namespace heapwatch::device_enrollment {

/**
 * GET /api/devices/self/intel — returns server's last known boot_cycle, or UINT32_MAX when unset.
 */
bool fetchLastBootCycle(const char* api_base, const char* device_api_key, uint32_t* last_boot_cycle_out);

}  // namespace heapwatch::device_enrollment
