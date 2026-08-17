#include "heapwatch_connector/ConnectorLifecycle.h"

#include "heapwatch_build_metadata.h"
#include "heapwatch.h"
#include "heapwatch_connector/ConnectorBootstrap.h"
#include "heapwatch_connector/HeapwatchConnector.h"
#include "heapwatch_endpoints.h"
#include "../stream_ingress/StreamIngressClient.h"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace heapwatch::connector_lifecycle {

namespace {

constexpr char kTag[] = "heapwatch_lifecycle";

volatile heapwatch_phase_t g_phase = HEAPWATCH_PHASE_STARTING;
volatile bool g_initialize_called = false;
volatile bool g_misconfigured = false;
volatile bool g_bootstrap_started = false;
volatile bool g_time_sync_attempted = false;
volatile bool g_enrollment_cycle_active = false;

bool is_placeholder_build_identifier(const char* value) {
    if (value == nullptr || std::strncmp(value, "sha256:", 7) != 0) {
        return true;
    }
    for (const char* p = value + 7; *p != '\0'; ++p) {
        if (*p != '0') {
            return false;
        }
    }
    return true;
}

esp_err_t initNvsFlash() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    return err;
}

bool hasProvisionedCredentialsInNvs() {
    connector_bootstrap::BootCredentials creds = {};
    return connector_bootstrap::loadBootCredentials(&creds);
}

void applyBuildMetadataToConnector() {
    const char* build_id = heapwatch_get_build_identifier();
    if (is_placeholder_build_identifier(build_id)) {
        return;
    }
    HeapwatchConnector::getInstance().setCoreDumpBuildIdentifier(build_id);
}

void onNetIpEvent(void*, esp_event_base_t event_base, int32_t event_id, void*) {
    if (event_base != IP_EVENT) {
        return;
    }

    auto& connector = HeapwatchConnector::getInstance();
    if (event_id == IP_EVENT_STA_GOT_IP || event_id == IP_EVENT_ETH_GOT_IP) {
        connector.setNetworkReady(true);
        connector_bootstrap::clearWifiPermanentlyFailed();
        if (connector.getStatus().initialized) {
            connector.queueEvent(HEAPWATCH_EVENT_INTERNET_CONNECTED);
        }
        ESP_LOGI(kTag, "Network IP acquired (event %ld)", static_cast<long>(event_id));
    } else if (event_id == IP_EVENT_STA_LOST_IP || event_id == IP_EVENT_ETH_LOST_IP) {
        connector.setNetworkReady(false);
        if (connector.getStatus().initialized) {
            connector.queueEvent(HEAPWATCH_EVENT_INTERNET_DISCONNECTED);
        }
        ESP_LOGI(kTag, "Network IP lost (event %ld)", static_cast<long>(event_id));
    }
}

void registerNetIpEventHandlers() {
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        onNetIpEvent, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                                        onNetIpEvent, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                                        onNetIpEvent, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_LOST_IP,
                                                        onNetIpEvent, nullptr, nullptr));
}

void updateMqttPhaseFromStatus() {
    const HeapwatchConnectorStatus status = HeapwatchConnector::getInstance().getStatus();
    if (!status.initialized) {
        return;
    }
    if (status.mqtt_connected) {
        setPhase(HEAPWATCH_PHASE_MQTT_CONNECTED);
    } else if (status.network_ready) {
        setPhase(HEAPWATCH_PHASE_MQTT_CONNECTING);
    } else if (getPhase() != HEAPWATCH_PHASE_NETWORK_FAILED &&
               getPhase() != HEAPWATCH_PHASE_PROVISIONING &&
               getPhase() != HEAPWATCH_PHASE_WAITING_ACCEPT) {
        setPhase(HEAPWATCH_PHASE_READY);
    }
}

