# Heapwatch connector — auto-invoked when a client firmware project depends on this component.
#
# Post-link: finalize HEAPWATCH_BUILD_IDENTIFIER and upload firmware to Heapwatch API.
#
# Upload MUST NOT run on a failed compile/link. ESP-IDF's `gen_project_binary` is a phony
# target that only depends on `.bin_timestamp`; that stamp stays satisfied by leftover
# binaries from the last successful build. An `add_custom_target(ALL COMMAND …)` that
# merely `add_dependencies(gen_project_binary)` therefore still runs — and would upload
# the previous .elf/.bin under the current metadata. Gate on a stamp that is only
# rewritten when `.bin_timestamp` itself is regenerated (successful elf2image).

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_LIST_DIR)
    return()
endif()

set(HEAPWATCH_SCRIPTS_DIR "${CMAKE_CURRENT_LIST_DIR}/scripts")
set(HEAPWATCH_METADATA_HEADER "${CMAKE_BINARY_DIR}/generated/heapwatch_build_metadata.h")

function(heapwatch_register_firmware_upload)
    if(NOT CONFIG_APP_BUILD_GENERATE_BINARIES)
        return()
    endif()
    if(NOT TARGET gen_project_binary)
        return()
    endif()
    if(TARGET heapwatch_firmware_upload)
        return()
    endif()

    set(_hw_stamp "${CMAKE_BINARY_DIR}/heapwatch_firmware_upload.stamp")
    set(_hw_bin_timestamp "${CMAKE_BINARY_DIR}/.bin_timestamp")

    add_custom_command(
        OUTPUT "${_hw_stamp}"
        COMMAND ${PYTHON} ${HEAPWATCH_SCRIPTS_DIR}/finalize_and_upload_firmware.py
                --build-dir ${CMAKE_BINARY_DIR}
                --metadata-header ${HEAPWATCH_METADATA_HEADER}
                --sdkconfig ${CMAKE_SOURCE_DIR}/sdkconfig
                --project-name ${CMAKE_PROJECT_NAME}
        COMMAND ${CMAKE_COMMAND} -E touch "${_hw_stamp}"
        DEPENDS "${_hw_bin_timestamp}"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Finalize HEAPWATCH_BUILD_IDENTIFIER and upload firmware (successful app binary only)"
        VERBATIM
    )

    add_custom_target(heapwatch_firmware_upload ALL
        DEPENDS "${_hw_stamp}"
    )
endfunction()

# project_include.cmake runs before esptool_py creates `.bin_timestamp` / gen_project_binary.
# Register at the end of the project directory so those outputs exist.
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL heapwatch_register_firmware_upload)
