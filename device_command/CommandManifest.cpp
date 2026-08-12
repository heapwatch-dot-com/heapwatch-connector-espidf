#include "CommandManifest.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "../connector_settings/ConnectorSettings.h"
#include "../device_credentials/DeviceCredentialsStore.h"
#include "../heapwatch_connector/ConnectorBootstrap.h"
#include "../heapwatch_endpoints.h"
#include "heapwatch.h"

namespace heapwatch::device_command {

namespace {

constexpr char TAG[] = "CmdManifest";
constexpr size_t kHttpResponseMax = 2048;
constexpr size_t kHttpUrlMax = 512;
constexpr size_t kManifestBodyMax = 8192;

struct HttpResponseBuffer {
    char data[kHttpResponseMax];
    size_t len = 0;
};

bool g_uploaded_this_boot = false;

esp_err_t httpEventHandler(esp_http_client_event_t* evt) {
    if (evt == nullptr || evt->user_data == nullptr) {
        return ESP_OK;
    }

    auto* buffer = static_cast<HttpResponseBuffer*>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        const size_t remaining = kHttpResponseMax - 1 - buffer->len;
        const size_t copy_len =
            evt->data_len < remaining ? static_cast<size_t>(evt->data_len) : remaining;
        if (copy_len > 0) {
            std::memcpy(buffer->data + buffer->len, evt->data, copy_len);
            buffer->len += copy_len;
            buffer->data[buffer->len] = '\0';
        }
    }

    return ESP_OK;
}

bool httpPostJson(const char* url, const char* body, const char* api_key,
                  HttpResponseBuffer* response) {
    if (url == nullptr || body == nullptr || api_key == nullptr || response == nullptr) {
        return false;
    }

    response->len = 0;
    response->data[0] = '\0';

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 20000;
    config.event_handler = httpEventHandler;
    config.user_data = response;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }

    char auth_header[HEAPWATCH_MAX_DEVICE_API_KEY_LEN + 16] = {};
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);

    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_post_field(client, body, std::strlen(body));

    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
        return false;
    }

    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTP status %d body=%s", status, response->data);
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
    cJSON* scan_values = cJSON_CreateArray();
    cJSON_AddItemToArray(scan_values, cJSON_CreateString("active"));
    cJSON_AddItemToArray(scan_values, cJSON_CreateString("passive"));
    cJSON_AddItemToObject(scan_type, "values", scan_values);
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

void uploadManifestIfNeeded() {
    if (g_uploaded_this_boot) {
        return;
    }

    heapwatch::connector_bootstrap::BootCredentials creds = {};
    if (!heapwatch::connector_bootstrap::resolveBootCredentials(&creds) ||
        creds.device_api_key[0] == '\0') {
        ESP_LOGW(TAG, "Skipping manifest upload — no device API key");
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

    if (std::strlen(body) >= kManifestBodyMax) {
        ESP_LOGE(TAG, "Manifest JSON too large (%u bytes)", static_cast<unsigned>(std::strlen(body)));
        free(body);
        return;
    }

    char url[kHttpUrlMax] = {};
    snprintf(url, sizeof(url), "%s/ingress/command-manifest", HEAPWATCH_API_BASE_URL);

    HttpResponseBuffer response = {};
    const bool ok = httpPostJson(url, body, creds.device_api_key, &response);
    free(body);

    if (ok) {
        g_uploaded_this_boot = true;
        ESP_LOGI(TAG, "Command manifest uploaded");
    } else {
        ESP_LOGW(TAG, "Command manifest upload failed");
    }
}

}  // namespace heapwatch::device_command
