#include "heapwatch/Lifecycle/ConnectorLifecycle.h"

#include "heapwatch_build_metadata.h"
#include "heapwatch/Helpers/heapwatch_freertos_alloc.h"
#include "heapwatch.h"
#include "heapwatch/Lifecycle/Bootstrap/ConnectorBootstrap.h"
#include "heapwatch/Mqtt/HeapwatchConnector.h"
#include "heapwatch_endpoints.h"
#include "heapwatch/StreamIngress/StreamIngressClient.h"
#include "heapwatch/Lifecycle/BootCycle/BootCycleStore.h"
#include "heapwatch/Enrollment/Credentials/DeviceCredentialsStore.h"
#include "heapwatch/Enrollment/DeviceSelfIntelClient.h"
#include "heapwatch/Enrollment/EnrollStateLedIndicator.h"
#include "heapwatch/Network/DeviceNetworkReporter.h"
#include "heapwatch/Fs/Volumes/FsVolumeManager.h"
#include "heapwatch/DeviceCommand/SleepTracking.h"

#include <cstring>
#include <cinttypes>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace Heapwatch::Lifecycle {

void refreshHandoffLed();

namespace {

constexpr char TAG[] = "HwLife";

volatile heapwatch_phase_t g_phase = HEAPWATCH_PHASE_STARTING;
volatile bool g_initialize_called = false;
volatile bool g_misconfigured = false;
volatile bool g_bootstrap_started = false;
volatile bool g_time_sync_attempted = false;
volatile bool g_enrollment_cycle_active = false;
volatile bool g_bootstrap_stop = false;
volatile bool g_bootstrap_exited = true;
volatile bool g_boot_cycle_resumed = false;
/** Per-boot, not per-init: deliberately NOT cleared by deinitialize(). */
volatile bool g_wake_telemetry_queued = false;
Heapwatch::Lifecycle::BootCycle::State g_boot_state = {};
TaskHandle_t g_bootstrap_task = nullptr;
bool g_ip_handlers_registered = false;
esp_event_handler_instance_t g_ip_sta_got = nullptr;
esp_event_handler_instance_t g_ip_eth_got = nullptr;
esp_event_handler_instance_t g_ip_sta_lost = nullptr;
esp_event_handler_instance_t g_ip_eth_lost = nullptr;

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
    Heapwatch::Lifecycle::Bootstrap::BootCredentials creds = {};
    return Heapwatch::Lifecycle::Bootstrap::loadBootCredentials(&creds);
}

/**
 * Advance the NVS boot counter and apply PANIC attribution. Not fatal for the host app: a
 * failing store degrades telemetry to boot_cycle 0 rather than blocking the client's boot.
 */
void initBootIdentity() {
    g_boot_state = {};
    if (!Heapwatch::Lifecycle::BootCycle::onBoot(&g_boot_state)) {
        ESP_LOGE(TAG, "Boot cycle store unavailable — telemetry will report boot_cycle 0");
        return;
    }
    HeapwatchConnector::getInstance().setBootCycleContext(g_boot_state.boot_cycle,
                                                          g_boot_state.last_crash_boot_cycle);
}

/**
 * NVS boot counter was reset while the device still holds fleet credentials: realign it with
 * the server's last known boot_cycle. Runs on hw_bootstrap after MQTT activation so it never
 * delays the client's boot or the first publish.
 */
void resumeBootCycleFromServer() {
    Heapwatch::Enrollment::Credentials::Credentials creds = {};
    if (!Heapwatch::Enrollment::Credentials::load(&creds) || creds.device_api_key[0] == '\0') {
        return;
    }

    uint32_t server_last_boot_cycle = UINT32_MAX;
    if (!Heapwatch::Enrollment::fetchLastBootCycle(
            HEAPWATCH_API_BASE_URL, creds.device_api_key, &server_last_boot_cycle)) {
        return;
    }
    if (!Heapwatch::Lifecycle::BootCycle::resumeFromServerLast(server_last_boot_cycle, &g_boot_state)) {
        return;
    }

    HeapwatchConnector::getInstance().setBootCycleContext(g_boot_state.boot_cycle,
                                                          g_boot_state.last_crash_boot_cycle);
}

/**
 * Deep-sleep wake attribution plus STARTING_UP. Requires the connector queues; the payloads
 * sit in them until MQTT is up. STARTING_UP re-fires on a re-initialize() after deinit(); the
 * wake attribution does not, because it describes the boot rather than the connector start.
 */
