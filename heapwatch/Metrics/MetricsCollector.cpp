// Copyright 2026 Heapwatch
// SPDX-License-Identifier: Apache-2.0

#include "heapwatch/Metrics/MetricsCollector.h"
#include "heapwatch/Metrics/MetricsCollectorInternal.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/freertos_debug.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "heapwatch/Mqtt/MqttTelemetryClient.h"
#include "heapwatch/Helpers/FreeRtosAlloc.h"
#include "heapwatch/Helpers/MemoryManager/HeapwatchMemoryManager.h"
#include "heapwatch/Helpers/DynamicParameterSegment.h"
#include "heapwatch/Helpers/IngressTimestamps.h"
#include "heapwatch/Mqtt/HeapwatchDispatcher.h"
#include "heapwatch_config.h"
#include "heapwatch/Lifecycle/OncePerBootTelemetry/OncePerBootTelemetry.h"
#include "heapwatch/Settings/ConnectorSettings.h"
#include "heapwatch/Fs/Volumes/FsVolumeManager.h"
#include "heapwatch/Network/DeviceNetworkReporter.h"
#include "heapwatch/Network/NetifTraffic/NetifTraffic.h"

namespace Heapwatch::Metrics {

uint64_t g_start_time_us = 0;
char* g_payload_buf = nullptr;
MqttMessage* g_mqtt_enqueue_msg = nullptr;


struct CpuUsageState {
    uint64_t prev_total_runtime = 0;
    uint64_t prev_idle_runtime[MAX_CORES_SUPPORTED] = {};
    bool initialized = false;
};
CpuUsageState g_cpu_usage_state;

#if CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS
TaskStats* g_task_stats = nullptr;
size_t g_task_stats_count = 0;
bool g_task_table_full_logged = false;
uint8_t g_dirty_task_stack_idx[HEAPWATCH_MAX_TASK_ROWS];
uint8_t g_dirty_task_stack_max_count = 0;

struct TaskStackSample {
    const uint8_t* lowest = nullptr;
    const uint8_t* highest = nullptr;
    uint32_t total = 0;
    uint32_t current = 0;
    uint32_t peak = 0;
};

// pxTopOfStack is the stack pointer saved on the task's last context switch, so for the task doing
// the measuring it is one switch out of date — read the live frame pointer instead. A task running
// on another core stays one switch behind; that is inherent to sampling it from the outside, and
// the saved pointer sits below the context-save frame, so this errs high.
const uint8_t* stack_pointer_of(TaskHandle_t handle, const TaskSnapshot_t& snapshot) {
    if (handle == xTaskGetCurrentTaskHandle()) {
        return static_cast<const uint8_t*>(__builtin_frame_address(0));
    }
    return reinterpret_cast<const uint8_t*>(snapshot.pxTopOfStack);
}

// Stack geometry read straight out of the TCB. pxStackBase (lowest address) is always populated by
// FreeRTOS, and vTaskGetSnapshot() hands over pxEndOfStack / pxTopOfStack, which TaskStatus_t omits
// on downward-growing ports like Xtensa and RISC-V. Nothing here asks the heap allocator about the
// stack, so a task created with xTaskCreateStatic() — whose stack lives in .bss and belongs to no
// heap — measures exactly like a heap-allocated one.
TaskStackSample measure_task_stack(const TaskStatus_t& task) {
    TaskStackSample sample;

    if (task.xHandle == nullptr || task.pxStackBase == nullptr) {
        return sample;
    }

    TaskSnapshot_t snapshot = {};
    if (vTaskGetSnapshot(task.xHandle, &snapshot) != pdTRUE || snapshot.pxEndOfStack == nullptr) {
        return sample;
    }

    const uint8_t* lowest = reinterpret_cast<const uint8_t*>(task.pxStackBase);
    const uint8_t* highest = reinterpret_cast<const uint8_t*>(snapshot.pxEndOfStack);
    if (highest <= lowest) {
        return sample;
    }

    sample.lowest = lowest;
    sample.highest = highest;
    sample.total = static_cast<uint32_t>(highest - lowest) + 1U;

    // Peak is FreeRTOS' high-water mark: the deepest point the task ever *wrote* to, found by
    // scanning the 0xa5 fill pattern. It says nothing about stack merely reserved by a large
    // frame, which is what current measures.
    const uint32_t hwm_free_bytes =
        static_cast<uint32_t>(task.usStackHighWaterMark) * sizeof(StackType_t);
    if (sample.total > hwm_free_bytes) {
        sample.peak = sample.total - hwm_free_bytes;
    }

    const uint8_t* stack_pointer = stack_pointer_of(task.xHandle, snapshot);
    if (stack_pointer >= lowest && stack_pointer <= highest) {
        sample.current = static_cast<uint32_t>(highest - stack_pointer) + 1U;
    }

    return sample;
}

/** Row for a task name, creating it on first sight. Returns nullptr once the table is full. */
TaskStats* acquire_task_row(const char* task_name) {
    const char* name = (task_name != nullptr && task_name[0] != '\0') ? task_name : "?";

    for (size_t i = 0; i < g_task_stats_count; ++i) {
        if (strcmp(g_task_stats[i].name, name) == 0) {
            return &g_task_stats[i];
        }
    }

    if (g_task_stats_count >= HEAPWATCH_MAX_TASK_ROWS) {
        if (!g_task_table_full_logged) {
            ESP_LOGE(TAG,
                     "task stats table full (%" PRIu32
                     " rows) — live task '%s' dropped from tracking (raise HEAPWATCH_MAX_TASK_ROWS)",
                     static_cast<uint32_t>(HEAPWATCH_MAX_TASK_ROWS), name);
            g_task_table_full_logged = true;
        }
        return nullptr;
    }

    TaskStats& row = g_task_stats[g_task_stats_count++];
    row = TaskStats{};
    strncpy(row.name, name, sizeof(row.name) - 1);
    return &row;
}
#endif  /* CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS */

void enqueue_mqtt_message(MqttMessageType type, const char* payload) {
    g_mqtt_enqueue_msg->type = type;
    snprintf(g_mqtt_enqueue_msg->payload, sizeof(g_mqtt_enqueue_msg->payload), "%s", payload);

    QueueHandle_t queue = heapwatch_get_large_mqtt_queue();
    if (queue != nullptr) {
        if (!heapwatch_enqueue_mqtt_queue_message(queue, g_mqtt_enqueue_msg, 0)) {
            ESP_LOGW(TAG, "large MQTT queue full — dropped %s payload",
                     type == MQTT_MESSAGE_TYPE_TASKS ? "TASKS" : "PARAMETER");
        }
    }
}

static_assert(HEAPWATCH_MAX_PAYLOAD_SIZE >= HEAPWATCH_LARGE_PAYLOAD_SIZE,
              "g_payload_buf must cover the large MQTT queue slot");

// LargeMqttMessage.payload is HEAPWATCH_LARGE_PAYLOAD_SIZE and is filled with snprintf,
// so the JSON string (including the closer) must be at most that size minus the NUL.
constexpr size_t LARGE_JSON_MAX = HEAPWATCH_LARGE_PAYLOAD_SIZE - 1;
constexpr int32_t PARAMETER_JSON_CLOSE_LEN = 1;   // }

bool append_json_chunk(int32_t* payload_len, const char* chunk, int32_t chunk_len, int32_t close_len) {
    if (payload_len == nullptr || chunk == nullptr || chunk_len <= 0) {
        return false;
    }
    if (*payload_len < 0
        || *payload_len + chunk_len + close_len > static_cast<int32_t>(LARGE_JSON_MAX)) {
        return false;
    }
    memcpy(g_payload_buf + *payload_len, chunk, static_cast<size_t>(chunk_len));
    *payload_len += chunk_len;
    return true;
}

#if CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS
constexpr int32_t TASKS_JSON_CLOSE_LEN = 2;       // ]}

constexpr size_t TASKS_JSON_HEADER_MAX =
    sizeof("{\"tsms\":18446744073709551615,\"utms\":18446744073709551615,"
           "\"boot_cycle\":4294967295,\"tasks\":[") - 1;
constexpr size_t TASK_NAME_MAX_CHARS = sizeof(TaskStats::name) - 1;
constexpr size_t TASK_JSON_ROW_MAX =
    sizeof(",{\"name\":\"\",\"total\":4294967295,\"now\":4294967295,\"min\":4294967295,"
           "\"max\":4294967295,\"peak\":4294967295,\"cpu\":100.00,\"exists\":false}")
    - 1 + TASK_NAME_MAX_CHARS;

static_assert(TASKS_JSON_HEADER_MAX + TASK_JSON_ROW_MAX + TASKS_JSON_CLOSE_LEN <= LARGE_JSON_MAX,
              "HEAPWATCH_LARGE_PAYLOAD_SIZE cannot hold one TASKS object");

// How many TASKS objects go in one MQTT_MESSAGE_TYPE_TASKS publish. Each publish is a
// complete, independently ingestible subset — later messages do not replace earlier ones.
constexpr size_t MAX_TASKS_PER_PUBLISH =
    (LARGE_JSON_MAX - TASKS_JSON_HEADER_MAX - static_cast<size_t>(TASKS_JSON_CLOSE_LEN))
    / TASK_JSON_ROW_MAX;
static_assert(MAX_TASKS_PER_PUBLISH >= 1, "large MQTT slot cannot hold a single TASKS row");

constexpr size_t TASK_JSON_ROW_BUF = TASK_JSON_ROW_MAX + 8;

int32_t start_tasks_payload(uint64_t tsms, uint64_t utms) {
    char wall_clock[HEAPWATCH_WALL_CLOCK_VALUE_BUFFER_SIZE] = {};
    heapwatch_format_wall_clock_or_null(tsms, wall_clock, sizeof(wall_clock));

    return snprintf(
        g_payload_buf,
        HEAPWATCH_LARGE_PAYLOAD_SIZE,
        "{\"tsms\":%s,\"utms\":%" PRIu64 ",\"boot_cycle\":%" PRIu32 ",\"tasks\":[",
        wall_clock,
        utms,
        heapwatch_get_boot_cycle());
}

void close_and_enqueue_tasks(int32_t* payload_len) {
    if (payload_len == nullptr || *payload_len <= 0) {
        return;
    }

    if (*payload_len + TASKS_JSON_CLOSE_LEN <= static_cast<int32_t>(LARGE_JSON_MAX)) {
        *payload_len += snprintf(
            g_payload_buf + *payload_len,
            HEAPWATCH_LARGE_PAYLOAD_SIZE - static_cast<size_t>(*payload_len),
            "]}");
    } else {
        g_payload_buf[LARGE_JSON_MAX] = '\0';
    }
    enqueue_mqtt_message(MQTT_MESSAGE_TYPE_TASKS, g_payload_buf);
}

int32_t format_task_json_row(
    char* out,
    size_t out_len,
    bool leading_comma,
    const char* task_name,
    uint32_t total,
    uint32_t now,
    uint32_t min_bytes,
    uint32_t max_bytes,
    uint32_t peak,
    float cpu_pct,
    bool exists) {
    return snprintf(
        out,
        out_len,
        "%s{\"name\":\"%s\",\"total\":%" PRIu32 ",\"now\":%" PRIu32
        ",\"min\":%" PRIu32 ",\"max\":%" PRIu32 ",\"peak\":%" PRIu32
        ",\"cpu\":%.2f,\"exists\":%s}",
        leading_comma ? "," : "",
        task_name,
        total,
        now,
        min_bytes,
        max_bytes,
        peak,
        static_cast<double>(cpu_pct),
        exists ? "true" : "false");
}

struct TasksPublish {
    uint64_t tsms = 0;
    uint64_t utms = 0;
    int32_t len = 0;
    size_t rows = 0;
};

void tasks_publish_begin(TasksPublish* pub, uint64_t tsms, uint64_t utms) {
    pub->tsms = tsms;
    pub->utms = utms;
    pub->len = start_tasks_payload(tsms, utms);
    pub->rows = 0;
}

void tasks_publish_close(TasksPublish* pub) {
    if (pub->rows == 0) {
        return;
    }
    close_and_enqueue_tasks(&pub->len);
    pub->len = 0;
    pub->rows = 0;
}

void tasks_publish_append(
    TasksPublish* pub,
    const char* task_name,
    uint32_t total,
    uint32_t now,
    uint32_t min_bytes,
    uint32_t max_bytes,
    uint32_t peak,
    float cpu_pct,
    bool exists) {
    char row[TASK_JSON_ROW_BUF];
    const bool slot_full =
        pub->rows > 0
        && pub->len + static_cast<int32_t>(TASK_JSON_ROW_MAX) + TASKS_JSON_CLOSE_LEN
            > static_cast<int32_t>(LARGE_JSON_MAX);
    if (slot_full) {
        tasks_publish_close(pub);
        tasks_publish_begin(pub, pub->tsms, pub->utms);
    }

    int32_t row_len = format_task_json_row(
        row,
        sizeof(row),
        pub->rows > 0,
        task_name,
        total,
        now,
        min_bytes,
        max_bytes,
        peak,
        cpu_pct,
        exists);
    if (row_len <= 0 || row_len >= static_cast<int32_t>(sizeof(row))) {
        return;
    }

    if (!append_json_chunk(&pub->len, row, row_len, TASKS_JSON_CLOSE_LEN)) {
        if (pub->rows == 0) {
            ESP_LOGW(TAG, "task '%s' does not fit one TASKS slot", task_name);
            return;
        }
        tasks_publish_close(pub);
        tasks_publish_begin(pub, pub->tsms, pub->utms);
        row_len = format_task_json_row(
            row,
            sizeof(row),
            false,
            task_name,
            total,
            now,
            min_bytes,
            max_bytes,
            peak,
            cpu_pct,
            exists);
        if (row_len <= 0
            || row_len >= static_cast<int32_t>(sizeof(row))
            || !append_json_chunk(&pub->len, row, row_len, TASKS_JSON_CLOSE_LEN)) {
            ESP_LOGW(TAG, "task '%s' does not fit one TASKS slot", task_name);
            return;
        }
    }
    pub->rows++;
}
#endif  /* CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS */

void refresh_connector_gauge_limits(size_t core_count) {
    auto& connector = MqttTelemetryClient::getInstance();
#if CONFIG_HEAPWATCH_ENABLE_HEAP_SNAPSHOTS
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
#endif
#if CONFIG_HEAPWATCH_ENABLE_CPU_SNAPSHOTS
    for (size_t core = 0; core < core_count; ++core) {
        char slug[32] = {};
        snprintf(slug, sizeof(slug), "cpu.%" PRIu32 ".usage_pct", static_cast<uint32_t>(core));
        connector.setParameterIntMin(slug, 0);
        connector.setParameterIntMax(slug, 100);
    }
#else
    (void)core_count;
#endif

    connector.setParameterIntMin("wifi.rssi_dbm", -100);
    connector.setParameterIntMax("wifi.rssi_dbm", -20);
}

/** Record the task's configured depth and queue a limits publish when it changed. */
#if CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS
void note_task_stack_total(TaskStats& row, uint32_t stack_total_bytes) {
    if (row.stack_total_bytes == stack_total_bytes) {
        return;
    }
    row.stack_total_bytes = stack_total_bytes;

    const size_t idx = static_cast<size_t>(&row - g_task_stats);
    if (g_dirty_task_stack_max_count < HEAPWATCH_MAX_TASK_ROWS) {
        g_dirty_task_stack_idx[g_dirty_task_stack_max_count++] = static_cast<uint8_t>(idx);
    }
}
#endif  /* CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS */

int32_t start_parameter_payload(uint64_t tsms, uint64_t utms) {
    char wall_clock[HEAPWATCH_WALL_CLOCK_VALUE_BUFFER_SIZE] = {};
    heapwatch_format_wall_clock_or_null(tsms, wall_clock, sizeof(wall_clock));

    return snprintf(
        g_payload_buf,
        HEAPWATCH_LARGE_PAYLOAD_SIZE,
        "{\"tsms\":%s,\"utms\":%" PRIu64 ",\"boot_cycle\":%" PRIu32,
        wall_clock,
        utms,
        heapwatch_get_boot_cycle());
}

void close_and_enqueue_parameter(int32_t* payload_len) {
    if (payload_len == nullptr || *payload_len <= 0) {
        return;
    }

    if (*payload_len + PARAMETER_JSON_CLOSE_LEN <= static_cast<int32_t>(LARGE_JSON_MAX)) {
        g_payload_buf[(*payload_len)++] = '}';
        g_payload_buf[*payload_len] = '\0';
    } else {
        g_payload_buf[LARGE_JSON_MAX] = '\0';
    }
    enqueue_mqtt_message(MQTT_MESSAGE_TYPE_PARAMETER, g_payload_buf);
}

#if CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS
void publish_all_task_stats(uint64_t tsms, uint64_t utms) {
    TasksPublish tasks_pub;
    tasks_publish_begin(&tasks_pub, tsms, utms);
    for (size_t ti = 0; ti < g_task_stats_count; ++ti) {
        const TaskStats& row = g_task_stats[ti];
        if (!row.stack_range_seeded) {
            continue;
        }
        tasks_publish_append(
            &tasks_pub,
            row.name,
            row.stack_total_bytes,
            row.stack_current_bytes,
            row.stack_min_bytes,
            row.stack_max_bytes,
            row.stack_peak_bytes,
            row.last_cpu_pct,
            row.exists);
    }
    tasks_publish_close(&tasks_pub);
}
#endif  /* CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS */

bool run_freertos_metrics_scan(
    size_t core_count,
    FreertosMetricsScanResult* out,
    bool publish_tasks,
    uint64_t tsms,
    uint64_t utms) {
    if (out == nullptr) {
        return false;
    }
    *out = FreertosMetricsScanResult{};

#if (configUSE_TRACE_FACILITY != 1)
    (void)core_count;
    (void)publish_tasks;
    (void)tsms;
    (void)utms;
    return false;
#else
    Heapwatch::Helpers::ConnectorMemoryBlock* ws = Heapwatch::Helpers::MemoryManager::get();
    UBaseType_t task_table_len =
        static_cast<UBaseType_t>(uxTaskGetNumberOfTasks()) + 4U;
    if (task_table_len > HEAPWATCH_MAX_TASK_STATUS_SCAN) {
        task_table_len = HEAPWATCH_MAX_TASK_STATUS_SCAN;
    }
    TaskStatus_t* task_status = (ws != nullptr) ? ws->task_status_scan : nullptr;
    if (task_status == nullptr) {
        return false;
    }

    uint32_t total_runtime = 0;
    const UBaseType_t task_count =
        uxTaskGetSystemState(task_status, task_table_len, &total_runtime);
    if (task_count == 0) {
        return false;
    }

    uint64_t idle_runtime[MAX_CORES_SUPPORTED] = {};
    char idle_task_name[MAX_CORES_SUPPORTED][16];
    for (size_t core = 0; core < core_count; ++core) {
        snprintf(idle_task_name[core], sizeof(idle_task_name[core]), "IDLE%" PRIu32,
                 static_cast<uint32_t>(core));
    }
    const uint64_t total_runtime_u64 = static_cast<uint64_t>(total_runtime);
    uint64_t total_runtime_delta = 0;
    if (g_cpu_usage_state.initialized
        && total_runtime_u64 > g_cpu_usage_state.prev_total_runtime) {
        total_runtime_delta = total_runtime_u64 - g_cpu_usage_state.prev_total_runtime;
    }

#if CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS
    if (g_task_stats != nullptr) {
        for (size_t i = 0; i < g_task_stats_count; ++i) {
            g_task_stats[i].exists = false;
        }
    }
#endif  /* CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS */

    for (UBaseType_t i = 0; i < task_count; ++i) {
        const TaskStatus_t& task = task_status[i];
        if (task.eCurrentState == eReady) {
            out->ready_tasks_count++;
        }
        if (task.pcTaskName != nullptr) {
            for (size_t core = 0; core < core_count; ++core) {
                if (strcmp(task.pcTaskName, idle_task_name[core]) == 0) {
                    idle_runtime[core] = static_cast<uint64_t>(task.ulRunTimeCounter);
                    break;
                }
            }
        }

#if CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS
        if (g_task_stats == nullptr) {
            continue;
        }

        TaskStats* row = acquire_task_row(task.pcTaskName);
        if (row == nullptr) {
            continue;
        }

        const bool new_instance = row->handle != task.xHandle;
        if (new_instance) {
            row->handle = task.xHandle;
            row->stack_total_bytes = 0;
            row->stack_current_bytes = 0;
            row->stack_min_bytes = 0;
            row->stack_max_bytes = 0;
            row->stack_peak_bytes = 0;
            row->last_cpu_pct = 0.0f;
            row->stack_range_seeded = false;
        }

        const uint64_t current_runtime = static_cast<uint64_t>(task.ulRunTimeCounter);
        const uint64_t runtime_delta =
            (!new_instance && current_runtime >= row->runtime)
                ? current_runtime - row->runtime
                : 0;
        row->runtime = current_runtime;

        const TaskStackSample stack = measure_task_stack(task);
        if (stack.total == 0) {
            continue;
        }

        row->stack_lowest = stack.lowest;
        row->stack_highest = stack.highest;
        if (stack.current > 0) {
            row->stack_current_bytes = stack.current;
            if (!row->stack_range_seeded) {
                row->stack_min_bytes = stack.current;
                row->stack_max_bytes = stack.current;
                row->stack_range_seeded = true;
            } else {
                if (stack.current < row->stack_min_bytes) {
                    row->stack_min_bytes = stack.current;
                }
                if (stack.current > row->stack_max_bytes) {
                    row->stack_max_bytes = stack.current;
                }
            }
        }
        row->stack_peak_bytes = stack.peak;
        note_task_stack_total(*row, stack.total);

        float task_cpu_pct = 0.0f;
        if (total_runtime_delta > 0) {
            task_cpu_pct = static_cast<float>(
                (static_cast<double>(runtime_delta) / static_cast<double>(total_runtime_delta))
                * 100.0);
            if (task_cpu_pct < 0.0f) {
                task_cpu_pct = 0.0f;
            }
            if (task_cpu_pct > 100.0f) {
                task_cpu_pct = 100.0f;
            }
        }

        if (!row->stack_range_seeded) {
            continue;
        }

        row->last_cpu_pct = task_cpu_pct;
        row->exists = true;
#endif  /* CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS */
    }

#if CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS
    if (publish_tasks) {
        publish_all_task_stats(tsms, utms);
    }
#else
    (void)publish_tasks;
    (void)tsms;
    (void)utms;
#endif  /* CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS */

#if CONFIG_HEAPWATCH_ENABLE_CPU_SNAPSHOTS
    if (total_runtime_delta > 0) {
        const double total_runtime_delta_d = static_cast<double>(total_runtime_delta);
        for (size_t core = 0; core < core_count; ++core) {
            const uint64_t delta_idle =
                idle_runtime[core] - g_cpu_usage_state.prev_idle_runtime[core];
            const double idle_pct =
                (static_cast<double>(delta_idle) / total_runtime_delta_d) * 100.0;
            float usage_pct = static_cast<float>(100.0 - idle_pct);
            if (usage_pct < 0.0f) {
                usage_pct = 0.0f;
            }
            if (usage_pct > 100.0f) {
                usage_pct = 100.0f;
            }
            out->cpu_usage_pct[core] = usage_pct;
        }
    }
#endif  /* CONFIG_HEAPWATCH_ENABLE_CPU_SNAPSHOTS */

    g_cpu_usage_state.initialized = true;
    g_cpu_usage_state.prev_total_runtime = total_runtime_u64;
#if CONFIG_HEAPWATCH_ENABLE_CPU_SNAPSHOTS
    for (size_t core = 0; core < core_count; ++core) {
        g_cpu_usage_state.prev_idle_runtime[core] = idle_runtime[core];
    }
#endif  /* CONFIG_HEAPWATCH_ENABLE_CPU_SNAPSHOTS */
    return true;
#endif  /* configUSE_TRACE_FACILITY */
}

#if CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS
void enqueue_dirty_task_stack_maxes(uint64_t tsms, uint64_t utms) {
    if (g_dirty_task_stack_max_count == 0) {
        return;
    }

    int32_t pos = start_parameter_payload(tsms, utms);
    int32_t header_len = pos;
    for (size_t i = 0; i < g_dirty_task_stack_max_count; ++i) {
        const TaskStats& row = g_task_stats[g_dirty_task_stack_idx[i]];
        char slug_seg[32];
        if (!Heapwatch::Helpers::normalizeDynamicSegment(row.name, slug_seg, sizeof(slug_seg))) {
            continue;
        }

        char chunk[256];
        const int32_t chunk_len = snprintf(
            chunk,
            sizeof(chunk),
            ",\"dynamic.task.%s.stack_current_bytes_max\":%" PRIu32
            ",\"dynamic.task.%s.stack_min_bytes_max\":%" PRIu32
            ",\"dynamic.task.%s.stack_max_bytes_max\":%" PRIu32
            ",\"dynamic.task.%s.stack_peak_bytes_max\":%" PRIu32,
            slug_seg, row.stack_total_bytes,
            slug_seg, row.stack_total_bytes,
            slug_seg, row.stack_total_bytes,
            slug_seg, row.stack_total_bytes);
        if (chunk_len <= 0 || chunk_len >= static_cast<int32_t>(sizeof(chunk))) {
            continue;
        }

        if (!append_json_chunk(&pos, chunk, chunk_len, PARAMETER_JSON_CLOSE_LEN)) {
            if (pos > header_len) {
                close_and_enqueue_parameter(&pos);
            }
            pos = start_parameter_payload(tsms, utms);
            header_len = pos;
            if (!append_json_chunk(&pos, chunk, chunk_len, PARAMETER_JSON_CLOSE_LEN)) {
                ESP_LOGW(TAG, "stack limit fields for '%s' do not fit one PARAMETER slot",
                         row.name);
            }
        }
    }
    if (pos > header_len) {
        close_and_enqueue_parameter(&pos);
    }
    g_dirty_task_stack_max_count = 0;
}
#endif  /* CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS */

void refreshConnectorGaugeLimits() {
    const size_t core_count =
        (portNUM_PROCESSORS <= MAX_CORES_SUPPORTED) ? portNUM_PROCESSORS : MAX_CORES_SUPPORTED;
    refresh_connector_gauge_limits(core_count);
}

void init() {
    g_start_time_us = static_cast<uint64_t>(esp_timer_get_time());
    Heapwatch::Helpers::ConnectorMemoryBlock* ws = Heapwatch::Helpers::MemoryManager::get();
    g_payload_buf = ws->metrics_payload;
    g_mqtt_enqueue_msg = &ws->metrics_enqueue;
#if CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS
    g_task_stats = ws->task_stats;
    // activateWithBroker runs on hw_bootstrap: seed the table before that task exits so
    // short-lived connector tasks are still reported with exists:false after MQTT is up.
    {
        const size_t core_count =
            (portNUM_PROCESSORS <= MAX_CORES_SUPPORTED) ? portNUM_PROCESSORS : MAX_CORES_SUPPORTED;
        FreertosMetricsScanResult unused;
        (void)run_freertos_metrics_scan(core_count, &unused, false, 0, 0);
    }
#endif
}

}  // namespace Heapwatch::Metrics
