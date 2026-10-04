// Copyright 2026 Heapwatch
// SPDX-License-Identifier: Apache-2.0

#ifndef HEAPWATCH_CONFIG_H
#define HEAPWATCH_CONFIG_H

#include <stdint.h>

#include "sdkconfig.h"

/* ============================================================================
   Heapwatch Connector Configuration
   =========================================================================== */

/* ---------------------------------------------------------------------------
   Message Queue Configuration
   --------------------------------------------------------------------------- */

#ifdef CONFIG_HEAPWATCH_TINY_QUEUE_LEN
#define HEAPWATCH_TINY_QUEUE_LEN         CONFIG_HEAPWATCH_TINY_QUEUE_LEN
#elif defined(CONFIG_HEAPWATCH_QUEUE_LEN)
/* Compat: older sdkconfigs had one shared depth for tiny+small. */
#define HEAPWATCH_TINY_QUEUE_LEN         CONFIG_HEAPWATCH_QUEUE_LEN
#else
#define HEAPWATCH_TINY_QUEUE_LEN         12
#endif

#ifdef CONFIG_HEAPWATCH_SMALL_QUEUE_LEN
#define HEAPWATCH_SMALL_QUEUE_LEN        CONFIG_HEAPWATCH_SMALL_QUEUE_LEN
#elif defined(CONFIG_HEAPWATCH_QUEUE_LEN)
#define HEAPWATCH_SMALL_QUEUE_LEN        CONFIG_HEAPWATCH_QUEUE_LEN
#else
#define HEAPWATCH_SMALL_QUEUE_LEN        12
#endif

#ifdef CONFIG_HEAPWATCH_LARGE_QUEUE_LEN
#define HEAPWATCH_LARGE_QUEUE_LEN        CONFIG_HEAPWATCH_LARGE_QUEUE_LEN
#else
#define HEAPWATCH_LARGE_QUEUE_LEN        3
#endif

/**
 * When enabled (Kconfig + SPIRAM): MQTT queues, command inbound queue, stream
 * TCP buffers, and metrics/publish scratch in PSRAM. Alloc failure aborts.
 * Default off.
 */
#if defined(CONFIG_HEAPWATCH_ALLOCATE_DATA_IN_PSRAM)
#define HEAPWATCH_ALLOCATE_DATA_IN_PSRAM 1
#else
#define HEAPWATCH_ALLOCATE_DATA_IN_PSRAM 0
#endif

/**
 * When enabled (Kconfig + SPIRAM), flash-safe hw_* stacks (log forward,
 * commanded-offline, enroll LED, BLE reporter) are allocated in PSRAM. Tasks that call
 * NVS / OTA / coredump / on-flash VFS (including hw_metrics FS volume stats) stay in
 * internal DRAM. Default off.
 */
#ifdef CONFIG_HEAPWATCH_STACKS_IN_PSRAM
#define HEAPWATCH_STACKS_IN_PSRAM 1
#else
#define HEAPWATCH_STACKS_IN_PSRAM 0
#endif

/** Max log objects per MQTT /log publish (backend accepts a JSON array). */
#ifdef CONFIG_HEAPWATCH_LOG_BATCH_MAX
#define HEAPWATCH_LOG_BATCH_MAX          CONFIG_HEAPWATCH_LOG_BATCH_MAX
#else
#define HEAPWATCH_LOG_BATCH_MAX          10
#endif

/**
 * Capture-ring slot text length and MQTT/stream truncate. The vprintf hook
 * cuts lines here before they enter the 16-slot DRAM ring.
 */
#ifdef CONFIG_HEAPWATCH_MAX_LOG_LINE_LEN
#define HEAPWATCH_MAX_LOG_LINE_LEN       CONFIG_HEAPWATCH_MAX_LOG_LINE_LEN
#else
#define HEAPWATCH_MAX_LOG_LINE_LEN       160
#endif

