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

#include "HeapwatchConnector.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_core_dump.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "../heapwatch_dispatcher/HeapwatchDispatcher.h"
#include "../metrics_collector/MetricsCollector.h"
#include "../boot_cycle/BootCycleStore.h"
#include "../device_specs/DeviceSpecsCollector.h"
#include "../device_network/DeviceNetworkReporter.h"
#include "../device_radio/BluetoothReporter.h"
#include "../connector_settings/ConnectorSettings.h"
#include "../device_command/GoingDarkPipeline.h"
#include "../device_fs/DeviceFsCommandHandler.h"
#include "../device_ota/OtaService.h"
#include "../storage_stats/StorageStats.h"
#include "../stream_ingress/StreamIngressClient.h"
#include "ParameterStore.h"
#include "heapwatch_connector_timing.h"

namespace {
static constexpr char TAG[] = "HeapwatchConnector";
/** OTA progress + FW_UPDATE events: block on full queue instead of dropping. */
static constexpr TickType_t kOtaMqttQueueWait = pdMS_TO_TICKS(1000);
QueueHandle_t g_tiny_queue = nullptr;
QueueHandle_t g_small_queue = nullptr;
QueueHandle_t g_large_queue = nullptr;
QueueSetHandle_t g_queue_set = nullptr;
QueueHandle_t g_coredump_queue = nullptr;
SemaphoreHandle_t g_publish_mutex = nullptr;
bool g_initialized = false;
bool g_infrastructure_ready = false;
volatile bool g_dispatcher_running = false;
volatile bool g_collector_running = false;
volatile bool g_network_ready = false;

char g_device_uid[64] = {0};
char g_tenant_slug[64] = {0};
char g_broker_uri[128] = "mqtts://192.168.50.210:8883";
char g_broker_password[128] = {0};
char g_coredump_upload_url[256] = {0};
char g_coredump_auth_token[256] = {0};
char g_coredump_build_identifier[128] = {0};
uint32_t g_boot_cycle = 0;
uint32_t g_crash_boot_cycle = 0;

// Scratch buffers shared by the queue*() publish helpers below. LargeMqttMessage alone
// is ~2KB (HEAPWATCH_LARGE_PAYLOAD_SIZE payload). Several producers of these
// messages run on tiny stacks -- notably ESP-IDF's default system event loop task
// (CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE, 2304 bytes) which drives
// wifi_event_handler() and the esp-mqtt MQTT_EVENT_* callback -- so these must never
// be stack-allocated. Access is serialized by g_publish_mutex since queueLog(),
// queueParameter(), queueEvent(), and queueSpecs() can be called concurrently from
// different tasks (event loop task, log_forward_task, sensor/app tasks, ...).
// One scratch struct per queue tier (M1, Phase 2b) since each tier has a different
// payload size.
TinyMqttMessage g_scratch_tiny_msg;
SmallMqttMessage g_scratch_small_msg;
LargeMqttMessage g_scratch_large_msg;
char g_scratch_specs_body[HEAPWATCH_LARGE_PAYLOAD_SIZE - 96];
// Bounded to 2x the raw log line cap (worst case every char needs a "\\"/"\\uXXXX"
// escape) — matches the ~160B framing headroom baked into HEAPWATCH_SMALL_PAYLOAD_SIZE
// (see heapwatch_config.h). message is truncated to HEAPWATCH_MAX_LOG_LINE_LEN raw
// chars in queueLog() before escaping, so this bound actually holds at runtime too.
char g_scratch_raw_log_line[HEAPWATCH_MAX_LOG_LINE_LEN + 1];
char g_scratch_escaped_message[HEAPWATCH_MAX_LOG_LINE_LEN * 2 + 1];
char g_scratch_escaped_task[64];
char g_scratch_escaped_tag[64];
char g_scratch_optional[192];

#if CONFIG_HEAPWATCH_DEBUG_PAYLOAD_SIZE_HISTOGRAM
constexpr size_t kHistogramBucketCount = 7;
constexpr size_t kHistogramBucketCeilings[kHistogramBucketCount] = {64,  128,  256,        512,
                                                                     1024, 2048, SIZE_MAX};
constexpr size_t kHistogramTypeCount = 7;  // LOG, PARAMETER, EVENT, SPECS, TASKS, OTA, NETWORK
constexpr const char* kHistogramTypeNames[kHistogramTypeCount] = {
    "LOG", "PARAMETER", "EVENT", "SPECS", "TASKS", "OTA", "NETWORK"};
uint32_t g_payload_histogram[kHistogramTypeCount][kHistogramBucketCount] = {};
#endif

void destroy_message_queues() {
    if (g_queue_set != nullptr) {
        if (g_tiny_queue != nullptr) {
            xQueueRemoveFromSet(g_tiny_queue, g_queue_set);
        }
        if (g_small_queue != nullptr) {
            xQueueRemoveFromSet(g_small_queue, g_queue_set);
        }
        if (g_large_queue != nullptr) {
            xQueueRemoveFromSet(g_large_queue, g_queue_set);
        }
        vQueueDelete(g_queue_set);
        g_queue_set = nullptr;
    }
    if (g_tiny_queue != nullptr) {
        vQueueDelete(g_tiny_queue);
        g_tiny_queue = nullptr;
    }
    if (g_small_queue != nullptr) {
        vQueueDelete(g_small_queue);
        g_small_queue = nullptr;
    }
    if (g_large_queue != nullptr) {
        vQueueDelete(g_large_queue);
        g_large_queue = nullptr;
    }
}

// RAII guard around g_publish_mutex with a bounded wait: publishing helpers run on
// event-handler stacks where blocking forever would be dangerous, so a failed
// acquire just drops the message instead of risking a deadlock.
struct PublishLockGuard {
    bool locked = false;
    PublishLockGuard() {
        if (heapwatch_dispatcher_is_shutting_down()) {
            return;
        }
        if (g_publish_mutex != nullptr) {
            locked = xSemaphoreTake(g_publish_mutex, pdMS_TO_TICKS(1000)) == pdTRUE;
        }
    }
    ~PublishLockGuard() {
        if (locked) {
            xSemaphoreGive(g_publish_mutex);
        }
    }
    PublishLockGuard(const PublishLockGuard&) = delete;
    PublishLockGuard& operator=(const PublishLockGuard&) = delete;
};

void initConnectorSubsystemsOnce() {
    static bool initialized = false;
    if (initialized) {
        return;
    }
    initialized = true;

    heapwatch::storage_stats::init();
    heapwatch::connector_settings::init();
    heapwatch::going_dark::init();
    heapwatch::device_ota::init();
    heapwatch::stream_ingress::init();
    heapwatch::device_fs::init();
}
}

