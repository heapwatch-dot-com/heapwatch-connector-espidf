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

#include "MetricsCollector.h"
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "../heapwatch_connector/ConnectorBootstrap.h"
#include "../heapwatch_connector/HeapwatchConnector.h"
#include "../heapwatch_connector/heapwatch_connector_timing.h"
#include "../heapwatch_dispatcher/HeapwatchDispatcher.h"
#include "../heapwatch_config.h"
#include "../connector_settings/ConnectorSettings.h"
#include "../storage_stats/StorageStats.h"

namespace heapwatch::metrics_collector {

namespace {
uint64_t g_start_time_us = 0;
char g_payload_buf[HEAPWATCH_MAX_PAYLOAD_SIZE];
MqttMessage g_mqtt_enqueue_msg;

constexpr size_t kMaxTaskRows = 48;

struct PrevTaskRuntime {
    char name[20];
    uint64_t runtime;
};

// CPU1: core-count-agnostic — sized for the widest ESP-IDF target in the VER2 support
// matrix rather than assuming exactly 2 cores (portNUM_PROCESSORS is 1 on single-core
// targets like ESP32-C3/C2, 2 on ESP32/S3, etc.).
constexpr size_t kMaxCoresSupported = 4;

struct CpuUsageState {
    uint64_t prev_total_runtime = 0;
    uint64_t prev_idle_runtime[kMaxCoresSupported] = {};
    bool initialized = false;
};

PrevTaskRuntime g_prev_task_runtime[kMaxTaskRows];
size_t g_prev_task_runtime_count = 0;
CpuUsageState g_cpu_usage_state;

uint64_t task_runtime_delta_by_name(const char* task_name, uint64_t current_runtime) {
    const char* name = task_name != nullptr ? task_name : "?";

    for (size_t i = 0; i < g_prev_task_runtime_count; ++i) {
        if (strcmp(g_prev_task_runtime[i].name, name) == 0) {
            const uint64_t previous = g_prev_task_runtime[i].runtime;
            g_prev_task_runtime[i].runtime = current_runtime;
            return current_runtime >= previous ? current_runtime - previous : 0;
        }
    }

    if (g_prev_task_runtime_count < kMaxTaskRows) {
        memset(g_prev_task_runtime[g_prev_task_runtime_count].name, 0,
               sizeof(g_prev_task_runtime[g_prev_task_runtime_count].name));
        strncpy(g_prev_task_runtime[g_prev_task_runtime_count].name, name,
                sizeof(g_prev_task_runtime[g_prev_task_runtime_count].name) - 1);
        g_prev_task_runtime[g_prev_task_runtime_count].runtime = current_runtime;
        g_prev_task_runtime_count++;
    }

    return 0;
}

void enqueue_mqtt_message(MqttMessageType type, const char* payload) {
    g_mqtt_enqueue_msg.type = type;
    snprintf(g_mqtt_enqueue_msg.payload, sizeof(g_mqtt_enqueue_msg.payload), "%s", payload);
    heapwatch_debug_record_payload_size(type, strlen(g_mqtt_enqueue_msg.payload));

    QueueHandle_t queue = heapwatch_get_queue();
    if (queue != nullptr) {
        xQueueSend(queue, &g_mqtt_enqueue_msg, 0);
    }
}
}  // namespace

void init() {
    g_start_time_us = static_cast<uint64_t>(esp_timer_get_time());
}

// NOTE (Phase 2 scope): the periodic PARAMETER message below bundles heap,
// Wi-Fi, and FreeRTOS-count fields into one hand-rolled JSON/flat payload, so
// HEAPWATCH_ENABLE_HEAP_SNAPSHOTS can't yet exclude just the heap fields
// without restructuring this into the generic per-slug parameter map (PARAM1,
// Phase 5c) — at that point each field becomes independently omittable for
// free. Until then this whole message is unconditionally sent; only the
// clearly separable TASKS message (above) and CPU-usage fields are gated
// today.
void collectTask(void* arg) {
    (void)arg;

    while (heapwatch_collector_running() && !g_mqtt_connected) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    while (heapwatch_collector_running()) {
        vTaskDelay(pdMS_TO_TICKS(heapwatch::connector_settings::telemetryIntervalMs()));
        if (!heapwatch_collector_running()) {
            break;
        }

        const uint32_t uptime_s = static_cast<uint32_t>(
            (static_cast<uint64_t>(esp_timer_get_time()) - g_start_time_us) / 1000000ULL);
        uint64_t tsms = 0;
        uint64_t utms = 0;
        heapwatch_get_ingress_timing_ms(&tsms, &utms);

        const uint32_t heap_free_bytes = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_8BIT));
        const uint32_t heap_total_bytes = static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_8BIT));
        const uint32_t heap_used_bytes = heap_total_bytes - heap_free_bytes;
        const uint32_t heap_used_bytes_max = heap_total_bytes;
        const uint32_t heap_largest_free_block_bytes =
            static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        const uint32_t heap_largest_free_block_bytes_max = heap_total_bytes;

        const uint32_t internal_heap_free_bytes =
            static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        const uint32_t internal_heap_total_bytes =
            static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
        const uint32_t internal_heap_used_bytes = internal_heap_total_bytes - internal_heap_free_bytes;
        const uint32_t internal_heap_used_bytes_max = internal_heap_total_bytes;

        uint32_t psram_used_bytes = 0;
        uint32_t psram_used_bytes_max = 0;
