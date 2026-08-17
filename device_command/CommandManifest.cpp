#include "CommandManifest.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "../connector_settings/ConnectorSettings.h"
#include "../stream_ingress/StreamIngressClient.h"
#include "../stream_ingress/StreamIngressMux.h"
#include "heapwatch.h"

namespace heapwatch::device_command {

namespace {

constexpr char TAG[] = "CmdManifest";
constexpr char kChannelCommandManifest[] = "command_manifest";
constexpr char kNvsNamespace[] = "heapwatch";
constexpr char kNvsKeyManifestHash[] = "man_hash";
constexpr char kNvsKeyManifestBuild[] = "man_build";
constexpr size_t kManifestHashHexLen = 64;
constexpr size_t kManifestBuildMax = 256;
constexpr uint32_t kStreamConnectTimeoutMs = 30000;
constexpr uint32_t kMuxSendRetryMs = 30000;

bool g_uploaded_this_boot = false;

bool readNvsString(const char* key, char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return false;
    }

    out[0] = '\0';
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    size_t required = out_len;
    const esp_err_t err = nvs_get_str(handle, key, out, &required);
    nvs_close(handle);

    return err == ESP_OK && out[0] != '\0';
}

bool writeNvsUploadMarker(const char* manifest_hash_hex, const char* build_identifier) {
    if (manifest_hash_hex == nullptr || build_identifier == nullptr) {
        return false;
    }

    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    bool ok = nvs_set_str(handle, kNvsKeyManifestHash, manifest_hash_hex) == ESP_OK &&
              nvs_set_str(handle, kNvsKeyManifestBuild, build_identifier) == ESP_OK &&
              nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
}

void sha256Hex(const char* body, char* out_hex, size_t out_hex_len) {
    if (body == nullptr || out_hex == nullptr || out_hex_len < kManifestHashHexLen + 1) {
        return;
    }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, reinterpret_cast<const uint8_t*>(body), strlen(body));

    uint8_t digest[32] = {};
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);

    for (size_t i = 0; i < sizeof(digest); ++i) {
        snprintf(out_hex + (i * 2), 3, "%02x", digest[i]);
    }
    out_hex[kManifestHashHexLen] = '\0';
}