HeapwatchConnector& HeapwatchConnector::getInstance() {
    static HeapwatchConnector instance;
    return instance;
}

bool HeapwatchConnector::ensureInfrastructure() {
    if (g_infrastructure_ready) {
        return true;
    }

    g_tiny_queue = xQueueCreate(HEAPWATCH_TINY_QUEUE_LEN, sizeof(TinyMqttMessage));
    g_small_queue = xQueueCreate(HEAPWATCH_SMALL_QUEUE_LEN, sizeof(SmallMqttMessage));
    g_large_queue = xQueueCreate(HEAPWATCH_LARGE_QUEUE_LEN, sizeof(LargeMqttMessage));
    if (g_tiny_queue == nullptr || g_small_queue == nullptr || g_large_queue == nullptr) {
        ESP_LOGE("HeapwatchConnector",
                 "Failed to create message queues (free_heap=%u, largest_block=%u)",
                 static_cast<unsigned>(esp_get_free_heap_size()),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)));
        destroy_message_queues();
        return false;
    }

    g_queue_set = xQueueCreateSet(HEAPWATCH_TINY_QUEUE_LEN + HEAPWATCH_SMALL_QUEUE_LEN +
                                  HEAPWATCH_LARGE_QUEUE_LEN);
    if (g_queue_set == nullptr || xQueueAddToSet(g_tiny_queue, g_queue_set) != pdPASS ||
        xQueueAddToSet(g_small_queue, g_queue_set) != pdPASS ||
        xQueueAddToSet(g_large_queue, g_queue_set) != pdPASS) {
        ESP_LOGE("HeapwatchConnector", "Failed to create/populate message queue set");
        destroy_message_queues();
        return false;
    }

    g_coredump_queue =
        xQueueCreate(HEAPWATCH_COREDUMP_QUEUE_LEN, sizeof(CoreDumpPayload*));
    if (g_coredump_queue == nullptr) {
        ESP_LOGE("HeapwatchConnector", "Failed to create core dump queue");
        destroy_message_queues();
        return false;
    }

    if (g_publish_mutex == nullptr) {
        g_publish_mutex = xSemaphoreCreateMutex();
        if (g_publish_mutex == nullptr) {
            ESP_LOGE("HeapwatchConnector", "Failed to create publish mutex");
            destroy_message_queues();
            vQueueDelete(g_coredump_queue);
            g_coredump_queue = nullptr;
            return false;
        }
    }

    g_infrastructure_ready = true;
    return true;
}

bool HeapwatchConnector::isInfrastructureReady() const { return g_infrastructure_ready; }

bool HeapwatchConnector::activateWithBroker(const char* broker_url) {
    if (g_initialized) {
        return true;
    }
    if (!g_infrastructure_ready && !ensureInfrastructure()) {
        return false;
    }

    if (broker_url != nullptr) {
        snprintf(g_broker_uri, sizeof(g_broker_uri), "%s", broker_url);
    }

    initConnectorSubsystemsOnce();
    heapwatch_dispatcher_init();
    heapwatch::metrics_collector::init();

    g_initialized = true;
    // network_ready is owned by IP_EVENT handlers — do not clear here or MQTT never starts.
    startDispatcherThread();
    startCollectorThread();
    return true;
}

bool HeapwatchConnector::initialize(const char* device_uid, const char* broker_url) {
    if (g_initialized) {
        return true;
    }

    if (device_uid != nullptr) {
        setMqttDeviceUid(device_uid);
    }

    return activateWithBroker(broker_url);
}

void HeapwatchConnector::deinitialize() {
    if (!g_initialized) {
        return;
    }

    g_collector_running = false;
    g_dispatcher_running = false;
    vTaskDelay(pdMS_TO_TICKS(200));

    heapwatch_dispatcher_shutdown();

    destroy_message_queues();
    if (g_coredump_queue != nullptr) {
        CoreDumpPayload* pending_payload = nullptr;
        while (xQueueReceive(g_coredump_queue, &pending_payload, 0) == pdTRUE) {
            free(pending_payload);
        }
        vQueueDelete(g_coredump_queue);
        g_coredump_queue = nullptr;
    }

    g_initialized = false;
    g_infrastructure_ready = false;
    g_network_ready = false;
}

void HeapwatchConnector::setNetworkReady(bool ready) { g_network_ready = ready; }

void HeapwatchConnector::setMqttDeviceUid(const char* uid) {
    if (uid == nullptr) {
        g_device_uid[0] = '\0';
        return;
    }
    snprintf(g_device_uid, sizeof(g_device_uid), "%s", uid);
}

void HeapwatchConnector::setTenantSlug(const char* slug) {
    if (slug == nullptr) {
        g_tenant_slug[0] = '\0';
        return;
    }
    snprintf(g_tenant_slug, sizeof(g_tenant_slug), "%s", slug);
}

