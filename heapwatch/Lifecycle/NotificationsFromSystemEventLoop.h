// Copyright 2026 Heapwatch
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace Heapwatch::Lifecycle {

/** IDF Wi-Fi/IP callbacks only set event-group bits. Register those callbacks. */
void registerHandlersOnSystemEventLoop();

void unregisterHandlersOnSystemEventLoop();

/**
 * Apply Wi-Fi/IP notifications recorded on sys_evt. Call from Heapwatch tasks only
 * (hw_bootstrap, hw_dispatcher), never from an IDF callback.
 */
void handleNotificationsFromSystemEventLoop();

/** Sleep up to timeout_ms, or until sys_evt records a notification. Does not apply them. */
void waitForNotificationFromSystemEventLoop(uint32_t timeout_ms);

/**
 * Sleep up to timeout_ms while applying sys_evt notifications on this task.
 * Use instead of a bare vTaskDelay wherever Wi-Fi reconnect must keep running.
 */
void waitWhileHandlingNotificationsFromSystemEventLoop(uint32_t timeout_ms);

}  // namespace Heapwatch::Lifecycle
