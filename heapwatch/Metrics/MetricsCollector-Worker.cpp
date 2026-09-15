#include "heapwatch/Metrics/MetricsCollectorInternal.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "heapwatch/Fs/Volumes/FsVolumeManager.h"
#include "heapwatch/Helpers/FreeRtosAlloc.h"
#include "heapwatch/Helpers/IngressTimestamps.h"
#include "heapwatch/Lifecycle/OncePerBootTelemetry/OncePerBootTelemetry.h"
#include "heapwatch/Mqtt/HeapwatchDispatcher.h"
#include "heapwatch/Mqtt/MqttTelemetryClient.h"
#include "heapwatch/Network/DeviceNetworkReporter.h"
#include "heapwatch/Network/NetifTraffic/NetifTraffic.h"
#include "heapwatch/Settings/ConnectorSettings.h"
#include "heapwatch_config.h"

namespace Heapwatch::Metrics {

void taskCollectMetrics(void* arg) {
    (void)arg;

    // Keep refreshing TaskStats while MQTT is down so tasks that live only in this
    // window (and anything seeded at Metrics::init) stay in the table for first publish.
    while (heapwatch_collector_running() && !g_mqtt_connected) {
#if CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS
        {
            const size_t core_count =
                (portNUM_PROCESSORS <= MAX_CORES_SUPPORTED) ? portNUM_PROCESSORS
                                                           : MAX_CORES_SUPPORTED;
            FreertosMetricsScanResult unused;
            (void)run_freertos_metrics_scan(core_count, &unused, false, 0, 0);
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    bool prev_mqtt_connected = g_mqtt_connected;
    bool boot_static_enqueued = false;

    while (heapwatch_collector_running()) {
        const bool mqtt_now = g_mqtt_connected;
        if (heapwatch_should_enqueue_once_per_boot_static_telemetry(prev_mqtt_connected, mqtt_now, boot_static_enqueued)) {
            Heapwatch::Lifecycle::OncePerBootTelemetry::enqueueOncePerBootStaticTelemetry();
            boot_static_enqueued = true;
        } else if (prev_mqtt_connected && !mqtt_now) {
            MqttTelemetryClient::getInstance().queueEvent(HEAPWATCH_EVENT_MQTT_DISCONNECTED);
        } else if (Heapwatch::Network::takeAndClearNetworkTopologyChangedFlag() && mqtt_now) {
            Heapwatch::Network::publishFullSync();
        }
        prev_mqtt_connected = mqtt_now;

        // Sliced so deinit does not have to wait out a long telemetry interval before
        // the queues this task publishes into can be destroyed.
        const uint32_t interval_ms = Heapwatch::Settings::telemetryIntervalMs();
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
        heapwatch_get_wall_clock_and_uptime_ms(&tsms, &utms);
#if CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS
        g_dirty_task_stack_max_count = 0;
#endif

#if CONFIG_HEAPWATCH_ENABLE_HEAP_SNAPSHOTS
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
#endif

        int16_t wifi_rssi_dbm = 0;
        wifi_ap_record_t ap_info = {};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            wifi_rssi_dbm = ap_info.rssi;
        }

        uint16_t freertos_ready_tasks_count = 0;
        // CPU1: one entry per actual core (portNUM_PROCESSORS), not a fixed cpu0/cpu1 pair.
        const size_t core_count =
            (portNUM_PROCESSORS <= MAX_CORES_SUPPORTED) ? portNUM_PROCESSORS : MAX_CORES_SUPPORTED;
#if CONFIG_HEAPWATCH_ENABLE_CPU_SNAPSHOTS
        float cpu_usage_pct[MAX_CORES_SUPPORTED] = {};
#endif
        refresh_connector_gauge_limits(core_count);

        FreertosMetricsScanResult scan = {};
        if (run_freertos_metrics_scan(core_count, &scan, true, tsms, utms)) {
            freertos_ready_tasks_count = scan.ready_tasks_count;
#if CONFIG_HEAPWATCH_ENABLE_CPU_SNAPSHOTS
            for (size_t core = 0; core < core_count; ++core) {
                cpu_usage_pct[core] = scan.cpu_usage_pct[core];
            }
#endif
        }

        // CPU1: emit one array entry per actual core rather than fixed cpu0/cpu1 fields.
#if CONFIG_HEAPWATCH_ENABLE_CPU_SNAPSHOTS
        char cpu_usage_array[MAX_CORES_SUPPORTED * 8 + 2] = {};
        {
            size_t pos = 0;
            cpu_usage_array[pos++] = '[';
            for (size_t core = 0; core < core_count; ++core) {
                pos += snprintf(cpu_usage_array + pos, sizeof(cpu_usage_array) - pos, "%s%.2f",
                                 (core > 0) ? "," : "", static_cast<double>(cpu_usage_pct[core]));
            }
            snprintf(cpu_usage_array + pos, sizeof(cpu_usage_array) - pos, "]");
        }
#endif

#ifdef HEAPWATCH_USE_JSON_FORMAT
        snprintf(g_payload_buf, HEAPWATCH_MAX_PAYLOAD_SIZE,
                 "{\"tsms\":%" PRIu64 ",\"utms\":%" PRIu64
                 ",\"device.uptime_s\":%" PRIu32
#if CONFIG_HEAPWATCH_ENABLE_HEAP_SNAPSHOTS
                 ",\"heap.internal.used_bytes\":%" PRIu32
                 ",\"heap.psram.used_bytes\":%" PRIu32
                 ",\"heap.internal.largest_free_block_bytes\":%" PRIu32
                 ",\"heap.psram.largest_free_block_bytes\":%" PRIu32
#endif
#if CONFIG_HEAPWATCH_ENABLE_CPU_SNAPSHOTS
                 ",\"cpu_usage_pct\":%s"
#endif
                 ",\"wifi.rssi_dbm\":%" PRId32
                 ",\"freertos.ready_tasks_count\":%" PRIu16 "}",
                 tsms,
                 utms,
                 uptime_s,
#if CONFIG_HEAPWATCH_ENABLE_HEAP_SNAPSHOTS
                 internal_heap_used_bytes,
                 psram_used_bytes,
                 internal_largest_free_block_bytes,
                 psram_largest_free_block_bytes,
#endif
#if CONFIG_HEAPWATCH_ENABLE_CPU_SNAPSHOTS
                 cpu_usage_array,
#endif
                 static_cast<int32_t>(wifi_rssi_dbm),
                 freertos_ready_tasks_count);
#else
        snprintf(g_payload_buf, HEAPWATCH_MAX_PAYLOAD_SIZE,
                 "tsms=%" PRIu64 "|utms=%" PRIu64
                 "|device.uptime_s=%" PRIu32
#if CONFIG_HEAPWATCH_ENABLE_HEAP_SNAPSHOTS
                 "%sheap.internal.used_bytes=%" PRIu32
                 "%sheap.psram.used_bytes=%" PRIu32
                 "%sheap.internal.largest_free_block_bytes=%" PRIu32
                 "%sheap.psram.largest_free_block_bytes=%" PRIu32
#endif
#if CONFIG_HEAPWATCH_ENABLE_CPU_SNAPSHOTS
                 "%scpu_usage_pct=%s"
#endif
                 "%swifi.rssi_dbm=%" PRId32
                 "%sfreertos.ready_tasks_count=%" PRIu16,
                 tsms,
                 utms,
                 uptime_s,
#if CONFIG_HEAPWATCH_ENABLE_HEAP_SNAPSHOTS
                 HEAPWATCH_FLAT_DELIMITER, internal_heap_used_bytes,
                 HEAPWATCH_FLAT_DELIMITER, psram_used_bytes,
                 HEAPWATCH_FLAT_DELIMITER, internal_largest_free_block_bytes,
                 HEAPWATCH_FLAT_DELIMITER, psram_largest_free_block_bytes,
#endif
#if CONFIG_HEAPWATCH_ENABLE_CPU_SNAPSHOTS
                 HEAPWATCH_FLAT_DELIMITER, cpu_usage_array,
#endif
                 HEAPWATCH_FLAT_DELIMITER, static_cast<int32_t>(wifi_rssi_dbm),
                 HEAPWATCH_FLAT_DELIMITER, freertos_ready_tasks_count);
#endif

        {
            const size_t len = strnlen(g_payload_buf, HEAPWATCH_MAX_PAYLOAD_SIZE);
            if (len > 0 && len < HEAPWATCH_MAX_PAYLOAD_SIZE) {
#ifdef HEAPWATCH_USE_JSON_FORMAT
                if (g_payload_buf[len - 1] == '}') {
                    g_payload_buf[len - 1] = '\0';
                    size_t pos = len - 1;
                    pos += Heapwatch::Network::NetifTraffic::appendParameterFields(
                        g_payload_buf + pos, HEAPWATCH_MAX_PAYLOAD_SIZE - pos, true);
                    pos += Heapwatch::Fs::Volumes::appendParameterFields(
                        g_payload_buf + pos, HEAPWATCH_MAX_PAYLOAD_SIZE - pos, true);
                    if (pos + 1 < HEAPWATCH_MAX_PAYLOAD_SIZE) {
                        g_payload_buf[pos++] = '}';
                        g_payload_buf[pos] = '\0';
                    }
                }
#else
                {
                    size_t pos = len;
                    pos += Heapwatch::Network::NetifTraffic::appendParameterFields(
                        g_payload_buf + pos, HEAPWATCH_MAX_PAYLOAD_SIZE - pos, false);
                    (void)Heapwatch::Fs::Volumes::appendParameterFields(
                        g_payload_buf + pos, HEAPWATCH_MAX_PAYLOAD_SIZE - pos, false);
                }
#endif
            }
        }

        enqueue_mqtt_message(MQTT_MESSAGE_TYPE_PARAMETER, g_payload_buf);
#if CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS
        enqueue_dirty_task_stack_maxes(tsms, utms);
#endif

        // PARAM1 (Phase 5c): flush the generic slug->value parameter map (POW1
        // power API, CELL1 cellular signal, connector gauge limits, any other
        // updateParameter*() / setParameter*Max call site) as its own batched
        // publish. No-op if nothing is due this round.
        MqttTelemetryClient::getInstance().flushParameters();

#if CONFIG_HEAPWATCH_ENABLE_HEAP_INTEGRITY_CHECK
        // HEAP1: default-off proactive corruption check — heap_caps_check_integrity() walks
        // the full heap, so this has a real CPU cost and stays opt-in.
        if (!heap_caps_check_integrity(MALLOC_CAP_DEFAULT, true)) {
            ESP_LOGE(TAG, "Heap corruption detected by heap_caps_check_integrity()");
            MqttTelemetryClient::getInstance().queueEvent(HEAPWATCH_EVENT_HEAP_CORRUPTION_DETECTED);
        }
#endif  /* CONFIG_HEAPWATCH_ENABLE_HEAP_INTEGRITY_CHECK */
    }
    heapwatch_mark_collector_task_exited();
    heapwatch_task_delete(nullptr);
}

}  // namespace Heapwatch::Metrics