bool waitForStreamTcp(uint32_t timeout_ms) {
    if (!heapwatch::stream_ingress::ensureTcpSession()) {
        return false;
    }

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        if (heapwatch::stream_ingress::isActive()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return false;
}

bool sendCtrlEnd(const char* channel_id) {
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(kMuxSendRetryMs);
    while (xTaskGetTickCount() < deadline) {
        if (heapwatch::stream_ingress::sendMuxCtrlFrame(channel_id)) {
            heapwatch::stream_ingress::flushOutbound();
            return true;
        }
        heapwatch::stream_ingress::flushOutbound();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}
bool sendDataChunk(const char* channel_id, const uint8_t* data, size_t len) {
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(kMuxSendRetryMs);
    while (xTaskGetTickCount() < deadline) {
        if (heapwatch::stream_ingress::sendMuxDataFrame(channel_id, data, len)) {
            heapwatch::stream_ingress::flushOutbound();
            return true;
        }
        heapwatch::stream_ingress::flushOutbound();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

bool streamManifestBody(const char* body) {
    if (body == nullptr) {
        return false;
    }

    const size_t total_len = strlen(body);
    size_t offset = 0;

    while (offset < total_len) {
        const size_t chunk_len =
            (total_len - offset) > stream_ingress::mux::kMaxPayloadLen
                ? stream_ingress::mux::kMaxPayloadLen
                : (total_len - offset);

        if (!sendDataChunk(kChannelCommandManifest,
                           reinterpret_cast<const uint8_t*>(body + offset), chunk_len)) {
            ESP_LOGW(TAG, "Failed to queue manifest chunk at offset %u", static_cast<unsigned>(offset));
            return false;
        }

        offset += chunk_len;
    }

    if (!sendCtrlEnd(kChannelCommandManifest)) {
        ESP_LOGW(TAG, "Failed to queue manifest end ctrl frame");
        return false;
    }

    return true;
}

void addCommand(cJSON* commands, const char* slug, const char* name, const char* description,
                const char* risk, const char* session, bool returns_payload, bool builtin,
                int response_timeout_s, bool panel_visible) {
    cJSON* cmd = cJSON_CreateObject();
    cJSON_AddStringToObject(cmd, "slug", slug);
    cJSON_AddStringToObject(cmd, "name", name);
    if (description != nullptr) {
        cJSON_AddStringToObject(cmd, "description", description);
    }
    cJSON_AddStringToObject(cmd, "risk", risk);
    cJSON_AddStringToObject(cmd, "session", session);
    cJSON_AddBoolToObject(cmd, "returns_payload", returns_payload);
    cJSON_AddBoolToObject(cmd, "builtin", builtin);
    cJSON_AddBoolToObject(cmd, "panel_visible", panel_visible);
    if (response_timeout_s > 0) {
        cJSON_AddNumberToObject(cmd, "response_timeout_s", response_timeout_s);
    }
    cJSON* args = cJSON_CreateArray();
    cJSON_AddItemToObject(cmd, "args", args);
    cJSON_AddItemToArray(commands, cmd);
}

void addSleepCommands(cJSON* commands) {
    cJSON* light = cJSON_CreateObject();
    cJSON_AddStringToObject(light, "slug", "sleep_light");
    cJSON_AddStringToObject(light, "name", "Light sleep");
    cJSON_AddStringToObject(light, "description",
                            "Enter light sleep after draining the MQTT outbox.");
    cJSON_AddStringToObject(light, "risk", "high");
    cJSON_AddStringToObject(light, "session", "non_terminal");
    cJSON_AddBoolToObject(light, "returns_payload", false);
    cJSON_AddBoolToObject(light, "builtin", true);
    cJSON_AddBoolToObject(light, "panel_visible", true);
    cJSON_AddNumberToObject(light, "response_timeout_s", 120);

    cJSON* light_args = cJSON_CreateArray();
    cJSON* light_duration = cJSON_CreateObject();
    cJSON_AddStringToObject(light_duration, "name", "duration_s");
    cJSON_AddStringToObject(light_duration, "type", "int");
    cJSON_AddStringToObject(light_duration, "label", "Timer wake");
    cJSON_AddStringToObject(light_duration, "unit", "s");
    cJSON_AddNumberToObject(light_duration, "min", 0);
    cJSON_AddNumberToObject(light_duration, "max", 86400);
    cJSON_AddBoolToObject(light_duration, "required", false);
    cJSON_AddItemToArray(light_args, light_duration);
    cJSON_AddItemToObject(light, "args", light_args);
    cJSON_AddItemToArray(commands, light);

    cJSON* deep = cJSON_CreateObject();
    cJSON_AddStringToObject(deep, "slug", "sleep_deep");
    cJSON_AddStringToObject(deep, "name", "Deep sleep");
    cJSON_AddStringToObject(deep, "description",
                            "Enter deep sleep after draining the MQTT outbox.");
    cJSON_AddStringToObject(deep, "risk", "critical");
    cJSON_AddStringToObject(deep, "session", "terminal");
    cJSON_AddBoolToObject(deep, "returns_payload", false);
    cJSON_AddBoolToObject(deep, "builtin", true);
    cJSON_AddBoolToObject(deep, "panel_visible", true);
    cJSON_AddNumberToObject(deep, "response_timeout_s", 120);

    cJSON* deep_args = cJSON_CreateArray();
    cJSON* deep_duration = cJSON_CreateObject();
    cJSON_AddStringToObject(deep_duration, "name", "duration_s");
    cJSON_AddStringToObject(deep_duration, "type", "int");
    cJSON_AddStringToObject(deep_duration, "label", "Timer wake");
    cJSON_AddStringToObject(deep_duration, "unit", "s");
    cJSON_AddNumberToObject(deep_duration, "min", 0);
    cJSON_AddNumberToObject(deep_duration, "max", 86400);
    cJSON_AddBoolToObject(deep_duration, "required", false);
    cJSON_AddItemToArray(deep_args, deep_duration);
    cJSON_AddItemToObject(deep, "args", deep_args);
    cJSON_AddItemToArray(commands, deep);
}

void addBluetoothCommands(cJSON* commands) {
    addCommand(commands, "bluetooth/get", "Bluetooth snapshot",
               "Queue a full /bluetooth MQTT snapshot.", "low", "non_terminal", false, true, 30, true);

    cJSON* scan_cmd = cJSON_CreateObject();
    cJSON_AddStringToObject(scan_cmd, "slug", "bluetooth/scan");
    cJSON_AddStringToObject(scan_cmd, "name", "BLE scan");
    cJSON_AddStringToObject(scan_cmd, "description",
                            "Run an active or passive BLE scan and stream peers.");
    cJSON_AddStringToObject(scan_cmd, "risk", "medium");
    cJSON_AddStringToObject(scan_cmd, "session", "non_terminal");
    cJSON_AddBoolToObject(scan_cmd, "returns_payload", false);
    cJSON_AddBoolToObject(scan_cmd, "builtin", true);
    cJSON_AddBoolToObject(scan_cmd, "panel_visible", true);
    cJSON_AddNumberToObject(scan_cmd, "response_timeout_s", 90);

    cJSON* scan_args = cJSON_CreateArray();
    cJSON* duration = cJSON_CreateObject();
    cJSON_AddStringToObject(duration, "name", "duration_seconds");
    cJSON_AddStringToObject(duration, "type", "int");
    cJSON_AddStringToObject(duration, "label", "Scan duration");
    cJSON_AddStringToObject(duration, "unit", "s");
    cJSON_AddNumberToObject(duration, "min", 1);
    cJSON_AddNumberToObject(duration, "max", 60);
    cJSON_AddBoolToObject(duration, "required", false);
    cJSON_AddItemToArray(scan_args, duration);

    cJSON* scan_type = cJSON_CreateObject();
    cJSON_AddStringToObject(scan_type, "name", "scan_type");
    cJSON_AddStringToObject(scan_type, "type", "enum");
    cJSON_AddStringToObject(scan_type, "label", "Scan type");
    cJSON_AddStringToObject(scan_type, "default", "active");
    cJSON* scan_options = cJSON_CreateArray();
    cJSON* scan_active = cJSON_CreateObject();
    cJSON_AddStringToObject(scan_active, "value", "active");
    cJSON_AddStringToObject(scan_active, "label", "Active");
    cJSON_AddItemToArray(scan_options, scan_active);
    cJSON* scan_passive = cJSON_CreateObject();
    cJSON_AddStringToObject(scan_passive, "value", "passive");
    cJSON_AddStringToObject(scan_passive, "label", "Passive");
    cJSON_AddItemToArray(scan_options, scan_passive);
    cJSON_AddItemToObject(scan_type, "options", scan_options);
    cJSON_AddBoolToObject(scan_type, "required", false);
    cJSON_AddItemToArray(scan_args, scan_type);

    cJSON_AddItemToObject(scan_cmd, "args", scan_args);
    cJSON_AddItemToArray(commands, scan_cmd);
}

void addWifiReconnectCommand(cJSON* commands) {
    cJSON* cmd = cJSON_CreateObject();
    cJSON_AddStringToObject(cmd, "slug", "wifi_reconnect");
    cJSON_AddStringToObject(cmd, "name", "WiFi reconnect");
    cJSON_AddStringToObject(cmd, "description",
                            "Disconnect Wi-Fi and reconnect. Optional offline duration.");
    cJSON_AddStringToObject(cmd, "risk", "medium");
    cJSON_AddStringToObject(cmd, "session", "non_terminal");
    cJSON_AddBoolToObject(cmd, "returns_payload", false);
    cJSON_AddNumberToObject(cmd, "response_timeout_s", 15);
    cJSON_AddBoolToObject(cmd, "builtin", true);
    cJSON_AddBoolToObject(cmd, "panel_visible", true);

    cJSON* args = cJSON_CreateArray();
    cJSON* duration = cJSON_CreateObject();
    cJSON_AddStringToObject(duration, "name", "duration_s");
    cJSON_AddStringToObject(duration, "type", "int");
    cJSON_AddStringToObject(duration, "label", "Offline duration");
    cJSON_AddStringToObject(duration, "unit", "s");
    cJSON_AddNumberToObject(duration, "min", 0);
    cJSON_AddNumberToObject(duration, "max", 300);
    cJSON_AddBoolToObject(duration, "required", false);
    cJSON_AddItemToArray(args, duration);
    cJSON_AddItemToObject(cmd, "args", args);

    cJSON_AddItemToArray(commands, cmd);
}

void addConfigNamespaceArg(cJSON* cmd, const char* slug) {
    cJSON* args = cJSON_GetObjectItem(cmd, "args");
    if (args == nullptr || !cJSON_IsArray(args)) {
        return;
    }
    cJSON* ns = cJSON_CreateObject();
    cJSON_AddStringToObject(ns, "name", "namespace");
    cJSON_AddStringToObject(ns, "type", "string");
    cJSON_AddStringToObject(ns, "label", "NVS namespace");
    cJSON_AddBoolToObject(ns, "required", true);
    if (strcmp(slug, "config/get") == 0) {
        cJSON_AddStringToObject(ns, "default", "*");
    }
    cJSON_AddItemToArray(args, ns);
}

void addConfigValuesArg(cJSON* cmd) {
    cJSON* args = cJSON_GetObjectItem(cmd, "args");
    if (args == nullptr || !cJSON_IsArray(args)) {
        return;
    }
    cJSON* values = cJSON_CreateObject();
    cJSON_AddStringToObject(values, "name", "values");
    cJSON_AddStringToObject(values, "type", "string");
    cJSON_AddStringToObject(values, "label", "Field values");
    cJSON_AddStringToObject(values, "description", "Object map of nvs_key to value.");
    cJSON_AddBoolToObject(values, "required", true);
    cJSON_AddItemToArray(args, values);
}

void addFsCommands(cJSON* commands) {
    static const char* kFsSlugs[] = {
        "fs/storage", "fs/list", "fs/read", "fs/download", "fs/upload", "fs/delete", "fs/rename",
        "fs/move", "fs/mkdir", "fs/create",
    };
    for (const char* slug : kFsSlugs) {
        addCommand(commands, slug, slug, "Filesystem browser command.", "medium", "non_terminal",
                   false, true, 0, false);
    }
}

void addOtaCommands(cJSON* commands) {
#if CONFIG_HEAPWATCH_ENABLE_OTA
    addCommand(commands, "ota/start", "OTA start", "Download and flash a firmware build.", "high",
               "non_terminal", false, true, 3600, false);
    addCommand(commands, "ota/abort", "OTA abort", "Abort the running OTA session.", "high",
               "non_terminal", false, true, 60, false);
    addCommand(commands, "ota/confirm", "OTA confirm", "Confirm the staged OTA image.", "high",
               "terminal", false, true, 60, false);
    addCommand(commands, "ota/rollback", "OTA rollback", "Rollback to the previous firmware.",
               "high", "terminal", false, true, 60, false);
#endif
}

cJSON* buildManifestRoot() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "manifest_version", 2);
    cJSON_AddStringToObject(root, "build_identifier", heapwatch_get_build_identifier());

    cJSON* commands = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "commands", commands);

    addCommand(commands, "ping", "Ping", "Connectivity check.", "low", "non_terminal", false, true, 15,
               true);
    addCommand(commands, "restart", "Restart", "Reboot the device.", "high", "terminal", false, true,
               60, true);
    addCommand(commands, "shutdown", "Shutdown", "Power down the device (indefinite deep sleep).",
               "critical", "terminal", false, true, 60, true);
    addCommand(commands, "specs_refresh", "Refresh specs", "Queue a device specs upload.", "low",
               "non_terminal", false, true, 30, false);
    addCommand(commands, "stream_open", "Open stream", "Open the stream ingress session.", "medium",
               "non_terminal", false, true, 30, false);
    addCommand(commands, "stream_close", "Close stream", "Close the stream ingress session.",
               "medium", "non_terminal", false, true, 30, false);

    addCommand(commands, "config/get", "Get config", "Read NVS values for a namespace.", "low",
               "non_terminal", true, true, 30, false);
    addConfigNamespaceArg(cJSON_GetArrayItem(commands, cJSON_GetArraySize(commands) - 1), "config/get");

    addCommand(commands, "config/set", "Set config", "Set in-RAM config values for a managed namespace.",
               "medium", "non_terminal", true, true, 30, false);
    {
        cJSON* set_cmd = cJSON_GetArrayItem(commands, cJSON_GetArraySize(commands) - 1);
        addConfigNamespaceArg(set_cmd, "config/set");
        addConfigValuesArg(set_cmd);
    }

    addCommand(commands, "config/persist", "Persist config",
               "Persist config values to NVS (managed dirty keys or unmanaged raw keys).", "high",
               "non_terminal", true, true, 30, false);
    {
        cJSON* persist_cmd = cJSON_GetArrayItem(commands, cJSON_GetArraySize(commands) - 1);
        addConfigNamespaceArg(persist_cmd, "config/persist");
        addConfigValuesArg(persist_cmd);
    }

    addCommand(commands, "config/revert", "Revert config",
               "Revert in-RAM config to persisted values for a managed namespace.", "medium",
               "non_terminal", true, true, 30, false);
    addConfigNamespaceArg(cJSON_GetArrayItem(commands, cJSON_GetArraySize(commands) - 1),
                          "config/revert");

    addWifiReconnectCommand(commands);
    addBluetoothCommands(commands);
    addSleepCommands(commands);
    addFsCommands(commands);
    addOtaCommands(commands);

    cJSON* catalogs = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "config_catalogs", catalogs);
    cJSON* registered_catalogs = heapwatch::connector_settings::buildAllConfigCatalogsJson();
    if (registered_catalogs != nullptr) {
        cJSON* catalog_item = nullptr;
        cJSON_ArrayForEach(catalog_item, registered_catalogs) {
            cJSON_AddItemToArray(catalogs, cJSON_Duplicate(catalog_item, 1));
        }
        cJSON_Delete(registered_catalogs);
    }

    return root;
}

}  // namespace

