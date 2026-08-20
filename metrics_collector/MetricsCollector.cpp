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
// For licensing inquiries, please contact: admin@heapwatch.com
//

#include "MetricsCollector.h"
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "../heapwatch_connector/HeapwatchConnector.h"
#include "../heapwatch_connector/heapwatch_connector_timing.h"
#include "../heapwatch_dispatcher/HeapwatchDispatcher.h"
#include "../heapwatch_config.h"
#include "../connector_boot_sync/ConnectorBootSync.h"
#include "../connector_settings/ConnectorSettings.h"
#include "../device_network/DeviceNetworkReporter.h"
#include "../storage_stats/StorageStats.h"

namespace heapwatch::metrics_collector {

namespace {
uint64_t g_start_time_us = 0;
char g_payload_buf[HEAPWATCH_MAX_PAYLOAD_SIZE];
MqttMessage g_mqtt_enqueue_msg;

constexpr size_t kMaxTaskRows = 48;

struct PrevTaskRuntime {
    char name[20];
    uint32_t stack_total_bytes;
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

uint8_t g_dirty_task_stack_idx[kMaxTaskRows];
uint8_t g_dirty_task_stack_max_count = 0;

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
        g_prev_task_runtime[g_prev_task_runtime_count].stack_total_bytes = 0;
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

void refresh_connector_gauge_limits(size_t core_count) {
    auto& connector = HeapwatchConnector::getInstance();
    const int64_t internal_total =
        static_cast<int64_t>(heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    const int64_t psram_total =
        static_cast<int64_t>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM));

    connector.setParameterIntMin("heap.internal.used_bytes", 0);
    connector.setParameterIntMax("heap.internal.used_bytes", internal_total);
    connector.setParameterIntMin("heap.psram.used_bytes", 0);
    connector.setParameterIntMax("heap.psram.used_bytes", psram_total);
    connector.setParameterIntMin("heap.internal.largest_free_block_bytes", 0);
    connector.setParameterIntMax("heap.internal.largest_free_block_bytes", internal_total);
    connector.setParameterIntMin("heap.psram.largest_free_block_bytes", 0);
    connector.setParameterIntMax("heap.psram.largest_free_block_bytes", psram_total);

    for (size_t core = 0; core < core_count; ++core) {
        char slug[32] = {};
        snprintf(slug, sizeof(slug), "cpu.%u.usage_pct", static_cast<unsigned>(core));
        connector.setParameterIntMin(slug, 0);
        connector.setParameterIntMax(slug, 100);
    }

