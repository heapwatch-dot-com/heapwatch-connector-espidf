#include "CommandCatalog.h"

#include "CommandCatalogBuiltin.h"

#include <cstring>
#include <cinttypes>

#include "cJSON.h"
#include "esp_log.h"

#include "../stream_ingress/StreamIngressClient.h"
#include "../stream_ingress/StreamMuxJsonUpload.h"

namespace heapwatch::device_command {

namespace {

constexpr char TAG[] = "HwCmd";
constexpr char CHANNEL_COMMAND_CATALOG[] = "command_catalog";
constexpr uint32_t STREAM_CONNECT_TIMEOUT_MS = 30000;

bool g_uploaded_this_boot = false;
bool g_upload_requested = false;
bool g_force_upload = false;

}  // namespace

const char* getBuiltinCommandCatalogJson() { return BUILTIN_COMMAND_CATALOG_JSON; }

void uploadCommandCatalogIfNeeded() {
    uploadCommandCatalog(false);
}

void uploadCommandCatalog(bool force) {
    if (!force && g_uploaded_this_boot) {
        return;
    }

    const char* body = BUILTIN_COMMAND_CATALOG_JSON;
    const size_t body_len = strlen(body);

    if (!heapwatch::stream_ingress::waitForStreamTcp(STREAM_CONNECT_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "Stream ingress not ready for command catalog upload");
        return;
    }

    if (!heapwatch::stream_ingress::streamJsonBody(CHANNEL_COMMAND_CATALOG, body)) {
        ESP_LOGW(TAG, "Command catalog stream upload failed");
        return;
    }

    g_uploaded_this_boot = true;
    ESP_LOGI(TAG, "Command catalog streamed (%" PRIu32 " bytes)", static_cast<uint32_t>(body_len));
}

void requestCommandCatalogUpload(bool force) {
    g_upload_requested = true;
    if (force) {
        g_force_upload = true;
    }
    heapwatch::stream_ingress::ensureBootStreamUploadWorker();
}

bool commandCatalogUploadedThisBoot() { return g_uploaded_this_boot; }

bool consumeCommandCatalogUploadRequest(bool* force_out) {
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

void resetCommandCatalogBootFlags() {
    g_uploaded_this_boot = false;
    g_upload_requested = false;
    g_force_upload = false;
}

}  // namespace heapwatch::device_command
