// Copyright (c) 2026 Viktor Jamrich Enterprises s.r.o. All rights reserved.
//
// This software is part of the Heapwatch Connector for ESP-IDF project.
// Unauthorized copying, modification, or distribution of this software,
// via any medium, is strictly prohibited unless otherwise permitted.
//
// This software is provided "as is", without warranty of any kind, express or implied,
// including but not limited to the warranties of merchantability, fitness for a particular purpose,
// and noninfringement. In no event shall the authors or copyright holders be liable for any claim,
// damages, or other liability, whether in an action of contract, tort, or otherwise, arising from,
// out of, or in connection with the software or the use or other dealings in the software.
//
// For licensing inquiries, please contact: admin@heapwatch.com
//

#include "ParameterAPI.h"

#include <cstdio>
#include <cstring>

#include "HeapwatchConnector.h"

namespace heapwatch::parameter_api {

namespace {

// Backend auto-provisioning (mirroring TaskParameterProvisioningService) keys
// off the normalized source name embedded in the slug -- lowercase, safe
// characters only. Client-supplied names are sanitized defensively here so a
// stray '.'/space in e.g. "Backup Battery" can't corrupt the slug's own
// `power.{name}.mv` dot-delimited structure.
void normalize_source_name(const char* source_name, char* out, size_t out_size) {
    if (source_name == nullptr || out == nullptr || out_size == 0) {
        if (out != nullptr && out_size > 0) {
            out[0] = '\0';
        }
        return;
    }
    size_t out_idx = 0;
    for (size_t i = 0; source_name[i] != '\0' && out_idx + 1 < out_size; ++i) {
        const char c = source_name[i];
        const bool is_lower = c >= 'a' && c <= 'z';
        const bool is_upper = c >= 'A' && c <= 'Z';
        const bool is_digit = c >= '0' && c <= '9';
        if (is_lower || is_digit) {
            out[out_idx++] = c;
        } else if (is_upper) {
            out[out_idx++] = static_cast<char>(c - 'A' + 'a');
        } else if (c == '_' || c == '-') {
            out[out_idx++] = '_';
        }
        // Any other character (space, '.', etc.) is dropped rather than
        // mapped to '_' -- avoids "Backup Battery"/"backup.battery" colliding
        // on the same normalized slug in a confusing way.
    }
    out[out_idx] = '\0';
}

void build_power_slug(const char* source_name, const char* field, char* out, size_t out_size) {
    char normalized[32];
    normalize_source_name(source_name, normalized, sizeof(normalized));
    snprintf(out, out_size, "power.%s.%s", normalized, field);
}

}  // namespace

void updatePowerMv(const char* source_name, int64_t mv) {
    char slug[48];
    build_power_slug(source_name, "mv", slug, sizeof(slug));
    HeapwatchConnector::getInstance().updateParameterInt(slug, mv);
}

void updatePowerMvRanged(const char* source_name, int64_t mv, int64_t min_mv, int64_t max_mv) {
    char slug[48];
    build_power_slug(source_name, "mv", slug, sizeof(slug));
    HeapwatchConnector::getInstance().updateParameterIntRanged(slug, mv, min_mv, max_mv);
}

void updatePowerPercent(const char* source_name, double percent) {
    char slug[48];
    build_power_slug(source_name, "percent", slug, sizeof(slug));
    HeapwatchConnector::getInstance().updateParameterDouble(slug, percent);
}

void updatePowerState(const char* source_name, heapwatch_power_state_t state) {
    char slug[48];
    build_power_slug(source_name, "state", slug, sizeof(slug));
    // PORT1: heapwatch_power_state_t is already Heapwatch-owned (no framework
    // equivalent to leak), so the raw enum value is fine to forward as-is.
    HeapwatchConnector::getInstance().updateParameterInt(slug, static_cast<int64_t>(state));
}

void updateCellularSignal(int rssi_dbm, int ber) {
    HeapwatchConnector::getInstance().updateParameterInt("cellular_rssi_dbm", rssi_dbm);
    HeapwatchConnector::getInstance().updateParameterInt("cellular_ber", ber);
}

}  // namespace heapwatch::parameter_api
