#include "DeviceEnrollmentClient.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace heapwatch::device_enrollment {

namespace {
constexpr char TAG[] = "HeapwatchEnroll";
constexpr size_t HTTP_RESPONSE_MAX = 2048;
constexpr size_t HTTP_URL_MAX = 512;
constexpr size_t HTTP_BODY_MAX = 768;

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

bool http_request(const char* url, esp_http_client_method_t method, const char* post_body,
                  const char* bearer_token, HttpResponseBuffer* response) {
    if (url == nullptr || response == nullptr) {
        return false;
    }

    response->len = 0;
    response->data[0] = '\0';

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = method;
    config.timeout_ms = 15000;
    config.event_handler = http_event_handler;
    config.user_data = response;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    if (method == HTTP_METHOD_POST) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        if (post_body != nullptr) {
            esp_http_client_set_post_field(client, post_body, std::strlen(post_body));
        }
    }

    if (bearer_token != nullptr && bearer_token[0] != '\0') {
        char auth_header[HEAPWATCH_MAX_DEVICE_API_KEY_LEN + 16] = {};
        std::snprintf(auth_header, sizeof(auth_header), "Bearer %s", bearer_token);
        esp_http_client_set_header(client, "Authorization", auth_header);
    }

    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        return false;
    }

    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTP status %d body=%s", status, response->data);
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

Status parse_enrollment_status(const char* status) {
    if (status == nullptr) {
        return Status::kError;
    }

    if (std::strcmp(status, "pending") == 0) {
        return Status::kPending;
    }
    if (std::strcmp(status, "accepted_awaiting_provision") == 0) {
        return Status::kAcceptedAwaitingProvision;
    }
    if (std::strcmp(status, "provisioned") == 0) {
        return Status::kProvisioned;
    }
    if (std::strcmp(status, "rejected") == 0) {
        return Status::kRejected;
    }
    if (std::strcmp(status, "re_enroll_blocked") == 0) {
        return Status::kReEnrollBlocked;
    }
    if (std::strcmp(status, "blocked_forever") == 0) {
        return Status::kBlockedForever;
    }
    if (std::strcmp(status, "key_expired") == 0) {
        return Status::kKeyExpired;
    }

    return Status::kError;
}

Status parse_status_payload(const cJSON* data) {
    char status[48] = {};
    if (!copy_json_string(data, "enrollment_status", status, sizeof(status))) {
        return Status::kError;
    }

    return parse_enrollment_status(status);
}

bool parse_enroll_payload(const cJSON* data, char* device_uid_out, size_t device_uid_out_len,
                          Status* status_out) {
    if (!copy_json_string(data, "device_uid", device_uid_out, device_uid_out_len)) {
        return false;
    }

    char status[48] = {};
    if (!copy_json_string(data, "enrollment_status", status, sizeof(status))) {
        return false;
    }

    if (status_out != nullptr) {
        *status_out = parse_enrollment_status(status);
    }

    return true;
}

bool fill_credentials_from_fetch(const cJSON* data,
                                 heapwatch::device_credentials::Credentials* out) {
    return copy_json_string(data, "tenant_slug", out->tenant_slug, sizeof(out->tenant_slug)) &&
           copy_json_string(data, "device_uid", out->device_uid, sizeof(out->device_uid)) &&
           copy_json_string(data, "mqtt_password", out->mqtt_password, sizeof(out->mqtt_password)) &&
           copy_json_string(data, "device_api_key", out->device_api_key,
                            sizeof(out->device_api_key)) &&
           copy_json_string(data, "stream_api_key", out->stream_api_key,
                            sizeof(out->stream_api_key));
}

bool is_terminal_failure(Status status) {
    return status == Status::kRejected || status == Status::kBlockedForever ||
           status == Status::kKeyExpired;
}

bool is_waiting_for_operator(Status status) {
    return status == Status::kPending || status == Status::kReEnrollBlocked;
}
}  // namespace

