// Copyright (c) 2026 Viktor Jamrich Enterprises s.r.o. All rights reserved.
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
// For licensing inquiries, please contact: admin@heapwatch.com
//

#ifndef HEAPWATCH_CONFIG_H
#define HEAPWATCH_CONFIG_H

#include <stdint.h>

#include "sdkconfig.h"

/* ============================================================================
   Heapwatch Connector Configuration
   =========================================================================== */

/* ---------------------------------------------------------------------------
   Message Queue Configuration (K7 — Kconfig-tunable, see Kconfig.projbuild
   "Memory / queue sizing"; #define fallbacks here only cover the
   host-unit-test build, which doesn't go through sdkconfig)
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

/**
 * Depth of the large-tier queue (SPECS/TASKS + MetricsCollector's periodic
 * PARAMETER blob, see M1 in task_plan.md). Deliberately shallow relative to
 * the tiny/small queues — these messages are periodic/rare compared to
 * logs/params, so there's little value buffering many of them.
 */
#ifdef CONFIG_HEAPWATCH_LARGE_QUEUE_LEN
#define HEAPWATCH_LARGE_QUEUE_LEN        CONFIG_HEAPWATCH_LARGE_QUEUE_LEN
#else
#define HEAPWATCH_LARGE_QUEUE_LEN        3
#endif

/**
 * There is only ever one core dump pending upload at a time (a device can only
 * crash once between boots), so this is a fixed constant, not a Kconfig knob.
 */
#define HEAPWATCH_COREDUMP_QUEUE_LEN     1

/** Max log objects per MQTT /log publish (backend accepts a JSON array). */
#ifdef CONFIG_HEAPWATCH_LOG_BATCH_MAX
#define HEAPWATCH_LOG_BATCH_MAX          CONFIG_HEAPWATCH_LOG_BATCH_MAX
#else
#define HEAPWATCH_LOG_BATCH_MAX          10
#endif

/**
 * Log lines longer than this are truncated before being queued (K7). Doesn't
 * yet back a dedicated smaller queue tier — that's the M1 tiered-queue work
 * (Phase 2b) — but bounds worst-case log payload size today independent of
 * HEAPWATCH_MAX_PAYLOAD_SIZE.
 */
#ifdef CONFIG_HEAPWATCH_MAX_LOG_LINE_LEN
#define HEAPWATCH_MAX_LOG_LINE_LEN       CONFIG_HEAPWATCH_MAX_LOG_LINE_LEN
#else
#define HEAPWATCH_MAX_LOG_LINE_LEN       160
#endif

/** FreeRTOS task stacks (bytes) — keep tight; largest MQTT payloads are ~2KB. */
#define HEAPWATCH_METRICS_TASK_STACK_BYTES      8192
#define HEAPWATCH_DISPATCHER_TASK_STACK_BYTES   8192

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
 * MQTT CONNECT keepalive (seconds). esp-mqtt pings at ~half this interval.
 * IDF default is 120 when unset; we set explicitly so broker session timing is predictable.
 */
#define HEAPWATCH_MQTT_KEEPALIVE_SEC 60

/**
 * C1: delay between core dump upload retry attempts on the heapwatch_dispatcher thread,
 * once MQTT is connected. A failed attempt no longer blocks the dispatcher's own
 * MQTT-publish work in between retries.
 */
#define HEAPWATCH_COREDUMP_RETRY_INTERVAL_MS 20000

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
   Wake Cause Codes (framework-agnostic; see PORT1 policy)
   =========================================================================== */

/**
 * Stable, Heapwatch-owned wake cause codes reported via the "wake_cause_code" parameter
 * on HEAPWATCH_EVENT_WOKE_UP_FROM_* events. Never send raw esp_sleep_wakeup_cause_t values
 * to the backend directly — translate through heapwatch_translate_current_wake_cause()
 * (see main/sleep_tracking/SleepTracking.h) so the wire format stays stable across
 * ESP-IDF versions and, eventually, other frameworks.
 */
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

/** HEAP1: proactive corruption detection via heap_caps_check_integrity(). */
#define HEAPWATCH_EVENT_HEAP_CORRUPTION_DETECTED  130

/* ============================================================================
   Maximum Payload Sizes
   =========================================================================== */

/**
 * Maximum message payload size in bytes.
 * This limits the length of log messages and other text payloads.
 */
#define HEAPWATCH_MAX_PAYLOAD_SIZE     2048
#define HEAPWATCH_COREDUMP_MAX_PAYLOAD_SIZE 32768
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
 *    severity — see the g_scratch_optional sizing in HeapwatchConnector.cpp)
 *  - large: SPECS/TASKS messages and MetricsCollector's still-unrefactored
 *    hand-rolled periodic PARAMETER blob (heap/Wi-Fi/FreeRTOS-count fields —
 *    Phase 5c/PARAM1 will eventually split this into real per-slug tiny
 *    messages) — unchanged from the old flat size
 */
#define HEAPWATCH_TINY_PAYLOAD_SIZE      96
#define HEAPWATCH_SMALL_PAYLOAD_SIZE     (HEAPWATCH_MAX_LOG_LINE_LEN * 2 + 320)
#define HEAPWATCH_LARGE_PAYLOAD_SIZE     HEAPWATCH_MAX_PAYLOAD_SIZE

/* ============================================================================
   Generic Parameter Mechanism (PARAM1, Phase 5c)
   =========================================================================== */

/**
 * Fixed capacity of the in-RAM sparse parameter map (slug -> value/min/max)
 * that HeapwatchConnector::flushParameters() batches into one publish per
 * telemetry round. No malloc/free — a slug simply fails to register once this
 * many distinct slugs are live in a boot (auto-provisioned per-task/per-power-source
 * slugs count against this same budget).
 */
#define HEAPWATCH_MAX_PARAMETER_SLOTS 32

/**
 * PARAM1b: a slug's _min/_max bounds are only re-included in a flush when they
 * actually changed since last sent (dirty-flag gated) -- EXCEPT for this rare
 * safety-net resend, so a long-uptime device with a narrow dashboard time
 * window still has a support band to show (see task_plan.md PARAM1b).
 */
#define HEAPWATCH_PARAMETER_BOUNDS_RESEND_INTERVAL_MS (60UL * 60UL * 1000UL)

/* ============================================================================
   Power State (POW1; Heapwatch-owned, no framework equivalent to leak per PORT1)
   =========================================================================== */

typedef enum {
    HEAPWATCH_POWER_STATE_UNKNOWN     = 0,
    HEAPWATCH_POWER_STATE_DISCHARGING = 1,
    HEAPWATCH_POWER_STATE_CHARGING    = 2,
    HEAPWATCH_POWER_STATE_FULL        = 3,
} heapwatch_power_state_t;

#endif /* HEAPWATCH_CONFIG_H */
