# Board-side option sweep — HTIT-WB32LAF_V3.2

ROM: Super Mario Land (sml.gb). Each configuration is built with
`benchmark/bench_options_board.py`, flashed to the Heltec board, and
sampled for 32 seconds with the first 8 seconds discarded
(boot + SML title screen). 24 useful fps samples per row.

## Headline

| Stack | min | avg | Δmin vs baseline |
|---|---:|---:|---:|
| baseline (defaults) | 54 | 79 | 0 |
| sync_all_opts (hs1024 + crop + OLED 100 ms) | 60-77 | ~100 | +6 to +23 |
| async + crop + OLED 100 ms | 79-83 | ~145 | +25 to +29 |
| **async + batching + crop + OLED 100 ms** | **80-83** | **147** | **+26 to +29** |
| async + batching + crop + no OLED | 74-87 | ~154 | up to +33 |

The minimum is run-to-run noisy by ±5 fps depending on what point in
SML's title-screen demo the bench captures. Numbers above are the
single-run min from one bench session; the best observed across
multiple runs was 90 fps min (async + no OLED).

## Optimization stack (all opt-in via cmake -D)

| Flag | Default | Effect on this workload |
|---|---|---|
| `GBJIT_PPU_ASYNC` | 0 | **+9 fps min** when enabled — moves PPU off Core 0 |
| `GBJIT_HALT_STEP_CYCLES` | 4 | **+6 fps min** at 1024; diminishing returns past that |
| `GBJIT_PPU_DRAW_MIN_LY / MAX_LY` | 0 / 144 | **+8 fps min** at 40/104 — skips off-screen rows |
| `GBJIT_PPU_DRAW_MIN_X / MAX_X` | 0 / 160 | no effect alone (LUT amortises) |
| `GBJIT_HTIT_OLED_MIN_INTERVAL_MS` | 0 | **+5-6 fps min** at 100; OLED steals Core 1 cycles |
| `GBJIT_DISPATCHER_CHAIN_BATCH` | 4 | **+3-4 fps min** — runs chained blocks without per-iter checks |
| `GBJIT_PPU_FAST_BG_RENDERER` | 1 | byte-identical to peanut_gb on host; ~0 fps on board |
| `GBJIT_CHAIN_PREDICTOR_WAYS` | 2 | within noise on SML |
| `GBJIT_INLINE_JP_HL` | 1 | within noise on SML |
| `GBJIT_INLINE_ADD_HL_RR` | 1 | within noise on SML |
| `GBJIT_DISPATCHER_HALT_INNER_LOOP` | 1 | **−9 fps min** if turned off; load-bearing |
| `GBJIT_FRAMEBUFFER_DOUBLE_BUFFER` | 0 | ±1 fps |
| `GBJIT_DISPATCHER_SERVICE_SHORTCUT` | 0 | regresses by 6 fps min with halt inner loop; kept off |

## Bottlenecks at the 80 fps floor

* PPU rendering itself is no longer the bottleneck — `GBJIT_PPU_SKIP_DRAW=1`
  (which makes ppu_draw_line a no-op) only moves the floor by ~2 fps
  beyond the cropped renderer.
* Core 1's OLED I²C activity steals 4-6 fps min from Core 0 even with
  the panel throttled to 10 fps, because the DMA shares the internal
  SRAM bus with the dispatcher's per-block cpu_state accesses.
* The remaining gap to 90 fps comes from the JIT's per-op cost during
  SML's active-gameplay scenes. The JIT block size (~50 GB cycles)
  and per-op overhead (~5 Xtensa cycles per GB cycle) put the active
  scenes ≈10 ms of Core 0 time per emulated frame.

## To reliably hit 90 fps min

The compile-time toggle sweep has reached its limit. The next attacks
need new code:

1. **Lazy flag materialisation** (task #20). SML's main loop runs
   thousands of arithmetic ops per frame, every one of which currently
   recomputes and stores F. Deferring the F-update until a flag
   consumer fires would cut ~5-10 cycles per ALU op.
2. **Direct block linking** in the JIT itself. Emit at block exit:
   `l32r a8, <patched_slot>; jx a8` so the dispatcher's hot loop
   doesn't run between consecutive chained blocks. Avoids the
   enter_block_native windowed bridge entirely.
3. **Move the I²C bus master to Core 0** so the OLED's DMA path
   stops sharing the SRAM-bus arbiter with the JIT.
4. **Switch to an SPI display panel** — the Heltec V3's I²C OLED
   caps at ~100 Hz blit; SPI would 10×+ the bandwidth and remove
   the OLED rate cap.

Reproduce: `python3 benchmark/bench_options_board.py` (board on
`/dev/ttyUSB0`; set `GBJIT_PORT` to override). The harness patches
sdkconfig.defaults + the gbjit / main CMakeLists to enable DEBUG +
INFO logs so the oled_task fps print reaches the UART, and restores
them on exit.
