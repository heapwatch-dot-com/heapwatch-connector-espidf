#include "heapwatch/StreamIngress/StreamIngressClientInternal.h"

#include <cstdio>
#include <cstring>
#include <sys/select.h>

#include "esp_log.h"
#include "esp_tls.h"

#include "heapwatch/DeviceCommand/CommandCatalog.h"
#include "heapwatch/DeviceCommand/ConfigCatalog.h"
#include "heapwatch/DeviceCommand/ConfigSnapshot.h"
#include "heapwatch/Fs/DeviceFsCommandHandler.h"
#include "heapwatch/Helpers/FreeRtosAlloc.h"
#include "heapwatch/StreamIngress/Mux/StreamIngressMux.h"

namespace Heapwatch::StreamIngress {

void handleInboundFrame(const Heapwatch::StreamIngress::Mux::Frame& frame) {
    if (frame.type == Heapwatch::StreamIngress::Mux::FRAME_TYPE_STDIN) {
        char line[STDIN_PAYLOAD_MAX + 1] = {};
        const size_t copy_len =
            frame.payload_len < STDIN_PAYLOAD_MAX ? frame.payload_len : STDIN_PAYLOAD_MAX;
        if (frame.payload != nullptr && copy_len > 0) {
            memcpy(line, frame.payload, copy_len);
        }
        ESP_LOGI(TAG, "stdin [%s]: %s", frame.channel_id, line);
        return;
    }

    if (frame.type != Heapwatch::StreamIngress::Mux::FRAME_TYPE_DATA) {
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

bool tryReadInbound() {
    if (g_tls == nullptr) {
        return false;
    }

    int sockfd = -1;
    if (esp_tls_get_conn_sockfd(g_tls, &sockfd) != ESP_OK || sockfd < 0) {
        return false;
    }

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(sockfd, &read_fds);

    struct timeval timeout = {};
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;

    const int32_t ready = select(sockfd + 1, &read_fds, nullptr, nullptr, &timeout);
    if (ready <= 0) {
        return ready == 0;
    }

    Heapwatch::StreamIngress::Mux::Frame frame = {};
    const bool decoded =
        Heapwatch::StreamIngress::Mux::decodeFromTls(g_tls, frame, g_inbound_buf, Heapwatch::StreamIngress::Mux::MAX_PAYLOAD_LEN);
    if (decoded) {
        handleInboundFrame(frame);
    }
    return decoded;
}

void taskStreamIngress(void* arg) {
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
            closeTlsSession();
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

            if (!connectTls()) {
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
            closeTlsSession();
            unlock();
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }

        const TickType_t now = xTaskGetTickCount();
        if ((now - last_flush) >= pdMS_TO_TICKS(COALESCE_FLUSH_MS)) {
            lock();
            if (!flushCoalesceLocked()) {
                closeTlsSession();
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
        Heapwatch::Fs::pollPendingWork();

        bool command_catalog_force = false;
        if (Heapwatch::DeviceCommand::consumeCommandCatalogUploadRequest(&command_catalog_force)) {
            Heapwatch::DeviceCommand::uploadCommandCatalog(command_catalog_force);
        }

        bool catalog_force = false;
        if (Heapwatch::DeviceCommand::consumeConfigCatalogUploadRequest(&catalog_force)) {
            Heapwatch::DeviceCommand::uploadConfigCatalog(catalog_force);
        }

        char snapshot_namespace[32] = {};
        bool snapshot_force = false;
        if (Heapwatch::DeviceCommand::consumeConfigSnapshotUploadRequest(
                snapshot_namespace, sizeof(snapshot_namespace), &snapshot_force)) {
            Heapwatch::DeviceCommand::uploadConfigSnapshot(snapshot_namespace, snapshot_force);
        }

        lock();
        const bool boot_uploads_pending = g_boot_stream_upload_requested;
        unlock();
        if (boot_uploads_pending) {
            if (!Heapwatch::DeviceCommand::commandCatalogUploadedThisBoot()) {
                Heapwatch::DeviceCommand::uploadCommandCatalog(false);
            }
            if (!Heapwatch::DeviceCommand::configCatalogUploadedThisBoot()) {
                Heapwatch::DeviceCommand::uploadConfigCatalog(false);
            }
            if (!Heapwatch::DeviceCommand::configSnapshotUploadedThisBoot()) {
                Heapwatch::DeviceCommand::uploadConfigSnapshot("*", false);
            }
            if (Heapwatch::DeviceCommand::commandCatalogUploadedThisBoot() &&
                Heapwatch::DeviceCommand::configCatalogUploadedThisBoot() &&
                Heapwatch::DeviceCommand::configSnapshotUploadedThisBoot()) {
                lock();
                g_boot_stream_upload_requested = false;
                unlock();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    lock();
    flushCoalesceLocked();
    closeTlsSession();
    unlock();
    g_worker_exited = true;
    g_worker_task = nullptr;
    heapwatch_task_delete_dram(nullptr);
}


}  // namespace Heapwatch::StreamIngress
