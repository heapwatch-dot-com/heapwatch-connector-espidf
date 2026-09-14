#pragma once

#include <stddef.h>
#include <stdint.h>

#include "heapwatch_config.h"

namespace heapwatch::device_command {

enum class InboundKind : uint8_t {
    MqttCommand = 0,
    CredentialRotation = 1,
    Stop = 2,
};

struct CommandInboundMessage {
    InboundKind kind = InboundKind::MqttCommand;
    uint8_t mqtt_retain = 0;
    uint16_t topic_len = 0;
    uint16_t payload_len = 0;
    char topic[HEAPWATCH_COMMAND_TOPIC_MAX] = {};
    char payload[HEAPWATCH_COMMAND_PAYLOAD_MAX] = {};
};

/** Create hw_command task (idempotent). Queue storage lives in the working set. */
void initCommandWorker();

void shutdownCommandWorker();

/** True when hw_command is not running (never started, or stopped). */
bool commandWorkerExited();

/**
 * Enqueue an MQTT command for hw_command (non-blocking).
 * Returns false when oversize or queue full.
 */
bool enqueueMqttCommand(const char* topic, size_t topic_len, const char* payload, size_t payload_len,
                        int32_t mqtt_retain);

/** Enqueue credential rotation work for hw_command (non-blocking). */
bool enqueueCredentialRotation();

}  // namespace heapwatch::device_command
