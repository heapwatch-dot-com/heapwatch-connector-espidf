#include "heapwatch/Lifecycle/BootSync/ConnectorBootSync.h"

#include "esp_log.h"
#include "sdkconfig.h"

#include "heapwatch/StreamIngress/StreamIngressClient.h"
#include "heapwatch/Network/DeviceNetworkReporter.h"
#include "heapwatch/Radio/BluetoothLe/BluetoothReporter.h"
#include "heapwatch/Mqtt/HeapwatchConnector.h"
#include "heapwatch_config.h"
#include "heapwatch/Metrics/MetricsCollector.h"

namespace Heapwatch::Lifecycle::BootSync {

namespace {

constexpr char TAG[] = "HwLife";
bool g_boot_static_sync_done = false;

}  // namespace

void runBootStaticSync() {
    if (g_boot_static_sync_done) {
        return;
    }
    g_boot_static_sync_done = true;

    ESP_LOGI(TAG, "MQTT-up boot sync — enqueueing static payloads");

    Heapwatch::Metrics::refreshConnectorGaugeLimits();
    HeapwatchConnector::getInstance().queueSpecs();
    HeapwatchConnector::getInstance().flushParameterBounds();
    Heapwatch::Network::publishFullSync();
#if CONFIG_HEAPWATCH_ENABLE_BLE_REPORTING
    Heapwatch::Radio::BluetoothLe::publishFullSync();
#endif
    HeapwatchConnector::getInstance().queueEvent(HEAPWATCH_EVENT_MQTT_CONNECTED);
    Heapwatch::StreamIngress::requestBootStreamUploads();
}

void resetBootStaticSync() { g_boot_static_sync_done = false; }

}  // namespace Heapwatch::Lifecycle::BootSync