#if CONFIG_SPIRAM
        const uint32_t psram_free_bytes = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        const uint32_t psram_total_bytes = static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
        psram_used_bytes = psram_total_bytes - psram_free_bytes;
        psram_used_bytes_max = psram_total_bytes;
#endif

        int16_t wifi_rssi_dbm = 0;
        wifi_ap_record_t ap_info = {};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            wifi_rssi_dbm = ap_info.rssi;
        }

        uint16_t freertos_tasks_count = static_cast<uint16_t>(uxTaskGetNumberOfTasks());
        uint16_t freertos_ready_tasks_count = 0;
        // CPU1: one entry per actual core (portNUM_PROCESSORS), not a fixed cpu0/cpu1 pair.
        const size_t core_count =
            (portNUM_PROCESSORS <= kMaxCoresSupported) ? portNUM_PROCESSORS : kMaxCoresSupported;
        float cpu_usage_pct[kMaxCoresSupported] = {};

        // Task snapshots (TASKS message) and CPU-usage snapshots share this single
        // FreeRTOS runtime-stats scan, so both toggles gate the same block. When only
        // one of the two is enabled, the other's fields/message are simply left at
        // their zero-initialized defaults below.
#if (configUSE_TRACE_FACILITY == 1) && \
    (CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS || CONFIG_HEAPWATCH_ENABLE_CPU_SNAPSHOTS)
        TaskStatus_t* task_status =
            static_cast<TaskStatus_t*>(pvPortMalloc(sizeof(TaskStatus_t) * (freertos_tasks_count + 4U)));
        if (task_status != nullptr) {
            uint32_t total_runtime = 0;
            const UBaseType_t task_count =
                uxTaskGetSystemState(task_status, freertos_tasks_count + 4U, &total_runtime);
            if (task_count > 0) {
                uint64_t idle_runtime[kMaxCoresSupported] = {};
                char idle_task_name[kMaxCoresSupported][8];
                for (size_t core = 0; core < core_count; ++core) {
                    snprintf(idle_task_name[core], sizeof(idle_task_name[core]), "IDLE%u",
                             static_cast<unsigned>(core));
                }
                const uint64_t total_runtime_u64 = static_cast<uint64_t>(total_runtime);
                // Denominator for per-task CPU % must be the FreeRTOS run-time counter
                // delta over the whole sample window (same basis as per-core usage below).
                // Do NOT use a running sum of task deltas inside the emit loop — that made
                // early tasks (often hw_metrics itself) report ~100% while cores were idle.
                uint64_t total_runtime_delta = 0;
                if (g_cpu_usage_state.initialized
                    && total_runtime_u64 > g_cpu_usage_state.prev_total_runtime) {
                    total_runtime_delta =
                        total_runtime_u64 - g_cpu_usage_state.prev_total_runtime;
                }

                int tasks_payload_len = snprintf(
                    g_payload_buf,
                    HEAPWATCH_MAX_PAYLOAD_SIZE,
                    "{\"tsms\":%" PRIu64 ",\"utms\":%" PRIu64 ",\"tasks\":[",
                    tsms,
                    utms);
                bool first_task_json = true;

                for (UBaseType_t i = 0; i < task_count; ++i) {
                    const TaskStatus_t& task = task_status[i];
                    if (task.eCurrentState == eReady) {
                        freertos_ready_tasks_count++;
                    }
                    if (task.pcTaskName != nullptr) {
                        for (size_t core = 0; core < core_count; ++core) {
                            if (strcmp(task.pcTaskName, idle_task_name[core]) == 0) {
                                idle_runtime[core] = static_cast<uint64_t>(task.ulRunTimeCounter);
                                break;
                            }
                        }
                    }

                    const uint64_t current_runtime = static_cast<uint64_t>(task.ulRunTimeCounter);
                    const uint64_t runtime_delta =
                        task_runtime_delta_by_name(task.pcTaskName, current_runtime);

                    uint32_t stack_total_bytes = 0;
                    if (task.xHandle != nullptr) {
                        uint8_t* stack_start = pxTaskGetStackStart(task.xHandle);
                        if (stack_start != nullptr) {
                            stack_total_bytes = heap_caps_get_allocated_size(stack_start);
                        }
                    }

                    const uint32_t stack_hwm_free_bytes =
                        static_cast<uint32_t>(task.usStackHighWaterMark) * sizeof(StackType_t);
                    uint32_t stack_used_bytes = 0;
                    if (stack_total_bytes > stack_hwm_free_bytes) {
                        stack_used_bytes = stack_total_bytes - stack_hwm_free_bytes;
                    }

                    if (stack_total_bytes == 0) {
                        continue;
                    }

                    float task_cpu_pct = 0.0f;
                    if (total_runtime_delta > 0) {
                        task_cpu_pct = static_cast<float>(
                            (static_cast<double>(runtime_delta)
                                / static_cast<double>(total_runtime_delta))
                            * 100.0);
                        if (task_cpu_pct < 0.0f) {
                            task_cpu_pct = 0.0f;
                        }
                        if (task_cpu_pct > 100.0f) {
                            task_cpu_pct = 100.0f;
                        }
                    }

                    if (tasks_payload_len < 0
                        || tasks_payload_len >= static_cast<int>(HEAPWATCH_MAX_PAYLOAD_SIZE - 96)) {
                        continue;
                    }

                    const char* task_name = task.pcTaskName != nullptr ? task.pcTaskName : "?";
                    const int written = snprintf(
                        g_payload_buf + tasks_payload_len,
                        HEAPWATCH_MAX_PAYLOAD_SIZE - tasks_payload_len,
                        "%s{\"n\":\"%s\",\"st\":%" PRIu32 ",\"su\":%" PRIu32 ",\"cpu\":%.2f}",
                        first_task_json ? "" : ",",
                        task_name,
                        stack_total_bytes,
                        stack_used_bytes,
                        static_cast<double>(task_cpu_pct));
                    if (written <= 0) {
                        continue;
                    }

                    tasks_payload_len += written;
                    first_task_json = false;
                }

#if CONFIG_HEAPWATCH_ENABLE_CPU_SNAPSHOTS
                if (total_runtime_delta > 0) {
                    const double total_runtime_delta_d = static_cast<double>(total_runtime_delta);
                    for (size_t core = 0; core < core_count; ++core) {
                        const uint64_t delta_idle =
                            idle_runtime[core] - g_cpu_usage_state.prev_idle_runtime[core];
                        const double idle_pct =
                            (static_cast<double>(delta_idle) / total_runtime_delta_d) * 100.0;
                        float usage_pct = static_cast<float>(100.0 - idle_pct);
                        if (usage_pct < 0.0f) usage_pct = 0.0f;
                        if (usage_pct > 100.0f) usage_pct = 100.0f;
                        cpu_usage_pct[core] = usage_pct;
                    }
                }
#endif  /* CONFIG_HEAPWATCH_ENABLE_CPU_SNAPSHOTS */

                // Advance the shared baseline after both task and per-core % use it.
                // Must run even when only TASK_SNAPSHOTS is enabled, or task CPU stays 0.
                g_cpu_usage_state.initialized = true;
                g_cpu_usage_state.prev_total_runtime = total_runtime_u64;
#if CONFIG_HEAPWATCH_ENABLE_CPU_SNAPSHOTS
                for (size_t core = 0; core < core_count; ++core) {
                    g_cpu_usage_state.prev_idle_runtime[core] = idle_runtime[core];
                }
#endif  /* CONFIG_HEAPWATCH_ENABLE_CPU_SNAPSHOTS */

#if CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS
                if (!first_task_json
                    && tasks_payload_len > 0
                    && tasks_payload_len < static_cast<int>(HEAPWATCH_MAX_PAYLOAD_SIZE - 4)) {
                    tasks_payload_len += snprintf(
                        g_payload_buf + tasks_payload_len,
                        HEAPWATCH_MAX_PAYLOAD_SIZE - tasks_payload_len,
                        "]}");
                    enqueue_mqtt_message(MQTT_MESSAGE_TYPE_TASKS, g_payload_buf);
                }
#else
                (void)first_task_json;
                (void)tasks_payload_len;
#endif  /* CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS */
            }
            vPortFree(task_status);
        }
