// Copyright 2026 Heapwatch
// SPDX-License-Identifier: Apache-2.0

#include "heapwatch/Helpers/MemoryManager/HeapwatchMemoryManager.h"

#include <cstdlib>
#include <cinttypes>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "heapwatch/Helpers/FreeRtosAlloc.h"

#if !configSUPPORT_STATIC_ALLOCATION
#error "Heapwatch memory block requires configSUPPORT_STATIC_ALLOCATION"
#endif

namespace Heapwatch::Helpers::MemoryManager {
namespace {

constexpr char TAG[] = "HwMem";

ConnectorMemoryBlock* g_ws = nullptr;

SemaphoreHandle_t create_mutex_static(StaticSemaphore_t* buf, const char* name) {
    SemaphoreHandle_t sem = xSemaphoreCreateMutexStatic(buf);
    if (sem == nullptr) {
        ESP_LOGE(TAG, "FATAL: static mutex %s failed", name);
        abort();
    }
    return sem;
}

SemaphoreHandle_t create_counting_static(UBaseType_t max_count, StaticSemaphore_t* buf,
                                         const char* name) {
    SemaphoreHandle_t sem = xSemaphoreCreateCountingStatic(max_count, 0, buf);
    if (sem == nullptr) {
        ESP_LOGE(TAG, "FATAL: static counting sem %s failed", name);
        abort();
    }
    return sem;
}

void create_rtos_objects(ConnectorMemoryBlock* ws) {
    ws->tiny_queue =
        heapwatch_queue_create_static(HEAPWATCH_TINY_QUEUE_LEN, sizeof(TinyMqttMessage),
                                      ws->tiny_queue_storage, &ws->tiny_queue_ctrl, "tiny");
#if CONFIG_HEAPWATCH_ENABLE_LOG_COLLECTION || CONFIG_HEAPWATCH_ENABLE_OTA
    ws->small_queue =
        heapwatch_queue_create_static(HEAPWATCH_SMALL_QUEUE_LEN, sizeof(SmallMqttMessage),
                                      ws->small_queue_storage, &ws->small_queue_ctrl, "small");
#endif
    ws->large_queue =
        heapwatch_queue_create_static(HEAPWATCH_LARGE_QUEUE_LEN, sizeof(LargeMqttMessage),
                                      ws->large_queue_storage, &ws->large_queue_ctrl, "large");

    ws->publish_mutex = create_mutex_static(&ws->publish_mutex_buf, "publish");
    UBaseType_t outbound_max = HEAPWATCH_TINY_QUEUE_LEN + HEAPWATCH_LARGE_QUEUE_LEN;
#if CONFIG_HEAPWATCH_ENABLE_LOG_COLLECTION || CONFIG_HEAPWATCH_ENABLE_OTA
    outbound_max += HEAPWATCH_SMALL_QUEUE_LEN;
#endif
    ws->mqtt_queue_work_sem = create_counting_static(outbound_max, &ws->mqtt_queue_work_sem_buf, "outbound");

    ws->command_inbound_queue = heapwatch_queue_create_static(
        HEAPWATCH_COMMAND_INBOUND_QUEUE_DEPTH, sizeof(Heapwatch::DeviceCommand::CommandInboundMessage),
        ws->command_inbound_storage, &ws->command_inbound_ctrl, "command");

#if CONFIG_HEAPWATCH_ENABLE_COREDUMP_UPLOAD
    ws->coredump_queue = heapwatch_queue_create_static(
        HEAPWATCH_COREDUMP_QUEUE_LEN, sizeof(CoreDumpPayload*), ws->coredump_queue_storage,
        &ws->coredump_queue_ctrl, "coredump");
#endif

#if CONFIG_HEAPWATCH_ENABLE_STREAM_INGRESS
    ws->stream_outbound_queue = heapwatch_queue_create_static(
        HEAPWATCH_STREAM_OUTBOUND_QUEUE_DEPTH, sizeof(ConnectorMemoryBlock::StreamOutboundQueued),
        ws->stream_outbound_queue_storage, &ws->stream_outbound_queue_ctrl, "stream");
    ws->stream_mutex = create_mutex_static(&ws->stream_mutex_buf, "stream");
#endif
}

void delete_rtos_objects(ConnectorMemoryBlock* ws) {
    heapwatch_queue_delete_static(ws->tiny_queue);
#if CONFIG_HEAPWATCH_ENABLE_LOG_COLLECTION || CONFIG_HEAPWATCH_ENABLE_OTA
    heapwatch_queue_delete_static(ws->small_queue);
#endif
    heapwatch_queue_delete_static(ws->large_queue);
    ws->tiny_queue = nullptr;
    ws->small_queue = nullptr;
    ws->large_queue = nullptr;

    if (ws->publish_mutex != nullptr) {
        vSemaphoreDelete(ws->publish_mutex);
        ws->publish_mutex = nullptr;
    }
    if (ws->mqtt_queue_work_sem != nullptr) {
        vSemaphoreDelete(ws->mqtt_queue_work_sem);
        ws->mqtt_queue_work_sem = nullptr;
    }

    heapwatch_queue_delete_static(ws->command_inbound_queue);
    ws->command_inbound_queue = nullptr;

#if CONFIG_HEAPWATCH_ENABLE_COREDUMP_UPLOAD
    if (ws->coredump_queue != nullptr) {
        CoreDumpPayload* pending = nullptr;
        while (xQueueReceive(ws->coredump_queue, &pending, 0) == pdTRUE) {
        }
        heapwatch_queue_delete_static(ws->coredump_queue);
        ws->coredump_queue = nullptr;
        ws->coredump_busy = false;
    }
#endif

#if CONFIG_HEAPWATCH_ENABLE_STREAM_INGRESS
    heapwatch_queue_delete_static(ws->stream_outbound_queue);
    ws->stream_outbound_queue = nullptr;
    if (ws->stream_mutex != nullptr) {
        vSemaphoreDelete(ws->stream_mutex);
        ws->stream_mutex = nullptr;
    }
#endif
}

}  // namespace

void init() {
    if (g_ws != nullptr) {
        return;
    }

    g_ws = static_cast<ConnectorMemoryBlock*>(heapwatch_data_alloc(sizeof(ConnectorMemoryBlock)));
    create_rtos_objects(g_ws);

    ESP_LOGI(TAG, "memory block %" PRIu32 " bytes (%s)", static_cast<uint32_t>(sizeof(ConnectorMemoryBlock)),
             HEAPWATCH_ALLOCATE_DATA_IN_PSRAM ? "PSRAM" : "DRAM");
#if HEAPWATCH_ALLOCATE_DATA_IN_PSRAM
    ESP_LOGI(TAG, "internal free=%" PRIu32 " largest=%" PRIu32,
             static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
#endif
}

void deinit() {
    if (g_ws == nullptr) {
        return;
    }
    delete_rtos_objects(g_ws);
    heapwatch_data_free(g_ws);
    g_ws = nullptr;
}

ConnectorMemoryBlock* get() { return g_ws; }

void* temporaryArenaAlloc(size_t bytes) {
    if (g_ws == nullptr || bytes == 0) {
        return nullptr;
    }
    const size_t aligned = (bytes + 7U) & ~static_cast<size_t>(7U);
    if (g_ws->temporary_arena_used > HEAPWATCH_MEM_SCRATCH_BYTES ||
        aligned > HEAPWATCH_MEM_SCRATCH_BYTES - g_ws->temporary_arena_used) {
        return nullptr;
    }
    void* p = g_ws->temporary_arena + g_ws->temporary_arena_used;
    g_ws->temporary_arena_used += aligned;
    return p;
}

void temporaryArenaReset() {
    if (g_ws != nullptr) {
        g_ws->temporary_arena_used = 0;
    }
}

}  // namespace Heapwatch::Helpers::MemoryManager
