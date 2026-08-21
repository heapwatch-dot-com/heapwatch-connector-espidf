#pragma once

#include <cstddef>
#include <cstdint>

namespace heapwatch::stream_ingress {

/** Wait for an active stream TCP session (used by hw_streaming upload jobs). */
bool waitForStreamTcp(uint32_t timeout_ms);

/** Stream a JSON body over a mux channel in DATA chunks + CTRL end frame. */
bool streamJsonBody(const char* channel_id, const char* body);

}  // namespace heapwatch::stream_ingress
