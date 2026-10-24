cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Build only main and what it requires, as host_proto does.
idf_build_set_property(MINIMAL_BUILD ON)
project(host_fatfs)
