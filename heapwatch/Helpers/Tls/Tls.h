// Copyright (c) 2026 Heapwatch. All rights reserved.
#pragma once

#include "esp_http_client.h"
#include "esp_tls.h"
#include "mqtt_client.h"

namespace Heapwatch::Helpers::Tls {

enum class Server {
    Api,
    Mqtt,
    Stream,
};

/** SNI / certificate name for this Heapwatch edge (`.local` in development, `.com` in production). */
const char* serverName(Server server);

/**
 * Encrypt is always on. Cert check is lab skip-verify, embedded Caddy CA, or production CRT bundle.
 * Always sets `common_name` so SNI is the name on the cert, even when dialing a LAN IP.
 */
void applyToHttpClient(esp_http_client_config_t& config);
void applyToMqtt(esp_mqtt_client_config_t& config);
void applyToEspTls(esp_tls_cfg_t& cfg, Server server);

}  // namespace Heapwatch::Helpers::Tls
