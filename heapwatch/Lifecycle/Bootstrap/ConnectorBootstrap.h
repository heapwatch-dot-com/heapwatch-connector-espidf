// Copyright 2025-2026 Heapwatch
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>

#include "heapwatch/Enrollment/Credentials/DeviceCredentialsStore.h"
#include "heapwatch/Mqtt/MqttTelemetryClient.h"

// Boilerplate a client integration would otherwise write by hand: Wi-Fi bring-up, SNTP time
// sync, boot-credential resolution (NVS provisioned credentials → device enrollment), and the
// ESP log-vprintf relay pipeline. Each piece is independently Kconfig-gated
// (HEAPWATCH_MANAGE_WIFI, HEAPWATCH_MANAGE_TIME_SYNC, HEAPWATCH_ENABLE_LOG_COLLECTION).
// Driven by connector_lifecycle; not part of the public heapwatch.h surface.
namespace Heapwatch::Lifecycle::Bootstrap {

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

bool loadBootCredentials(BootCredentials* creds);

/**
 * Non-blocking Wi-Fi STA bring-up when HEAPWATCH_MANAGE_WIFI=y. When disabled, the host app
 * owns networking; IP events on any esp_netif still drive connector network readiness.
 */
void startWifiIfManaged();

void startNetworkReporters();

/** Zero the reconnect attempt count. Called when Wi-Fi gets an IP. */
void resetWifiReconnectAttempts();

/** Runs factory enrollment loop on the calling task (bootstrap only — never app_main). */
bool runFactoryEnrollment(BootCredentials* creds);

/** Disconnect and reconnect Wi-Fi when HEAPWATCH_MANAGE_WIFI=y. Optional offline duration. */
bool requestWifiReconnect(uint32_t duration_s);

/** `esp_wifi_connect()` when HEAPWATCH_MANAGE_WIFI=y. Call from handleNotificationsFromSystemEventLoop only. */
void connectManagedWifi();

/** On disconnect, schedule the next join. Call from handleNotificationsFromSystemEventLoop only. */
void handleManagedWifiDisconnected();

/** Start a scheduled join once the wait has passed. Call from handleNotificationsFromSystemEventLoop only. */
void serviceManagedWifiReconnect();

/**
 * Blocks (bounded, ~100s) until SNTP time sync succeeds when HEAPWATCH_MANAGE_TIME_SYNC=y.
 * Intended for the bootstrap task only — never call from app_main.
 */
void syncTimeIfManaged();

/**
 * Installs the ESP log vprintf hook that feeds the log-relay ring buffer, when
 * HEAPWATCH_ENABLE_LOG_COLLECTION=y. Called automatically by heapwatch_initialize().
 */
void installLogRelayHookIfEnabled();

/**
 * Starts the "hw_log_forward" FreeRTOS task that drains the log-relay ring buffer into the
 * connector's log queue, when HEAPWATCH_ENABLE_LOG_COLLECTION=y.
 */
void startLogRelayTaskIfEnabled();

void stopLogRelay();

void stopManagedWifi();

void stopTimeSyncIfManaged();

/** Fill-null-only backfill of claimed MAC / UID128 for provisioned devices (once per boot). */
void syncClaimedIdentitiesIfProvisioned();

uint32_t getWifiReconnectsTotal();
uint32_t getLogLinesDroppedTotal();
uint32_t getLogMqttDroppedTotal();

}  // namespace Heapwatch::Lifecycle::Bootstrap
