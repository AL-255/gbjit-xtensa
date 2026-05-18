# HTIT-WB32LAF_V3.2 (ESP32-S3) — GB JIT with onboard I2C OLED

Heltec-style ESP32-S3 dev board with an integrated 128×64 SSD1306 OLED
on the user-supplied wiring:

| Signal | GPIO |
|--------|------|
| OLED SDA | 17 |
| OLED SCL | 18 |
| OLED RST | 20 |

I2C0 at 400 kHz, address 0x3C. See `main/board.h` to change pins.

## Architecture

| Core | Task | What it does |
|------|------|-------------:|
| 0 | `app_main` → `gbjit_dispatcher_run_until` | GB CPU emulation (JIT) — uncontested |
| 1 | `ppu_thread` | Runs `ppu_tick` in a tight loop (the `GBJIT_PPU_ASYNC` design from `core/ppu_thread.c`) — produces the framebuffer scanlines this board displays |
| 1 | `oled_task` | Polls `mmu->frame_seq`, crops 128×64 from the centre of the GB framebuffer, blits via I2C (~15 ms / frame). Shares Core 1 with `ppu_thread`; both are mostly idle (early-return / vTaskDelay / semaphore-blocked on I2C) so they coexist fine. |

The PPU's per-scanline renderer (`core/ppu.c::ppu_draw_line`) covers BG,
window, and DMG sprites (BG/OBJ palettes, 8×8 and 8×16, X/Y flip, BG
priority, color-0 transparency). It writes one byte of shade-0..3 into
`mmu->framebuffer[160*144]` at every mode 3 → mode 0 transition.

The OLED is monochrome, so `oled_task.c` thresholds shade ≥ 2 to "lit".
Centred 128×64 crop, no scaling.

## Build & flash

```sh
cd boards/HTIT-WB32LAF_V3.2
source $IDF_PATH/export.sh                  # ESP-IDF v6.x
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor        # serial @ 115 200
```

The default ROM is Super Mario Land. To run Blargg 06-ld_r_r instead:

```sh
idf.py -DBOARD_ROM=blargg_06 build
```

## ROM compatibility status

The emulator is in CPU-phase development; not every ROM gets all the
way to gameplay. What you'll actually see on the OLED right now:

| ROM | Serial output | OLED output |
|-----|---------------|-------------|
| `blargg_06` (LD r,r) | `06-ld r,r` + `Passed` | Test name + "Passed" rendered to VRAM, visible |
| `sml` (Super Mario Land) | none | Blank — SML init loops at PC=$01C5/$01D4 with LCD disabled. Reproduces on host interp too, so it's an emulator-side feature gap (timer/sound/joypad behaviour likely), not a board issue. Tracked separately. |

The Blargg test prints PASS/FAIL strings over the GB's serial port —
captured to UART via the `serial_sink` hook in `app_main.c` and shown in
`idf.py monitor`.
