# Choose which ROM to embed. Defaults to Super Mario Land; pass
# `idf.py -DBOARD_ROM=blargg_06 build` to switch to the Blargg cpu_instrs
# 06-ld_r_r sub-test for correctness checks on the OLED.
if(NOT DEFINED BOARD_ROM)
    set(BOARD_ROM "sml")
endif()

# Reuse the ROM blobs that already live alongside port/esp32s3 to avoid
# duplicating binaries in the tree.
get_filename_component(GBJIT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(_rom_dir "${GBJIT_ROOT}/port/esp32s3/main")

if(BOARD_ROM STREQUAL "blargg_06")
    set(_embed_rom "${_rom_dir}/blargg_06.gb")
    set(_rom_define "BOARD_ROM_BLARGG=1")
else()
    set(_embed_rom "${_rom_dir}/sml.gb")
    set(_rom_define "BOARD_ROM_SML=1")
endif()

idf_component_register(
    SRCS
        "app_main.c"
        "ssd1306.c"
        "oled_task.c"
    PRIV_REQUIRES gbjit esp_timer esp_driver_i2c esp_driver_gpio
    EMBED_FILES "${_embed_rom}"
    INCLUDE_DIRS "."
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE ${_rom_define})
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -O3)

# Optional wall-clock frame-rate cap. The pacer (in app_main.c) busy-
# waits at every frame boundary to hold emulation at the requested
# fps. Set to 60 (or any positive value) to lock to that rate; leave
# at 0 (the default) to let the dispatcher run as fast as Core 0 can
# carry it. The pacer code itself is #if'd out at 0 — no runtime cost
# when disabled.
#   idf.py -DGBJIT_FRAME_LIMIT_FPS=60 build
if(NOT DEFINED GBJIT_FRAME_LIMIT_FPS)
    set(GBJIT_FRAME_LIMIT_FPS 0)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    GBJIT_FRAME_LIMIT_FPS=${GBJIT_FRAME_LIMIT_FPS})