/** FreeRTOS task stacks (bytes). */
#ifdef CONFIG_HEAPWATCH_METRICS_TASK_STACK_BYTES
#define HEAPWATCH_METRICS_TASK_STACK_BYTES      CONFIG_HEAPWATCH_METRICS_TASK_STACK_BYTES
#else
#define HEAPWATCH_METRICS_TASK_STACK_BYTES      6144
#endif
#ifdef CONFIG_HEAPWATCH_DISPATCHER_TASK_STACK_BYTES
#define HEAPWATCH_DISPATCHER_TASK_STACK_BYTES   CONFIG_HEAPWATCH_DISPATCHER_TASK_STACK_BYTES
#else
#define HEAPWATCH_DISPATCHER_TASK_STACK_BYTES   12288
#endif
#ifdef CONFIG_HEAPWATCH_OFFLINE_TASK_STACK_BYTES
#define HEAPWATCH_OFFLINE_TASK_STACK_BYTES     CONFIG_HEAPWATCH_OFFLINE_TASK_STACK_BYTES
#else
#define HEAPWATCH_OFFLINE_TASK_STACK_BYTES     4096
#endif
#ifdef CONFIG_HEAPWATCH_LOG_FORWARD_TASK_STACK_BYTES
#define HEAPWATCH_LOG_FORWARD_TASK_STACK_BYTES  CONFIG_HEAPWATCH_LOG_FORWARD_TASK_STACK_BYTES
#else
#define HEAPWATCH_LOG_FORWARD_TASK_STACK_BYTES  4096
#endif
#ifdef CONFIG_HEAPWATCH_STREAMING_TASK_STACK_BYTES
#define HEAPWATCH_STREAMING_TASK_STACK_BYTES    CONFIG_HEAPWATCH_STREAMING_TASK_STACK_BYTES
#else
#define HEAPWATCH_STREAMING_TASK_STACK_BYTES    8192
#endif

/* ---------------------------------------------------------------------------
   Timing Configuration
   --------------------------------------------------------------------------- */

/**
 * Interval between automatic telemetry metric collection cycles.
 * The metrics collector thread sleeps for this duration between each collection.
 */
#ifdef CONFIG_HEAPWATCH_METRICS_INTERVAL_MS
#define HEAPWATCH_METRICS_INTERVAL_MS    CONFIG_HEAPWATCH_METRICS_INTERVAL_MS
#else
#define HEAPWATCH_METRICS_INTERVAL_MS    2000
#endif

/**
 * Delay between MQTT reconnection attempts when connection fails.
 */
#define HEAPWATCH_MQTT_RETRY_INTERVAL_MS 1000

/**
 * Slice length for tasks that sleep longer than this and must still notice
 * heapwatch_deinit()'s stop flag. Teardown tolerates a task reacting this late,
 * so keep it coarse — a shorter slice only buys deinit latency nobody waits on,
 * at the cost of extra wakeups for the entire uptime of the device.
 */
#define HEAPWATCH_STOP_CHECK_SLICE_MS 2000

/**
 * MQTT CONNECT keepalive (seconds). esp-mqtt pings at ~half this interval.
 * IDF default is 120 when unset; we set explicitly so broker session timing is predictable.
 */
#define HEAPWATCH_MQTT_KEEPALIVE_SEC 60

/**
 * Delay between core dump upload retry attempts on the heapwatch_dispatcher thread,
 * once MQTT is connected. A failed attempt no longer blocks the dispatcher's own
 * MQTT-publish work in between retries.
 */
#define HEAPWATCH_COREDUMP_RETRY_INTERVAL_MS 20000

/**
 * Delay between claimed-identities POST retry attempts on heapwatch_dispatcher once
 * heapwatch_network_ready() is true (host-owned or managed networking).
 */
#define HEAPWATCH_IDENTITIES_RETRY_INTERVAL_MS 20000

/**
 * Default delay between device enrollment poll requests (fallback only —
 * CONFIG_HEAPWATCH_ENROLLMENT_POLL_INTERVAL_MS is used directly where available).
 */
#define HEAPWATCH_ENROLLMENT_POLL_INTERVAL_MS 60000

/* ---------------------------------------------------------------------------
   Message Payload Format Selection
   --------------------------------------------------------------------------- */