void HeapwatchConnector::setMqttPassword(const char* password) {
    if (password == nullptr) {
        g_broker_password[0] = '\0';
        return;
    }
    snprintf(g_broker_password, sizeof(g_broker_password), "%s", password);
}

void HeapwatchConnector::setCoreDumpUploadUrl(const char* url) {
    if (url == nullptr) {
        g_coredump_upload_url[0] = '\0';
        return;
    }
    snprintf(g_coredump_upload_url, sizeof(g_coredump_upload_url), "%s", url);
}

void HeapwatchConnector::setRestApiAuthToken(const char* token) {
    if (token == nullptr) {
        g_coredump_auth_token[0] = '\0';
        return;
    }
    snprintf(g_coredump_auth_token, sizeof(g_coredump_auth_token), "%s", token);
}

void HeapwatchConnector::setCoreDumpBuildIdentifier(const char* build_identifier) {
    if (build_identifier == nullptr) {
        g_coredump_build_identifier[0] = '\0';
        return;
    }
    snprintf(g_coredump_build_identifier, sizeof(g_coredump_build_identifier), "%s",
             build_identifier);
}

void HeapwatchConnector::setBootCycleContext(uint32_t boot_cycle, uint32_t crash_boot_cycle) {
    g_boot_cycle = boot_cycle;
    g_crash_boot_cycle = crash_boot_cycle;
}

static const char* severity_to_string(HeapwatchConnectorSeverity severity) {
    switch (severity) {
        case HEAPWATCH_SEVERITY_DEBUG: return "debug";
        case HEAPWATCH_SEVERITY_INFO: return "info";
        case HEAPWATCH_SEVERITY_WARNING: return "warning";
        case HEAPWATCH_SEVERITY_ERROR: return "error";
        case HEAPWATCH_SEVERITY_CRITICAL: return "critical";
        default: return "info";
    }
}

static void json_escape_string(const char* input, char* output, size_t output_len) {
    if (input == nullptr || output == nullptr || output_len == 0) {
        return;
    }

    size_t out_idx = 0;
    for (size_t i = 0; input[i] != '\0' && out_idx + 1 < output_len; ++i) {
        const unsigned char c = static_cast<unsigned char>(input[i]);
        if ((c == '\\' || c == '"') && out_idx + 2 < output_len) {
            output[out_idx++] = '\\';
            output[out_idx++] = static_cast<char>(c);
            continue;
        }
        if (c == '\n' || c == '\r' || c == '\t') {
            output[out_idx++] = ' ';
            continue;
        }
        if (c < 0x20 && out_idx + 6 < output_len) {
            snprintf(output + out_idx, output_len - out_idx, "\\u%04x", c);
            out_idx += 6;
            continue;
        }
        output[out_idx++] = static_cast<char>(c);
    }
    output[out_idx] = '\0';
}

bool HeapwatchConnector::formatLogJsonLine(HeapwatchConnectorSeverity severity,
                                           const char* message,
                                           const HeapwatchConnectorLogContext* context,
                                           char* out, size_t out_len) {
#if !CONFIG_HEAPWATCH_ENABLE_LOG_COLLECTION
    (void)severity;
    (void)message;
    (void)context;
    (void)out;
    (void)out_len;
    return false;
#else
    if (out == nullptr || out_len == 0 || message == nullptr) {
        return false;
    }

    uint64_t tsms = 0;
    uint64_t utms = 0;
    heapwatch_get_ingress_timing_ms(&tsms, &utms);

    snprintf(g_scratch_raw_log_line, sizeof(g_scratch_raw_log_line), "%s", message);
    json_escape_string(g_scratch_raw_log_line, g_scratch_escaped_message,
                       sizeof(g_scratch_escaped_message));

    g_scratch_escaped_task[0] = '\0';
    g_scratch_escaped_tag[0] = '\0';
    const bool has_task =
        context != nullptr && context->task != nullptr && context->task[0] != '\0';
    const bool has_tag = context != nullptr && context->tag != nullptr && context->tag[0] != '\0';
    const bool has_core = context != nullptr && context->core >= 0;

    if (has_task) {
        json_escape_string(context->task, g_scratch_escaped_task, sizeof(g_scratch_escaped_task));
    }
    if (has_tag) {
        json_escape_string(context->tag, g_scratch_escaped_tag, sizeof(g_scratch_escaped_tag));
    }

#ifdef HEAPWATCH_USE_JSON_FORMAT
    g_scratch_optional[0] = '\0';
    size_t optional_json_len = 0;
    if (has_task) {
        optional_json_len += snprintf(g_scratch_optional + optional_json_len,
                                      sizeof(g_scratch_optional) - optional_json_len, ",\"task\":\"%s\"",
                                      g_scratch_escaped_task);
    }
    if (has_tag) {
        optional_json_len += snprintf(g_scratch_optional + optional_json_len,
                                      sizeof(g_scratch_optional) - optional_json_len, ",\"tag\":\"%s\"",
                                      g_scratch_escaped_tag);
    }
    if (has_core) {
        optional_json_len += snprintf(g_scratch_optional + optional_json_len,
                                      sizeof(g_scratch_optional) - optional_json_len, ",\"core\":%d",
                                      context->core);
    }

    const int written = snprintf(
        out, out_len,
        "{\"tsms\":%llu,\"utms\":%llu%s,\"severity\":\"%s\",\"message\":\"%s\"}",
        static_cast<unsigned long long>(tsms), static_cast<unsigned long long>(utms),
        g_scratch_optional, severity_to_string(severity), g_scratch_escaped_message);

    return written > 0 && static_cast<size_t>(written) < out_len;
#else
    (void)out;
    (void)out_len;
    return false;
#endif
#endif  /* CONFIG_HEAPWATCH_ENABLE_LOG_COLLECTION */
}