bool enroll(const char* api_base, const char* enrollment_key, const char* claimed_mac,
            const char* build_identifier, const char* build_environment, char* device_uid_out,
            size_t device_uid_out_len, Status* status_out) {
    if (api_base == nullptr || enrollment_key == nullptr || claimed_mac == nullptr ||
        device_uid_out == nullptr || device_uid_out_len == 0 || enrollment_key[0] == '\0' ||
        claimed_mac[0] == '\0') {
        return false;
    }

    char url[HTTP_URL_MAX] = {};
    std::snprintf(url, sizeof(url), "%s/api/device-enrollment/enroll", api_base);

    char body[HTTP_BODY_MAX] = {};
    std::snprintf(body, sizeof(body),
                  "{\"enrollment_key\":\"%s\",\"claimed_mac\":\"%s\","
                  "\"build_identifier\":\"%s\",\"build_environment\":\"%s\"}",
                  enrollment_key, claimed_mac, build_identifier != nullptr ? build_identifier : "",
                  build_environment != nullptr ? build_environment : "development");

    HttpResponseBuffer response = {};
    if (!http_request(url, HTTP_METHOD_POST, body, nullptr, &response)) {
        return false;
    }

    cJSON* root = cJSON_Parse(response.data);
    if (root == nullptr) {
        return false;
    }

    const cJSON* data = cJSON_GetObjectItem(root, "data");
    const bool ok =
        data != nullptr && cJSON_IsObject(data) &&
        parse_enroll_payload(data, device_uid_out, device_uid_out_len, status_out);
    cJSON_Delete(root);

    if (ok) {
        ESP_LOGI(TAG, "Enrolled device %s status=%d", device_uid_out,
                 status_out != nullptr ? static_cast<int>(*status_out) : -1);
    }

    return ok;
}

Status pollStatus(const char* api_base, const char* enrollment_key, const char* device_uid) {
    if (api_base == nullptr || enrollment_key == nullptr || device_uid == nullptr ||
        enrollment_key[0] == '\0' || device_uid[0] == '\0') {
        return Status::kError;
    }

    char url[HTTP_URL_MAX] = {};
    std::snprintf(url, sizeof(url),
                  "%s/api/device-enrollment/status?enrollment_key=%s&device_uid=%s", api_base,
                  enrollment_key, device_uid);

    HttpResponseBuffer response = {};
    if (!http_request(url, HTTP_METHOD_GET, nullptr, nullptr, &response)) {
        return Status::kError;
    }

    cJSON* root = cJSON_Parse(response.data);
    if (root == nullptr) {
        return Status::kError;
    }

    const cJSON* data = cJSON_GetObjectItem(root, "data");
    if (data == nullptr || !cJSON_IsObject(data)) {
        cJSON_Delete(root);
        return Status::kError;
    }

    const Status status = parse_status_payload(data);
    cJSON_Delete(root);
    return status;
}

bool fetchProvisioning(const char* api_base, const char* enrollment_key, const char* device_uid,
                       heapwatch::device_credentials::Credentials* out) {
    if (api_base == nullptr || enrollment_key == nullptr || device_uid == nullptr ||
        out == nullptr || enrollment_key[0] == '\0' || device_uid[0] == '\0') {
        return false;
    }

    char url[HTTP_URL_MAX] = {};
    std::snprintf(url, sizeof(url), "%s/api/device-enrollment/provisioning/fetch", api_base);

    char body[HTTP_BODY_MAX] = {};
    std::snprintf(body, sizeof(body), "{\"enrollment_key\":\"%s\",\"device_uid\":\"%s\"}",
                  enrollment_key, device_uid);

    HttpResponseBuffer response = {};
    if (!http_request(url, HTTP_METHOD_POST, body, nullptr, &response)) {
        return false;
    }

    cJSON* root = cJSON_Parse(response.data);
    if (root == nullptr) {
        return false;
    }

    const cJSON* data = cJSON_GetObjectItem(root, "data");
    const bool ok = data != nullptr && cJSON_IsObject(data) && fill_credentials_from_fetch(data, out);
    cJSON_Delete(root);

    if (ok) {
        ESP_LOGI(TAG, "Fetched provisioning for %s", out->device_uid);
    }

    return ok;
}

