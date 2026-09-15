#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void heapwatch_get_wall_clock_and_uptime_ms(uint64_t* wall_clock_ms_out, uint64_t* uptime_ms_out);

#ifdef __cplusplus
}
#endif
