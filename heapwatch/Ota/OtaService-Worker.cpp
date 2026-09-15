#include "heapwatch/Ota/OtaServiceInternal.h"

#include "sdkconfig.h"

#if CONFIG_HEAPWATCH_ENABLE_OTA

#include "heapwatch/Helpers/FreeRtosAlloc.h"

namespace Heapwatch::Ota {

void taskPerformOta(void* /*arg*/) {
    while (!g_stop_requested) {
        // Block until hw_command arms a job (task notification — lighter than a binary
        // semaphore and cannot accumulate stray wake counts). shutdown() notifies too,
        // so this stays a zero-wakeup idle rather than a poll.
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (g_stop_requested) {
            break;
        }
        if (g_phase != Phase::Running || g_job == nullptr) {
            continue;
        }

        OtaJob job = *g_job;
        runOtaJob(job);
    }
    g_task_exited = true;
    g_ota_task = nullptr;
    heapwatch_task_delete_dram(nullptr);
}


}  // namespace Heapwatch::Ota

#endif  // CONFIG_HEAPWATCH_ENABLE_OTA
