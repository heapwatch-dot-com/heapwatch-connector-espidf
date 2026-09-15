#pragma once

// Shared between MetricsCollector.cpp and MetricsCollector-Worker.cpp.
// Not part of the public MetricsCollector API beyond taskCollectMetrics (declared in .h).

#include <cstddef>
#include <cstdint>

#include "sdkconfig.h"

#include "heapwatch/Metrics/MetricsCollector.h"
#include "heapwatch/Mqtt/HeapwatchDispatcher.h"
#include "heapwatch/Mqtt/MqttTelemetryClient.h"

namespace Heapwatch::Metrics {

constexpr char TAG[] = "HwMetrics";
constexpr size_t MAX_CORES_SUPPORTED = 4;

extern uint64_t g_start_time_us;
extern char* g_payload_buf;

#if CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS
extern uint8_t g_dirty_task_stack_max_count;
#endif

struct FreertosMetricsScanResult {
    uint16_t ready_tasks_count = 0;
#if CONFIG_HEAPWATCH_ENABLE_CPU_SNAPSHOTS
    float cpu_usage_pct[MAX_CORES_SUPPORTED] = {};
#endif
};

void refresh_connector_gauge_limits(size_t core_count);
void enqueue_mqtt_message(MqttMessageType type, const char* payload);

bool run_freertos_metrics_scan(
    size_t core_count,
    FreertosMetricsScanResult* out,
    bool publish_tasks,
    uint64_t tsms,
    uint64_t utms);

#if CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS
void enqueue_dirty_task_stack_maxes(uint64_t tsms, uint64_t utms);
#endif

}  // namespace Heapwatch::Metrics
