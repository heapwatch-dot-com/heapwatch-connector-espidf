#include "DeviceCommandHandler.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"

#include "../heapwatch_dispatcher/HeapwatchDispatcher.h"
#include "../heapwatch_connector/HeapwatchConnector.h"
#include "../heapwatch_connector/heapwatch_connector_timing.h"
#include "../device_fs/DeviceFsCommandHandler.h"
#include "../device_ota/OtaService.h"
#include "../device_radio/BluetoothReporter.h"
#include "../stream_ingress/StreamIngressClient.h"
#include "heapwatch/radio/BleDelegate.h"
#include "ConfigCommands.h"
#include "GoingDarkPipeline.h"
#include "SleepCommands.h"

namespace heapwatch::device_command {

namespace {

const char* TAG = "DeviceCommand";

constexpr size_t kSeenCommandIdsCapacity = 16;
char g_seen_command_ids[kSeenCommandIdsCapacity][27];
size_t g_seen_write_index = 0;

bool is_duplicate_command_id(const char* command_id) {
    if (command_id == nullptr || command_id[0] == '\0') {
        return false;
    }

    for (size_t i = 0; i < kSeenCommandIdsCapacity; i++) {
        if (g_seen_command_ids[i][0] != '\0' && strcmp(g_seen_command_ids[i], command_id) == 0) {
            return true;
        }
    }

    return false;
}

void remember_command_id(const char* command_id) {
    if (command_id == nullptr || command_id[0] == '\0') {
        return;
    }

    snprintf(g_seen_command_ids[g_seen_write_index], sizeof(g_seen_command_ids[0]), "%s", command_id);
    g_seen_write_index = (g_seen_write_index + 1) % kSeenCommandIdsCapacity;
}

struct CommandRegistration {
    char name[32];
    CommandHandler handler;
    void* user_ctx;
};

CommandRegistration g_registered_commands[kMaxRegisteredCommands] = {};

bool is_builtin_name(const char* name) {
    return strcmp(name, "ping") == 0 || strcmp(name, "specs_refresh") == 0 ||
           strcmp(name, "restart") == 0 || strcmp(name, "stream_open") == 0 ||
           strcmp(name, "stream_close") == 0 || strcmp(name, "config/get") == 0 ||
           strcmp(name, "config/set") == 0 || strcmp(name, "config/persist") == 0 ||
           strcmp(name, "config/revert") == 0 || strcmp(name, "wifi_reconnect") == 0 ||
           strcmp(name, "bluetooth/get") == 0 || strcmp(name, "bluetooth/scan") == 0 ||
           strcmp(name, "sleep_light") == 0 || strcmp(name, "sleep_deep") == 0;
}

const CommandRegistration* find_registered_command(const char* suffix) {
    for (const auto& registration : g_registered_commands) {
        if (registration.handler != nullptr && strcmp(registration.name, suffix) == 0) {
            return &registration;
        }
    }
    return nullptr;
}

bool publish_phased_message(const char* suffix, const char* command_id, const char* phase, bool ok,
                            const char* code, const char* message, cJSON* result) {
    uint64_t tsms = 0;
    uint64_t utms = 0;
    heapwatch_get_ingress_timing_ms(&tsms, &utms);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "command_id", command_id != nullptr ? command_id : "");
    cJSON_AddStringToObject(root, "phase", phase);
    cJSON_AddNumberToObject(root, "tsms", static_cast<double>(tsms));
    cJSON_AddNumberToObject(root, "utms", static_cast<double>(utms));
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "code", code != nullptr ? code : "");
    cJSON_AddStringToObject(root, "message", message != nullptr ? message : "");
    if (result != nullptr) {
        cJSON_AddItemToObject(root, "result", result);
    } else if (strcmp(phase, "response") == 0) {
        cJSON_AddNullToObject(root, "result");
    }

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == nullptr) {
        ESP_LOGW(TAG, "Failed to serialize command %s for suffix %s", phase, suffix);
        return false;
    }

    const bool published = heapwatch_dispatcher_publish_command_response(suffix, json);
    free(json);
    if (!published) {
        ESP_LOGW(TAG, "Failed to enqueue command %s for suffix %s", phase, suffix);
    }

    return published;
}

