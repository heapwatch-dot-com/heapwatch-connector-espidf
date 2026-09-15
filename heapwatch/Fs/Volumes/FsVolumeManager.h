#pragma once

#include <cstddef>
#include <cstdint>

#include "cJSON.h"
#include "esp_err.h"
#include "wear_levelling.h"

namespace Heapwatch::Fs::Volumes {

constexpr size_t MAX_FLASH_VOLUMES = 8;
constexpr size_t MAX_SD_VOLUMES = 4;
constexpr size_t MAX_VOLUMES = MAX_FLASH_VOLUMES + MAX_SD_VOLUMES;
constexpr size_t LABEL_MAX = 17;
constexpr size_t MOUNT_PATH_MAX = 64;

enum class Kind : uint8_t {
    KIND_SPIFFS = 0,
    KIND_LITTLEFS,
    KIND_FAT,
};

struct Volume {
    char label[LABEL_MAX] = {};
    char mount_path[MOUNT_PATH_MAX] = {};
    Kind kind = Kind::KIND_SPIFFS;
    bool mounted = false;
    bool browsable = false;
    bool connector_mounted = false;
    /** No flash partition backs this volume (SD card) — reported with a null partition_id. */
    bool external = false;
    wl_handle_t fat_wl = WL_INVALID_HANDLE;
};

struct HostMapping {
    char label[LABEL_MAX] = {};
    char mount_path[MOUNT_PATH_MAX] = {};
    bool used = false;
};

/** Drop working-set pointers after MemoryManager::deinit(). */
void unbindMemoryBlock();

/**
 * Map a flash partition label to a VFS path. Safe before or after heapwatch_initialize().
 * After mountAll(), an already-mounted volume is registered immediately (or on the next fs
 * command if the path is not a VFS root yet). Does not remount a connector-owned volume to a
 * new path.
 */
esp_err_t registerVolume(const char* partition_label, const char* mount_path);

/**
 * Host-mounted SD card at an arbitrary VFS path. Safe before or after heapwatch_initialize().
 * Read-only registration of an already-mounted card: the connector never touches the bus and
 * never unmounts the card.
 */
esp_err_t registerSdVolume(const char* label, const char* mount_path);

/** Scan flash FS partitions; auto-mount or register already-mounted volumes. */
void mountAll();

/**
 * Bind registered mappings whose VFS roots appeared after mountAll() (host mounted later).
 * Safe to call from fs command handlers.
 */
void tryBindRegisteredVolumesNowMounted();

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

/**
 * Append `dynamic.fs.{id}.used_bytes` and `_max` (total) for each browsable volume.
 * JSON: leading commas. Flat: leading `|`. Returns bytes written (not including NUL).
 */
size_t appendParameterFields(char* out, size_t out_len, bool json);

}  // namespace Heapwatch::Fs::Volumes
