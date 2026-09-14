#include "StreamIngressClient.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cinttypes>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "../heapwatch_connector/HeapwatchConnector.h"
#include "../heapwatch_connector/heapwatch_freertos_alloc.h"
#include "../memory_manager/HeapwatchMemoryManager.h"
#include "../device_credentials/DeviceCredentialsStore.h"
#include "../heapwatch_endpoints.h"
#include "../device_fs/DeviceFsCommandHandler.h"
#include "../device_command/CommandCatalog.h"
#include "../device_command/ConfigCatalog.h"
#include "../device_command/ConfigSnapshot.h"
#include "StreamIngressMux.h"

namespace heapwatch::stream_ingress {

char g_runtime_stream_api_key[HEAPWATCH_MAX_STREAM_API_KEY_LEN] = {};

namespace {

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
constexpr uint32_t WORKER_STACK_BYTES = 10240;

struct OutboundQueued {
    uint8_t slot_index = 0;
};

SemaphoreHandle_t g_mutex = nullptr;
QueueHandle_t g_outbound_queue = nullptr;
TaskHandle_t g_worker_task = nullptr;
volatile bool g_stop_requested = false;
volatile bool g_worker_exited = true;

bool g_tcp_requested = false;
bool g_log_capture_requested = false;
bool g_close_requested = false;
bool g_connected = false;
bool g_boot_stream_upload_requested = false;
int g_socket_fd = -1;

FsUploadChunkHandler g_fs_upload_handler = nullptr;

char* g_coalesce_buf = nullptr;
size_t g_coalesce_len = 0;
uint64_t g_coalesce_uptime_ms = 0;

/** Single-threaded stream_ingress worker owns these — no malloc on the hot path. */
mux::OutboundSlot* g_outbound_slots = nullptr;
uint8_t* g_encode_buf = nullptr;
uint8_t* g_inbound_buf = nullptr;

void bindStreamWorkingSet() {
#if CONFIG_HEAPWATCH_ENABLE_STREAM_INGRESS
    static_assert(sizeof(OutboundQueued) == sizeof(heapwatch::HeapwatchWorkingSet::StreamOutboundQueued),
                  "stream outbound queue item size must match the working set");
    heapwatch::HeapwatchWorkingSet* ws = heapwatch::memory_manager::get();
    if (ws == nullptr) {
        return;
    }
    g_mutex = ws->stream_mutex;
    g_outbound_queue = ws->stream_outbound_queue;
    g_coalesce_buf = ws->stream_coalesce_buf;
    g_outbound_slots = ws->stream_outbound_slots;
    g_encode_buf = ws->stream_encode_buf;
    g_inbound_buf = ws->stream_inbound_buf;
#endif
}

void unbindStreamWorkingSet() {
    g_mutex = nullptr;
    g_outbound_queue = nullptr;
    g_coalesce_buf = nullptr;
    g_outbound_slots = nullptr;
    g_encode_buf = nullptr;
    g_inbound_buf = nullptr;
}

bool streamIngressEnabled() {
#if CONFIG_HEAPWATCH_ENABLE_STREAM_INGRESS
    return g_runtime_stream_api_key[0] != '\0';
#else
    return false;
#endif
}

const char* activeStreamApiKey() {
    if (g_runtime_stream_api_key[0] != '\0') {
        return g_runtime_stream_api_key;
    }
    return "";
}

void lock() {
    if (g_mutex != nullptr) {
        xSemaphoreTake(g_mutex, portMAX_DELAY);
    }
}

void unlock() {
    if (g_mutex != nullptr) {
        xSemaphoreGive(g_mutex);
    }
}

void closeSocket() {
    if (g_socket_fd >= 0) {
        close(g_socket_fd);
        g_socket_fd = -1;
    }
    g_connected = false;

    // Drop any queued outbound slots — never hold copies across reconnect.
    if (g_outbound_queue != nullptr) {
        OutboundQueued queued = {};
        while (xQueueReceive(g_outbound_queue, &queued, 0) == pdTRUE) {
            if (queued.slot_index < HEAPWATCH_STREAM_OUTBOUND_QUEUE_DEPTH) {
                g_outbound_slots[queued.slot_index].busy = false;
                g_outbound_slots[queued.slot_index].len = 0;
            }
        }
    }
}

bool writeAll(int socket_fd, const uint8_t* data, size_t len) {
    size_t offset = 0;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);