void bootstrapTask(void*) {
    setPhase(HEAPWATCH_PHASE_STARTING);
    connector_bootstrap::startWifiIfManaged();
    connector_bootstrap::startNetworkReporters();

    while (true) {
        updateMqttPhaseFromStatus();

        if (!heapwatch_network_ready()) {
            if (connector_bootstrap::isWifiPermanentlyFailed()) {
                setPhase(HEAPWATCH_PHASE_NETWORK_FAILED);
            } else {
                const heapwatch_phase_t phase = getPhase();
                if (phase != HEAPWATCH_PHASE_PROVISIONING &&
                    phase != HEAPWATCH_PHASE_WAITING_ACCEPT) {
                    setPhase(HEAPWATCH_PHASE_AWAITING_NETWORK);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!g_time_sync_attempted) {
            g_time_sync_attempted = true;
            connector_bootstrap::syncTimeIfManaged();
        }

        if (HeapwatchConnector::getInstance().getStatus().initialized) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (g_enrollment_cycle_active) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        connector_bootstrap::BootCredentials creds = {};
        if (connector_bootstrap::loadBootCredentials(&creds)) {
            if (activateConnectorFromCredentials(creds)) {
                setPhase(HEAPWATCH_PHASE_READY);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        g_enrollment_cycle_active = true;
        setPhase(HEAPWATCH_PHASE_PROVISIONING);
        const bool enrolled = connector_bootstrap::runFactoryEnrollment(&creds);
        g_enrollment_cycle_active = false;

        if (enrolled && activateConnectorFromCredentials(creds)) {
            setPhase(HEAPWATCH_PHASE_READY);
        } else {
            const uint32_t poll_interval_ms = CONFIG_HEAPWATCH_ENROLLMENT_POLL_INTERVAL_MS > 0
                                                  ? static_cast<uint32_t>(
                                                        CONFIG_HEAPWATCH_ENROLLMENT_POLL_INTERVAL_MS)
                                                  : 60000U;
            vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
        }
    }
}

}  // namespace

void setPhase(heapwatch_phase_t phase) { g_phase = phase; }

heapwatch_phase_t getPhase() { return g_phase; }

bool activateConnectorFromCredentials(const connector_bootstrap::BootCredentials& creds) {
    auto& connector = HeapwatchConnector::getInstance();
    if (!connector.ensureInfrastructure()) {
        return false;
    }

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
    applyBuildMetadataToConnector();

    if (!connector.activateWithBroker(HEAPWATCH_MQTT_BROKER_URI)) {
        return false;
    }

    return true;
}

esp_err_t initialize() {
    if (g_initialize_called) {
        return g_misconfigured ? ESP_ERR_HEAPWATCH_INIT_FAILED : ESP_OK;
    }
    g_initialize_called = true;

    if (is_placeholder_build_identifier(HEAPWATCH_BUILD_IDENTIFIER)) {
        ESP_LOGE(kTag,
                 "FATAL: HEAPWATCH_BUILD_IDENTIFIER is still the pre-link placeholder — run a full "
                 "idf.py build so the post-link fingerprint step can run");
        g_misconfigured = true;
        setPhase(HEAPWATCH_PHASE_MISCONFIGURED);
        return ESP_ERR_HEAPWATCH_INIT_FAILED;
    }

    const esp_err_t nvs_err = initNvsFlash();
    if (nvs_err != ESP_OK) {
        ESP_LOGE(kTag, "FATAL: nvs_flash_init failed: %s", esp_err_to_name(nvs_err));
        g_misconfigured = true;
        setPhase(HEAPWATCH_PHASE_MISCONFIGURED);
        return ESP_ERR_HEAPWATCH_INIT_FAILED;
    }

    if (hasProvisionedCredentialsInNvs()) {
        ESP_LOGI(kTag, "Provisioned credentials in NVS — empty enrollment key is OK");
    } else if (CONFIG_HEAPWATCH_ENROLLMENT_KEY[0] == '\0') {
        ESP_LOGE(kTag,
                 "FATAL: device is not provisioned and CONFIG_HEAPWATCH_ENROLLMENT_KEY is empty — "
                 "cannot enroll");
        g_misconfigured = true;
        setPhase(HEAPWATCH_PHASE_MISCONFIGURED);
        return ESP_ERR_HEAPWATCH_NOT_PROVISIONED;
    }

    connector_bootstrap::ensureDefaultEventLoop();
    registerNetIpEventHandlers();

    if (!HeapwatchConnector::getInstance().ensureInfrastructure()) {
        ESP_LOGE(kTag, "FATAL: failed to create connector message queues");
        g_misconfigured = true;
        setPhase(HEAPWATCH_PHASE_MISCONFIGURED);
        return ESP_ERR_HEAPWATCH_INIT_FAILED;
    }

    connector_bootstrap::installLogRelayHookIfEnabled();
    connector_bootstrap::startLogRelayTaskIfEnabled();

    if (!g_bootstrap_started) {
        g_bootstrap_started = true;
        constexpr uint32_t kBootstrapStackBytes = 16384;
        if (xTaskCreate(bootstrapTask, "hw_bootstrap", kBootstrapStackBytes, nullptr,
                        tskIDLE_PRIORITY + 2, nullptr) != pdPASS) {
            ESP_LOGE(kTag, "FATAL: failed to start hw_bootstrap task");
            g_misconfigured = true;
            setPhase(HEAPWATCH_PHASE_MISCONFIGURED);
            return ESP_ERR_HEAPWATCH_INIT_FAILED;
        }
    }

    setPhase(HEAPWATCH_PHASE_STARTING);
    return ESP_OK;
}

}  // namespace heapwatch::connector_lifecycle
