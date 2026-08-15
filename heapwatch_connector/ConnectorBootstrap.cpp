// Copyright (c) 2025-2026 Viktor Jamrich Enterprises s.r.o. All rights reserved.
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

#include "heapwatch_connector/ConnectorBootstrap.h"
#include "heapwatch_connector/HeapwatchConnector.h"
#include "device_network/DeviceNetworkReporter.h"
#include "device_radio/BluetoothReporter.h"
#include "heapwatch_connector/heapwatch_connector_timing.h"
#include "../stream_ingress/StreamIngressClient.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#include "../connector_settings/ConnectorSettings.h"
#include "device_enrollment/DeviceEnrollmentClient.h"
#include "device_enrollment/EnrollStateLedIndicator.h"
#include "device_enrollment/ClaimedIdentitiesBootSync.h"
#include "device_credentials/DeviceCredentialsStore.h"
#include "device_identity/DeviceIdentity.h"
#include "heapwatch_endpoints.h"
#include "heapwatch.h"

namespace heapwatch::connector_bootstrap {

namespace {

constexpr char kTag[] = "connector_bootstrap";

constexpr EventBits_t kWifiConnectedBit = BIT0;
constexpr EventBits_t kWifiFailBit = BIT1;
constexpr int kWifiMaxRetry = 10;

constexpr uint32_t kLogForwardIntervalMs = 100;
constexpr uint32_t kLogAssemblyFlushTimeoutMs = 250;
constexpr size_t kLogLineMaxLen = 256;
constexpr size_t kLogTagMaxLen = 32;
constexpr size_t kLogRingCapacity = 64;

EventGroupHandle_t g_wifi_event_group = nullptr;
int g_wifi_retry = 0;
uint32_t g_wifi_reconnects_total = 0;
volatile bool g_command_wifi_reconnect_active = false;

int (*g_default_log_vprintf)(const char* fmt, va_list args) = nullptr;
uint32_t g_log_lines_dropped_total = 0;
uint32_t g_log_mqtt_dropped_total = 0;

struct RawLogLine {
    char text[kLogLineMaxLen];
    char task_name[configMAX_TASK_NAME_LEN];
    char tag[kLogTagMaxLen];
    int core;
};

RawLogLine g_log_ring[kLogRingCapacity] = {};
size_t g_log_ring_head = 0;
size_t g_log_ring_tail = 0;
size_t g_log_ring_count = 0;
portMUX_TYPE g_log_ring_lock = portMUX_INITIALIZER_UNLOCKED;

char g_log_line_assembly[kLogLineMaxLen] = {};
size_t g_log_line_assembly_len = 0;
char g_log_line_task_name[configMAX_TASK_NAME_LEN] = {};
int g_log_line_core = -1;
portMUX_TYPE g_log_assembly_lock = portMUX_INITIALIZER_UNLOCKED;
TickType_t g_log_line_assembly_last_update = 0;

bool pushLogRingLine(const RawLogLine* line) {
    if (line == nullptr) {
        return false;
    }

    bool pushed = false;
    taskENTER_CRITICAL(&g_log_ring_lock);
    if (g_log_ring_count < kLogRingCapacity) {
        g_log_ring[g_log_ring_head] = *line;
        g_log_ring_head = (g_log_ring_head + 1U) % kLogRingCapacity;
        g_log_ring_count++;
        pushed = true;
    }
    taskEXIT_CRITICAL(&g_log_ring_lock);
    return pushed;
}

bool popLogRingLine(RawLogLine* line) {
    if (line == nullptr) {
        return false;
    }

    bool popped = false;
    taskENTER_CRITICAL(&g_log_ring_lock);
    if (g_log_ring_count > 0) {
        *line = g_log_ring[g_log_ring_tail];
        g_log_ring_tail = (g_log_ring_tail + 1U) % kLogRingCapacity;
        g_log_ring_count--;
        popped = true;
    }
    taskEXIT_CRITICAL(&g_log_ring_lock);
    return popped;
}

void stripAnsiSequences(const char* input, char* output, size_t output_len) {
    if (input == nullptr || output == nullptr || output_len == 0) {
        return;
    }

    size_t out_idx = 0;
    for (size_t i = 0; input[i] != '\0' && out_idx + 1 < output_len; ++i) {
        if (input[i] == '\033' && input[i + 1] == '[') {
            i += 2;
            while (input[i] != '\0' && input[i] != 'm') {
                ++i;
            }
            continue;
        }

        output[out_idx++] = input[i];
    }
    output[out_idx] = '\0';
}

const char* skipAnsiPrefix(const char* text) {
    if (text == nullptr || text[0] != '\033') {
        return text;
    }
    const char* marker = std::strchr(text, 'm');
    if (marker == nullptr || marker[1] == '\0') {
        return text;
    }
    return marker + 1;
}

bool looksLikeEspLogPrefix(const char* text) {
    if (text == nullptr) {
        return false;
    }
    if (text[0] != 'E' && text[0] != 'W' && text[0] != 'I' && text[0] != 'D' && text[0] != 'V') {
        return false;
    }
    if (text[1] != ' ' || text[2] != '(') {
        return false;
    }
    size_t i = 3;
    bool has_digit = false;
    while (text[i] >= '0' && text[i] <= '9') {
        has_digit = true;
        ++i;
    }
    return has_digit && text[i] == ')';
}

bool parseEspLogTag(const char* text, char* tag_out, size_t tag_out_len) {
    if (text == nullptr || tag_out == nullptr || tag_out_len == 0) {
        return false;
    }

    tag_out[0] = '\0';
    const char* line = skipAnsiPrefix(text);
    if (!looksLikeEspLogPrefix(line)) {
        return false;
    }

    const char* after_paren = std::strchr(line, ')');
    if (after_paren == nullptr) {
        return false;
    }

    const char* tag_start = after_paren + 1;
    while (*tag_start == ' ') {
        ++tag_start;
    }

    const char* colon = std::strchr(tag_start, ':');
    if (colon == nullptr || colon == tag_start) {
        return false;
    }

    const size_t tag_len = static_cast<size_t>(colon - tag_start);
    if (tag_len == 0 || tag_len >= tag_out_len) {
        return false;
    }

    std::memcpy(tag_out, tag_start, tag_len);
    tag_out[tag_len] = '\0';
    return true;
}

void captureLogLineContextLocked() {
    const char* task = pcTaskGetName(nullptr);
    if (task != nullptr) {
        std::snprintf(g_log_line_task_name, sizeof(g_log_line_task_name), "%s", task);
    } else {
        g_log_line_task_name[0] = '\0';
    }
    g_log_line_core = static_cast<int>(xPortGetCoreID());
}

void flushAssembledLogLine() {
    if (g_log_line_assembly_len == 0) {
        return;
    }
    g_log_line_assembly[g_log_line_assembly_len] = '\0';

    RawLogLine line = {};
    std::snprintf(line.text, kLogLineMaxLen, "%s", g_log_line_assembly);
    std::snprintf(line.task_name, sizeof(line.task_name), "%s", g_log_line_task_name);
    line.core = g_log_line_core;
    parseEspLogTag(g_log_line_assembly, line.tag, sizeof(line.tag));

    if (!pushLogRingLine(&line)) {
        g_log_lines_dropped_total++;
    }
    g_log_line_assembly_len = 0;
    g_log_line_assembly[0] = '\0';
    g_log_line_task_name[0] = '\0';
    g_log_line_core = -1;
    g_log_line_assembly_last_update = 0;
}

HeapwatchConnectorSeverity parseEspLogSeverity(const char* text) {
    const char* line = skipAnsiPrefix(text);
    if (!looksLikeEspLogPrefix(line)) {
        return HEAPWATCH_SEVERITY_INFO;
    }

    switch (line[0]) {
        case 'E':
            return HEAPWATCH_SEVERITY_ERROR;
        case 'W':
            return HEAPWATCH_SEVERITY_WARNING;
        case 'I':
            return HEAPWATCH_SEVERITY_INFO;
        case 'D':
        case 'V':
            return HEAPWATCH_SEVERITY_DEBUG;
        default:
            return HEAPWATCH_SEVERITY_INFO;
    }
}

bool shouldRelayLogSeverity(HeapwatchConnectorSeverity severity) {
    return severity >= static_cast<HeapwatchConnectorSeverity>(
               heapwatch::connector_settings::minRelaySeverity());
}

void ingestLogTextChunk(const char* text) {
    if (text == nullptr) {
        return;
    }

    taskENTER_CRITICAL(&g_log_assembly_lock);
    if (g_log_line_assembly_len > 0 && looksLikeEspLogPrefix(skipAnsiPrefix(text))) {
        flushAssembledLogLine();
    }

    if (g_log_line_assembly_len == 0) {
        captureLogLineContextLocked();
    }

    for (size_t i = 0; text[i] != '\0'; ++i) {
        const char c = text[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            flushAssembledLogLine();
            continue;
        }

        if (g_log_line_assembly_len >= (kLogLineMaxLen - 1)) {
            flushAssembledLogLine();
        }

        g_log_line_assembly[g_log_line_assembly_len++] = c;
    }
    g_log_line_assembly_last_update = xTaskGetTickCount();
    taskEXIT_CRITICAL(&g_log_assembly_lock);
}

#if CONFIG_HEAPWATCH_ENABLE_LOG_COLLECTION
int mqttLogVprintfHook(const char* fmt, va_list args) {
    va_list copy_for_stdout;
    va_copy(copy_for_stdout, args);
    const int printed =
        (g_default_log_vprintf != nullptr) ? g_default_log_vprintf(fmt, copy_for_stdout) : 0;
    va_end(copy_for_stdout);

    if (fmt == nullptr) {
        return printed;
    }

    char buffer[kLogLineMaxLen] = {};
    va_list copy_for_parse;
    va_copy(copy_for_parse, args);
    std::vsnprintf(buffer, sizeof(buffer), fmt, copy_for_parse);
    va_end(copy_for_parse);

    if (buffer[0] == '\0') {
        return printed;
    }

    ingestLogTextChunk(buffer);
    return printed;
}

void logForwardTask(void* arg) {
    (void)arg;

    RawLogLine log_line = {};
    TickType_t last_wake = xTaskGetTickCount();
    while (true) {
        while (popLogRingLine(&log_line)) {
            char sanitized_line[kLogLineMaxLen] = {};
            stripAnsiSequences(log_line.text, sanitized_line, sizeof(sanitized_line));

            const HeapwatchConnectorSeverity severity = parseEspLogSeverity(sanitized_line);
            if (!shouldRelayLogSeverity(severity)) {
                continue;
            }

            HeapwatchConnectorLogContext context = {};
            context.task = log_line.task_name[0] != '\0' ? log_line.task_name : nullptr;
            context.tag = log_line.tag[0] != '\0' ? log_line.tag : nullptr;
            context.core = log_line.core;

            if (heapwatch::stream_ingress::isCaptureActive()) {
                char json[HEAPWATCH_SMALL_PAYLOAD_SIZE] = {};
                if (HeapwatchConnector::getInstance().formatLogJsonLine(
                        severity, sanitized_line, &context, json, sizeof(json))) {
                    heapwatch::stream_ingress::enqueueLogJson(json);
                }
            } else {
                const bool queued =
                    HeapwatchConnector::getInstance().queueLog(severity, sanitized_line, &context);
                if (!queued) {
                    g_log_mqtt_dropped_total++;
                }
            }
        }

        taskENTER_CRITICAL(&g_log_assembly_lock);
        if (g_log_line_assembly_len > 0 && g_log_line_assembly_last_update != 0 &&
            (xTaskGetTickCount() - g_log_line_assembly_last_update) >=
                pdMS_TO_TICKS(kLogAssemblyFlushTimeoutMs)) {
            flushAssembledLogLine();
        }
        taskEXIT_CRITICAL(&g_log_assembly_lock);

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kLogForwardIntervalMs));
    }
}
#endif /* CONFIG_HEAPWATCH_ENABLE_LOG_COLLECTION */

