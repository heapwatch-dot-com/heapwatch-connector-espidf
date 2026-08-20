#pragma once

#include <stddef.h>
#include <stdint.h>

namespace heapwatch::device_command {

/** Create hw_command task + inbound queue (idempotent). */
void initCommandWorker();

/**
 * Enqueue an MQTT command for hw_command (non-blocking).
 * Returns false when oversize or queue full.
 */
bool enqueueMqttCommand(const char* topic, size_t topic_len, const char* payload, size_t payload_len,
                        int mqtt_retain);

/** Enqueue credential rotation work for hw_command (non-blocking). */
bool enqueueCredentialRotation();

}  // namespace heapwatch::device_command
