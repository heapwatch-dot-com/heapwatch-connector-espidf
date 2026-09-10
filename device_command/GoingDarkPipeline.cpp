#include "GoingDarkPipeline.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "../connector_settings/ConnectorSettings.h"
#include "../heapwatch_config.h"
#include "../heapwatch_dispatcher/HeapwatchDispatcher.h"
#include "../heapwatch_connector/ConnectorBootstrap.h"
#include "../heapwatch_connector/HeapwatchConnector.h"
#include "../sleep_tracking/SleepTracking.h"
#include "../heapwatch.h"
#include "DeviceCommandHandler.h"
#include "SleepHooks.h"

namespace heapwatch::going_dark {

namespace {

using heapwatch::device_command::publishCommandAck;
using heapwatch::device_command::publishCommandResponse;

constexpr char TAG[] = "HwCmd";
constexpr uint32_t kEventPublishGraceMs = 1500;
constexpr size_t kCommandIdMax = 64;
constexpr size_t kSuffixMax = 64;

enum class RequestKind : uint8_t {
    Restart,
    Shutdown,
    Sleep,
    WifiReconnect,
};

struct GoingDarkRequest {
    char suffix[kSuffixMax] = {};
    char command_id[kCommandIdMax] = {};
    RequestKind kind = RequestKind::Sleep;
    SleepKind sleep_kind = SleepKind::Light;
    uint32_t duration_s = 0;
};

QueueHandle_t g_request_queue = nullptr;
volatile bool g_active = false;

void rejectRequest(const GoingDarkRequest& request, const char* code, const char* message) {
    publishCommandResponse(request.suffix, request.command_id, false, code, message);
    g_active = false;
}

void reportLightSleepWakeContext(int64_t sleep_started_us) {
    const int64_t elapsed_ms = (esp_timer_get_time() - sleep_started_us) / 1000;
    const heapwatch_wake_cause_t cause = heapwatch_translate_current_wake_cause();
    auto& connector = HeapwatchConnector::getInstance();
    connector.queueParameter("wake_cause_code", static_cast<int64_t>(cause));
    connector.queueParameter("sleep_duration_ms", elapsed_ms);
}

void configureTimerWakeup(uint32_t duration_s) {
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (duration_s > 0) {
        const uint64_t duration_us = static_cast<uint64_t>(duration_s) * 1000000ULL;
        ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(duration_us));
    }
}

void waitOutboxDrained(uint32_t timeout_ms) {
    const TickType_t start = xTaskGetTickCount();
    while (heapwatch_dispatcher_get_outbox_size() > 0) {
        const TickType_t elapsed_ms =
            static_cast<uint32_t>((xTaskGetTickCount() - start) * portTICK_PERIOD_MS);
        if (elapsed_ms >= timeout_ms) {
            ESP_LOGW(TAG, "Outbox drain timeout (%u ms)", static_cast<unsigned>(timeout_ms));
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * Shared going-dark step for every connectivity-terminating command:
 * publish command ack → arm shutdown → drain MQTT outbox.
 */
bool ackAndDrain(const GoingDarkRequest& request, const char* ack_message,
                 HeapwatchShutdownAction shutdown_action) {
    if (!publishCommandAck(request.suffix, request.command_id, ack_message)) {
        g_active = false;
        ESP_LOGE(TAG, "Failed to publish going-dark ack for %s", request.suffix);
        return false;
    }

    heapwatch_dispatcher_begin_shutdown(shutdown_action);

    const uint32_t drain_timeout_ms = heapwatch::connector_settings::outboxDrainTimeoutMs();
    waitOutboxDrained(drain_timeout_ms);
    return true;
}

void runRestartPipeline(GoingDarkRequest request) {
    if (!ackAndDrain(request, "restart accepted", HeapwatchShutdownAction::Restart)) {
        return;
    }

    // Dispatcher reboots once the outbox is empty (Restart action).
    g_active = false;
}

void runWifiReconnectPipeline(GoingDarkRequest request) {
    if (!ackAndDrain(request, "wifi reconnect accepted", HeapwatchShutdownAction::SleepHandoff)) {
        return;
    }

    // Outbox drain only ensures the command ack (and any prior tail) was sent. Unlike
    // restart/sleep the device keeps running — re-enable queuing before the Wi-Fi outage
    // so metrics and events buffer in RAM and flush once MQTT is back.
    heapwatch_dispatcher_disarm_shutdown();

#if CONFIG_HEAPWATCH_MANAGE_WIFI
    if (!heapwatch::connector_bootstrap::requestWifiReconnect(request.duration_s)) {
        g_active = false;
        ESP_LOGE(TAG, "Wi-Fi reconnect failed after ack");
        return;
    }
#else
    (void)request.duration_s;
    g_active = false;
    ESP_LOGE(TAG, "Wi-Fi reconnect not available (HEAPWATCH_MANAGE_WIFI=n)");
    return;
#endif

    g_active = false;
    ESP_LOGI(TAG, "Wi-Fi reconnect pipeline complete");
}

void runSleepPipeline(GoingDarkRequest request) {
    const bool hooks_ok =
        request.sleep_kind == SleepKind::Light ? sleep_hooks::runBeforeLightSleepHooks()
                                               : sleep_hooks::runBeforeDeepSleepHooks();
    if (!hooks_ok || sleep_hooks::isMustStayAwake() || heapwatch_keep_running()) {
        rejectRequest(request, "interlock", "Sleep blocked by hook or keep_running");
        return;
    }

    auto& connector = HeapwatchConnector::getInstance();
    if (request.sleep_kind == SleepKind::Light) {
        connector.queueEvent(HEAPWATCH_EVENT_ENTERING_LIGHT_SLEEP);
    } else {
        connector.queueEvent(HEAPWATCH_EVENT_ENTERING_DEEP_SLEEP);
    }

    // Let the dispatcher move the pre-sleep event toward MQTT before we arm shutdown.
    vTaskDelay(pdMS_TO_TICKS(kEventPublishGraceMs));

    const char* ack_message =
        request.sleep_kind == SleepKind::Light ? "light sleep accepted" : "deep sleep accepted";
    if (!ackAndDrain(request, ack_message, HeapwatchShutdownAction::SleepHandoff)) {
        return;
    }

    configureTimerWakeup(request.duration_s);

    if (request.sleep_kind == SleepKind::Light) {
        const int64_t sleep_started_us = esp_timer_get_time();
        ESP_LOGI(TAG, "Entering light sleep (duration_s=%u)",
                 static_cast<unsigned>(request.duration_s));
        esp_light_sleep_start();

        heapwatch_dispatcher_disarm_shutdown();
        g_active = false;

        connector.queueEvent(HEAPWATCH_EVENT_WOKE_UP_FROM_LIGHT_SLEEP);
        reportLightSleepWakeContext(sleep_started_us);
        vTaskDelay(pdMS_TO_TICKS(kEventPublishGraceMs));
        ESP_LOGI(TAG, "Woke from light sleep");
        return;
    }

    ESP_LOGI(TAG, "Entering deep sleep (duration_s=%u)", static_cast<unsigned>(request.duration_s));
    heapwatch_sleep_tracking_mark_entering_deep_sleep();
    esp_deep_sleep_start();
}

void runShutdownPipeline(GoingDarkRequest request) {
    if (!ackAndDrain(request, "shutdown accepted", HeapwatchShutdownAction::SleepHandoff)) {
        return;
    }

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    ESP_LOGI(TAG, "Entering indefinite deep sleep (shutdown)");
    esp_deep_sleep_start();
}

void goingDarkTask(void* /*arg*/) {
    GoingDarkRequest request = {};
    while (true) {
        if (xQueueReceive(g_request_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (request.kind) {
            case RequestKind::Restart:
                runRestartPipeline(request);
                break;
            case RequestKind::Shutdown:
                runShutdownPipeline(request);
                break;
            case RequestKind::WifiReconnect:
                runWifiReconnectPipeline(request);
                break;
            case RequestKind::Sleep:
                runSleepPipeline(request);
                break;
        }
    }
}

}  // namespace

void init() {
    if (g_request_queue != nullptr) {
        return;
    }

    g_request_queue = xQueueCreate(1, sizeof(GoingDarkRequest));
    if (g_request_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create request queue");
        return;
    }

    xTaskCreate(goingDarkTask, "hw_going_dark", 6144, nullptr, tskIDLE_PRIORITY + 2, nullptr);
}

bool isActive() { return g_active; }

bool scheduleRequest(const GoingDarkRequest& request) {
    if (g_request_queue == nullptr) {
        return false;
    }
    if (g_active || heapwatch_dispatcher_is_shutting_down()) {
        return false;
    }

    g_active = true;
    if (xQueueSend(g_request_queue, &request, 0) != pdTRUE) {
        g_active = false;
        return false;
    }

    return true;
}

bool scheduleRestart(const char* suffix, const char* command_id) {
    if (suffix == nullptr || command_id == nullptr) {
        return false;
    }

    GoingDarkRequest request = {};
    snprintf(request.suffix, sizeof(request.suffix), "%s", suffix);
    snprintf(request.command_id, sizeof(request.command_id), "%s", command_id);
    request.kind = RequestKind::Restart;

    return scheduleRequest(request);
}

bool scheduleShutdown(const char* suffix, const char* command_id) {
    if (suffix == nullptr || command_id == nullptr) {
        return false;
    }

    GoingDarkRequest request = {};
    snprintf(request.suffix, sizeof(request.suffix), "%s", suffix);
    snprintf(request.command_id, sizeof(request.command_id), "%s", command_id);
    request.kind = RequestKind::Shutdown;

    return scheduleRequest(request);
}

bool scheduleSleep(const char* suffix, const char* command_id, SleepKind kind, uint32_t duration_s) {
    if (suffix == nullptr || command_id == nullptr) {
        return false;
    }

    GoingDarkRequest request = {};
    snprintf(request.suffix, sizeof(request.suffix), "%s", suffix);
    snprintf(request.command_id, sizeof(request.command_id), "%s", command_id);
    request.kind = RequestKind::Sleep;
    request.sleep_kind = kind;
    request.duration_s = duration_s;

    return scheduleRequest(request);
}

bool scheduleWifiReconnect(const char* suffix, const char* command_id, uint32_t duration_s) {
    if (suffix == nullptr || command_id == nullptr) {
        return false;
    }

    GoingDarkRequest request = {};
    snprintf(request.suffix, sizeof(request.suffix), "%s", suffix);
    snprintf(request.command_id, sizeof(request.command_id), "%s", command_id);
    request.kind = RequestKind::WifiReconnect;
    request.duration_s = duration_s;

    return scheduleRequest(request);
}

}  // namespace heapwatch::going_dark
