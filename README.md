[heapwatch-connector.webm](https://github.com/user-attachments/assets/82d0646a-6a02-4be4-9a4d-537fe4e7ba61)
# Heapwatch Connector for ESP-IDF

Heapwatch Connector is an ESP-IDF component that you add to your ESP32-family project to see what's actually going on inside your device.

Heapwatch can collect your logs, core dumps, rich time series data, CPU utilization, heap and task statistics, configuration, storage data and much more - all in real time!

**This component requires ESP-IDF ≥ 5.3**

**Full integration guide:** [docs.heapwatch.com/integration](https://docs.heapwatch.com/integration/getting-started/overview)

## Getting Started

The simplest way to start is to use the ["Connect Your Device" wizard in Heapwatch UI](https://app.heapwatch.com/dashboard/connect-first-device).

This wizard will guide you through the few neccessary changes that must be done to configure the connector correctly.

Because the connector configuration also includes adding or changing sdkconfig directives based on what features you want to enable, putting them into this would make them incomplete and misleading.

Refer to the wizard in Heapwatch UI for proper configuration.

## License

Apache License 2.0. See [LICENSE](LICENSE).
