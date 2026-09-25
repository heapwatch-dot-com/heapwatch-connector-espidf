// Copyright 2026 Heapwatch
// SPDX-License-Identifier: Apache-2.0

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