#if CONFIG_HEAPWATCH_MANAGE_WIFI
void wifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id,
                       void* event_data) {
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        HeapwatchConnector::getInstance().queueEvent(HEAPWATCH_EVENT_INTERNET_DISCONNECTED);
        g_wifi_reconnects_total++;
        if (g_command_wifi_reconnect_active) {
            ESP_LOGI(kTag, "Wi-Fi disconnected (commanded reconnect — deferring auto-reconnect)");
            return;
        }
        if (g_wifi_retry < kWifiMaxRetry) {
            g_wifi_retry++;
            esp_wifi_connect();
            ESP_LOGW(kTag, "Wi-Fi reconnect attempt %d/%d", g_wifi_retry, kWifiMaxRetry);
        } else {
            xEventGroupSetBits(g_wifi_event_group, kWifiFailBit);
            ESP_LOGE(kTag, "Wi-Fi connect failed after %d retries", kWifiMaxRetry);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        g_wifi_retry = 0;
        xEventGroupSetBits(g_wifi_event_group, kWifiConnectedBit);
        ESP_LOGI(kTag, "Wi-Fi connected and IP acquired");
        HeapwatchConnector::getInstance().queueEvent(HEAPWATCH_EVENT_INTERNET_CONNECTED);
    }
}
#endif /* CONFIG_HEAPWATCH_MANAGE_WIFI (wifiEventHandler) */