bool HeapwatchConnector::queueLog(HeapwatchConnectorSeverity severity, const char* message,
                               const HeapwatchConnectorLogContext* context) {
#if !CONFIG_HEAPWATCH_ENABLE_LOG_COLLECTION
    (void)severity;
    (void)message;
    (void)context;
    return false;
#else
    if (!g_infrastructure_ready || g_small_queue == nullptr || message == nullptr) {
        return false;
    }

    PublishLockGuard lock;
    if (!lock.locked) {
        return false;
    }

    g_scratch_small_msg.type = MQTT_MESSAGE_TYPE_LOG;
    if (!formatLogJsonLine(severity, message, context, g_scratch_small_msg.payload,
                           sizeof(g_scratch_small_msg.payload))) {
        return false;
    }

    heapwatch_debug_record_payload_size(MQTT_MESSAGE_TYPE_LOG, strlen(g_scratch_small_msg.payload));
    return xQueueSend(g_small_queue, &g_scratch_small_msg, 0) == pdTRUE;
#endif  /* CONFIG_HEAPWATCH_ENABLE_LOG_COLLECTION */
}

bool HeapwatchConnector::queueCoreDump(const char* base64_payload, size_t payload_len) {
#if !CONFIG_HEAPWATCH_ENABLE_COREDUMP_UPLOAD
    (void)base64_payload;
    (void)payload_len;
    return false;
#else
    if (!g_infrastructure_ready || g_coredump_queue == nullptr || base64_payload == nullptr) {
        return false;
    }
    if (payload_len == 0 || payload_len >= HEAPWATCH_COREDUMP_MAX_PAYLOAD_SIZE) {
        return false;
    }

    CoreDumpPayload* payload = static_cast<CoreDumpPayload*>(calloc(1, sizeof(CoreDumpPayload)));
    if (payload == nullptr) {
        return false;
    }

    payload->payload_len = payload_len;
    memcpy(payload->payload, base64_payload, payload_len);
    payload->payload[payload_len] = '\0';

    if (xQueueSend(g_coredump_queue, &payload, 0) != pdTRUE) {
        free(payload);
        return false;
    }
    return true;
#endif  /* CONFIG_HEAPWATCH_ENABLE_COREDUMP_UPLOAD */
}

