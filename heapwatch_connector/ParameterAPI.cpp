// Copyright (c) 2026 Heapwatch. All rights reserved.
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
// For licensing inquiries, please contact: info@heapwatch.com
//

#include "ParameterAPI.h"

#include <cstdio>
#include <cstring>

#include "HeapwatchConnector.h"
#include "DynamicParameterSegment.h"

namespace heapwatch::parameter_api {

namespace {

void build_power_slug(const char* source_name, const char* field, char* out, size_t out_size) {
    char normalized[32];
    if (!heapwatch::normalizeDynamicSegment(source_name, normalized, sizeof(normalized))) {
        if (out_size > 0) {
            out[0] = '\0';
        }
        return;
    }
    snprintf(out, out_size, "dynamic.power.%s.%s", normalized, field);
}

}  // namespace

void updatePowerMv(const char* source_name, int64_t mv) {
    char slug[80];
    build_power_slug(source_name, "mv", slug, sizeof(slug));
    if (slug[0] == '\0') {
        return;
    }
    HeapwatchConnector::getInstance().updateParameterInt(slug, mv);
}

void updatePowerMvRanged(const char* source_name, int64_t mv, int64_t min_mv, int64_t max_mv) {
    char slug[80];
    build_power_slug(source_name, "mv", slug, sizeof(slug));
    if (slug[0] == '\0') {
        return;
    }
    HeapwatchConnector::getInstance().updateParameterIntRanged(slug, mv, min_mv, max_mv);
}

void updatePowerPercent(const char* source_name, double percent) {
    char slug[80];
    build_power_slug(source_name, "percent", slug, sizeof(slug));
    if (slug[0] == '\0') {
        return;
    }
    HeapwatchConnector::getInstance().updateParameterDouble(slug, percent);
}

void updatePowerState(const char* source_name, heapwatch_power_state_t state) {
    char slug[80];
    build_power_slug(source_name, "state", slug, sizeof(slug));
    if (slug[0] == '\0') {
        return;
    }
    HeapwatchConnector::getInstance().updateParameterInt(slug, static_cast<int64_t>(state));
}

void updateCellularSignal(int rssi_dbm, int ber) {
    HeapwatchConnector::getInstance().updateParameterInt("cellular_rssi_dbm", rssi_dbm);
    HeapwatchConnector::getInstance().updateParameterInt("cellular_ber", ber);
}

}  // namespace heapwatch::parameter_api
