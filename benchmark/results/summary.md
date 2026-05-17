# QEMU-Xtensa benchmark — interp vs JIT (per-mode trace)

## Firmware-reported throughput

| Mode   | GB cycles | wall (µs) | T-cycle MHz | × DMG |
|--------|----------:|----------:|------------:|------:|
| interp | 200004 | 6732 | 29.709 | 7.083 |
| jit    | 200020 | 9246 | 21.633 | 5.158 |

JIT dispatcher: blocks_compiled=25 executed=6552 chain_hits=6494 chain_misses=57

## Per-mode Xtensa execution (full trace incl. boot ROM + IDF runtime)

| Region | TBs (interp) | TBs (jit) | Instrs (interp) | Instrs (jit) |
|--------|-------------:|----------:|----------------:|-------------:|
| flash XIP (interp + JIT helpers + IDF) |    1,423,920 |   597,344 |       3,548,513 |    1,707,484 |
| IRAM (JIT-emitted code) |    3,148,164 |   401,724 |      12,305,702 |    1,492,507 |
| boot ROM |    1,523,410 | 1,711,191 |       3,672,696 |    3,349,320 |
| **total** | **6,095,494** | **2,710,259** | **19,526,911** | **6,549,311** |

## Memory-fetch / store instructions

| Region | Loads (interp) | Loads (jit) | Stores (interp) | Stores (jit) |
|--------|---------------:|------------:|----------------:|-------------:|
| flash XIP (interp + JIT helpers + IDF) |        818,643 |     444,316 |         223,315 |      140,870 |
| IRAM (JIT-emitted code) |      2,818,714 |     392,316 |       1,041,177 |      163,034 |
| boot ROM |        741,890 |     707,901 |         265,474 |      287,222 |
| **total** | **4,379,247** | **1,544,533** | **1,529,966** | **591,126** |

## Headline ratios

| Metric | interp | jit | jit / interp |
|--------|------:|----:|-------------:|
| Xtensa instructions (full trace) | 19,526,911 | 6,549,311 | 0.34× |
| Xtensa memory loads | 4,379,247 | 1,544,533 | 0.35× |
| Xtensa memory stores | 1,529,966 | 591,126 | 0.39× |

## Per GB T-cycle (whole-trace average — boot + IDF + bench)

| Metric | interp | jit |
|--------|------:|----:|
| Xtensa instructions / GB cycle | 97.63 | 32.74 |
| Xtensa memory loads / GB cycle | 21.90 | 7.72 |
| Xtensa memory stores / GB cycle | 7.65 | 2.96 |

## Approximate boot/IDF floor and bench-only work

Boot-ROM floor (shared between modes): ~3,349,320 Xtensa instructions.

| Metric | interp | jit |
|--------|------:|----:|
| Xtensa instructions above boot floor | 16,177,591 | 3,199,991 |
| → per GB cycle | 80.89 | 16.00 |

## Notes

- Each mode is run in its own qemu boot. The trace covers boot ROM,
  IDF init, the benchmark itself, and the post-bench idle suspend
  (`vTaskDelay(portMAX_DELAY)`) until qemu hits its 60-second wall
  timeout. The idle-task `WAITI` keeps the trace growth small after
  the bench completes, but FreeRTOS scheduler ticks still emit some.
- IRAM activity in interp mode is FreeRTOS / IDF code that the
  linker placed in the internal-SRAM-mapped IRAM region; the JIT's
  arena is not allocated in interp-only builds, so the interp mode
  is never executing JIT-emitted Xtensa from there.
- QEMU has no Xtensa cache model: each load is a single emulated
  cycle. On real ESP32-S3 silicon, instruction fetches from the
  flash-XIP region cost 1 cycle on icache hit and 10–40 cycles on
  miss. The flash-XIP load counts above are therefore an upper
  bound on the wait-cycle penalty real hardware would pay; multiply
  by an expected miss rate (1–5 % typical for well-cached code) to
  estimate actual stall budget.
- Firmware-reported throughput (`elapsed_us` from `esp_timer`) does
  NOT track Xtensa instructions one-to-one in qemu — qemu advances
  the simulated system timer based partly on wall clock, so simpler
  Xtensa instructions emulate faster per unit simulated-time. The
  JIT's lower instruction count is the relevant figure for real-
  hardware performance.
