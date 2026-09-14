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

/**
 * Heapwatch Connector for ESP-IDF — public C API (Phase 5b).
 *
 * Typical app_main:
 *
 *   ESP_ERROR_CHECK(heapwatch_initialize());
 *   // ... your application (mount SD, register volumes, sensors) — never blocks on enrollment ...
 *   heapwatch_notify_app_started();
 *
 * Boot identity (boot counter, crash attribution, STARTING_UP and deep-sleep wake telemetry)
 * is owned by the connector — the host app never wires it up.
 */

#ifndef HEAPWATCH_H
#define HEAPWATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "heapwatch_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
   Lifecycle phase (sidecar bootstrap — query via heapwatch_get_status)
   --------------------------------------------------------------------------- */

typedef enum {
    HEAPWATCH_PHASE_MISCONFIGURED = 0,
    HEAPWATCH_PHASE_STARTING,
    HEAPWATCH_PHASE_AWAITING_NETWORK,
    HEAPWATCH_PHASE_NETWORK_FAILED,
    HEAPWATCH_PHASE_PROVISIONING,
    HEAPWATCH_PHASE_WAITING_ACCEPT,
    HEAPWATCH_PHASE_READY,
    HEAPWATCH_PHASE_MQTT_CONNECTING,
    HEAPWATCH_PHASE_MQTT_CONNECTED,
} heapwatch_phase_t;

/* ---------------------------------------------------------------------------
   Component-specific esp_err_t range (E1)
   Well clear of Espressif bases (WIFI 0x3000 … MEMPROT 0xd000).
   --------------------------------------------------------------------------- */

#define ESP_ERR_HEAPWATCH_BASE                 0x11000
#define ESP_ERR_HEAPWATCH_NOT_PROVISIONED      (ESP_ERR_HEAPWATCH_BASE + 0x01)
#define ESP_ERR_HEAPWATCH_QUEUE_FULL           (ESP_ERR_HEAPWATCH_BASE + 0x02)
#define ESP_ERR_HEAPWATCH_NOT_CONNECTED        (ESP_ERR_HEAPWATCH_BASE + 0x03)
#define ESP_ERR_HEAPWATCH_NOT_INITIALIZED      (ESP_ERR_HEAPWATCH_BASE + 0x04)
#define ESP_ERR_HEAPWATCH_INVALID_ARG          (ESP_ERR_HEAPWATCH_BASE + 0x05)
#define ESP_ERR_HEAPWATCH_NO_SPACE             (ESP_ERR_HEAPWATCH_BASE + 0x06)
#define ESP_ERR_HEAPWATCH_ALREADY_INITIALIZED  (ESP_ERR_HEAPWATCH_BASE + 0x07)
#define ESP_ERR_HEAPWATCH_INIT_FAILED          (ESP_ERR_HEAPWATCH_BASE + 0x08)

/* ---------------------------------------------------------------------------
   Status
   --------------------------------------------------------------------------- */

typedef struct {
    heapwatch_phase_t phase;
    bool initialized;
    bool network_ready;
    bool mqtt_connected;
    bool dispatcher_running;
    bool collector_running;
    /**
     * True while the host requested keep-running OR Heapwatch OTA is in progress.
     * Host firmware should skip light/deep sleep while this is true.
     */
    bool keep_running;
    /** Unix epoch ms of last successful publish enqueue, or -1 if none this boot. */
    int64_t last_successful_publish_unix_ms;
    uint32_t tiny_queue_depth;
    uint32_t small_queue_depth;
    uint32_t large_queue_depth;
    uint32_t coredump_queue_depth;
} heapwatch_status_t;

typedef struct {
    const char* task;
    const char* tag;
    int core;
} heapwatch_log_context_t;

/** CMD1: custom inbound MQTT command handler (see heapwatch_register_command). */
typedef void (*heapwatch_command_handler_t)(const char* suffix, const char* command_id,
                                            void* user_ctx);

