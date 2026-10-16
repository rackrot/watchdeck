cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Build only main and what it requires. Without this, IDF builds every linux-capable component,
# and esp_driver_ana_cmpr then fails to resolve on the linux target.
idf_build_set_property(MINIMAL_BUILD ON)
project(host_proto)
