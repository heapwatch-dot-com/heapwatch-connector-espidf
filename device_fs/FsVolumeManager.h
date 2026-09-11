#pragma once

#include <cstddef>
#include <cstdint>

#include "cJSON.h"
#include "esp_err.h"
#include "wear_levelling.h"

namespace heapwatch::fs_volumes {

constexpr size_t kMaxVolumes = 8;
constexpr size_t kLabelMax = 17;
constexpr size_t kMountPathMax = 64;

enum class Kind : uint8_t {
    Spiffs = 0,
    Littlefs,
    Fat,
};

struct Volume {
    char label[kLabelMax] = {};
    char mount_path[kMountPathMax] = {};
    Kind kind = Kind::Spiffs;
    bool mounted = false;
    bool browsable = false;
    bool connector_mounted = false;
    wl_handle_t fat_wl = WL_INVALID_HANDLE;
};

/** Host mapping: call only before heapwatch_initialize() (and after deinit). */
esp_err_t registerVolume(const char* partition_label, const char* mount_path);

/** Scan flash FS partitions; auto-mount or adopt already-mounted volumes. */
void mountAll();

/** Unmount only connector-owned volumes. Host mappings survive. */
void unmountConnectorOwned();

size_t mountedCount();
const Volume* volumeAt(size_t index);

/** Longest mount-prefix match among browsable volumes. */
const Volume* findByPath(const char* path);

bool pathIsAllowed(const char* path);

bool queryUsage(const Volume& volume, size_t* total_bytes, size_t* used_bytes);

/** Used/total by partition label when the FS driver can answer (even if not browsable). */
bool queryUsageByLabel(const char* label, Kind kind, size_t* total_bytes, size_t* used_bytes);

const char* kindName(Kind kind);

cJSON* buildVolumeObject(const Volume& volume);

}  // namespace heapwatch::fs_volumes
