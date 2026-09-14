#include "CommandWorker.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "../device_credential_rotation/DeviceCredentialRotationClient.h"
#include "../device_credentials/DeviceCredentialsStore.h"
#include "../heapwatch_connector/HeapwatchConnector.h"
#include "../heapwatch_connector/heapwatch_freertos_alloc.h"
#include "../heapwatch_dispatcher/HeapwatchDispatcher.h"
#include "../heapwatch_endpoints.h"
#include "DeviceCommandHandler.h"

namespace heapwatch::device_command {

namespace {

constexpr char TAG[] = "HwCmd";
constexpr size_t INBOUND_QUEUE_DEPTH = 8;
constexpr size_t TOPIC_MAX = 160;
/** Matches CONFIG_MQTT_BUFFER_SIZE on client firmware projects. */
constexpr size_t PAYLOAD_MAX = 1024;
constexpr uint32_t WORKER_STACK_BYTES = 10240;

enum class InboundKind : uint8_t {
    MqttCommand = 0,
    CredentialRotation = 1,
    Stop = 2,
};

struct CommandInboundMessage {
    InboundKind kind = InboundKind::MqttCommand;
    uint8_t mqtt_retain = 0;
    uint16_t topic_len = 0;
    uint16_t payload_len = 0;
    char topic[TOPIC_MAX] = {};
    char payload[PAYLOAD_MAX] = {};
};

QueueHandle_t g_inbound_queue = nullptr;
volatile bool g_worker_exited = true;

void executeCredentialRotation() {
    heapwatch::device_credentials::Credentials current = {};
    if (!heapwatch::device_credentials::load(&current)) {
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

    heapwatch::device_credentials::Credentials updated = {};
    if (!heapwatch::device_credential_rotation::fetchAndConfirm(HEAPWATCH_API_BASE_URL, &current,
                                                                 &updated)) {
        ESP_LOGE(TAG, "Credential rotation failed");
        return;
    }

    if (!heapwatch::device_credentials::save(&updated)) {
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
        if (xQueueReceive(g_inbound_queue, &msg, portMAX_DELAY) != pdTRUE) {
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
    if (g_inbound_queue != nullptr) {
        return;
    }

    g_inbound_queue = heapwatch_queue_create(INBOUND_QUEUE_DEPTH, sizeof(CommandInboundMessage));
    if (g_inbound_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create inbound command queue");
        return;
    }

    g_worker_exited = false;
    if (heapwatch_task_create_dram(commandWorkerTask, "hw_command", WORKER_STACK_BYTES, nullptr, tskIDLE_PRIORITY + 2,
                                  nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create hw_command task");
        heapwatch_queue_delete(g_inbound_queue);
        g_inbound_queue = nullptr;
        g_worker_exited = true;
    }
}

void shutdownCommandWorker() {
    if (g_inbound_queue == nullptr) {
        return;
    }

    CommandInboundMessage msg = {};
    msg.kind = InboundKind::Stop;
    (void)xQueueSend(g_inbound_queue, &msg, pdMS_TO_TICKS(200));

    // Generous: a queued command may still be doing HTTP (credential rotation).
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
    while (!g_worker_exited && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!g_worker_exited) {
        ESP_LOGE(TAG, "hw_command did not stop in time — leaving it running");
        return;
    }

    heapwatch_queue_delete(g_inbound_queue);
    g_inbound_queue = nullptr;
}

bool enqueueMqttCommand(const char* topic, size_t topic_len, const char* payload, size_t payload_len,
                        int mqtt_retain) {
    if (g_inbound_queue == nullptr || topic == nullptr || payload == nullptr) {
        return false;
    }
    if (topic_len == 0 || topic_len >= TOPIC_MAX || payload_len >= PAYLOAD_MAX) {
        ESP_LOGW(TAG, "Dropping oversize MQTT command (topic_len=%u payload_len=%u)",
                 static_cast<unsigned>(topic_len), static_cast<unsigned>(payload_len));
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

    if (xQueueSend(g_inbound_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Inbound command queue full — dropping command");
        return false;
    }

    return true;
}

bool enqueueCredentialRotation() {
    if (g_inbound_queue == nullptr) {
        return false;
    }

    CommandInboundMessage msg = {};
    msg.kind = InboundKind::CredentialRotation;
    if (xQueueSend(g_inbound_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Inbound command queue full — dropping credential rotation");
        return false;
    }

    return true;
}

}  // namespace heapwatch::device_command
