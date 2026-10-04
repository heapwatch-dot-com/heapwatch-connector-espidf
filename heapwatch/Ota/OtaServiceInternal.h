#pragma once

// Shared between OtaService.cpp and OtaService-Worker.cpp.
// Not part of the public OtaService API.

#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "heapwatch/Ota/OtaService.h"

#if CONFIG_HEAPWATCH_ENABLE_OTA

namespace Heapwatch::Ota {

constexpr size_t DOWNLOAD_CHUNK = CONFIG_HEAPWATCH_OTA_DOWNLOAD_CHUNK_BYTES;
constexpr uint32_t OTA_TASK_STACK_BYTES = CONFIG_HEAPWATCH_OTA_TASK_STACK_BYTES;

enum class Phase : uint8_t {
    Idle = 0,
    Running,
    BootPointerSwitched,
};

extern uint8_t g_ota_io_buf[];
extern SemaphoreHandle_t g_mutex;
extern TaskHandle_t g_ota_task;
extern volatile bool g_abort_requested;
extern volatile bool g_stop_requested;
extern volatile bool g_task_exited;
extern volatile Phase g_phase;
extern OtaJob* g_job;

void runOtaJob(const OtaJob& job);
void taskPerformOta(void* arg);

}  // namespace Heapwatch::Ota

#endif  // CONFIG_HEAPWATCH_ENABLE_OTA
