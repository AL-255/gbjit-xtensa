# Pick which embedded ROM the bench harness runs. Default keeps the
# existing Blargg cpu_instrs sub-test 06; pass -DBENCH_ROM=sml to swap
# in Super Mario Land for game-workload measurements.
if(NOT DEFINED BENCH_ROM)
    set(BENCH_ROM "blargg_06")
endif()

if(BENCH_ROM STREQUAL "sml")
    set(_embed_rom "sml.gb")
    set(_rom_define "BENCH_ROM_SML=1")
else()
    set(_embed_rom "blargg_06.gb")
    set(_rom_define "BENCH_ROM_BLARGG=1")
endif()

idf_component_register(
    SRCS "app_main.c"
    PRIV_REQUIRES gbjit esp_timer
    EMBED_FILES "${_embed_rom}"
    INCLUDE_DIRS ""
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE ${_rom_define})

# Tuning knob for the benchmark harness; override on the cmake line via
# `idf.py -DBENCH_CYCLES_BUDGET=500000 build` when capturing qemu traces.
if(DEFINED BENCH_CYCLES_BUDGET)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "BENCH_CYCLES_BUDGET=${BENCH_CYCLES_BUDGET}ull")
endif()
# Which mode(s) to run. Default: both. Override with
#   idf.py -DBENCH_MODE_INTERP_ONLY=1 build   or   -DBENCH_MODE_JIT_ONLY=1 build
# for per-mode qemu traces (so the trace files don't mix the two passes).
if(BENCH_MODE_INTERP_ONLY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BENCH_MODE_INTERP_ONLY=1)
endif()
if(BENCH_MODE_JIT_ONLY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BENCH_MODE_JIT_ONLY=1)
endif()
if(BENCH_MODE_JIT_WARM_ONLY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BENCH_MODE_JIT_WARM_ONLY=1)
endif()
if(BENCH_MODE_JIT_NOCACHE_ONLY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BENCH_MODE_JIT_NOCACHE_ONLY=1)
endif()
if(BENCH_MODE_JIT_NOPREFETCH_ONLY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BENCH_MODE_JIT_NOPREFETCH_ONLY=1)
endif()
