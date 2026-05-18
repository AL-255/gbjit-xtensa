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

# Profiling mode: enable the dispatcher's stat counters so the board's
# oled_task can log them once a second. The increments are cheap (a
# few extra u64 stores per block call) and we need them to identify
# bottlenecks; flip back off (drop the DEBUG=1 line) once the data is
# in hand.
target_compile_definitions(${COMPONENT_LIB} PRIVATE DEBUG=1)

# PPU placement.
#
# Async (default): ppu_tick runs on Core 1 as a pinned FreeRTOS task,
# keeping Core 0 uncontested for the dispatcher. The historical
# cross-core race that broke BG scrolling — Core 1's per-scanline
# reads of SCX/SCY/BGP/LCDC seeing Core 0's writes intended for some
# other frame — is fixed in core/ppu.c by latching all BG-relevant IO
# regs into ppu_latched_* fields at end-of-VBlank (and at LCD enable).
# ppu_draw_line then renders the next frame against the snapshot, so
# every scanline of one frame sees the same SCX/etc., regardless of
# what Core 0 has written in the meantime. WY was already latched the
# same way. Per-line raster effects (mid-frame SCX) are NOT supported
# while latched — SML and the rest of our compat targets don't do
# them. Sprites read OAM live (atomic via OAM DMA on the CPU side).
#
# Sync fallback (set GBJIT_PPU_ASYNC=0): runs ppu_tick on Core 0
# inside sm83_service_interrupts. Same call site as the reference
# interpreter. Drops throughput on this workload by ~40% (PPU work
# shares Core 0 with the dispatcher) but is the most conservative
# correctness option if a new IO-related race shows up.
if(NOT DEFINED GBJIT_PPU_ASYNC)
    set(GBJIT_PPU_ASYNC 1)
endif()
if(GBJIT_PPU_ASYNC)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE GBJIT_PPU_ASYNC=1)
endif()
