#include "heapwatch/Enrollment/CredentialRotation/DeviceCredentialRotationClient.h"

#include <cstdio>
#include <cstring>
#include <cinttypes>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "heapwatch/Helpers/Tls/Tls.h"

namespace Heapwatch::Enrollment::CredentialRotation {

namespace {
constexpr char TAG[] = "HwEnroll";
constexpr size_t HTTP_RESPONSE_MAX = 2048;
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

bool authorized_post(const char* url,
                     const char* bearer_token,
                     HttpResponseBuffer* response) {
    if (url == nullptr || bearer_token == nullptr || response == nullptr) {
        return false;
    }

    response->len = 0;
    response->data[0] = '\0';

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 15000;
    config.event_handler = http_event_handler;
    config.user_data = response;
    Heapwatch::Helpers::Tls::applyToHttpClient(config);

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }

    Heapwatch::Helpers::Tls::applyHttpHostHeader(client);
    char auth_header[HEAPWATCH_MAX_DEVICE_API_KEY_LEN + 16] = {};
    std::snprintf(auth_header, sizeof(auth_header), "Bearer %s", bearer_token);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, "{}", 2);

    const esp_err_t err = esp_http_client_perform(client);
    const int32_t status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        return false;
    }

    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTP status %" PRId32 " body=%s", status, response->data);
        return false;
    }

    return true;
}

bool copy_json_string(const cJSON* object, const char* key, char* out, size_t out_len) {
    const cJSON* item = cJSON_GetObjectItem(object, key);
    if (item == nullptr || !cJSON_IsString(item) || item->valuestring == nullptr) {
        return false;
    }

    std::strncpy(out, item->valuestring, out_len - 1);
    out[out_len - 1] = '\0';
    return out[0] != '\0';
}

bool parse_fetch_payload(const char* json,
                         char* mqtt_password,
                         size_t mqtt_password_len,
                         char* device_api_key,
                         size_t device_api_key_len) {
    cJSON* root = cJSON_Parse(json);
    if (root == nullptr) {
        return false;
    }

    const cJSON* data = cJSON_GetObjectItem(root, "data");
    const bool ok = data != nullptr && cJSON_IsObject(data) &&
                    copy_json_string(data, "mqtt_password", mqtt_password, mqtt_password_len) &&
                    copy_json_string(data, "device_api_key", device_api_key, device_api_key_len);
    cJSON_Delete(root);
    return ok;
}
}  // namespace

bool fetchAndConfirm(const char* api_base, const Heapwatch::Enrollment::Credentials::Credentials* current,
                      Heapwatch::Enrollment::Credentials::Credentials* updated) {
    if (api_base == nullptr || current == nullptr || updated == nullptr ||
        current->device_api_key[0] == '\0') {
        return false;
    }

    char fetch_url[HTTP_URL_MAX] = {};
    std::snprintf(fetch_url, sizeof(fetch_url), "%s/api/devices/self/credentials/rotate/fetch",
                  api_base);

    HttpResponseBuffer fetch_response = {};
    if (!authorized_post(fetch_url, current->device_api_key, &fetch_response)) {
        return false;
    }

    char next_mqtt_password[HEAPWATCH_MAX_MQTT_PASSWORD_LEN] = {};
    char next_api_key[HEAPWATCH_MAX_DEVICE_API_KEY_LEN] = {};
    if (!parse_fetch_payload(fetch_response.data, next_mqtt_password, sizeof(next_mqtt_password),
                             next_api_key, sizeof(next_api_key))) {
        ESP_LOGE(TAG, "Failed to parse fetch response");
        return false;
    }

    char confirm_url[HTTP_URL_MAX] = {};
    std::snprintf(confirm_url, sizeof(confirm_url),
                  "%s/api/devices/self/credentials/rotate/confirm", api_base);

    HttpResponseBuffer confirm_response = {};
    if (!authorized_post(confirm_url, next_api_key, &confirm_response)) {
        return false;
    }

    std::strncpy(updated->tenant_slug, current->tenant_slug, sizeof(updated->tenant_slug) - 1);
    std::strncpy(updated->device_uid, current->device_uid, sizeof(updated->device_uid) - 1);
    std::strncpy(updated->mqtt_password, next_mqtt_password, sizeof(updated->mqtt_password) - 1);
    std::strncpy(updated->device_api_key, next_api_key, sizeof(updated->device_api_key) - 1);
    std::strncpy(updated->stream_api_key, current->stream_api_key, sizeof(updated->stream_api_key) - 1);

    ESP_LOGI(TAG, "Credential rotation confirmed for %s", updated->device_uid);
    return true;
}

}  // namespace Heapwatch::Enrollment::CredentialRotation
