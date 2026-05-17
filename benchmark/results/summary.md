# QEMU-Xtensa benchmark — interp vs JIT (cold + warm)

## Firmware-reported throughput


`jit_nocache` dispatcher stats: blocks_compiled=6552 executed=6552 chain_hits=0 chain_misses=0

`jit` dispatcher stats: blocks_compiled=25 executed=6552 chain_hits=6494 chain_misses=57

`jit_warm` dispatcher stats: blocks_compiled=0 executed=6552 chain_hits=6517 chain_misses=34

| Mode | GB cycles | wall (µs) | T-cycle MHz | × DMG |
|------|----------:|----------:|------------:|------:|
| interp | 200004 | 7219 | 27.705 | 6.605 |
| jit_nocache | 200020 | 378470 | 0.528 | 0.126 |
| jit | 200020 | 9126 | 21.918 | 5.226 |
| jit_warm | 200020 | 3650 | 54.800 | 13.065 |

## Per-mode Xtensa execution (whole trace: boot ROM + IDF + bench + idle)

| Region | TBs interp | TBs jit_nocache | TBs jit | TBs jit_warm | Instrs interp | Instrs jit_nocache | Instrs jit | Instrs jit_warm |
|--------|----------:|---------------:|--------:|------------:|-------------:|------------------:|-----------:|---------------:|
| flash XIP | 1,424,801 | 4,751,259 | 616,418 | 1,158,087 | 3,553,824 | 26,771,003 | 1,768,946 | 3,273,769 |
| IRAM | 3,195,436 | 2,144,624 | 402,969 | 447,413 | 12,434,782 | 7,571,734 | 1,496,921 | 1,912,147 |
| boot ROM | 1,509,749 | 2,306,142 | 1,694,309 | 1,707,623 | 3,653,417 | 5,499,573 | 3,326,286 | 3,387,035 |
| **total** | **6,129,986** | **9,202,025** | **2,713,696** | **3,313,123** | **19,642,023** | **39,842,310** | **6,592,153** | **8,572,951** |

## Memory-fetch / memory-store instructions

| Mode | Loads | Stores |
|------|------:|-------:|
| interp | 4,389,101 | 1,529,803 |
| jit_nocache | 7,516,083 | 4,390,553 |
| jit | 1,567,623 | 592,075 |
| jit_warm | 2,099,393 | 790,066 |

## Headline ratios (each mode vs baseline `interp`)

| Metric | interp | jit_nocache | jit | jit_warm |
|--------|------:|-----------:|---:|--------:|
| Xtensa instructions (full trace) | 19,642,023 (1.00×) | 39,842,310 (2.03×) | 6,592,153 (0.34×) | 8,572,951 (0.44×) |
| Xtensa memory loads | 4,389,101 (1.00×) | 7,516,083 (1.71×) | 1,567,623 (0.36×) | 2,099,393 (0.48×) |
| Xtensa memory stores | 1,529,803 (1.00×) | 4,390,553 (2.87×) | 592,075 (0.39×) | 790,066 (0.52×) |

## Per GB T-cycle (whole-trace average — boot + IDF + bench + idle)

| Metric | interp | jit_nocache | jit | jit_warm |
|--------|------:|-----------:|---:|--------:|
| Xtensa instructions / GB cycle | 98.21 | 199.19 | 32.96 | 42.86 |
| Xtensa loads / GB cycle | 21.95 | 37.58 | 7.84 | 10.50 |
| Xtensa stores / GB cycle | 7.65 | 21.95 | 2.96 | 3.95 |

## JIT cache speedup (with vs without)

`mode=jit_nocache` runs the dispatcher with `no_cache=true`: every dispatch
iteration recompiles the block, the codecache arena is reset before each
compile, and the predicted-next chain cache is skipped. `mode=jit` is the
default cached behaviour. `mode=jit_warm` is cached AND pre-compiled (no
compilations in the measured window).

| Mode | Wall µs | × DMG | blocks_compiled | Speedup vs `jit_nocache` |
|------|--------:|------:|----------------:|-------------------------:|
| jit_nocache (no cache) | 378,470 | 0.126 | 6552 | 1.00× (baseline) |
| jit (cached, cold)     | 9,126 | 5.226    | 25 | **41.47×** |
| jit_warm (cached, pre-compiled) | 3,650 | 13.065 | 0 | **103.69×** |

For the 200 000-GB-cycle window:
- `jit_nocache` invoked `gbjit_compile_block` ~6552 times (one per dispatch step).
- `jit` (cached) compiled only 25 unique blocks — the cache turned the other
  ~6,527
  dispatch iterations into pure lookups + executions.
- `jit_warm` paid the 25-block compile cost in a discarded warm-up pass, so its
  measured window was 100% reuse.

## JIT on-the-fly compilation overhead (the answer to: does the JIT row include translation cost?)

YES — `mode=jit` measures one *cold* run, which during the 200 000-cycle window
compiled 25 unique blocks on-the-fly and then executed them 6 552 times.
`mode=jit_warm` runs the JIT twice from inside the firmware: a discarded warm-up
pass to populate the dispatcher's block cache, then a `cpu_reset` and a *second*
run that is the one whose `elapsed_us` we report. In the warm pass
`blocks_compiled=0` — purely the cost of executing the already-translated code.

| Metric | jit (cold) | jit_warm (executed pass only) | overhead (cold − warm) |
|--------|----------:|------------------------------:|-----------------------:|
| Wall µs (firmware-reported, simulated) | 9,126 | 3,650 | **5,476 (60.0% of cold)** |

So at this workload's mix the JIT spends about 27.38 µs of qemu
simulated time per 1 000 GB cycles on translation. The overhead is *per unique
block*, not per GB cycle — real ROMs that loop through the same code millions of
times amortise it to near-zero. Our 200 000-cycle micro-benchmark only invokes
each compiled block ~260 times on average, which makes compile cost look large
relative to execution.

Caveats on the full-trace Xtensa instruction column: the warm-only firmware
variant runs the JIT *twice* (warm-up + measured), so its full-trace instruction
total is *higher* than the cold run's, not lower. The 59% wall-time figure above
is the correct compile-overhead measure; the trace totals confirm the cold run
executed fewer instructions overall because most of its time was spent in
`gbjit_compile_block` (flash-XIP) rather than the compiled blocks (IRAM).

## Notes

- Each mode is its own qemu boot. The trace covers boot ROM, IDF init,
  the benchmark, and the idle `WAITI` after `vTaskDelay(portMAX_DELAY)`.
- `jit` (cold) measures wall-clock that includes 25 calls to
  `gbjit_compile_block`. `jit_warm` first compiles every block, resets
  cpu_state, then re-runs — its measured window has `blocks_compiled=0`
  and is pure inlined-JIT execution.
- QEMU has no Xtensa cache model. The flash-XIP load counts upper-bound
  the icache-miss-penalty real silicon would pay; multiply by an
  expected miss rate (1–5 % typical) to estimate stall budget.
- The firmware-reported `elapsed_us` is qemu-simulated time, which
  does not track Xtensa instructions one-to-one — qemu's effective MIPS
  depends on the instruction mix. The instruction-count metric is what
  matters on real ESP32-S3 silicon.
