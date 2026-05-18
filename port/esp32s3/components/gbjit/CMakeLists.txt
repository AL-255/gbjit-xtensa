# gbjit component: portable core + JIT codegen built for the ESP32-S3.
# The host xtensa simulator (jit/xtensa_sim.c) is excluded — on the target
# we execute the generated code natively.

get_filename_component(GBJIT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)

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
        "${CMAKE_CURRENT_LIST_DIR}/jit_trampolines.S"
    INCLUDE_DIRS
        "${GBJIT_ROOT}/include"
        "${GBJIT_ROOT}/core"
        "${GBJIT_ROOT}/jit"
    REQUIRES esp_common esp_system heap freertos esp_timer
)

# Suppress shadow warnings IDF's headers trigger; keep -Wall otherwise.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-shadow)

# Qemu bench harness reads dispatcher.blocks_compiled / chain_hits /
# .blocks_executed for its [BENCH] line, so it needs the counters
# populated. The boards/* firmwares (which don't print stats) opt out
# of this define and get a faster hot loop.
target_compile_definitions(${COMPONENT_LIB} PRIVATE DEBUG=1)

# Codecache arena size override. The dispatcher.c default is 64 KB;
# the bench sweep (benchmark/run_cache_sweep.sh) rebuilds with several
# values to plot performance vs cache size.
if(DEFINED GBJIT_ARENA_KB)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE GBJIT_ARENA_KB=${GBJIT_ARENA_KB}u)
endif()

# Move ppu_tick onto Core 1. See core/ppu_thread.h for the design and
# the synchronisation contract with sm83_service_interrupts. Opt-out by
# passing -DGBJIT_PPU_ASYNC=0 to fall back to the in-line PPU tick.
if(NOT DEFINED GBJIT_PPU_ASYNC)
    set(GBJIT_PPU_ASYNC 1)
endif()
if(GBJIT_PPU_ASYNC)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE GBJIT_PPU_ASYNC=1)
endif()
