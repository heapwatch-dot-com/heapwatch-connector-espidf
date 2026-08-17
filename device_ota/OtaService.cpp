#include "OtaService.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "sdkconfig.h"

#include "../device_command/DeviceCommandHandler.h"
#include "../heapwatch_config.h"
#include "../heapwatch_connector/ConnectorBootstrap.h"
#include "../heapwatch_connector/HeapwatchConnector.h"

namespace heapwatch::device_ota {

namespace {

constexpr size_t kUrlMax = 512;
constexpr size_t kShaHexMax = 65;
#if CONFIG_HEAPWATCH_ENABLE_OTA
constexpr size_t kDownloadChunk = CONFIG_HEAPWATCH_OTA_DOWNLOAD_CHUNK_BYTES;
#else
constexpr size_t kDownloadChunk = 256;
#endif
// BSS — single hw_ota task; keeps chunk RAM off the 12 KiB task stack.
uint8_t g_ota_io_buf[kDownloadChunk] = {};
constexpr size_t kMaxDataPartitions = 4;
constexpr size_t kPartitionNameMax = 32;
// Yield every download chunk so MQTT can deliver ota/abort while HTTPS streams.
constexpr size_t kMqttYieldEveryBytes = kDownloadChunk;
// MQTT ota/progress publishes — decoupled from HTTPS read chunk size.
constexpr size_t kProgressEveryBytes = 128 * 1024;
constexpr int kHttpTimeoutMs = 15000;

struct DataPartitionJob {
    char name[kPartitionNameMax] = {};
    char download_url[kUrlMax] = {};
    char sha256[kShaHexMax] = {};
    int64_t size = 0;
};

struct OtaJob {
    int64_t ota_session_id = 0;
    char code_download_url[kUrlMax] = {};
    char code_checksum[kShaHexMax] = {};
    int64_t code_size = 0;
    bool include_data = false;
    size_t data_count = 0;
    DataPartitionJob data[kMaxDataPartitions] = {};
};

enum class Phase : uint8_t {
    Idle = 0,
    Running,
    BootPointerSwitched,
};

SemaphoreHandle_t g_mutex = nullptr;
volatile bool g_abort_requested = false;
volatile Phase g_phase = Phase::Idle;
TaskHandle_t g_task = nullptr;
OtaJob g_job = {};

void log_phase(int64_t session_id, const char* phase, const char* detail = nullptr) {
    if (detail != nullptr && detail[0] != '\0') {
        ESP_LOGI(kLogTag, "OTA session=%lld phase=%s %s", static_cast<long long>(session_id), phase,
                 detail);
    } else {
        ESP_LOGI(kLogTag, "OTA session=%lld phase=%s", static_cast<long long>(session_id), phase);
    }
}

void emit_event(int event_type_id) {
    HeapwatchConnector::getInstance().queueEvent(event_type_id);
}

void emit_progress(int64_t session_id, const char* phase, int64_t bytes, int64_t total) {
    HeapwatchConnector::getInstance().queueOtaProgress(session_id, phase, bytes, total);
}

bool hex_nibble(char c, uint8_t* out) {
    if (c >= '0' && c <= '9') {
        *out = static_cast<uint8_t>(c - '0');
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        *out = static_cast<uint8_t>(c - 'a' + 10);
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        *out = static_cast<uint8_t>(c - 'A' + 10);
        return true;
    }
    return false;
}

bool parse_sha256_hex(const char* hex, uint8_t out[32]) {
    if (hex == nullptr || std::strlen(hex) != 64) {
        return false;
    }
    for (size_t i = 0; i < 32; ++i) {
        uint8_t hi = 0;
        uint8_t lo = 0;
        if (!hex_nibble(hex[i * 2], &hi) || !hex_nibble(hex[i * 2 + 1], &lo)) {
            return false;
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

void sha256_to_hex(const uint8_t digest[32], char out[kShaHexMax]) {
    for (size_t i = 0; i < 32; ++i) {
        snprintf(out + (i * 2), 3, "%02x", digest[i]);
    }
    out[64] = '\0';
}

bool sha256_matches(const uint8_t digest[32], const char* expected_hex) {
    uint8_t expected[32] = {};
    if (!parse_sha256_hex(expected_hex, expected)) {
        return false;
    }
    return std::memcmp(digest, expected, 32) == 0;
}

bool read_string(cJSON* root, const char* key, char* out, size_t out_len) {
    const cJSON* item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsString(item) || item->valuestring == nullptr || item->valuestring[0] == '\0') {
        return false;
    }
    snprintf(out, out_len, "%s", item->valuestring);
    return true;
}

bool read_int64(cJSON* root, const char* key, int64_t* out) {
    const cJSON* item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *out = static_cast<int64_t>(item->valuedouble);
    return true;
}

bool read_bool(cJSON* root, const char* key, bool* out) {
    const cJSON* item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsBool(item)) {
        return false;
    }
    *out = cJSON_IsTrue(item);
    return true;
}

void fail_job(int64_t session_id, const char* reason) {
    log_phase(session_id, "failed", reason);
    emit_event(HEAPWATCH_EVENT_FW_UPDATE_FAILED);
    if (g_mutex != nullptr && xSemaphoreTake(g_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        g_phase = Phase::Idle;
        g_abort_requested = false;
        g_task = nullptr;
        xSemaphoreGive(g_mutex);
    }
}

/**
 * Same device REST auth as coredump / credential rotation: Bearer device API key
 * via resolveBootCredentials (NVS from enrollment, or fresh enrollment run).
 */
bool apply_device_api_key_auth(esp_http_client_handle_t client) {
    if (client == nullptr) {
        return false;
    }

    heapwatch::connector_bootstrap::BootCredentials creds = {};
    if (!heapwatch::connector_bootstrap::loadBootCredentials(&creds) ||
        creds.device_api_key[0] == '\0') {
        ESP_LOGE(kLogTag, "Device API key missing; cannot auth OTA download");
        return false;
    }

    char auth_header[HEAPWATCH_MAX_DEVICE_API_KEY_LEN + 16] = {};
    std::snprintf(auth_header, sizeof(auth_header), "Bearer %s", creds.device_api_key);
    if (esp_http_client_set_header(client, "Authorization", auth_header) != ESP_OK) {
        return false;
    }
    return true;
}

bool flash_data_partition(const DataPartitionJob& part, int64_t session_id) {
    const esp_partition_t* partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, part.name);
    if (partition == nullptr) {
        ESP_LOGE(kLogTag, "Data partition '%s' not found", part.name);
        return false;
    }

    if (part.size > 0 && static_cast<uint32_t>(part.size) > partition->size) {
        ESP_LOGE(kLogTag, "Data image too large for partition '%s'", part.name);
        return false;
    }

    char detail[96];
    snprintf(detail, sizeof(detail), "partition=%s", part.name);
    log_phase(session_id, "data_flash", detail);

    if (esp_partition_erase_range(partition, 0, partition->size) != ESP_OK) {
        return false;
    }

    esp_http_client_config_t config = {};
    config.url = part.download_url;
    config.timeout_ms = kHttpTimeoutMs;
    config.buffer_size = static_cast<int>(kDownloadChunk);
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }

    if (!apply_device_api_key_auth(client)) {
        esp_http_client_cleanup(client);
        return false;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return false;
    }

    (void)esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);
    size_t written = 0;
    size_t last_progress_bytes = 0;
    size_t last_yield_bytes = 0;
    const int64_t expected_total = part.size > 0 ? part.size : 0;
    emit_progress(session_id, "flashing", 0, expected_total);
    while (true) {
        if (g_abort_requested) {
            mbedtls_sha256_free(&sha_ctx);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }

        const int n =
            esp_http_client_read(client, reinterpret_cast<char*>(g_ota_io_buf), kDownloadChunk);
        if (n < 0) {
            mbedtls_sha256_free(&sha_ctx);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
        if (n == 0) {
            break;
        }

        if (written + static_cast<size_t>(n) > partition->size) {
            mbedtls_sha256_free(&sha_ctx);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }

        if (esp_partition_write(partition, written, g_ota_io_buf, static_cast<size_t>(n)) !=
            ESP_OK) {
            mbedtls_sha256_free(&sha_ctx);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }

        mbedtls_sha256_update(&sha_ctx, g_ota_io_buf, static_cast<size_t>(n));
        written += static_cast<size_t>(n);

        if (written - last_progress_bytes >= kProgressEveryBytes) {
            last_progress_bytes = written;
            emit_progress(session_id, "flashing", static_cast<int64_t>(written), expected_total);
        }
        if (written - last_yield_bytes >= kMqttYieldEveryBytes) {
            last_yield_bytes = written;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    uint8_t digest[32] = {};
    mbedtls_sha256_finish(&sha_ctx, digest);
    mbedtls_sha256_free(&sha_ctx);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (part.sha256[0] != '\0' && !sha256_matches(digest, part.sha256)) {
        ESP_LOGE(kLogTag, "Data partition SHA mismatch for '%s'", part.name);
        return false;
    }

    if (part.size > 0 && written != static_cast<size_t>(part.size)) {
        ESP_LOGE(kLogTag, "Data partition size mismatch for '%s'", part.name);
        return false;
    }

    return true;
}

bool stream_flash_app(const OtaJob& job) {
    const esp_partition_t* update = esp_ota_get_next_update_partition(nullptr);
    if (update == nullptr) {
        fail_job(job.ota_session_id, "reason=no_ota_partition");
        return false;
    }

    char slot_detail[64];
    snprintf(slot_detail, sizeof(slot_detail), "slot=%s size=%lu", update->label,
             static_cast<unsigned long>(update->size));
    log_phase(job.ota_session_id, "flash", slot_detail);

    // Streaming HTTPS → OTA partition. Event order stays monotonic for the backend:
    // STARTED → DOWNLOADING → DOWNLOADED → FLASHING → FLASHED → SUCCESSFUL
    emit_event(HEAPWATCH_EVENT_FW_UPDATE_DOWNLOADING);
    log_phase(job.ota_session_id, "download", "starting");
    emit_progress(job.ota_session_id, "downloading", 0,
                  job.code_size > 0 ? job.code_size : 0);

    esp_http_client_config_t config = {};
    config.url = job.code_download_url;
    config.timeout_ms = kHttpTimeoutMs;
    config.buffer_size = static_cast<int>(kDownloadChunk);
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        fail_job(job.ota_session_id, "reason=http_init");
        return false;
    }

    if (!apply_device_api_key_auth(client)) {
        esp_http_client_cleanup(client);
        fail_job(job.ota_session_id, "reason=http_auth");
        return false;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        fail_job(job.ota_session_id, "reason=http_open");
        return false;
    }

    (void)esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        fail_job(job.ota_session_id, "reason=http_status");
        return false;
    }

    esp_ota_handle_t ota_handle = 0;
    const size_t image_size =
        job.code_size > 0 ? static_cast<size_t>(job.code_size) : OTA_SIZE_UNKNOWN;
    err = esp_ota_begin(update, image_size, &ota_handle);
    if (err != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        fail_job(job.ota_session_id, "reason=ota_begin");
        return false;
    }

    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);
    size_t total = 0;
    size_t last_progress_bytes = 0;
    size_t last_yield_bytes = 0;
    const int64_t expected_total = job.code_size > 0 ? job.code_size : 0;
    while (true) {
        if (g_abort_requested) {
            esp_ota_abort(ota_handle);
            mbedtls_sha256_free(&sha_ctx);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            log_phase(job.ota_session_id, "aborted", "reason=user_abort_before_boot_pointer");
            if (g_mutex != nullptr && xSemaphoreTake(g_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                g_phase = Phase::Idle;
                g_abort_requested = false;
                g_task = nullptr;
                xSemaphoreGive(g_mutex);
            }
            return false;
        }

        const int n =
            esp_http_client_read(client, reinterpret_cast<char*>(g_ota_io_buf), kDownloadChunk);
        if (n < 0) {
            esp_ota_abort(ota_handle);
            mbedtls_sha256_free(&sha_ctx);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            fail_job(job.ota_session_id, "reason=http_read");
            return false;
        }
        if (n == 0) {
            break;
        }

        err = esp_ota_write(ota_handle, g_ota_io_buf, static_cast<size_t>(n));
        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            mbedtls_sha256_free(&sha_ctx);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            fail_job(job.ota_session_id, "reason=ota_write");
            return false;
        }

        mbedtls_sha256_update(&sha_ctx, g_ota_io_buf, static_cast<size_t>(n));
        total += static_cast<size_t>(n);

        // Progress every 128 KiB; yield every download chunk so MQTT can deliver ota/abort.
        if (total - last_progress_bytes >= kProgressEveryBytes) {
            last_progress_bytes = total;
            char progress[64];
            snprintf(progress, sizeof(progress), "bytes=%u", static_cast<unsigned>(total));
            log_phase(job.ota_session_id, "download", progress);
            emit_progress(job.ota_session_id, "downloading", static_cast<int64_t>(total),
                          expected_total);
        }
        if (total - last_yield_bytes >= kMqttYieldEveryBytes) {
            last_yield_bytes = total;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    uint8_t digest[32] = {};
    mbedtls_sha256_finish(&sha_ctx, digest);
    mbedtls_sha256_free(&sha_ctx);

    char digest_hex[kShaHexMax] = {};
    sha256_to_hex(digest, digest_hex);

    if (job.code_checksum[0] != '\0' && !sha256_matches(digest, job.code_checksum)) {
        esp_ota_abort(ota_handle);
        fail_job(job.ota_session_id, "reason=sha256_mismatch");
        return false;
    }

    if (job.code_size > 0 && total != static_cast<size_t>(job.code_size)) {
        esp_ota_abort(ota_handle);
        fail_job(job.ota_session_id, "reason=size_mismatch");
        return false;
    }

    emit_event(HEAPWATCH_EVENT_FW_UPDATE_DOWNLOADED);
    emit_progress(job.ota_session_id, "downloaded", static_cast<int64_t>(total),
                  expected_total > 0 ? expected_total : static_cast<int64_t>(total));
    char verify_detail[96];
    snprintf(verify_detail, sizeof(verify_detail), "sha256=%s", digest_hex);
    log_phase(job.ota_session_id, "verify", verify_detail);

    emit_event(HEAPWATCH_EVENT_FW_UPDATE_FLASHING);
    emit_progress(job.ota_session_id, "flashing", static_cast<int64_t>(total),
                  expected_total > 0 ? expected_total : static_cast<int64_t>(total));
    log_phase(job.ota_session_id, "flashing", "finalize");

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        fail_job(job.ota_session_id, "reason=ota_end");
        return false;
    }

    if (job.include_data) {
        for (size_t i = 0; i < job.data_count; ++i) {
            if (g_abort_requested) {
                log_phase(job.ota_session_id, "aborted", "reason=user_abort_before_boot_pointer");
                if (g_mutex != nullptr && xSemaphoreTake(g_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                    g_phase = Phase::Idle;
                    g_abort_requested = false;
                    g_task = nullptr;
                    xSemaphoreGive(g_mutex);
                }
                return false;
            }
            if (!flash_data_partition(job.data[i], job.ota_session_id)) {
                fail_job(job.ota_session_id, "reason=data_flash");
                return false;
            }
        }
    }

    // Last chance to abort before switching the boot pointer.
    if (g_abort_requested) {
        log_phase(job.ota_session_id, "aborted", "reason=user_abort_before_boot_pointer");
        if (g_mutex != nullptr && xSemaphoreTake(g_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
            g_phase = Phase::Idle;
            g_abort_requested = false;
            g_task = nullptr;
            xSemaphoreGive(g_mutex);
        }
        return false;
    }

    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        fail_job(job.ota_session_id, "reason=set_boot_partition");
        return false;
    }

    if (g_mutex != nullptr && xSemaphoreTake(g_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        g_phase = Phase::BootPointerSwitched;
        xSemaphoreGive(g_mutex);
    }

    emit_event(HEAPWATCH_EVENT_FW_UPDATE_FLASHED);
    emit_event(HEAPWATCH_EVENT_FW_UPDATE_SUCCESSFUL);
    emit_progress(job.ota_session_id, "flashed",
                  expected_total > 0 ? expected_total : static_cast<int64_t>(total),
                  expected_total > 0 ? expected_total : static_cast<int64_t>(total));
    log_phase(job.ota_session_id, "reboot", "boot_pointer_switched");

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return true;
}

void ota_task(void* /*arg*/) {
    // Use file-local g_job in place (anonymous namespace). No stack snapshot: start
    // rejects concurrent jobs and abort only sets g_abort_requested.
    emit_event(HEAPWATCH_EVENT_FW_UPDATE_STARTED);
    log_phase(g_job.ota_session_id, "accepted", nullptr);
    emit_progress(g_job.ota_session_id, "started", 0, g_job.code_size > 0 ? g_job.code_size : 0);

    (void)stream_flash_app(g_job);

    if (g_mutex != nullptr && xSemaphoreTake(g_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        if (g_phase != Phase::BootPointerSwitched) {
            g_phase = Phase::Idle;
        }
        g_task = nullptr;
        xSemaphoreGive(g_mutex);
    }
    vTaskDelete(nullptr);
}

bool parse_start_payload(const char* payload, size_t payload_len, OtaJob* job) {
    cJSON* root = cJSON_ParseWithLength(payload, payload_len);
    if (root == nullptr || job == nullptr) {
        cJSON_Delete(root);
        return false;
    }

    *job = OtaJob{};

    if (!read_int64(root, "ota_session_id", &job->ota_session_id) ||
        !read_string(root, "code_download_url", job->code_download_url, sizeof(job->code_download_url)) ||
        !read_string(root, "code_checksum", job->code_checksum, sizeof(job->code_checksum)) ||
        !read_int64(root, "code_size", &job->code_size)) {
        cJSON_Delete(root);
        return false;
    }

    (void)read_bool(root, "include_data_partitions", &job->include_data);

    const cJSON* data = cJSON_GetObjectItem(root, "data_partitions");
    if (job->include_data && cJSON_IsArray(data)) {
        const int count = cJSON_GetArraySize(data);
        for (int i = 0; i < count && job->data_count < kMaxDataPartitions; ++i) {
            const cJSON* entry = cJSON_GetArrayItem(data, i);
            if (!cJSON_IsObject(entry)) {
                continue;
            }
            DataPartitionJob& part = job->data[job->data_count];
            if (!read_string(const_cast<cJSON*>(entry), "name", part.name, sizeof(part.name)) ||
                !read_string(const_cast<cJSON*>(entry), "download_url", part.download_url,
                             sizeof(part.download_url))) {
                continue;
            }
            (void)read_string(const_cast<cJSON*>(entry), "sha256", part.sha256, sizeof(part.sha256));
            (void)read_int64(const_cast<cJSON*>(entry), "size", &part.size);
            job->data_count++;
        }
    }

    cJSON_Delete(root);
    return true;
}

bool handle_start(const char* command_id, const char* payload, size_t payload_len) {
    OtaJob job = {};
    if (!parse_start_payload(payload, payload_len, &job)) {
        heapwatch::device_command::publishCommandResponse("ota/start", command_id, false,
                                                          "invalid_payload",
                                                          "Missing required OTA start fields");
        return true;
    }

    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running != nullptr) {
        esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
        if (esp_ota_get_state_partition(running, &state) == ESP_OK
            && state == ESP_OTA_IMG_PENDING_VERIFY) {
            heapwatch::device_command::publishCommandResponse(
                "ota/start", command_id, false, "ota_pending_confirm",
                "Running image is PENDING_VERIFY; confirm or roll back before starting OTA");
            return true;
        }
    }

    if (g_mutex == nullptr) {
        heapwatch::device_command::publishCommandResponse("ota/start", command_id, false, "other",
                                                          "OTA service not initialized");
        return true;
    }

    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        heapwatch::device_command::publishCommandResponse("ota/start", command_id, false, "busy",
                                                          "OTA mutex busy");
        return true;
    }

    if (g_phase == Phase::Running || g_task != nullptr) {
        xSemaphoreGive(g_mutex);
        heapwatch::device_command::publishCommandResponse("ota/start", command_id, false, "busy",
                                                          "OTA already in progress");
        return true;
    }

    g_job = job;
    g_abort_requested = false;
    g_phase = Phase::Running;

    BaseType_t created =
        xTaskCreate(ota_task, "hw_ota", 12288, nullptr, tskIDLE_PRIORITY + 3, &g_task);
    if (created != pdPASS) {
        g_phase = Phase::Idle;
        g_task = nullptr;
        xSemaphoreGive(g_mutex);
        heapwatch::device_command::publishCommandResponse("ota/start", command_id, false, "other",
                                                          "Failed to create OTA task");
        return true;
    }

    xSemaphoreGive(g_mutex);
    heapwatch::device_command::publishCommandResponse("ota/start", command_id, true, "", "OTA started");
    return true;
}

bool handle_abort(const char* command_id) {
    if (g_mutex == nullptr) {
        heapwatch::device_command::publishCommandResponse("ota/abort", command_id, false, "other",
                                                          "OTA service not initialized");
        return true;
    }

    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        heapwatch::device_command::publishCommandResponse("ota/abort", command_id, false, "busy",
                                                          "OTA mutex busy");
        return true;
    }

    if (g_phase == Phase::BootPointerSwitched) {
        xSemaphoreGive(g_mutex);
        heapwatch::device_command::publishCommandResponse(
            "ota/abort", command_id, false, "too_late",
            "Boot pointer already switched; use rollback or confirm");
        return true;
    }

    if (g_phase != Phase::Running) {
        xSemaphoreGive(g_mutex);
        heapwatch::device_command::publishCommandResponse("ota/abort", command_id, false, "other",
                                                          "No OTA in progress");
        return true;
    }

    g_abort_requested = true;
    xSemaphoreGive(g_mutex);
    heapwatch::device_command::publishCommandResponse("ota/abort", command_id, true, "",
                                                      "Abort requested");
    return true;
}

bool handle_confirm(const char* command_id) {
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        heapwatch::device_command::publishCommandResponse("ota/confirm", command_id, false,
                                                          "confirm_failed", esp_err_to_name(err));
        return true;
    }

