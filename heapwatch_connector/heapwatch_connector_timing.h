#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void heapwatch_get_ingress_timing_ms(uint64_t* tsms_out, uint64_t* utms_out);

#ifdef __cplusplus
}
#endif
