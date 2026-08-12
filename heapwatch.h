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

/**
 * Heapwatch Connector for ESP-IDF — public C API (Phase 5b).
 *
 * Pure C ABI: include this header from main.c or main.cpp. Internals are C++;
 * this surface is the only compatibility contract for client firmware (VER1).
 *
 * Typical app_main (credentials from Kconfig / NVS via bootstrap):
 *
 *   nvs_flash_init();
 *   heapwatch_ensure_event_loop();
 *   heapwatch_init_wifi_if_managed();
 *   heapwatch_sync_time_if_managed();
 *   heapwatch_config_t cfg = HEAPWATCH_CONFIG_DEFAULT();
 *   cfg.build_identifier = BUILD_IDENTIFIER;
 *   ESP_ERROR_CHECK(heapwatch_init(&cfg));
 *   heapwatch_install_log_relay_if_enabled();
 *   heapwatch_set_network_ready(true);
 *   heapwatch_start_log_relay_if_enabled();
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
   Config / status (CFG1 — flat POD, no version field)
   --------------------------------------------------------------------------- */

/**
 * Init config. String fields may be NULL to resolve credentials via the
 * connector bootstrap (NVS → device enrollment → Kconfig lab settings).
 * When providing explicit credentials, tenant_slug and mqtt_password are
 * required; device_uid may be NULL/empty (MAC-derived fallback is applied
 * only on the bootstrap path — pass a UID when supplying credentials
 * explicitly).
 */
typedef struct {
    const char* tenant_slug;
    const char* device_uid;
    const char* mqtt_password;
    const char* device_api_key;
    /** Optional; used as the coredump/build fingerprint. NULL = omit. */
    const char* build_identifier;
} heapwatch_config_t;

/** Zero-init helper — all pointers NULL (bootstrap credential resolution). */
static inline heapwatch_config_t heapwatch_config_default(void) {
    heapwatch_config_t config;
    config.tenant_slug = NULL;
    config.device_uid = NULL;
    config.mqtt_password = NULL;
    config.device_api_key = NULL;
    config.build_identifier = NULL;
    return config;
}
#define HEAPWATCH_CONFIG_DEFAULT() heapwatch_config_default()

typedef struct {
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
 * Idempotent default event-loop create (I1). Safe if the host already created it.
 */
void heapwatch_ensure_event_loop(void);

/** Wi-Fi STA bring-up when HEAPWATCH_MANAGE_WIFI=y; no-op when n. */
void heapwatch_init_wifi_if_managed(void);

/** Bounded SNTP wait when HEAPWATCH_MANAGE_TIME_SYNC=y; no-op when n. */
void heapwatch_sync_time_if_managed(void);

/**
 * Apply credentials, start dispatcher/metrics threads. Does not set network
 * ready — call heapwatch_set_network_ready(true) once connectivity is up.
 */
esp_err_t heapwatch_init(const heapwatch_config_t* config);

void heapwatch_deinit(void);

void heapwatch_set_network_ready(bool ready);

/**
 * Optional: attribute STARTING_UP / coredump headers to a boot-cycle counter.
 * Call after heapwatch_init(), before queuing the STARTING_UP event.
 */
void heapwatch_set_boot_cycle_context(uint32_t boot_cycle, uint32_t crash_boot_cycle);

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

void heapwatch_install_log_relay_if_enabled(void);
void heapwatch_start_log_relay_if_enabled(void);

/* ---------------------------------------------------------------------------
   Telemetry
   --------------------------------------------------------------------------- */

/**
 * Queue a standard event. Pass boot_cycle < 0 to omit the boot_cycle JSON field.
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

/** POW1 / CELL1 typed wrappers. */
esp_err_t heapwatch_update_power_mv(const char* source_name, int64_t mv);
esp_err_t heapwatch_update_power_mv_ranged(const char* source_name, int64_t mv, int64_t min_mv,
                                           int64_t max_mv);
esp_err_t heapwatch_update_power_percent(const char* source_name, double percent);
esp_err_t heapwatch_update_power_state(const char* source_name, heapwatch_power_state_t state);
esp_err_t heapwatch_update_cellular_signal(int rssi_dbm, int ber);

esp_err_t heapwatch_queue_specs(void);

/* ---------------------------------------------------------------------------
   Remote commands (CMD1)
   --------------------------------------------------------------------------- */

/**
 * Register a client command (topic suffix). Built-ins ping/specs_refresh/restart
 * cannot be overridden. Table is fixed-size (8); returns ESP_ERR_HEAPWATCH_NO_SPACE
 * when full.
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
 * Register a managed NVS config catalog. The namespace becomes managed and supports
 * config/set, config/persist, and config/revert via MQTT commands.
 */
esp_err_t heapwatch_register_config_catalog(const heapwatch_config_catalog_def_t* catalog);

#ifdef __cplusplus
}
#endif

#endif /* HEAPWATCH_H */