/**
 * Choose the message payload format for MQTT messages.
 * 
 * Options:
 * - HEAPWATCH_USE_JSON_FORMAT (recommended): JSON with named fields
 *   Example: {"tsms":1730000000123,"utms":8333,"task":"hw_dispatcher","tag":"mqtt_client","core":0,"severity":"info","message":"Hello"}
 * 
 * - HEAPWATCH_USE_FLAT_KEYVALUE_FORMAT: Flat key=value format
 *   Example: tsms=1730000000123|utms=8333|task=hw_dispatcher|tag=mqtt_client|core=0|severity=info|message=Hello
 *
 * `tsms` is always present but is `null` until the device has a real wall clock; `utms` (uptime)
 * and `boot_cycle` are always real, and the backend uses them to place clockless data in time.
 * 
 * Uncomment ONE of the following lines to select the desired format.
 */

#define HEAPWATCH_USE_JSON_FORMAT
/* #define HEAPWATCH_USE_FLAT_KEYVALUE_FORMAT */


/* ============================================================================
   Severity Levels for Log Messages (JSON format only)
   =========================================================================== */

/* typedef enum so this header is includable from pure-C translation units
 * (public heapwatch.h). Same values/names as before — no wire/ABI change. */
typedef enum {
    HEAPWATCH_SEVERITY_DEBUG     = 0,
    HEAPWATCH_SEVERITY_INFO      = 1,
    HEAPWATCH_SEVERITY_WARNING   = 2,
    HEAPWATCH_SEVERITY_ERROR     = 3,
    HEAPWATCH_SEVERITY_CRITICAL  = 4
} HeapwatchConnectorSeverity;

/**
 * Minimum severity relayed over MQTT. Logs below this level are still printed
 * locally but are not queued for remote delivery. Backed by the
 * HEAPWATCH_MIN_LOG_SEVERITY Kconfig choice; falls back to INFO for the
 * host-unit-test build (no sdkconfig).
 */
#if defined(CONFIG_HEAPWATCH_MIN_LOG_SEVERITY_DEBUG)
#define HEAPWATCH_MIN_RELAY_SEVERITY  HEAPWATCH_SEVERITY_DEBUG
#elif defined(CONFIG_HEAPWATCH_MIN_LOG_SEVERITY_WARNING)
#define HEAPWATCH_MIN_RELAY_SEVERITY  HEAPWATCH_SEVERITY_WARNING
#elif defined(CONFIG_HEAPWATCH_MIN_LOG_SEVERITY_ERROR)
#define HEAPWATCH_MIN_RELAY_SEVERITY  HEAPWATCH_SEVERITY_ERROR
#elif defined(CONFIG_HEAPWATCH_MIN_LOG_SEVERITY_CRITICAL)
#define HEAPWATCH_MIN_RELAY_SEVERITY  HEAPWATCH_SEVERITY_CRITICAL
#else
#define HEAPWATCH_MIN_RELAY_SEVERITY  HEAPWATCH_SEVERITY_INFO
#endif

/* ============================================================================
   Wake Cause Codes (framework-agnostic)
   =========================================================================== */

typedef enum {
    HEAPWATCH_WAKE_CAUSE_UNDEFINED = 0,
    HEAPWATCH_WAKE_CAUSE_TIMER     = 1,
    HEAPWATCH_WAKE_CAUSE_EXT0      = 2,
    HEAPWATCH_WAKE_CAUSE_EXT1      = 3,
    HEAPWATCH_WAKE_CAUSE_TOUCHPAD  = 4,
    HEAPWATCH_WAKE_CAUSE_ULP       = 5,
    HEAPWATCH_WAKE_CAUSE_GPIO      = 6,
    HEAPWATCH_WAKE_CAUSE_UART      = 7,
    HEAPWATCH_WAKE_CAUSE_WIFI      = 8,
    HEAPWATCH_WAKE_CAUSE_BT        = 9,
    HEAPWATCH_WAKE_CAUSE_COCPU     = 10,
    HEAPWATCH_WAKE_CAUSE_OTHER     = 99,
} heapwatch_wake_cause_t;

/* ============================================================================
   Event Type IDs (for QEvent)
   =========================================================================== */

/**
 * Standard event type IDs.
 */
#define HEAPWATCH_EVENT_STARTED_UP                  0
#define HEAPWATCH_EVENT_RESTARTING                  1
#define HEAPWATCH_EVENT_SHUTTING_DOWN               2

