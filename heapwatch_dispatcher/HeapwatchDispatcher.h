// Copyright (c) 2026 Heapwatch. All rights reserved.
//
// This software is part of the Heapwatch Connector for ESP-IDF project.
// Unauthorized copying, modification, or distribution of this software,
// via any medium, is strictly prohibited unless otherwise permitted.
//
// This software is provided "as is", without warranty of any kind, express or implied,
// including but not limited to the warranties of merchantability, fitness for a particular purpose,
// and noninfringement. In no event shall the authors or copyright holders be liable for any claim,
// damages, or other liability, whether in an action of contract, tort, or otherwise, arising from,
// out of, or in connection with the software or the use or other dealings in the software.
//
// For licensing inquiries, please contact: info@heapwatch.com
//

#pragma once

#include <cstdint>

/* ============================================================================
   MQTT Dispatcher - Header File
   =========================================================================== */

/**
 * @brief MQTT Dispatcher Module
 * 
 * Handles MQTT connection management and message publishing from the shared queue.
 * Runs in a dedicated thread that:
 * - Maintains persistent MQTT connection with automatic reconnection
 * - Receives messages from the shared queue (portMAX_DELAY)
 * - Publishes messages to appropriate topics based on message type
 * 
 * Thread Priority: IDLE_PRIORITY + 3 (highest among SDK threads)
 */

/* ============================================================================
   Connection State
   =========================================================================== */

/**
 * @brief MQTT connection state flag
 * 
 * Set to true when connected, false when disconnected.
 * Used by the dispatcher thread to determine whether to publish messages.
 */
extern volatile bool g_mqtt_connected;

/**
 * @brief True after a destructive command (e.g. restart) arms shutdown.
 *
 * While set, no new MQTT publishes are enqueued; the dispatcher drains the
 * esp-mqtt outbox then calls esp_restart().
 */
extern volatile bool g_mqtt_shutting_down;

enum class HeapwatchShutdownAction : uint8_t {
    Restart = 0,
    SleepHandoff = 1,
};

bool heapwatch_dispatcher_is_shutting_down(void);

/**
 * Arm shutdown: block new connector publishes. On outbox drain:
 * - Restart → esp_restart()
 * - SleepHandoff → going-dark task continues (no reboot)
 */
void heapwatch_dispatcher_begin_shutdown(HeapwatchShutdownAction action);

/** Cancel shutdown after a brief_offline sleep wake (light sleep only). */
void heapwatch_dispatcher_disarm_shutdown(void);

/** Bytes waiting in the esp-mqtt outbox (0 when disconnected). */
int heapwatch_dispatcher_get_outbox_size(void);

/**
 * @brief Arm shutdown: block new publishes and reboot once the outbox is empty.
 *
 * The restart command ack must be enqueued before calling this.
 */
void heapwatch_dispatcher_begin_shutdown(void);

/* ============================================================================
   Dispatcher Task Function
   =========================================================================== */

/**
 * @brief MQTT dispatcher task function
 * 
 * Main loop that:
 * 1. Ensures MQTT client is connected (creates/reconnects if needed)
 * 2. Receives messages from the shared queue
 * 3. Publishes messages to appropriate topics
 * 
 * @param arg Task argument (unused, set to nullptr)
 */
void heapwatch_dispatch_task(void* arg);

/* ============================================================================
   Initialization Functions
   =========================================================================== */

/**
 * @brief Initialize MQTT dispatcher state
 * 
 * Should be called before starting the dispatcher thread.
 * 
 * @return true if initialization succeeded
 */
bool heapwatch_dispatcher_init();

/**
 * @brief Shutdown MQTT dispatcher and cleanup resources
 * 
 * Destroys MQTT client and frees any allocated resources.
 */
void heapwatch_dispatcher_shutdown();

/**
 * @brief Force the MQTT client to reconnect (e.g. after password rotation).
 */
void heapwatch_dispatcher_request_reconnect();

/**
 * @brief Publish a JSON payload to {tenant}/response/device/{uid}/{suffix}.
 */
bool heapwatch_dispatcher_publish_command_response(const char* suffix, const char* json_payload);

/**
 * @brief STAT1: whether the MQTT client currently reports itself connected.
 *
 * Read-only snapshot of g_mqtt_connected for HeapwatchConnector's synchronous
 * status API (heapwatch_get_status()) — kept as a function rather than
 * exposing the volatile directly outside this module.
 */
bool heapwatch_dispatcher_is_mqtt_connected();

/**
 * @brief STAT1: unix-epoch ms timestamp of the last successful publish enqueue,
 * or -1 if none has happened yet this boot. "Successful" here means esp-mqtt
 * accepted the message into its outbox (QoS0 publishes have no broker ACK to
 * wait on), which is the strongest signal available without adding one.
 */
int64_t heapwatch_dispatcher_get_last_publish_unix_ms();
