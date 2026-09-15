#include "heapwatch/Fs/DeviceFilesystemOperations.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cinttypes>

#include "esp_log.h"
#include "mbedtls/sha256.h"

#include "heapwatch/StreamIngress/StreamIngressClient.h"
#include "heapwatch/Fs/Volumes/FsVolumeManager.h"

namespace Heapwatch::Fs {

namespace {

constexpr char TAG[] = "HwFs";

constexpr size_t DOWNLOAD_CHUNK_BYTES = 4096;

// Shared by sha256File / download — lives in BSS, not on the 6KiB stream-ingress stack.
// Only called from hw_stream_ingress (single-threaded FS work).
uint8_t g_io_buf[DOWNLOAD_CHUNK_BYTES] = {};

struct ActiveUpload {
    bool active = false;
    char command_id[FS_COMMAND_ID_MAX] = {};
    char target_path[FS_PATH_MAX] = {};
    char temp_path[FS_PATH_MAX] = {};
    char expected_sha256[FS_SHA256_HEX_MAX] = {};
    bool replace = false;
    int64_t expected_size = 0;
    int64_t received_size = 0;
    FILE* file = nullptr;
};

ActiveUpload g_upload = {};
bool g_logged_inactive_upload_drop = false;

void cleanupOrphanUploadTemps(const char* mount) {
    if (mount == nullptr) {
        return;
    }
    DIR* dir = opendir(mount);
    if (dir == nullptr) {
        return;
    }

    int32_t removed = 0;
    while (dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '\0') {
            continue;
        }
        // Matcher: .hw-<id>.tmp (legacy) or hwXXXXXX.tmp (FatFS 8.3-safe).
        const size_t name_len = strlen(entry->d_name);
        const bool legacy_spiffs_temp =
            name_len >= 9 && strncmp(entry->d_name, ".hw-", 4) == 0 &&
            strcmp(entry->d_name + name_len - 4, ".tmp") == 0;
        const bool fat_safe_temp = name_len >= 8 && strncmp(entry->d_name, "hw", 2) == 0 &&
                                   strcmp(entry->d_name + name_len - 4, ".tmp") == 0;
        if (!legacy_spiffs_temp && !fat_safe_temp) {
            continue;
        }

        char full[FS_PATH_MAX] = {};
        const int32_t written = snprintf(full, sizeof(full), "%s/%s", mount, entry->d_name);
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(full)) {
            continue;
        }
        if (unlink(full) == 0) {
            ++removed;
            ESP_LOGW(TAG, "Removed orphan upload temp %s", full);
        }
    }
    closedir(dir);

    if (removed > 0) {
        ESP_LOGW(TAG, "Cleaned %" PRId32 " orphan upload temp(s) under %s", removed, mount);
    }
}

bool volumeHasSpaceForUpload(const char* path, int64_t needed, FsError* error) {
    if (path == nullptr || needed <= 0) {
        if (error != nullptr) {
            error->code = "invalid_args";
            error->message = "Upload requires a positive size.";
        }
        return false;
    }

    int64_t free_bytes = -1;

    const Heapwatch::Fs::Volumes::Volume* flash_vol = Heapwatch::Fs::Volumes::findByPath(path);
    if (flash_vol != nullptr) {
        size_t total = 0;
        size_t used = 0;
        if (!Heapwatch::Fs::Volumes::queryUsage(*flash_vol, &total, &used)) {
            if (error != nullptr) {
                error->code = "io_error";
                error->message = "Failed to query filesystem free space.";
            }
            return false;
        }
        free_bytes = static_cast<int64_t>(total) - static_cast<int64_t>(used);
    } else {
        // Unknown / unmounted volume — let fopen fail rather than guessing.
        return true;
    }

    // Temp and target coexist until finalize — do not count reclaim of the old file here.
    if (free_bytes < needed) {
        if (error != nullptr) {
            error->code = "no_space";
            error->message = "Not enough free space on the volume for this upload.";
        }
        ESP_LOGW(TAG, "Upload rejected: need %" PRId64 " bytes, free %" PRId64,
                 static_cast<int64_t>(needed), static_cast<int64_t>(free_bytes));
        return false;
    }

    return true;
}

bool hexEncodeSha256(const uint8_t digest[32], char* out, size_t out_len) {
    if (out == nullptr || out_len < FS_SHA256_HEX_MAX) {
        return false;
    }

    static const char* HEX_DIGITS = "0123456789abcdef";
    for (int32_t i = 0; i < 32; i++) {
        out[i * 2] = HEX_DIGITS[(digest[i] >> 4) & 0x0f];
        out[i * 2 + 1] = HEX_DIGITS[digest[i] & 0x0f];
    }
    out[64] = '\0';
    return true;
}

