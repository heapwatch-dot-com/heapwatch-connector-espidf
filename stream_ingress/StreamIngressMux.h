#pragma once

#include <cstddef>
#include <cstdint>

namespace heapwatch::stream_ingress::mux {

constexpr uint8_t VERSION_1 = 1;
constexpr uint8_t FRAME_TYPE_DATA = 1;
constexpr uint8_t FRAME_TYPE_STDIN = 2;
constexpr uint8_t FRAME_TYPE_CTRL = 3;

constexpr size_t MAX_CHANNEL_ID_LEN = 64;
/** Hard cap for one mux frame payload — light streaming chunk size. */
constexpr size_t MAX_PAYLOAD_LEN = 4 * 1024;
constexpr size_t COALESCE_MAX_BYTES = 4096;
constexpr size_t ENCODE_MAX = MAX_PAYLOAD_LEN + 128;

struct OutboundSlot {
    char channel_id[MAX_CHANNEL_ID_LEN + 1] = {};
    size_t len = 0;
    bool busy = false;
    uint8_t frame_type = FRAME_TYPE_DATA;
    uint8_t data[MAX_PAYLOAD_LEN] = {};
};

struct Frame {
    char channel_id[MAX_CHANNEL_ID_LEN + 1];
    uint8_t type = FRAME_TYPE_DATA;
    uint64_t uptime_ms = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
};

/**
 * Serialize a mux frame into out_buf. Returns false if the buffer is too small or inputs are invalid.
 */
bool encode(const Frame& frame, uint8_t* out_buf, size_t out_cap, size_t* out_len);

/**
 * Read exactly one mux frame from a connected TCP socket. Returns false on disconnect or parse error.
 */
bool decodeFromSocket(int socket_fd, Frame& frame, uint8_t* payload_buf, size_t payload_buf_cap);

}  // namespace heapwatch::stream_ingress::mux
