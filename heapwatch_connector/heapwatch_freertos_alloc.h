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

#include <cstdlib>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "heapwatch_config.h"

#if HEAPWATCH_ALLOCATE_DATA_IN_PSRAM || HEAPWATCH_STACKS_IN_PSRAM
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#endif

#if HEAPWATCH_STACKS_IN_PSRAM && !defined(CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY)
#error "CONFIG_HEAPWATCH_STACKS_IN_PSRAM requires CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY (menuconfig the option, then idf.py reconfigure)"
#endif

#if HEAPWATCH_ALLOCATE_DATA_IN_PSRAM || HEAPWATCH_STACKS_IN_PSRAM
static constexpr UBaseType_t HEAPWATCH_PSRAM_CAPS = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#endif

/**
 * Create a FreeRTOS queue. PSRAM when HEAPWATCH_ALLOCATE_DATA_IN_PSRAM;
 * internal DRAM otherwise. PSRAM create failure aborts.
 */
inline QueueHandle_t heapwatch_queue_create(UBaseType_t uxQueueLength, UBaseType_t uxItemSize) {
#if HEAPWATCH_ALLOCATE_DATA_IN_PSRAM
    QueueHandle_t queue = xQueueCreateWithCaps(uxQueueLength, uxItemSize, HEAPWATCH_PSRAM_CAPS);
    if (queue == nullptr) {
        ESP_LOGE("HwMem", "FATAL: PSRAM queue create failed (len=%u item=%u)",
                 static_cast<unsigned>(uxQueueLength), static_cast<unsigned>(uxItemSize));
        abort();
    }
    return queue;
#else
    return xQueueCreate(uxQueueLength, uxItemSize);
#endif
}

/**
 * Bytes for MQTT/stream/metrics scratch. PSRAM when the data option is on
 * (abort on failure); otherwise DRAM calloc (abort on failure).
 */
inline void* heapwatch_data_alloc(size_t bytes) {
    if (bytes == 0) {
        return nullptr;
    }
#if HEAPWATCH_ALLOCATE_DATA_IN_PSRAM
    void* p = heap_caps_calloc(1, bytes, HEAPWATCH_PSRAM_CAPS);
    if (p == nullptr) {
        ESP_LOGE("HwMem", "FATAL: PSRAM data alloc %u bytes failed", static_cast<unsigned>(bytes));
        abort();
    }
    return p;
#else
    void* p = calloc(1, bytes);
    if (p == nullptr) {
        ESP_LOGE("HwMem", "FATAL: DRAM data alloc %u bytes failed", static_cast<unsigned>(bytes));
        abort();
    }
    return p;
#endif
}

inline void heapwatch_data_free(void* p) {
    if (p == nullptr) {
        return;
    }
#if HEAPWATCH_ALLOCATE_DATA_IN_PSRAM
    heap_caps_free(p);
#else
    free(p);
#endif
}

/** Pair with heapwatch_queue_create — WithCaps queues must use vQueueDeleteWithCaps. */
inline void heapwatch_queue_delete(QueueHandle_t queue) {
    if (queue == nullptr) {
        return;
    }
#if HEAPWATCH_ALLOCATE_DATA_IN_PSRAM
    vQueueDeleteWithCaps(queue);
#else
    vQueueDelete(queue);
#endif
}

/**
 * Create an hw_* task. Stack is internal DRAM (xTaskCreate) unless
 * HEAPWATCH_STACKS_IN_PSRAM, then PSRAM via xTaskCreateWithCaps.
 * usStackDepth is bytes (ESP-IDF convention).
 *
 * Do not use this for tasks that call NVS / OTA / partition / on-flash VFS:
 * SPI flash disables the cache, and a PSRAM stack then trips
 * esp_task_stack_is_sane_cache_disabled(). Use heapwatch_task_create_dram.
 */
inline BaseType_t heapwatch_task_create(TaskFunction_t fn, const char* name, uint32_t stack_bytes,
                                        void* arg, UBaseType_t prio, TaskHandle_t* handle) {
#if HEAPWATCH_STACKS_IN_PSRAM
    return xTaskCreateWithCaps(fn, name, stack_bytes, arg, prio, handle, HEAPWATCH_PSRAM_CAPS);
#else
    return xTaskCreate(fn, name, stack_bytes, arg, prio, handle);
#endif
}

/**
 * Same as heapwatch_task_create but the stack is always internal DRAM via
 * xTaskCreate (never WithCaps). Flash-using tasks must use this.
 * Pair with heapwatch_task_delete_dram.
 */
inline BaseType_t heapwatch_task_create_dram(TaskFunction_t fn, const char* name,
                                             uint32_t stack_bytes, void* arg, UBaseType_t prio,
                                             TaskHandle_t* handle) {
    return xTaskCreate(fn, name, stack_bytes, arg, prio, handle);
}

/**
 * Delete a task created by heapwatch_task_create (PSRAM / WithCaps when
 * HEAPWATCH_STACKS_IN_PSRAM). Pass nullptr to delete the calling task.
 */
inline void heapwatch_task_delete(TaskHandle_t task) {
#if HEAPWATCH_STACKS_IN_PSRAM
    vTaskDeleteWithCaps(task);
#else
    vTaskDelete(task);
#endif
}

/** Delete a task created by heapwatch_task_create_dram (always xTaskCreate). */
inline void heapwatch_task_delete_dram(TaskHandle_t task) {
    vTaskDelete(task);
}

#endif  // HEAPWATCH_FREERTOS_ALLOC_H