bool sha256File(const char* path, char* out_hex, size_t out_hex_len, int64_t* out_size) {
    FILE* fp = fopen(path, "rb");
    if (fp == nullptr) {
        return false;
    }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);

    int64_t total = 0;
    size_t read = 0;
    while ((read = fread(g_io_buf, 1, sizeof(g_io_buf), fp)) > 0) {
        mbedtls_sha256_update(&ctx, g_io_buf, read);
        total += static_cast<int64_t>(read);
    }

    fclose(fp);

    uint8_t digest[32] = {};
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);

    if (out_size != nullptr) {
        *out_size = total;
    }

    return hexEncodeSha256(digest, out_hex, out_hex_len);
}

void resetUploadState() {
    if (g_upload.file != nullptr) {
        fclose(g_upload.file);
        g_upload.file = nullptr;
    }
    g_upload = {};
}

void finalizeUpload() {
    if (!g_upload.active) {
        return;
    }

    char command_id[FS_COMMAND_ID_MAX] = {};
    snprintf(command_id, sizeof(command_id), "%s", g_upload.command_id);
    const bool replace = g_upload.replace;
    char target_path[FS_PATH_MAX] = {};
    char temp_path[FS_PATH_MAX] = {};
    snprintf(target_path, sizeof(target_path), "%s", g_upload.target_path);
    snprintf(temp_path, sizeof(temp_path), "%s", g_upload.temp_path);

    if (g_upload.file != nullptr) {
        fclose(g_upload.file);
        g_upload.file = nullptr;
    }

    if (g_upload.received_size != g_upload.expected_size) {
        FsError err = {"size_mismatch", "Uploaded byte count does not match expected size."};
        unlink(temp_path);
        resetUploadState();
        sendFileBrowserResultOverStream(command_id, false, &err, nullptr);
        return;
    }

    char actual_sha256[FS_SHA256_HEX_MAX] = {};
    int64_t actual_size = 0;
    if (!sha256File(temp_path, actual_sha256, sizeof(actual_sha256), &actual_size)) {
        FsError err = {"io_error", "Failed to hash uploaded temp file."};
        unlink(temp_path);
        resetUploadState();
        sendFileBrowserResultOverStream(command_id, false, &err, nullptr);
        return;
    }

    if (strcasecmp(actual_sha256, g_upload.expected_sha256) != 0) {
        ESP_LOGE(TAG, "checksum mismatch path=%s expected=%s actual=%s size=%" PRId64, temp_path,
                 g_upload.expected_sha256, actual_sha256,
                 static_cast<int64_t>(actual_size));
        FsError err = {"checksum_mismatch", "SHA256 checksum mismatch on uploaded file."};
        unlink(temp_path);
        resetUploadState();
        sendFileBrowserResultOverStream(command_id, false, &err, nullptr);
        return;
    }

    if (replace) {
        unlink(target_path);
    }

    if (rename(temp_path, target_path) != 0) {
        const int rename_errno = errno;
        ESP_LOGE(TAG, "rename(%s -> %s) failed errno=%d (%s)", temp_path, target_path,
                 rename_errno, strerror(rename_errno));
        static char rename_msg[160];
        if (rename_errno == EINVAL) {
            snprintf(rename_msg, sizeof(rename_msg),
                     "Target filename is not valid on this volume (errno=%d). "
                     "FatFS without long names only allows 8.3 names.",
                     rename_errno);
        } else {
            snprintf(rename_msg, sizeof(rename_msg),
                     "Failed to rename temp file to target path (errno=%d).", rename_errno);
        }
        FsError err = {"io_error", rename_msg};
        unlink(temp_path);
        resetUploadState();
        sendFileBrowserResultOverStream(command_id, false, &err, nullptr);
        return;
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "complete");
    cJSON_AddStringToObject(result, "path", target_path);
    cJSON_AddStringToObject(result, "sha256", actual_sha256);
    cJSON_AddNumberToObject(result, "size", static_cast<double>(actual_size));

    resetUploadState();
    sendFileBrowserResultOverStream(command_id, true, nullptr, result);
}

