#include "ConnectorBootSync.h"

#include "esp_log.h"
#include "sdkconfig.h"

#include "../stream_ingress/StreamIngressClient.h"
#include "../device_network/DeviceNetworkReporter.h"
#include "../device_radio/BluetoothReporter.h"
#include "../heapwatch_connector/HeapwatchConnector.h"
#include "../heapwatch_config.h"
#include "../metrics_collector/MetricsCollector.h"

namespace heapwatch::connector_boot_sync {

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

    heapwatch::metrics_collector::refreshConnectorGaugeLimits();
    HeapwatchConnector::getInstance().queueSpecs();
    HeapwatchConnector::getInstance().flushParameterBounds();
    heapwatch::device_network::publishFullSync();
#if CONFIG_HEAPWATCH_ENABLE_BLE_REPORTING
    heapwatch::device_radio::publishFullSync();
#endif
    HeapwatchConnector::getInstance().queueEvent(HEAPWATCH_EVENT_MQTT_CONNECTED);
    heapwatch::stream_ingress::requestBootStreamUploads();
}

void resetBootStaticSync() { g_boot_static_sync_done = false; }

}  // namespace heapwatch::connector_boot_sync