    while (offset < len) {
        if (xTaskGetTickCount() >= deadline) {
            return false;
        }

        const ssize_t sent = send(socket_fd, data + offset, len - offset, MSG_DONTWAIT);
        if (sent > 0) {
            offset += static_cast<size_t>(sent);
            continue;
        }

        if (sent == 0) {
            return false;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        return false;
    }

    return true;
}

bool sendHandshake(int socket_fd) {
    const char* stream_key = activeStreamApiKey();
    if (stream_key == nullptr || stream_key[0] == '\0') {
        return false;
    }
    const char* tenant = heapwatch_get_tenant_slug();
    const char* device_uid = heapwatch_get_device_uid();
    const uint32_t boot_cycle = heapwatch_get_boot_cycle();

    char handshake[HANDSHAKE_MAX] = {};
    const int32_t written = snprintf(
        handshake, sizeof(handshake),
        "{\"stream_api_key\":\"%s\",\"tenant_slug\":\"%s\",\"device_uid\":\"%s\",\"boot_cycle\":%" PRIu32 "}\n",
        stream_key, tenant, device_uid, static_cast<uint32_t>(boot_cycle));
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(handshake)) {
        return false;
    }

    return writeAll(socket_fd, reinterpret_cast<const uint8_t*>(handshake), static_cast<size_t>(written));
}

bool connectTcp() {
    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(HEAPWATCH_STREAM_INGRESS_TCP_PORT);

    if (inet_aton(HEAPWATCH_STREAM_INGRESS_HOST, &dest.sin_addr) == 0) {
        ESP_LOGE(TAG, "Invalid stream ingress host: %s", HEAPWATCH_STREAM_INGRESS_HOST);
        return false;
    }

    const int socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_fd < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        return false;
    }

    if (connect(socket_fd, reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest)) != 0) {
        ESP_LOGE(TAG, "connect(%s:%" PRId32 ") failed: errno=%d", HEAPWATCH_STREAM_INGRESS_HOST,
                 static_cast<int32_t>(HEAPWATCH_STREAM_INGRESS_TCP_PORT), errno);
        close(socket_fd);
        return false;
    }

    const int flags = fcntl(socket_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        ESP_LOGE(TAG, "Failed to set socket non-blocking: errno=%d", errno);
        close(socket_fd);
        return false;
    }

    if (!sendHandshake(socket_fd)) {
        ESP_LOGE(TAG, "Stream handshake write failed");
        close(socket_fd);
        return false;
    }

    lock();
    if (g_connected && g_socket_fd >= 0) {
        close(socket_fd);
        unlock();
        return true;
    }

    g_socket_fd = socket_fd;
    g_connected = true;
    unlock();

    ESP_LOGI(TAG, "Stream ingress TCP connected to %s:%" PRId32, HEAPWATCH_STREAM_INGRESS_HOST,
             static_cast<int32_t>(HEAPWATCH_STREAM_INGRESS_TCP_PORT));
    return true;
}

bool enqueueOutbound(const char* channel_id, uint8_t frame_type, const uint8_t* payload,
                     size_t payload_len) {
    if (g_outbound_queue == nullptr || channel_id == nullptr) {
        return false;
    }

    if (frame_type == mux::FRAME_TYPE_CTRL) {
        if (payload_len != 0) {
            return false;
        }
    } else if (payload == nullptr || payload_len == 0 || payload_len > mux::MAX_PAYLOAD_LEN) {
        return false;
    }

    int32_t free_slot = -1;
    for (size_t i = 0; i < HEAPWATCH_STREAM_OUTBOUND_QUEUE_DEPTH; ++i) {
        if (!g_outbound_slots[i].busy) {
            free_slot = static_cast<int32_t>(i);
            break;
        }
    }
    if (free_slot < 0) {
        return false;
    }

    mux::OutboundSlot& slot = g_outbound_slots[free_slot];
    snprintf(slot.channel_id, sizeof(slot.channel_id), "%s", channel_id);
    slot.frame_type = frame_type;
    if (payload_len > 0 && payload != nullptr) {
        memcpy(slot.data, payload, payload_len);
    }
    slot.len = payload_len;
    slot.busy = true;

    OutboundQueued queued = {};
    queued.slot_index = static_cast<uint8_t>(free_slot);
    if (xQueueSend(g_outbound_queue, &queued, 0) != pdTRUE) {
        slot.busy = false;
        slot.len = 0;
        return false;
    }

    return true;
}