void downloadOnStreamTask(const char* command_id, const char* path) {
    FILE* fp = fopen(path, "rb");
    if (fp == nullptr) {
        FsError err = {"not_found", "File not found for download."};
        sendFileBrowserResultOverStream(command_id, false, &err, nullptr);
        return;
    }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);

    int64_t total_size = 0;
    size_t read = 0;
    while ((read = fread(g_io_buf, 1, sizeof(g_io_buf), fp)) > 0) {
        if (!Heapwatch::StreamIngress::sendFsDownloadChunk(command_id, g_io_buf, read)) {
            FsError err = {"io_error", "Failed to stream download chunk to ingress."};
            fclose(fp);
            mbedtls_sha256_free(&ctx);
            sendFileBrowserResultOverStream(command_id, false, &err, nullptr);
            return;
        }
        // Same task as TCP writer — push bytes out before next fread.
        Heapwatch::StreamIngress::flushOutbound();
        mbedtls_sha256_update(&ctx, g_io_buf, read);
        total_size += static_cast<int64_t>(read);
    }

    fclose(fp);

    uint8_t digest[32] = {};
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);

    char sha_hex[FS_SHA256_HEX_MAX] = {};
    hexEncodeSha256(digest, sha_hex, sizeof(sha_hex));

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "complete");
    cJSON_AddStringToObject(result, "sha256", sha_hex);
    cJSON_AddNumberToObject(result, "size", static_cast<double>(total_size));

    sendFileBrowserResultOverStream(command_id, true, nullptr, result);
    Heapwatch::StreamIngress::flushOutbound();
}

}  // namespace

void sendFileBrowserResultOverStream(const char* command_id, bool ok, const FsError* error, cJSON* result) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "command_id", command_id != nullptr ? command_id : "");
    cJSON_AddBoolToObject(root, "ok", ok);

    if (error != nullptr && !ok) {
        cJSON* err_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(err_obj, "code", error->code);
        cJSON_AddStringToObject(err_obj, "message", error->message);
        cJSON_AddItemToObject(root, "error", err_obj);
        cJSON_AddNullToObject(root, "result");
    } else {
        cJSON_AddNullToObject(root, "error");
        if (result != nullptr) {
            cJSON_AddItemToObject(root, "result", result);
        } else {
            cJSON_AddNullToObject(root, "result");
        }
    }

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json == nullptr) {
        return;
    }

    if (!Heapwatch::StreamIngress::sendFileBrowserJson(json)) {
        ESP_LOGW(TAG, "Failed to queue file_browser result for command %s",
                 command_id != nullptr ? command_id : "");
    }

    cJSON_free(json);
}

