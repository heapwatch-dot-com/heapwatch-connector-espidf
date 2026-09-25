// Copyright 2026 Heapwatch
// SPDX-License-Identifier: Apache-2.0

#ifndef MQTT_TELEMETRY_CLIENT_H
#define MQTT_TELEMETRY_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "heapwatch_config.h"

struct HeapwatchConnectorLogContext {
    const char* task;
    const char* tag;
    int32_t core;
};

enum MqttMessageType : uint8_t {
    MQTT_MESSAGE_TYPE_LOG = 0,
    MQTT_MESSAGE_TYPE_PARAMETER = 1,
    MQTT_MESSAGE_TYPE_EVENT = 2,
    MQTT_MESSAGE_TYPE_SPECS = 3,
    MQTT_MESSAGE_TYPE_TASKS = 4,
    MQTT_MESSAGE_TYPE_OTA_PROGRESS = 5,
    MQTT_MESSAGE_TYPE_NETWORK = 6,
    MQTT_MESSAGE_TYPE_BLUETOOTH = 7,
    MQTT_MESSAGE_TYPE_CONFIG = 8,
};

/**
 * M1 (Phase 2b): three fixed-size, by-value message structs — one per queue
 * tier — instead of a single flat struct sized for the worst case (SPECS/
 * TASKS) for every message. See heapwatch_config.h for the size rationale.
 * Still zero malloc/free, same "reserve once at boot" property as before.
 */
struct TinyMqttMessage {
    MqttMessageType type;
    char payload[HEAPWATCH_TINY_PAYLOAD_SIZE];
};

struct SmallMqttMessage {
    MqttMessageType type;
    char payload[HEAPWATCH_SMALL_PAYLOAD_SIZE];
};

struct LargeMqttMessage {
    MqttMessageType type;
    char payload[HEAPWATCH_LARGE_PAYLOAD_SIZE];
};

/** Kept name for the large tier: used by SPECS/TASKS and the dispatcher's canonical publish path. */
using MqttMessage = LargeMqttMessage;

struct CoreDumpPayload {
    size_t payload_len;
    char payload[HEAPWATCH_COREDUMP_MAX_PAYLOAD_SIZE];
};

/**
 * STAT1: synchronous, always-compiled-in status snapshot for app-code
 * introspection/gating (e.g. "don't enter light sleep until the tiny queue is
 * drained"). Distinct use case from the periodic connector-health *telemetry*
 * publish (HEAPWATCH_ENABLE_CONNECTOR_HEALTH, not yet implemented) — this is a
 * cheap local read, not a backend-visible message.
 */
struct HeapwatchConnectorStatus {
    bool initialized;
    bool network_ready;
    bool mqtt_connected;
    bool dispatcher_running;
    bool collector_running;
    /** Unix epoch ms of the last successful publish enqueue, or -1 if none yet this boot. */
    int64_t last_successful_publish_unix_ms;
    uint32_t tiny_queue_depth;
    uint32_t small_queue_depth;
    uint32_t large_queue_depth;
    uint32_t coredump_queue_depth;
};

class MqttTelemetryClient {
public:
    static MqttTelemetryClient& getInstance();

    /** Creates MQTT queues/mutex — safe to call before credentials exist. */
    bool createMqttQueuesAndMutexes();
    bool isInfrastructureReady() const;

    /** Starts dispatcher/collector after credentials are applied. */
    bool activateWithBroker(const char* broker_url);

    bool initialize(const char* device_uid, const char* broker_url);
    void deinitialize();
    void setNetworkReady(bool ready);

    void setMqttDeviceUid(const char* uid);
    void setTenantSlug(const char* slug);
    void setMqttPassword(const char* password);
    void setCoreDumpUploadUrl(const char* url);
    void setRestApiAuthToken(const char* token);
    void setCoreDumpBuildIdentifier(const char* build_identifier);
    /** Internal — driven by connector_lifecycle, which owns the NVS boot counter. */
    void setBootCycleContext(uint32_t boot_cycle, uint32_t crash_boot_cycle);

