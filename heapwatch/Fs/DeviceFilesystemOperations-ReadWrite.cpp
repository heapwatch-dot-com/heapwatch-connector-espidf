#include "heapwatch/Fs/DeviceFilesystemOperations.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "heapwatch/StreamIngress/Mux/StreamIngressMux.h"
#include "heapwatch/Fs/Volumes/FsVolumeManager.h"

namespace Heapwatch::Fs {

namespace {

constexpr size_t READ_LINE_BUF = 512;
// file_browser mux frames are ≤4 KiB. Leave headroom for JSON envelope + escaping.
constexpr size_t READ_JSON_ENVELOPE_RESERVE = 384;
constexpr size_t READ_CONTENT_ESCAPED_BUDGET =
    Heapwatch::StreamIngress::Mux::MAX_PAYLOAD_LEN > READ_JSON_ENVELOPE_RESERVE
        ? Heapwatch::StreamIngress::Mux::MAX_PAYLOAD_LEN - READ_JSON_ENVELOPE_RESERVE
        : 512;

struct DirEntry {
    std::string name;
    std::string path;
    std::string type;
    int64_t size = 0;
    std::string modified_at;
};

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

cJSON* buildVolumeSnapshotForPath(const char* path) {
    const Heapwatch::Fs::Volumes::Volume* vol = Heapwatch::Fs::Volumes::findByPath(path);
    if (vol != nullptr) {
        return Heapwatch::Fs::Volumes::buildVolumeObject(*vol);
    }

    return nullptr;
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

}  // namespace

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

}  // namespace Heapwatch::Fs
