# gbjit component: portable core + JIT codegen built for the ESP32-S3.
# The host xtensa simulator (jit/xtensa_sim.c) is excluded — on the target
# we execute the generated code natively.

get_filename_component(GBJIT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)

idf_component_register(
    SRCS
        "${GBJIT_ROOT}/core/memory.c"
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
    REQUIRES esp_common esp_system heap
)

# Suppress shadow warnings IDF's headers trigger; keep -Wall otherwise.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-shadow)