/**
 * Before-sleep hook: return true to allow sleep, false to reject the command.
 * May call heapwatch_must_stay_awake() to veto without returning false.
 */
typedef bool (*heapwatch_before_sleep_hook_t)(void* user_ctx);

/* ---------------------------------------------------------------------------
   Lifecycle
   --------------------------------------------------------------------------- */

/**
 * One-call sidecar bootstrap: NVS, log relay, Wi-Fi/SNTP (when Kconfig-managed),
 * background enrollment, and MQTT — returns immediately. Idempotent while running
 * (second call is a no-op). After heapwatch_deinit(), this may be called again.
 * Fails fast on fatal misconfiguration (empty enrollment key + no NVS creds, placeholder
 * build fingerprint, NVS init failure).
 */
esp_err_t heapwatch_initialize(void);

/** Full connector teardown. After this, heapwatch_initialize() may be called again. */
void heapwatch_deinit(void);

/**
 * Report that the host application finished its own start-up (queues STARTED_UP).
 * Call once at the end of your init. heapwatch_initialize() already queued STARTING_UP,
 * so this pair brackets your boot on the timeline.
 * Returns ESP_ERR_HEAPWATCH_NOT_INITIALIZED when called before heapwatch_initialize().
 */
esp_err_t heapwatch_notify_app_started(void);

esp_err_t heapwatch_get_status(heapwatch_status_t* out);

/**
 * Host-controlled critical-section flag: when true, heapwatch_keep_running()
 * stays true so the app can skip sleep. Cleared by heapwatch_set_keep_running(false).
 * Heapwatch OTA in progress also forces keep_running without using this flag.
 */
void heapwatch_set_keep_running(bool keep_running);

/**
 * True if the host set keep-running OR an internal critical op (OTA download/flash)
 * is active. Check this before light/deep sleep.
 */
bool heapwatch_keep_running(void);

/* ---------------------------------------------------------------------------
   Telemetry
   --------------------------------------------------------------------------- */

/**
 * Queue a standard event. Pass boot_cycle < 0 to use the connector’s current boot cycle.
 * The MQTT payload always includes boot_cycle.
 */
esp_err_t heapwatch_queue_event(int event_type_id, int32_t boot_cycle);

esp_err_t heapwatch_queue_log(HeapwatchConnectorSeverity severity, const char* message,
                              const heapwatch_log_context_t* context);

/** One-shot parameter publish (event-adjacent values). */
esp_err_t heapwatch_queue_parameter_int(const char* slug, int64_t value);
esp_err_t heapwatch_queue_parameter_double(const char* slug, double value);

/** Coalesced map updates (PARAM1); flushed each metrics cycle. */
esp_err_t heapwatch_update_parameter_int(const char* slug, int64_t value);
esp_err_t heapwatch_update_parameter_double(const char* slug, double value);
esp_err_t heapwatch_update_parameter_int_ranged(const char* slug, int64_t value, int64_t min_value,
                                                int64_t max_value);
esp_err_t heapwatch_update_parameter_double_ranged(const char* slug, double value,
                                                   double min_value, double max_value);

/**
 * Limits-only map updates. Does not require a live value; must not be used to
 * push per-tick max as timeseries (bounds are dirty-flag / safety-net only).
 */
esp_err_t heapwatch_set_parameter_max(const char* slug, int64_t max_value);
esp_err_t heapwatch_set_parameter_min(const char* slug, int64_t min_value);
esp_err_t heapwatch_set_parameter_double_max(const char* slug, double max_value);
esp_err_t heapwatch_set_parameter_double_min(const char* slug, double min_value);

/** Typed power helpers (`dynamic.power.{source}.{mv|percent|state}`). */
esp_err_t heapwatch_update_power_mv(const char* source_name, int64_t mv);
esp_err_t heapwatch_update_power_mv_ranged(const char* source_name, int64_t mv, int64_t min_mv,
                                           int64_t max_mv);