bool beginUploadJob(const char* command_id, const char* path, const char* temp_path, bool replace,
                    const char* expected_sha256, int64_t expected_size, FsError* error) {
    // Interactive FS is now-or-never: a new upload command supersedes any stuck prior job
    // (e.g. bytes never arrived after MQTT arm). Without this the device stays busy forever.
    if (g_upload.active) {
        char old_command_id[FS_COMMAND_ID_MAX] = {};
        snprintf(old_command_id, sizeof(old_command_id), "%s", g_upload.command_id);
        ESP_LOGW(TAG, "Superseding active upload %s with %s", old_command_id,
                 command_id != nullptr ? command_id : "");
        cancelActiveUpload();
        FsError cancelled = {"cancelled", "Upload superseded by a newer filesystem upload."};
        sendFileBrowserResultOverStream(old_command_id, false, &cancelled, nullptr);
    }

    if (!isAllowedPath(path) || !isAllowedPath(temp_path)) {
        if (error != nullptr) {
            error->code = "invalid_path";
            error->message = "Upload path is not under an allowed mount.";
        }
        return false;
    }

    if (expected_sha256 == nullptr || strlen(expected_sha256) != 64 || expected_size <= 0) {
        if (error != nullptr) {
            error->code = "invalid_args";
            error->message = "Upload requires sha256 and positive size.";
        }
        return false;
    }

    if (!replace) {
        FILE* existing = fopen(path, "rb");
        if (existing != nullptr) {
            fclose(existing);
            if (error != nullptr) {
                error->code = "already_exists";
                error->message = "Target file already exists; use Replace File.";
            }
            return false;
        }
    }

    const Heapwatch::Fs::Volumes::Volume* target_vol = Heapwatch::Fs::Volumes::findByPath(path);
    if (target_vol != nullptr) {
        cleanupOrphanUploadTemps(target_vol->mount_path);
    }

    if (!volumeHasSpaceForUpload(path, expected_size, error)) {
        // Replace on a nearly-full volume: temp + target cannot coexist. Unlink the
        // existing file first so the new content can be written (atomic replace is
        // impossible without spare space).
        const bool no_space = error != nullptr && error->code != nullptr &&
                              strcmp(error->code, "no_space") == 0;
        if (!replace || !no_space) {
            return false;
        }
        struct stat st = {};
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            return false;
        }
        if (unlink(path) != 0) {
            if (error != nullptr) {
                error->code = "io_error";
                error->message = "Failed to remove existing file before replace upload.";
            }
            return false;
        }
        ESP_LOGW(TAG, "Unlinked %s before replace upload to reclaim space", path);
        if (!volumeHasSpaceForUpload(path, expected_size, error)) {
            return false;
        }
    }

    // Drop a stale temp left by a previous failed job (same random path is rare;
    // still safe if the path happens to collide).
    unlink(temp_path);

    FILE* fp = fopen(temp_path, "wb");
    if (fp == nullptr) {
        const int open_errno = errno;
        ESP_LOGE(TAG, "fopen(%s) failed errno=%d (%s)", temp_path, open_errno,
                 strerror(open_errno));
        if (error != nullptr) {
            error->code = open_errno == ENOSPC ? "no_space" : "io_error";
            if (open_errno == ENOSPC) {
                error->message = "Not enough free space on the volume for this upload.";
            } else {
                static char fopen_msg[96];
                snprintf(fopen_msg, sizeof(fopen_msg), "Failed to open temp upload path (errno=%d).",
                         open_errno);
                error->message = fopen_msg;
            }
        }
        return false;
    }

    g_upload.active = true;
    g_logged_inactive_upload_drop = false;
    snprintf(g_upload.command_id, sizeof(g_upload.command_id), "%s", command_id);
    snprintf(g_upload.target_path, sizeof(g_upload.target_path), "%s", path);
    snprintf(g_upload.temp_path, sizeof(g_upload.temp_path), "%s", temp_path);
    snprintf(g_upload.expected_sha256, sizeof(g_upload.expected_sha256), "%s", expected_sha256);
    g_upload.replace = replace;
    g_upload.expected_size = expected_size;
    g_upload.received_size = 0;
    g_upload.file = fp;

    ESP_LOGI(TAG, "Upload job armed command_id=%s temp=%s size=%" PRId64, command_id, temp_path,
             static_cast<int64_t>(expected_size));
    return true;
}

void handleUploadChunk(const uint8_t* data, size_t len) {
    if (!g_upload.active || g_upload.file == nullptr || data == nullptr || len == 0) {
        if (!g_logged_inactive_upload_drop) {
            ESP_LOGW(TAG, "Dropping upload chunk (%" PRIu32 " bytes) — no active upload armed",
                     static_cast<uint32_t>(len));
            g_logged_inactive_upload_drop = true;
        }
        return;
    }

    const size_t written = fwrite(data, 1, len, g_upload.file);
    if (written != len) {
        const bool no_space = errno == ENOSPC;
        FsError err = {
            no_space ? "no_space" : "io_error",
            no_space ? "Not enough free space on the volume for this upload."
                     : "Failed to write upload chunk.",
        };
        char command_id[FS_COMMAND_ID_MAX] = {};
        snprintf(command_id, sizeof(command_id), "%s", g_upload.command_id);
        unlink(g_upload.temp_path);
        resetUploadState();
        g_logged_inactive_upload_drop = false;
        sendFileBrowserResultOverStream(command_id, false, &err, nullptr);
        return;
    }

    g_upload.received_size += static_cast<int64_t>(written);

    if (g_upload.received_size >= g_upload.expected_size) {
        finalizeUpload();
    }
}

void cancelActiveUpload() {
    if (!g_upload.active) {
        return;
    }
    unlink(g_upload.temp_path);
    resetUploadState();
}

bool beginDownloadJob(const char* command_id, const char* path, FsError* error) {
    if (!isAllowedPath(path)) {
        if (error != nullptr) {
            error->code = "invalid_path";
            error->message = "Path is not under an allowed mount.";
        }
        return false;
    }

    struct stat st = {};
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) {
        if (error != nullptr) {
            error->code = "not_found";
            error->message = "Download path is not a readable file.";
        }
        return false;
    }

    // Runs on hw_stream_ingress via pollPendingWork — no extra task.
    downloadOnStreamTask(command_id, path);
    return true;
}

}  // namespace Heapwatch::Fs
