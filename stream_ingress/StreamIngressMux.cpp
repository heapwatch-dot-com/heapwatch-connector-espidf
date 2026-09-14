#include "StreamIngressMux.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace heapwatch::stream_ingress::mux {

namespace {

void writeU64BE(uint8_t* out, uint64_t value) {
    for (int32_t i = 0; i < 8; i++) {
        out[7 - i] = static_cast<uint8_t>((value >> (static_cast<uint32_t>(i) * 8)) & 0xffU);
    }
}

uint64_t readU64BE(const uint8_t* in) {
    uint64_t value = 0;
    for (int32_t i = 0; i < 8; i++) {
        value = (value << 8) | static_cast<uint64_t>(in[i]);
    }
    return value;
}

bool readExact(int socket_fd, uint8_t* buffer, size_t len) {
    size_t offset = 0;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);

    while (offset < len) {
        if (xTaskGetTickCount() >= deadline) {
            return false;
        }

        const ssize_t received =
            recv(socket_fd, buffer + offset, len - offset, MSG_DONTWAIT);
        if (received > 0) {
            offset += static_cast<size_t>(received);
            continue;
        }

        if (received == 0) {
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

bool channelIdValid(const char* channel_id, size_t len) {
    return channel_id != nullptr && len > 0 && len <= MAX_CHANNEL_ID_LEN;
}

}  // namespace

bool encode(const Frame& frame, uint8_t* out_buf, size_t out_cap, size_t* out_len) {
    if (out_buf == nullptr || out_len == nullptr) {
        return false;
    }

    const char* channel_id = frame.channel_id;
    if (channel_id == nullptr || channel_id[0] == '\0') {
        channel_id = "console";
    }

    const size_t channel_len = strlen(channel_id);
    if (!channelIdValid(channel_id, channel_len)) {
        return false;
    }

    if (frame.payload_len > MAX_PAYLOAD_LEN) {
        return false;
    }

    if (frame.type != FRAME_TYPE_DATA && frame.type != FRAME_TYPE_STDIN && frame.type != FRAME_TYPE_CTRL) {
        return false;
    }

    const size_t header_len = 1 + 1 + 2 + channel_len + 8 + 4;
    const size_t total_len = header_len + frame.payload_len;
    if (total_len > out_cap) {
        return false;
    }

    out_buf[0] = VERSION_1;
    out_buf[1] = frame.type;
    const uint16_t channel_len_be = htons(static_cast<uint16_t>(channel_len));
    memcpy(out_buf + 2, &channel_len_be, sizeof(channel_len_be));
    memcpy(out_buf + 4, channel_id, channel_len);

    size_t offset = 4 + channel_len;
    writeU64BE(out_buf + offset, frame.uptime_ms);
    offset += 8;

    const uint32_t payload_len_be = htonl(static_cast<uint32_t>(frame.payload_len));
    memcpy(out_buf + offset, &payload_len_be, sizeof(payload_len_be));
    offset += 4;

    if (frame.payload_len > 0 && frame.payload != nullptr) {
        memcpy(out_buf + offset, frame.payload, frame.payload_len);
    }

    *out_len = total_len;
    return true;
}

bool decodeFromSocket(int socket_fd, Frame& frame, uint8_t* payload_buf, size_t payload_buf_cap) {
    if (payload_buf == nullptr) {
        return false;
    }

    uint8_t header[4] = {};
    if (!readExact(socket_fd, header, sizeof(header))) {
        return false;
    }

    if (header[0] != VERSION_1) {
        return false;
    }

    const uint8_t frame_type = header[1];
    if (frame_type != FRAME_TYPE_DATA && frame_type != FRAME_TYPE_STDIN && frame_type != FRAME_TYPE_CTRL) {
        return false;
    }

    const uint16_t channel_len = ntohs(*reinterpret_cast<uint16_t*>(header + 2));
    if (channel_len == 0 || channel_len > MAX_CHANNEL_ID_LEN) {
        return false;
    }

    uint8_t channel_bytes[MAX_CHANNEL_ID_LEN] = {};
    if (!readExact(socket_fd, channel_bytes, channel_len)) {
        return false;
    }

    uint8_t timing[12] = {};
    if (!readExact(socket_fd, timing, sizeof(timing))) {
        return false;
    }

    const uint64_t uptime_ms = readU64BE(timing);
    const uint32_t payload_len = ntohl(*reinterpret_cast<uint32_t*>(timing + 8));
    if (payload_len > MAX_PAYLOAD_LEN || payload_len > payload_buf_cap) {
        return false;
    }

    if (payload_len > 0) {
        if (!readExact(socket_fd, payload_buf, payload_len)) {
            return false;
        }
    }

    memset(frame.channel_id, 0, sizeof(frame.channel_id));
    const size_t copy_len = channel_len < sizeof(frame.channel_id) - 1 ? channel_len
                                                                         : sizeof(frame.channel_id) - 1;
    memcpy(frame.channel_id, channel_bytes, copy_len);

    frame.type = frame_type;
    frame.uptime_ms = uptime_ms;
    frame.payload = payload_buf;
    frame.payload_len = payload_len;
    return true;
}

}  // namespace heapwatch::stream_ingress::mux