void drainOutboundLocked() {
    if (!g_connected || g_socket_fd < 0) {
        return;
    }

    OutboundQueued queued = {};
    while (xQueueReceive(g_outbound_queue, &queued, 0) == pdTRUE) {
        if (queued.slot_index >= HEAPWATCH_STREAM_OUTBOUND_QUEUE_DEPTH) {
            continue;
        }

        mux::OutboundSlot& slot = g_outbound_slots[queued.slot_index];
        if (!slot.busy) {
            slot.busy = false;
            continue;
        }

        if (slot.len == 0 && slot.frame_type != mux::FRAME_TYPE_CTRL) {
            slot.busy = false;
            continue;
        }

        size_t encoded_len = 0;

        mux::Frame frame = {};
        snprintf(frame.channel_id, sizeof(frame.channel_id), "%s", slot.channel_id);
        frame.type = slot.frame_type;
        frame.uptime_ms = 0;
        frame.payload = slot.len > 0 ? slot.data : nullptr;
        frame.payload_len = slot.len;

        if (!mux::encode(frame, g_encode_buf, mux::ENCODE_MAX, &encoded_len)) {
            ESP_LOGW(TAG, "Failed to encode mux frame on channel %s", slot.channel_id);
            slot.busy = false;
            slot.len = 0;
            continue;
        }

        const bool wrote = writeAll(g_socket_fd, g_encode_buf, encoded_len);
        slot.busy = false;
        slot.len = 0;

        if (!wrote) {
            ESP_LOGW(TAG, "Stream write failed on channel %s", frame.channel_id);
            closeSocket();
            return;
        }
    }
}

bool sendMuxFrame(const char* channel_id, uint8_t frame_type, const uint8_t* payload,
                  size_t payload_len) {
    if (channel_id == nullptr) {
        return false;
    }

    lock();
    const bool queued = enqueueOutbound(channel_id, frame_type, payload, payload_len);
    unlock();

    if (!queued) {
        return false;
    }

    ensureTcpSession();
    return true;
}

void handleInboundFrame(const mux::Frame& frame) {
    if (frame.type == mux::FRAME_TYPE_STDIN) {
        char line[STDIN_PAYLOAD_MAX + 1] = {};
        const size_t copy_len =
            frame.payload_len < STDIN_PAYLOAD_MAX ? frame.payload_len : STDIN_PAYLOAD_MAX;
        if (frame.payload != nullptr && copy_len > 0) {
            memcpy(line, frame.payload, copy_len);
        }
        ESP_LOGI(TAG, "stdin [%s]: %s", frame.channel_id, line);
        return;
    }

    if (frame.type != mux::FRAME_TYPE_DATA) {
        return;
    }

    if (strncmp(frame.channel_id, CHANNEL_FS_UPLOAD, strlen(CHANNEL_FS_UPLOAD)) == 0) {
        const char* suffix = frame.channel_id + strlen(CHANNEL_FS_UPLOAD);
        if (suffix[0] == '/') {
            ++suffix;
        } else if (suffix[0] != '\0') {
            return;
        }

        if (g_fs_upload_handler != nullptr && frame.payload != nullptr && frame.payload_len > 0) {
            g_fs_upload_handler(frame.payload, frame.payload_len);
        }
    }
}