bool HeapwatchConnector::uploadCoreDumpFromFlashIfPresent() {
#if !CONFIG_HEAPWATCH_ENABLE_COREDUMP_UPLOAD || !CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    return false;
#else
    if (g_coredump_upload_url[0] == '\0') {
        ESP_LOGW(TAG, "Skipping coredump upload: upload URL is not configured");
        return false;
    }
    if (g_coredump_auth_token[0] == '\0') {
        ESP_LOGW(TAG, "Skipping coredump upload: auth token is not configured");
        return false;
    }
    if (g_coredump_build_identifier[0] == '\0') {
        ESP_LOGW(TAG, "Skipping coredump upload: build identifier is not configured");
        return false;
    }

    if (esp_core_dump_image_check() != ESP_OK) {
        ESP_LOGI(TAG, "No valid core dump found in flash");
        return true;
    }

    size_t dump_addr = 0;
    size_t dump_size = 0;
    if (esp_core_dump_image_get(&dump_addr, &dump_size) != ESP_OK || dump_size == 0) {
        ESP_LOGW(TAG, "Core dump exists but metadata could not be read");
        return false;
    }

    const size_t free_heap = esp_get_free_heap_size();
    const size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    // Streaming upload only needs a tiny chunk buffer + TLS; refuse if TLS can't start.
    constexpr size_t kMinHeapForStreamingUpload = 12 * 1024;
    if (largest_block < 4096 || free_heap < kMinHeapForStreamingUpload) {
        ESP_LOGW(TAG,
                 "Skipping coredump upload at boot: dump=%u bytes, free_heap=%u, largest_block=%u "
                 "(not enough headroom for TLS streaming)",
                 static_cast<unsigned>(dump_size), static_cast<unsigned>(free_heap),
                 static_cast<unsigned>(largest_block));
        return false;
    }

    const esp_partition_t* coredump_partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
                                 nullptr);
    if (coredump_partition == nullptr) {
        ESP_LOGE(TAG, "Core dump partition not found");
        return false;
    }
    if (dump_addr < static_cast<size_t>(coredump_partition->address)) {
        ESP_LOGE(TAG, "Core dump address is outside coredump partition");
        return false;
    }

    const size_t partition_offset = dump_addr - static_cast<size_t>(coredump_partition->address);
    if (partition_offset + dump_size > coredump_partition->size) {
        ESP_LOGE(TAG, "Core dump range exceeds coredump partition");
        return false;
    }

    esp_http_client_config_t http_config = {};
    http_config.url = g_coredump_upload_url;
    http_config.timeout_ms = 30000;
    http_config.buffer_size = 512;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t http_client = esp_http_client_init(&http_config);
    if (http_client == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client for core dump upload");
        return false;
    }

    char timestamp_header[32] = {};
    snprintf(timestamp_header, sizeof(timestamp_header), "%lld",
             static_cast<long long>(time(nullptr)));

    esp_http_client_set_method(http_client, HTTP_METHOD_POST);
    esp_http_client_set_header(http_client, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(http_client, "Accept", "application/json");
    esp_http_client_set_header(http_client, "X-Build-Identifier", g_coredump_build_identifier);
    esp_http_client_set_header(http_client, "X-Device-Timestamp", timestamp_header);
    esp_http_client_set_header(http_client, "Authorization", g_coredump_auth_token);

    char boot_cycle_header[16] = {};
    snprintf(boot_cycle_header, sizeof(boot_cycle_header), "%lu",
             static_cast<unsigned long>(g_boot_cycle));
    esp_http_client_set_header(http_client, "X-Boot-Cycle", boot_cycle_header);

    if (g_crash_boot_cycle > 0) {
        char crash_boot_cycle_header[16] = {};
        snprintf(crash_boot_cycle_header, sizeof(crash_boot_cycle_header), "%lu",
                 static_cast<unsigned long>(g_crash_boot_cycle));
        esp_http_client_set_header(http_client, "X-Crash-Boot-Cycle", crash_boot_cycle_header);
    }

    // Stream from flash in small chunks — never malloc(dump_size).
    constexpr size_t kChunkBytes = 256;
    uint8_t chunk[kChunkBytes];

    esp_err_t http_result = esp_http_client_open(http_client, static_cast<int>(dump_size));
    if (http_result != ESP_OK) {
        ESP_LOGW(TAG, "Core dump HTTP open failed: %s. Caller will retry.",
                 esp_err_to_name(http_result));
        esp_http_client_cleanup(http_client);
        return false;
    }

    size_t sent = 0;
    while (sent < dump_size) {
        const size_t to_read = (dump_size - sent) > kChunkBytes ? kChunkBytes : (dump_size - sent);
        const esp_err_t read_result =
            esp_partition_read(coredump_partition, partition_offset + sent, chunk, to_read);
        if (read_result != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read core dump chunk at +%u: %s", static_cast<unsigned>(sent),
                     esp_err_to_name(read_result));
            esp_http_client_close(http_client);
            esp_http_client_cleanup(http_client);
            return false;
        }

        const int written = esp_http_client_write(http_client, reinterpret_cast<const char*>(chunk),
                                                    static_cast<int>(to_read));
        if (written < 0 || static_cast<size_t>(written) != to_read) {
            ESP_LOGW(TAG, "Core dump HTTP write failed at +%u (wrote %d of %u). Caller will retry.",
                     static_cast<unsigned>(sent), written, static_cast<unsigned>(to_read));
            esp_http_client_close(http_client);
            esp_http_client_cleanup(http_client);
            return false;
        }
        sent += to_read;
    }

    const int content_length = esp_http_client_fetch_headers(http_client);
    const int status_code = esp_http_client_get_status_code(http_client);
    char response_preview[128] = {};
    const int response_size =
        esp_http_client_read(http_client, response_preview, sizeof(response_preview) - 1);
    if (response_size > 0) {
        response_preview[response_size] = '\0';
        ESP_LOGI(TAG, "Core dump upload success: status=%d len=%d body='%s'", status_code,
                 content_length, response_preview);
    } else {
        ESP_LOGI(TAG, "Core dump upload success: status=%d len=%d", status_code, content_length);
    }

    esp_http_client_close(http_client);
    esp_http_client_cleanup(http_client);

    if (status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "Core dump upload got non-2xx status %d. Caller will retry.", status_code);
        return false;
    }

    const esp_err_t erase_result = esp_core_dump_image_erase();
    if (erase_result != ESP_OK) {
        ESP_LOGE(TAG, "Core dump upload succeeded, but failed to erase coredump from flash: %s",
                 esp_err_to_name(erase_result));
        return false;
    }

    if (esp_core_dump_image_check() == ESP_OK) {
        ESP_LOGE(TAG, "Core dump upload succeeded, but coredump is still present after erase");
        return false;
    }

    if (!heapwatch::boot_cycle::clearLastCrash()) {
        ESP_LOGW(TAG, "Core dump upload succeeded, but failed to clear last_crash_boot_cycle");
    } else {
        g_crash_boot_cycle = 0;
    }

    ESP_LOGI(TAG, "Core dump upload completed (%u bytes raw, streamed) and coredump was erased from flash.",
             static_cast<unsigned>(dump_size));
    return true;
#endif  /* CONFIG_HEAPWATCH_ENABLE_COREDUMP_UPLOAD && CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH */
}

void HeapwatchConnector::queueParameter(const char* name, int64_t value) {
    if (!g_infrastructure_ready || g_tiny_queue == nullptr || name == nullptr) {
        return;
    }
    PublishLockGuard lock;
    if (!lock.locked) {
        return;
    }
    g_scratch_tiny_msg.type = MQTT_MESSAGE_TYPE_PARAMETER;
    uint64_t tsms = 0;
    uint64_t utms = 0;
    heapwatch_get_ingress_timing_ms(&tsms, &utms);

#ifdef HEAPWATCH_USE_JSON_FORMAT
    snprintf(g_scratch_tiny_msg.payload, sizeof(g_scratch_tiny_msg.payload),
             "{\"tsms\":%llu,\"utms\":%llu,\"%s\":%lld}",
             static_cast<unsigned long long>(tsms), static_cast<unsigned long long>(utms), name,
             static_cast<long long>(value));
#else
    snprintf(g_scratch_tiny_msg.payload, sizeof(g_scratch_tiny_msg.payload),
             "tsms=%llu|utms=%llu|%s=%lld",
             static_cast<unsigned long long>(tsms), static_cast<unsigned long long>(utms), name,
             static_cast<long long>(value));
#endif

    heapwatch_debug_record_payload_size(MQTT_MESSAGE_TYPE_PARAMETER, strlen(g_scratch_tiny_msg.payload));
    xQueueSend(g_tiny_queue, &g_scratch_tiny_msg, 0);
}

