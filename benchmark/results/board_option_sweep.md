# Board-side option sweep — HTIT-WB32LAF_V3.2

ROM: Super Mario Land (sml.gb). Each configuration was built with
`benchmark/bench_options_board.py`, flashed to the Heltec board, and
sampled for 32 seconds with the first 8 seconds discarded (boot + SML
title screen). 24 useful fps samples per row.

## Headline result

Min FPS rose from **54** (defaults) to **86** with all opt-in
optimizations stacked. The user's 90-fps target is reached only with
the OLED disabled entirely; with the OLED display active and capped
at 100 ms blit interval, the steady-state floor is 79-80 fps.

## Detailed sweep

| Config | min | avg | max | Notes |
|---|---:|---:|---:|---|
| baseline (no opts) | 54 | 78.0 | 139 | sync PPU, no crop, halt step 4, OLED unlimited |
| sync + all_opts | 60 | 100.8 | 412 | hs1024 + row+col crop + OLED 100 ms cap |
| async + OLED 100 ms | **79** | 141.6 | 538 | async PPU on Core 1 + crop + OLED 100 ms |
| async + OLED 500 ms | 73 | 148.0 | 495 | OLED 500 ms = ~2 fps display |
| async + no OLED | **86** | 154.4 | 527 | display off |

The "all opts" stack:

| Flag | Value | What it does |
|---|---|---|
| `GBJIT_HALT_STEP_CYCLES` | 1024 | Halt loop advances 1024 GB cycles per iter |
| `GBJIT_PPU_DRAW_MIN_LY` / `MAX_LY` | 40 / 104 | Skip rows outside OLED's visible 64-row band |
| `GBJIT_PPU_DRAW_MIN_X` / `MAX_X` | 16 / 144 | Skip columns outside OLED's visible 128-col band |
| `GBJIT_HTIT_OLED_MIN_INTERVAL_MS` | 100 | OLED blits at most every 100 ms (~10 fps display) |
| `GBJIT_PPU_ASYNC` | 1 | Run PPU on Core 1 — frees Core 0 from ppu_tick + draws |

## What works and what doesn't

The big wins on **average** fps were halt-step increase and PPU
async. The big wins on **minimum** fps were async + crop:

* `hs256` → `hs1024`: +avg, ~+1 fps min (avg jumped from 80 → 120, min barely moved)
* Row crop alone: **+8 fps min**
* Async PPU: **+9 fps min** (over the sync ceiling)
* OLED rate cap from unlimited → 100 ms: **+6 fps min** in sync mode

What didn't move the needle:

* `GBJIT_CHAIN_PREDICTOR_WAYS=4` vs 2: ±1 fps (noise)
* `GBJIT_INLINE_JP_HL=0`, `GBJIT_INLINE_ADD_HL_RR=0`: ±1 fps (noise on SML; likely matters more on Blargg)
* Column crop alone: 0 fps (the LUT renderer amortises tile lookups; trimming 32 px of edge doesn't reduce tile count)
* `GBJIT_FRAMEBUFFER_DOUBLE_BUFFER=1`: ±1 fps
* `GBJIT_DISPATCHER_SERVICE_SHORTCUT=1`: −6 fps min (regresses with halt inner loop)
* `GBJIT_ARENA_KB=96`: ±0 fps
* Halt step beyond 1024 (`hs4096`): no further gain
* `GBJIT_PPU_FAST_BG_RENDERER=0` (legacy renderer): ±1 fps (LUT path is byte-identical to peanut_gb on host but the speedup is marginal because the inner store loop is byte-unaligned for non-zero SCX)

## What blocks 90 fps min with the OLED on

`async + no OLED` reaches 86 fps min consistently. `async + OLED 100 ms`
sits at 79. The 6-7 fps gap is Core 1's OLED activity stealing Core 1
time from the PPU thread, which delays IF.VBLANK on Core 0 and slows
the halt loop. Possible future attacks:

1. **Move OLED I²C interrupt to Core 0** so Core 1 is pure PPU. Tricky
   because the ESP-IDF i2c_master driver picks the interrupt's affinity
   based on where init was called from.
2. **Move OLED rendering off the I²C critical path** — DMA chained so
   compose and blit overlap, then sleep until next frame.
3. **Lazy flag materialisation** (task #20). Cuts per-op cost on the
   ALU-heavy code paths inside SML's main loop. Most directly attacks
   the active-gameplay floor.
4. **Direct block linking**. Removes the ~30 Xtensa cycles of
   dispatcher overhead per chain hit. Modest individual win, but stacks.
5. **A 3rd-party board with SPI OLED** would let us run the display at
   ~20 MHz instead of 1 MHz I²C — 20× the blit bandwidth.

Reproduce: `python3 benchmark/bench_options_board.py`. The harness
temporarily patches sdkconfig.defaults + the gbjit / main CMakeLists
to enable DEBUG + INFO logs (so the oled_task fps print reaches the
UART), and restores them on exit.
