file(GLOB_RECURSE HEAPWATCH_CONNECTOR_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/boot_cycle/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/connector_settings/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/device_command/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/device_credential_rotation/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/device_credentials/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/device_enrollment/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/device_fs/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/device_identity/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/device_network/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/device_ota/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/device_radio/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/device_specs/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/heapwatch_connector/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/heapwatch_dispatcher/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/metrics_collector/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/sleep_tracking/SleepTracking.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/storage_stats/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/stream_ingress/*.cpp"
)

set(HEAPWATCH_REQUIRES spiffs)

# ESP-IDF 5.x ships cJSON as the built-in `json` component. IDF 6+ uses
# espressif/cjson from the Component Manager (see idf_component.yml).
if(IDF_VERSION_MAJOR LESS 6)
    list(APPEND HEAPWATCH_REQUIRES json)
endif()

set(HEAPWATCH_PRIV_REQUIRES
    driver bootloader_support mqtt esp_wifi esp_event esp_netif nvs_flash lwip esp-tls
    mbedtls efuse app_update fatfs sdmmc espcoredump esp_http_client bt)

idf_component_register(
    SRCS ${HEAPWATCH_CONNECTOR_SRCS}
    INCLUDE_DIRS "." "heapwatch_connector"
    PRIV_REQUIRES ${HEAPWATCH_PRIV_REQUIRES}
    REQUIRES ${HEAPWATCH_REQUIRES}
    EMBED_TXTFILES "certs/heapwatch_local_ca.pem"
)

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
    COMMENT "Generate heapwatch_build_metadata.h (app + connector git describe)"
    VERBATIM
)

add_custom_target(heapwatch_regenerate_build_metadata DEPENDS ${HEAPWATCH_METADATA_HEADER})
add_dependencies(${COMPONENT_LIB} heapwatch_regenerate_build_metadata)
target_include_directories(${COMPONENT_LIB} PUBLIC "${HEAPWATCH_GENERATED_DIR}")
