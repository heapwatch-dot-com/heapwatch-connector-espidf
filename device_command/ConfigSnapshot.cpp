#include "ConfigSnapshot.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_log.h"

#include "../connector_settings/ConnectorSettings.h"
#include "../stream_ingress/StreamIngressClient.h"
#include "../stream_ingress/StreamMuxJsonUpload.h"

namespace heapwatch::device_command {

namespace {

constexpr char TAG[] = "CfgSnapshot";
constexpr char kChannelConfigSnapshot[] = "config_snapshot";
constexpr uint32_t kStreamConnectTimeoutMs = 30000;

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
        return heapwatch::connector_settings::buildFullPartitionResponse();
    }

    if (heapwatch::connector_settings::isManagedNamespace(nvs_namespace)) {
        return heapwatch::connector_settings::buildManagedNamespaceResponse(nvs_namespace);
    }

    return heapwatch::connector_settings::buildUnmanagedNamespaceResponse(nvs_namespace);
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

    if (!heapwatch::stream_ingress::waitForStreamTcp(kStreamConnectTimeoutMs)) {
        ESP_LOGW(TAG, "Stream ingress not ready for config snapshot upload");
        cJSON_free(body);
        return;
    }

    if (!heapwatch::stream_ingress::streamJsonBody(kChannelConfigSnapshot, body)) {
        ESP_LOGW(TAG, "Config snapshot stream upload failed");
        cJSON_free(body);
        return;
    }

    cJSON_free(body);

    if (isWildcardNamespace(nvs_namespace)) {
        g_full_partition_uploaded_this_boot = true;
    }

    ESP_LOGI(TAG, "Config snapshot streamed (%u bytes, ns=%s)", static_cast<unsigned>(body_len),
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
    heapwatch::stream_ingress::ensureBootStreamUploadWorker();
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

}  // namespace heapwatch::device_command
