// Copyright (c) 2026 Heapwatch. All rights reserved.

#include "heapwatch/Radio/BluetoothLe/BluetoothReporter.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cinttypes>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "heapwatch/Radio/BleDelegate.h"
#include "heapwatch_config.h"
#include "heapwatch/Mqtt/MqttTelemetryClient.h"
#include "heapwatch/Helpers/FreeRtosAlloc.h"
#include "heapwatch/Helpers/IngressTimestamps.h"
#include "heapwatch/Radio/BluetoothLe/IRadioBleProvider.h"
#include "sdkconfig.h"
#include "heapwatch/StreamIngress/StreamIngressClient.h"

namespace Heapwatch::Radio::BluetoothLe {

void emitBleEvent(int32_t event_id, const char* detail, const char* peer_addr);

namespace {

constexpr const char* TAG = "HwBle";
constexpr size_t JSON_LINE_MAX = 512;
constexpr size_t COMMAND_ID_MAX = 64;

bool g_started = false;
volatile bool g_stop_requested = false;
volatile bool g_task_exited = true;
bool g_prev_stack_ready = false;
bool g_prev_advertising = false;
bool g_prev_scanning = false;
RadioBleSnapshot g_prev_snapshot = {};
char g_scan_command_id[COMMAND_ID_MAX] = {};

bool append(char* out, size_t out_len, size_t* offset, const char* fmt, ...) {
    if (*offset >= out_len) {
        return false;
    }
    va_list args;
    va_start(args, fmt);
    const int32_t written = vsnprintf(out + *offset, out_len - *offset, fmt, args);
    va_end(args);
    if (written < 0 || static_cast<size_t>(written) >= out_len - *offset) {
        return false;
    }
    *offset += static_cast<size_t>(written);
    return true;
}

bool append_json_string(char* out, size_t out_len, size_t* offset, const char* value) {
    if (value == nullptr) {
        value = "";
    }
    if (!append(out, out_len, offset, "\"")) {
        return false;
    }
    for (const uint8_t* p = reinterpret_cast<const uint8_t*>(value); *p != '\0'; ++p) {
        const uint8_t c = *p;
        if (c == '"' || c == '\\') {
            if (!append(out, out_len, offset, "\\%c", c)) {
                return false;
            }
        } else if (c < 0x20) {
            if (!append(out, out_len, offset, "\\u%04x", c)) {
                return false;
            }
        } else if (!append(out, out_len, offset, "%c", c)) {
            return false;
        }
    }
    return append(out, out_len, offset, "\"");
}

bool snapshotsEqual(const RadioBleSnapshot& a, const RadioBleSnapshot& b) {
    return a.stack_ready == b.stack_ready && a.advertising == b.advertising &&
           a.scanning == b.scanning && a.connection_count == b.connection_count &&
           a.bond_count == b.bond_count && strncmp(a.mac, b.mac, sizeof(a.mac)) == 0;
}

void emitStateTransitions(const RadioBleSnapshot& snap) {
#if CONFIG_HEAPWATCH_ENABLE_STANDARD_EVENTS
    if (snap.stack_ready && !g_prev_stack_ready) {
        emitBleEvent(HEAPWATCH_EVENT_BLE_STACK_READY, "stack_ready", nullptr);
    }
    if (snap.advertising && !g_prev_advertising) {
        emitBleEvent(HEAPWATCH_EVENT_BLE_ADV_STARTED, "adv_started", nullptr);
    }
    if (!snap.advertising && g_prev_advertising) {
        emitBleEvent(HEAPWATCH_EVENT_BLE_ADV_STOPPED, "adv_stopped", nullptr);
    }
    if (snap.scanning && !g_prev_scanning) {
        emitBleEvent(HEAPWATCH_EVENT_BLE_SCAN_STARTED, "scan_started", nullptr);
    }
    if (!snap.scanning && g_prev_scanning) {
        emitBleEvent(HEAPWATCH_EVENT_BLE_SCAN_STOPPED, "scan_stopped", nullptr);
    }
#endif
    g_prev_stack_ready = snap.stack_ready;
    g_prev_advertising = snap.advertising;
    g_prev_scanning = snap.scanning;
}

void onScannedBleDevice(const ScannedBleDevice& peer, void* /*ctx*/) {
    uint64_t tsms = 0;
    uint64_t utms = 0;
    heapwatch_get_wall_clock_and_uptime_ms(&tsms, &utms);

    char wall_clock[HEAPWATCH_WALL_CLOCK_VALUE_BUFFER_SIZE] = {};
    heapwatch_format_wall_clock_or_null(tsms, wall_clock, sizeof(wall_clock));

    char line[JSON_LINE_MAX] = {};
    if (g_scan_command_id[0] != '\0') {
        snprintf(line, sizeof(line),
                 "{\"command_id\":\"%s\",\"addr\":\"%s\",\"rssi\":%" PRId32 ",\"name\":\"%s\","
                 "\"tsms\":%s,\"utms\":%" PRIu64 "}",
                 g_scan_command_id, peer.addr, static_cast<int32_t>(peer.rssi), peer.name,
                 wall_clock, static_cast<uint64_t>(utms));
    } else {
        snprintf(line, sizeof(line),
                 "{\"command_id\":null,\"addr\":\"%s\",\"rssi\":%" PRId32 ",\"name\":\"%s\",\"tsms\":%s,"
                 "\"utms\":%" PRIu64 "}",
                 peer.addr, static_cast<int32_t>(peer.rssi), peer.name,
                 wall_clock, static_cast<uint64_t>(utms));
    }

    Heapwatch::StreamIngress::sendBleScanJson(line);
}

void onScanComplete(bool success, const char* message, void* /*ctx*/) {
    uint64_t tsms = 0;
    uint64_t utms = 0;
    heapwatch_get_wall_clock_and_uptime_ms(&tsms, &utms);

    char wall_clock[HEAPWATCH_WALL_CLOCK_VALUE_BUFFER_SIZE] = {};
    heapwatch_format_wall_clock_or_null(tsms, wall_clock, sizeof(wall_clock));

    char line[JSON_LINE_MAX] = {};
    if (g_scan_command_id[0] != '\0') {
        snprintf(line, sizeof(line),
                 "{\"command_id\":\"%s\",\"done\":true,\"ok\":%s,\"message\":\"%s\","
                 "\"tsms\":%s,\"utms\":%" PRIu64 "}",
                 g_scan_command_id, success ? "true" : "false", message != nullptr ? message : "",
                 wall_clock, static_cast<uint64_t>(utms));
    } else {
        snprintf(line, sizeof(line),
                 "{\"command_id\":null,\"done\":true,\"ok\":%s,\"message\":\"%s\",\"tsms\":%s,"
                 "\"utms\":%" PRIu64 "}",
                 success ? "true" : "false", message != nullptr ? message : "",
                 wall_clock, static_cast<uint64_t>(utms));
    }

    Heapwatch::StreamIngress::sendBleScanJson(line);
    g_scan_command_id[0] = '\0';
    publishFullSync();
}

void taskReportBluetooth(void* /*arg*/) {
#if defined(CONFIG_HEAPWATCH_BLE_TELEMETRY_ENABLED) && CONFIG_HEAPWATCH_BLE_TELEMETRY_ENABLED
    const TickType_t interval =
        pdMS_TO_TICKS(CONFIG_HEAPWATCH_BLE_TELEMETRY_INTERVAL_SEC * 1000);

    while (!g_stop_requested) {
        RadioBleSnapshot snap = {};
        getBleProvider().fillSnapshot(snap);

        bool should_publish = true;
#if defined(CONFIG_HEAPWATCH_BLE_TELEMETRY_ON_CHANGE) && CONFIG_HEAPWATCH_BLE_TELEMETRY_ON_CHANGE
        should_publish = !snapshotsEqual(snap, g_prev_snapshot);
#endif
        if (should_publish) {
            publishFullSync();
        }
        vTaskDelay(interval);
    }
#else
    while (!g_stop_requested) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
#endif
    g_task_exited = true;
    heapwatch_task_delete(nullptr);
}

}  // namespace

bool isBleReportingEnabled() {
#if CONFIG_HEAPWATCH_ENABLE_BLE_REPORTING
    return true;
#else
    return heapwatch_ble_has_delegate();
#endif
}

bool buildPayload(char* out, size_t out_len) {
    if (out == nullptr || out_len < 32) {
        return false;
    }

    RadioBleSnapshot snap = {};
    getBleProvider().fillSnapshot(snap);

    size_t offset = 0;
    if (!append(out, out_len, &offset, "{\"ble\":{")) {
        return false;
    }
    if (!append(out, out_len, &offset, "\"bt_enabled\":%s,\"stack_ready\":%s,\"advertising\":%s,"
                                   "\"scanning\":%s,\"mac\":",
                snap.bt_enabled ? "true" : "false", snap.stack_ready ? "true" : "false",
                snap.advertising ? "true" : "false", snap.scanning ? "true" : "false")) {
        return false;
    }
    if (!append_json_string(out, out_len, &offset, snap.mac)) {
        return false;
    }

    if (!append(out, out_len, &offset, ",\"connections\":[")) {
        return false;
    }
    for (size_t i = 0; i < snap.connection_count; ++i) {
        const RadioBleConnection& conn = snap.connections[i];
        if (!append(out, out_len, &offset, "%s{\"peer_addr\":", i == 0 ? "" : ",")) {
            return false;
        }
        if (!append_json_string(out, out_len, &offset, conn.peer_addr)) {
            return false;
        }
        if (!append(out, out_len, &offset, ",\"conn_handle\":%" PRIu32 ",\"role\":\"%s\"}",
                    static_cast<uint32_t>(conn.conn_handle),
                    conn.is_central ? "central" : "peripheral")) {
            return false;
        }
    }

    if (!append(out, out_len, &offset, "],\"bonds\":[")) {
        return false;
    }
    for (size_t i = 0; i < snap.bond_count; ++i) {
        if (!append(out, out_len, &offset, "%s{\"peer_addr\":", i == 0 ? "" : ",")) {
            return false;
        }
        if (!append_json_string(out, out_len, &offset, snap.bonds[i].peer_addr)) {
            return false;
        }
        if (!append(out, out_len, &offset, "}")) {
            return false;
        }
    }

    if (!append(out, out_len, &offset, "]}}")) {
        return false;
    }

    return true;
}

void emitBleEvent(int32_t event_id, const char* detail, const char* peer_addr) {
#if CONFIG_HEAPWATCH_ENABLE_STANDARD_EVENTS
    MqttTelemetryClient::getInstance().queueEvent(event_id);
#endif

    uint64_t tsms = 0;
    uint64_t utms = 0;
    heapwatch_get_wall_clock_and_uptime_ms(&tsms, &utms);

    char wall_clock[HEAPWATCH_WALL_CLOCK_VALUE_BUFFER_SIZE] = {};
    heapwatch_format_wall_clock_or_null(tsms, wall_clock, sizeof(wall_clock));

    char line[JSON_LINE_MAX] = {};
    if (peer_addr != nullptr && peer_addr[0] != '\0') {
        snprintf(line, sizeof(line),
                 "{\"event_id\":%" PRId32 ",\"detail\":\"%s\",\"peer_addr\":\"%s\",\"tsms\":%s,"
                 "\"utms\":%" PRIu64 "}",
                 event_id, detail != nullptr ? detail : "", peer_addr,
                 wall_clock, static_cast<uint64_t>(utms));
    } else {
        snprintf(line, sizeof(line),
                 "{\"event_id\":%" PRId32 ",\"detail\":\"%s\",\"peer_addr\":null,\"tsms\":%s,"
                 "\"utms\":%" PRIu64 "}",
                 event_id, detail != nullptr ? detail : "",
                 wall_clock, static_cast<uint64_t>(utms));
    }

    Heapwatch::StreamIngress::sendBleEventJson(line);
}

void publishFullSync() {
    RadioBleSnapshot snap = {};
    getBleProvider().fillSnapshot(snap);
    emitStateTransitions(snap);
    g_prev_snapshot = snap;
    MqttTelemetryClient::getInstance().queueBluetoothSnapshot();
    ESP_LOGI(TAG, "Queued full /bluetooth sync");
}

void handleGetCommand() { publishFullSync(); }

bool startCommandScan(const char* command_id, bool active, uint32_t duration_seconds) {
    IRadioBleProvider& provider = getBleProvider();
    if (!provider.isAvailable()) {
        return false;
    }

    g_scan_command_id[0] = '\0';
    if (command_id != nullptr && command_id[0] != '\0') {
        snprintf(g_scan_command_id, sizeof(g_scan_command_id), "%s", command_id);
    }

    if (!Heapwatch::StreamIngress::ensureTcpSession()) {
        ESP_LOGW(TAG, "Stream ingress unavailable for BLE scan");
    }

    return provider.startScan(active, duration_seconds, onScannedBleDevice, onScanComplete, nullptr);
}

void start() {
    if (g_started) {
        return;
    }
    g_started = true;

    if (!isBleReportingEnabled() && !heapwatch_ble_has_delegate()) {
        ESP_LOGI(TAG, "BLE reporting disabled");
        return;
    }

    if (!getBleProvider().initialize()) {
        ESP_LOGW(TAG, "BLE provider init failed");
    }

    g_stop_requested = false;
#if defined(CONFIG_HEAPWATCH_BLE_TELEMETRY_ENABLED) && CONFIG_HEAPWATCH_BLE_TELEMETRY_ENABLED
    g_task_exited = false;
    heapwatch_task_create(taskReportBluetooth, "hw_ble_report", 4096, nullptr, tskIDLE_PRIORITY + 1, nullptr);
#else
    g_task_exited = true;
#endif

    ESP_LOGI(TAG, "Bluetooth reporter started");
}

void stop() {
    if (!g_started) {
        return;
    }
    g_stop_requested = true;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
    while (!g_task_exited && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    g_started = false;
}

}  // namespace Heapwatch::Radio::BluetoothLe
