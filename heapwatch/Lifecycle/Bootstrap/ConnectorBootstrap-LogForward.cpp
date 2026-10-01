// Copyright 2025-2026 Heapwatch
// SPDX-License-Identifier: Apache-2.0
//
// Log-forward concern for ConnectorBootstrap: ESP vprintf hook + hw_log_forward worker.

#include "heapwatch/Lifecycle/Bootstrap/ConnectorBootstrap.h"
#include "heapwatch/Mqtt/MqttTelemetryClient.h"
#include "heapwatch/Helpers/FreeRtosAlloc.h"
#include "heapwatch/StreamIngress/StreamIngressClient.h"
#include "heapwatch/Settings/ConnectorSettings.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "sdkconfig.h"

#include "heapwatch.h"
#include "heapwatch_config.h"

namespace Heapwatch::Lifecycle::Bootstrap {

#if CONFIG_HEAPWATCH_ENABLE_LOG_COLLECTION
constexpr uint32_t LOG_FORWARD_INTERVAL_MS = 100;
constexpr uint32_t LOG_ASSEMBLY_FLUSH_TIMEOUT_MS = 250;
constexpr size_t LOG_LINE_MAX_LEN = HEAPWATCH_MAX_LOG_LINE_LEN;
constexpr size_t LOG_TAG_MAX_LEN = 32;
constexpr size_t LOG_RING_CAPACITY = 16;

int (*g_default_log_vprintf)(const char* fmt, va_list args) = nullptr;
uint32_t g_log_lines_dropped_total = 0;
uint32_t g_log_mqtt_dropped_total = 0;

struct RawLogLine {
    char text[LOG_LINE_MAX_LEN];
    char task_name[configMAX_TASK_NAME_LEN];
    char tag[LOG_TAG_MAX_LEN];
    int32_t core;
};

RawLogLine g_log_ring[LOG_RING_CAPACITY] = {};
size_t g_log_ring_head = 0;
size_t g_log_ring_tail = 0;
size_t g_log_ring_count = 0;
portMUX_TYPE g_log_ring_lock = portMUX_INITIALIZER_UNLOCKED;

char g_log_line_assembly[LOG_LINE_MAX_LEN] = {};
size_t g_log_line_assembly_len = 0;
char g_log_line_task_name[configMAX_TASK_NAME_LEN] = {};
int32_t g_log_line_core = -1;
portMUX_TYPE g_log_assembly_lock = portMUX_INITIALIZER_UNLOCKED;
TickType_t g_log_line_assembly_last_update = 0;
volatile bool g_log_stop_requested = false;
volatile bool g_log_task_exited = true;
TaskHandle_t g_log_forward_task = nullptr;

bool pushLogRingLine(const RawLogLine* line) {
    if (line == nullptr) {
        return false;
    }

    bool pushed = false;
    taskENTER_CRITICAL(&g_log_ring_lock);
    if (g_log_ring_count < LOG_RING_CAPACITY) {
        g_log_ring[g_log_ring_head] = *line;
        g_log_ring_head = (g_log_ring_head + 1U) % LOG_RING_CAPACITY;
        g_log_ring_count++;
        pushed = true;
    }
    taskEXIT_CRITICAL(&g_log_ring_lock);
    return pushed;
}

bool popLogRingLine(RawLogLine* line) {
    if (line == nullptr) {
        return false;
    }

    bool popped = false;
    taskENTER_CRITICAL(&g_log_ring_lock);
    if (g_log_ring_count > 0) {
        *line = g_log_ring[g_log_ring_tail];
        g_log_ring_tail = (g_log_ring_tail + 1U) % LOG_RING_CAPACITY;
        g_log_ring_count--;
        popped = true;
    }
    taskEXIT_CRITICAL(&g_log_ring_lock);
    return popped;
}

void stripAnsiSequences(const char* input, char* output, size_t output_len) {
    if (input == nullptr || output == nullptr || output_len == 0) {
        return;
    }

    size_t out_idx = 0;
    for (size_t i = 0; input[i] != '\0' && out_idx + 1 < output_len; ++i) {
        if (input[i] == '\033' && input[i + 1] == '[') {
            i += 2;
            while (input[i] != '\0' && input[i] != 'm') {
                ++i;
            }
            continue;
        }

        output[out_idx++] = input[i];
    }
    output[out_idx] = '\0';
}

void redactAuthorizationBearer(char* line, size_t cap) {
    if (line == nullptr || cap == 0) {
        return;
    }

    static constexpr char REDACTED[] = "(REDACTED)";
    const size_t rlen = sizeof(REDACTED) - 1;
    char* p = line;
    while (*p != '\0') {
        if (strncasecmp(p, "authorization:", 14) != 0) {
            ++p;
            continue;
        }
        p += 14;
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (strncasecmp(p, "bearer", 6) != 0) {
            continue;
        }
        const char after = p[6];
        if (after != ' ' && after != '\t' && after != '\0') {
            continue;
        }
        p += 6;
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (*p == '\0') {
            return;
        }
        char* token_start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
            ++p;
        }
        const size_t used = static_cast<size_t>(token_start - line);
        const size_t tail_len = std::strlen(p);
        if (used + rlen + tail_len + 1 > cap) {
            token_start[0] = '\0';
            return;
        }
        std::memmove(token_start + rlen, p, tail_len + 1);
        std::memcpy(token_start, REDACTED, rlen);
        p = token_start + rlen;
    }
}

