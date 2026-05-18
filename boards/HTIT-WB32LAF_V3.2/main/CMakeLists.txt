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
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
