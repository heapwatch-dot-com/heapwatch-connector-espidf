#include "heapwatch/Helpers/heapwatch_connector_timing.h"

#include <sys/time.h>

#include "esp_timer.h"

void heapwatch_get_ingress_timing_ms(uint64_t* tsms_out, uint64_t* utms_out) {
    struct timeval tv = {};
    gettimeofday(&tv, nullptr);

    if (tsms_out != nullptr) {
        *tsms_out = static_cast<uint64_t>(tv.tv_sec) * 1000ULL +
                    static_cast<uint64_t>(tv.tv_usec / 1000);
    }

    if (utms_out != nullptr) {
        *utms_out = static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
    }
}
