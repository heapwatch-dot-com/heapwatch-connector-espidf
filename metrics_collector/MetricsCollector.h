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

#pragma once

/**
 * @brief Metrics Collector Module
 *
 * Runs in a dedicated thread that:
 * - Collects system telemetry metrics every 2 seconds (configurable)
 * - Creates parameter payloads with all collected data
 * - Sends messages to the shared MQTT queue
 *
 * Thread Priority: IDLE_PRIORITY + 2 (lower than dispatcher)
 */

namespace heapwatch::metrics_collector {

void init();

/**
 * @brief Metrics collector task function
 *
 * Main loop that:
 * 1. Sleeps for HEAPWATCH_METRICS_INTERVAL_MS (default: 2000ms)
 * 2. Collects all telemetry metrics (CPU, heap, WiFi, tasks, etc.)
 * 3. Creates one parameter payload with formatted metrics values
 * 4. Sends message to shared MQTT queue
 *
 * @param arg Task argument (unused, set to nullptr)
 */
void collectTask(void* arg);

}  // namespace heapwatch::metrics_collector
