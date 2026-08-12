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

/**
 * Phase 5b: pure C ABI bodies. Each function forwards into the internal C++
 * implementation — not a second copy of the logic (S2/S3).
 */

#include "heapwatch.h"

#include <cstdio>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "heapwatch_connector/ConnectorBootstrap.h"
#include "heapwatch_connector/HeapwatchConnector.h"
#include "heapwatch_connector/ParameterAPI.h"
#include "heapwatch_endpoints.h"
#include "device_command/DeviceCommandHandler.h"
#include "device_command/SleepHooks.h"
#include "connector_settings/ConnectorSettings.h"
#include "device_identity/DeviceIdentity.h"
#include "device_ota/OtaService.h"
#include "../stream_ingress/StreamIngressClient.h"

namespace {

constexpr char kTag[] = "heapwatch_c_api";

/** Host sticky flag — ORed with OTA-in-progress in heapwatch_keep_running(). */
bool g_host_keep_running = false;

const char* g_build_identifier_ptr = "";
const char* g_firmware_version_ptr = "";

void store_app_metadata(const heapwatch_config_t* config) {
    g_build_identifier_ptr = "";
    g_firmware_version_ptr = "";
    if (config == nullptr) {
        return;
    }
    if (config->build_identifier != nullptr && config->build_identifier[0] != '\0') {
        g_build_identifier_ptr = config->build_identifier;
    }
    if (config->firmware_version != nullptr && config->firmware_version[0] != '\0') {
        g_firmware_version_ptr = config->firmware_version;
    }
}

bool has_explicit_credentials(const heapwatch_config_t* config) {
    return config != nullptr && config->tenant_slug != nullptr && config->tenant_slug[0] != '\0' &&
           config->mqtt_password != nullptr && config->mqtt_password[0] != '\0';
}

bool apply_boot_credentials(const heapwatch::connector_bootstrap::BootCredentials& creds,
                            const char* build_identifier) {
    auto& connector = HeapwatchConnector::getInstance();
    connector.setTenantSlug(creds.tenant_slug);
    connector.setMqttDeviceUid(creds.device_uid);
    connector.setMqttPassword(creds.mqtt_password);
    if (creds.coredump_upload_url[0] != '\0') {
        connector.setCoreDumpUploadUrl(creds.coredump_upload_url);
    }
    if (creds.coredump_auth_header[0] != '\0') {
        connector.setRestApiAuthToken(creds.coredump_auth_header);
    }
    if (creds.stream_api_key[0] != '\0') {
        heapwatch::stream_ingress::setStreamApiKey(creds.stream_api_key);
    }
    if (build_identifier != nullptr && build_identifier[0] != '\0') {
        connector.setCoreDumpBuildIdentifier(build_identifier);
    }
    return connector.initialize(nullptr, HEAPWATCH_MQTT_BROKER_URI);
}

bool apply_explicit_credentials(const heapwatch_config_t* config) {
    auto& connector = HeapwatchConnector::getInstance();
    connector.setTenantSlug(config->tenant_slug);
    connector.setMqttPassword(config->mqtt_password);

    char device_uid[HEAPWATCH_MAX_DEVICE_UID_LEN] = {};
    if (config->device_uid != nullptr && config->device_uid[0] != '\0') {
        std::snprintf(device_uid, sizeof(device_uid), "%s", config->device_uid);
    } else {
        heapwatch::device_identity::getMacUid(device_uid, sizeof(device_uid));
    }
    if (device_uid[0] == '\0') {
        ESP_LOGE(kTag, "device_uid empty and MAC fallback failed");
        return false;
    }
    connector.setMqttDeviceUid(device_uid);

    if (config->device_api_key != nullptr && config->device_api_key[0] != '\0') {
        char auth_header[HEAPWATCH_MAX_DEVICE_API_KEY_LEN + 16] = {};
        std::snprintf(auth_header, sizeof(auth_header), "Bearer %s", config->device_api_key);
        connector.setRestApiAuthToken(auth_header);

        char upload_url[256] = {};
        std::snprintf(upload_url, sizeof(upload_url), "%s/ingress/core-dump", HEAPWATCH_API_BASE_URL);
        connector.setCoreDumpUploadUrl(upload_url);
    }

    if (config->build_identifier != nullptr && config->build_identifier[0] != '\0') {
        connector.setCoreDumpBuildIdentifier(config->build_identifier);
    }

    return connector.initialize(nullptr, HEAPWATCH_MQTT_BROKER_URI);
}

esp_err_t require_initialized() {
    const HeapwatchConnectorStatus status = HeapwatchConnector::getInstance().getStatus();
    if (!status.initialized) {
        return ESP_ERR_HEAPWATCH_NOT_INITIALIZED;
    }
    return ESP_OK;
}

}  // namespace

