#include "heapwatch/Helpers/IngressTimestamps.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_timer.h"

namespace {

constexpr char WALL_CLOCK_NULL_TOKEN[] = "null";

/** Both payload formats we emit: JSON objects and the flat key=value form. */
constexpr char JSON_NULL_WALL_CLOCK[] = "\"tsms\":null";
constexpr char JSON_UPTIME_KEY[] = "\"utms\":";
constexpr char FLAT_NULL_WALL_CLOCK[] = "tsms=null";
constexpr char FLAT_UPTIME_KEY[] = "utms=";

bool read_uptime_from_payload(const char* payload, const char* uptime_key, uint64_t* uptime_ms_out) {
    const char* found = strstr(payload, uptime_key);
    if (found == nullptr) {
        return false;
    }

    const char* digits = found + strlen(uptime_key);
    char* end = nullptr;
    const uint64_t parsed = strtoull(digits, &end, 10);
    if (end == digits) {
        return false;
    }

    *uptime_ms_out = parsed;
    return true;
}

/** Wall clock the device had when a payload with this uptime was built. */
uint64_t wall_clock_at_uptime(uint64_t payload_uptime_ms) {
    uint64_t wall_clock_now_ms = 0;
    uint64_t uptime_now_ms = 0;
    heapwatch_get_wall_clock_and_uptime_ms(&wall_clock_now_ms, &uptime_now_ms);

    if (payload_uptime_ms >= uptime_now_ms) {
        return wall_clock_now_ms;
    }

    const uint64_t age_ms = uptime_now_ms - payload_uptime_ms;
    if (age_ms >= wall_clock_now_ms) {
        return wall_clock_now_ms;
    }

    return wall_clock_now_ms - age_ms;
}

/** Replaces the `null` token that starts at null_token_start with the given digits. */
bool replace_null_token_with_digits(char* payload, size_t payload_capacity, char* null_token_start,
                                    const char* digits) {
    const size_t payload_len = strlen(payload);
    const size_t digits_len = strlen(digits);
    const size_t null_token_len = strlen(WALL_CLOCK_NULL_TOKEN);
    const size_t grown_len = payload_len + digits_len - null_token_len;

    if (grown_len + 1 > payload_capacity) {
        return false;
    }

    char* tail = null_token_start + null_token_len;
    memmove(null_token_start + digits_len, tail, strlen(tail) + 1);
    memcpy(null_token_start, digits, digits_len);
    return true;
}

}  // namespace

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

bool heapwatch_is_wall_clock_trusted(void) {
    uint64_t wall_clock_ms = 0;
    heapwatch_get_wall_clock_and_uptime_ms(&wall_clock_ms, nullptr);
    return wall_clock_ms >= HEAPWATCH_MIN_TRUSTED_WALL_CLOCK_MS;
}

void heapwatch_format_wall_clock_or_null(uint64_t wall_clock_ms, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return;
    }

    if (wall_clock_ms < HEAPWATCH_MIN_TRUSTED_WALL_CLOCK_MS) {
        snprintf(out, out_size, "%s", WALL_CLOCK_NULL_TOKEN);
        return;
    }

    snprintf(out, out_size, "%" PRIu64, wall_clock_ms);
}

bool heapwatch_restamp_payload_wall_clock(char* payload, size_t payload_capacity) {
    if (payload == nullptr || payload_capacity == 0 || !heapwatch_is_wall_clock_trusted()) {
        return false;
    }

    char* null_token_start = nullptr;
    const char* uptime_key = nullptr;

    if (char* json_match = strstr(payload, JSON_NULL_WALL_CLOCK); json_match != nullptr) {
        null_token_start = json_match + strlen(JSON_NULL_WALL_CLOCK) - strlen(WALL_CLOCK_NULL_TOKEN);
        uptime_key = JSON_UPTIME_KEY;
    } else if (char* flat_match = strstr(payload, FLAT_NULL_WALL_CLOCK); flat_match != nullptr) {
        null_token_start = flat_match + strlen(FLAT_NULL_WALL_CLOCK) - strlen(WALL_CLOCK_NULL_TOKEN);
        uptime_key = FLAT_UPTIME_KEY;
    } else {
        return false;
    }

    uint64_t payload_uptime_ms = 0;
    if (!read_uptime_from_payload(payload, uptime_key, &payload_uptime_ms)) {
        return false;
    }

    char digits[HEAPWATCH_WALL_CLOCK_VALUE_BUFFER_SIZE] = {};
    snprintf(digits, sizeof(digits), "%" PRIu64, wall_clock_at_uptime(payload_uptime_ms));

    return replace_null_token_with_digits(payload, payload_capacity, null_token_start, digits);
}