bool publish_response(const char* suffix, const char* command_id, bool ok, const char* code,
                      const char* message) {
    return publish_phased_message(suffix, command_id, "response", ok, code, message, nullptr);
}

bool publish_response_with_result(const char* suffix, const char* command_id, bool ok,
                                  const char* code, const char* message, cJSON* result) {
    return publish_phased_message(suffix, command_id, "response", ok, code, message, result);
}

bool publish_ack(const char* suffix, const char* command_id, const char* message) {
    return publish_phased_message(suffix, command_id, "ack", true, "", message, nullptr);
}

bool publish_ack_then_response(const char* suffix, const char* command_id, bool ok, const char* code,
                               const char* message) {
    if (!publish_ack(suffix, command_id, message)) {
        return false;
    }

    return publish_response(suffix, command_id, ok, code, message);
}

bool publish_ack_then_response_with_result(const char* suffix, const char* command_id, bool ok,
                                           const char* code, const char* message, cJSON* result) {
    if (!publish_ack(suffix, command_id, message)) {
        if (result != nullptr) {
            cJSON_Delete(result);
        }
        return false;
    }

    return publish_response_with_result(suffix, command_id, ok, code, message, result);
}

bool extract_command_suffix(const char* topic, size_t topic_len, char* suffix_out,
                             size_t suffix_out_len) {
    const char* uid = heapwatch_get_device_uid();
    const char* tenant = heapwatch_get_tenant_slug();
    char prefix[128];
    const int prefix_len = snprintf(prefix, sizeof(prefix), "%s/command/device/%s/", tenant, uid);
    if (prefix_len <= 0 || topic_len < static_cast<size_t>(prefix_len)) {
        return false;
    }

    if (strncmp(topic, prefix, static_cast<size_t>(prefix_len)) != 0) {
        return false;
    }

    const char* suffix = topic + prefix_len;
    const size_t suffix_len = topic_len - static_cast<size_t>(prefix_len);
    if (suffix_len == 0 || suffix_len >= suffix_out_len) {
        return false;
    }

    memcpy(suffix_out, suffix, suffix_len);
    suffix_out[suffix_len] = '\0';
    return true;
}

char* extract_command_id(const char* payload, size_t payload_len) {
    cJSON* root = cJSON_ParseWithLength(payload, static_cast<int>(payload_len));
    if (root == nullptr) {
        return nullptr;
    }

    const cJSON* command_id = cJSON_GetObjectItem(root, "command_id");
    char* copied = nullptr;
    if (cJSON_IsString(command_id) && command_id->valuestring != nullptr) {
        copied = strdup(command_id->valuestring);
    }

    cJSON_Delete(root);
    return copied;
}

void handle_ping(const char* suffix, char* command_id) {
    publish_ack_then_response(suffix, command_id, true, "", "pong");
    free(command_id);
}

void handle_specs_refresh(const char* suffix, char* command_id) {
    HeapwatchConnector::getInstance().queueSpecs();
    publish_ack(suffix, command_id, "specs queued");
    free(command_id);
}

void handle_restart(const char* suffix, char* command_id) {
    if (!heapwatch::going_dark::scheduleRestart(suffix, command_id)) {
        publish_response(suffix, command_id, false, "busy", "Going-dark pipeline busy");
        free(command_id);
        return;
    }

    free(command_id);
}

void handle_stream_open(const char* suffix, char* command_id) {
    heapwatch::stream_ingress::requestOpen();
    publish_ack(suffix, command_id, "stream open accepted");
    free(command_id);
}

void handle_stream_close(const char* suffix, char* command_id) {
    heapwatch::stream_ingress::requestClose();
    publish_ack(suffix, command_id, "stream close accepted");
    free(command_id);
}

