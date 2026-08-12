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

// Production REST/MQTT endpoints (K1). These are NOT Kconfig options — every
// client build always talks to the real Heapwatch backend. There is no
// supported way to point production firmware at a different endpoint.
//
// HEAPWATCH_DEV_LOCAL_NETWORK (K2) is an internal-only switch for our own
// firmware dev loop, never exposed via Kconfig and never shipped to clients:
// it swaps both endpoints to our LAN dev stack. Deliberately decoupled from
// CONFIG_HEAPWATCH_BUILD_ENV, which is a pure business/reporting label sent
// to the backend (development/testing/staging/production), not a
// network-target switch — flip this macro locally, don't repurpose BUILD_ENV.
//
// To point a local dev build at the LAN stack, uncomment the line below (or
// pass -DHEAPWATCH_DEV_LOCAL_NETWORK via CMakeLists.txt) before building.

// #define HEAPWATCH_DEV_LOCAL_NETWORK

#if defined(HEAPWATCH_DEV_LOCAL_NETWORK)

#define HEAPWATCH_API_BASE_URL    "http://192.168.50.210:8090"
#define HEAPWATCH_MQTT_BROKER_URI "mqtts://192.168.50.210:8883"
#define HEAPWATCH_STREAM_INGRESS_HOST "192.168.50.210"
#define HEAPWATCH_STREAM_INGRESS_TCP_PORT 8910

#else

#define HEAPWATCH_API_BASE_URL    "https://api.heapwatch.com"
#define HEAPWATCH_MQTT_BROKER_URI "mqtts://mqtts.mqtt.heapwatch.com:8883"
#define HEAPWATCH_STREAM_INGRESS_HOST "ingress-stream-tcp.heapwatch.com"
#define HEAPWATCH_STREAM_INGRESS_TCP_PORT 8910

#endif