void setCoredumpAuthHeader(BootCredentials* creds) {
    if (creds == nullptr || creds->device_api_key[0] == '\0') {
        creds->coredump_auth_header[0] = '\0';
        return;
    }

    std::snprintf(creds->coredump_auth_header, sizeof(creds->coredump_auth_header), "Bearer %s",
                  creds->device_api_key);
}

void setCoredumpUploadUrlFromApiBase(BootCredentials* creds) {
    if (creds == nullptr) {
        return;
    }

    std::snprintf(creds->coredump_upload_url, sizeof(creds->coredump_upload_url),
                  "%s/ingress/core-dump", HEAPWATCH_API_BASE_URL);
}

bool loadBootCredentials(BootCredentials* creds) {
    heapwatch::device_credentials::Credentials stored = {};
    if (!heapwatch::device_credentials::load(&stored)) {
        return false;
    }

    std::strncpy(creds->tenant_slug, stored.tenant_slug, sizeof(creds->tenant_slug) - 1);
    std::strncpy(creds->device_uid, stored.device_uid, sizeof(creds->device_uid) - 1);
    std::strncpy(creds->mqtt_password, stored.mqtt_password, sizeof(creds->mqtt_password) - 1);
    std::strncpy(creds->device_api_key, stored.device_api_key, sizeof(creds->device_api_key) - 1);
    std::strncpy(creds->stream_api_key, stored.stream_api_key, sizeof(creds->stream_api_key) - 1);
    setCoredumpUploadUrlFromApiBase(creds);
    setCoredumpAuthHeader(creds);
    return true;
}