void queueBootLifecycleTelemetry() {
    auto& connector = HeapwatchConnector::getInstance();

    if (!g_wake_telemetry_queued && esp_reset_reason() == ESP_RST_DEEPSLEEP) {
        g_wake_telemetry_queued = true;
        connector.queueEvent(HEAPWATCH_EVENT_WOKE_UP_FROM_DEEP_SLEEP);
        connector.queueParameter("wake_cause_code",
                                 static_cast<int64_t>(heapwatch_translate_current_wake_cause()));
        const int32_t slept_ms = heapwatch_sleep_tracking_consume_deep_sleep_duration_ms();
        if (slept_ms >= 0) {
            connector.queueParameter("sleep_duration_ms", static_cast<int64_t>(slept_ms));
        }
    }

    connector.queueEvent(HEAPWATCH_EVENT_STARTING_UP);
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
        Heapwatch::Lifecycle::Bootstrap::clearWifiPermanentlyFailed();
        if (connector.getStatus().initialized) {
            connector.queueEvent(HEAPWATCH_EVENT_INTERNET_CONNECTED);
        }
        ESP_LOGI(TAG, "Network IP acquired (event %" PRId32 ")", static_cast<int32_t>(event_id));
        onRuntimeConnectivityChanged();
    } else if (event_id == IP_EVENT_STA_LOST_IP || event_id == IP_EVENT_ETH_LOST_IP) {
        connector.setNetworkReady(false);
        if (connector.getStatus().initialized) {
            connector.queueEvent(HEAPWATCH_EVENT_INTERNET_DISCONNECTED);
        }
        ESP_LOGI(TAG, "Network IP lost (event %" PRId32 ")", static_cast<int32_t>(event_id));
        onRuntimeConnectivityChanged();
    }
}

void registerNetIpEventHandlers() {
    if (g_ip_handlers_registered) {
        return;
    }
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        onNetIpEvent, nullptr, &g_ip_sta_got));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                                        onNetIpEvent, nullptr, &g_ip_eth_got));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                                        onNetIpEvent, nullptr, &g_ip_sta_lost));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_LOST_IP,
                                                        onNetIpEvent, nullptr, &g_ip_eth_lost));
    g_ip_handlers_registered = true;
}

void unregisterNetIpEventHandlers() {
    if (!g_ip_handlers_registered) {
        return;
    }
    (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, g_ip_sta_got);
    (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, g_ip_eth_got);
    (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_LOST_IP, g_ip_sta_lost);
    (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_ETH_LOST_IP, g_ip_eth_lost);
    g_ip_sta_got = nullptr;
    g_ip_eth_got = nullptr;
    g_ip_sta_lost = nullptr;
    g_ip_eth_lost = nullptr;
    g_ip_handlers_registered = false;
}