bool flushCoalesceLocked() {
    if (g_coalesce_len == 0 || g_socket_fd < 0) {
        return true;
    }

    size_t encoded_len = 0;

    mux::Frame frame = {};
    snprintf(frame.channel_id, sizeof(frame.channel_id), "%s", CHANNEL_LOGS);
    frame.type = mux::FRAME_TYPE_DATA;
    frame.uptime_ms = g_coalesce_uptime_ms;
    frame.payload = reinterpret_cast<const uint8_t*>(g_coalesce_buf);
    frame.payload_len = g_coalesce_len;

    if (!mux::encode(frame, g_encode_buf, mux::ENCODE_MAX, &encoded_len)) {
        ESP_LOGW(TAG, "Failed to encode coalesced log frame (%" PRIu32 " bytes)", static_cast<uint32_t>(g_coalesce_len));
        g_coalesce_len = 0;
        return false;
    }

    const bool ok = writeAll(g_socket_fd, g_encode_buf, encoded_len);
    if (!ok) {
        ESP_LOGW(TAG, "Stream write failed during coalesce flush");
        return false;
    }

    g_coalesce_len = 0;
    return true;
}

bool tryReadInbound() {
    if (g_socket_fd < 0) {
        return false;
    }

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(g_socket_fd, &read_fds);

    struct timeval timeout = {};
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;

    const int32_t ready = select(g_socket_fd + 1, &read_fds, nullptr, nullptr, &timeout);
    if (ready <= 0) {
        return ready == 0;
    }

    mux::Frame frame = {};
    const bool decoded =
        mux::decodeFromSocket(g_socket_fd, frame, g_inbound_buf, mux::MAX_PAYLOAD_LEN);
    if (decoded) {
        handleInboundFrame(frame);
    }
    return decoded;
}

