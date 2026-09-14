#include "ConfigCatalog.h"

#include <cstring>

#include "cJSON.h"
#include "esp_log.h"

#include "../connector_settings/ConnectorSettings.h"
#include "../stream_ingress/StreamIngressClient.h"
#include "../stream_ingress/StreamMuxJsonUpload.h"

namespace heapwatch::device_command {

namespace {

constexpr char TAG[] = "HwCmd";
constexpr char CHANNEL_CONFIG_CATALOG[] = "config_catalog";
constexpr uint32_t STREAM_CONNECT_TIMEOUT_MS = 30000;

bool g_uploaded_this_boot = false;
bool g_upload_requested = false;
bool g_force_upload = false;

cJSON* buildConfigCatalogRootObject() {
    cJSON* catalogs = heapwatch::connector_settings::buildAllConfigCatalogsJson();
    if (catalogs == nullptr) {
        return nullptr;
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        cJSON_Delete(catalogs);
        return nullptr;
    }

    cJSON_AddItemToObject(root, "catalogs", catalogs);
    return root;
}

}  // namespace

void uploadConfigCatalog(bool force) {
    if (!force && g_uploaded_this_boot) {
        return;
    }

    cJSON* root = buildConfigCatalogRootObject();
    if (root == nullptr) {
        ESP_LOGW(TAG, "Failed to build config catalog JSON");
        return;
    }

    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == nullptr) {
        ESP_LOGW(TAG, "Failed to serialize config catalog");
        return;
    }

    const size_t body_len = strlen(body);

    if (!heapwatch::stream_ingress::waitForStreamTcp(STREAM_CONNECT_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "Stream ingress not ready for config catalog upload");
        cJSON_free(body);
        return;
    }

    if (!heapwatch::stream_ingress::streamJsonBody(CHANNEL_CONFIG_CATALOG, body)) {
        ESP_LOGW(TAG, "Config catalog stream upload failed");
        cJSON_free(body);
        return;
    }

    cJSON_free(body);

    g_uploaded_this_boot = true;
    ESP_LOGI(TAG, "Config catalog streamed (%u bytes)", static_cast<unsigned>(body_len));
}

void requestConfigCatalogUpload(bool force) {
    g_upload_requested = true;
    if (force) {
        g_force_upload = true;
    }
    heapwatch::stream_ingress::ensureBootStreamUploadWorker();
}

bool configCatalogUploadedThisBoot() { return g_uploaded_this_boot; }

bool consumeConfigCatalogUploadRequest(bool* force_out) {
    if (!g_upload_requested) {
        return false;
    }

    g_upload_requested = false;
    if (force_out != nullptr) {
        *force_out = g_force_upload;
        g_force_upload = false;
    } else {
        g_force_upload = false;
    }
    return true;
}

void resetConfigCatalogBootFlags() {
    g_uploaded_this_boot = false;
    g_upload_requested = false;
    g_force_upload = false;
}

}  // namespace heapwatch::device_command
