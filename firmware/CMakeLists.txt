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
