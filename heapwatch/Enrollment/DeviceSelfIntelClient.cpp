#include "heapwatch/Enrollment/DeviceSelfIntelClient.h"

#include <cstdio>
#include <cstring>
#include <cinttypes>

#include "cJSON.h"
#include "heapwatch/Enrollment/Credentials/DeviceCredentialsStore.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "heapwatch/Helpers/Tls/Tls.h"

namespace Heapwatch::Enrollment {

namespace {
constexpr char TAG[] = "HwEnroll";
constexpr size_t HTTP_RESPONSE_MAX = 1024;
constexpr size_t HTTP_URL_MAX = 512;

struct HttpResponseBuffer {
    char data[HTTP_RESPONSE_MAX];
    size_t len = 0;
};

esp_err_t http_event_handler(esp_http_client_event_t* evt) {
    if (evt == nullptr || evt->user_data == nullptr) {
        return ESP_OK;
    }

    auto* buffer = static_cast<HttpResponseBuffer*>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        const size_t remaining = HTTP_RESPONSE_MAX - 1 - buffer->len;
        const size_t copy_len = evt->data_len < remaining ? static_cast<size_t>(evt->data_len)
                                                          : remaining;
        if (copy_len > 0) {
            std::memcpy(buffer->data + buffer->len, evt->data, copy_len);
            buffer->len += copy_len;
            buffer->data[buffer->len] = '\0';
        }
    }

    return ESP_OK;
}
}  // namespace

bool fetchLastBootCycle(const char* api_base, const char* device_api_key,
                        uint32_t* last_boot_cycle_out) {
    if (api_base == nullptr || device_api_key == nullptr || last_boot_cycle_out == nullptr ||
        device_api_key[0] == '\0') {
        return false;
    }

    char url[HTTP_URL_MAX] = {};
    std::snprintf(url, sizeof(url), "%s/api/devices/self/intel", api_base);

    HttpResponseBuffer response = {};
    response.data[0] = '\0';

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 15000;
    config.event_handler = http_event_handler;
    config.user_data = &response;
    Heapwatch::Helpers::Tls::applyToHttpClient(config);

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }

    Heapwatch::Helpers::Tls::applyHttpHostHeader(client);
    char auth_header[HEAPWATCH_MAX_DEVICE_API_KEY_LEN + 16] = {};
    std::snprintf(auth_header, sizeof(auth_header), "Bearer %s", device_api_key);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Accept", "application/json");

    const esp_err_t err = esp_http_client_perform(client);
    const int32_t status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        return false;
    }

    if (status < 200 || status >= 300) {
        if (status == 401) {
            ESP_LOGW(TAG,
                     "Device API key rejected (401) — NVS credentials likely belong to another "
                     "Heapwatch stack or a deleted device; re-enroll or erase NVS (namespace "
                     "'heapwatch')");
        } else {
            ESP_LOGE(TAG, "HTTP status %" PRId32 " body=%s", status, response.data);
        }
        return false;
    }

    cJSON* root = cJSON_Parse(response.data);
    if (root == nullptr) {
        return false;
    }

    const cJSON* data = cJSON_GetObjectItem(root, "data");
    const cJSON* item = data != nullptr ? cJSON_GetObjectItem(data, "last_boot_cycle") : nullptr;
    if (item == nullptr || cJSON_IsNull(item)) {
        cJSON_Delete(root);
        *last_boot_cycle_out = UINT32_MAX;
        return true;
    }

    if (!cJSON_IsNumber(item)) {
        cJSON_Delete(root);
        return false;
    }

    *last_boot_cycle_out = static_cast<uint32_t>(item->valuedouble);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Self-intel last_boot_cycle=%" PRIu32,
             static_cast<uint32_t>(*last_boot_cycle_out));
    return true;
}

}  // namespace Heapwatch::Enrollment
