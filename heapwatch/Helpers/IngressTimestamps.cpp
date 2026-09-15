#include "heapwatch/Helpers/IngressTimestamps.h"

#include <sys/time.h>

#include "esp_timer.h"

void heapwatch_get_wall_clock_and_uptime_ms(uint64_t* wall_clock_ms_out, uint64_t* uptime_ms_out) {
    struct timeval tv = {};
    gettimeofday(&tv, nullptr);

    if (wall_clock_ms_out != nullptr) {
        *wall_clock_ms_out = static_cast<uint64_t>(tv.tv_sec) * 1000ULL +
                    static_cast<uint64_t>(tv.tv_usec / 1000);
    }

    if (uptime_ms_out != nullptr) {
        *uptime_ms_out = static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
    }
}