#define HEAPWATCH_EVENT_MQTT_CONNECTED              7
#define HEAPWATCH_EVENT_MQTT_DISCONNECTED           8
#define HEAPWATCH_EVENT_INTERNET_CONNECTED          9
#define HEAPWATCH_EVENT_INTERNET_DISCONNECTED      10
#define HEAPWATCH_EVENT_GNSS_GOT_FIX               11
#define HEAPWATCH_EVENT_GNSS_LOST_FIX              12
#define HEAPWATCH_EVENT_STARTING_UP                15

#define HEAPWATCH_EVENT_ENTERING_DEEP_SLEEP        17
#define HEAPWATCH_EVENT_WOKE_UP_FROM_DEEP_SLEEP   18
#define HEAPWATCH_EVENT_ENTERING_LIGHT_SLEEP       19
#define HEAPWATCH_EVENT_WOKE_UP_FROM_LIGHT_SLEEP   20

#define HEAPWATCH_EVENT_NETIF_UP                   40
#define HEAPWATCH_EVENT_NETIF_DOWN                 41
#define HEAPWATCH_EVENT_NETIF_GOT_IP               42
#define HEAPWATCH_EVENT_NETIF_LOST_IP              43
#define HEAPWATCH_EVENT_DEFAULT_ROUTE_CHANGED      44

#define HEAPWATCH_EVENT_BLE_STACK_READY           60
#define HEAPWATCH_EVENT_BLE_ADV_STARTED           61
#define HEAPWATCH_EVENT_BLE_ADV_STOPPED           62
#define HEAPWATCH_EVENT_BLE_SCAN_STARTED          63
#define HEAPWATCH_EVENT_BLE_SCAN_STOPPED          64
#define HEAPWATCH_EVENT_BLE_CONNECTED             65
#define HEAPWATCH_EVENT_BLE_DISCONNECTED          66
#define HEAPWATCH_EVENT_BLE_BOND_ADDED            67
#define HEAPWATCH_EVENT_BLE_BOND_REMOVED          68
#define HEAPWATCH_EVENT_BLE_AUTH_FAILED           69

#define HEAPWATCH_EVENT_FW_UPDATE_STARTED         100
#define HEAPWATCH_EVENT_FW_UPDATE_DOWNLOADING     101
#define HEAPWATCH_EVENT_FW_UPDATE_DOWNLOADED      102
#define HEAPWATCH_EVENT_FW_UPDATE_FLASHING        103
#define HEAPWATCH_EVENT_FW_UPDATE_FLASHED         104
#define HEAPWATCH_EVENT_FW_UPDATE_SUCCESSFUL      105
#define HEAPWATCH_EVENT_FW_UPDATE_FAILED          106
#define HEAPWATCH_EVENT_FW_UPDATE_CONFIRMED       107

#define HEAPWATCH_EVENT_AUTO_SLEEP_RESET          120
#define HEAPWATCH_EVENT_AUTO_SLEEP_STOPPED        121
#define HEAPWATCH_EVENT_AUTO_SLEEP_PUTTING_TO_SLEEP 122

/* ============================================================================
   Maximum Payload Sizes
   =========================================================================== */

/**
 * Maximum message payload size in bytes.
 * This limits the length of log messages and other text payloads.
 */
#define HEAPWATCH_MAX_PAYLOAD_SIZE     2048
#define HEAPWATCH_FLAT_DELIMITER       "|"

/**
 * M1 (Phase 2b): three static queue tiers instead of one flat
 * HEAPWATCH_MAX_PAYLOAD_SIZE slot for every message, regardless of its real
 * size. Sized off the real per-message-type payload distribution surveyed
 * during planning (see findings.md "Memory architecture options"):
 *  - tiny:  single-value PARAMETER (queueParameter()) / EVENT messages
 *    (~60-90B in practice)
 *  - small: LOG messages — driven by HEAPWATCH_MAX_LOG_LINE_LEN since a log
 *    line can be JSON-escaped to up to 2x its raw length (worst case every
 *    character needs a "\\" or "\\uXXXX" escape) plus ~320B of JSON framing
 *    headroom (timestamps, escaped task/tag fields up to 64B each, core,
 *    severity — see the g_mqtt_payload_build_buffers optional sizing in MqttTelemetryClient.cpp)
 *  - large: SPECS/TASKS messages and MetricsCollector's still-unrefactored
 *    hand-rolled periodic PARAMETER blob (heap/Wi-Fi/FreeRTOS-count fields —
 *    Phase 5c/PARAM1 will eventually split this into real per-slug tiny
 *    messages) — unchanged from the old flat size
 */