extern "C" {

void heapwatch_ensure_event_loop(void) {
    heapwatch::connector_bootstrap::ensureDefaultEventLoop();
}

void heapwatch_init_wifi_if_managed(void) {
    heapwatch::connector_bootstrap::initWifiIfManaged();
}

void heapwatch_sync_time_if_managed(void) {
    heapwatch::connector_bootstrap::syncTimeIfManaged();
}

esp_err_t heapwatch_init(const heapwatch_config_t* config) {
    const HeapwatchConnectorStatus prior = HeapwatchConnector::getInstance().getStatus();
    if (prior.initialized) {
        return ESP_ERR_HEAPWATCH_ALREADY_INITIALIZED;
    }

    store_app_metadata(config);
    heapwatch_ensure_event_loop();

    bool ok = false;
    if (has_explicit_credentials(config)) {
        ok = apply_explicit_credentials(config);
    } else {
        // Mixed NULL/non-NULL credential fields are invalid — either all via
        // bootstrap, or explicit tenant+password at minimum.
        if (config != nullptr &&
            ((config->tenant_slug != nullptr && config->tenant_slug[0] != '\0') ||
             (config->mqtt_password != nullptr && config->mqtt_password[0] != '\0') ||
             (config->device_api_key != nullptr && config->device_api_key[0] != '\0'))) {
            ESP_LOGE(kTag, "Partial credentials: supply tenant_slug + mqtt_password, or leave all NULL");
            return ESP_ERR_HEAPWATCH_INVALID_ARG;
        }

        heapwatch::connector_bootstrap::BootCredentials creds = {};
        if (!heapwatch::connector_bootstrap::resolveBootCredentials(&creds)) {
            return ESP_ERR_HEAPWATCH_NOT_PROVISIONED;
        }
        const char* build_id = (config != nullptr) ? config->build_identifier : nullptr;
        ok = apply_boot_credentials(creds, build_id);
    }

    if (!ok) {
        return ESP_ERR_HEAPWATCH_INIT_FAILED;
    }

    HeapwatchConnector::getInstance().setNetworkReady(false);
    return ESP_OK;
}

void heapwatch_deinit(void) {
    HeapwatchConnector::getInstance().deinitialize();
}

void heapwatch_set_network_ready(bool ready) {
    HeapwatchConnector::getInstance().setNetworkReady(ready);
}

void heapwatch_set_boot_cycle_context(uint32_t boot_cycle, uint32_t crash_boot_cycle) {
    HeapwatchConnector::getInstance().setBootCycleContext(boot_cycle, crash_boot_cycle);
}

esp_err_t heapwatch_get_status(heapwatch_status_t* out) {
    if (out == nullptr) {
        return ESP_ERR_HEAPWATCH_INVALID_ARG;
    }
    const HeapwatchConnectorStatus status = HeapwatchConnector::getInstance().getStatus();
    out->initialized = status.initialized;
    out->network_ready = status.network_ready;
    out->mqtt_connected = status.mqtt_connected;
    out->dispatcher_running = status.dispatcher_running;
    out->collector_running = status.collector_running;
    out->keep_running = heapwatch_keep_running();
    out->last_successful_publish_unix_ms = status.last_successful_publish_unix_ms;
    out->tiny_queue_depth = status.tiny_queue_depth;
    out->small_queue_depth = status.small_queue_depth;
    out->large_queue_depth = status.large_queue_depth;
    out->coredump_queue_depth = status.coredump_queue_depth;
    return ESP_OK;
}

void heapwatch_set_keep_running(bool keep_running) {
    g_host_keep_running = keep_running;
    ESP_LOGI(kTag, "keep_running host flag=%s (effective=%s)", keep_running ? "true" : "false",
             heapwatch_keep_running() ? "true" : "false");
}

bool heapwatch_keep_running(void) {
    return g_host_keep_running || heapwatch::device_ota::isInProgress();
}

void heapwatch_install_log_relay_if_enabled(void) {
    heapwatch::connector_bootstrap::installLogRelayHookIfEnabled();
}

void heapwatch_start_log_relay_if_enabled(void) {
    heapwatch::connector_bootstrap::startLogRelayTaskIfEnabled();
}

esp_err_t heapwatch_queue_event(int event_type_id, int32_t boot_cycle) {
    const esp_err_t ready = require_initialized();
    if (ready != ESP_OK) {
        return ready;
    }
    HeapwatchConnector::getInstance().queueEvent(event_type_id, boot_cycle);
    return ESP_OK;
}

esp_err_t heapwatch_queue_log(HeapwatchConnectorSeverity severity, const char* message,
                              const heapwatch_log_context_t* context) {
    if (message == nullptr) {
        return ESP_ERR_HEAPWATCH_INVALID_ARG;
    }
    const esp_err_t ready = require_initialized();
    if (ready != ESP_OK) {
        return ready;
    }

    HeapwatchConnectorLogContext cpp_ctx = {};
    const HeapwatchConnectorLogContext* cpp_ctx_ptr = nullptr;
    if (context != nullptr) {
        cpp_ctx.task = context->task;
        cpp_ctx.tag = context->tag;
        cpp_ctx.core = context->core;
        cpp_ctx_ptr = &cpp_ctx;
    }

    if (!HeapwatchConnector::getInstance().queueLog(severity, message, cpp_ctx_ptr)) {
        return ESP_ERR_HEAPWATCH_QUEUE_FULL;
    }
    return ESP_OK;
}

esp_err_t heapwatch_queue_parameter_int(const char* slug, int64_t value) {
    if (slug == nullptr || slug[0] == '\0') {
        return ESP_ERR_HEAPWATCH_INVALID_ARG;
    }
    const esp_err_t ready = require_initialized();
    if (ready != ESP_OK) {
        return ready;
    }
    HeapwatchConnector::getInstance().queueParameter(slug, value);
    return ESP_OK;
}

esp_err_t heapwatch_queue_parameter_double(const char* slug, double value) {
    if (slug == nullptr || slug[0] == '\0') {
        return ESP_ERR_HEAPWATCH_INVALID_ARG;
    }
    const esp_err_t ready = require_initialized();
    if (ready != ESP_OK) {
        return ready;
    }
    HeapwatchConnector::getInstance().queueParameter(slug, value);
    return ESP_OK;
}

esp_err_t heapwatch_update_parameter_int(const char* slug, int64_t value) {
    if (slug == nullptr || slug[0] == '\0') {
        return ESP_ERR_HEAPWATCH_INVALID_ARG;
    }
    const esp_err_t ready = require_initialized();
    if (ready != ESP_OK) {
        return ready;
    }
    HeapwatchConnector::getInstance().updateParameterInt(slug, value);
    return ESP_OK;
}

esp_err_t heapwatch_update_parameter_double(const char* slug, double value) {
    if (slug == nullptr || slug[0] == '\0') {
        return ESP_ERR_HEAPWATCH_INVALID_ARG;
    }
    const esp_err_t ready = require_initialized();
    if (ready != ESP_OK) {
        return ready;
    }
    HeapwatchConnector::getInstance().updateParameterDouble(slug, value);
    return ESP_OK;
}

esp_err_t heapwatch_update_parameter_int_ranged(const char* slug, int64_t value, int64_t min_value,
                                                int64_t max_value) {
    if (slug == nullptr || slug[0] == '\0') {
        return ESP_ERR_HEAPWATCH_INVALID_ARG;
    }
    const esp_err_t ready = require_initialized();
    if (ready != ESP_OK) {
        return ready;
    }
    HeapwatchConnector::getInstance().updateParameterIntRanged(slug, value, min_value, max_value);
    return ESP_OK;
}

esp_err_t heapwatch_update_parameter_double_ranged(const char* slug, double value, double min_value,
                                                   double max_value) {
    if (slug == nullptr || slug[0] == '\0') {
        return ESP_ERR_HEAPWATCH_INVALID_ARG;
    }
    const esp_err_t ready = require_initialized();
    if (ready != ESP_OK) {
        return ready;
    }
    HeapwatchConnector::getInstance().updateParameterDoubleRanged(slug, value, min_value, max_value);
    return ESP_OK;
}

esp_err_t heapwatch_update_power_mv(const char* source_name, int64_t mv) {
    if (source_name == nullptr || source_name[0] == '\0') {
        return ESP_ERR_HEAPWATCH_INVALID_ARG;
    }
    const esp_err_t ready = require_initialized();
    if (ready != ESP_OK) {
        return ready;
    }
    heapwatch::parameter_api::updatePowerMv(source_name, mv);
    return ESP_OK;
}

esp_err_t heapwatch_update_power_mv_ranged(const char* source_name, int64_t mv, int64_t min_mv,
                                           int64_t max_mv) {
    if (source_name == nullptr || source_name[0] == '\0') {
        return ESP_ERR_HEAPWATCH_INVALID_ARG;
    }
    const esp_err_t ready = require_initialized();
    if (ready != ESP_OK) {
        return ready;
    }
    heapwatch::parameter_api::updatePowerMvRanged(source_name, mv, min_mv, max_mv);
    return ESP_OK;
}

esp_err_t heapwatch_update_power_percent(const char* source_name, double percent) {
    if (source_name == nullptr || source_name[0] == '\0') {
        return ESP_ERR_HEAPWATCH_INVALID_ARG;
    }
    const esp_err_t ready = require_initialized();
    if (ready != ESP_OK) {
        return ready;
    }
    heapwatch::parameter_api::updatePowerPercent(source_name, percent);
    return ESP_OK;
}

esp_err_t heapwatch_update_power_state(const char* source_name, heapwatch_power_state_t state) {
    if (source_name == nullptr || source_name[0] == '\0') {
        return ESP_ERR_HEAPWATCH_INVALID_ARG;
    }
    const esp_err_t ready = require_initialized();
    if (ready != ESP_OK) {
        return ready;
    }
    heapwatch::parameter_api::updatePowerState(source_name, state);
    return ESP_OK;
}

esp_err_t heapwatch_update_cellular_signal(int rssi_dbm, int ber) {
    const esp_err_t ready = require_initialized();
    if (ready != ESP_OK) {
        return ready;
    }
    heapwatch::parameter_api::updateCellularSignal(rssi_dbm, ber);
    return ESP_OK;
}

esp_err_t heapwatch_queue_specs(void) {
    const esp_err_t ready = require_initialized();
    if (ready != ESP_OK) {
        return ready;
    }
    HeapwatchConnector::getInstance().queueSpecs();
    return ESP_OK;
}

esp_err_t heapwatch_register_command(const char* name, heapwatch_command_handler_t handler,
                                     void* user_ctx) {
    if (name == nullptr || name[0] == '\0' || handler == nullptr) {
        return ESP_ERR_HEAPWATCH_INVALID_ARG;
    }
    const esp_err_t err = heapwatch::device_command::registerCommand(name, handler, user_ctx);
    if (err == ESP_OK) {
        return ESP_OK;
    }
    if (err == ESP_ERR_NOT_ALLOWED) {
        return ESP_ERR_HEAPWATCH_INVALID_ARG;
    }
    if (err == ESP_ERR_NO_MEM) {
        return ESP_ERR_HEAPWATCH_NO_SPACE;
    }
    return ESP_ERR_HEAPWATCH_INVALID_ARG;
}

esp_err_t heapwatch_publish_command_response(const char* suffix, const char* command_id, bool ok,
                                             const char* code, const char* message) {
    if (suffix == nullptr || suffix[0] == '\0') {
        return ESP_ERR_HEAPWATCH_INVALID_ARG;
    }
    if (!heapwatch::device_command::publishCommandResponse(suffix, command_id, ok, code, message)) {
        return ESP_ERR_HEAPWATCH_NOT_CONNECTED;
    }
    return ESP_OK;
}

esp_err_t heapwatch_register_before_light_sleep_hook(heapwatch_before_sleep_hook_t hook,
                                                     void* user_ctx) {
    if (hook == nullptr) {
        return ESP_ERR_HEAPWATCH_INVALID_ARG;
    }
    if (!heapwatch::sleep_hooks::registerBeforeLightSleep(hook, user_ctx)) {
        return ESP_ERR_HEAPWATCH_NO_SPACE;
    }
    return ESP_OK;
}

esp_err_t heapwatch_register_before_deep_sleep_hook(heapwatch_before_sleep_hook_t hook,
                                                    void* user_ctx) {
    if (hook == nullptr) {
        return ESP_ERR_HEAPWATCH_INVALID_ARG;
    }
    if (!heapwatch::sleep_hooks::registerBeforeDeepSleep(hook, user_ctx)) {
        return ESP_ERR_HEAPWATCH_NO_SPACE;
    }
    return ESP_OK;
}

void heapwatch_must_stay_awake(void) { heapwatch::sleep_hooks::mustStayAwake(); }

esp_err_t heapwatch_register_config_catalog(const heapwatch_config_catalog_def_t* catalog) {
    if (catalog == nullptr || catalog->id == nullptr || catalog->title == nullptr ||
        catalog->nvs_namespace == nullptr || catalog->fields == nullptr || catalog->field_count == 0) {
        return ESP_ERR_HEAPWATCH_INVALID_ARG;
    }

    heapwatch::connector_settings::FieldSchema fields[32] = {};
    if (catalog->field_count > sizeof(fields) / sizeof(fields[0])) {
        return ESP_ERR_HEAPWATCH_INVALID_ARG;
    }

    for (size_t i = 0; i < catalog->field_count; ++i) {
        const heapwatch_config_field_def_t& src = catalog->fields[i];
        fields[i] = {src.nvs_key, src.name, src.description, src.type, src.secret, src.patchable,
                     src.unit, src.min_value, src.max_value};
    }

    if (!heapwatch::connector_settings::registerManagedCatalog(
            catalog->id, catalog->title, catalog->nvs_namespace, fields, catalog->field_count)) {
        return ESP_ERR_HEAPWATCH_INVALID_ARG;
    }

    return ESP_OK;
}

const char* heapwatch_get_build_identifier(void) { return g_build_identifier_ptr; }

const char* heapwatch_get_firmware_version(void) { return g_firmware_version_ptr; }

}  // extern "C"
