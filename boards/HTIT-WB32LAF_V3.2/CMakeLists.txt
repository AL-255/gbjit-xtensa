cmake_minimum_required(VERSION 3.22)

# Path to the gbjit-xtensa repo root (this file's grandparent of grandparent).
get_filename_component(GBJIT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# Expose core+jit as an ESP-IDF "extra component", reusing the same
# wrapper layout as port/esp32s3.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(gbjit_htit_wb32la)