void HeapwatchConnector::queueParameter(const char* name, double value) {
    if (!g_infrastructure_ready || g_tiny_queue == nullptr || name == nullptr) {
        return;
    }
    PublishLockGuard lock;
    if (!lock.locked) {
        return;
    }
    g_scratch_tiny_msg.type = MQTT_MESSAGE_TYPE_PARAMETER;
    uint64_t tsms = 0;
    uint64_t utms = 0;
    heapwatch_get_ingress_timing_ms(&tsms, &utms);

#ifdef HEAPWATCH_USE_JSON_FORMAT
    snprintf(g_scratch_tiny_msg.payload, sizeof(g_scratch_tiny_msg.payload),
             "{\"tsms\":%llu,\"utms\":%llu,\"%s\":%.6f}",
             static_cast<unsigned long long>(tsms), static_cast<unsigned long long>(utms), name, value);
#else
    snprintf(g_scratch_tiny_msg.payload, sizeof(g_scratch_tiny_msg.payload),
             "tsms=%llu|utms=%llu|%s=%.6f",
             static_cast<unsigned long long>(tsms), static_cast<unsigned long long>(utms), name, value);
#endif

    heapwatch_debug_record_payload_size(MQTT_MESSAGE_TYPE_PARAMETER, strlen(g_scratch_tiny_msg.payload));
    xQueueSend(g_tiny_queue, &g_scratch_tiny_msg, 0);
}

void HeapwatchConnector::queueEvent(int eventTypeId, int32_t boot_cycle) {
#if !CONFIG_HEAPWATCH_ENABLE_STANDARD_EVENTS
    (void)eventTypeId;
    (void)boot_cycle;
    return;
#else
    if (!g_infrastructure_ready || g_tiny_queue == nullptr) {
        return;
    }
    PublishLockGuard lock;
    if (!lock.locked) {
        return;
    }
    g_scratch_tiny_msg.type = MQTT_MESSAGE_TYPE_EVENT;
    uint64_t tsms = 0;
    uint64_t utms = 0;
    heapwatch_get_ingress_timing_ms(&tsms, &utms);

#ifdef HEAPWATCH_USE_JSON_FORMAT
    if (boot_cycle >= 0) {
        snprintf(g_scratch_tiny_msg.payload, sizeof(g_scratch_tiny_msg.payload),
                 "{\"tsms\":%llu,\"utms\":%llu,\"event_type_id\":%d,\"boot_cycle\":%ld}",
                 static_cast<unsigned long long>(tsms), static_cast<unsigned long long>(utms),
                 eventTypeId, static_cast<long>(boot_cycle));
    } else {
        snprintf(g_scratch_tiny_msg.payload, sizeof(g_scratch_tiny_msg.payload),
                 "{\"tsms\":%llu,\"utms\":%llu,\"event_type_id\":%d}",
                 static_cast<unsigned long long>(tsms), static_cast<unsigned long long>(utms),
                 eventTypeId);
    }
#else
    if (boot_cycle >= 0) {
        snprintf(g_scratch_tiny_msg.payload, sizeof(g_scratch_tiny_msg.payload),
                 "tsms=%llu|utms=%llu|event_type_id=%d|boot_cycle=%ld",
                 static_cast<unsigned long long>(tsms), static_cast<unsigned long long>(utms),
                 eventTypeId, static_cast<long>(boot_cycle));
    } else {
        snprintf(g_scratch_tiny_msg.payload, sizeof(g_scratch_tiny_msg.payload),
                 "tsms=%llu|utms=%llu|event_type_id=%d",
                 static_cast<unsigned long long>(tsms), static_cast<unsigned long long>(utms),
                 eventTypeId);
    }
#endif

    heapwatch_debug_record_payload_size(MQTT_MESSAGE_TYPE_EVENT, strlen(g_scratch_tiny_msg.payload));
    const bool fw_ota_event =
        eventTypeId >= HEAPWATCH_EVENT_FW_UPDATE_STARTED &&
        eventTypeId <= HEAPWATCH_EVENT_FW_UPDATE_FAILED;
    const TickType_t send_wait = fw_ota_event ? kOtaMqttQueueWait : 0;
    xQueueSend(g_tiny_queue, &g_scratch_tiny_msg, send_wait);
#endif  /* CONFIG_HEAPWATCH_ENABLE_STANDARD_EVENTS */
}

void HeapwatchConnector::queueOtaProgress(int64_t ota_session_id, const char* phase, int64_t bytes,
                                          int64_t total) {
    if (!g_infrastructure_ready || g_small_queue == nullptr || phase == nullptr || phase[0] == '\0') {
        return;
    }

    PublishLockGuard lock;
    if (!lock.locked) {
        return;
    }

    g_scratch_small_msg.type = MQTT_MESSAGE_TYPE_OTA_PROGRESS;
    uint64_t tsms = 0;
    uint64_t utms = 0;
    heapwatch_get_ingress_timing_ms(&tsms, &utms);

#ifdef HEAPWATCH_USE_JSON_FORMAT
    snprintf(g_scratch_small_msg.payload, sizeof(g_scratch_small_msg.payload),
             "{\"tsms\":%llu,\"utms\":%llu,\"ota_session_id\":%lld,\"phase\":\"%s\",\"bytes\":%lld,"
             "\"total\":%lld}",
             static_cast<unsigned long long>(tsms), static_cast<unsigned long long>(utms),
             static_cast<long long>(ota_session_id), phase, static_cast<long long>(bytes),
             static_cast<long long>(total));
#else
    snprintf(g_scratch_small_msg.payload, sizeof(g_scratch_small_msg.payload),
             "tsms=%llu|utms=%llu|ota_session_id=%lld|phase=%s|bytes=%lld|total=%lld",
             static_cast<unsigned long long>(tsms), static_cast<unsigned long long>(utms),
             static_cast<long long>(ota_session_id), phase, static_cast<long long>(bytes),
             static_cast<long long>(total));
#endif

    heapwatch_debug_record_payload_size(MQTT_MESSAGE_TYPE_OTA_PROGRESS,
                                        strlen(g_scratch_small_msg.payload));
    xQueueSend(g_small_queue, &g_scratch_small_msg, kOtaMqttQueueWait);
}

