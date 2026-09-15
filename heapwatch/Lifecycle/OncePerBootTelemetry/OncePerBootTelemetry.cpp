#include "heapwatch/Lifecycle/OncePerBootTelemetry/OncePerBootTelemetry.h"

#include "esp_log.h"
#include "sdkconfig.h"

#include "heapwatch/StreamIngress/StreamIngressClient.h"
#include "heapwatch/Network/DeviceNetworkReporter.h"
#include "heapwatch/Radio/BluetoothLe/BluetoothReporter.h"
#include "heapwatch/Mqtt/MqttTelemetryClient.h"
#include "heapwatch_config.h"
#include "heapwatch/Metrics/MetricsCollector.h"

namespace Heapwatch::Lifecycle::OncePerBootTelemetry {

namespace {

constexpr char TAG[] = "HwLife";
bool g_boot_static_sync_done = false;

}  // namespace

void enqueueOncePerBootStaticTelemetry() {
    if (g_boot_static_sync_done) {
        return;
    }
    g_boot_static_sync_done = true;

    ESP_LOGI(TAG, "MQTT-up once-per-boot static telemetry — enqueueing static payloads");

    Heapwatch::Metrics::refreshConnectorGaugeLimits();
    MqttTelemetryClient::getInstance().queueSpecs();
    MqttTelemetryClient::getInstance().flushParameterBounds();
    Heapwatch::Network::publishFullSync();
#if CONFIG_HEAPWATCH_ENABLE_BLE_REPORTING
    Heapwatch::Radio::BluetoothLe::publishFullSync();
#endif
    MqttTelemetryClient::getInstance().queueEvent(HEAPWATCH_EVENT_MQTT_CONNECTED);
    Heapwatch::StreamIngress::requestBootStreamUploads();
}

void resetOncePerBootStaticTelemetry() { g_boot_static_sync_done = false; }

}  // namespace Heapwatch::Lifecycle::OncePerBootTelemetry
