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
