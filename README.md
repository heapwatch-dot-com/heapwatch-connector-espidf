# Heapwatch Connector for ESP-IDF

ESP-IDF component that connects ESP32 devices to [Heapwatch](https://heapwatch.com) for crash analytics, logs, metrics, remote commands, OTA, and filesystem access.

## Add as a dependency

**Git (recommended for now):**

```yaml
# main/idf_component.yml
dependencies:
  heapwatch_connector_espidf:
    git: git@github.com:viktorjamrich/heapwatch-connector-espidf.git
    version: main
```

**Local path (development):**

```yaml
dependencies:
  heapwatch_connector_espidf:
    path: ../heapwatch-connector-espidf
```

Then in your app `main/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "main.cpp"
    REQUIRES heapwatch_connector_espidf
)
```

## Client API

Include `heapwatch.h` from `app_main` — see `heapwatch-firmware-crasher/QUICKSTART.md` in the lab firmware repo for a full integration example.

## Lab vs production endpoints

Production REST/MQTT URLs are hardcoded in `heapwatch_endpoints.h`. For local dev against a LAN Heapwatch stack, define `HEAPWATCH_DEV_LOCAL_NETWORK` in your **app** project (not in this component):

```cmake
# app CMakeLists.txt (before project())
add_compile_definitions(HEAPWATCH_DEV_LOCAL_NETWORK)
```