    log_phase(0, "confirmed", "esp_ota_mark_app_valid_cancel_rollback");
    emit_event(HEAPWATCH_EVENT_FW_UPDATE_CONFIRMED);
    heapwatch::device_command::publishCommandResponse("ota/confirm", command_id, true, "",
                                                      "Firmware permanently confirmed");
    // Specs carry ota_pending_confirm; republish so capability matches the confirmed image.
    HeapwatchConnector::getInstance().queueSpecs();
    return true;
}

bool handle_rollback(const char* command_id) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running == nullptr) {
        heapwatch::device_command::publishCommandResponse("ota/rollback", command_id, false,
                                                          "no_running", "No running partition");
        return true;
    }

    // Prefer last_invalid when available; otherwise the other OTA slot.
    const esp_partition_t* previous = esp_ota_get_last_invalid_partition();
    if (previous == nullptr) {
        if (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) {
            previous = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1,
                                                nullptr);
        } else if (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) {
            previous = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0,
                                                nullptr);
        }
    }

    if (previous == nullptr) {
        heapwatch::device_command::publishCommandResponse("ota/rollback", command_id, false,
                                                          "no_previous", "No previous OTA slot");
        return true;
    }

    const esp_err_t err = esp_ota_set_boot_partition(previous);
    if (err != ESP_OK) {
        heapwatch::device_command::publishCommandResponse("ota/rollback", command_id, false,
                                                          "set_boot_failed", esp_err_to_name(err));
        return true;
    }

    log_phase(0, "rolled_back", previous->label);
    heapwatch::device_command::publishCommandResponse("ota/rollback", command_id, true, "",
                                                      "Rollback boot pointer set; rebooting");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return true;
}

}  // namespace

