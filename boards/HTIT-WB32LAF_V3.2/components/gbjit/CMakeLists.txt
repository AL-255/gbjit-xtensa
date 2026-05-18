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

# Production firmware: DEBUG is OFF. The per-block stat counters
# (chain hits/misses, blocks executed/compiled, prefetch hits, ...)
# all compile out via include/gbjit_debug.h's GBJIT_STAT_INC, removing
# the u64 stores from the inner dispatcher loop and the helper-call
# bookkeeping. Re-add `target_compile_definitions(... PRIVATE DEBUG=1)`
# temporarily if you need the oled_task per-second log to dump
# dispatcher counters again.

# PPU placement.
#
# `async` ran ppu_tick on Core 1 (pinned FreeRTOS task) so Core 0's
# dispatcher kept an uncontested busy loop, but the BG-rendering path
# reads SCX/SCY/LCDC/BGP from mmu->io at draw time, while Core 0 writes
# those bytes from CPU code. The two cores see each other's stores
# without a timing relationship to cpu->cycles — for any IO reg that
# the PPU consumes per-scanline, that's a real race: Core 1 can draw
# frame F's BG using SCX written by Core 0 for frame F+1 (or vice
# versa), producing visibly broken BG scrolling. Sprites are immune
# because OAM is written atomically by the DMA copy in mmu_write8
# during the game's VBlank handler — every byte lands on Core 0 before
# Core 1's next OAM-scan.
#
# Sync mode runs ppu_tick on Core 0 inside sm83_service_interrupts —
# the same call site the reference interpreter uses — so IO reads and
# the cpu->cycles that triggered them happen on the same core in
# program order. Throughput is lower (PPU work now competes with the
# dispatcher loop on Core 0), but SML in HALT spends most cycles in
# the halt loop anyway, so the hit is modest.
#
# To go back to async: set GBJIT_PPU_ASYNC=1 in the cmake cache (or in
# the calling project file). The cross-core race is unsolved upstream
# — see core/ppu.c for the IO regs that would need cycle-stamped
# write queues before async is safe.
if(NOT DEFINED GBJIT_PPU_ASYNC)
    set(GBJIT_PPU_ASYNC 0)
endif()
if(GBJIT_PPU_ASYNC)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE GBJIT_PPU_ASYNC=1)
endif()

# --- Optional perf / correctness toggles ------------------------------
# Each defaults to the production-good value. Override at configure time
# with `idf.py -D<NAME>=<VALUE> build` to A/B against the legacy path.
#
#   GBJIT_CHAIN_PREDICTOR_WAYS=N   N-way predicted-next cache. Default
#                                  2 (best on SML); set to 1 for the
#                                  classic single-slot cache.
#
#   GBJIT_INLINE_JP_HL=0|1         Inline the JP HL terminator instead
#                                  of dispatching through sm83_step.
#                                  Default 1.
#
#   GBJIT_DISPATCHER_HALT_INNER_LOOP=0|1
#                                  Tight inner loop in the dispatcher
#                                  when cpu->halted, avoiding the
#                                  sm83_service_interrupts call cost
#                                  on every halt iter. Default 1.
#
#   GBJIT_FRAMEBUFFER_DOUBLE_BUFFER=0|1
#                                  Add a back-buffer to mmu and memcpy
#                                  to the front at frame complete.
#                                  +23 KB DRAM, ~50 µs/frame. Default 0
#                                  (sync-PPU mode largely eliminates
#                                  the tearing this was meant to fix).
if(DEFINED GBJIT_CHAIN_PREDICTOR_WAYS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        GBJIT_CHAIN_PREDICTOR_WAYS=${GBJIT_CHAIN_PREDICTOR_WAYS})
endif()
if(DEFINED GBJIT_INLINE_JP_HL)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        GBJIT_INLINE_JP_HL=${GBJIT_INLINE_JP_HL})
endif()
if(DEFINED GBJIT_DISPATCHER_HALT_INNER_LOOP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        GBJIT_DISPATCHER_HALT_INNER_LOOP=${GBJIT_DISPATCHER_HALT_INNER_LOOP})
endif()
if(DEFINED GBJIT_FRAMEBUFFER_DOUBLE_BUFFER)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        GBJIT_FRAMEBUFFER_DOUBLE_BUFFER=${GBJIT_FRAMEBUFFER_DOUBLE_BUFFER})
endif()
