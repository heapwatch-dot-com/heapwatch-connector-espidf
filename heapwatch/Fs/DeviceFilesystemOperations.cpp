#include "heapwatch/Fs/DeviceFilesystemOperations.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <cinttypes>

#include "esp_flash.h"
#include "esp_flash_partitions.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "mbedtls/sha256.h"
#include "sdkconfig.h"

#include "heapwatch/StreamIngress/StreamIngressClient.h"
#include "heapwatch/StreamIngress/Mux/StreamIngressMux.h"
#include "heapwatch/Fs/Volumes/FsVolumeManager.h"
#include "heapwatch/Fs/HeapwatchFsPartition.h"

namespace Heapwatch::Fs {

namespace {

constexpr char TAG[] = "HwFs";

/** ESP-IDF: partition table always occupies one 4 KiB flash sector. */
constexpr uint32_t PARTITION_TABLE_SECTOR_BYTES = 0x1000;
/** Secure Boot V1 digest at flash offset 0 (one sector). */
constexpr uint32_t SECURE_BOOT_DIGEST_BYTES = 0x1000;
/** ESP-IDF partition types (esp_partition_type_t) — stable since IDF 5.x CSV tooling. */
constexpr int32_t PARTITION_TYPE_BOOTLOADER = 0x02;
constexpr int32_t PARTITION_TYPE_PARTITION_TABLE = 0x03;
constexpr int32_t PARTITION_SUBTYPE_PRIMARY = 0x00;

constexpr size_t DOWNLOAD_CHUNK_BYTES = 4096;
constexpr size_t READ_LINE_BUF = 512;
// file_browser mux frames are ≤4 KiB. Leave headroom for JSON envelope + escaping.
constexpr size_t READ_JSON_ENVELOPE_RESERVE = 384;
constexpr size_t READ_CONTENT_ESCAPED_BUDGET =
    Heapwatch::StreamIngress::Mux::MAX_PAYLOAD_LEN > READ_JSON_ENVELOPE_RESERVE
        ? Heapwatch::StreamIngress::Mux::MAX_PAYLOAD_LEN - READ_JSON_ENVELOPE_RESERVE
        : 512;

// Shared by sha256File / download — lives in BSS, not on the 6KiB stream-ingress stack.
// Only called from hw_stream_ingress (single-threaded FS work).
uint8_t g_io_buf[DOWNLOAD_CHUNK_BYTES] = {};

struct DirEntry {
    std::string name;
    std::string path;
    std::string type;
    int64_t size = 0;
    std::string modified_at;
};

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

bool containsDotDot(const char* path) {
    if (path == nullptr) {
        return true;
    }
    return strstr(path, "/..") != nullptr || strstr(path, "../") != nullptr ||
           strcmp(path, "..") == 0;
}

/** Remove leftover `.hw-*.tmp` objects from a prior failed upload (SPIFFS slots + space). */
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

const char* extensionOrFolder(const char* name, bool is_dir) {
    if (is_dir) {
        return "folder";
    }
    if (name == nullptr) {
        return "file";
    }
    const char* dot = strrchr(name, '.');
    if (dot == nullptr || dot == name || dot[1] == '\0') {
        return "file";
    }
    return dot + 1;
}

bool formatIso8601(time_t epoch_seconds, char* out, size_t out_len) {
    if (out == nullptr || out_len < 21) {
        return false;
    }

    struct tm tm_utc = {};
    if (gmtime_r(&epoch_seconds, &tm_utc) == nullptr) {
        return false;
    }

    const size_t written = strftime(out, out_len, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return written > 0;
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

bool isAllowedPath(const char* path) {
    if (path == nullptr || path[0] != '/') {
        return false;
    }
    if (containsDotDot(path)) {
        return false;
    }
    return Heapwatch::Fs::Volumes::pathIsAllowed(path);
}

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

/** ESP-IDF type name when known; nullptr if only type_id should be used. */
const char* espTypeName(esp_partition_type_t type) {
    switch (type) {
        case ESP_PARTITION_TYPE_APP:
            return "app";
        case ESP_PARTITION_TYPE_DATA:
            return "data";
        default:
            return nullptr;
    }
}

/** ESP-IDF subtype CSV name when known; nullptr if unknown (do not invent hex here). */
const char* espSubtypeName(esp_partition_type_t type, esp_partition_subtype_t subtype) {
    if (type == ESP_PARTITION_TYPE_APP) {
        if (subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
            return "factory";
        }
        if (subtype == ESP_PARTITION_SUBTYPE_APP_TEST) {
            return "test";
        }
        // ota_N is formatted by the caller (needs a writable buffer).
        if (subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
            subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
            return nullptr;
        }
        return nullptr;
    }

    if (type == ESP_PARTITION_TYPE_DATA) {
        switch (subtype) {
            case ESP_PARTITION_SUBTYPE_DATA_OTA:
                return "ota";
            case ESP_PARTITION_SUBTYPE_DATA_PHY:
                return "phy";
            case ESP_PARTITION_SUBTYPE_DATA_NVS:
                return "nvs";
            case ESP_PARTITION_SUBTYPE_DATA_COREDUMP:
                return "coredump";
            case ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS:
                return "nvs_keys";
            case ESP_PARTITION_SUBTYPE_DATA_EFUSE_EM:
                return "efuse_em";
            case ESP_PARTITION_SUBTYPE_DATA_UNDEFINED:
                return "undefined";
            case ESP_PARTITION_SUBTYPE_DATA_FAT:
                return "fat";
            case ESP_PARTITION_SUBTYPE_DATA_SPIFFS:
                return "spiffs";
            default:
                if (heapwatchIsLittlefsSubtype(subtype)) {
                    return "littlefs";
                }
                return nullptr;
        }
    }

    return nullptr;
}

bool formatOtaSubtypeName(esp_partition_type_t type, esp_partition_subtype_t subtype, char* buf, size_t buflen) {
    if (buf == nullptr || buflen == 0) {
        return false;
    }
    if (type != ESP_PARTITION_TYPE_APP) {
        return false;
    }
    if (subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN || subtype > ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
        return false;
    }
    const int32_t slot = static_cast<int32_t>(subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN);
    snprintf(buf, buflen, "ota_%" PRId32, slot);
    return true;
}

bool tryFsUsedBytes(const char* label, Heapwatch::Fs::Volumes::Kind kind, int64_t* used_out, int64_t* total_out) {
    if (label == nullptr || used_out == nullptr || total_out == nullptr) {
        return false;
    }
    size_t total = 0;
    size_t used = 0;
    if (kind == Heapwatch::Fs::Volumes::Kind::Fat) {
        const Heapwatch::Fs::Volumes::Volume* vol = nullptr;
        for (size_t i = 0; i < Heapwatch::Fs::Volumes::mountedCount(); ++i) {
            const Heapwatch::Fs::Volumes::Volume* candidate = Heapwatch::Fs::Volumes::volumeAt(i);
            if (candidate != nullptr && !candidate->external &&
                strcmp(candidate->label, label) == 0) {
                vol = candidate;
                break;
            }
        }
        if (vol == nullptr || !Heapwatch::Fs::Volumes::queryUsage(*vol, &total, &used)) {
            return false;
        }
    } else if (!Heapwatch::Fs::Volumes::queryUsageByLabel(label, kind, &total, &used)) {
        return false;
    }
    *total_out = static_cast<int64_t>(total);
    *used_out = static_cast<int64_t>(used);
    return true;
}

uint32_t bootloaderFlashOffset() {
#if defined(CONFIG_BOOTLOADER_OFFSET_IN_FLASH)
    return static_cast<uint32_t>(CONFIG_BOOTLOADER_OFFSET_IN_FLASH);
#else
    return static_cast<uint32_t>(ESP_BOOTLOADER_OFFSET);
#endif
}

cJSON* buildSyntheticPartitionObject(const char* id, const char* label, const char* type,
                                     const char* subtype, int32_t type_id, int32_t subtype_id,
                                     uint32_t address, uint32_t size_bytes) {
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", id);
    cJSON_AddStringToObject(obj, "label", label);
    cJSON_AddStringToObject(obj, "type", type);
    cJSON_AddStringToObject(obj, "subtype", subtype);
    cJSON_AddNumberToObject(obj, "type_id", static_cast<double>(type_id));
    cJSON_AddNumberToObject(obj, "subtype_id", static_cast<double>(subtype_id));
    cJSON_AddNumberToObject(obj, "address", static_cast<double>(address));
    cJSON_AddNumberToObject(obj, "total_bytes", static_cast<double>(size_bytes));
    cJSON_AddNullToObject(obj, "used_bytes");
    cJSON_AddBoolToObject(obj, "browsable", false);
    cJSON_AddNullToObject(obj, "volume_id");
    cJSON* flags = cJSON_CreateArray();
    cJSON_AddItemToObject(obj, "flags", flags);
    return obj;
}

void appendHiddenFlashPartitions(cJSON* partitions) {
    if (partitions == nullptr) {
        return;
    }

    const uint32_t table_offset = static_cast<uint32_t>(ESP_PARTITION_TABLE_OFFSET);
    const uint32_t boot_offset = bootloaderFlashOffset();

#if CONFIG_SECURE_BOOT
    cJSON_AddItemToArray(
        partitions,
        buildSyntheticPartitionObject("secure_boot_digest", "Reserved", "reserved", "reserved", 0, 0,
                                      0, SECURE_BOOT_DIGEST_BYTES));
#endif

    if (table_offset > boot_offset) {
        cJSON_AddItemToArray(
            partitions,
            buildSyntheticPartitionObject("bootloader", "Bootloader", "bootloader", "primary",
                                      PARTITION_TYPE_BOOTLOADER, PARTITION_SUBTYPE_PRIMARY,
                                      boot_offset, table_offset - boot_offset));
    }

    cJSON_AddItemToArray(
        partitions,
        buildSyntheticPartitionObject("partition_table", "Partition Table", "partition_table",
                                      "primary", PARTITION_TYPE_PARTITION_TABLE,
                                      PARTITION_SUBTYPE_PRIMARY, table_offset,
                                      PARTITION_TABLE_SECTOR_BYTES));
}

cJSON* buildPartitionObject(const esp_partition_t* part) {
    if (part == nullptr) {
        return nullptr;
    }

    const char* label = part->label[0] != '\0' ? part->label : "unnamed";
    const char* type_name = espTypeName(part->type);
    char ota_subtype_buf[8];
    const char* subtype_name = espSubtypeName(part->type, part->subtype);
    if (subtype_name == nullptr &&
        formatOtaSubtypeName(part->type, part->subtype, ota_subtype_buf, sizeof(ota_subtype_buf))) {
        subtype_name = ota_subtype_buf;
    }

    bool browsable = false;
    const char* volume_id = nullptr;
    int64_t used = -1;
    int64_t total_bytes = static_cast<int64_t>(part->size);

    Heapwatch::Fs::Volumes::Kind kind = Heapwatch::Fs::Volumes::Kind::Spiffs;
    bool is_fs = false;
    if (part->type == ESP_PARTITION_TYPE_DATA &&
        part->subtype == ESP_PARTITION_SUBTYPE_DATA_SPIFFS) {
        kind = Heapwatch::Fs::Volumes::Kind::Spiffs;
        is_fs = true;
    } else if (part->type == ESP_PARTITION_TYPE_DATA &&
               heapwatchIsLittlefsSubtype(part->subtype)) {
        kind = Heapwatch::Fs::Volumes::Kind::Littlefs;
        is_fs = true;
    } else if (part->type == ESP_PARTITION_TYPE_DATA &&
               part->subtype == ESP_PARTITION_SUBTYPE_DATA_FAT) {
        kind = Heapwatch::Fs::Volumes::Kind::Fat;
        is_fs = true;
    }

    if (is_fs) {
        int64_t fs_used = 0;
        int64_t fs_total = 0;
        if (tryFsUsedBytes(label, kind, &fs_used, &fs_total)) {
            used = fs_used;
            (void)fs_total;
        }
        for (size_t i = 0; i < Heapwatch::Fs::Volumes::mountedCount(); ++i) {
            const Heapwatch::Fs::Volumes::Volume* vol = Heapwatch::Fs::Volumes::volumeAt(i);
            if (vol != nullptr && vol->browsable && !vol->external &&
                strcmp(vol->label, label) == 0) {
                browsable = true;
                volume_id = vol->label;
                break;
            }
        }
    }

    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", label);
    cJSON_AddStringToObject(obj, "label", label);
    if (type_name != nullptr) {
        cJSON_AddStringToObject(obj, "type", type_name);
    } else {
        cJSON_AddNullToObject(obj, "type");
    }
    if (subtype_name != nullptr) {
        cJSON_AddStringToObject(obj, "subtype", subtype_name);
    } else {
        cJSON_AddNullToObject(obj, "subtype");
    }
    cJSON_AddNumberToObject(obj, "type_id", static_cast<double>(part->type));
    cJSON_AddNumberToObject(obj, "subtype_id", static_cast<double>(part->subtype));
    cJSON_AddNumberToObject(obj, "address", static_cast<double>(part->address));
    cJSON_AddNumberToObject(obj, "total_bytes", static_cast<double>(total_bytes));
    if (used >= 0) {
        cJSON_AddNumberToObject(obj, "used_bytes", static_cast<double>(used));
    } else {
        cJSON_AddNullToObject(obj, "used_bytes");
    }
    cJSON_AddBoolToObject(obj, "browsable", browsable);
    if (volume_id != nullptr) {
        cJSON_AddStringToObject(obj, "volume_id", volume_id);
    } else {
        cJSON_AddNullToObject(obj, "volume_id");
    }

    cJSON* flags = cJSON_CreateArray();
    if (part->encrypted) {
        cJSON_AddItemToArray(flags, cJSON_CreateString("encrypted"));
    }
    cJSON_AddItemToObject(obj, "flags", flags);

    return obj;
}

cJSON* buildPartitionsArray() {
    cJSON* partitions = cJSON_CreateArray();
    appendHiddenFlashPartitions(partitions);

    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    while (it != nullptr) {
        const esp_partition_t* part = esp_partition_get(it);
        cJSON* obj = buildPartitionObject(part);
        if (obj != nullptr) {
            cJSON_AddItemToArray(partitions, obj);
        }
        it = esp_partition_next(it);
    }
    // esp_partition_next(NULL terminator) already releases the iterator.
    return partitions;
}

cJSON* buildVolumeSnapshotForPath(const char* path) {
    const Heapwatch::Fs::Volumes::Volume* vol = Heapwatch::Fs::Volumes::findByPath(path);
    if (vol != nullptr) {
        return Heapwatch::Fs::Volumes::buildVolumeObject(*vol);
    }

    return nullptr;
}

cJSON* buildVolumesResult() {
    cJSON* volumes = cJSON_CreateArray();

    for (size_t i = 0; i < Heapwatch::Fs::Volumes::mountedCount(); ++i) {
        const Heapwatch::Fs::Volumes::Volume* vol = Heapwatch::Fs::Volumes::volumeAt(i);
        if (vol == nullptr || !vol->browsable) {
            continue;
        }
        cJSON* obj = Heapwatch::Fs::Volumes::buildVolumeObject(*vol);
        if (obj != nullptr) {
            cJSON_AddItemToArray(volumes, obj);
        }
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "volumes", volumes);
    return result;
}

cJSON* buildStorageResult() {
    cJSON* result = cJSON_CreateObject();

    uint32_t flash_size = 0;
    if (esp_flash_get_physical_size(nullptr, &flash_size) == ESP_OK && flash_size > 0) {
        cJSON_AddNumberToObject(result, "flash_size_bytes", static_cast<double>(flash_size));
    } else {
        cJSON_AddNullToObject(result, "flash_size_bytes");
    }

    const uint32_t table_offset = static_cast<uint32_t>(ESP_PARTITION_TABLE_OFFSET);
    const uint32_t boot_offset = bootloaderFlashOffset();
    cJSON_AddNumberToObject(result, "partition_table_offset", static_cast<double>(table_offset));
    cJSON_AddNumberToObject(result, "bootloader_offset", static_cast<double>(boot_offset));

    cJSON_AddItemToObject(result, "partitions", buildPartitionsArray());

    cJSON* volumesWrapper = buildVolumesResult();
    cJSON* volumes = cJSON_DetachItemFromObject(volumesWrapper, "volumes");
    cJSON_Delete(volumesWrapper);
    if (volumes == nullptr) {
        volumes = cJSON_CreateArray();
    }
    cJSON_AddItemToObject(result, "volumes", volumes);
    return result;
}

cJSON* buildListResult(const char* path, int32_t page, int32_t per_page, FsError* error) {
    if (!isAllowedPath(path)) {
        if (error != nullptr) {
            error->code = "invalid_path";
            error->message = "Path is not under an allowed mount.";
        }
        return nullptr;
    }

    if (page < 1) {
        page = 1;
    }
    if (per_page < 1) {
        per_page = 25;
    }

    DIR* dir = opendir(path);
    if (dir == nullptr) {
        if (error != nullptr) {
            error->code = "not_found";
            error->message = "Directory not found.";
        }
        return nullptr;
    }

    std::vector<DirEntry> entries;
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        char child_path[FS_PATH_MAX] = {};
        const int32_t path_written = snprintf(child_path, sizeof(child_path), "%s/%s", path, ent->d_name);
        if (path_written <= 0 || static_cast<size_t>(path_written) >= sizeof(child_path)) {
            continue;
        }

        struct stat st = {};
        if (stat(child_path, &st) != 0) {
            continue;
        }

        DirEntry row = {};
        row.name = ent->d_name;
        row.path = child_path;
        row.type = extensionOrFolder(ent->d_name, S_ISDIR(st.st_mode));
        row.size = S_ISDIR(st.st_mode) ? 0 : static_cast<int64_t>(st.st_size);

        char iso[32] = {};
        // LittleFS dirs (and SPIFFS) often report Unix epoch 0; treat as unknown.
        if (st.st_mtime > 0 && formatIso8601(st.st_mtime, iso, sizeof(iso))) {
            row.modified_at = iso;
        }

        entries.push_back(std::move(row));
    }
    closedir(dir);

    std::sort(entries.begin(), entries.end(),
              [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });

    const int32_t total = static_cast<int32_t>(entries.size());
    const int32_t start = (page - 1) * per_page;
    const int32_t end = std::min(start + per_page, total);

    cJSON* entry_array = cJSON_CreateArray();
    if (start < total) {
        for (int32_t i = start; i < end; i++) {
            const DirEntry& row = entries[static_cast<size_t>(i)];
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", row.name.c_str());
            cJSON_AddStringToObject(item, "path", row.path.c_str());
            cJSON_AddStringToObject(item, "type", row.type.c_str());
            cJSON_AddNumberToObject(item, "size", static_cast<double>(row.size));
            if (!row.modified_at.empty()) {
                cJSON_AddStringToObject(item, "modified_at", row.modified_at.c_str());
            } else {
                cJSON_AddNullToObject(item, "modified_at");
            }
            cJSON_AddItemToArray(entry_array, item);
        }
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "entries", entry_array);
    cJSON_AddNumberToObject(result, "total", total);

    cJSON* volume = buildVolumeSnapshotForPath(path);
    if (volume != nullptr) {
        cJSON_AddItemToObject(result, "volume", volume);
    } else {
        cJSON_AddNullToObject(result, "volume");
    }

    return result;
}

size_t jsonEscapedSize(const char* text) {
    if (text == nullptr) {
        return 0;
    }

    size_t escaped = 0;
    for (const uint8_t* p = reinterpret_cast<const uint8_t*>(text); *p != '\0'; ++p) {
        const uint8_t c = *p;
        if (c == '"' || c == '\\' || c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t') {
            escaped += 2;
        } else if (c < 0x20) {
            escaped += 6;  // \u00XX
        } else {
            escaped += 1;
        }
    }
    return escaped;
}

cJSON* buildReadResult(const char* path, int32_t from_line, int32_t to_line, FsError* error) {
    if (!isAllowedPath(path)) {
        if (error != nullptr) {
            error->code = "invalid_path";
            error->message = "Path is not under an allowed mount.";
        }
        return nullptr;
    }

    if (from_line < 1 || to_line < from_line) {
        if (error != nullptr) {
            error->code = "invalid_range";
            error->message = "Line range must be 1-based inclusive.";
        }
        return nullptr;
    }

    FILE* fp = fopen(path, "rb");
    if (fp == nullptr) {
        if (error != nullptr) {
            error->code = "not_found";
            error->message = "File not found.";
        }
        return nullptr;
    }

    struct stat st = {};
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        fclose(fp);
        if (error != nullptr) {
            error->code = "is_directory";
            error->message = "Cannot read a directory as text.";
        }
        return nullptr;
    }

    std::string content;
    size_t escaped_bytes = 0;
    int32_t current_line = 0;
    int32_t first_emitted = 0;
    int32_t last_emitted = 0;
    char line[READ_LINE_BUF] = {};

    while (fgets(line, sizeof(line), fp) != nullptr) {
        current_line++;
        if (current_line < from_line) {
            continue;
        }
        if (current_line > to_line) {
            break;
        }

        const size_t line_escaped = jsonEscapedSize(line);
        if (escaped_bytes + line_escaped > READ_CONTENT_ESCAPED_BUDGET) {
            // Mux frame is full — return only lines that already fit.
            // Prefer returning a prefix of the window over failing the whole read.
            if (first_emitted == 0) {
                // Even the first requested line alone does not fit.
                fclose(fp);
                if (error != nullptr) {
                    error->code = "line_too_large";
                    error->message = "A single line exceeds the stream frame budget.";
                }
                return nullptr;
            }
            break;
        }

        content.append(line);
        escaped_bytes += line_escaped;
        if (first_emitted == 0) {
            first_emitted = current_line;
        }
        last_emitted = current_line;
    }

    fclose(fp);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "path", path);
    // 0/0 when the window is past EOF or the file has no lines (do not echo the requested range).
    // Otherwise report the exact lines included (may be a prefix of the request if budget full).
    cJSON_AddNumberToObject(result, "from_line", first_emitted);
    cJSON_AddNumberToObject(result, "to_line", last_emitted);
    cJSON_AddStringToObject(result, "content", content.c_str());
    return result;
}

bool deleteEntry(const char* path, FsError* error) {
    if (!isAllowedPath(path)) {
        if (error != nullptr) {
            error->code = "invalid_path";
            error->message = "Path is not under an allowed mount.";
        }
        return false;
    }

    struct stat st = {};
    if (stat(path, &st) != 0) {
        if (error != nullptr) {
            error->code = "not_found";
            error->message = "Entry not found.";
        }
        return false;
    }

    if (S_ISDIR(st.st_mode)) {
        if (rmdir(path) != 0) {
            if (error != nullptr) {
                error->code = "io_error";
                error->message = "Failed to remove directory.";
            }
            return false;
        }
        return true;
    }

    if (unlink(path) != 0) {
        if (error != nullptr) {
            error->code = "io_error";
            error->message = "Failed to delete file.";
        }
        return false;
    }
    return true;
}

bool renameEntry(const char* path, const char* new_name, FsError* error) {
    if (!isAllowedPath(path) || new_name == nullptr || new_name[0] == '\0' ||
        strchr(new_name, '/') != nullptr) {
        if (error != nullptr) {
            error->code = "invalid_path";
            error->message = "Invalid path or new name.";
        }
        return false;
    }

    const char* slash = strrchr(path, '/');
    if (slash == nullptr) {
        if (error != nullptr) {
            error->code = "invalid_path";
            error->message = "Path must be absolute.";
        }
        return false;
    }

    char dest[FS_PATH_MAX] = {};
    const size_t dir_len = static_cast<size_t>(slash - path);
    snprintf(dest, sizeof(dest), "%.*s/%s", static_cast<int>(dir_len), path, new_name);

    if (!isAllowedPath(dest)) {
        if (error != nullptr) {
            error->code = "invalid_path";
            error->message = "Renamed path escapes allowed mounts.";
        }
        return false;
    }

    if (rename(path, dest) != 0) {
        if (error != nullptr) {
            error->code = "io_error";
            error->message = "Failed to rename entry.";
        }
        return false;
    }
    return true;
}

bool moveEntry(const char* path, const char* destination_path, FsError* error) {
    if (!isAllowedPath(path) || !isAllowedPath(destination_path)) {
        if (error != nullptr) {
            error->code = "invalid_path";
            error->message = "Path is not under an allowed mount.";
        }
        return false;
    }

    if (rename(path, destination_path) != 0) {
        if (error != nullptr) {
            error->code = "io_error";
            error->message = "Failed to move entry.";
        }
        return false;
    }
    return true;
}

bool mkdirEntry(const char* path, FsError* error) {
    if (!isAllowedPath(path)) {
        if (error != nullptr) {
            error->code = "invalid_path";
            error->message = "Path is not under an allowed mount.";
        }
        return false;
    }

    if (mkdir(path, 0755) != 0) {
        if (error != nullptr) {
            error->code = "io_error";
            error->message = "Failed to create directory.";
        }
        return false;
    }
    return true;
}

bool createEmptyFile(const char* path, FsError* error) {
    if (!isAllowedPath(path)) {
        if (error != nullptr) {
            error->code = "invalid_path";
            error->message = "Path is not under an allowed mount.";
        }
        return false;
    }

    FILE* fp = fopen(path, "wb");
    if (fp == nullptr) {
        if (error != nullptr) {
            error->code = "io_error";
            error->message = "Failed to create file.";
        }
        return false;
    }
    fclose(fp);
    return true;
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