#endif

        // CPU1: emit one array entry per actual core rather than fixed cpu0/cpu1 fields.
        char cpu_usage_array[kMaxCoresSupported * 8 + 2] = {};
        {
            size_t pos = 0;
            cpu_usage_array[pos++] = '[';
            for (size_t core = 0; core < core_count; ++core) {
                pos += snprintf(cpu_usage_array + pos, sizeof(cpu_usage_array) - pos, "%s%.2f",
                                 (core > 0) ? "," : "", static_cast<double>(cpu_usage_pct[core]));
            }
            snprintf(cpu_usage_array + pos, sizeof(cpu_usage_array) - pos, "]");
        }

#ifdef HEAPWATCH_USE_JSON_FORMAT
        snprintf(g_payload_buf, HEAPWATCH_MAX_PAYLOAD_SIZE,
                 "{\"tsms\":%" PRIu64 ",\"utms\":%" PRIu64 ",\"uptime_s\":%" PRIu32 ",\"heap_used_bytes\":%" PRIu32
                 ",\"heap_used_bytes_max\":%" PRIu32 ",\"heap_largest_free_block_bytes\":%" PRIu32
                 ",\"heap_largest_free_block_bytes_max\":%" PRIu32
                 ",\"internal_heap_used_bytes\":%" PRIu32 ",\"internal_heap_used_bytes_max\":%" PRIu32
                 ",\"psram_used_bytes\":%" PRIu32 ",\"psram_used_bytes_max\":%" PRIu32
                 ",\"cpu_usage_pct\":%s"
                 ",\"freertos_tasks_count\":%" PRIu16 ",\"freertos_ready_tasks_count\":%" PRIu16
                 ",\"wifi_rssi_dbm\":%d"
                 ",\"wifi_reconnects_total\":%" PRIu32
                 ",\"log_lines_dropped_total\":%" PRIu32
                 ",\"log_mqtt_dropped_total\":%" PRIu32 "}",
                 tsms,
                 utms,
                 uptime_s,
                 heap_used_bytes,
                 heap_used_bytes_max,
                 heap_largest_free_block_bytes,
                 heap_largest_free_block_bytes_max,
                 internal_heap_used_bytes,
                 internal_heap_used_bytes_max,
                 psram_used_bytes,
                 psram_used_bytes_max,
                 cpu_usage_array,
                 freertos_tasks_count,
                 freertos_ready_tasks_count,
                 static_cast<int>(wifi_rssi_dbm),
                 heapwatch::connector_bootstrap::getWifiReconnectsTotal(),
                 heapwatch::connector_bootstrap::getLogLinesDroppedTotal(),
                 heapwatch::connector_bootstrap::getLogMqttDroppedTotal());
