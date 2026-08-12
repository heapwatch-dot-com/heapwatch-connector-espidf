#pragma once

#include <cstddef>
#include <cstdint>

#define HEAPWATCH_MAX_TENANT_SLUG_LEN 64
#define HEAPWATCH_MAX_DEVICE_UID_LEN 64
#define HEAPWATCH_MAX_MQTT_PASSWORD_LEN 128
#define HEAPWATCH_MAX_DEVICE_API_KEY_LEN 256
#define HEAPWATCH_MAX_STREAM_API_KEY_LEN 256

namespace heapwatch::device_credentials {

struct Credentials {
    char tenant_slug[HEAPWATCH_MAX_TENANT_SLUG_LEN];
    char device_uid[HEAPWATCH_MAX_DEVICE_UID_LEN];
    char mqtt_password[HEAPWATCH_MAX_MQTT_PASSWORD_LEN];
    char device_api_key[HEAPWATCH_MAX_DEVICE_API_KEY_LEN];
    char stream_api_key[HEAPWATCH_MAX_STREAM_API_KEY_LEN];
};

bool load(Credentials* out);
bool save(const Credentials* credentials);
void clear();

/** Server-minted hw uid persisted between enroll and confirm (provisioned=0). */
bool loadPendingDeviceUid(char* out, size_t out_len);
bool savePendingDeviceUid(const char* device_uid);
void clearPendingDeviceUid();

}  // namespace heapwatch::device_credentials
