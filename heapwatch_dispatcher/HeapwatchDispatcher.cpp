// Copyright (c) 2026 Heapwatch. All rights reserved.
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

#include "HeapwatchDispatcher.h"
#include <stdlib.h>
#include <string.h>

#include <cstring>
#include <string>
#include <sys/time.h>

#include "esp_crt_bundle.h"
#include "esp_core_dump.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "mqtt_client.h"
#include "../heapwatch_connector/HeapwatchConnector.h"
#include "device_enrollment/ClaimedIdentitiesBootSync.h"
#include "../device_command/CommandWorker.h"
#include "heapwatch_config.h"
#include "heapwatch_endpoints.h"
#include "sdkconfig.h"

#include "freertos/task.h"

/* ============================================================================
   Static Member Definitions
   =========================================================================== */

volatile bool g_mqtt_connected = false;
volatile bool g_mqtt_shutting_down = false;
static esp_mqtt_client_handle_t g_mqtt_client = nullptr;
static std::string g_mqtt_username;
static char g_lwt_topic[128] = {};
static volatile bool g_mqtt_reconnect_requested = false;
static char g_command_topic[128] = {};

extern const char heapwatch_local_ca_pem_start[] asm("_binary_heapwatch_local_ca_pem_start");
extern const char heapwatch_local_ca_pem_end[] asm("_binary_heapwatch_local_ca_pem_end");

static constexpr char k_dev_mqtts_hostname[] = "mqtts.mqtt.heapwatch.local";
// T2: production broker cert CN/SAN — verified rather than skipped, same rigor as dev.
static constexpr char k_prod_mqtts_hostname[] = "mqtts.mqtt.heapwatch.com";
/** esp-mqtt outbox bytes; 0 = unlimited. Keep well under free-heap headroom for TLS. */
static constexpr uint64_t k_mqtt_outbox_limit_bytes = 16 * 1024;
static constexpr uint32_t k_mqtt_outbox_full_backoff_ms = 10;

static HeapwatchShutdownAction g_shutdown_action = HeapwatchShutdownAction::Restart;

// STAT1: -1 means "no successful publish yet this boot".
static volatile int64_t g_last_publish_unix_ms = -1;

// After a failed publish we leave the head in the FreeRTOS queue (peek-only).
// Queue sets only wake on new data / post-receive leftover notify — so sticky-retry
// the blocked queue instead of SendToFront requeue (which crashed under pressure).
static QueueHandle_t g_publish_blocked_queue = nullptr;

static void mark_publish_success() {
    struct timeval now = {};
    gettimeofday(&now, nullptr);
    g_last_publish_unix_ms =
        static_cast<int64_t>(now.tv_sec) * 1000 + static_cast<int64_t>(now.tv_usec) / 1000;
}

static bool mqtt_try_publish(const char* topic, const char* payload, int payload_len, int qos,
                             bool store) {
    if (g_mqtt_shutting_down || g_mqtt_client == nullptr) {
        return false;
    }

    const int result =
        esp_mqtt_client_enqueue(g_mqtt_client, topic, payload, payload_len, qos, 0, store);
    if (result >= 0) {
        mark_publish_success();
        return true;
    }

    if (result == -2) {
        vTaskDelay(pdMS_TO_TICKS(k_mqtt_outbox_full_backoff_ms));
    }
    return false;
}

static const char* get_topic_for_message_type(enum MqttMessageType msg_type);

/** Peek head → MQTT outbox → only then Receive (pop). Never SendToFront. */
template <typename MsgT>
static void try_publish_queue_head(QueueHandle_t queue) {
    MsgT msg;
    if (xQueuePeek(queue, &msg, 0) != pdTRUE) {
        if (g_publish_blocked_queue == queue) {
            g_publish_blocked_queue = nullptr;
        }
        return;
    }

    const char* topic = get_topic_for_message_type(msg.type);
    const int payload_len = static_cast<int>(strlen(msg.payload));
    if (!mqtt_try_publish(topic, msg.payload, payload_len, 0, true)) {
        g_publish_blocked_queue = queue;
        return;
    }

    (void)xQueueReceive(queue, &msg, 0);
    g_publish_blocked_queue = nullptr;
}

