# gbjit component for the HTIT-WB32LAF_V3.2 board. Mirrors
# port/esp32s3/components/gbjit/CMakeLists.txt; both build the same
# repo-root source files. The jit_trampolines.S is borrowed from
# port/esp32s3 since the CALL0/windowed bridging is identical on every
# S3 board.

get_filename_component(GBJIT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(_S3_TRAMP "${GBJIT_ROOT}/port/esp32s3/components/gbjit/jit_trampolines.S")

idf_component_register(
    SRCS
        "${GBJIT_ROOT}/core/memory.c"
        "${GBJIT_ROOT}/core/ppu.c"
        "${GBJIT_ROOT}/core/ppu_thread.c"
        "${GBJIT_ROOT}/core/sm83_decoder.c"
        "${GBJIT_ROOT}/core/sm83_interp.c"
        "${GBJIT_ROOT}/jit/emit_xtensa.c"
        "${GBJIT_ROOT}/jit/codecache.c"
        "${GBJIT_ROOT}/jit/codegen.c"
        "${GBJIT_ROOT}/jit/dispatcher.c"
        "${_S3_TRAMP}"
    INCLUDE_DIRS
        "${GBJIT_ROOT}/include"
        "${GBJIT_ROOT}/core"
        "${GBJIT_ROOT}/jit"
    REQUIRES esp_common esp_system heap freertos esp_timer
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-shadow)

# Move ppu_tick onto Core 1 by default. See core/ppu_thread.h.
if(NOT DEFINED GBJIT_PPU_ASYNC)
    set(GBJIT_PPU_ASYNC 1)
endif()
if(GBJIT_PPU_ASYNC)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE GBJIT_PPU_ASYNC=1)
endif()