void HeapwatchConnector::queueSpecs() {
#if !CONFIG_HEAPWATCH_ENABLE_DEVICE_SPECS
    return;
#else
    if (!g_infrastructure_ready || g_large_queue == nullptr) {
        return;
    }

    PublishLockGuard lock;
    if (!lock.locked) {
        return;
    }

    g_scratch_large_msg.type = MQTT_MESSAGE_TYPE_SPECS;

    uint64_t tsms = 0;
    uint64_t utms = 0;
    heapwatch_get_ingress_timing_ms(&tsms, &utms);

    if (!heapwatch::device_specs::buildPayload(g_scratch_specs_body, sizeof(g_scratch_specs_body))) {
        ESP_LOGW(TAG, "Failed to build device specs payload");
        return;
    }

#ifdef HEAPWATCH_USE_JSON_FORMAT
    snprintf(g_scratch_large_msg.payload, sizeof(g_scratch_large_msg.payload),
             "{\"tsms\":%llu,\"utms\":%llu,%s", static_cast<unsigned long long>(tsms),
             static_cast<unsigned long long>(utms), g_scratch_specs_body + 1);
#else
    snprintf(g_scratch_large_msg.payload, sizeof(g_scratch_large_msg.payload),
             "tsms=%llu|utms=%llu|%s", static_cast<unsigned long long>(tsms),
             static_cast<unsigned long long>(utms), g_scratch_specs_body + 1);
#endif

    heapwatch_debug_record_payload_size(MQTT_MESSAGE_TYPE_SPECS, strlen(g_scratch_large_msg.payload));
    xQueueSend(g_large_queue, &g_scratch_large_msg, 0);
#endif  /* CONFIG_HEAPWATCH_ENABLE_DEVICE_SPECS */
}

void HeapwatchConnector::queueNetwork() {
    if (!g_infrastructure_ready || g_large_queue == nullptr) {
        return;
    }

    PublishLockGuard lock;
    if (!lock.locked) {
        return;
    }

    g_scratch_large_msg.type = MQTT_MESSAGE_TYPE_NETWORK;

    uint64_t tsms = 0;
    uint64_t utms = 0;
    heapwatch_get_ingress_timing_ms(&tsms, &utms);

    if (!heapwatch::device_network::buildPayload(g_scratch_specs_body, sizeof(g_scratch_specs_body))) {
        ESP_LOGW(TAG, "Failed to build device network payload");
        return;
    }

#ifdef HEAPWATCH_USE_JSON_FORMAT
    snprintf(g_scratch_large_msg.payload, sizeof(g_scratch_large_msg.payload),
             "{\"tsms\":%llu,\"utms\":%llu,%s", static_cast<unsigned long long>(tsms),
             static_cast<unsigned long long>(utms), g_scratch_specs_body + 1);
#else
    snprintf(g_scratch_large_msg.payload, sizeof(g_scratch_large_msg.payload),
             "tsms=%llu|utms=%llu|%s", static_cast<unsigned long long>(tsms),
             static_cast<unsigned long long>(utms), g_scratch_specs_body + 1);
#endif

    heapwatch_debug_record_payload_size(MQTT_MESSAGE_TYPE_NETWORK,
                                        strlen(g_scratch_large_msg.payload));
    xQueueSend(g_large_queue, &g_scratch_large_msg, 0);
}

void HeapwatchConnector::queueBluetoothSnapshot() {
    if (!g_infrastructure_ready || g_large_queue == nullptr) {
        return;
    }

    PublishLockGuard lock;
    if (!lock.locked) {
        return;
    }

    g_scratch_large_msg.type = MQTT_MESSAGE_TYPE_BLUETOOTH;

    uint64_t tsms = 0;
    uint64_t utms = 0;
    heapwatch_get_ingress_timing_ms(&tsms, &utms);

    if (!heapwatch::device_radio::buildPayload(g_scratch_specs_body, sizeof(g_scratch_specs_body))) {
        ESP_LOGW(TAG, "Failed to build device bluetooth payload");
        return;
    }

#ifdef HEAPWATCH_USE_JSON_FORMAT
    snprintf(g_scratch_large_msg.payload, sizeof(g_scratch_large_msg.payload),
             "{\"tsms\":%llu,\"utms\":%llu,%s", static_cast<unsigned long long>(tsms),
             static_cast<unsigned long long>(utms), g_scratch_specs_body + 1);
#else
    snprintf(g_scratch_large_msg.payload, sizeof(g_scratch_large_msg.payload),
             "tsms=%llu|utms=%llu|%s", static_cast<unsigned long long>(tsms),
             static_cast<unsigned long long>(utms), g_scratch_specs_body + 1);
#endif

    heapwatch_debug_record_payload_size(MQTT_MESSAGE_TYPE_BLUETOOTH,
                                        strlen(g_scratch_large_msg.payload));
    xQueueSend(g_large_queue, &g_scratch_large_msg, 0);
}

void HeapwatchConnector::updateParameterInt(const char* slug, int64_t value) {
    heapwatch::parameter_store::updateInt(slug, value);
}

void HeapwatchConnector::updateParameterDouble(const char* slug, double value) {
    heapwatch::parameter_store::updateDouble(slug, value);
}

void HeapwatchConnector::updateParameterIntRanged(const char* slug, int64_t value,
                                                   int64_t min_value, int64_t max_value) {
    heapwatch::parameter_store::updateIntRanged(slug, value, min_value, max_value);
}

void HeapwatchConnector::updateParameterDoubleRanged(const char* slug, double value,
                                                      double min_value, double max_value) {
    heapwatch::parameter_store::updateDoubleRanged(slug, value, min_value, max_value);
}

void HeapwatchConnector::setParameterIntMax(const char* slug, int64_t max_value) {
    heapwatch::parameter_store::setIntMax(slug, max_value);
}

