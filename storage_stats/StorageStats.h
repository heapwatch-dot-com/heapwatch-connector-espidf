#pragma once

// STORAGE1: filesystem used/free bytes. Flash SPIFFS/LittleFS/FatFS volumes are
// mounted (or adopted) by FsVolumeManager. Optional SD-over-SPI is still mounted
// here when HEAPWATCH_ENABLE_SD_CARD_STATS is on.
//
// Reports storage.free_bytes/storage.used_bytes via PARAM1 as an aggregate across
// every mounted volume.
namespace heapwatch::storage_stats {

void init();

void collectAndReport();

}  // namespace heapwatch::storage_stats
