#pragma once

#include <cstddef>

namespace heapwatch::device_fs {

/**
 * Handle an inbound filesystem MQTT command (topic suffix fs/...).
 * Returns true when suffix starts with "fs/".
 * Publishes MQTT ack/nak and queues work for hw_stream_ingress (no dedicated FS task).
 */
bool handleMqttCommand(const char* suffix, const char* command_id, const char* payload,
                       size_t payload_len);

void init();

/**
 * Non-blocking: run at most one queued FS work item.
 * Must be called from hw_stream_ingress so TCP drain stays on one task.
 */
void pollPendingWork();

}  // namespace heapwatch::device_fs
