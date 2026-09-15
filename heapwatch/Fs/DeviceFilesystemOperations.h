#pragma once

#include <cstddef>
#include <cstdint>

#include "cJSON.h"

namespace Heapwatch::Fs {

constexpr size_t FS_PATH_MAX = 256;
constexpr size_t FS_NAME_MAX = 128;
constexpr size_t FS_SHA256_HEX_MAX = 65;
constexpr size_t FS_COMMAND_ID_MAX = 27;

struct FsError {
    const char* code = "io_error";
    const char* message = "Filesystem operation failed.";
};

/** Returns true when path is absolute and under an allowed mount prefix. */
bool isAllowedPath(const char* path);

/** Build a file_browser TCP envelope and send it (requires stream ingress session). */
void sendFileBrowserResultOverStream(const char* command_id, bool ok, const FsError* error, cJSON* result);

cJSON* buildVolumesResult();

/** Partitions (flash table) + mounted VFS volumes. */
cJSON* buildStorageResult();

cJSON* buildListResult(const char* path, int32_t page, int32_t per_page, FsError* error);

cJSON* buildReadResult(const char* path, int32_t from_line, int32_t to_line, FsError* error);

bool deleteEntry(const char* path, FsError* error);

bool renameEntry(const char* path, const char* new_name, FsError* error);

bool moveEntry(const char* path, const char* destination_path, FsError* error);

bool mkdirEntry(const char* path, FsError* error);

bool createEmptyFile(const char* path, FsError* error);

bool beginUploadJob(const char* command_id, const char* path, const char* temp_path, bool replace,
                    const char* expected_sha256, int64_t expected_size, FsError* error);

void handleUploadChunk(const uint8_t* data, size_t len);

void cancelActiveUpload();

bool beginDownloadJob(const char* command_id, const char* path, FsError* error);

}  // namespace Heapwatch::Fs