cJSON* buildManifestRootObject() {
    return buildManifestRoot();
}

void uploadManifestIfNeeded() {
    if (g_uploaded_this_boot) {
        return;
    }

    cJSON* root = buildManifestRoot();
    if (root == nullptr) {
        return;
    }

    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == nullptr) {
        ESP_LOGE(TAG, "Failed to serialize manifest JSON");
        return;
    }

    const char* build_identifier = heapwatch_get_build_identifier();
    char manifest_hash_hex[kManifestHashHexLen + 1] = {};
    sha256Hex(body, manifest_hash_hex, sizeof(manifest_hash_hex));

    char stored_hash[kManifestHashHexLen + 1] = {};
    char stored_build[kManifestBuildMax + 1] = {};
    const bool has_stored = readNvsString(kNvsKeyManifestHash, stored_hash, sizeof(stored_hash)) &&
                            readNvsString(kNvsKeyManifestBuild, stored_build, sizeof(stored_build));

    if (has_stored && strcmp(stored_hash, manifest_hash_hex) == 0 &&
        strcmp(stored_build, build_identifier) == 0) {
        free(body);
        g_uploaded_this_boot = true;
        ESP_LOGI(TAG, "Command manifest unchanged — skip upload");
        return;
    }

    if (!waitForStreamTcp(kStreamConnectTimeoutMs)) {
        ESP_LOGW(TAG, "Stream ingress not ready for manifest upload");
        free(body);
        return;
    }

    const size_t body_len = strlen(body);
    const bool streamed = streamManifestBody(body);
    free(body);

    if (!streamed) {
        ESP_LOGW(TAG, "Command manifest stream upload failed");
        return;
    }

    if (!writeNvsUploadMarker(manifest_hash_hex, build_identifier)) {
        ESP_LOGW(TAG, "Manifest uploaded but NVS marker write failed");
    }

    g_uploaded_this_boot = true;
    ESP_LOGI(TAG, "Command manifest streamed (%u bytes)", static_cast<unsigned>(body_len));
}

}  // namespace heapwatch::device_command