const char* skipAnsiPrefix(const char* text) {
    if (text == nullptr || text[0] != '\033') {
        return text;
    }
    const char* marker = std::strchr(text, 'm');
    if (marker == nullptr || marker[1] == '\0') {
        return text;
    }
    return marker + 1;
}

bool looksLikeEspLogPrefix(const char* text) {
    if (text == nullptr) {
        return false;
    }
    if (text[0] != 'E' && text[0] != 'W' && text[0] != 'I' && text[0] != 'D' && text[0] != 'V') {
        return false;
    }
    if (text[1] != ' ' || text[2] != '(') {
        return false;
    }
    size_t i = 3;
    bool has_digit = false;
    while (text[i] >= '0' && text[i] <= '9') {
        has_digit = true;
        ++i;
    }
    return has_digit && text[i] == ')';
}

bool parseEspLogTag(const char* text, char* tag_out, size_t tag_out_len) {
    if (text == nullptr || tag_out == nullptr || tag_out_len == 0) {
        return false;
    }

    tag_out[0] = '\0';
    const char* line = skipAnsiPrefix(text);
    if (!looksLikeEspLogPrefix(line)) {
        return false;
    }

    const char* after_paren = std::strchr(line, ')');
    if (after_paren == nullptr) {
        return false;
    }

    const char* tag_start = after_paren + 1;
    while (*tag_start == ' ') {
        ++tag_start;
    }

    const char* colon = std::strchr(tag_start, ':');
    if (colon == nullptr || colon == tag_start) {
        return false;
    }

    const size_t tag_len = static_cast<size_t>(colon - tag_start);
    if (tag_len == 0 || tag_len >= tag_out_len) {
        return false;
    }

    std::memcpy(tag_out, tag_start, tag_len);
    tag_out[tag_len] = '\0';
    return true;
}

void captureLogLineContextLocked() {
    const char* task = pcTaskGetName(nullptr);
    if (task != nullptr) {
        std::snprintf(g_log_line_task_name, sizeof(g_log_line_task_name), "%s", task);
    } else {
        g_log_line_task_name[0] = '\0';
    }
    g_log_line_core = static_cast<int32_t>(xPortGetCoreID());
}

void flushAssembledLogLine() {
    if (g_log_line_assembly_len == 0) {
        return;
    }
    g_log_line_assembly[g_log_line_assembly_len] = '\0';

    RawLogLine line = {};
    std::snprintf(line.text, LOG_LINE_MAX_LEN, "%s", g_log_line_assembly);
    std::snprintf(line.task_name, sizeof(line.task_name), "%s", g_log_line_task_name);
    line.core = g_log_line_core;
    parseEspLogTag(g_log_line_assembly, line.tag, sizeof(line.tag));

    if (!pushLogRingLine(&line)) {
        g_log_lines_dropped_total++;
    }
    g_log_line_assembly_len = 0;
    g_log_line_assembly[0] = '\0';
    g_log_line_task_name[0] = '\0';
    g_log_line_core = -1;
    g_log_line_assembly_last_update = 0;
}

HeapwatchConnectorSeverity parseEspLogSeverity(const char* text) {
    const char* line = skipAnsiPrefix(text);
    if (!looksLikeEspLogPrefix(line)) {
        return HEAPWATCH_SEVERITY_INFO;
    }

    switch (line[0]) {
        case 'E':
            return HEAPWATCH_SEVERITY_ERROR;
        case 'W':
            return HEAPWATCH_SEVERITY_WARNING;
        case 'I':
            return HEAPWATCH_SEVERITY_INFO;
        case 'D':
        case 'V':
            return HEAPWATCH_SEVERITY_DEBUG;
        default:
            return HEAPWATCH_SEVERITY_INFO;
    }
}

bool shouldRelayLogSeverity(HeapwatchConnectorSeverity severity) {
    return severity >= static_cast<HeapwatchConnectorSeverity>(
               Heapwatch::Settings::minRelaySeverity());
}

void ingestLogTextChunk(const char* text) {
    if (text == nullptr) {
        return;
    }

    taskENTER_CRITICAL(&g_log_assembly_lock);
    if (g_log_line_assembly_len > 0 && looksLikeEspLogPrefix(skipAnsiPrefix(text))) {
        flushAssembledLogLine();
    }

    if (g_log_line_assembly_len == 0) {
        captureLogLineContextLocked();
    }

    for (size_t i = 0; text[i] != '\0'; ++i) {
        const char c = text[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            flushAssembledLogLine();
            continue;
        }

        if (g_log_line_assembly_len >= (LOG_LINE_MAX_LEN - 1)) {
            flushAssembledLogLine();
        }

        g_log_line_assembly[g_log_line_assembly_len++] = c;
    }
    g_log_line_assembly_last_update = xTaskGetTickCount();
    taskEXIT_CRITICAL(&g_log_assembly_lock);
}

