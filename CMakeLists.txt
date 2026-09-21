file(GLOB_RECURSE HEAPWATCH_CONNECTOR_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/heapwatch/*.cpp"
)

set(HEAPWATCH_REQUIRES spiffs fatfs wear_levelling joltwallet__littlefs)

# ESP-IDF 5.x ships cJSON as the built-in `json` component. IDF 6+ uses
# espressif/cjson from the Component Manager (see idf_component.yml).
if(IDF_VERSION_MAJOR LESS 6)
    list(APPEND HEAPWATCH_REQUIRES json)
endif()

set(HEAPWATCH_PRIV_REQUIRES
    driver bootloader_support mqtt esp_wifi esp_event esp_netif nvs_flash lwip esp-tls
    mbedtls efuse app_update fatfs sdmmc espcoredump esp_http_client spi_flash)

# BLE reporting is off unless the host sets HEAPWATCH_ENABLE_BLE_REPORTING.
# Do not link the BT controller (NimBLE or Bluedroid) otherwise.
if(CONFIG_HEAPWATCH_ENABLE_BLE_REPORTING)
    list(APPEND HEAPWATCH_PRIV_REQUIRES bt)
endif()

        idf_component_register(
    SRCS ${HEAPWATCH_CONNECTOR_SRCS}
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${HEAPWATCH_PRIV_REQUIRES}
    REQUIRES ${HEAPWATCH_REQUIRES}
    EMBED_TXTFILES "certs/heapwatch_local_ca.pem"
)

# Log relay includes FreeRTOS task names; names longer than this are truncated.
# Cannot Kconfig-select an int default, so warn at configure time instead.
if(CONFIG_HEAPWATCH_ENABLE_LOG_COLLECTION AND DEFINED CONFIG_FREERTOS_MAX_TASK_NAME_LEN)
    if(CONFIG_FREERTOS_MAX_TASK_NAME_LEN LESS 16)
        message(WARNING
            "CONFIG_FREERTOS_MAX_TASK_NAME_LEN is ${CONFIG_FREERTOS_MAX_TASK_NAME_LEN} "
            "(< 16). Heapwatch log collection may truncate task names; raise it in "
            "menuconfig → Component config → FreeRTOS → Kernel.")
    endif()
endif()

# Pre-build: git describe for client app + connector -> heapwatch_build_metadata.h
set(HEAPWATCH_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(HEAPWATCH_METADATA_HEADER "${HEAPWATCH_GENERATED_DIR}/heapwatch_build_metadata.h")
set(HEAPWATCH_SCRIPTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/scripts")

add_custom_command(
    OUTPUT ${HEAPWATCH_METADATA_HEADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${HEAPWATCH_GENERATED_DIR}
    COMMAND ${PYTHON} ${HEAPWATCH_SCRIPTS_DIR}/generate_build_metadata.py
            --app-root ${CMAKE_SOURCE_DIR}
            --connector-root ${CMAKE_CURRENT_SOURCE_DIR}
            --output ${HEAPWATCH_METADATA_HEADER}
    COMMENT "Generate heapwatch_build_metadata.h (app + connector versions, git or empty)"
    VERBATIM
)

add_custom_target(heapwatch_regenerate_build_metadata DEPENDS ${HEAPWATCH_METADATA_HEADER})
add_dependencies(${COMPONENT_LIB} heapwatch_regenerate_build_metadata)
target_include_directories(${COMPONENT_LIB} PUBLIC "${HEAPWATCH_GENERATED_DIR}")

# Post-link fingerprint refresh rewrites this header; force recompile of TUs that embed it.
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/heapwatch/Lifecycle/PublicCApi.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/heapwatch/Lifecycle/ConnectorLifecycle.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/heapwatch/DeviceCommand/CommandCatalog.cpp"
    PROPERTIES OBJECT_DEPENDS "${HEAPWATCH_METADATA_HEADER}"
)
