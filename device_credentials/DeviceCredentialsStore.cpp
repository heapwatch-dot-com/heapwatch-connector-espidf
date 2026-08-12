#include "DeviceCredentialsStore.h"

#include <cstring>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace heapwatch::device_credentials {

namespace {
constexpr char TAG[] = "HeapwatchCreds";
constexpr char NVS_NAMESPACE[] = "heapwatch";
constexpr char KEY_PROVISIONED[] = "provisioned";
constexpr char KEY_TENANT_SLUG[] = "tenant_slug";
constexpr char KEY_DEVICE_UID[] = "device_uid";
constexpr char KEY_MQTT_PASSWORD[] = "mqtt_password";
constexpr char KEY_DEVICE_API_KEY[] = "device_api_key";
constexpr char KEY_STREAM_API_KEY[] = "stream_api_key";
constexpr char KEY_PENDING_UID[] = "pending_uid";
}  // namespace

bool load(Credentials* out) {
    if (out == nullptr) {
        return false;
    }

    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    uint8_t provisioned = 0;
    if (nvs_get_u8(handle, KEY_PROVISIONED, &provisioned) != ESP_OK || provisioned == 0) {
        nvs_close(handle);
        return false;
    }

    size_t len = sizeof(out->tenant_slug);
    if (nvs_get_str(handle, KEY_TENANT_SLUG, out->tenant_slug, &len) != ESP_OK) {
        nvs_close(handle);
        return false;
    }

    len = sizeof(out->device_uid);
    if (nvs_get_str(handle, KEY_DEVICE_UID, out->device_uid, &len) != ESP_OK) {
        nvs_close(handle);
        return false;
    }

    len = sizeof(out->mqtt_password);
    if (nvs_get_str(handle, KEY_MQTT_PASSWORD, out->mqtt_password, &len) != ESP_OK) {
        nvs_close(handle);
        return false;
    }

    len = sizeof(out->device_api_key);
    if (nvs_get_str(handle, KEY_DEVICE_API_KEY, out->device_api_key, &len) != ESP_OK) {
        nvs_close(handle);
        return false;
    }

    out->stream_api_key[0] = '\0';
    len = sizeof(out->stream_api_key);
    nvs_get_str(handle, KEY_STREAM_API_KEY, out->stream_api_key, &len);

    nvs_close(handle);
    return out->tenant_slug[0] != '\0' && out->device_uid[0] != '\0' &&
           out->mqtt_password[0] != '\0';
}

bool save(const Credentials* credentials) {
    if (credentials == nullptr) {
        return false;
    }

    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    esp_err_t err = nvs_set_str(handle, KEY_TENANT_SLUG, credentials->tenant_slug);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_DEVICE_UID, credentials->device_uid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_MQTT_PASSWORD, credentials->mqtt_password);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_DEVICE_API_KEY, credentials->device_api_key);
    }
    if (err == ESP_OK && credentials->stream_api_key[0] != '\0') {
        err = nvs_set_str(handle, KEY_STREAM_API_KEY, credentials->stream_api_key);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_PROVISIONED, 1);
    }
    if (err == ESP_OK) {
        err = nvs_erase_key(handle, KEY_PENDING_UID);
    }
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save credentials: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Saved provisioned credentials for device %s", credentials->device_uid);
    return true;
}

void clear() {
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }

    nvs_erase_key(handle, KEY_PROVISIONED);
    nvs_erase_key(handle, KEY_TENANT_SLUG);
    nvs_erase_key(handle, KEY_DEVICE_UID);
    nvs_erase_key(handle, KEY_MQTT_PASSWORD);
    nvs_erase_key(handle, KEY_DEVICE_API_KEY);
    nvs_erase_key(handle, KEY_STREAM_API_KEY);
    nvs_erase_key(handle, KEY_PENDING_UID);
    nvs_commit(handle);
    nvs_close(handle);
}

bool loadPendingDeviceUid(char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return false;
    }

    out[0] = '\0';

    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    size_t len = out_len;
    const esp_err_t err = nvs_get_str(handle, KEY_PENDING_UID, out, &len);
    nvs_close(handle);

    return err == ESP_OK && out[0] != '\0';
}

bool savePendingDeviceUid(const char* device_uid) {
    if (device_uid == nullptr || device_uid[0] == '\0') {
        return false;
    }

    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    esp_err_t err = nvs_set_str(handle, KEY_PENDING_UID, device_uid);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err == ESP_OK;
}

void clearPendingDeviceUid() {
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }

    nvs_erase_key(handle, KEY_PENDING_UID);
    nvs_commit(handle);
    nvs_close(handle);
}

}  // namespace heapwatch::device_credentials