int captureEspLogLineForForwarding(const char* fmt, va_list args) {
    va_list copy_for_stdout;
    va_copy(copy_for_stdout, args);
    const int printed =
        (g_default_log_vprintf != nullptr) ? g_default_log_vprintf(fmt, copy_for_stdout) : 0;
    va_end(copy_for_stdout);

    if (fmt == nullptr) {
        return printed;
    }

    char buffer[LOG_LINE_MAX_LEN] = {};
    va_list copy_for_parse;
    va_copy(copy_for_parse, args);
    std::vsnprintf(buffer, sizeof(buffer), fmt, copy_for_parse);
    va_end(copy_for_parse);

    if (buffer[0] == '\0') {
        return printed;
    }

    ingestLogTextChunk(buffer);
    return printed;
}

void taskForwardEspLogs(void* arg) {
    (void)arg;

    RawLogLine log_line = {};
    TickType_t last_wake = xTaskGetTickCount();
    while (!g_log_stop_requested) {
        while (popLogRingLine(&log_line)) {
            char sanitized_line[LOG_LINE_MAX_LEN] = {};
            stripAnsiSequences(log_line.text, sanitized_line, sizeof(sanitized_line));
            redactAuthorizationBearer(sanitized_line, sizeof(sanitized_line));

            const HeapwatchConnectorSeverity severity = parseEspLogSeverity(sanitized_line);
            if (!shouldRelayLogSeverity(severity)) {
                continue;
            }

            HeapwatchConnectorLogContext context = {};
            context.task = log_line.task_name[0] != '\0' ? log_line.task_name : nullptr;
            context.tag = log_line.tag[0] != '\0' ? log_line.tag : nullptr;
            context.core = log_line.core;

            if (Heapwatch::StreamIngress::isCaptureActive()) {
                char json[HEAPWATCH_SMALL_PAYLOAD_SIZE] = {};
                if (MqttTelemetryClient::getInstance().formatLogJsonLine(
                        severity, sanitized_line, &context, json, sizeof(json))) {
                    Heapwatch::StreamIngress::enqueueLogJson(json);
                }
            } else {
                const bool queued =
                    MqttTelemetryClient::getInstance().queueLog(severity, sanitized_line, &context);
                if (!queued) {
                    g_log_mqtt_dropped_total++;
                }
            }
        }

        taskENTER_CRITICAL(&g_log_assembly_lock);
        if (g_log_line_assembly_len > 0 && g_log_line_assembly_last_update != 0 &&
            (xTaskGetTickCount() - g_log_line_assembly_last_update) >=
                pdMS_TO_TICKS(LOG_ASSEMBLY_FLUSH_TIMEOUT_MS)) {
            flushAssembledLogLine();
        }
        taskEXIT_CRITICAL(&g_log_assembly_lock);

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(LOG_FORWARD_INTERVAL_MS));
    }
    g_log_task_exited = true;
    g_log_forward_task = nullptr;
    heapwatch_task_delete(nullptr);
}
#endif /* CONFIG_HEAPWATCH_ENABLE_LOG_COLLECTION */


void installLogRelayHookIfEnabled() {
#if CONFIG_HEAPWATCH_ENABLE_LOG_COLLECTION
    g_default_log_vprintf = esp_log_set_vprintf(captureEspLogLineForForwarding);
#endif
}


void startLogRelayTaskIfEnabled() {
#if CONFIG_HEAPWATCH_ENABLE_LOG_COLLECTION
    if (g_log_forward_task != nullptr) {
        return;
    }
    g_log_stop_requested = false;
    g_log_task_exited = false;
    if (heapwatch_task_create(taskForwardEspLogs, "hw_log_forward", HEAPWATCH_LOG_FORWARD_TASK_STACK_BYTES,
                              nullptr, tskIDLE_PRIORITY + 1, &g_log_forward_task) != pdPASS) {
        g_log_forward_task = nullptr;
        g_log_task_exited = true;
    }
#endif
}


void stopLogRelay() {
#if CONFIG_HEAPWATCH_ENABLE_LOG_COLLECTION
    g_log_stop_requested = true;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
    while (!g_log_task_exited && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    g_log_forward_task = nullptr;
    if (g_default_log_vprintf != nullptr) {
        (void)esp_log_set_vprintf(g_default_log_vprintf);
        g_default_log_vprintf = nullptr;
    }
#endif
}


uint32_t getLogLinesDroppedTotal() {
#if CONFIG_HEAPWATCH_ENABLE_LOG_COLLECTION
    return g_log_lines_dropped_total;
#else
    return 0;
#endif
}

uint32_t getLogMqttDroppedTotal() {
#if CONFIG_HEAPWATCH_ENABLE_LOG_COLLECTION
    return g_log_mqtt_dropped_total;
#else
    return 0;
#endif
}


}  // namespace Heapwatch::Lifecycle::Bootstrap
