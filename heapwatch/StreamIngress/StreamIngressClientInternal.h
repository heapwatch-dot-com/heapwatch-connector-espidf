#pragma once

// Shared between StreamIngressClient.cpp and StreamIngressClient-Worker.cpp.
// Not part of the public StreamIngress API.

#include <cstddef>
#include <cstdint>

#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "heapwatch/StreamIngress/Mux/StreamIngressMux.h"
#include "heapwatch/StreamIngress/StreamIngressClient.h"
#include "heapwatch_config.h"

namespace Heapwatch::StreamIngress {

struct OutboundQueued {
    uint8_t slot_index = 0;
};

extern char g_runtime_stream_api_key[];

extern SemaphoreHandle_t g_mutex;
extern QueueHandle_t g_outbound_queue;
extern TaskHandle_t g_worker_task;
extern volatile bool g_stop_requested;
extern volatile bool g_worker_exited;

extern bool g_tcp_requested;
extern bool g_log_capture_requested;
extern bool g_close_requested;
extern bool g_connected;
extern bool g_boot_stream_upload_requested;
extern esp_tls_t* g_tls;

extern FsUploadChunkHandler g_fs_upload_handler;

extern char* g_coalesce_buf;
extern size_t g_coalesce_len;
extern uint64_t g_coalesce_uptime_ms;

extern Heapwatch::StreamIngress::Mux::OutboundSlot* g_outbound_slots;
extern uint8_t* g_encode_buf;
extern uint8_t* g_inbound_buf;

constexpr char TAG[] = "HwStream";
constexpr char CHANNEL_LOGS[] = "logs";
constexpr char CHANNEL_FILE_BROWSER[] = "file_browser";
constexpr char CHANNEL_BLE_SCAN[] = "ble_scan";
constexpr char CHANNEL_BLE_EVENTS[] = "ble_events";
constexpr char CHANNEL_FS_DOWNLOAD[] = "fs_download";
constexpr char CHANNEL_FS_UPLOAD[] = "fs_upload";

constexpr size_t HANDSHAKE_MAX = 512;
constexpr size_t STDIN_PAYLOAD_MAX = 512;
constexpr uint32_t COALESCE_FLUSH_MS = 100;
constexpr uint32_t WORKER_POLL_MS = 100;
constexpr uint32_t RECONNECT_DELAY_MS = 2000;
constexpr uint32_t WORKER_STACK_BYTES = HEAPWATCH_STREAMING_TASK_STACK_BYTES;

void bindStreamWorkingSet();
void unbindStreamWorkingSet();
bool streamIngressEnabled();
const char* activeStreamApiKey();
void lock();
void unlock();
void closeTlsSession();
bool writeAll(esp_tls_t* tls, const uint8_t* data, size_t len);
bool sendHandshake(esp_tls_t* tls);
bool connectTls();
bool enqueueOutbound(const char* channel_id, uint8_t frame_type, const uint8_t* payload,
                     size_t payload_len);
void drainOutboundLocked();
bool sendMuxFrame(const char* channel_id, uint8_t frame_type, const uint8_t* payload,
                  size_t payload_len);
bool flushCoalesceLocked();

void taskStreamIngress(void* arg);

}  // namespace Heapwatch::StreamIngress
