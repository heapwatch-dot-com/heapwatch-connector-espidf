// Copyright (c) 2026 Heapwatch. All rights reserved.
#pragma once

#include "esp_http_client.h"
#include "esp_tls.h"
#include "mqtt_client.h"

namespace Heapwatch::Helpers::Tls {

enum class Server {
    SERVER_API,
    SERVER_MQTT,
    SERVER_STREAM,
};

/** SNI / certificate name for this Heapwatch edge (`.local` on the lab LAN, `.com` on the public servers). */
const char* serverName(Server server);

/**
 * Encrypt is always on. The embedded lab CA is used only when HEAPWATCH_DEV_LOCAL_NETWORK is set.
 * Every other build verifies the public Heapwatch servers with the CA bundle.
 * Always sets `common_name` so SNI is the name on the cert, even when dialing a LAN IP.
 */
void applyToHttpClient(esp_http_client_config_t& config);
void applyToMqtt(esp_mqtt_client_config_t& config);
void applyToEspTls(esp_tls_cfg_t& cfg, Server server);

}  // namespace Heapwatch::Helpers::Tls