void workerTask(void* arg) {
    (void)arg;
    TickType_t last_flush = xTaskGetTickCount();

    while (!g_stop_requested) {
        lock();
        const bool close_requested = g_close_requested;
        const bool tcp_requested = g_tcp_requested;
        unlock();

        if (close_requested) {
            lock();
            flushCoalesceLocked();
            closeSocket();
            g_tcp_requested = false;
            g_log_capture_requested = false;
            g_close_requested = false;
            g_coalesce_len = 0;
            unlock();
            vTaskDelay(pdMS_TO_TICKS(WORKER_POLL_MS));
            continue;
        }

        if (!g_connected) {
            if (!tcp_requested || !streamIngressEnabled()) {
                vTaskDelay(pdMS_TO_TICKS(WORKER_POLL_MS));
                continue;
            }

            if (!connectTcp()) {
                vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
                continue;
            }
            last_flush = xTaskGetTickCount();
            lock();
            drainOutboundLocked();
            unlock();
        }

        if (!tryReadInbound()) {
            lock();
            flushCoalesceLocked();
            closeSocket();
            unlock();
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }

        const TickType_t now = xTaskGetTickCount();
        if ((now - last_flush) >= pdMS_TO_TICKS(COALESCE_FLUSH_MS)) {
            lock();
            if (!flushCoalesceLocked()) {
                closeSocket();
            } else {
                drainOutboundLocked();
            }
            unlock();
            last_flush = now;
        } else {
            lock();
            drainOutboundLocked();
            unlock();
        }

        // FS work shares this task — no dedicated hw_device_fs / hw_fs_download stacks.
        heapwatch::device_fs::pollPendingWork();

        bool command_catalog_force = false;
        if (heapwatch::device_command::consumeCommandCatalogUploadRequest(&command_catalog_force)) {
            heapwatch::device_command::uploadCommandCatalog(command_catalog_force);
        }

        bool catalog_force = false;
        if (heapwatch::device_command::consumeConfigCatalogUploadRequest(&catalog_force)) {
            heapwatch::device_command::uploadConfigCatalog(catalog_force);
        }

        char snapshot_namespace[32] = {};
        bool snapshot_force = false;
        if (heapwatch::device_command::consumeConfigSnapshotUploadRequest(
                snapshot_namespace, sizeof(snapshot_namespace), &snapshot_force)) {
            heapwatch::device_command::uploadConfigSnapshot(snapshot_namespace, snapshot_force);
        }

        lock();
        const bool boot_uploads_pending = g_boot_stream_upload_requested;
        unlock();
        if (boot_uploads_pending) {
            if (!heapwatch::device_command::commandCatalogUploadedThisBoot()) {
                heapwatch::device_command::uploadCommandCatalog(false);
            }
            if (!heapwatch::device_command::configCatalogUploadedThisBoot()) {
                heapwatch::device_command::uploadConfigCatalog(false);
            }
            if (!heapwatch::device_command::configSnapshotUploadedThisBoot()) {
                heapwatch::device_command::uploadConfigSnapshot("*", false);
            }
            if (heapwatch::device_command::commandCatalogUploadedThisBoot() &&
                heapwatch::device_command::configCatalogUploadedThisBoot() &&
                heapwatch::device_command::configSnapshotUploadedThisBoot()) {
                lock();
                g_boot_stream_upload_requested = false;
                unlock();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    lock();
    flushCoalesceLocked();
    closeSocket();
    unlock();
    g_worker_exited = true;
    g_worker_task = nullptr;
    heapwatch_task_delete_dram(nullptr);
}

}  // namespace

void setStreamApiKey(const char* stream_api_key) {
    if (stream_api_key == nullptr) {
        g_runtime_stream_api_key[0] = '\0';
        return;
    }

    std::strncpy(g_runtime_stream_api_key, stream_api_key, sizeof(g_runtime_stream_api_key) - 1);
    g_runtime_stream_api_key[sizeof(g_runtime_stream_api_key) - 1] = '\0';
}

static void ensureWorkerStarted() {
    if (!streamIngressEnabled()) {
        return;
    }
    bindStreamWorkingSet();
    if (g_worker_task == nullptr) {
        g_stop_requested = false;
        g_worker_exited = false;
        // Create only when TCP is actually needed — saves ~8KiB stack at boot for MQTT.
        if (heapwatch_task_create_dram(workerTask, "hw_streaming", WORKER_STACK_BYTES, nullptr,
                                       tskIDLE_PRIORITY + 2, &g_worker_task) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create hw_streaming task");
            g_worker_task = nullptr;
            g_worker_exited = true;
        }
    }
}

void init() {
    if (!streamIngressEnabled()) {
        ESP_LOGI(TAG, "Stream ingress disabled (missing stream API key or feature toggle)");
        return;
    }

    // Worker task is deferred until ensureTcpSession/requestOpen
    // so boot heap stays large enough for the MQTT client task.
    bindStreamWorkingSet();
    ESP_LOGI(TAG, "Stream ingress ready (worker deferred until TCP needed)");
}

void requestOpen() {
    if (!streamIngressEnabled()) {
        ESP_LOGW(TAG, "stream_open ignored: stream ingress not configured");
        return;
    }

    ensureWorkerStarted();

    lock();
    g_tcp_requested = true;
    g_log_capture_requested = true;
    g_close_requested = false;
    unlock();

    ESP_LOGI(TAG, "Stream ingress open requested");
}

void requestClose() {
    lock();
    g_close_requested = true;
    unlock();

    ESP_LOGI(TAG, "Stream ingress close requested");
}

void shutdown() {
    g_stop_requested = true;
    requestClose();
    // Generous: the worker also runs filesystem jobs, which can be mid-hash.
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
    while (!g_worker_exited && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!g_worker_exited) {
        ESP_LOGE(TAG, "hw_streaming did not stop in time — leaving it running");
        return;
    }

    g_worker_task = nullptr;
    g_tcp_requested = false;
    g_log_capture_requested = false;
    g_close_requested = false;
    g_boot_stream_upload_requested = false;
    g_stop_requested = false;
    unbindStreamWorkingSet();
}

bool workerExited() { return g_worker_exited; }

bool isShuttingDown() { return g_stop_requested; }

bool ensureTcpSession() {
    if (!streamIngressEnabled()) {
        return false;
    }

    ensureWorkerStarted();
    if (g_worker_task == nullptr) {
        return false;
    }

    lock();
    g_tcp_requested = true;
    g_close_requested = false;
    unlock();
    return true;
}

bool isCaptureActive() {
    lock();
    const bool active = g_log_capture_requested && !g_close_requested;
    unlock();
    return active;
}

bool isActive() {
    return g_connected;
}

void enqueueLogJson(const char* json_line) {
    if (!streamIngressEnabled() || json_line == nullptr || json_line[0] == '\0') {
        return;
    }

    if (!isCaptureActive() || !g_connected) {
        return;
    }

    lock();

    const size_t line_len = strlen(json_line);
    const size_t needed = line_len + 1;
    if (needed > mux::COALESCE_MAX_BYTES) {
        unlock();
        return;
    }

    if (g_coalesce_len > 0 && (g_coalesce_len + needed) > mux::COALESCE_MAX_BYTES) {
        if (!flushCoalesceLocked()) {
            closeSocket();
            unlock();
            return;
        }
    }

    if (g_coalesce_len == 0) {
        g_coalesce_uptime_ms = 0;
    }

    memcpy(g_coalesce_buf + g_coalesce_len, json_line, line_len);
    g_coalesce_len += line_len;
    g_coalesce_buf[g_coalesce_len++] = '\n';

    unlock();
}

bool sendMuxDataFrame(const char* channel_id, const uint8_t* payload, size_t payload_len) {
    return sendMuxFrame(channel_id, mux::FRAME_TYPE_DATA, payload, payload_len);
}

bool sendMuxCtrlFrame(const char* channel_id) {
    return sendMuxFrame(channel_id, mux::FRAME_TYPE_CTRL, nullptr, 0);
}

bool sendFileBrowserJson(const char* json_line) {
    if (json_line == nullptr || json_line[0] == '\0') {
        return false;
    }
    return sendMuxDataFrame(CHANNEL_FILE_BROWSER, reinterpret_cast<const uint8_t*>(json_line),
                            strlen(json_line));
}

bool sendBleScanJson(const char* json_line) {
    if (json_line == nullptr || json_line[0] == '\0') {
        return false;
    }
    return sendMuxDataFrame(CHANNEL_BLE_SCAN, reinterpret_cast<const uint8_t*>(json_line),
                            strlen(json_line));
}

bool sendBleEventJson(const char* json_line) {
    if (json_line == nullptr || json_line[0] == '\0') {
        return false;
    }
    return sendMuxDataFrame(CHANNEL_BLE_EVENTS, reinterpret_cast<const uint8_t*>(json_line),
                            strlen(json_line));
}

bool sendFsDownloadChunk(const char* command_id, const uint8_t* data, size_t len) {
    if (command_id == nullptr || command_id[0] == '\0' || data == nullptr || len == 0) {
        return false;
    }

    char channel_id[mux::MAX_CHANNEL_ID_LEN + 1] = {};
    snprintf(channel_id, sizeof(channel_id), "%s/%s", CHANNEL_FS_DOWNLOAD, command_id);

    return sendMuxDataFrame(channel_id, data, len);
}

void flushOutbound() {
    lock();
    drainOutboundLocked();
    unlock();
}

void setFsUploadChunkHandler(FsUploadChunkHandler handler) {
    g_fs_upload_handler = handler;
}

void ensureBootStreamUploadWorker() {
    if (!streamIngressEnabled()) {
        return;
    }

    ensureWorkerStarted();
    lock();
    g_tcp_requested = true;
    g_close_requested = false;
    unlock();
}

void requestCommandCatalogUpload() {
    heapwatch::device_command::requestCommandCatalogUpload(false);
}

void requestBootStreamUploads() {
    if (!streamIngressEnabled()) {
        return;
    }

    ensureBootStreamUploadWorker();
    lock();
    g_boot_stream_upload_requested = true;
    unlock();
    heapwatch::device_command::requestCommandCatalogUpload(false);
    heapwatch::device_command::requestConfigCatalogUpload(false);
    heapwatch::device_command::requestConfigSnapshotUpload("*", false);
}

}  // namespace heapwatch::stream_ingress