// Device enrollment: enroll → status poll → fetch → confirm.
bool runFactoryEnrollment(BootCredentials* creds) {
    if (CONFIG_HEAPWATCH_ENROLLMENT_KEY[0] == '\0') {
        return false;
    }

    char claimed_mac[32] = {};
    char claimed_uid128[33] = {};
    heapwatch::device_identity::getClaimedMac(claimed_mac, sizeof(claimed_mac));
    heapwatch::device_identity::getClaimedUid128(claimed_uid128, sizeof(claimed_uid128));

    heapwatch::device_credentials::Credentials enrolled = {};
    const uint32_t poll_interval_ms = CONFIG_HEAPWATCH_ENROLLMENT_POLL_INTERVAL_MS > 0
                                          ? static_cast<uint32_t>(
                                                CONFIG_HEAPWATCH_ENROLLMENT_POLL_INTERVAL_MS)
                                          : HEAPWATCH_ENROLLMENT_POLL_INTERVAL_MS;

    if (!heapwatch::device_enrollment::runUntilProvisioned(
            HEAPWATCH_API_BASE_URL, CONFIG_HEAPWATCH_ENROLLMENT_KEY, claimed_mac, claimed_uid128,
            heapwatch_get_build_identifier(),
            CONFIG_HEAPWATCH_BUILD_ENV, poll_interval_ms, &enrolled)) {
        heapwatch::device_enrollment::setIndicatorPhase(
            heapwatch::device_enrollment::EnrollStateLedPhase::kNetworkError);
        return false;
    }

    heapwatch::device_enrollment::setIndicatorPhase(
        heapwatch::device_enrollment::EnrollStateLedPhase::kProvisioned);

    if (!heapwatch::device_credentials::save(&enrolled)) {
        return false;
    }

    std::strncpy(creds->tenant_slug, enrolled.tenant_slug, sizeof(creds->tenant_slug) - 1);
    std::strncpy(creds->device_uid, enrolled.device_uid, sizeof(creds->device_uid) - 1);
    std::strncpy(creds->mqtt_password, enrolled.mqtt_password, sizeof(creds->mqtt_password) - 1);
    std::strncpy(creds->device_api_key, enrolled.device_api_key, sizeof(creds->device_api_key) - 1);
    std::strncpy(creds->stream_api_key, enrolled.stream_api_key, sizeof(creds->stream_api_key) - 1);
    setCoredumpUploadUrlFromApiBase(creds);
    setCoredumpAuthHeader(creds);
    return true;
}

