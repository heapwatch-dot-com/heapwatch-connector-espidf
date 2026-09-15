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

#include "heapwatch/Lifecycle/NotificationsFromSystemEventLoop.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "heapwatch.h"
#include "heapwatch/Lifecycle/Bootstrap/ConnectorBootstrap.h"
#include "heapwatch/Lifecycle/ConnectorLifecycle.h"
#include "heapwatch/Mqtt/MqttTelemetryClient.h"
#include "heapwatch/Network/DeviceNetworkReporter.h"
#include "sdkconfig.h"

#include <cinttypes>

namespace Heapwatch::Lifecycle {
namespace {

constexpr char TAG[] = "HwLife";

constexpr EventBits_t SYSTEM_EVENT_LOOP_STA_START = BIT0;
constexpr EventBits_t SYSTEM_EVENT_LOOP_STA_CONNECTED = BIT1;
constexpr EventBits_t SYSTEM_EVENT_LOOP_STA_DISCONNECTED = BIT2;
constexpr EventBits_t SYSTEM_EVENT_LOOP_GOT_IP = BIT3;
constexpr EventBits_t SYSTEM_EVENT_LOOP_LOST_IP = BIT4;
constexpr EventBits_t SYSTEM_EVENT_LOOP_NETIF_UP = BIT5;
constexpr EventBits_t SYSTEM_EVENT_LOOP_NETIF_DOWN = BIT6;
constexpr EventBits_t SYSTEM_EVENT_LOOP_ALL_NOTIFICATIONS =
    SYSTEM_EVENT_LOOP_STA_START | SYSTEM_EVENT_LOOP_STA_CONNECTED |
    SYSTEM_EVENT_LOOP_STA_DISCONNECTED | SYSTEM_EVENT_LOOP_GOT_IP | SYSTEM_EVENT_LOOP_LOST_IP |
    SYSTEM_EVENT_LOOP_NETIF_UP | SYSTEM_EVENT_LOOP_NETIF_DOWN;

constexpr uint32_t HANDLE_NOTIFICATIONS_WAIT_SLICE_MS = 200;

EventGroupHandle_t g_notifications_event_group = nullptr;
SemaphoreHandle_t g_apply_mutex = nullptr;
bool g_handlers_registered = false;

esp_event_handler_instance_t g_wifi_sta_start = nullptr;
esp_event_handler_instance_t g_wifi_sta_connected = nullptr;
esp_event_handler_instance_t g_wifi_sta_disconnected = nullptr;
esp_event_handler_instance_t g_ip_sta_got = nullptr;
esp_event_handler_instance_t g_ip_eth_got = nullptr;
esp_event_handler_instance_t g_ip_sta_lost = nullptr;
esp_event_handler_instance_t g_ip_eth_lost = nullptr;
#ifdef IP_EVENT_PPP_GOT_IP
esp_event_handler_instance_t g_ip_ppp_got = nullptr;
#endif
#ifdef IP_EVENT_PPP_LOST_IP
esp_event_handler_instance_t g_ip_ppp_lost = nullptr;
#endif
#ifdef IP_EVENT_NETIF_UP
esp_event_handler_instance_t g_ip_netif_up = nullptr;
#endif
#ifdef IP_EVENT_NETIF_DOWN
esp_event_handler_instance_t g_ip_netif_down = nullptr;
#endif

void onSystemEventLoopEvent(void* /*arg*/, esp_event_base_t event_base, int32_t event_id,
                            void* /*event_data*/) {
    if (g_notifications_event_group == nullptr) {
        return;
    }

    EventBits_t bit = 0;
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                bit = SYSTEM_EVENT_LOOP_STA_START;
                break;
            case WIFI_EVENT_STA_CONNECTED:
                bit = SYSTEM_EVENT_LOOP_STA_CONNECTED;
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                bit = SYSTEM_EVENT_LOOP_STA_DISCONNECTED;
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
            case IP_EVENT_STA_GOT_IP:
            case IP_EVENT_ETH_GOT_IP:
#ifdef IP_EVENT_PPP_GOT_IP
            case IP_EVENT_PPP_GOT_IP:
#endif
                bit = SYSTEM_EVENT_LOOP_GOT_IP;
                break;
            case IP_EVENT_STA_LOST_IP:
            case IP_EVENT_ETH_LOST_IP:
#ifdef IP_EVENT_PPP_LOST_IP
            case IP_EVENT_PPP_LOST_IP:
#endif
                bit = SYSTEM_EVENT_LOOP_LOST_IP;
                break;
#ifdef IP_EVENT_NETIF_UP
            case IP_EVENT_NETIF_UP:
                bit = SYSTEM_EVENT_LOOP_NETIF_UP;
                break;
#endif
#ifdef IP_EVENT_NETIF_DOWN
            case IP_EVENT_NETIF_DOWN:
                bit = SYSTEM_EVENT_LOOP_NETIF_DOWN;
                break;
#endif
            default:
                break;
        }
    }