/* Envelope is tsms + utms + boot_cycle, and a queued null tsms grows by ~9 bytes when the
 * dispatcher restamps it after time sync — so a single-value message needs more than 96 B. */
#define HEAPWATCH_TINY_PAYLOAD_SIZE      128
#define HEAPWATCH_SMALL_PAYLOAD_SIZE     (HEAPWATCH_MAX_LOG_LINE_LEN * 2 + 320)
#define HEAPWATCH_LARGE_PAYLOAD_SIZE     HEAPWATCH_MAX_PAYLOAD_SIZE

/** Rows in the metrics task-stats table (name → stack min/max/peak). */
#ifndef HEAPWATCH_MAX_TASK_ROWS
#define HEAPWATCH_MAX_TASK_ROWS 48
#endif

/** uxTaskGetSystemState() scratch; must be ≥ HEAPWATCH_MAX_TASK_ROWS. */
#ifndef HEAPWATCH_MAX_TASK_STATUS_SCAN
#define HEAPWATCH_MAX_TASK_STATUS_SCAN 64
#endif
#if HEAPWATCH_MAX_TASK_STATUS_SCAN < HEAPWATCH_MAX_TASK_ROWS
#error "HEAPWATCH_MAX_TASK_STATUS_SCAN must be >= HEAPWATCH_MAX_TASK_ROWS"
#endif

/**
 * The backend sends one command and waits for its ack/response before sending
 * the next, so steady-state depth is 1. The 2nd slot covers a command and a
 * credential-rotation notification landing back-to-back (both funnel through
 * this same queue), or a brief redelivery burst right after MQTT reconnect.
 */
#ifdef CONFIG_HEAPWATCH_COMMAND_INBOUND_QUEUE_DEPTH
#define HEAPWATCH_COMMAND_INBOUND_QUEUE_DEPTH CONFIG_HEAPWATCH_COMMAND_INBOUND_QUEUE_DEPTH
#else
#define HEAPWATCH_COMMAND_INBOUND_QUEUE_DEPTH 2
#endif
#define HEAPWATCH_COMMAND_TOPIC_MAX           160
#define HEAPWATCH_COMMAND_PAYLOAD_MAX         1024

/**
 * Every producer except BLE scan/events (logs, file browser, FS download/upload,
 * catalog/config/snapshot uploads) enqueues one frame and immediately calls
 * flushOutbound() before producing the next one — same task, synchronous TLS write —
 * so only 1 slot is ever actually "busy" from those paths. BLE reporting is currently
 * hard-blocked (see CMakeLists.txt), so there is no cross-task producer left that
 * needs headroom beyond 1. Each slot is ~4.2 KB (MAX_PAYLOAD_LEN-sized), so this is one
 * of the largest line items in the connector memory block. Fixed constant, not a
 * Kconfig knob — raising it only makes sense alongside reintroducing a concurrent
 * producer (e.g. re-enabling BLE reporting), which is a code change anyway.
 */
#define HEAPWATCH_STREAM_OUTBOUND_QUEUE_DEPTH 1

/* ============================================================================
   Internal connector use only — not part of the public MVP API
   (in-RAM parameter map capacity for connector telemetry)
   =========================================================================== */

#define HEAPWATCH_MAX_PARAMETER_SLOTS 32

/* ============================================================================
   Internal connector use only — not part of the public MVP API
   (Heapwatch-owned power state for connector parameter helpers)
   =========================================================================== */

typedef enum {
    HEAPWATCH_POWER_STATE_UNKNOWN     = 0,
    HEAPWATCH_POWER_STATE_DISCHARGING = 1,
    HEAPWATCH_POWER_STATE_CHARGING    = 2,
    HEAPWATCH_POWER_STATE_FULL        = 3,
} heapwatch_power_state_t;

#endif /* HEAPWATCH_CONFIG_H */
