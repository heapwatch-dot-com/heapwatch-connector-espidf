// Copyright (c) 2025-2026 Viktor Jamrich Enterprises s.r.o. All rights reserved.
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

#pragma once

#include <cstddef>
#include <cstdint>

#include "device_credentials/DeviceCredentialsStore.h"
#include "heapwatch_connector/HeapwatchConnector.h"

// Phase 5 (main.cpp extraction): host-app boilerplate that every client integration
// otherwise has to write by hand today — Wi-Fi bring-up, SNTP time sync, boot-credential
// resolution (NVS / device enrollment / Kconfig lab settings), and the ESP log-vprintf
// relay pipeline. Each piece stays independently Kconfig-gated (HEAPWATCH_MANAGE_WIFI,
// HEAPWATCH_MANAGE_TIME_SYNC, HEAPWATCH_ENABLE_LOG_COLLECTION), same behavior as before the
// extraction — this only moves the code out of main.cpp so a client app_main can stay short.
// Deliberately kept as free functions in `main/` (not yet folded into HeapwatchConnector
// itself, nor extracted to the standalone component repo) per this project's documented
// approach: "develop in place inside heapwatch-firmware-crasher; extract at the end."
namespace heapwatch::connector_bootstrap {

struct BootCredentials {
    char tenant_slug[HEAPWATCH_MAX_TENANT_SLUG_LEN] = {};
    char device_uid[HEAPWATCH_MAX_DEVICE_UID_LEN] = {};
    char mqtt_password[HEAPWATCH_MAX_MQTT_PASSWORD_LEN] = {};
    char device_api_key[HEAPWATCH_MAX_DEVICE_API_KEY_LEN] = {};
    char stream_api_key[HEAPWATCH_MAX_STREAM_API_KEY_LEN] = {};
    char coredump_upload_url[256] = {};
    char coredump_auth_header[HEAPWATCH_MAX_DEVICE_API_KEY_LEN + 16] = {};
};

/** I1: idempotent; tolerates the event loop already existing (host app may have created it). */
void ensureDefaultEventLoop();

/**
 * Resolves device credentials in priority order: NVS-provisioned (device_credentials) ->
 * device enrollment (if HEAPWATCH_ENABLE_DEVICE_ENROLLMENT and an enrollment key is set).
 * When device enrollment is enabled, Kconfig lab credentials are not used.
 */
bool resolveBootCredentials(BootCredentials* creds);

/**
 * Brings up Wi-Fi STA and blocks until connected when HEAPWATCH_MANAGE_WIFI=y (aborts on
 * exhausted retries, matching prior main.cpp behavior). Logs and returns immediately,
 * performing no netif/Wi-Fi init, when HEAPWATCH_MANAGE_WIFI=n -- the host app owns
 * networking and must call HeapwatchConnector::setNetworkReady(true) itself once up.
 */
void initWifiIfManaged();

/** Disconnect and reconnect Wi-Fi when HEAPWATCH_MANAGE_WIFI=y. Optional offline duration. */
bool requestWifiReconnect(uint32_t duration_s);

/**
 * Blocks (bounded, ~20s) until SNTP time sync succeeds when HEAPWATCH_MANAGE_TIME_SYNC=y.
 * Logs and returns immediately when HEAPWATCH_MANAGE_TIME_SYNC=n -- the host app is
 * responsible for system time in that case.
 */
void syncTimeIfManaged();

/**
 * Installs the ESP log vprintf hook that feeds the log-relay ring buffer, when
 * HEAPWATCH_ENABLE_LOG_COLLECTION=y. No-op otherwise. Call once, early in app_main, before
 * relying on any relayed log lines -- matches prior ordering (installed before
 * setNetworkReady(true)).
 */
void installLogRelayHookIfEnabled();

/**
 * Starts the "hw_log_forward" FreeRTOS task that drains the log-relay ring buffer into the
 * connector's log queue, when HEAPWATCH_ENABLE_LOG_COLLECTION=y. No-op otherwise.
 */
void startLogRelayTaskIfEnabled();

uint32_t getWifiReconnectsTotal();
uint32_t getLogLinesDroppedTotal();
uint32_t getLogMqttDroppedTotal();

}  // namespace heapwatch::connector_bootstrap
