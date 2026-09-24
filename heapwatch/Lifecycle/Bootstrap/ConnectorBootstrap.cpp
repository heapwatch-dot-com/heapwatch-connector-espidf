// Copyright (c) 2025-2026 Heapwatch. All rights reserved.
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

#include "heapwatch/Lifecycle/Bootstrap/ConnectorBootstrap.h"
#include "heapwatch/Mqtt/MqttTelemetryClient.h"
#include "heapwatch/Network/DeviceNetworkReporter.h"
#include "heapwatch/Radio/BluetoothLe/BluetoothReporter.h"
#include "heapwatch/Helpers/IngressTimestamps.h"

#include <cstdio>
#include <cstring>
#include <cinttypes>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#include "heapwatch/Settings/ConnectorSettings.h"
#include "heapwatch/Enrollment/DeviceEnrollmentClient.h"
#include "heapwatch/Enrollment/EnrollStateLedIndicator.h"
#include "heapwatch/Enrollment/ClaimedIdentitiesBootSync.h"
#include "heapwatch/Enrollment/Credentials/DeviceCredentialsStore.h"
#include "heapwatch/Enrollment/Identity/DeviceIdentity.h"
#include "heapwatch.h"
#include "heapwatch_endpoints.h"
#include "heapwatch/Lifecycle/ConnectorLifecycle.h"
#include "heapwatch/Lifecycle/NotificationsFromSystemEventLoop.h"

