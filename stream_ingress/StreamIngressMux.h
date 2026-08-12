#pragma once

#include <cstddef>
#include <cstdint>

namespace heapwatch::stream_ingress::mux {

constexpr uint8_t kVersion1 = 1;
constexpr uint8_t kFrameTypeData = 1;
constexpr uint8_t kFrameTypeStdin = 2;
constexpr uint8_t kFrameTypeCtrl = 3;

constexpr size_t kMaxChannelIdLen = 64;
/** Hard cap for one mux frame payload — light streaming chunk size. */
constexpr size_t kMaxPayloadLen = 4 * 1024;
constexpr size_t kCoalesceMaxBytes = 4096;

struct Frame {
    char channel_id[kMaxChannelIdLen + 1];
    uint8_t type = kFrameTypeData;
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