#else
        snprintf(g_payload_buf, HEAPWATCH_MAX_PAYLOAD_SIZE,
                 "tsms=%" PRIu64 "|utms=%" PRIu64 "|uptime_s=%" PRIu32 "|heap_used_bytes=%" PRIu32
                 "%sheap_used_bytes_max=%" PRIu32 "%sheap_largest_free_block_bytes=%" PRIu32
                 "%sheap_largest_free_block_bytes_max=%" PRIu32
                 "%sinternal_heap_used_bytes=%" PRIu32 "%sinternal_heap_used_bytes_max=%" PRIu32
                 "%spsram_used_bytes=%" PRIu32 "%spsram_used_bytes_max=%" PRIu32
                 "%scpu_usage_pct=%s"
                 "%sfreertos_tasks_count=%" PRIu16 "%sfreertos_ready_tasks_count=%" PRIu16
                 "%swifi_rssi_dbm=%d"
                 "%swifi_reconnects_total=%" PRIu32
                 "%slog_lines_dropped_total=%" PRIu32
                 "%slog_mqtt_dropped_total=%" PRIu32,
                 tsms,
                 utms,
                 uptime_s,
                 heap_used_bytes, HEAPWATCH_FLAT_DELIMITER,
                 heap_used_bytes_max, HEAPWATCH_FLAT_DELIMITER,
                 heap_largest_free_block_bytes, HEAPWATCH_FLAT_DELIMITER,
                 heap_largest_free_block_bytes_max, HEAPWATCH_FLAT_DELIMITER,
                 internal_heap_used_bytes, HEAPWATCH_FLAT_DELIMITER,
                 internal_heap_used_bytes_max, HEAPWATCH_FLAT_DELIMITER,
                 psram_used_bytes, HEAPWATCH_FLAT_DELIMITER,
                 psram_used_bytes_max, HEAPWATCH_FLAT_DELIMITER,
                 cpu_usage_array, HEAPWATCH_FLAT_DELIMITER,
                 freertos_tasks_count, HEAPWATCH_FLAT_DELIMITER,
                 freertos_ready_tasks_count, HEAPWATCH_FLAT_DELIMITER,
                 static_cast<int>(wifi_rssi_dbm), HEAPWATCH_FLAT_DELIMITER,
                 heapwatch::connector_bootstrap::getWifiReconnectsTotal(), HEAPWATCH_FLAT_DELIMITER,
                 heapwatch::connector_bootstrap::getLogLinesDroppedTotal(), HEAPWATCH_FLAT_DELIMITER,
                 heapwatch::connector_bootstrap::getLogMqttDroppedTotal());