void init() {
#if !CONFIG_HEAPWATCH_ENABLE_OTA
    ESP_LOGI(kLogTag, "Heapwatch OTA disabled (CONFIG_HEAPWATCH_ENABLE_OTA=n)");
    return;
#endif
    if (g_mutex == nullptr) {
        g_mutex = xSemaphoreCreateMutex();
    }
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    if (next != nullptr) {
        ESP_LOGI(kLogTag, "OTA service ready (next slot=%s size=%lu chunk=%u B)", next->label,
                 static_cast<unsigned long>(next->size), static_cast<unsigned>(kDownloadChunk));
    } else {
        ESP_LOGW(kLogTag, "OTA service ready but no next OTA partition (factory-only layout?)");
    }
}

bool isInProgress() {
#if !CONFIG_HEAPWATCH_ENABLE_OTA
    return false;
#else
    return g_phase == Phase::Running;
#endif
}

bool handleMqttCommand(const char* suffix, const char* command_id, const char* payload,
                       size_t payload_len) {
    if (suffix == nullptr || std::strncmp(suffix, "ota/", 4) != 0) {
        return false;
    }

#if !CONFIG_HEAPWATCH_ENABLE_OTA
    (void)payload;
    (void)payload_len;
    heapwatch::device_command::publishCommandResponse(
        suffix, command_id, false, "feature_disabled",
        "Heapwatch OTA is disabled on this device");
    return true;
#else
    if (strcmp(suffix, "ota/start") == 0) {
        return handle_start(command_id, payload, payload_len);
    }
    if (strcmp(suffix, "ota/abort") == 0) {
        return handle_abort(command_id);
    }
    // During HTTPS OTA only Safe Abort is allowed — confirm/rollback wait until
    // the transfer finishes and the device is idle (typically PENDING_VERIFY).
    if (isInProgress() && (strcmp(suffix, "ota/confirm") == 0 || strcmp(suffix, "ota/rollback") == 0)) {
        ESP_LOGW(kLogTag, "Rejecting %s — OTA in progress", suffix);
        heapwatch::device_command::publishCommandResponse(
            suffix, command_id, false, "busy",
            "Rejected: firmware OTA is in progress");
        return true;
    }
    if (strcmp(suffix, "ota/confirm") == 0) {
        return handle_confirm(command_id);
    }
    if (strcmp(suffix, "ota/rollback") == 0) {
        return handle_rollback(command_id);
    }

    heapwatch::device_command::publishCommandResponse(suffix, command_id, false, "unsupported",
                                                      "Unknown OTA command");
    return true;
#endif
}

}  // namespace heapwatch::device_ota