/** Boot-critical outbound first: large (specs/bounds/network) → tiny (events) → small (telemetry). */
static bool try_publish_priority_outbound(QueueHandle_t large_queue, QueueHandle_t tiny_queue,
                                          QueueHandle_t small_queue) {
    if (large_queue != nullptr && uxQueueMessagesWaiting(large_queue) > 0) {
        try_publish_queue_head<LargeMqttMessage>(large_queue);
        return true;
    }
    if (tiny_queue != nullptr && uxQueueMessagesWaiting(tiny_queue) > 0) {
        try_publish_queue_head<TinyMqttMessage>(tiny_queue);
        return true;
    }
    if (small_queue != nullptr && uxQueueMessagesWaiting(small_queue) > 0) {
        try_publish_queue_head<SmallMqttMessage>(small_queue);
        return true;
    }
    return false;
}

static void configure_mqtt_tls_verification(esp_mqtt_client_config_t& config) {
    if (std::strcmp(CONFIG_HEAPWATCH_BUILD_ENV, "development") == 0) {
        // Local dev: broker URI is LAN IP; common_name drives TLS SNI for Caddy L4.
#if defined(CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY) && CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
        // Caddy uses a local CA that is awkward to keep synced in flash; skip verify in
        // dev only (sdkconfig.defaults). SNI must still be set (skip_common_name=false).
        config.broker.verification.certificate = nullptr;
        config.broker.verification.certificate_len = 0;
        config.broker.verification.crt_bundle_attach = nullptr;
#else
        // PEM length must stay 0 so esp-mqtt uses strlen() for PEM parse (non-zero selects DER).
        config.broker.verification.certificate =
            static_cast<const char*>(static_cast<const void*>(heapwatch_local_ca_pem_start));
        config.broker.verification.certificate_len = 0;
        config.broker.verification.crt_bundle_attach = nullptr;
#endif
        config.broker.address.hostname = k_dev_mqtts_hostname;
        config.broker.verification.common_name = k_dev_mqtts_hostname;
        config.broker.verification.skip_cert_common_name_check = false;
        return;
    }

    // T2 (fixed): production previously skipped CN/SAN verification, which weakens the
    // "SUPER STABLE"/secure bar for a production device fleet. Verify the broker's cert
    // against its real hostname, same rigor as the dev path above.
    config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    config.broker.verification.common_name = k_prod_mqtts_hostname;
    config.broker.verification.skip_cert_common_name_check = false;
}

/**
 * Create esp-mqtt client: prefer MQTT v5 when compiled in (CONFIG_MQTT_PROTOCOL_5),
 * fall back to v3.1.1 (CONFIG_MQTT_PROTOCOL_311) so client firmware builds without v5
 * in sdkconfig still connect to the broker.
 */
static esp_mqtt_client_handle_t create_mqtt_client(esp_mqtt_client_config_t& config) {
#if CONFIG_MQTT_PROTOCOL_5
    config.session.protocol_ver = MQTT_PROTOCOL_V_5;
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&config);
    if (client != nullptr) {
        ESP_LOGI("HeapwatchDispatcher", "MQTT client created (protocol v5)");
        return client;
    }
    ESP_LOGW("HeapwatchDispatcher", "MQTT v5 init failed, trying v3.1.1");
#endif

#if CONFIG_MQTT_PROTOCOL_311
    config.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&config);
    if (client != nullptr) {
        ESP_LOGI("HeapwatchDispatcher", "MQTT client created (protocol v3.1.1)");
        return client;
    }
    ESP_LOGE("HeapwatchDispatcher", "MQTT v3.1.1 client init failed");
#else
    ESP_LOGE("HeapwatchDispatcher",
             "No MQTT protocol enabled in sdkconfig (enable CONFIG_MQTT_PROTOCOL_5 and/or "
             "CONFIG_MQTT_PROTOCOL_311)");
#endif

    return nullptr;
}

/* ============================================================================
   Topic Functions
   =========================================================================== */