#endif

        enqueue_mqtt_message(MQTT_MESSAGE_TYPE_PARAMETER, g_payload_buf);

        // STORAGE1: update storage_free_bytes/storage_used_bytes in the parameter map
        // (no-op internally if HEAPWATCH_ENABLE_STORAGE_STATS=n or nothing is mounted)
        // before flushing it below, so the round below carries the freshest values.
        heapwatch::storage_stats::collectAndReport();

        // PARAM1 (Phase 5c): flush the generic slug->value parameter map (POW1
        // power API, CELL1 cellular signal, STORAGE1 storage stats, any other
        // updateParameter*() call site) as its own batched publish. No-op if nothing
        // has ever called updateParameter*() this boot -- distinct from the
        // hand-rolled heap/Wi-Fi/FreeRTOS blob above, which is always sent.
        HeapwatchConnector::getInstance().flushParameters();

#if CONFIG_HEAPWATCH_ENABLE_HEAP_INTEGRITY_CHECK
        // HEAP1: default-off proactive corruption check — heap_caps_check_integrity() walks
        // the full heap, so this has a real CPU cost and stays opt-in.
        if (!heap_caps_check_integrity(MALLOC_CAP_DEFAULT, true)) {
            ESP_LOGE("MetricsCollector", "Heap corruption detected by heap_caps_check_integrity()");
            HeapwatchConnector::getInstance().queueEvent(HEAPWATCH_EVENT_HEAP_CORRUPTION_DETECTED);
        }
#endif  /* CONFIG_HEAPWATCH_ENABLE_HEAP_INTEGRITY_CHECK */

#if CONFIG_HEAPWATCH_DEBUG_PAYLOAD_SIZE_HISTOGRAM
        // M2 (Phase 2b): dump the histogram every ~15 collection cycles rather than
        // every cycle, so it stays readable on serial even at short metrics intervals.
        static uint32_t s_histogram_dump_counter = 0;
        if (++s_histogram_dump_counter >= 15) {
            s_histogram_dump_counter = 0;
            heapwatch_debug_dump_payload_histogram();
        }
#endif
    }
}

}  // namespace heapwatch::metrics_collector
