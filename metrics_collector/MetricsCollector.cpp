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

#include "MetricsCollector.h"
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
#include "../heapwatch_connector/HeapwatchConnector.h"
#include "../heapwatch_connector/DynamicParameterSegment.h"
#include "../heapwatch_connector/heapwatch_connector_timing.h"
#include "../heapwatch_dispatcher/HeapwatchDispatcher.h"
#include "../heapwatch_config.h"
#include "../connector_boot_sync/ConnectorBootSync.h"
#include "../connector_settings/ConnectorSettings.h"
#include "../device_network/DeviceNetworkReporter.h"
#include "../device_network/NetifTraffic.h"
namespace heapwatch::metrics_collector {

namespace {
constexpr char kTag[] = "HwMetrics";
uint64_t g_start_time_us = 0;
char g_payload_buf[HEAPWATCH_MAX_PAYLOAD_SIZE];
MqttMessage g_mqtt_enqueue_msg;

constexpr size_t kMaxTaskRows = 48;

// One row per task name, carried across telemetry cycles.
struct TaskStats {
    char name[20];
    TaskHandle_t handle;
    const uint8_t* stack_lowest;
    const uint8_t* stack_highest;
    uint32_t stack_total_bytes;
    uint32_t stack_current_bytes;
    uint32_t stack_min_bytes;
    uint32_t stack_max_bytes;
    uint32_t stack_peak_bytes;
    uint64_t runtime;
    bool stack_range_seeded;
};


constexpr size_t kMaxCoresSupported = 4;

struct CpuUsageState {
    uint64_t prev_total_runtime = 0;
    uint64_t prev_idle_runtime[kMaxCoresSupported] = {};
    bool initialized = false;
};

TaskStats g_task_stats[kMaxTaskRows];
size_t g_task_stats_count = 0;
bool g_task_table_full_logged = false;
CpuUsageState g_cpu_usage_state;

uint8_t g_dirty_task_stack_idx[kMaxTaskRows];
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

    if (g_task_stats_count >= kMaxTaskRows) {
        if (!g_task_table_full_logged) {
            ESP_LOGW(kTag, "task stats table full (%u rows) — '%s' is not reported",
                     static_cast<unsigned>(kMaxTaskRows), name);
            g_task_table_full_logged = true;
        }
        return nullptr;
    }

