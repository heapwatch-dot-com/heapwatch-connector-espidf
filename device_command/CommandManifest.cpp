#include "CommandManifest.h"

#include "CommandManifestBuiltin.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#include "../stream_ingress/StreamIngressClient.h"
#include "../stream_ingress/StreamIngressMux.h"
#include "heapwatch.h"

namespace heapwatch::device_command {

namespace {

constexpr char TAG[] = "CmdManifest";
constexpr char kChannelCommandManifest[] = "command_manifest";
constexpr char kNvsNamespace[] = "heapwatch";
constexpr char kNvsKeyManifestHash[] = "man_hash";
constexpr char kNvsKeyManifestBuild[] = "man_build";
constexpr size_t kManifestHashHexLen = 64;
constexpr size_t kManifestBuildMax = 256;
constexpr uint32_t kStreamConnectTimeoutMs = 30000;
constexpr uint32_t kMuxSendRetryMs = 30000;

bool g_uploaded_this_boot = false;

bool readNvsString(const char* key, char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return false;
    }

    out[0] = '\0';
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    size_t required = out_len;
    const esp_err_t err = nvs_get_str(handle, key, out, &required);
    nvs_close(handle);

    return err == ESP_OK && out[0] != '\0';
}

bool writeNvsUploadMarker(const char* manifest_hash_hex, const char* build_identifier) {
    if (manifest_hash_hex == nullptr || build_identifier == nullptr) {
        return false;
    }

    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    bool ok = nvs_set_str(handle, kNvsKeyManifestHash, manifest_hash_hex) == ESP_OK &&
              nvs_set_str(handle, kNvsKeyManifestBuild, build_identifier) == ESP_OK &&
              nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
}

void sha256Hex(const char* body, char* out_hex, size_t out_hex_len) {
    if (body == nullptr || out_hex == nullptr || out_hex_len < kManifestHashHexLen + 1) {
        return;
    }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, reinterpret_cast<const uint8_t*>(body), strlen(body));

    uint8_t digest[32] = {};
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);

    for (size_t i = 0; i < sizeof(digest); ++i) {
        snprintf(out_hex + (i * 2), 3, "%02x", digest[i]);
    }
    out_hex[kManifestHashHexLen] = '\0';
}

bool waitForStreamTcp(uint32_t timeout_ms) {
    if (!heapwatch::stream_ingress::ensureTcpSession()) {
        return false;
    }

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        if (heapwatch::stream_ingress::isActive()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return false;
}

bool sendCtrlEnd(const char* channel_id) {
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(kMuxSendRetryMs);
    while (xTaskGetTickCount() < deadline) {
        if (heapwatch::stream_ingress::sendMuxCtrlFrame(channel_id)) {
            heapwatch::stream_ingress::flushOutbound();
            return true;
        }
        heapwatch::stream_ingress::flushOutbound();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

bool sendDataChunk(const char* channel_id, const uint8_t* data, size_t len) {
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(kMuxSendRetryMs);
    while (xTaskGetTickCount() < deadline) {
        if (heapwatch::stream_ingress::sendMuxDataFrame(channel_id, data, len)) {
            heapwatch::stream_ingress::flushOutbound();
            return true;
        }
        heapwatch::stream_ingress::flushOutbound();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

bool streamManifestBody(const char* body) {
    if (body == nullptr) {
        return false;
    }

    const size_t total_len = strlen(body);
    size_t offset = 0;

    while (offset < total_len) {
        const size_t chunk_len =
            (total_len - offset) > stream_ingress::mux::kMaxPayloadLen
                ? stream_ingress::mux::kMaxPayloadLen
                : (total_len - offset);

        if (!sendDataChunk(kChannelCommandManifest,
                           reinterpret_cast<const uint8_t*>(body + offset), chunk_len)) {
            ESP_LOGW(TAG, "Failed to queue manifest chunk at offset %u",
                     static_cast<unsigned>(offset));
            return false;
        }

        offset += chunk_len;
    }

    if (!sendCtrlEnd(kChannelCommandManifest)) {
        ESP_LOGW(TAG, "Failed to queue manifest end ctrl frame");
        return false;
    }

    return true;
}

}  // namespace

const char* getBuiltinCommandManifestJson() { return kBuiltinCommandManifestJson; }

cJSON* buildManifestRootObject() { return cJSON_Parse(kBuiltinCommandManifestJson); }

void uploadManifestIfNeeded() {
    if (g_uploaded_this_boot) {
        return;
    }

    const char* body = kBuiltinCommandManifestJson;
    const size_t body_len = strlen(body);

    const char* build_identifier = heapwatch_get_build_identifier();
    char manifest_hash_hex[kManifestHashHexLen + 1] = {};
    sha256Hex(body, manifest_hash_hex, sizeof(manifest_hash_hex));

    char stored_hash[kManifestHashHexLen + 1] = {};
    char stored_build[kManifestBuildMax + 1] = {};
    const bool has_stored = readNvsString(kNvsKeyManifestHash, stored_hash, sizeof(stored_hash)) &&
                            readNvsString(kNvsKeyManifestBuild, stored_build, sizeof(stored_build));

    if (has_stored && strcmp(stored_hash, manifest_hash_hex) == 0 &&
        strcmp(stored_build, build_identifier) == 0) {
        g_uploaded_this_boot = true;
        ESP_LOGI(TAG, "Command manifest unchanged — skip upload");
        return;
    }

    if (!waitForStreamTcp(kStreamConnectTimeoutMs)) {
        ESP_LOGW(TAG, "Stream ingress not ready for manifest upload");
        return;
    }

    if (!streamManifestBody(body)) {
        ESP_LOGW(TAG, "Command manifest stream upload failed");
        return;
    }

    if (!writeNvsUploadMarker(manifest_hash_hex, build_identifier)) {
        ESP_LOGW(TAG, "Manifest uploaded but NVS marker write failed");
    }

    g_uploaded_this_boot = true;
    ESP_LOGI(TAG, "Command manifest streamed (%u bytes)", static_cast<unsigned>(body_len));
}

}  // namespace heapwatch::device_command
