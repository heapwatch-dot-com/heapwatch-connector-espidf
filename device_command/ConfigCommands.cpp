#include "ConfigCommands.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"

#include "../connector_settings/ConnectorSettings.h"
#include "DeviceCommandHandler.h"
#include "GoingDarkPipeline.h"

namespace heapwatch::device_command {

namespace {

bool publishWithResult(const char* suffix, const char* command_id, bool ok, const char* code,
                       const char* message, cJSON* result) {
    return publishCommandResponseWithResult(suffix, command_id, ok, code, message, result);
}

bool publishAckThenWithResult(const char* suffix, const char* command_id, bool ok, const char* code,
                              const char* message, cJSON* result) {
    if (!publishCommandAck(suffix, command_id, message)) {
        if (result != nullptr) {
            cJSON_Delete(result);
        }
        return false;
    }

    return publishWithResult(suffix, command_id, ok, code, message, result);
}

bool readNamespace(const cJSON* root, char* out, size_t out_len) {
    const cJSON* ns = cJSON_GetObjectItem(root, "namespace");
    if (!cJSON_IsString(ns) || ns->valuestring == nullptr || ns->valuestring[0] == '\0') {
        return false;
    }
    snprintf(out, out_len, "%s", ns->valuestring);
    return true;
}

bool isWildcardNamespace(const char* nvs_namespace) {
    return nvs_namespace != nullptr &&
           (strcmp(nvs_namespace, "*") == 0 || strcmp(nvs_namespace, "all") == 0);
}

cJSON* buildNamespaceGetResult(const char* nvs_namespace) {
    if (isWildcardNamespace(nvs_namespace)) {
        return heapwatch::connector_settings::buildFullPartitionResponse();
    }

    if (heapwatch::connector_settings::isManagedNamespace(nvs_namespace)) {
        return heapwatch::connector_settings::buildManagedNamespaceResponse(nvs_namespace);
    }

    return heapwatch::connector_settings::buildUnmanagedNamespaceResponse(nvs_namespace);
}

cJSON* buildPostMutationResult(const char* nvs_namespace) {
    if (heapwatch::connector_settings::isManagedNamespace(nvs_namespace)) {
        return heapwatch::connector_settings::buildManagedNamespaceResponse(nvs_namespace);
    }

    return heapwatch::connector_settings::buildUnmanagedNamespaceResponse(nvs_namespace);
}

}  // namespace

void handleConfigGet(const char* suffix, char* command_id, const char* payload, size_t payload_len) {
    cJSON* root = nullptr;
    char namespace_buf[32] = {};

    if (payload_len > 0) {
        root = cJSON_ParseWithLength(payload, static_cast<int>(payload_len));
        if (root == nullptr) {
            publishWithResult(suffix, command_id, false, "bad_args", "Invalid JSON payload", nullptr);
            free(command_id);
            return;
        }

        if (!readNamespace(root, namespace_buf, sizeof(namespace_buf))) {
            cJSON_Delete(root);
            publishWithResult(suffix, command_id, false, "bad_args", "namespace is required", nullptr);
            free(command_id);
            return;
        }
        cJSON_Delete(root);
    } else {
        snprintf(namespace_buf, sizeof(namespace_buf), "*");
    }

    cJSON* result = buildNamespaceGetResult(namespace_buf);
    if (result == nullptr) {
        publishWithResult(suffix, command_id, false, "bad_args", "Unsupported namespace", nullptr);
        free(command_id);
        return;
    }

    publishAckThenWithResult(suffix, command_id, true, "", "config loaded", result);
    free(command_id);
}

void handleConfigSet(const char* suffix, char* command_id, const char* payload, size_t payload_len) {
    cJSON* root = cJSON_ParseWithLength(payload, static_cast<int>(payload_len));
    if (root == nullptr) {
        publishWithResult(suffix, command_id, false, "bad_args", "Invalid JSON payload", nullptr);
        free(command_id);
        return;
    }

    char namespace_buf[32] = {};
    if (!readNamespace(root, namespace_buf, sizeof(namespace_buf))) {
        cJSON_Delete(root);
        publishWithResult(suffix, command_id, false, "bad_args", "namespace is required", nullptr);
        free(command_id);
        return;
    }

    if (!heapwatch::connector_settings::isManagedNamespace(namespace_buf)) {
        cJSON_Delete(root);
        publishWithResult(suffix, command_id, false, "bad_args", "namespace not managed", nullptr);
        free(command_id);
        return;
    }

    const cJSON* values = cJSON_GetObjectItem(root, "values");
    if (!cJSON_IsObject(values)) {
        cJSON_Delete(root);
        publishWithResult(suffix, command_id, false, "bad_args", "values object is required", nullptr);
        free(command_id);
        return;
    }

    const cJSON* child = values->child;
    while (child != nullptr) {
        if (child->string == nullptr) {
            cJSON_Delete(root);
            publishWithResult(suffix, command_id, false, "bad_args", "invalid values key", nullptr);
            free(command_id);
            return;
        }
        if (!cJSON_IsNumber(child) && !cJSON_IsBool(child)) {
            char err[96] = {};
            snprintf(err, sizeof(err), "invalid value for %s", child->string);
            cJSON_Delete(root);
            publishWithResult(suffix, command_id, false, "bad_args", err, nullptr);
            free(command_id);
            return;
        }

        char set_err[96] = {};
        if (!heapwatch::connector_settings::setValue(namespace_buf, child->string, child, set_err,
                                                     sizeof(set_err))) {
            cJSON_Delete(root);
            publishWithResult(suffix, command_id, false, "bad_args", set_err, nullptr);
            free(command_id);
            return;
        }

        child = child->next;
    }
    cJSON_Delete(root);

    cJSON* result = buildPostMutationResult(namespace_buf);
    publishAckThenWithResult(suffix, command_id, true, "", "config set", result);
    free(command_id);
}

void handleConfigPersist(const char* suffix, char* command_id, const char* payload,
                         size_t payload_len) {
    cJSON* root = cJSON_ParseWithLength(payload, static_cast<int>(payload_len));
    if (root == nullptr) {
        publishWithResult(suffix, command_id, false, "bad_args", "Invalid JSON payload", nullptr);
        free(command_id);
        return;
    }

    char namespace_buf[32] = {};
    if (!readNamespace(root, namespace_buf, sizeof(namespace_buf))) {
        cJSON_Delete(root);
        publishWithResult(suffix, command_id, false, "bad_args", "namespace is required", nullptr);
        free(command_id);
        return;
    }

    const cJSON* values = cJSON_GetObjectItem(root, "values");
    const cJSON* keys = cJSON_GetObjectItem(root, "keys");

    char persist_err[96] = {};
    bool ok = false;

    if (heapwatch::connector_settings::isManagedNamespace(namespace_buf)) {
        if (cJSON_IsObject(values)) {
            ok = heapwatch::connector_settings::persistManagedValues(namespace_buf, values, persist_err,
                                                                     sizeof(persist_err));
        } else if (cJSON_IsArray(keys)) {
            cJSON* key_map = cJSON_CreateObject();
            const int count = cJSON_GetArraySize(keys);
            for (int i = 0; i < count; ++i) {
                const cJSON* item = cJSON_GetArrayItem(keys, i);
                if (cJSON_IsString(item) && item->valuestring != nullptr) {
                    cJSON_AddTrueToObject(key_map, item->valuestring);
                }
            }
            ok = heapwatch::connector_settings::persistManagedValues(namespace_buf, key_map,
                                                                     persist_err, sizeof(persist_err));
            cJSON_Delete(key_map);
        } else {
            ok = heapwatch::connector_settings::persistManagedValues(namespace_buf, nullptr, persist_err,
                                                                     sizeof(persist_err));
        }
    } else {
        if (!cJSON_IsObject(values)) {
            cJSON_Delete(root);
            publishWithResult(suffix, command_id, false, "bad_args", "values object is required",
                              nullptr);
            free(command_id);
            return;
        }
        ok = heapwatch::connector_settings::persistUnmanagedValues(namespace_buf, values, persist_err,
                                                                   sizeof(persist_err));
    }

    cJSON_Delete(root);

    if (!ok) {
        publishWithResult(suffix, command_id, false, "bad_args", persist_err, nullptr);
        free(command_id);
        return;
    }

    cJSON* result = buildPostMutationResult(namespace_buf);
    publishAckThenWithResult(suffix, command_id, true, "", "config persisted", result);
    free(command_id);
}

void handleConfigRevert(const char* suffix, char* command_id, const char* payload,
                        size_t payload_len) {
    cJSON* root = cJSON_ParseWithLength(payload, static_cast<int>(payload_len));
    if (root == nullptr) {
        publishWithResult(suffix, command_id, false, "bad_args", "Invalid JSON payload", nullptr);
        free(command_id);
        return;
    }

    char namespace_buf[32] = {};
    if (!readNamespace(root, namespace_buf, sizeof(namespace_buf))) {
        cJSON_Delete(root);
        publishWithResult(suffix, command_id, false, "bad_args", "namespace is required", nullptr);
        free(command_id);
        return;
    }

    if (!heapwatch::connector_settings::isManagedNamespace(namespace_buf)) {
        cJSON_Delete(root);
        publishWithResult(suffix, command_id, false, "bad_args", "namespace not managed", nullptr);
        free(command_id);
        return;
    }

    const cJSON* keys = cJSON_GetObjectItem(root, "keys");
    char revert_err[96] = {};
    const bool ok = heapwatch::connector_settings::revertManagedValues(
        namespace_buf, cJSON_IsArray(keys) ? keys : nullptr, revert_err, sizeof(revert_err));
    cJSON_Delete(root);

    if (!ok) {
        publishWithResult(suffix, command_id, false, "bad_args", revert_err, nullptr);
        free(command_id);
        return;
    }

    cJSON* result = buildPostMutationResult(namespace_buf);
    publishAckThenWithResult(suffix, command_id, true, "", "config reverted", result);
    free(command_id);
}

void handleWifiReconnect(const char* suffix, char* command_id, const char* payload,
                         size_t payload_len) {
    uint32_t duration_s = 0;

    if (payload_len > 0) {
        cJSON* root = cJSON_ParseWithLength(payload, static_cast<int>(payload_len));
        if (root != nullptr) {
            const cJSON* duration = cJSON_GetObjectItem(root, "duration_s");
            if (cJSON_IsNumber(duration)) {
                if (duration->valuedouble < 0 || duration->valuedouble > 300) {
                    cJSON_Delete(root);
                    publishWithResult(suffix, command_id, false, "bad_args",
                                      "duration_s must be 0-300", nullptr);
                    free(command_id);
                    return;
                }
                duration_s = static_cast<uint32_t>(duration->valuedouble);
            }
            cJSON_Delete(root);
        }
    }

#if CONFIG_HEAPWATCH_MANAGE_WIFI
    if (!heapwatch::going_dark::scheduleWifiReconnect(suffix, command_id, duration_s)) {
        publishWithResult(suffix, command_id, false, "busy",
                          "Going-dark pipeline busy or unavailable", nullptr);
        free(command_id);
        return;
    }
    free(command_id);
#else
    (void)duration_s;
    publishWithResult(suffix, command_id, false, "not_permitted",
                      "Wi-Fi is not managed by the connector", nullptr);
    free(command_id);
#endif
}

}  // namespace heapwatch::device_command
