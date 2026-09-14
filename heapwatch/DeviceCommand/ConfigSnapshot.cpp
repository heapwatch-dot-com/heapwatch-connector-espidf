#include "heapwatch/DeviceCommand/ConfigSnapshot.h"

#include <cstdio>
#include <cstring>
#include <cinttypes>

#include "cJSON.h"
#include "esp_log.h"

#include "heapwatch/Settings/ConnectorSettings.h"
#include "heapwatch/StreamIngress/StreamIngressClient.h"
#include "heapwatch/StreamIngress/StreamMuxJsonUpload.h"

namespace Heapwatch::DeviceCommand {

namespace {

constexpr char TAG[] = "HwCmd";
constexpr char CHANNEL_CONFIG_SNAPSHOT[] = "config_snapshot";
constexpr uint32_t STREAM_CONNECT_TIMEOUT_MS = 30000;

bool g_full_partition_uploaded_this_boot = false;
bool g_upload_requested = false;
bool g_force_upload = false;
char g_requested_namespace[32] = "*";

bool isWildcardNamespace(const char* nvs_namespace) {
    return nvs_namespace != nullptr &&
           (strcmp(nvs_namespace, "*") == 0 || strcmp(nvs_namespace, "all") == 0);
}

cJSON* buildSnapshotRootObject(const char* nvs_namespace) {
    if (isWildcardNamespace(nvs_namespace)) {
        return Heapwatch::Settings::buildFullPartitionResponse();
    }

    if (Heapwatch::Settings::isManagedNamespace(nvs_namespace)) {
        return Heapwatch::Settings::buildManagedNamespaceResponse(nvs_namespace);
    }

    return Heapwatch::Settings::buildUnmanagedNamespaceResponse(nvs_namespace);
}

}  // namespace

void uploadConfigSnapshot(const char* nvs_namespace, bool force) {
    if (nvs_namespace == nullptr || nvs_namespace[0] == '\0') {
        nvs_namespace = "*";
    }

    if (!force && isWildcardNamespace(nvs_namespace) && g_full_partition_uploaded_this_boot) {
        return;
    }

    cJSON* root = buildSnapshotRootObject(nvs_namespace);
    if (root == nullptr) {
        ESP_LOGW(TAG, "Failed to build config snapshot for namespace '%s'", nvs_namespace);
        return;
    }

    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == nullptr) {
        ESP_LOGW(TAG, "Failed to serialize config snapshot");
        return;
    }

    const size_t body_len = strlen(body);

    if (!Heapwatch::StreamIngress::waitForStreamTcp(STREAM_CONNECT_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "Stream ingress not ready for config snapshot upload");
        cJSON_free(body);
        return;
    }

    if (!Heapwatch::StreamIngress::streamJsonBody(CHANNEL_CONFIG_SNAPSHOT, body)) {
        ESP_LOGW(TAG, "Config snapshot stream upload failed");
        cJSON_free(body);
        return;
    }

    cJSON_free(body);

    if (isWildcardNamespace(nvs_namespace)) {
        g_full_partition_uploaded_this_boot = true;
    }

    ESP_LOGI(TAG, "Config snapshot streamed (%" PRIu32 " bytes, ns=%s)", static_cast<uint32_t>(body_len),
             nvs_namespace);
}

void requestConfigSnapshotUpload(const char* nvs_namespace, bool force) {
    if (nvs_namespace == nullptr || nvs_namespace[0] == '\0') {
        snprintf(g_requested_namespace, sizeof(g_requested_namespace), "*");
    } else {
        snprintf(g_requested_namespace, sizeof(g_requested_namespace), "%s", nvs_namespace);
    }

    g_upload_requested = true;
    if (force) {
        g_force_upload = true;
    }
    Heapwatch::StreamIngress::ensureBootStreamUploadWorker();
}

bool configSnapshotUploadedThisBoot() { return g_full_partition_uploaded_this_boot; }

bool consumeConfigSnapshotUploadRequest(char* namespace_out, size_t namespace_out_len, bool* force_out) {
    if (!g_upload_requested) {
        return false;
    }

    g_upload_requested = false;
    if (namespace_out != nullptr && namespace_out_len > 0) {
        snprintf(namespace_out, namespace_out_len, "%s", g_requested_namespace);
    }
    if (force_out != nullptr) {
        *force_out = g_force_upload;
        g_force_upload = false;
    } else {
        g_force_upload = false;
    }
    return true;
}

void resetConfigSnapshotBootFlags() {
    g_full_partition_uploaded_this_boot = false;
    g_upload_requested = false;
    g_force_upload = false;
}

}  // namespace Heapwatch::DeviceCommand
