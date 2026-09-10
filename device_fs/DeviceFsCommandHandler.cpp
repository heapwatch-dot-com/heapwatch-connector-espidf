#include "DeviceFsCommandHandler.h"

#include <cstring>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "../device_command/DeviceCommandHandler.h"
#include "../stream_ingress/StreamIngressClient.h"
#include "DeviceFsService.h"

namespace heapwatch::device_fs {

namespace {

constexpr char TAG[] = "HwFs";

constexpr size_t kWorkQueueDepth = 4;
constexpr TickType_t kUploadArmTimeoutTicks = pdMS_TO_TICKS(5000);

enum class WorkKind : uint8_t {
    Storage,
    List,
    Read,
    Delete,
    Rename,
    Move,
    Mkdir,
    Create,
    Upload,
    Download,
};

struct WorkItem {
    WorkKind kind = WorkKind::Storage;
    char command_id[kFsCommandIdMax] = {};
    char path[kFsPathMax] = {};
    char path2[kFsPathMax] = {};
    char sha256[kFsSha256HexMax] = {};
    int page = 1;
    int per_page = 25;
    int from_line = 1;
    int to_line = 1;
    bool replace = false;
    int64_t size = 0;
};

QueueHandle_t g_work_queue = nullptr;
SemaphoreHandle_t g_upload_arm_done = nullptr;
bool g_upload_arm_ok = false;
FsError g_upload_arm_error = {};

bool readStringField(cJSON* root, const char* key, char* out, size_t out_len) {
    const cJSON* item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsString(item) || item->valuestring == nullptr || item->valuestring[0] == '\0') {
        return false;
    }
    snprintf(out, out_len, "%s", item->valuestring);
    return true;
}

bool readIntField(cJSON* root, const char* key, int* out) {
    const cJSON* item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *out = item->valueint;
    return true;
}

bool readBoolField(cJSON* root, const char* key, bool* out) {
    const cJSON* item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsBool(item)) {
        return false;
    }
    *out = cJSON_IsTrue(item);
    return true;
}

bool readInt64Field(cJSON* root, const char* key, int64_t* out) {
    const cJSON* item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *out = static_cast<int64_t>(item->valuedouble);
    return true;
}

void processWorkItem(const WorkItem& work) {
    FsError error = {};

    switch (work.kind) {
        case WorkKind::Storage: {
            cJSON* result = buildStorageResult();
            sendTcpResult(work.command_id, true, nullptr, result);
            break;
        }
        case WorkKind::List: {
            cJSON* result = buildListResult(work.path, work.page, work.per_page, &error);
            if (result == nullptr) {
                sendTcpResult(work.command_id, false, &error, nullptr);
            } else {
                sendTcpResult(work.command_id, true, nullptr, result);
            }
            break;
        }
        case WorkKind::Read: {
            cJSON* result = buildReadResult(work.path, work.from_line, work.to_line, &error);
            if (result == nullptr) {
                sendTcpResult(work.command_id, false, &error, nullptr);
            } else {
                sendTcpResult(work.command_id, true, nullptr, result);
            }
            break;
        }
        case WorkKind::Delete:
            if (deleteEntry(work.path, &error)) {
                sendTcpResult(work.command_id, true, nullptr, nullptr);
            } else {
                sendTcpResult(work.command_id, false, &error, nullptr);
            }
            break;
        case WorkKind::Rename:
            if (renameEntry(work.path, work.path2, &error)) {
                sendTcpResult(work.command_id, true, nullptr, nullptr);
            } else {
                sendTcpResult(work.command_id, false, &error, nullptr);
            }
            break;
        case WorkKind::Move:
            if (moveEntry(work.path, work.path2, &error)) {
                sendTcpResult(work.command_id, true, nullptr, nullptr);
            } else {
                sendTcpResult(work.command_id, false, &error, nullptr);
            }
            break;
        case WorkKind::Mkdir:
            if (mkdirEntry(work.path, &error)) {
                sendTcpResult(work.command_id, true, nullptr, nullptr);
            } else {
                sendTcpResult(work.command_id, false, &error, nullptr);
            }
            break;
        case WorkKind::Create:
            if (createEmptyFile(work.path, &error)) {
                sendTcpResult(work.command_id, true, nullptr, nullptr);
            } else {
                sendTcpResult(work.command_id, false, &error, nullptr);
            }
            break;
        case WorkKind::Upload: {
            // SPIFFS FILE* must live on hw_stream_ingress (same task as handleUploadChunk).
            // MQTT waits on g_upload_arm_done before ack so Laravel won't push bytes early.
            g_upload_arm_ok = beginUploadJob(work.command_id, work.path, work.path2, work.replace,
                                             work.sha256, work.size, &g_upload_arm_error);
            if (g_upload_arm_done != nullptr) {
                xSemaphoreGive(g_upload_arm_done);
            }
            break;
        }
        case WorkKind::Download:
            if (!beginDownloadJob(work.command_id, work.path, &error)) {
                sendTcpResult(work.command_id, false, &error, nullptr);
            }
            break;
    }
}