    if (bit != 0) {
        xEventGroupSetBits(g_notifications_event_group, bit);
    }
}

void registerOne(esp_event_base_t base, int32_t event_id, esp_event_handler_instance_t* instance) {
    ESP_ERROR_CHECK(esp_event_handler_instance_register(base, event_id, &onSystemEventLoopEvent,
                                                        nullptr, instance));
}

void unregisterOne(esp_event_base_t base, int32_t event_id, esp_event_handler_instance_t* instance) {
    if (*instance == nullptr) {
        return;
    }
    (void)esp_event_handler_instance_unregister(base, event_id, *instance);
    *instance = nullptr;
}

void applyGotIp() {
    auto& connector = MqttTelemetryClient::getInstance();
    connector.setNetworkReady(true);
    Heapwatch::Lifecycle::Bootstrap::clearWifiPermanentlyFailed();
    Heapwatch::Network::markTopologyDirty();
    connector.queueEvent(HEAPWATCH_EVENT_NETIF_GOT_IP);
    if (connector.getStatus().initialized) {
        connector.queueEvent(HEAPWATCH_EVENT_INTERNET_CONNECTED);
    }
    ESP_LOGI(TAG, "Network IP acquired");
    onRuntimeConnectivityChanged();
}

void applyLostIp() {
    auto& connector = MqttTelemetryClient::getInstance();
    connector.setNetworkReady(false);
    Heapwatch::Network::markTopologyDirty();
    connector.queueEvent(HEAPWATCH_EVENT_NETIF_LOST_IP);
    if (connector.getStatus().initialized) {
        connector.queueEvent(HEAPWATCH_EVENT_INTERNET_DISCONNECTED);
    }
    ESP_LOGI(TAG, "Network IP lost");
    onRuntimeConnectivityChanged();
}

}  // namespace

void registerHandlersOnSystemEventLoop() {
    if (g_handlers_registered) {
        return;
    }
    if (g_notifications_event_group == nullptr) {
        g_notifications_event_group = xEventGroupCreate();
    }
    if (g_apply_mutex == nullptr) {
        g_apply_mutex = xSemaphoreCreateMutex();
    }

    registerOne(WIFI_EVENT, WIFI_EVENT_STA_START, &g_wifi_sta_start);
    registerOne(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &g_wifi_sta_connected);
    registerOne(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &g_wifi_sta_disconnected);
    registerOne(IP_EVENT, IP_EVENT_STA_GOT_IP, &g_ip_sta_got);
    registerOne(IP_EVENT, IP_EVENT_ETH_GOT_IP, &g_ip_eth_got);
    registerOne(IP_EVENT, IP_EVENT_STA_LOST_IP, &g_ip_sta_lost);
    registerOne(IP_EVENT, IP_EVENT_ETH_LOST_IP, &g_ip_eth_lost);
#ifdef IP_EVENT_PPP_GOT_IP
    registerOne(IP_EVENT, IP_EVENT_PPP_GOT_IP, &g_ip_ppp_got);
#endif
#ifdef IP_EVENT_PPP_LOST_IP
    registerOne(IP_EVENT, IP_EVENT_PPP_LOST_IP, &g_ip_ppp_lost);
#endif
#ifdef IP_EVENT_NETIF_UP
    registerOne(IP_EVENT, IP_EVENT_NETIF_UP, &g_ip_netif_up);
#endif
#ifdef IP_EVENT_NETIF_DOWN
    registerOne(IP_EVENT, IP_EVENT_NETIF_DOWN, &g_ip_netif_down);
#endif
    g_handlers_registered = true;
}

