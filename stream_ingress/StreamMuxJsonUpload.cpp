#include "StreamMuxJsonUpload.h"

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "StreamIngressClient.h"
#include "StreamIngressMux.h"

namespace heapwatch::stream_ingress {

namespace {

constexpr uint32_t kMuxSendRetryMs = 30000;

bool sendCtrlEnd(const char* channel_id) {
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(kMuxSendRetryMs);
    while (xTaskGetTickCount() < deadline) {
        if (sendMuxCtrlFrame(channel_id)) {
            flushOutbound();
            return true;
        }
        flushOutbound();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

bool sendDataChunk(const char* channel_id, const uint8_t* data, size_t len) {
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(kMuxSendRetryMs);
    while (xTaskGetTickCount() < deadline) {
        if (sendMuxDataFrame(channel_id, data, len)) {
            flushOutbound();
            return true;
        }
        flushOutbound();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

}  // namespace

bool waitForStreamTcp(uint32_t timeout_ms) {
    if (!ensureTcpSession()) {
        return false;
    }

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        if (isActive()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return false;
}

bool streamJsonBody(const char* channel_id, const char* body) {
    if (channel_id == nullptr || body == nullptr) {
        return false;
    }

    const size_t total_len = strlen(body);
    size_t offset = 0;

    while (offset < total_len) {
        const size_t chunk_len =
            (total_len - offset) > mux::kMaxPayloadLen ? mux::kMaxPayloadLen : (total_len - offset);

        if (!sendDataChunk(channel_id, reinterpret_cast<const uint8_t*>(body + offset), chunk_len)) {
            return false;
        }

        offset += chunk_len;
    }

    return sendCtrlEnd(channel_id);
}

}  // namespace heapwatch::stream_ingress
