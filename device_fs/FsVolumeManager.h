#pragma once

#include <cstddef>
#include <cstdint>

#include "cJSON.h"
#include "esp_err.h"
#include "wear_levelling.h"

namespace heapwatch::fs_volumes {

constexpr size_t kMaxFlashVolumes = 8;
constexpr size_t kMaxSdVolumes = 4;
constexpr size_t kMaxVolumes = kMaxFlashVolumes + kMaxSdVolumes;
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
    /** No flash partition backs this volume (SD card) — reported with a null partition_id. */
    bool external = false;
    wl_handle_t fat_wl = WL_INVALID_HANDLE;
};

/**
 * Map a flash partition label to a VFS path. Safe before or after heapwatch_initialize().
 * After mountAll(), the volume is adopted immediately (or on the next fs command if the
 * path is not a VFS root yet). Does not remount a connector-owned volume to a new path.
 */
esp_err_t registerVolume(const char* partition_label, const char* mount_path);

/**
 * Host-mounted SD card at an arbitrary VFS path. Safe before or after heapwatch_initialize().
 * Read-only adoption: the connector never touches the bus and never unmounts the card.
 */
esp_err_t registerSdVolume(const char* label, const char* mount_path);

/** Scan flash FS partitions; auto-mount or adopt already-mounted volumes. */
void mountAll();

/**
 * Adopt registered mappings whose VFS roots appeared after mountAll() (host mounted later).
 * Safe to call from fs command handlers.
 */
void tryAdoptPending();

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
