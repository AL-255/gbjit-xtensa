cmake_minimum_required(VERSION 3.16)
project(gbjit_xtensa C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wshadow -Wno-unused-parameter -fno-strict-aliasing)

# Portable core
add_library(gbjit_core STATIC
    core/memory.c
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