    bool queueLog(HeapwatchConnectorSeverity severity, const char* message,
                  const HeapwatchConnectorLogContext* context = nullptr);
    /**
     * Format one MQTT-shaped log JSON object (same wire shape as MQTT /log payloads).
     * Returns false if output buffer is too small or inputs are invalid.
     */
    bool formatLogJsonLine(HeapwatchConnectorSeverity severity, const char* message,
                           const HeapwatchConnectorLogContext* context, char* out,
                           size_t out_len);
    bool queueCoreDump(const char* base64_payload, size_t payload_len);
    bool uploadCoreDumpFromFlashIfPresent();
    void queueParameter(const char* name, int64_t value);
    void queueParameter(const char* name, double value);
    void queueEvent(int32_t eventTypeId, int32_t boot_cycle = -1);
    /** Device → cloud OTA byte progress on `{tenant}/device/{uid}/ota/progress`. */
    void queueOtaProgress(int64_t ota_session_id, const char* phase, int64_t bytes, int64_t total);
    void queueSpecs();
    /** Device → cloud full netif snapshot on `{tenant}/device/{uid}/network`. */
    void queueNetwork();
    /** Device → cloud BLE snapshot on `{tenant}/device/{uid}/bluetooth`. */
    void queueBluetoothSnapshot();

    /**
     * PARAM1 (Phase 5c): generic escape-hatch parameter API -- unlike
     * queueParameter() above (which enqueues an immediate one-shot publish,
     * used for event-adjacent values like wake_cause_code/sleep_duration_ms),
     * these update an in-RAM sparse map (see ParameterStore.h) that gets
     * batch-flushed as one publish per telemetry round via flushParameters().
     * This is the mechanism POW1 (power) and CELL1 (cellular signal) build on.
     */
    void updateParameterInt(const char* slug, int64_t value);
    void updateParameterDouble(const char* slug, double value);
    void updateParameterIntRanged(const char* slug, int64_t value, int64_t min_value,
                                   int64_t max_value);
    void updateParameterDoubleRanged(const char* slug, double value, double min_value,
                                      double max_value);

    /**
     * Limits-only ParameterStore updates (PARAM1b). Does not require a live
     * value; the next flushParameters() may publish only `slug_max` / `slug_min`.
     */
    void setParameterIntMax(const char* slug, int64_t max_value);
    void setParameterIntMin(const char* slug, int64_t min_value);
    void setParameterDoubleMax(const char* slug, double max_value);
    void setParameterDoubleMin(const char* slug, double min_value);

    /**
     * Flushes the entire current parameter map (see ParameterStore.h) as one
     * batched publish. Called once per telemetry round by MetricsCollector;
     * a no-op if the map is currently empty (no parameters ever updated).
     */
    void flushParameters();

    /** Flush dirty `_min`/`_max` bounds only (boot sync path). */
    void flushParameterBounds();

    /** STAT1: see HeapwatchConnectorStatus. Always compiled in, cheap to call frequently. */
    HeapwatchConnectorStatus getStatus() const;

    /** Starts hw_metrics once mqtt_task exists (frees internal DRAM at MQTT client start). */
    void ensureMetricsCollectorThreadStarted();

private:
    MqttTelemetryClient() = default;
    ~MqttTelemetryClient() = default;
    MqttTelemetryClient(const MqttTelemetryClient&) = delete;
    MqttTelemetryClient& operator=(const MqttTelemetryClient&) = delete;

    void startDispatcherThread();
    void startCollectorThread();
};

QueueHandle_t heapwatch_get_tiny_queue();
QueueHandle_t heapwatch_get_small_queue();
QueueHandle_t heapwatch_get_large_mqtt_queue();  // large MQTT tier (SPECS/TASKS/metrics blob)
bool heapwatch_enqueue_mqtt_queue_message(QueueHandle_t queue, const void* item, TickType_t wait);
void heapwatch_mark_mqtt_queue_message_published();
void heapwatch_wait_for_queued_mqtt_message();
QueueHandle_t heapwatch_get_coredump_queue();
const char* heapwatch_get_device_uid();
const char* heapwatch_get_tenant_slug();
const char* heapwatch_get_broker_uri();
const char* heapwatch_get_broker_password();
const char* heapwatch_get_rest_api_auth_token();
uint32_t heapwatch_get_boot_cycle();
bool heapwatch_dispatcher_running();
bool heapwatch_collector_running();
bool heapwatch_network_ready();

void heapwatch_mark_dispatcher_task_exited();
void heapwatch_mark_collector_task_exited();

#endif