struct EnrollmentTaskParams {
    BootCredentials* creds = nullptr;
    bool success = false;
    SemaphoreHandle_t done = nullptr;
};

void enrollmentTaskEntry(void* param) {
    auto* params = static_cast<EnrollmentTaskParams*>(param);
    params->success = runFactoryEnrollment(params->creds);
    if (params->done != nullptr) {
        xSemaphoreGive(params->done);
    }
    vTaskDelete(nullptr);
}

bool runFactoryEnrollmentOnDedicatedTask(BootCredentials* creds) {
    EnrollmentTaskParams params = {creds, false, xSemaphoreCreateBinary()};
    if (params.done == nullptr) {
        return false;
    }

    constexpr uint32_t kEnrollmentTaskStackBytes = 14336;
    if (xTaskCreate(enrollmentTaskEntry, "hw_enroll", kEnrollmentTaskStackBytes, &params,
                    tskIDLE_PRIORITY + 2, nullptr) != pdPASS) {
        vSemaphoreDelete(params.done);
        ESP_LOGE(kTag, "Failed to start enrollment task");
        return false;
    }

    xSemaphoreTake(params.done, portMAX_DELAY);
    vSemaphoreDelete(params.done);
    return params.success;
}

}  // namespace

bool requestWifiReconnect(uint32_t duration_s) {
#if CONFIG_HEAPWATCH_MANAGE_WIFI
    g_command_wifi_reconnect_active = true;
    esp_wifi_disconnect();
    if (duration_s > 0) {
        vTaskDelay(pdMS_TO_TICKS(duration_s * 1000UL));
    }
    const esp_err_t err = esp_wifi_connect();
    g_command_wifi_reconnect_active = false;
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(kTag, "Wi-Fi reconnect requested (duration_s=%u)", static_cast<unsigned>(duration_s));
    return true;
#else
    (void)duration_s;
    return false;
#endif
}

void ensureDefaultEventLoop() {
    const esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
}

bool resolveBootCredentials(BootCredentials* creds) {
    if (loadBootCredentials(creds)) {
        ESP_LOGI(kTag, "Using NVS provisioned credentials for %s", creds->device_uid);
        return true;
    }

    if (CONFIG_HEAPWATCH_ENROLLMENT_KEY[0] == '\0') {
        ESP_LOGE(kTag, "No NVS credentials and CONFIG_HEAPWATCH_ENROLLMENT_KEY is empty");
        return false;
    }

    ESP_LOGI(kTag, "Starting device enrollment");
    return runFactoryEnrollmentOnDedicatedTask(creds);
}