void HeapwatchConnector::setParameterIntMin(const char* slug, int64_t min_value) {
    heapwatch::parameter_store::setIntMin(slug, min_value);
}

void HeapwatchConnector::setParameterDoubleMax(const char* slug, double max_value) {
    heapwatch::parameter_store::setDoubleMax(slug, max_value);
}

void HeapwatchConnector::setParameterDoubleMin(const char* slug, double min_value) {
    heapwatch::parameter_store::setDoubleMin(slug, min_value);
}

void HeapwatchConnector::flushParameters() {
    if (!g_infrastructure_ready || g_large_queue == nullptr) {
        return;
    }

    PublishLockGuard lock;
    if (!lock.locked) {
        return;
    }

    if (!heapwatch::parameter_store::buildFlushPayload(g_scratch_large_msg.payload,
                                                        sizeof(g_scratch_large_msg.payload))) {
        return;  // Nothing has ever been set via updateParameter*() this boot.
    }

    g_scratch_large_msg.type = MQTT_MESSAGE_TYPE_PARAMETER;
    heapwatch_debug_record_payload_size(MQTT_MESSAGE_TYPE_PARAMETER,
                                         strlen(g_scratch_large_msg.payload));
    xQueueSend(g_large_queue, &g_scratch_large_msg, 0);
}

HeapwatchConnectorStatus HeapwatchConnector::getStatus() const {
    HeapwatchConnectorStatus status = {};
    status.initialized = g_initialized;
    status.network_ready = g_network_ready;
    status.mqtt_connected = heapwatch_dispatcher_is_mqtt_connected();
    status.dispatcher_running = g_dispatcher_running;
    status.collector_running = g_collector_running;
    status.last_successful_publish_unix_ms = heapwatch_dispatcher_get_last_publish_unix_ms();
    status.tiny_queue_depth =
        g_tiny_queue != nullptr ? static_cast<uint32_t>(uxQueueMessagesWaiting(g_tiny_queue)) : 0;
    status.small_queue_depth =
        g_small_queue != nullptr ? static_cast<uint32_t>(uxQueueMessagesWaiting(g_small_queue)) : 0;
    status.large_queue_depth =
        g_large_queue != nullptr ? static_cast<uint32_t>(uxQueueMessagesWaiting(g_large_queue)) : 0;
    status.coredump_queue_depth =
        g_coredump_queue != nullptr ? static_cast<uint32_t>(uxQueueMessagesWaiting(g_coredump_queue))
                                    : 0;
    return status;
}

void HeapwatchConnector::startDispatcherThread() {
    g_dispatcher_running = true;
    xTaskCreate(heapwatch_dispatch_task, "hw_dispatcher", HEAPWATCH_DISPATCHER_TASK_STACK_BYTES,
                nullptr, tskIDLE_PRIORITY + 3, nullptr);
}

void HeapwatchConnector::startCollectorThread() {
    g_collector_running = true;
    xTaskCreate(heapwatch::metrics_collector::collectTask, "hw_metrics",
                HEAPWATCH_METRICS_TASK_STACK_BYTES, nullptr, tskIDLE_PRIORITY + 2, nullptr);
}

QueueHandle_t heapwatch_get_tiny_queue() { return g_tiny_queue; }
QueueHandle_t heapwatch_get_small_queue() { return g_small_queue; }
QueueHandle_t heapwatch_get_queue() { return g_large_queue; }
QueueSetHandle_t heapwatch_get_queue_set() { return g_queue_set; }
QueueHandle_t heapwatch_get_coredump_queue() { return g_coredump_queue; }
const char* heapwatch_get_device_uid() { return g_device_uid; }
const char* heapwatch_get_tenant_slug() { return g_tenant_slug; }
const char* heapwatch_get_broker_uri() { return g_broker_uri; }
const char* heapwatch_get_broker_password() { return g_broker_password; }
const char* heapwatch_get_rest_api_auth_token() { return g_coredump_auth_token; }
uint32_t heapwatch_get_boot_cycle() { return g_boot_cycle; }
bool heapwatch_dispatcher_running() { return g_dispatcher_running; }
bool heapwatch_collector_running() { return g_collector_running; }
bool heapwatch_network_ready() { return g_network_ready; }

void heapwatch_debug_record_payload_size(MqttMessageType type, size_t len) {
#if CONFIG_HEAPWATCH_DEBUG_PAYLOAD_SIZE_HISTOGRAM
    if (static_cast<size_t>(type) >= kHistogramTypeCount) {
        return;
    }
    for (size_t i = 0; i < kHistogramBucketCount; ++i) {
        if (len <= kHistogramBucketCeilings[i]) {
            g_payload_histogram[type][i]++;
            return;
        }
    }
#else
    (void)type;
    (void)len;
#endif
}

void heapwatch_debug_dump_payload_histogram() {
#if CONFIG_HEAPWATCH_DEBUG_PAYLOAD_SIZE_HISTOGRAM
    for (size_t t = 0; t < kHistogramTypeCount; ++t) {
        ESP_LOGI(TAG,
                 "payload_histogram[%s]: <=64:%u <=128:%u <=256:%u <=512:%u <=1024:%u <=2048:%u "
                 ">2048:%u",
                 kHistogramTypeNames[t], static_cast<unsigned>(g_payload_histogram[t][0]),
                 static_cast<unsigned>(g_payload_histogram[t][1]),
                 static_cast<unsigned>(g_payload_histogram[t][2]),
                 static_cast<unsigned>(g_payload_histogram[t][3]),
                 static_cast<unsigned>(g_payload_histogram[t][4]),
                 static_cast<unsigned>(g_payload_histogram[t][5]),
                 static_cast<unsigned>(g_payload_histogram[t][6]));
    }
#endif
}