    connector.setParameterIntMin("wifi.rssi_dbm", -100);
    connector.setParameterIntMax("wifi.rssi_dbm", -20);
}

void note_task_stack_total(const char* task_name, uint32_t stack_total_bytes) {
    const char* name = (task_name != nullptr && task_name[0] != '\0') ? task_name : "?";
    size_t idx = kMaxTaskRows;
    for (size_t i = 0; i < g_prev_task_runtime_count; ++i) {
        if (strcmp(g_prev_task_runtime[i].name, name) == 0) {
            idx = i;
            break;
        }
    }
    if (idx >= kMaxTaskRows) {
        if (g_prev_task_runtime_count >= kMaxTaskRows) {
            return;
        }
        idx = g_prev_task_runtime_count;
        memset(g_prev_task_runtime[idx].name, 0, sizeof(g_prev_task_runtime[idx].name));
        strncpy(g_prev_task_runtime[idx].name, name, sizeof(g_prev_task_runtime[idx].name) - 1);
        g_prev_task_runtime[idx].runtime = 0;
        g_prev_task_runtime[idx].stack_total_bytes = 0;
        g_prev_task_runtime_count++;
    }

    PrevTaskRuntime& row = g_prev_task_runtime[idx];
    if (row.stack_total_bytes == stack_total_bytes) {
        return;
    }
    row.stack_total_bytes = stack_total_bytes;
    if (g_dirty_task_stack_max_count < kMaxTaskRows) {
        g_dirty_task_stack_idx[g_dirty_task_stack_max_count++] = static_cast<uint8_t>(idx);
    }
}

void enqueue_dirty_task_stack_maxes(uint64_t tsms, uint64_t utms) {
    if (g_dirty_task_stack_max_count == 0) {
        return;
    }

    size_t pos = 0;
    pos += snprintf(g_payload_buf + pos, HEAPWATCH_MAX_PAYLOAD_SIZE - pos,
                    "{\"tsms\":%" PRIu64 ",\"utms\":%" PRIu64, tsms, utms);
    for (size_t i = 0; i < g_dirty_task_stack_max_count && pos < HEAPWATCH_MAX_PAYLOAD_SIZE; ++i) {
        const PrevTaskRuntime& row = g_prev_task_runtime[g_dirty_task_stack_idx[i]];
        pos += snprintf(g_payload_buf + pos, HEAPWATCH_MAX_PAYLOAD_SIZE - pos,
                        ",\"task.%s.stack_used_bytes_max\":%" PRIu32, row.name,
                        row.stack_total_bytes);
    }
    if (pos < HEAPWATCH_MAX_PAYLOAD_SIZE) {
        g_payload_buf[pos++] = '}';
    }
    if (pos < HEAPWATCH_MAX_PAYLOAD_SIZE) {
        g_payload_buf[pos] = '\0';
    } else {
        g_payload_buf[HEAPWATCH_MAX_PAYLOAD_SIZE - 1] = '\0';
    }
    enqueue_mqtt_message(MQTT_MESSAGE_TYPE_PARAMETER, g_payload_buf);
    g_dirty_task_stack_max_count = 0;
}
}  // namespace

void refreshConnectorGaugeLimits() {
    const size_t core_count =
        (portNUM_PROCESSORS <= kMaxCoresSupported) ? portNUM_PROCESSORS : kMaxCoresSupported;
    refresh_connector_gauge_limits(core_count);
}

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

    bool prev_mqtt_connected = g_mqtt_connected;
    bool boot_static_enqueued = false;

    while (heapwatch_collector_running()) {
        const bool mqtt_now = g_mqtt_connected;
        if (heapwatch_should_enqueue_boot_static(prev_mqtt_connected, mqtt_now, boot_static_enqueued)) {
            heapwatch::connector_boot_sync::runBootStaticSync();
            boot_static_enqueued = true;
        } else if (prev_mqtt_connected && !mqtt_now) {
            HeapwatchConnector::getInstance().queueEvent(HEAPWATCH_EVENT_MQTT_DISCONNECTED);
        } else if (heapwatch::device_network::consumeTopologyDirty() && mqtt_now) {
            heapwatch::device_network::publishFullSync();
        }
        prev_mqtt_connected = mqtt_now;

        vTaskDelay(pdMS_TO_TICKS(heapwatch::connector_settings::telemetryIntervalMs()));
        if (!heapwatch_collector_running()) {
            break;
        }

        const uint32_t uptime_s = static_cast<uint32_t>(
            (static_cast<uint64_t>(esp_timer_get_time()) - g_start_time_us) / 1000000ULL);
        uint64_t tsms = 0;
        uint64_t utms = 0;
        heapwatch_get_ingress_timing_ms(&tsms, &utms);
        g_dirty_task_stack_max_count = 0;

        const uint32_t internal_heap_free_bytes =
            static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        const uint32_t internal_heap_total_bytes =
            static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
        const uint32_t internal_heap_used_bytes = internal_heap_total_bytes - internal_heap_free_bytes;
        const uint32_t internal_largest_free_block_bytes =
            static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

        const uint32_t psram_free_bytes =
            static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        const uint32_t psram_total_bytes =
            static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
        const uint32_t psram_used_bytes =
            (psram_total_bytes > psram_free_bytes) ? (psram_total_bytes - psram_free_bytes) : 0;
        const uint32_t psram_largest_free_block_bytes =
            static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

        int16_t wifi_rssi_dbm = 0;
        wifi_ap_record_t ap_info = {};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            wifi_rssi_dbm = ap_info.rssi;
        }

        uint16_t freertos_ready_tasks_count = 0;
        // CPU1: one entry per actual core (portNUM_PROCESSORS), not a fixed cpu0/cpu1 pair.
        const size_t core_count =
            (portNUM_PROCESSORS <= kMaxCoresSupported) ? portNUM_PROCESSORS : kMaxCoresSupported;
        float cpu_usage_pct[kMaxCoresSupported] = {};
        refresh_connector_gauge_limits(core_count);

