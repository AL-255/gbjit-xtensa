cmake_minimum_required(VERSION 3.22)

# Path to the gbjit-xtensa repo root (this file's grandparent of grandparent).
get_filename_component(GBJIT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# Expose core+jit as an ESP-IDF "extra component", reusing the same
# wrapper layout as port/esp32s3.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)

# Release-build defaults for this board. Each is a CMake variable so a
# user can still override at configure time:
#   idf.py -DGBJIT_HALT_STEP_CYCLES=4 build
# These are the values that benchmark/results/board_option_sweep.md
# documents as the steady-state-best for SML on the Heltec V3 OLED:
#   sync PPU (correct BG)
#   1024-cycle halt step (large halt-loop strides on HALT-heavy ROMs)
#   crop the per-line render to the OLED's centre 128×64 window
#   OLED I²C blits at native bandwidth (no artificial cap)
#   chain-batching 4 blocks per outer iter (default already)
#
# QEMU virtual-LCD builds (-DGBJIT_QEMU_LCD=1) override the crop
# defaults to render the full 160×144 GB frame — the host viewer
# shows the whole screen, not just the OLED's centre window.
# GBJIT_PPU_ASYNC is intentionally NOT defaulted or propagated here.
# The source uses `#ifdef GBJIT_PPU_ASYNC` (definedness, not value), so
# exporting `GBJIT_PPU_ASYNC=0` project-wide would still select the
# async path. The gbjit component's own CMakeLists handles it correctly
# — it only defines the macro (as =1) when the value is truthy, so the
# default and an explicit `-DGBJIT_PPU_ASYNC=0` both mean synchronous.
if(NOT DEFINED GBJIT_HALT_STEP_CYCLES)
    set(GBJIT_HALT_STEP_CYCLES 1024)
endif()
if(GBJIT_QEMU_LCD)
    # Full-frame render for the virtual LCD.
    if(NOT DEFINED GBJIT_PPU_DRAW_MIN_LY)
        set(GBJIT_PPU_DRAW_MIN_LY 0)
    endif()
    if(NOT DEFINED GBJIT_PPU_DRAW_MAX_LY)
        set(GBJIT_PPU_DRAW_MAX_LY 144)
    endif()
    if(NOT DEFINED GBJIT_PPU_DRAW_MIN_X)
        set(GBJIT_PPU_DRAW_MIN_X 0)
    endif()
    if(NOT DEFINED GBJIT_PPU_DRAW_MAX_X)
        set(GBJIT_PPU_DRAW_MAX_X 160)
    endif()
else()
    if(NOT DEFINED GBJIT_PPU_DRAW_MIN_LY)
        set(GBJIT_PPU_DRAW_MIN_LY 40)
    endif()
    if(NOT DEFINED GBJIT_PPU_DRAW_MAX_LY)
        set(GBJIT_PPU_DRAW_MAX_LY 104)
    endif()
    if(NOT DEFINED GBJIT_PPU_DRAW_MIN_X)
        set(GBJIT_PPU_DRAW_MIN_X 16)
    endif()
    if(NOT DEFINED GBJIT_PPU_DRAW_MAX_X)
        set(GBJIT_PPU_DRAW_MAX_X 144)
    endif()
endif()
if(NOT DEFINED GBJIT_HTIT_OLED_MIN_INTERVAL_MS)
    set(GBJIT_HTIT_OLED_MIN_INTERVAL_MS 0)
endif()

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
        GBJIT_PPU_DRAW_MIN_X
        GBJIT_PPU_DRAW_MAX_X
        GBJIT_ARENA_KB
        GBJIT_HTIT_OLED_ENABLE
        GBJIT_HTIT_OLED_MIN_INTERVAL_MS
        GBJIT_DISPATCHER_SERVICE_SHORTCUT
        GBJIT_DISPATCHER_CHAIN_BATCH)
    if(DEFINED ${_opt})
        idf_build_set_property(COMPILE_DEFINITIONS
            "${_opt}=${${_opt}}" APPEND)
    endif()
endforeach()

project(gbjit_htit_wb32la)
