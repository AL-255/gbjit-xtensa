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

# Production firmware: -O3 on the hot path (codegen, dispatcher,
# interp, PPU). This component owns the dispatcher's busy loop, so the
# higher optimisation level pays off most here. IDF's default is -O2
# via CONFIG_COMPILER_OPTIMIZATION_PERF; we override per-component to
# keep the upgrade scoped. DEBUG is intentionally NOT defined here, so
# the per-block stat-counter increments compile out (see
# include/gbjit_debug.h).
target_compile_options(${COMPONENT_LIB} PRIVATE -O3)

# Bump arena to 96 KB (default is 64). The cache-size sweep showed
# +25 % cold-JIT throughput at 96 vs 64 because SML's ~133-block
# working set fits at 96 with comfortable headroom; at 64 a handful
# of cold blocks have to chain-miss-compile during the bench window.
# The dispatcher.c "latent codegen bug past ~100 unique blocks"
# warning applies to Blargg 06-ld r,r specifically — SML at 96 KB
# is fine in the qemu sweep, and the real-S3 firmware exercises only
# this ROM.
target_compile_definitions(${COMPONENT_LIB} PRIVATE GBJIT_ARENA_KB=96u)

# Move ppu_tick onto Core 1 by default. See core/ppu_thread.h.
if(NOT DEFINED GBJIT_PPU_ASYNC)
    set(GBJIT_PPU_ASYNC 1)
endif()
if(GBJIT_PPU_ASYNC)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE GBJIT_PPU_ASYNC=1)
endif()
