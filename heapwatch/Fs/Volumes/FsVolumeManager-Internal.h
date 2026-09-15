#pragma once

#include "heapwatch/Fs/Volumes/FsVolumeManager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

namespace Heapwatch::Fs::Volumes {
namespace Internal {

extern HostMapping* g_host_map;
extern HostMapping* g_sd_map;
extern Volume* g_volumes;
extern size_t g_volume_count;
extern bool g_mount_all_done;

void bindVolumeTables();
void lockTable();
void unlockTable();

struct TableLock {
    TableLock() { lockTable(); }
    ~TableLock() { unlockTable(); }
    TableLock(const TableLock&) = delete;
    TableLock& operator=(const TableLock&) = delete;
};

void copyLabel(char* dest, size_t dest_len, const char* src);
bool containsDotDot(const char* path);
bool validMountPath(const char* path);
bool validLabel(const char* label);
bool pathStartsWithMount(const char* path, const char* mount);
bool dirExists(const char* path);
const HostMapping* findHostMapping(const char* label);
HostMapping* claimMappingSlot(HostMapping* table, size_t table_len, const char* label);
Volume* findVolumeByLabelLocked(const char* label);
void defaultMountPath(const char* label, char* out, size_t out_len);

void registerAlreadyMountedVolume(Kind kind, const char* label, const char* mount_path, bool connector_mounted,
                                  wl_handle_t fat_wl, bool browsable, bool external = false);

esp_err_t applyFlashMappingLocked(const HostMapping& mapping);
esp_err_t applySdMappingLocked(const HostMapping& mapping);
void tryBindRegisteredVolumesNowMountedLocked();

void scanAndMountFlashPartitions();
void registerAlreadyMountedSdVolumes();
#if CONFIG_HEAPWATCH_ENABLE_SD_CARD_STATS
void unmountConnectorSdCard(const char* mount_path);
#endif

}  // namespace Internal
}  // namespace Heapwatch::Fs::Volumes