esp_err_t heapwatch_update_power_percent(const char* source_name, double percent);
esp_err_t heapwatch_update_power_state(const char* source_name, heapwatch_power_state_t state);

/* ---------------------------------------------------------------------------
   Remote commands (CMD1)
   --------------------------------------------------------------------------- */

/**
 * Register a client command (topic suffix). Safe before or after heapwatch_initialize().
 * Built-ins ping/specs_refresh/restart cannot be overridden. Table is fixed-size (8);
 * returns ESP_ERR_HEAPWATCH_NO_SPACE when full.
 */
esp_err_t heapwatch_register_command(const char* name, heapwatch_command_handler_t handler,
                                     void* user_ctx);

esp_err_t heapwatch_publish_command_response(const char* suffix, const char* command_id, bool ok,
                                             const char* code, const char* message);

esp_err_t heapwatch_register_before_light_sleep_hook(heapwatch_before_sleep_hook_t hook,
                                                     void* user_ctx);
esp_err_t heapwatch_register_before_deep_sleep_hook(heapwatch_before_sleep_hook_t hook,
                                                    void* user_ctx);

/** Veto the current sleep_light / sleep_deep command from inside a before-sleep hook. */
void heapwatch_must_stay_awake(void);

/* ---------------------------------------------------------------------------
   Managed config catalogs
   --------------------------------------------------------------------------- */

typedef struct {
    const char* nvs_key;
    const char* name;
    const char* description;
    const char* type;
    bool secret;
    bool patchable;
    const char* unit;
    int min_value;
    int max_value;
} heapwatch_config_field_def_t;

typedef struct {
    const char* id;
    const char* title;
    const char* nvs_namespace;
    const heapwatch_config_field_def_t* fields;
    size_t field_count;
} heapwatch_config_catalog_def_t;

/**
 * Register a managed NVS config catalog. Safe before or after heapwatch_initialize().
 * The namespace becomes managed and supports config/set, config/persist, and
 * config/revert via MQTT commands. A late register re-uploads the catalog to the cloud.
 */
esp_err_t heapwatch_register_config_catalog(const heapwatch_config_catalog_def_t* catalog);

/* ---------------------------------------------------------------------------
   Filesystem volumes
   --------------------------------------------------------------------------- */

/**
 * Map a flash partition label to a VFS mount path. Safe before or after
 * heapwatch_initialize() (and after heapwatch_deinit() before the next init).
 *
 * Auto-mount on: connector mounts this label at mount_path instead of /<label>
 * if you register before it mounts. After a connector-owned mount, a different
 * path is rejected — do not unmount/remount live.
 * Auto-mount off: required so the file browser can use a host-mounted volume.
 *
 */
esp_err_t heapwatch_register_fs_volume(const char* partition_label, const char* mount_path);

/**
 * Register an SD card the host app already mounted (or will mount shortly).
 * Safe before or after heapwatch_initialize().
 *
 * A card has no partition-table entry, so the connector cannot discover it the way it
 * discovers flash volumes — the host must name it. `label` is a host-chosen id (e.g. "sd0")
 * used as the volume id; `mount_path` is the live VFS root (e.g. "/sd"). Call once per card.
 * If the path is not a VFS root yet, the mapping is kept and adopted on the next fs/storage.
 *
 */
esp_err_t heapwatch_register_sd_volume(const char* label, const char* mount_path);

/** Build metadata from generated heapwatch_build_metadata.h (compile-time). */
const char* heapwatch_get_build_identifier(void);
/** Client app git-describe string (HEAPWATCH_APP_SOURCE_VERSION). */
const char* heapwatch_get_app_source_version(void);
/** Connector component git-describe string (HEAPWATCH_CONNECTOR_SOURCE_VERSION). */
const char* heapwatch_get_connector_source_version(void);

#ifdef __cplusplus
}
#endif

#endif /* HEAPWATCH_H */