bool confirmProvisioning(const char* api_base, const char* device_api_key) {
    if (api_base == nullptr || device_api_key == nullptr || device_api_key[0] == '\0') {
        return false;
    }

    char url[HTTP_URL_MAX] = {};
    std::snprintf(url, sizeof(url), "%s/api/devices/self/provisioning/confirm", api_base);

    HttpResponseBuffer response = {};
    if (!http_request(url, HTTP_METHOD_POST, "{}", device_api_key, &response)) {
        return false;
    }

    ESP_LOGI(TAG, "Provisioning confirmed");
    return true;
}

bool runUntilProvisioned(const char* api_base, const char* enrollment_key, const char* claimed_mac,
                         const char* build_identifier, const char* build_environment,
                         uint32_t poll_interval_ms,
                         heapwatch::device_credentials::Credentials* out) {
    if (api_base == nullptr || enrollment_key == nullptr || claimed_mac == nullptr ||
        out == nullptr || enrollment_key[0] == '\0' || claimed_mac[0] == '\0') {
        return false;
    }

    char device_uid[HEAPWATCH_MAX_DEVICE_UID_LEN] = {};
    if (!heapwatch::device_credentials::loadPendingDeviceUid(device_uid, sizeof(device_uid))) {
        Status enroll_status = Status::kError;
        if (!enroll(api_base, enrollment_key, claimed_mac, build_identifier, build_environment,
                    device_uid, sizeof(device_uid), &enroll_status)) {
            ESP_LOGE(TAG, "Initial enrollment submit failed");
            return false;
        }

        if (is_terminal_failure(enroll_status)) {
            ESP_LOGE(TAG, "Enrollment terminal status %d for %s", static_cast<int>(enroll_status),
                     device_uid);
            return false;
        }

        if (!heapwatch::device_credentials::savePendingDeviceUid(device_uid)) {
            ESP_LOGW(TAG, "Failed to persist pending device uid %s", device_uid);
        }
    } else {
        ESP_LOGI(TAG, "Resuming enrollment for pending device %s", device_uid);
    }

    while (true) {
        const Status status = pollStatus(api_base, enrollment_key, device_uid);

        if (status == Status::kAcceptedAwaitingProvision) {
            break;
        }
        if (is_terminal_failure(status)) {
            ESP_LOGE(TAG, "Enrollment terminal status %d for %s", static_cast<int>(status),
                     device_uid);
            heapwatch::device_credentials::clearPendingDeviceUid();
            return false;
        }
        if (status == Status::kProvisioned) {
            ESP_LOGE(TAG, "Device %s already provisioned but NVS credentials missing", device_uid);
            heapwatch::device_credentials::clearPendingDeviceUid();
            return false;
        }
        if (status == Status::kError) {
            ESP_LOGW(TAG, "Enrollment status poll error for %s, retrying", device_uid);
        } else if (is_waiting_for_operator(status)) {
            ESP_LOGI(TAG, "Enrollment awaiting operator for %s (%d), waiting %u ms", device_uid,
                     static_cast<int>(status), static_cast<unsigned>(poll_interval_ms));
        } else {
            ESP_LOGI(TAG, "Enrollment status %d for %s, waiting %u ms", static_cast<int>(status),
                     device_uid, static_cast<unsigned>(poll_interval_ms));
        }

        vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
    }

    if (!fetchProvisioning(api_base, enrollment_key, device_uid, out)) {
        ESP_LOGE(TAG, "Provisioning fetch failed for %s", device_uid);
        return false;
    }

    if (!confirmProvisioning(api_base, out->device_api_key)) {
        ESP_LOGE(TAG, "Provisioning confirm failed for %s", device_uid);
        return false;
    }

    return true;
}

}  // namespace heapwatch::device_enrollment