void bootstrapTask(void*) {
    setPhase(HEAPWATCH_PHASE_STARTING);
    Heapwatch::Lifecycle::Bootstrap::startWifiIfManaged();
    Heapwatch::Lifecycle::Bootstrap::startNetworkReporters();

    while (!g_bootstrap_stop) {
        updateMqttPhaseFromStatus();

        if (!heapwatch_network_ready()) {
            if (Heapwatch::Lifecycle::Bootstrap::isWifiPermanentlyFailed()) {
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
            Heapwatch::Lifecycle::Bootstrap::syncTimeIfManaged();
        }

        if (HeapwatchConnector::getInstance().getStatus().initialized) {
            if (!g_boot_cycle_resumed) {
                g_boot_cycle_resumed = true;
                resumeBootCycleFromServer();
            }
            updateMqttPhaseFromStatus();
            refreshHandoffLed();
            ESP_LOGI(TAG, "hw_bootstrap done — exiting");
            break;
        }

        if (g_enrollment_cycle_active) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        Heapwatch::Lifecycle::Bootstrap::BootCredentials creds = {};
        if (Heapwatch::Lifecycle::Bootstrap::loadBootCredentials(&creds)) {
            if (activateConnectorFromCredentials(creds)) {
                setPhase(HEAPWATCH_PHASE_READY);
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        g_enrollment_cycle_active = true;
        setPhase(HEAPWATCH_PHASE_PROVISIONING);
        const bool enrolled = Heapwatch::Lifecycle::Bootstrap::runFactoryEnrollment(&creds);
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
    g_bootstrap_exited = true;
    g_bootstrap_task = nullptr;
    heapwatch_task_delete_dram(nullptr);
}

}  // namespace

void setPhase(heapwatch_phase_t phase) { g_phase = phase; }

heapwatch_phase_t getPhase() { return g_phase; }

void updateMqttPhaseFromStatus() {
    const HeapwatchConnectorStatus status = HeapwatchConnector::getInstance().getStatus();
    if (!status.initialized) {
        return;
    }
    if (status.mqtt_connected) {
        setPhase(HEAPWATCH_PHASE_MQTT_CONNECTED);
    } else if (Heapwatch::Lifecycle::Bootstrap::isWifiPermanentlyFailed()) {
        setPhase(HEAPWATCH_PHASE_NETWORK_FAILED);
    } else if (status.network_ready) {
        setPhase(HEAPWATCH_PHASE_MQTT_CONNECTING);
    } else if (getPhase() != HEAPWATCH_PHASE_NETWORK_FAILED &&
               getPhase() != HEAPWATCH_PHASE_PROVISIONING &&
               getPhase() != HEAPWATCH_PHASE_WAITING_ACCEPT) {
        setPhase(HEAPWATCH_PHASE_READY);
    }
}

void refreshHandoffLed() {
#if CONFIG_HEAPWATCH_ENROLL_STATE_LED_INDICATOR
    if (!HeapwatchConnector::getInstance().getStatus().initialized) {
        return;
    }
    if (Heapwatch::Lifecycle::Bootstrap::isWifiPermanentlyFailed()) {
        Heapwatch::Enrollment::setIndicatorPhase(
            Heapwatch::Enrollment::EnrollStateLedPhase::kWifiFailed);
    } else if (!heapwatch_network_ready()) {
        Heapwatch::Enrollment::setIndicatorPhase(
            Heapwatch::Enrollment::EnrollStateLedPhase::kNetworkError);
    } else {
        Heapwatch::Enrollment::setIndicatorPhase(
            Heapwatch::Enrollment::EnrollStateLedPhase::kProvisioned);
    }
#endif
}

void onRuntimeConnectivityChanged() {
    updateMqttPhaseFromStatus();
    refreshHandoffLed();
}

bool activateConnectorFromCredentials(const Heapwatch::Lifecycle::Bootstrap::BootCredentials& creds) {
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
        Heapwatch::StreamIngress::setStreamApiKey(creds.stream_api_key);
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
        ESP_LOGE(TAG,
                 "FATAL: HEAPWATCH_BUILD_IDENTIFIER is still the pre-link placeholder — run a full "
                 "idf.py build so the post-link fingerprint step can run");
        g_misconfigured = true;
        setPhase(HEAPWATCH_PHASE_MISCONFIGURED);
        return ESP_ERR_HEAPWATCH_INIT_FAILED;
    }

    const esp_err_t nvs_err = initNvsFlash();
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "FATAL: nvs_flash_init failed: %s", esp_err_to_name(nvs_err));
        g_misconfigured = true;
        setPhase(HEAPWATCH_PHASE_MISCONFIGURED);
        return ESP_ERR_HEAPWATCH_INIT_FAILED;
    }

    initBootIdentity();

    if (hasProvisionedCredentialsInNvs()) {
        ESP_LOGI(TAG, "Provisioned credentials in NVS — empty enrollment key is OK");
    } else if (CONFIG_HEAPWATCH_ENROLLMENT_KEY[0] == '\0') {
        ESP_LOGE(TAG,
                 "FATAL: device is not provisioned and CONFIG_HEAPWATCH_ENROLLMENT_KEY is empty — "
                 "cannot enroll");
        g_misconfigured = true;
        setPhase(HEAPWATCH_PHASE_MISCONFIGURED);
        return ESP_ERR_HEAPWATCH_NOT_PROVISIONED;
    }

    Heapwatch::Lifecycle::Bootstrap::ensureDefaultEventLoop();
    registerNetIpEventHandlers();

    if (!HeapwatchConnector::getInstance().ensureInfrastructure()) {
        ESP_LOGE(TAG, "FATAL: failed to create connector message queues");
        g_misconfigured = true;
        setPhase(HEAPWATCH_PHASE_MISCONFIGURED);
        return ESP_ERR_HEAPWATCH_INIT_FAILED;
    }

    Heapwatch::Lifecycle::Bootstrap::installLogRelayHookIfEnabled();
    Heapwatch::Lifecycle::Bootstrap::startLogRelayTaskIfEnabled();

    queueBootLifecycleTelemetry();

    if (!g_bootstrap_started) {
        g_bootstrap_started = true;
        g_bootstrap_stop = false;
        g_bootstrap_exited = false;
        constexpr uint32_t BOOTSTRAP_STACK_BYTES = 10240;
        if (heapwatch_task_create_dram(bootstrapTask, "hw_bootstrap", BOOTSTRAP_STACK_BYTES, nullptr,
                                         tskIDLE_PRIORITY + 2, &g_bootstrap_task) != pdPASS) {
            ESP_LOGE(TAG, "FATAL: failed to start hw_bootstrap task");
            g_bootstrap_started = false;
            g_bootstrap_exited = true;
            g_misconfigured = true;
            setPhase(HEAPWATCH_PHASE_MISCONFIGURED);
            return ESP_ERR_HEAPWATCH_INIT_FAILED;
        }
    }

    setPhase(HEAPWATCH_PHASE_STARTING);
    return ESP_OK;
}

void deinitialize() {
    g_bootstrap_stop = true;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(15000);
    while (!g_bootstrap_exited && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    HeapwatchConnector::getInstance().deinitialize();
    Heapwatch::Lifecycle::Bootstrap::stopLogRelay();
    Heapwatch::Lifecycle::Bootstrap::stopTimeSyncIfManaged();
    Heapwatch::Lifecycle::Bootstrap::stopManagedWifi();
    unregisterNetIpEventHandlers();
    Heapwatch::Enrollment::stopIndicator();
    Heapwatch::Network::stop();
    Heapwatch::Fs::Volumes::unmountConnectorOwned();

    g_initialize_called = false;
    g_misconfigured = false;
    g_bootstrap_started = false;
    g_time_sync_attempted = false;
    g_enrollment_cycle_active = false;
    g_bootstrap_task = nullptr;
    g_bootstrap_exited = true;
    g_boot_cycle_resumed = false;
    g_boot_state = {};
    setPhase(HEAPWATCH_PHASE_STARTING);
}

bool initializeCalled() {
    return g_initialize_called;
}

}  // namespace Heapwatch::Lifecycle