    TaskStats& row = g_task_stats[g_task_stats_count++];
    row = TaskStats{};
    strncpy(row.name, name, sizeof(row.name) - 1);
    return &row;
}

void enqueue_mqtt_message(MqttMessageType type, const char* payload) {
    g_mqtt_enqueue_msg.type = type;
    snprintf(g_mqtt_enqueue_msg.payload, sizeof(g_mqtt_enqueue_msg.payload), "%s", payload);

    QueueHandle_t queue = heapwatch_get_queue();
    if (queue != nullptr) {
        if (!heapwatch_outbound_send(queue, &g_mqtt_enqueue_msg, 0)) {
            ESP_LOGW(kTag, "large MQTT queue full — dropped %s payload",
                     type == MQTT_MESSAGE_TYPE_TASKS ? "TASKS" : "PARAMETER");
        }
    }
}

static_assert(HEAPWATCH_MAX_PAYLOAD_SIZE >= HEAPWATCH_LARGE_PAYLOAD_SIZE,
              "g_payload_buf must cover the large MQTT queue slot");

// LargeMqttMessage.payload is HEAPWATCH_LARGE_PAYLOAD_SIZE and is filled with snprintf,
// so the JSON string (including the closer) must be at most that size minus the NUL.
constexpr size_t kLargeJsonMax = HEAPWATCH_LARGE_PAYLOAD_SIZE - 1;
constexpr int kParameterJsonCloseLen = 1;   // }

bool append_json_chunk(int* payload_len, const char* chunk, int chunk_len, int close_len) {
    if (payload_len == nullptr || chunk == nullptr || chunk_len <= 0) {
        return false;
    }
    if (*payload_len < 0
        || *payload_len + chunk_len + close_len > static_cast<int>(kLargeJsonMax)) {
        return false;
    }
    memcpy(g_payload_buf + *payload_len, chunk, static_cast<size_t>(chunk_len));
    *payload_len += chunk_len;
    return true;
}

#if CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS
constexpr int kTasksJsonCloseLen = 2;       // ]}

constexpr size_t kTasksJsonHeaderMax =
    sizeof("{\"tsms\":18446744073709551615,\"utms\":18446744073709551615,\"tasks\":[") - 1;
constexpr size_t kTaskNameMaxChars = sizeof(TaskStats::name) - 1;
constexpr size_t kTaskJsonRowMax =
    sizeof(",{\"name\":\"\",\"total\":4294967295,\"now\":4294967295,\"min\":4294967295,"
           "\"max\":4294967295,\"peak\":4294967295,\"cpu\":100.00}")
    - 1 + kTaskNameMaxChars;

static_assert(kTasksJsonHeaderMax + kTaskJsonRowMax + kTasksJsonCloseLen <= kLargeJsonMax,
              "HEAPWATCH_LARGE_PAYLOAD_SIZE cannot hold one TASKS object");

// How many TASKS objects go in one MQTT_MESSAGE_TYPE_TASKS publish. Each publish is a
// complete, independently ingestible subset — later messages do not replace earlier ones.
constexpr size_t kMaxTasksPerPublish =
    (kLargeJsonMax - kTasksJsonHeaderMax - static_cast<size_t>(kTasksJsonCloseLen))
    / kTaskJsonRowMax;
static_assert(kMaxTasksPerPublish >= 1, "large MQTT slot cannot hold a single TASKS row");

constexpr size_t kTaskJsonRowBuf = kTaskJsonRowMax + 8;

int start_tasks_payload(uint64_t tsms, uint64_t utms) {
    return snprintf(
        g_payload_buf,
        HEAPWATCH_LARGE_PAYLOAD_SIZE,
        "{\"tsms\":%" PRIu64 ",\"utms\":%" PRIu64 ",\"tasks\":[",
        tsms,
        utms);
}

void close_and_enqueue_tasks(int* payload_len) {
    if (payload_len == nullptr || *payload_len <= 0) {
        return;
    }

    if (*payload_len + kTasksJsonCloseLen <= static_cast<int>(kLargeJsonMax)) {
        *payload_len += snprintf(
            g_payload_buf + *payload_len,
            HEAPWATCH_LARGE_PAYLOAD_SIZE - static_cast<size_t>(*payload_len),
            "]}");
    } else {
        g_payload_buf[kLargeJsonMax] = '\0';
    }
    enqueue_mqtt_message(MQTT_MESSAGE_TYPE_TASKS, g_payload_buf);
}

int format_task_json_row(
    char* out,
    size_t out_len,
    bool leading_comma,
    const char* task_name,
    uint32_t total,
    uint32_t now,
    uint32_t min_bytes,
    uint32_t max_bytes,
    uint32_t peak,
    float cpu_pct) {
    return snprintf(
        out,
        out_len,
        "%s{\"name\":\"%s\",\"total\":%" PRIu32 ",\"now\":%" PRIu32
        ",\"min\":%" PRIu32 ",\"max\":%" PRIu32 ",\"peak\":%" PRIu32
        ",\"cpu\":%.2f}",
        leading_comma ? "," : "",
        task_name,
        total,
        now,
        min_bytes,
        max_bytes,
        peak,
        static_cast<double>(cpu_pct));
}

struct TasksPublish {
    uint64_t tsms = 0;
    uint64_t utms = 0;
    int len = 0;
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
    float cpu_pct) {
    char row[kTaskJsonRowBuf];
    const bool slot_full =
        pub->rows > 0
        && pub->len + static_cast<int>(kTaskJsonRowMax) + kTasksJsonCloseLen
            > static_cast<int>(kLargeJsonMax);
    if (slot_full) {
        tasks_publish_close(pub);
        tasks_publish_begin(pub, pub->tsms, pub->utms);
    }

    int row_len = format_task_json_row(
        row,
        sizeof(row),
        pub->rows > 0,
        task_name,
        total,
        now,
        min_bytes,
        max_bytes,
        peak,
        cpu_pct);
    if (row_len <= 0 || row_len >= static_cast<int>(sizeof(row))) {
        return;
    }

    if (!append_json_chunk(&pub->len, row, row_len, kTasksJsonCloseLen)) {
        if (pub->rows == 0) {
            ESP_LOGW(kTag, "task '%s' does not fit one TASKS slot", task_name);
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
            cpu_pct);
        if (row_len <= 0
            || row_len >= static_cast<int>(sizeof(row))
            || !append_json_chunk(&pub->len, row, row_len, kTasksJsonCloseLen)) {
            ESP_LOGW(kTag, "task '%s' does not fit one TASKS slot", task_name);
            return;
        }
    }
    pub->rows++;
}
#endif  /* CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS */

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

/** Record the task's configured depth and queue a limits publish when it changed. */
void note_task_stack_total(TaskStats& row, uint32_t stack_total_bytes) {
    if (row.stack_total_bytes == stack_total_bytes) {
        return;
    }
    row.stack_total_bytes = stack_total_bytes;

    const size_t idx = static_cast<size_t>(&row - g_task_stats);
    if (g_dirty_task_stack_max_count < kMaxTaskRows) {
        g_dirty_task_stack_idx[g_dirty_task_stack_max_count++] = static_cast<uint8_t>(idx);
    }
}

int start_parameter_payload(uint64_t tsms, uint64_t utms) {
    return snprintf(
        g_payload_buf,
        HEAPWATCH_LARGE_PAYLOAD_SIZE,
        "{\"tsms\":%" PRIu64 ",\"utms\":%" PRIu64,
        tsms,
        utms);
}

void close_and_enqueue_parameter(int* payload_len) {
    if (payload_len == nullptr || *payload_len <= 0) {
        return;
    }

    if (*payload_len + kParameterJsonCloseLen <= static_cast<int>(kLargeJsonMax)) {
        g_payload_buf[(*payload_len)++] = '}';
        g_payload_buf[*payload_len] = '\0';
    } else {
        g_payload_buf[kLargeJsonMax] = '\0';
    }
    enqueue_mqtt_message(MQTT_MESSAGE_TYPE_PARAMETER, g_payload_buf);
}

void enqueue_dirty_task_stack_maxes(uint64_t tsms, uint64_t utms) {
    if (g_dirty_task_stack_max_count == 0) {
        return;
    }

    int pos = start_parameter_payload(tsms, utms);
    int header_len = pos;
    for (size_t i = 0; i < g_dirty_task_stack_max_count; ++i) {
        const TaskStats& row = g_task_stats[g_dirty_task_stack_idx[i]];
        char slug_seg[32];
        if (!heapwatch::normalizeDynamicSegment(row.name, slug_seg, sizeof(slug_seg))) {
            continue;
        }

        char chunk[256];
        const int chunk_len = snprintf(
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
        if (chunk_len <= 0 || chunk_len >= static_cast<int>(sizeof(chunk))) {
            continue;
        }

        if (!append_json_chunk(&pos, chunk, chunk_len, kParameterJsonCloseLen)) {
            if (pos > header_len) {
                close_and_enqueue_parameter(&pos);
            }
            pos = start_parameter_payload(tsms, utms);
            header_len = pos;
            if (!append_json_chunk(&pos, chunk, chunk_len, kParameterJsonCloseLen)) {
                ESP_LOGW(kTag, "stack limit fields for '%s' do not fit one PARAMETER slot",
                         row.name);
            }
        }
    }
    if (pos > header_len) {
        close_and_enqueue_parameter(&pos);
    }
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

        // Sliced so deinit does not have to wait out a long telemetry interval before
        // the queues this task publishes into can be destroyed.
        const uint32_t interval_ms = heapwatch::connector_settings::telemetryIntervalMs();
        if (interval_ms <= HEAPWATCH_STOP_CHECK_SLICE_MS) {
            vTaskDelay(pdMS_TO_TICKS(interval_ms));
        } else {
            const TickType_t wake_at = xTaskGetTickCount() + pdMS_TO_TICKS(interval_ms);
            while (heapwatch_collector_running() && xTaskGetTickCount() < wake_at) {
                vTaskDelay(pdMS_TO_TICKS(HEAPWATCH_STOP_CHECK_SLICE_MS));
            }
        }
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

#if CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS
                TasksPublish tasks_pub;
                tasks_publish_begin(&tasks_pub, tsms, utms);
#endif  /* CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS */

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

                    TaskStats* row = acquire_task_row(task.pcTaskName);
                    if (row == nullptr) {
                        continue;
                    }

                    // A name can be reused by a task that was deleted and created again. Its stack
                    // lives somewhere else and its run-time counter starts over, so every
                    // statistic accumulated for the previous instance has to go — including the
                    // recorded depth, so the ceiling limits are republished for the new stack.
                    const bool new_instance = row->handle != task.xHandle;
                    if (new_instance) {
                        row->handle = task.xHandle;
                        row->stack_total_bytes = 0;
                        row->stack_current_bytes = 0;
                        row->stack_min_bytes = 0;
                        row->stack_max_bytes = 0;
                        row->stack_peak_bytes = 0;
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
                    // A depth of zero means the stack pointer failed the range check, not that the
                    // task is using nothing, so keep the last good reading rather than report 0.
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

                    if (!row->stack_range_seeded) {
                        continue;
                    }

#if CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS
                    tasks_publish_append(
                        &tasks_pub,
                        task.pcTaskName != nullptr ? task.pcTaskName : "?",
                        stack.total,
                        row->stack_current_bytes,
                        row->stack_min_bytes,
                        row->stack_max_bytes,
                        stack.peak,
                        task_cpu_pct);
#else
                    (void)task_cpu_pct;
#endif  /* CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS */
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
                tasks_publish_close(&tasks_pub);
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

        {
            const size_t len = strnlen(g_payload_buf, HEAPWATCH_MAX_PAYLOAD_SIZE);
            if (len > 0 && len < HEAPWATCH_MAX_PAYLOAD_SIZE) {
#ifdef HEAPWATCH_USE_JSON_FORMAT
                if (g_payload_buf[len - 1] == '}') {
                    g_payload_buf[len - 1] = '\0';
                    const size_t added = heapwatch::netif_traffic::appendParameterFields(
                        g_payload_buf + len - 1, HEAPWATCH_MAX_PAYLOAD_SIZE - (len - 1), true);
                    size_t end = (len - 1) + added;
                    if (end + 1 < HEAPWATCH_MAX_PAYLOAD_SIZE) {
                        g_payload_buf[end++] = '}';
                        g_payload_buf[end] = '\0';
                    }
                }
#else
                (void)heapwatch::netif_traffic::appendParameterFields(
                    g_payload_buf + len, HEAPWATCH_MAX_PAYLOAD_SIZE - len, false);
#endif
            }
        }

        enqueue_mqtt_message(MQTT_MESSAGE_TYPE_PARAMETER, g_payload_buf);
        enqueue_dirty_task_stack_maxes(tsms, utms);

        // PARAM1 (Phase 5c): flush the generic slug->value parameter map (POW1
        // power API, CELL1 cellular signal, connector gauge limits, any other
        // updateParameter*() / setParameter*Max call site) as its own batched
        // publish. No-op if nothing is due this round.
        HeapwatchConnector::getInstance().flushParameters();

#if CONFIG_HEAPWATCH_ENABLE_HEAP_INTEGRITY_CHECK
        // HEAP1: default-off proactive corruption check — heap_caps_check_integrity() walks
        // the full heap, so this has a real CPU cost and stays opt-in.
        if (!heap_caps_check_integrity(MALLOC_CAP_DEFAULT, true)) {
            ESP_LOGE(kTag, "Heap corruption detected by heap_caps_check_integrity()");
            HeapwatchConnector::getInstance().queueEvent(HEAPWATCH_EVENT_HEAP_CORRUPTION_DETECTED);
        }
#endif  /* CONFIG_HEAPWATCH_ENABLE_HEAP_INTEGRITY_CHECK */
    }
    heapwatch_mark_collector_task_exited();
    vTaskDelete(nullptr);
}

}  // namespace heapwatch::metrics_collector