        // Ready-task count, task stack maxes, TASKS snapshots, and CPU-usage share
        // this FreeRTOS runtime-stats scan. TASKS / CPU fields stay gated; the
        // scan itself always runs when trace facility is on so ready-count and
        // stack limits stay current.
#if (configUSE_TRACE_FACILITY == 1)
        const UBaseType_t task_table_len =
            static_cast<UBaseType_t>(uxTaskGetNumberOfTasks()) + 4U;
        TaskStatus_t* task_status =
            static_cast<TaskStatus_t*>(pvPortMalloc(sizeof(TaskStatus_t) * task_table_len));
        if (task_status != nullptr) {
            uint32_t total_runtime = 0;
            const UBaseType_t task_count =
                uxTaskGetSystemState(task_status, task_table_len, &total_runtime);
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

                    note_task_stack_total(task.pcTaskName, stack_total_bytes);

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
                 "{\"tsms\":%" PRIu64 ",\"utms\":%" PRIu64
                 ",\"device.uptime_s\":%" PRIu32
                 ",\"heap.internal.used_bytes\":%" PRIu32
                 ",\"heap.psram.used_bytes\":%" PRIu32
                 ",\"heap.internal.largest_free_block_bytes\":%" PRIu32
                 ",\"heap.psram.largest_free_block_bytes\":%" PRIu32
                 ",\"cpu_usage_pct\":%s"
                 ",\"wifi.rssi_dbm\":%d"
                 ",\"freertos.ready_tasks_count\":%" PRIu16 "}",
                 tsms,
                 utms,
                 uptime_s,
                 internal_heap_used_bytes,
                 psram_used_bytes,
                 internal_largest_free_block_bytes,
                 psram_largest_free_block_bytes,
                 cpu_usage_array,
                 static_cast<int>(wifi_rssi_dbm),
                 freertos_ready_tasks_count);
#else
        snprintf(g_payload_buf, HEAPWATCH_MAX_PAYLOAD_SIZE,
                 "tsms=%" PRIu64 "|utms=%" PRIu64
                 "|device.uptime_s=%" PRIu32
                 "%sheap.internal.used_bytes=%" PRIu32
                 "%sheap.psram.used_bytes=%" PRIu32
                 "%sheap.internal.largest_free_block_bytes=%" PRIu32
                 "%sheap.psram.largest_free_block_bytes=%" PRIu32
                 "%scpu_usage_pct=%s"
                 "%swifi.rssi_dbm=%d"
                 "%sfreertos.ready_tasks_count=%" PRIu16,
                 tsms,
                 utms,
                 uptime_s,
                 HEAPWATCH_FLAT_DELIMITER, internal_heap_used_bytes,
                 HEAPWATCH_FLAT_DELIMITER, psram_used_bytes,
                 HEAPWATCH_FLAT_DELIMITER, internal_largest_free_block_bytes,
                 HEAPWATCH_FLAT_DELIMITER, psram_largest_free_block_bytes,
                 HEAPWATCH_FLAT_DELIMITER, cpu_usage_array,
                 HEAPWATCH_FLAT_DELIMITER, static_cast<int>(wifi_rssi_dbm),
                 HEAPWATCH_FLAT_DELIMITER, freertos_ready_tasks_count);
#endif

        enqueue_mqtt_message(MQTT_MESSAGE_TYPE_PARAMETER, g_payload_buf);
        enqueue_dirty_task_stack_maxes(tsms, utms);

        // STORAGE1: update storage.free_bytes / storage.used_bytes in the parameter map
        // (no-op internally if HEAPWATCH_ENABLE_STORAGE_STATS=n or nothing is mounted)
        // before flushing it below, so the round below carries the freshest values.
        heapwatch::storage_stats::collectAndReport();

        // PARAM1 (Phase 5c): flush the generic slug->value parameter map (POW1
        // power API, CELL1 cellular signal, STORAGE1 storage stats, connector
        // gauge limits, any other updateParameter*() / setParameter*Max call
        // site) as its own batched publish. No-op if nothing is due this round.
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