bool enqueueWork(const WorkItem& work) {
    if (g_work_queue == nullptr) {
        return false;
    }
    return xQueueSend(g_work_queue, &work, 0) == pdTRUE;
}

bool mqttReject(const char* suffix, const char* command_id, const char* code, const char* message) {
    return heapwatch::device_command::publishCommandResponse(suffix, command_id, false, code, message);
}

bool mqttAccept(const char* suffix, const char* command_id) {
    return heapwatch::device_command::publishCommandResponse(suffix, command_id, true, "", "accepted");
}

bool ensureIngressReady(const char* suffix, const char* command_id) {
    if (!heapwatch::stream_ingress::ensureTcpSession()) {
        mqttReject(suffix, command_id, "stream_unavailable", "Stream ingress is not configured.");
        return false;
    }
    return true;
}

bool ensureWorkQueue() {
    if (g_work_queue == nullptr) {
        g_work_queue = xQueueCreate(kWorkQueueDepth, sizeof(WorkItem));
    }
    if (g_upload_arm_done == nullptr) {
        g_upload_arm_done = xSemaphoreCreateBinary();
    }
    return g_work_queue != nullptr && g_upload_arm_done != nullptr;
}

bool armUploadAndWait(const WorkItem& work, FsError* error) {
    // Clear a stale give from a prior timed-out arm.
    (void)xSemaphoreTake(g_upload_arm_done, 0);

    g_upload_arm_ok = false;
    g_upload_arm_error = {};

    if (!enqueueWork(work)) {
        if (error != nullptr) {
            error->code = "busy";
            error->message = "Filesystem worker queue is full.";
        }
        return false;
    }

    if (xSemaphoreTake(g_upload_arm_done, kUploadArmTimeoutTicks) != pdTRUE) {
        if (error != nullptr) {
            error->code = "busy";
            error->message = "Timed out arming filesystem upload.";
        }
        return false;
    }

    if (!g_upload_arm_ok) {
        if (error != nullptr) {
            *error = g_upload_arm_error;
        }
        return false;
    }

    return true;
}

}  // namespace

void init() {
    (void)ensureWorkQueue();
    heapwatch::stream_ingress::setFsUploadChunkHandler(handleUploadChunk);
}

void pollPendingWork() {
    if (g_work_queue == nullptr) {
        return;
    }
    WorkItem work = {};
    if (xQueueReceive(g_work_queue, &work, 0) != pdTRUE) {
        return;
    }
    processWorkItem(work);
    heapwatch::stream_ingress::flushOutbound();
}