static const char* get_topic_for_message_type(enum MqttMessageType msg_type) {
    const char* uid = heapwatch_get_device_uid();
    const char* tenant = heapwatch_get_tenant_slug();
    static char topic_buf[128];
    topic_buf[0] = '\0';
    switch (msg_type) {
        case MQTT_MESSAGE_TYPE_LOG:
            snprintf(topic_buf, sizeof(topic_buf), "%s/device/%s/log", tenant, uid);
            return topic_buf;
        case MQTT_MESSAGE_TYPE_PARAMETER:
            snprintf(topic_buf, sizeof(topic_buf), "%s/device/%s/parameter", tenant, uid);
            return topic_buf;
        case MQTT_MESSAGE_TYPE_EVENT:
            snprintf(topic_buf, sizeof(topic_buf), "%s/device/%s/event", tenant, uid);
            return topic_buf;
        case MQTT_MESSAGE_TYPE_SPECS:
            snprintf(topic_buf, sizeof(topic_buf), "%s/device/%s/specs", tenant, uid);
            return topic_buf;
        case MQTT_MESSAGE_TYPE_TASKS:
            snprintf(topic_buf, sizeof(topic_buf), "%s/device/%s/tasks", tenant, uid);
            return topic_buf;
        case MQTT_MESSAGE_TYPE_OTA_PROGRESS:
            snprintf(topic_buf, sizeof(topic_buf), "%s/device/%s/ota/progress", tenant, uid);
            return topic_buf;
        case MQTT_MESSAGE_TYPE_NETWORK:
            snprintf(topic_buf, sizeof(topic_buf), "%s/device/%s/network", tenant, uid);
            return topic_buf;
        case MQTT_MESSAGE_TYPE_BLUETOOTH:
            snprintf(topic_buf, sizeof(topic_buf), "%s/device/%s/bluetooth", tenant, uid);
            return topic_buf;
        case MQTT_MESSAGE_TYPE_CONFIG:
            snprintf(topic_buf, sizeof(topic_buf), "%s/device/%s/config", tenant, uid);
            return topic_buf;
        default:
            return "";
    }
}

#if 0
static const char* get_coredump_topic() {
    const char* uid = heapwatch_get_device_uid();
    static char topic_buf[128];
    snprintf(topic_buf, sizeof(topic_buf), "device/%s/coredump", uid);
    return topic_buf;
}
#endif

static void subscribe_command_topics() {
    if (g_mqtt_client == nullptr) {
        return;
    }

    const char* uid = heapwatch_get_device_uid();
    const char* tenant = heapwatch_get_tenant_slug();
    std::snprintf(g_command_topic, sizeof(g_command_topic), "%s/command/device/%s/#", tenant, uid);
    esp_mqtt_client_subscribe(g_mqtt_client, g_command_topic, 1);
    ESP_LOGI("HeapwatchDispatcher", "Subscribed to %s", g_command_topic);
}

static void mqtt_event_handler(void*, esp_event_base_t, int32_t event_id, void* event_data) {
    if (event_id == MQTT_EVENT_CONNECTED) {
        g_mqtt_connected = true;
    } else if (event_id == MQTT_EVENT_DISCONNECTED || event_id == MQTT_EVENT_ERROR) {
        g_mqtt_connected = false;
    } else if (event_id == MQTT_EVENT_DATA && event_data != nullptr && !g_mqtt_shutting_down) {
        const auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);
        if (event->topic_len <= 0 || event->data_len <= 0) {
            return;
        }

        const std::string topic(event->topic, static_cast<size_t>(event->topic_len));
        if (topic.find("credential_rotation_due") != std::string::npos) {
            ESP_LOGI("HeapwatchDispatcher", "Credential rotation due — queueing for hw_command");
            (void)heapwatch::device_command::enqueueCredentialRotation();
        } else if (topic.find("/command/device/") != std::string::npos) {
            (void)heapwatch::device_command::enqueueMqttCommand(
                event->topic, static_cast<size_t>(event->topic_len), event->data,
                static_cast<size_t>(event->data_len), event->retain);
        }
    }
}

/* ============================================================================
   Dispatcher Task Function
   =========================================================================== */