void handle_bluetooth_get(const char* suffix, char* command_id) {
    if (!heapwatch::device_radio::isBleReportingEnabled() && !heapwatch_ble_has_delegate()) {
        publish_response(suffix, command_id, false, "feature_disabled",
                          "Bluetooth reporting is disabled on this device");
        free(command_id);
        return;
    }

    heapwatch::device_radio::handleGetCommand();
    publish_ack(suffix, command_id, "bluetooth snapshot queued");
    free(command_id);
}

void handle_bluetooth_scan(const char* suffix, char* command_id, const char* payload,
                           size_t payload_len) {
    if (!heapwatch::device_radio::isBleReportingEnabled() && !heapwatch_ble_has_delegate()) {
        publish_response(suffix, command_id, false, "feature_disabled",
                          "Bluetooth reporting is disabled on this device");
        free(command_id);
        return;
    }

    cJSON* root = cJSON_ParseWithLength(payload, static_cast<int>(payload_len));
    uint32_t duration_seconds = 10;
    bool active = true;

    if (root != nullptr) {
        const cJSON* duration = cJSON_GetObjectItem(root, "duration_seconds");
        if (cJSON_IsNumber(duration)) {
            duration_seconds = static_cast<uint32_t>(duration->valueint);
        }
        const cJSON* scan_type = cJSON_GetObjectItem(root, "scan_type");
        if (cJSON_IsString(scan_type) && scan_type->valuestring != nullptr) {
            active = strcmp(scan_type->valuestring, "passive") != 0;
        }
        cJSON_Delete(root);
    }

    if (duration_seconds == 0) {
        duration_seconds = 10;
    }
    if (duration_seconds > 60) {
        duration_seconds = 60;
    }

    if (!heapwatch::device_radio::startCommandScan(command_id, active, duration_seconds)) {
        publish_response(suffix, command_id, false, "busy",
                          "BLE scan unavailable or already running");
        free(command_id);
        return;
    }

    publish_ack(suffix, command_id, "bluetooth scan started");
    free(command_id);
}

void handle_unknown(const char* suffix, char* command_id) {
    publish_response(suffix, command_id, false, "unsupported", "Command not supported on device");
    free(command_id);
}

}  // namespace

