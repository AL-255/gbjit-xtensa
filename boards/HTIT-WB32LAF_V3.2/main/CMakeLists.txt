# Choose which ROM to embed. Defaults to Super Mario Land. Pass
#   -DBOARD_ROM=blargg_06   Blargg cpu_instrs 06-ld_r_r correctness test
#   -DBOARD_ROM=tetris      Tetris (roms/Tetris_JUE_V1.1.gb)
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
elseif(BOARD_ROM STREQUAL "tetris")
    set(_embed_rom "${GBJIT_ROOT}/roms/Tetris_JUE_V1.1.gb")
    set(_rom_define "BOARD_ROM_TETRIS=1")
else()
    set(_embed_rom "${_rom_dir}/sml.gb")
    set(_rom_define "BOARD_ROM_SML=1")
endif()

idf_component_register(
    SRCS
        "app_main.c"
        "ssd1306.c"
        "oled_task.c"
        "qemu_lcd.c"
    PRIV_REQUIRES gbjit esp_timer esp_driver_i2c esp_driver_gpio esp_driver_uart
    EMBED_FILES "${_embed_rom}"
    INCLUDE_DIRS "."
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE ${_rom_define})
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -O3)

# QEMU virtual-LCD mode. When GBJIT_QEMU_LCD=1, app_main streams the
# 160x144 framebuffer over UART1 (see main/qemu_lcd.c) instead of
# driving the I2C OLED — the host viewer tools/qemu_lcd.py renders it.
# Build + run with tools/qemu_lcd_run.sh.
if(NOT DEFINED GBJIT_QEMU_LCD)
    set(GBJIT_QEMU_LCD 0)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    GBJIT_QEMU_LCD=${GBJIT_QEMU_LCD})

# FPS probe. When GBJIT_FPS_PROBE=1, app_main installs a frame-complete
# callback that measures emulation throughput (frames/wall-second) and
# logs a running average + lowest 1-second window over the serial
# console. Off by default — purely a measurement aid.
#   idf.py -DGBJIT_FPS_PROBE=1 build
if(NOT DEFINED GBJIT_FPS_PROBE)
    set(GBJIT_FPS_PROBE 0)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    GBJIT_FPS_PROBE=${GBJIT_FPS_PROBE})

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