// C1: coredump-from-flash upload moved off the blocking app_main path onto this thread,
// which already loops on connectivity state — a natural fit for "wait for network, then
// act." Runs once MQTT is connected, retries indefinitely with backoff on failure, and
// never blocks MQTT publishing (each attempt is now a single HTTP try, see
// HeapwatchConnector::uploadCoreDumpFromFlashIfPresent()).
static bool g_coredump_upload_done = false;
static TickType_t g_coredump_next_attempt_tick = 0;

static void service_coredump_upload_if_due() {
#if CONFIG_HEAPWATCH_ENABLE_COREDUMP_UPLOAD
    if (g_coredump_upload_done) {
        return;
    }
    const TickType_t now = xTaskGetTickCount();
    if (g_coredump_next_attempt_tick != 0 && now < g_coredump_next_attempt_tick) {
        return;
    }

    if (HeapwatchConnector::getInstance().uploadCoreDumpFromFlashIfPresent()) {
        g_coredump_upload_done = true;
    } else {
        g_coredump_next_attempt_tick = now + pdMS_TO_TICKS(HEAPWATCH_COREDUMP_RETRY_INTERVAL_MS);
    }
#endif  /* CONFIG_HEAPWATCH_ENABLE_COREDUMP_UPLOAD */
}

static void service_claimed_identities_post_if_due() {
    (void)heapwatch::device_enrollment::serviceClaimedIdentitiesBootPostIfDue();
}

