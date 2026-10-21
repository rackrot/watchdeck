cmake_minimum_required(VERSION 3.22)
# Local overrides (Wi-Fi credentials) are optional and gitignored. Changes to
# sdkconfig.defaults.local only apply after deleting sdkconfig.
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults.local")
endif()

set(PROJECT_VER "0.2.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(watchdeck)

# A debug-only, build-id-stamped copy of the ELF for `mise run fw:symbols`. It is a separate file
# because elf2image hashes watchdeck.elf, so stamping that in place would change the stored hash.
set(app_elf ${CMAKE_PROJECT_NAME}.elf)
set(debug_elf ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.debug.elf)
if(NOT PYTHON)
    message(FATAL_ERROR "IDF's PYTHON is not set, so ${CMAKE_PROJECT_NAME}.debug.elf can't be stamped")
endif()
set(stamp_script ${CMAKE_CURRENT_LIST_DIR}/tools/stamp_build_id.py)
add_custom_command(
    OUTPUT ${debug_elf}
    COMMAND ${PYTHON} ${stamp_script} --objcopy ${CMAKE_OBJCOPY} $<TARGET_FILE:${app_elf}> ${debug_elf}
    DEPENDS ${app_elf} ${stamp_script}
    COMMENT "Stamping ${CMAKE_PROJECT_NAME}.debug.elf"
    VERBATIM
)
add_custom_target(debug_elf ALL DEPENDS ${debug_elf})