bool handleMqttCommand(const char* suffix, const char* command_id, const char* payload,
                       size_t payload_len) {
    if (suffix == nullptr || strncmp(suffix, "fs/", 3) != 0) {
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(payload, static_cast<int>(payload_len));
    if (root == nullptr) {
        mqttReject(suffix, command_id, "invalid_payload", "Filesystem command payload is not valid JSON.");
        return true;
    }

    WorkItem work = {};
    snprintf(work.command_id, sizeof(work.command_id), "%s", command_id != nullptr ? command_id : "");

    const char* op = suffix + 3;
    bool valid = true;

    if (strcmp(op, "storage") == 0) {
        work.kind = WorkKind::Storage;
    } else if (strcmp(op, "list") == 0) {
        work.kind = WorkKind::List;
        valid = readStringField(root, "path", work.path, sizeof(work.path));
        if (valid) {
            int page = 1;
            int per_page = 25;
            readIntField(root, "page", &page);
            readIntField(root, "per_page", &per_page);
            work.page = page;
            work.per_page = per_page;
        }
    } else if (strcmp(op, "read") == 0) {
        work.kind = WorkKind::Read;
        valid = readStringField(root, "path", work.path, sizeof(work.path)) &&
                readIntField(root, "from_line", &work.from_line) &&
                readIntField(root, "to_line", &work.to_line);
    } else if (strcmp(op, "delete") == 0) {
        work.kind = WorkKind::Delete;
        valid = readStringField(root, "path", work.path, sizeof(work.path));
    } else if (strcmp(op, "rename") == 0) {
        work.kind = WorkKind::Rename;
        valid = readStringField(root, "path", work.path, sizeof(work.path)) &&
                readStringField(root, "new_name", work.path2, sizeof(work.path2));
    } else if (strcmp(op, "move") == 0) {
        work.kind = WorkKind::Move;
        valid = readStringField(root, "path", work.path, sizeof(work.path)) &&
                readStringField(root, "destination_path", work.path2, sizeof(work.path2));
    } else if (strcmp(op, "mkdir") == 0) {
        work.kind = WorkKind::Mkdir;
        valid = readStringField(root, "path", work.path, sizeof(work.path));
    } else if (strcmp(op, "create") == 0) {
        work.kind = WorkKind::Create;
        valid = readStringField(root, "path", work.path, sizeof(work.path));
    } else if (strcmp(op, "upload") == 0) {
        work.kind = WorkKind::Upload;
        valid = readStringField(root, "path", work.path, sizeof(work.path)) &&
                readStringField(root, "temp_path", work.path2, sizeof(work.path2)) &&
                readStringField(root, "sha256", work.sha256, sizeof(work.sha256)) &&
                readInt64Field(root, "size", &work.size);
        if (valid) {
            bool replace = false;
            readBoolField(root, "replace", &replace);
            work.replace = replace;
        }
    } else if (strcmp(op, "download") == 0) {
        work.kind = WorkKind::Download;
        valid = readStringField(root, "path", work.path, sizeof(work.path));
    } else {
        cJSON_Delete(root);
        mqttReject(suffix, command_id, "unsupported", "Filesystem command is not supported.");
        return true;
    }

    cJSON_Delete(root);

    if (!valid) {
        mqttReject(suffix, command_id, "invalid_args", "Filesystem command arguments are invalid.");
        return true;
    }

    if (!ensureWorkQueue()) {
        mqttReject(suffix, command_id, "busy", "Filesystem work queue unavailable.");
        return true;
    }

    if (!ensureIngressReady(suffix, command_id)) {
        return true;
    }

    // Upload: arm on stream_ingress BEFORE MQTT ack (Laravel waits for ack, then pushes bytes).
    // Do not fopen/fwrite on mqtt_task — SPIFFS is not safe across tasks with a shared FILE*.
    if (work.kind == WorkKind::Upload) {
        FsError error = {};
        if (!armUploadAndWait(work, &error)) {
            mqttReject(suffix, command_id, error.code != nullptr ? error.code : "io_error",
                       error.message != nullptr ? error.message : "Failed to arm upload.");
            return true;
        }

        if (!mqttAccept(suffix, command_id)) {
            ESP_LOGW(TAG, "Failed to publish MQTT ack for %s after arming upload", suffix);
            cancelActiveUpload();
        }
        return true;
    }

    if (!mqttAccept(suffix, command_id)) {
        ESP_LOGW(TAG, "Failed to publish MQTT ack for %s", suffix);
        return true;
    }

    if (!enqueueWork(work)) {
        FsError err = {"busy", "Filesystem worker queue is full."};
        sendTcpResult(work.command_id, false, &err, nullptr);
    }

    return true;
}

}  // namespace heapwatch::device_fs
