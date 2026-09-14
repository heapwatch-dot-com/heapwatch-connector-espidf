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

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "device_command/CommandWorker.h"
#include "heapwatch_config.h"
#include "heapwatch_connector/HeapwatchConnector.h"
#include "metrics_collector/MetricsCollector.h"
#include "sdkconfig.h"

#if CONFIG_HEAPWATCH_ENABLE_STREAM_INGRESS
#include "stream_ingress/StreamIngressMux.h"
#endif

namespace heapwatch {

struct PublishScratch {
    TinyMqttMessage tiny;
#if CONFIG_HEAPWATCH_ENABLE_LOG_COLLECTION || CONFIG_HEAPWATCH_ENABLE_OTA
    SmallMqttMessage small;
#endif
    LargeMqttMessage large;
    char specs_body[HEAPWATCH_LARGE_PAYLOAD_SIZE - 96];
#if CONFIG_HEAPWATCH_ENABLE_LOG_COLLECTION
    char raw_log_line[HEAPWATCH_MAX_LOG_LINE_LEN + 1];
    char escaped_message[HEAPWATCH_MAX_LOG_LINE_LEN * 2 + 1];
    char escaped_task[64];
    char escaped_tag[64];
    char optional[192];
#endif
};

/**
 * Single typed working set. One heap_caps/calloc of sizeof(*this) at init.
 * Queue/semaphore storage is in this struct; handles are filled by memory_manager::init().
 */
struct HeapwatchWorkingSet {
    PublishScratch publish;

    char metrics_payload[HEAPWATCH_MAX_PAYLOAD_SIZE];
    MqttMessage metrics_enqueue;
#if CONFIG_HEAPWATCH_ENABLE_TASK_SNAPSHOTS
    metrics_collector::TaskStats task_stats[HEAPWATCH_MAX_TASK_ROWS];
#endif
#if (configUSE_TRACE_FACILITY == 1)
    TaskStatus_t task_status_scan[HEAPWATCH_MAX_TASK_STATUS_SCAN];
#endif

    alignas(8) uint8_t tiny_queue_storage[HEAPWATCH_TINY_QUEUE_LEN * sizeof(TinyMqttMessage)];
#if CONFIG_HEAPWATCH_ENABLE_LOG_COLLECTION || CONFIG_HEAPWATCH_ENABLE_OTA
    alignas(8) uint8_t small_queue_storage[HEAPWATCH_SMALL_QUEUE_LEN * sizeof(SmallMqttMessage)];
    StaticQueue_t small_queue_ctrl;
#endif
    alignas(8) uint8_t large_queue_storage[HEAPWATCH_LARGE_QUEUE_LEN * sizeof(LargeMqttMessage)];
    StaticQueue_t tiny_queue_ctrl;
    StaticQueue_t large_queue_ctrl;
    QueueHandle_t tiny_queue;
    QueueHandle_t small_queue;
    QueueHandle_t large_queue;

    StaticSemaphore_t publish_mutex_buf;
    StaticSemaphore_t outbound_sem_buf;
    SemaphoreHandle_t publish_mutex;
    SemaphoreHandle_t outbound_sem;

    alignas(8) uint8_t command_inbound_storage[HEAPWATCH_COMMAND_INBOUND_QUEUE_DEPTH *
                                               sizeof(device_command::CommandInboundMessage)];
    StaticQueue_t command_inbound_ctrl;
    QueueHandle_t command_inbound_queue;

#if CONFIG_HEAPWATCH_ENABLE_COREDUMP_UPLOAD
    CoreDumpPayload coredump;
    bool coredump_busy;
    alignas(8) uint8_t coredump_queue_storage[HEAPWATCH_COREDUMP_QUEUE_LEN * sizeof(CoreDumpPayload*)];
    StaticQueue_t coredump_queue_ctrl;
    QueueHandle_t coredump_queue;
#endif

#if CONFIG_HEAPWATCH_ENABLE_STREAM_INGRESS
    stream_ingress::mux::OutboundSlot stream_outbound_slots[HEAPWATCH_STREAM_OUTBOUND_QUEUE_DEPTH];
    char stream_coalesce_buf[stream_ingress::mux::COALESCE_MAX_BYTES];
    uint8_t stream_encode_buf[stream_ingress::mux::ENCODE_MAX];
    uint8_t stream_inbound_buf[stream_ingress::mux::MAX_PAYLOAD_LEN];
    struct StreamOutboundQueued {
        uint8_t slot_index;
    };
    alignas(8) uint8_t stream_outbound_queue_storage[HEAPWATCH_STREAM_OUTBOUND_QUEUE_DEPTH *
                                                     sizeof(StreamOutboundQueued)];
    StaticQueue_t stream_outbound_queue_ctrl;
    StaticSemaphore_t stream_mutex_buf;
    QueueHandle_t stream_outbound_queue;
    SemaphoreHandle_t stream_mutex;
#endif

    alignas(8) uint8_t scratch[HEAPWATCH_MEM_SCRATCH_BYTES];
    size_t scratch_used;
};

}  // namespace heapwatch