namespace Heapwatch::Lifecycle::Bootstrap {

constexpr char TAG[] = "HwLife";

#if CONFIG_HEAPWATCH_MANAGE_WIFI
constexpr int32_t WIFI_CONNECT_MAX_RETRY = 10;
#endif

int32_t g_wifi_retry = 0;
uint32_t g_wifi_reconnects_total = 0;
volatile bool g_wifi_permanently_failed = false;
volatile bool g_command_wifi_reconnect_active = false;
bool g_wifi_driver_inited = false;


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
    Heapwatch::Enrollment::Credentials::Credentials stored = {};
    if (!Heapwatch::Enrollment::Credentials::load(&stored)) {
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

bool runFactoryEnrollment(BootCredentials* creds) {
    if (CONFIG_HEAPWATCH_ENROLLMENT_KEY[0] == '\0') {
        return false;
    }

    char claimed_mac[32] = {};
    char claimed_uid128[33] = {};
    Heapwatch::Enrollment::Identity::getClaimedMac(claimed_mac, sizeof(claimed_mac));
    Heapwatch::Enrollment::Identity::getClaimedUid128(claimed_uid128, sizeof(claimed_uid128));

    Heapwatch::Enrollment::Credentials::Credentials enrolled = {};
    const uint32_t poll_interval_ms = CONFIG_HEAPWATCH_ENROLLMENT_POLL_INTERVAL_MS > 0
                                          ? static_cast<uint32_t>(
                                                CONFIG_HEAPWATCH_ENROLLMENT_POLL_INTERVAL_MS)
                                          : HEAPWATCH_ENROLLMENT_POLL_INTERVAL_MS;

    if (!Heapwatch::Enrollment::runUntilProvisioned(
            HEAPWATCH_API_BASE_URL, CONFIG_HEAPWATCH_ENROLLMENT_KEY, claimed_mac, claimed_uid128,
            heapwatch_get_build_identifier(),
            CONFIG_HEAPWATCH_BUILD_ENV, poll_interval_ms, &enrolled)) {
        Heapwatch::Enrollment::setIndicatorPhase(
            Heapwatch::Enrollment::EnrollStateLedPhase::ENROLL_LED_NETWORK_ERROR);
        return false;
    }

    Heapwatch::Enrollment::setIndicatorPhase(
        Heapwatch::Enrollment::EnrollStateLedPhase::ENROLL_LED_PROVISIONED);

    std::strncpy(creds->tenant_slug, enrolled.tenant_slug, sizeof(creds->tenant_slug) - 1);
    std::strncpy(creds->device_uid, enrolled.device_uid, sizeof(creds->device_uid) - 1);
    std::strncpy(creds->mqtt_password, enrolled.mqtt_password, sizeof(creds->mqtt_password) - 1);
    std::strncpy(creds->device_api_key, enrolled.device_api_key, sizeof(creds->device_api_key) - 1);
    std::strncpy(creds->stream_api_key, enrolled.stream_api_key, sizeof(creds->stream_api_key) - 1);
    setCoredumpUploadUrlFromApiBase(creds);
    setCoredumpAuthHeader(creds);
    return true;
}

bool isWifiPermanentlyFailed() { return g_wifi_permanently_failed; }

void clearWifiPermanentlyFailed() {
    g_wifi_permanently_failed = false;
    g_wifi_retry = 0;
}

void connectManagedWifi() {
#if CONFIG_HEAPWATCH_MANAGE_WIFI
    esp_wifi_connect();
#endif
}

void handleManagedWifiDisconnected() {
#if CONFIG_HEAPWATCH_MANAGE_WIFI
    g_wifi_reconnects_total++;
    if (g_command_wifi_reconnect_active) {
        ESP_LOGI(TAG, "Wi-Fi disconnected (commanded reconnect — deferring auto-reconnect)");
        return;
    }
    if (g_wifi_retry < WIFI_CONNECT_MAX_RETRY) {
        g_wifi_retry++;
        esp_wifi_connect();
        ESP_LOGW(TAG, "Wi-Fi reconnect attempt %" PRId32 "/%" PRId32, g_wifi_retry,
                 WIFI_CONNECT_MAX_RETRY);
    } else {
        g_wifi_permanently_failed = true;
        Heapwatch::Lifecycle::setPhase(HEAPWATCH_PHASE_NETWORK_FAILED);
        Heapwatch::Enrollment::setIndicatorPhase(
            Heapwatch::Enrollment::EnrollStateLedPhase::ENROLL_LED_WIFI_FAILED);
        ESP_LOGE(TAG, "Wi-Fi connect failed after %" PRId32 " retries", WIFI_CONNECT_MAX_RETRY);
    }
#endif
}

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
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Wi-Fi reconnect requested (duration_s=%" PRIu32 ")", static_cast<uint32_t>(duration_s));
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

void startNetworkReporters() {
    Heapwatch::Network::start();
    Heapwatch::Radio::BluetoothLe::start();
}

void startWifiIfManaged() {
#if CONFIG_HEAPWATCH_MANAGE_WIFI
#if CONFIG_HEAPWATCH_ENROLL_STATE_LED_INDICATOR
    Heapwatch::Enrollment::startIndicator();
    Heapwatch::Enrollment::setIndicatorPhase(
        Heapwatch::Enrollment::EnrollStateLedPhase::ENROLL_LED_WIFI_CONNECTING);
#endif
    g_wifi_permanently_failed = false;
    g_wifi_retry = 0;
    ESP_ERROR_CHECK(esp_netif_init());
    if (!g_wifi_driver_inited) {
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        g_wifi_driver_inited = true;
    }

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
    ESP_LOGI(TAG, "Wi-Fi STA started (non-blocking)");
#else
    ESP_LOGI(TAG, "HEAPWATCH_MANAGE_WIFI disabled; host app owns networking");
#endif /* CONFIG_HEAPWATCH_MANAGE_WIFI */
}

void syncTimeIfManaged() {
#if !CONFIG_HEAPWATCH_MANAGE_TIME_SYNC
    ESP_LOGI(TAG, "HEAPWATCH_MANAGE_TIME_SYNC disabled; client is responsible for system time");
#else
    esp_sntp_stop();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    constexpr int32_t MAX_CHECKS = 100;
    constexpr time_t MIN_VALID_UNIX = 1700000000;
    int32_t checks = 0;

    while (checks < MAX_CHECKS) {
        const time_t now = time(nullptr);
        if (now >= MIN_VALID_UNIX) {
            ESP_LOGI(TAG, "System time synced: %" PRId64, static_cast<int64_t>(now));
            return;
        }
        checks++;
        ESP_LOGI(TAG, "Waiting for SNTP sync... (%" PRId32 "/%" PRId32 ")", checks, MAX_CHECKS);
        Heapwatch::Lifecycle::waitWhileHandlingNotificationsFromSystemEventLoop(1000);
    }

    ESP_LOGW(TAG, "SNTP sync timeout, continuing with current system time");
#endif /* CONFIG_HEAPWATCH_MANAGE_TIME_SYNC */
}

void stopManagedWifi() {
#if CONFIG_HEAPWATCH_MANAGE_WIFI
    if (g_wifi_driver_inited) {
        (void)esp_wifi_stop();
    }
    g_wifi_retry = 0;
    g_wifi_permanently_failed = false;
#endif
}

void stopTimeSyncIfManaged() {
#if CONFIG_HEAPWATCH_MANAGE_TIME_SYNC
    esp_sntp_stop();
#endif
}

uint32_t getWifiReconnectsTotal() { return g_wifi_reconnects_total; }
void syncClaimedIdentitiesIfProvisioned() {
    Heapwatch::Enrollment::requestClaimedIdentitiesBootPostNow();
}

}  // namespace Heapwatch::Lifecycle::Bootstrap
