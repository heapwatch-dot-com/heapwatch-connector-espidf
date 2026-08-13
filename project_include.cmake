# Heapwatch connector — auto-invoked when a client firmware project depends on this component.
#
# Post-link: finalize HEAPWATCH_BUILD_IDENTIFIER and upload firmware to Heapwatch API.

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_LIST_DIR)
    return()
endif()

set(HEAPWATCH_SCRIPTS_DIR "${CMAKE_CURRENT_LIST_DIR}/scripts")
set(HEAPWATCH_METADATA_HEADER "${CMAKE_BINARY_DIR}/generated/heapwatch_build_metadata.h")

add_custom_target(heapwatch_firmware_upload ALL
    COMMAND ${PYTHON} ${HEAPWATCH_SCRIPTS_DIR}/finalize_and_upload_firmware.py
            --build-dir ${CMAKE_BINARY_DIR}
            --metadata-header ${HEAPWATCH_METADATA_HEADER}
            --sdkconfig ${CMAKE_SOURCE_DIR}/sdkconfig
            --project-name ${CMAKE_PROJECT_NAME}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Finalize HEAPWATCH_BUILD_IDENTIFIER and upload firmware (after .bin exists)"
    VERBATIM
)

add_dependencies(heapwatch_firmware_upload gen_project_binary)