esp_err_t registerCommand(const char* name, CommandHandler handler, void* user_ctx) {
    if (name == nullptr || name[0] == '\0' || handler == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (is_builtin_name(name)) {
        ESP_LOGE(TAG, "Cannot register command '%s': collides with a built-in command", name);
        return ESP_ERR_NOT_ALLOWED;
    }

    for (auto& registration : g_registered_commands) {
        if (registration.handler != nullptr && strcmp(registration.name, name) == 0) {
            ESP_LOGW(TAG, "Command '%s' already registered; overwriting handler", name);
            registration.handler = handler;
            registration.user_ctx = user_ctx;
            return ESP_OK;
        }
    }

    for (auto& registration : g_registered_commands) {
        if (registration.handler == nullptr) {
            snprintf(registration.name, sizeof(registration.name), "%s", name);
            registration.handler = handler;
            registration.user_ctx = user_ctx;
            return ESP_OK;
        }
    }

    ESP_LOGE(TAG, "Cannot register command '%s': registration table full (max %u)", name,
             static_cast<unsigned>(kMaxRegisteredCommands));
    return ESP_ERR_NO_MEM;
}

bool publishCommandAck(const char* suffix, const char* command_id, const char* message) {
    return publish_ack(suffix, command_id, message);
}

bool publishCommandResponse(const char* suffix, const char* command_id, bool ok, const char* code,
                             const char* message) {
    return publish_response(suffix, command_id, ok, code, message);
}

bool publishCommandResponseWithResult(const char* suffix, const char* command_id, bool ok,
                                      const char* code, const char* message, cJSON* result) {
    return publish_response_with_result(suffix, command_id, ok, code, message, result);
}

void handleMqttMessage(const char* topic, size_t topic_len, const char* payload,
                        size_t payload_len, int mqtt_retain) {
    if (mqtt_retain != 0) {
        ESP_LOGW(TAG, "Ignoring retained command MQTT message");
        return;
    }

    char suffix[64];
    if (!extract_command_suffix(topic, topic_len, suffix, sizeof(suffix))) {
        return;
    }

    char* command_id = extract_command_id(payload, payload_len);
    if (command_id == nullptr) {
        command_id = strdup("");
    }

    if (is_duplicate_command_id(command_id)) {
        ESP_LOGW(TAG, "Duplicate command_id %s — skipping execution", command_id);
        publish_response(suffix, command_id, true, "", "duplicate");
        free(command_id);
        return;
    }

    remember_command_id(command_id);

    ESP_LOGI(TAG, "Command received: %s", suffix);

    if (heapwatch::going_dark::isActive()) {
        ESP_LOGW(TAG, "Rejecting command '%s' — going-dark pipeline active", suffix);
        publish_response(suffix, command_id, false, "busy", "Rejected: going-dark pipeline active");
        free(command_id);
        return;
    }

    if (heapwatch_dispatcher_is_shutting_down()) {
        ESP_LOGW(TAG, "Rejecting command '%s' — MQTT shutdown in progress", suffix);
        publish_response(suffix, command_id, false, "busy", "Rejected: device shutting down");
        free(command_id);
        return;
    }

    // While HTTPS OTA is running, only ota/start + ota/abort may proceed.
    // ota/confirm and ota/rollback are rejected inside OtaService with busy.
    // Non-ota commands must not interrupt the flash pipeline.
    if (heapwatch::device_ota::isInProgress() && strncmp(suffix, "ota/", 4) != 0) {
        ESP_LOGW(TAG, "Rejecting command '%s' — OTA in progress", suffix);
        publish_response(suffix, command_id, false, "busy",
                         "Rejected: firmware OTA is in progress");
        free(command_id);
        return;
    }

    if (strcmp(suffix, "ping") == 0) {
        handle_ping(suffix, command_id);
        return;
    }

    if (strcmp(suffix, "specs_refresh") == 0) {
        handle_specs_refresh(suffix, command_id);
        return;
    }

    if (strcmp(suffix, "restart") == 0) {
        handle_restart(suffix, command_id);
        return;
    }

    if (strcmp(suffix, "stream_open") == 0) {
        handle_stream_open(suffix, command_id);
        return;
    }

    if (strcmp(suffix, "stream_close") == 0) {
        handle_stream_close(suffix, command_id);
        return;
    }

    if (strcmp(suffix, "bluetooth/get") == 0) {
        handle_bluetooth_get(suffix, command_id);
        return;
    }

    if (strcmp(suffix, "bluetooth/scan") == 0) {
        handle_bluetooth_scan(suffix, command_id, payload, payload_len);
        return;
    }

    if (strcmp(suffix, "config/get") == 0) {
        handleConfigGet(suffix, command_id, payload, payload_len);
        return;
    }

    if (strcmp(suffix, "config/set") == 0) {
        handleConfigSet(suffix, command_id, payload, payload_len);
        return;
    }

    if (strcmp(suffix, "config/persist") == 0) {
        handleConfigPersist(suffix, command_id, payload, payload_len);
        return;
    }

    if (strcmp(suffix, "config/revert") == 0) {
        handleConfigRevert(suffix, command_id, payload, payload_len);
        return;
    }

    if (strcmp(suffix, "wifi_reconnect") == 0) {
        handleWifiReconnect(suffix, command_id, payload, payload_len);
        return;
    }

    if (strcmp(suffix, "sleep_light") == 0) {
        handleSleepLight(suffix, command_id, payload, payload_len);
        return;
    }

    if (strcmp(suffix, "sleep_deep") == 0) {
        handleSleepDeep(suffix, command_id, payload, payload_len);
        return;
    }

    if (heapwatch::device_fs::handleMqttCommand(suffix, command_id, payload, payload_len)) {
        free(command_id);
        return;
    }

    if (heapwatch::device_ota::handleMqttCommand(suffix, command_id, payload, payload_len)) {
        free(command_id);
        return;
    }

    if (const CommandRegistration* registration = find_registered_command(suffix)) {
        registration->handler(suffix, command_id, registration->user_ctx);
        free(command_id);
        return;
    }

    handle_unknown(suffix, command_id);
}

}  // namespace heapwatch::device_command
