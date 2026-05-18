cmake_minimum_required(VERSION 3.22)

# Path to the gbjit-xtensa repo root (this file's grandparent).
get_filename_component(GBJIT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# Expose core+jit as an ESP-IDF "extra component". The host main is excluded;
# port/esp32/main provides the target app_main.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(gbjit_esp32)
