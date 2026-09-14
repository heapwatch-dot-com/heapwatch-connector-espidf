#include "heapwatch/DeviceCommand/CommandWorker.h"

#include <cstdio>
#include <cstring>
#include <cinttypes>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "heapwatch/Enrollment/CredentialRotation/DeviceCredentialRotationClient.h"
#include "heapwatch/Enrollment/Credentials/DeviceCredentialsStore.h"
#include "heapwatch/Mqtt/HeapwatchConnector.h"
#include "heapwatch/Helpers/heapwatch_freertos_alloc.h"
#include "heapwatch/Helpers/MemoryManager/HeapwatchMemoryManager.h"
#include "heapwatch/Mqtt/HeapwatchDispatcher.h"
#include "heapwatch_endpoints.h"
#include "heapwatch/DeviceCommand/DeviceCommandHandler.h"

namespace Heapwatch::DeviceCommand {

namespace {

constexpr char TAG[] = "HwCmd";
constexpr uint32_t WORKER_STACK_BYTES = 10240;

QueueHandle_t commandQueue() {
    Heapwatch::Helpers::HeapwatchWorkingSet* ws = Heapwatch::Helpers::MemoryManager::get();
    return ws != nullptr ? ws->command_inbound_queue : nullptr;
}

volatile bool g_worker_exited = true;

void executeCredentialRotation() {
    Heapwatch::Enrollment::Credentials::Credentials current = {};
    if (!Heapwatch::Enrollment::Credentials::load(&current)) {
        current.tenant_slug[0] = '\0';
        std::strncpy(current.tenant_slug, heapwatch_get_tenant_slug(),
                     sizeof(current.tenant_slug) - 1);
        std::strncpy(current.device_uid, heapwatch_get_device_uid(), sizeof(current.device_uid) - 1);
        std::strncpy(current.mqtt_password, heapwatch_get_broker_password(),
                     sizeof(current.mqtt_password) - 1);
        const char* auth = heapwatch_get_rest_api_auth_token();
        if (auth != nullptr && std::strncmp(auth, "Bearer ", 7) == 0) {
            std::strncpy(current.device_api_key, auth + 7, sizeof(current.device_api_key) - 1);
        }
    }

    Heapwatch::Enrollment::Credentials::Credentials updated = {};
    if (!Heapwatch::Enrollment::CredentialRotation::fetchAndConfirm(HEAPWATCH_API_BASE_URL, &current,
                                                                 &updated)) {
        ESP_LOGE(TAG, "Credential rotation failed");
        return;
    }

    if (!Heapwatch::Enrollment::Credentials::save(&updated)) {
        ESP_LOGE(TAG, "Failed to persist rotated credentials");
        return;
    }

    HeapwatchConnector::getInstance().setMqttPassword(updated.mqtt_password);
    char auth_header[HEAPWATCH_MAX_DEVICE_API_KEY_LEN + 16] = {};
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", updated.device_api_key);
    HeapwatchConnector::getInstance().setRestApiAuthToken(auth_header);
    heapwatch_dispatcher_request_reconnect();
}

void commandWorkerTask(void* /*arg*/) {
    while (true) {
        CommandInboundMessage msg = {};
        if (xQueueReceive(commandQueue(), &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (msg.kind == InboundKind::Stop) {
            break;
        }

        if (msg.kind == InboundKind::CredentialRotation) {
            executeCredentialRotation();
            continue;
        }

        handleMqttMessage(msg.topic, msg.topic_len, msg.payload, msg.payload_len, msg.mqtt_retain);
    }
    g_worker_exited = true;
    heapwatch_task_delete_dram(nullptr);
}

}  // namespace

void initCommandWorker() {
    if (!g_worker_exited) {
        return;
    }
    if (commandQueue() == nullptr) {
        ESP_LOGE(TAG, "Command inbound queue missing (working set not inited)");
        return;
    }

    g_worker_exited = false;
    if (heapwatch_task_create_dram(commandWorkerTask, "hw_command", WORKER_STACK_BYTES, nullptr, tskIDLE_PRIORITY + 2,
                                  nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create hw_command task");
        g_worker_exited = true;
    }
}

void shutdownCommandWorker() {
    QueueHandle_t queue = commandQueue();
    if (queue == nullptr || g_worker_exited) {
        return;
    }

    CommandInboundMessage msg = {};
    msg.kind = InboundKind::Stop;
    (void)xQueueSend(queue, &msg, pdMS_TO_TICKS(200));

    // Generous: a queued command may still be doing HTTP (credential rotation).
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
    while (!g_worker_exited && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!g_worker_exited) {
        ESP_LOGE(TAG, "hw_command did not stop in time — leaving it running");
    }
}

bool commandWorkerExited() { return g_worker_exited; }

bool enqueueMqttCommand(const char* topic, size_t topic_len, const char* payload, size_t payload_len,
                        int32_t mqtt_retain) {
    QueueHandle_t queue = commandQueue();
    if (queue == nullptr || topic == nullptr || payload == nullptr) {
        return false;
    }
    if (topic_len == 0 || topic_len >= HEAPWATCH_COMMAND_TOPIC_MAX ||
        payload_len >= HEAPWATCH_COMMAND_PAYLOAD_MAX) {
        ESP_LOGW(TAG, "Dropping oversize MQTT command (topic_len=%" PRIu32 " payload_len=%" PRIu32 ")",
                 static_cast<uint32_t>(topic_len), static_cast<uint32_t>(payload_len));
        return false;
    }

    CommandInboundMessage msg = {};
    msg.kind = InboundKind::MqttCommand;
    msg.mqtt_retain = static_cast<uint8_t>(mqtt_retain);
    msg.topic_len = static_cast<uint16_t>(topic_len);
    msg.payload_len = static_cast<uint16_t>(payload_len);
    std::memcpy(msg.topic, topic, topic_len);
    msg.topic[topic_len] = '\0';
    std::memcpy(msg.payload, payload, payload_len);
    msg.payload[payload_len] = '\0';

    if (xQueueSend(queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Inbound command queue full — dropping command");
        return false;
    }

    return true;
}

bool enqueueCredentialRotation() {
    QueueHandle_t queue = commandQueue();
    if (queue == nullptr) {
        return false;
    }

    CommandInboundMessage msg = {};
    msg.kind = InboundKind::CredentialRotation;
    if (xQueueSend(queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Inbound command queue full — dropping credential rotation");
        return false;
    }

    return true;
}

}  // namespace Heapwatch::DeviceCommand
