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

// Production endpoints

#if defined(HEAPWATCH_DEV_LOCAL_NETWORK)

#define HEAPWATCH_API_BASE_URL    "https://192.168.50.210"
#define HEAPWATCH_MQTT_BROKER_URI "mqtts://192.168.50.210:8883"
#define HEAPWATCH_STREAM_INGRESS_HOST "192.168.50.210"
#define HEAPWATCH_STREAM_INGRESS_TCP_PORT 8910
#define HEAPWATCH_API_TLS_SERVER_NAME "api.heapwatch.local"
#define HEAPWATCH_MQTT_TLS_SERVER_NAME "mqtts.mqtt.heapwatch.local"
#define HEAPWATCH_STREAM_TLS_SERVER_NAME "stream.heapwatch.local"

#else

#define HEAPWATCH_API_BASE_URL    "https://api.heapwatch.com"
#define HEAPWATCH_MQTT_BROKER_URI "mqtts://mqtts.mqtt.heapwatch.com:8883"
#define HEAPWATCH_STREAM_INGRESS_HOST "stream.heapwatch.com"
#define HEAPWATCH_STREAM_INGRESS_TCP_PORT 8910
#define HEAPWATCH_API_TLS_SERVER_NAME "api.heapwatch.com"
#define HEAPWATCH_MQTT_TLS_SERVER_NAME "mqtts.mqtt.heapwatch.com"
#define HEAPWATCH_STREAM_TLS_SERVER_NAME "stream.heapwatch.com"

#endif
