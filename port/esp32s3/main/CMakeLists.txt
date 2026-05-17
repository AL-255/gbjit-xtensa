idf_component_register(
    SRCS "app_main.c"
    PRIV_REQUIRES gbjit esp_timer
    EMBED_FILES "blargg_06.gb"
    INCLUDE_DIRS ""
)

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