void unregisterHandlersOnSystemEventLoop() {
    if (!g_handlers_registered) {
        return;
    }
    unregisterOne(WIFI_EVENT, WIFI_EVENT_STA_START, &g_wifi_sta_start);
    unregisterOne(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &g_wifi_sta_connected);
    unregisterOne(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &g_wifi_sta_disconnected);
    unregisterOne(IP_EVENT, IP_EVENT_STA_GOT_IP, &g_ip_sta_got);
    unregisterOne(IP_EVENT, IP_EVENT_ETH_GOT_IP, &g_ip_eth_got);
    unregisterOne(IP_EVENT, IP_EVENT_STA_LOST_IP, &g_ip_sta_lost);
    unregisterOne(IP_EVENT, IP_EVENT_ETH_LOST_IP, &g_ip_eth_lost);
#ifdef IP_EVENT_PPP_GOT_IP
    unregisterOne(IP_EVENT, IP_EVENT_PPP_GOT_IP, &g_ip_ppp_got);
#endif
#ifdef IP_EVENT_PPP_LOST_IP
    unregisterOne(IP_EVENT, IP_EVENT_PPP_LOST_IP, &g_ip_ppp_lost);
#endif
#ifdef IP_EVENT_NETIF_UP
    unregisterOne(IP_EVENT, IP_EVENT_NETIF_UP, &g_ip_netif_up);
#endif
#ifdef IP_EVENT_NETIF_DOWN
    unregisterOne(IP_EVENT, IP_EVENT_NETIF_DOWN, &g_ip_netif_down);
#endif
    g_handlers_registered = false;
}

void handleNotificationsFromSystemEventLoop() {
    if (g_notifications_event_group == nullptr) {
        return;
    }
    if (g_apply_mutex != nullptr) {
        xSemaphoreTake(g_apply_mutex, portMAX_DELAY);
    }

    const EventBits_t bits = xEventGroupWaitBits(g_notifications_event_group,
                                                 SYSTEM_EVENT_LOOP_ALL_NOTIFICATIONS, pdTRUE,
                                                 pdFALSE, 0);

    if ((bits & SYSTEM_EVENT_LOOP_STA_START) != 0) {
        Heapwatch::Lifecycle::Bootstrap::connectManagedWifi();
    }
    if ((bits & SYSTEM_EVENT_LOOP_STA_CONNECTED) != 0) {
        Heapwatch::Network::markTopologyDirty();
        MqttTelemetryClient::getInstance().queueEvent(HEAPWATCH_EVENT_NETIF_UP);
    }
    if ((bits & SYSTEM_EVENT_LOOP_STA_DISCONNECTED) != 0) {
        Heapwatch::Network::markTopologyDirty();
        MqttTelemetryClient::getInstance().queueEvent(HEAPWATCH_EVENT_NETIF_DOWN);
#if CONFIG_HEAPWATCH_MANAGE_WIFI
        MqttTelemetryClient::getInstance().queueEvent(HEAPWATCH_EVENT_INTERNET_DISCONNECTED);
        Heapwatch::Lifecycle::Bootstrap::handleManagedWifiDisconnected();
#endif
    }
    if ((bits & SYSTEM_EVENT_LOOP_GOT_IP) != 0) {
        applyGotIp();
    }
    if ((bits & SYSTEM_EVENT_LOOP_LOST_IP) != 0) {
        applyLostIp();
    }
    if ((bits & SYSTEM_EVENT_LOOP_NETIF_UP) != 0) {
        Heapwatch::Network::markTopologyDirty();
        MqttTelemetryClient::getInstance().queueEvent(HEAPWATCH_EVENT_NETIF_UP);
    }
    if ((bits & SYSTEM_EVENT_LOOP_NETIF_DOWN) != 0) {
        Heapwatch::Network::markTopologyDirty();
        MqttTelemetryClient::getInstance().queueEvent(HEAPWATCH_EVENT_NETIF_DOWN);
    }

    if (g_apply_mutex != nullptr) {
        xSemaphoreGive(g_apply_mutex);
    }
}

void waitForNotificationFromSystemEventLoop(uint32_t timeout_ms) {
    if (g_notifications_event_group == nullptr) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        return;
    }
    xEventGroupWaitBits(g_notifications_event_group, SYSTEM_EVENT_LOOP_ALL_NOTIFICATIONS, pdFALSE,
                        pdFALSE, pdMS_TO_TICKS(timeout_ms));
}

void waitWhileHandlingNotificationsFromSystemEventLoop(uint32_t timeout_ms) {
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        handleNotificationsFromSystemEventLoop();
        const TickType_t now = xTaskGetTickCount();
        if (now >= deadline) {
            return;
        }
        uint32_t remaining_ms =
            static_cast<uint32_t>((deadline - now) * static_cast<uint32_t>(portTICK_PERIOD_MS));
        if (remaining_ms > HANDLE_NOTIFICATIONS_WAIT_SLICE_MS) {
            remaining_ms = HANDLE_NOTIFICATIONS_WAIT_SLICE_MS;
        }
        waitForNotificationFromSystemEventLoop(remaining_ms);
    }
}

}  // namespace Heapwatch::Lifecycle
