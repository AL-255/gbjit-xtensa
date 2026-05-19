cmake_minimum_required(VERSION 3.22)

# Path to the gbjit-xtensa repo root (this file's grandparent of grandparent).
get_filename_component(GBJIT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# Expose core+jit as an ESP-IDF "extra component", reusing the same
# wrapper layout as port/esp32s3.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)

# Propagate JIT/PPU compile-time toggles project-wide. Flags that
# affect the mmu struct layout (e.g. GBJIT_FRAMEBUFFER_DOUBLE_BUFFER)
# MUST be visible to every translation unit that includes
# core/memory.h — gbjit, main, anything else — or different TUs see
# different `mmu` sizes and crash on first cross-component access.
# Pass any subset on the command line, e.g.
#   idf.py -DGBJIT_FRAMEBUFFER_DOUBLE_BUFFER=1 -DGBJIT_CHAIN_PREDICTOR_WAYS=1 build
foreach(_opt
        GBJIT_CHAIN_PREDICTOR_WAYS
        GBJIT_INLINE_JP_HL
        GBJIT_INLINE_ADD_HL_RR
        GBJIT_DISPATCHER_HALT_INNER_LOOP
        GBJIT_HALT_STEP_CYCLES
        GBJIT_FRAMEBUFFER_DOUBLE_BUFFER
        GBJIT_PPU_FAST_BG_RENDERER
        GBJIT_PPU_SKIP_DRAW
        GBJIT_PPU_DRAW_MIN_LY
        GBJIT_PPU_DRAW_MAX_LY
        GBJIT_PPU_ASYNC)
    if(DEFINED ${_opt})
        idf_build_set_property(COMPILE_DEFINITIONS
            "${_opt}=${${_opt}}" APPEND)
    endif()
endforeach()

project(gbjit_htit_wb32la)
