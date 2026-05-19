cmake_minimum_required(VERSION 3.16)
project(gbjit_xtensa C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wshadow -Wno-unused-parameter -fno-strict-aliasing)

# Host build: define DEBUG so the dispatcher's per-block counters
# (blocks_compiled, chain_hits, smc_invalidations, ...) are populated.
# gbjit_host's printf line and the unit tests both read these. The
# production board firmware (boards/*) intentionally does NOT define
# DEBUG, so those increments are compiled out of the hot path.
add_compile_definitions(DEBUG=1)

# Propagate the JIT/PPU build-time toggles so the host build can A/B
# them too. Each is optional — when undefined the C source picks up
# its own #ifndef default. See boards/HTIT-WB32LAF_V3.2/components/
# gbjit/CMakeLists.txt for the full list and what each one does.
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
        GBJIT_PPU_ASYNC)
    if(DEFINED ${_opt})
        add_compile_definitions(${_opt}=${${_opt}})
    endif()
endforeach()

# Portable core
add_library(gbjit_core STATIC
    core/memory.c
    core/ppu.c
    core/ppu_thread.c
    core/sm83_decoder.c
    core/sm83_interp.c
)
target_include_directories(gbjit_core PUBLIC include core)

# JIT codegen (Xtensa LX7 emitter is portable C; consumers map to executable memory)
add_library(gbjit_jit STATIC
    jit/emit_xtensa.c
    jit/codegen.c
    jit/codecache.c
    jit/dispatcher.c
    jit/xtensa_sim.c
)
target_include_directories(gbjit_jit PUBLIC include core jit)
target_link_libraries(gbjit_jit PUBLIC gbjit_core)

# Host port (Linux mmap PROT_EXEC arena, ELF tests)
add_executable(gbjit_host
    port/host_linux/main.c
)
target_link_libraries(gbjit_host PRIVATE gbjit_jit)

enable_testing()
add_subdirectory(tests)