void initWifiIfManaged() {
#if CONFIG_HEAPWATCH_MANAGE_WIFI
#if CONFIG_HEAPWATCH_ENROLL_STATE_LED_INDICATOR
    heapwatch::device_enrollment::startIndicator();
    heapwatch::device_enrollment::setIndicatorPhase(
        heapwatch::device_enrollment::EnrollStateLedPhase::kWifiConnecting);
#endif
    g_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, nullptr));
    ESP_ERROR_CHECK(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler, nullptr));

    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), CONFIG_HEAPWATCH_WIFI_SSID,
                 sizeof(wifi_config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password), CONFIG_HEAPWATCH_WIFI_PASSWORD,
                 sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(g_wifi_event_group, kWifiConnectedBit | kWifiFailBit,
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    if ((bits & kWifiConnectedBit) == 0) {
#if CONFIG_HEAPWATCH_ENROLL_STATE_LED_INDICATOR
        heapwatch::device_enrollment::setIndicatorPhase(
            heapwatch::device_enrollment::EnrollStateLedPhase::kWifiFailed);
#endif
        ESP_LOGE(kTag, "Unable to connect to Wi-Fi SSID '%s'", CONFIG_HEAPWATCH_WIFI_SSID);
        abort();
    }
#if CONFIG_HEAPWATCH_ENROLL_STATE_LED_INDICATOR
    heapwatch::device_enrollment::setIndicatorPhase(
        heapwatch::device_enrollment::EnrollStateLedPhase::kEnrolling);
#endif
#else
    ESP_LOGW(kTag, "HEAPWATCH_MANAGE_WIFI disabled; waiting for host app to bring up its own "
                   "netif and call HeapwatchConnector::setNetworkReady(true)");
#endif /* CONFIG_HEAPWATCH_MANAGE_WIFI */
    heapwatch::device_network::start();
    heapwatch::device_radio::start();
}

void syncTimeIfManaged() {
#if !CONFIG_HEAPWATCH_MANAGE_TIME_SYNC
    ESP_LOGI(kTag, "HEAPWATCH_MANAGE_TIME_SYNC disabled; client is responsible for system time");
#else
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    constexpr int max_checks = 20;
    constexpr time_t min_valid_unix = 1700000000;
    int checks = 0;

    while (checks < max_checks) {
        const time_t now = time(nullptr);
        if (now >= min_valid_unix) {
            ESP_LOGI(kTag, "System time synced: %lld", static_cast<long long>(now));
            return;
        }
        checks++;
        ESP_LOGI(kTag, "Waiting for SNTP sync... (%d/%d)", checks, max_checks);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGW(kTag, "SNTP sync timeout, continuing with current system time");
#endif /* CONFIG_HEAPWATCH_MANAGE_TIME_SYNC */
}

void installLogRelayHookIfEnabled() {
#if CONFIG_HEAPWATCH_ENABLE_LOG_COLLECTION
    g_default_log_vprintf = esp_log_set_vprintf(mqttLogVprintfHook);
#endif
}

void startLogRelayTaskIfEnabled() {
#if CONFIG_HEAPWATCH_ENABLE_LOG_COLLECTION
    xTaskCreate(logForwardTask, "hw_log_forward", 6144, nullptr, tskIDLE_PRIORITY + 1, nullptr);
#endif
}

uint32_t getWifiReconnectsTotal() { return g_wifi_reconnects_total; }
uint32_t getLogLinesDroppedTotal() { return g_log_lines_dropped_total; }
uint32_t getLogMqttDroppedTotal() { return g_log_mqtt_dropped_total; }

void syncClaimedIdentitiesIfProvisioned() {
    heapwatch::device_enrollment::requestClaimedIdentitiesBootPostNow();
}

}  // namespace heapwatch::connector_bootstrap
