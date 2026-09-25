// Copyright 2026 Heapwatch
// SPDX-License-Identifier: Apache-2.0

#include "heapwatch/Parameters/PowerParameterHelpers.h"

#include <cstdio>
#include <cstring>

#include "heapwatch/Mqtt/MqttTelemetryClient.h"
#include "heapwatch/Helpers/DynamicParameterSegment.h"

namespace Heapwatch::Parameters {

namespace {

void build_power_slug(const char* source_name, const char* field, char* out, size_t out_size) {
    char normalized[32];
    if (!Heapwatch::Helpers::normalizeDynamicSegment(source_name, normalized, sizeof(normalized))) {
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
    MqttTelemetryClient::getInstance().updateParameterInt(slug, mv);
}

void updatePowerMvRanged(const char* source_name, int64_t mv, int64_t min_mv, int64_t max_mv) {
    char slug[80];
    build_power_slug(source_name, "mv", slug, sizeof(slug));
    if (slug[0] == '\0') {
        return;
    }
    MqttTelemetryClient::getInstance().updateParameterIntRanged(slug, mv, min_mv, max_mv);
}

void updatePowerPercent(const char* source_name, double percent) {
    char slug[80];
    build_power_slug(source_name, "percent", slug, sizeof(slug));
    if (slug[0] == '\0') {
        return;
    }
    MqttTelemetryClient::getInstance().updateParameterDouble(slug, percent);
}

void updatePowerState(const char* source_name, heapwatch_power_state_t state) {
    char slug[80];
    build_power_slug(source_name, "state", slug, sizeof(slug));
    if (slug[0] == '\0') {
        return;
    }
    MqttTelemetryClient::getInstance().updateParameterInt(slug, static_cast<int64_t>(state));
}

}  // namespace Heapwatch::Parameters