void heapwatch_dispatch_task(void* arg) {
    (void)arg;  // Unused parameter
    
    ESP_LOGI("HeapwatchDispatcher", "MQTT dispatcher thread started");
    bool dispatcher_prev_mqtt_connected = false;

    while (true) {
        if (!heapwatch_dispatcher_running()) {
            break;
        }
        if (!heapwatch_network_ready()) {
            vTaskDelay(pdMS_TO_TICKS(HEAPWATCH_MQTT_RETRY_INTERVAL_MS));
            continue;
        }

        service_claimed_identities_post_if_due();

        // -----------------------------------------------------------------------
        // Step 1: Ensure MQTT client is connected
        // -----------------------------------------------------------------------

        if (g_mqtt_reconnect_requested && g_mqtt_client && !g_mqtt_shutting_down) {
            g_mqtt_reconnect_requested = false;
            g_mqtt_connected = false;
            g_publish_blocked_queue = nullptr;
            esp_mqtt_client_destroy(g_mqtt_client);
            g_mqtt_client = nullptr;
            g_mqtt_username.clear();
            continue;
        }
        
        if (!g_mqtt_client) {
            ESP_LOGI("HeapwatchDispatcher", "heap before mqtt start: free=%u largest=%u",
                     static_cast<unsigned>(esp_get_free_heap_size()),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)));

            // Create MQTT client
            esp_mqtt_client_config_t config = {};
            config.broker.address.uri = heapwatch_get_broker_uri();
            configure_mqtt_tls_verification(config);

            g_mqtt_username = std::string(heapwatch_get_tenant_slug()) + ":" +
                              heapwatch_get_device_uid();
            config.credentials.username = g_mqtt_username.c_str();
            config.credentials.client_id = g_mqtt_username.c_str();
            config.credentials.authentication.password = heapwatch_get_broker_password();
            config.session.keepalive = HEAPWATCH_MQTT_KEEPALIVE_SEC;

            std::snprintf(g_lwt_topic, sizeof(g_lwt_topic), "%s/device/%s/disconnected",
                          heapwatch_get_tenant_slug(), heapwatch_get_device_uid());
            // EMQX forwards the LWT to the Laravel ingress webhook, which rejects an empty body.
            static const char k_lwt_payload[] = "{}";
            config.session.last_will.topic = g_lwt_topic;
            config.session.last_will.msg = k_lwt_payload;
            config.session.last_will.msg_len = sizeof(k_lwt_payload) - 1;
            config.session.last_will.qos = 0;
            config.session.last_will.retain = 0;
            config.outbox.limit = k_mqtt_outbox_limit_bytes;

            g_mqtt_client = create_mqtt_client(config);
            if (!g_mqtt_client) {
                ESP_LOGE("HeapwatchDispatcher", "Failed to create MQTT client");
                vTaskDelay(pdMS_TO_TICKS(HEAPWATCH_MQTT_RETRY_INTERVAL_MS));
                continue;
            }

            // Start MQTT client
            esp_mqtt_client_register_event(g_mqtt_client, MQTT_EVENT_ANY, mqtt_event_handler,
                                           nullptr);
            if (esp_mqtt_client_start(g_mqtt_client) != ESP_OK) {
                ESP_LOGE("HeapwatchDispatcher", "Failed to start MQTT client");
                vTaskDelay(pdMS_TO_TICKS(HEAPWATCH_MQTT_RETRY_INTERVAL_MS));
                continue;
            }

            ESP_LOGI("HeapwatchDispatcher", "MQTT client started (awaiting broker connection)");
        }

        // -----------------------------------------------------------------------
        // Step 2: Wait for connection — esp-mqtt auto-reconnects on failure.
        // Do NOT call esp_mqtt_client_reconnect() while the initial handshake is
        // still in progress; that was aborting TLS mid-flight (EOF -0x7280).
        // -----------------------------------------------------------------------
        
        if (!g_mqtt_connected) {
            dispatcher_prev_mqtt_connected = false;
            vTaskDelay(pdMS_TO_TICKS(HEAPWATCH_MQTT_RETRY_INTERVAL_MS));
            continue;
        }

        if (!dispatcher_prev_mqtt_connected && !g_mqtt_shutting_down) {
            subscribe_command_topics();
        }
        dispatcher_prev_mqtt_connected = true;

        if (g_mqtt_shutting_down) {
            const int outbox_bytes = esp_mqtt_client_get_outbox_size(g_mqtt_client);
            if (outbox_bytes <= 0) {
                if (g_shutdown_action == HeapwatchShutdownAction::Restart) {
                    ESP_LOGI("HeapwatchDispatcher", "MQTT outbox drained — restarting");
                    esp_restart();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        service_coredump_upload_if_due();

        // -----------------------------------------------------------------------
        // Step 3: Receive message from shared queue
        // -----------------------------------------------------------------------
        // Core dump upload over MQTT is intentionally disabled during HTTP upload testing.
        // Keep this block around for quick rollback.
        #if 0
        CoreDumpPayload* core_dump_payload = nullptr;
        QueueHandle_t coredump_queue = heapwatch_get_coredump_queue();
        if (coredump_queue != nullptr &&
            xQueueReceive(coredump_queue, &core_dump_payload, pdMS_TO_TICKS(10))) {
            if (core_dump_payload == nullptr) {
                continue;
            }
            const char* coredump_topic = get_coredump_topic();
            const int result = esp_mqtt_client_publish(g_mqtt_client, coredump_topic,
                                                       core_dump_payload->payload,
                                                       core_dump_payload->payload_len, 0, 0);
            if (result < 0) {
                ESP_LOGE("HeapwatchDispatcher", "Failed to publish core dump payload: %d", result);
                if (xQueueSendToFront(coredump_queue, &core_dump_payload, 0) != pdTRUE) {
                    free(core_dump_payload);
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
            } else {
                ESP_LOGI("HeapwatchDispatcher", "Published core dump payload (%u bytes)",
                         static_cast<unsigned>(core_dump_payload->payload_len));
                const esp_err_t erase_result = esp_core_dump_image_erase();
                if (erase_result != ESP_OK) {
                    ESP_LOGW("HeapwatchDispatcher", "Failed to erase coredump after publish: %s",
                             esp_err_to_name(erase_result));
                }
                free(core_dump_payload);
            }
            continue;
        }
        #endif

        // M1 (Phase 2b): three tiered queues (tiny/small/large) serviced via a
        // FreeRTOS Queue Set — blocks efficiently on "any queue has data" instead
        // of polling each queue in turn with short timeouts.
        QueueHandle_t tiny_queue = heapwatch_get_tiny_queue();
        QueueHandle_t small_queue = heapwatch_get_small_queue();
        QueueHandle_t large_queue = heapwatch_get_queue();
        QueueSetHandle_t queue_set = heapwatch_get_queue_set();

        // Prefer a queue whose head was already peeked but MQTT outbox rejected it.
        QueueSetMemberHandle_t activated_queue = g_publish_blocked_queue;
        if (activated_queue == nullptr &&
            try_publish_priority_outbound(large_queue, tiny_queue, small_queue)) {
            continue;
        }
        if (activated_queue == nullptr) {
            activated_queue =
                (queue_set != nullptr) ? xQueueSelectFromSet(queue_set, pdMS_TO_TICKS(250)) : nullptr;
        }

        if (activated_queue == tiny_queue) {
            try_publish_queue_head<TinyMqttMessage>(tiny_queue);
        } else if (activated_queue == small_queue) {
            try_publish_queue_head<SmallMqttMessage>(small_queue);
        } else if (activated_queue == large_queue) {
            try_publish_queue_head<LargeMqttMessage>(large_queue);
        }
    }
}

/* ============================================================================
   Initialization Functions
   =========================================================================== */

bool heapwatch_dispatcher_init() {
    g_mqtt_client = nullptr;
    g_mqtt_connected = false;
    g_mqtt_shutting_down = false;
    g_shutdown_action = HeapwatchShutdownAction::Restart;
    g_publish_blocked_queue = nullptr;
    g_mqtt_username.clear();
    return true;
}

void heapwatch_dispatcher_shutdown() {
    if (g_mqtt_client) {
        esp_mqtt_client_destroy(g_mqtt_client);
        g_mqtt_client = nullptr;
    }
    
    g_mqtt_connected = false;
    g_mqtt_reconnect_requested = false;
    g_mqtt_shutting_down = false;
    g_shutdown_action = HeapwatchShutdownAction::Restart;
    g_publish_blocked_queue = nullptr;
}

void heapwatch_dispatcher_request_reconnect() {
    if (g_mqtt_shutting_down) {
        return;
    }
    g_mqtt_reconnect_requested = true;
}

bool heapwatch_dispatcher_is_shutting_down(void) {
    return g_mqtt_shutting_down;
}

void heapwatch_dispatcher_begin_shutdown(HeapwatchShutdownAction action) {
    if (g_mqtt_shutting_down) {
        return;
    }

    g_shutdown_action = action;
    g_mqtt_shutting_down = true;
    ESP_LOGI("HeapwatchDispatcher", "Shutdown armed (action=%u) — blocking new MQTT publishes",
             static_cast<unsigned>(action));
}

void heapwatch_dispatcher_begin_shutdown(void) {
    heapwatch_dispatcher_begin_shutdown(HeapwatchShutdownAction::Restart);
}

void heapwatch_dispatcher_disarm_shutdown(void) {
    g_mqtt_shutting_down = false;
    g_shutdown_action = HeapwatchShutdownAction::Restart;
    ESP_LOGI("HeapwatchDispatcher", "Shutdown disarmed");
}

int heapwatch_dispatcher_get_outbox_size(void) {
    if (g_mqtt_client == nullptr || !g_mqtt_connected) {
        return 0;
    }
    return esp_mqtt_client_get_outbox_size(g_mqtt_client);
}

bool heapwatch_dispatcher_publish_command_response(const char* suffix, const char* json_payload) {
    if (g_mqtt_client == nullptr || suffix == nullptr || json_payload == nullptr) {
        return false;
    }

    const char* tenant = heapwatch_get_tenant_slug();
    const char* uid = heapwatch_get_device_uid();
    char topic[160];
    std::snprintf(topic, sizeof(topic), "%s/response/device/%s/%s", tenant, uid, suffix);

    const int payload_len = static_cast<int>(strlen(json_payload));
    const int result = esp_mqtt_client_enqueue(g_mqtt_client, topic, json_payload, payload_len, 1, 0,
                                               false);
    if (result >= 0) {
        mark_publish_success();
    }
    return result >= 0;
}

bool heapwatch_dispatcher_is_mqtt_connected() { return g_mqtt_connected; }

int64_t heapwatch_dispatcher_get_last_publish_unix_ms() { return g_last_publish_unix_ms; }

