// Copyright (c) 2026 Heapwatch. All rights reserved.
//
// This software is part of the Heapwatch Connector for ESP-IDF project.
// Unauthorized copying, modification, or distribution of this software,
// via any medium, is strictly prohibited unless otherwise permitted.
//
// This software is provided "as is", without warranty of any kind, express or implied,
// including but not limited to the warranties of merchantability, fitness for a particular purpose,
// and noninfringement. In no event shall the authors or copyright holders be liable for any claim,
// damages, or other liability, whether in an action of contract, tort, or otherwise, arising from,
// out of, or in connection with the software or the use or other dealings in the software.
//
// For licensing inquiries, please contact: info@heapwatch.com
//

#ifndef HEAPWATCH_FREERTOS_ALLOC_H
#define HEAPWATCH_FREERTOS_ALLOC_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "heapwatch_config.h"

#if HEAPWATCH_ALLOCATE_QUEUES_IN_PSRAM
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#endif

static constexpr UBaseType_t kHeapwatchPsramCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

/**
 * Create a FreeRTOS queue for MQTT outbound slots (tiny/small/large/coredump).
 * Internal DRAM when HEAPWATCH_ALLOCATE_QUEUES_IN_PSRAM is off; PSRAM when on.
 */
inline QueueHandle_t heapwatch_queue_create(UBaseType_t uxQueueLength, UBaseType_t uxItemSize) {
#if HEAPWATCH_ALLOCATE_QUEUES_IN_PSRAM
    return xQueueCreateWithCaps(uxQueueLength, uxItemSize, kHeapwatchPsramCaps);
#else
    return xQueueCreate(uxQueueLength, uxItemSize);
#endif
}

/** Pair with heapwatch_queue_create — WithCaps queues must use vQueueDeleteWithCaps. */
inline void heapwatch_queue_delete(QueueHandle_t queue) {
    if (queue == nullptr) {
        return;
    }
#if HEAPWATCH_ALLOCATE_QUEUES_IN_PSRAM
    vQueueDeleteWithCaps(queue);
#else
    vQueueDelete(queue);
#endif
}

#endif  // HEAPWATCH_FREERTOS_ALLOC_H
