// Copyright (c) 2026 Heapwatch. All rights reserved.

#include "heapwatch/Helpers/Tls/Tls.h"

#include "esp_crt_bundle.h"
#include "heapwatch_endpoints.h"
#include "sdkconfig.h"

extern const char heapwatch_local_ca_pem_start[] asm("_binary_heapwatch_local_ca_pem_start");
extern const char heapwatch_local_ca_pem_end[] asm("_binary_heapwatch_local_ca_pem_end");

namespace Heapwatch::Helpers::Tls {

namespace {

bool usesLocalNetwork() {
#if defined(HEAPWATCH_DEV_LOCAL_NETWORK)
    return true;
#else
    return false;
#endif
}

bool skipServerCertVerify() {
#if defined(CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY) && CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
    return usesLocalNetwork();
#else
    return false;
#endif
}

const char* localCaPem() {
    return heapwatch_local_ca_pem_start;
}

size_t localCaPemBytes() {
    return static_cast<size_t>(heapwatch_local_ca_pem_end - heapwatch_local_ca_pem_start);
}

}  // namespace

const char* serverName(Server server) {
    switch (server) {
        case Server::SERVER_API:
            return HEAPWATCH_API_TLS_SERVER_NAME;
        case Server::SERVER_MQTT:
            return HEAPWATCH_MQTT_TLS_SERVER_NAME;
        case Server::SERVER_STREAM:
            return HEAPWATCH_STREAM_TLS_SERVER_NAME;
    }
    return HEAPWATCH_API_TLS_SERVER_NAME;
}

void applyToHttpClient(esp_http_client_config_t& config) {
    config.common_name = serverName(Server::SERVER_API);
    config.skip_cert_common_name_check = false;

    if (skipServerCertVerify()) {
        config.cert_pem = nullptr;
        config.cert_len = 0;
        config.crt_bundle_attach = nullptr;
        return;
    }

    if (usesLocalNetwork()) {
        config.cert_pem = localCaPem();
        config.cert_len = 0;
        config.crt_bundle_attach = nullptr;
        return;
    }

    config.cert_pem = nullptr;
    config.cert_len = 0;
    config.crt_bundle_attach = esp_crt_bundle_attach;
}

void applyToMqtt(esp_mqtt_client_config_t& config) {
    const char* name = serverName(Server::SERVER_MQTT);
    config.broker.verification.common_name = name;
    config.broker.verification.skip_cert_common_name_check = false;

    if (usesLocalNetwork()) {
        // URI is the LAN IP; hostname/common_name drive SNI for Caddy L4.
        config.broker.address.hostname = name;
    }

    if (skipServerCertVerify()) {
        config.broker.verification.certificate = nullptr;
        config.broker.verification.certificate_len = 0;
        config.broker.verification.crt_bundle_attach = nullptr;
        return;
    }

    if (usesLocalNetwork()) {
        config.broker.verification.certificate = localCaPem();
        config.broker.verification.certificate_len = 0;
        config.broker.verification.crt_bundle_attach = nullptr;
        return;
    }

    config.broker.verification.certificate = nullptr;
    config.broker.verification.certificate_len = 0;
    config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
}

void applyToEspTls(esp_tls_cfg_t& cfg, Server server) {
    cfg.common_name = serverName(server);
    cfg.skip_common_name = false;

    if (skipServerCertVerify()) {
        cfg.cacert_buf = nullptr;
        cfg.cacert_bytes = 0;
        cfg.crt_bundle_attach = nullptr;
        return;
    }

    if (usesLocalNetwork()) {
        cfg.cacert_buf = reinterpret_cast<const unsigned char*>(localCaPem());
        cfg.cacert_bytes = static_cast<unsigned int>(localCaPemBytes());
        cfg.crt_bundle_attach = nullptr;
        return;
    }

    cfg.cacert_buf = nullptr;
    cfg.cacert_bytes = 0;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
}

}  // namespace Heapwatch::Helpers::Tls
