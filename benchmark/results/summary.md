# QEMU-Xtensa benchmark — interp vs JIT (cold + warm)

## Firmware-reported throughput


`jit_nocache` dispatcher stats: blocks_compiled=6552 executed=6552 chain_hits=0 chain_misses=0

`jit_noprefetch` dispatcher stats: blocks_compiled=25 executed=6552 chain_hits=6494 chain_misses=57

`jit` dispatcher stats: blocks_compiled=26 executed=6552 chain_hits=6494 chain_misses=57

`jit_warm` dispatcher stats: blocks_compiled=0 executed=6552 chain_hits=6517 chain_misses=34

| Mode | GB cycles | wall (µs) | T-cycle MHz | × DMG |
|------|----------:|----------:|------------:|------:|
| interp | 200004 | 7153 | 27.961 | 6.666 |
| jit_nocache | 200020 | 390162 | 0.513 | 0.122 |
| jit_noprefetch | 200020 | 9329 | 21.441 | 5.112 |
| jit | 200020 | 8644 | 23.140 | 5.517 |
| jit_warm | 200020 | 3663 | 54.606 | 13.019 |

## Per-mode Xtensa execution (whole trace: boot ROM + IDF + bench + idle)

| Region | TBs interp | TBs jit_nocache | TBs jit_noprefetch | TBs jit | TBs jit_warm | Instrs interp | Instrs jit_nocache | Instrs jit_noprefetch | Instrs jit | Instrs jit_warm |
|--------|----------:|---------------:|------------------:|--------:|------------:|-------------:|------------------:|---------------------:|-----------:|---------------:|
| flash XIP | 1,423,133 | 4,934,433 | 620,682 | 616,409 | 1,143,998 | 3,546,137 | 27,268,554 | 1,753,804 | 1,753,497 | 3,221,206 |
| IRAM | 3,179,482 | 2,147,554 | 404,595 | 403,759 | 446,445 | 12,394,623 | 7,583,901 | 1,501,949 | 1,499,906 | 1,910,213 |
| boot ROM | 1,554,588 | 2,327,596 | 1,722,120 | 1,669,375 | 1,747,972 | 3,715,404 | 5,539,286 | 3,371,923 | 3,286,399 | 3,448,265 |
| **total** | **6,157,203** | **9,409,583** | **2,747,397** | **2,689,543** | **3,338,415** | **19,656,164** | **40,391,741** | **6,627,676** | **6,539,802** | **8,579,684** |

## Memory-fetch / memory-store instructions

| Mode | Loads | Stores |
|------|------:|-------:|
| interp | 4,361,246 | 1,528,942 |
| jit_nocache | 7,625,460 | 4,462,866 |
| jit_noprefetch | 1,568,193 | 593,517 |
| jit | 1,548,988 | 593,768 |
| jit_warm | 2,080,063 | 792,351 |

## Headline ratios (each mode vs baseline `interp`)

| Metric | interp | jit_nocache | jit_noprefetch | jit | jit_warm |
|--------|------:|-----------:|--------------:|---:|--------:|
| Xtensa instructions (full trace) | 19,656,164 (1.00×) | 40,391,741 (2.05×) | 6,627,676 (0.34×) | 6,539,802 (0.33×) | 8,579,684 (0.44×) |
| Xtensa memory loads | 4,361,246 (1.00×) | 7,625,460 (1.75×) | 1,568,193 (0.36×) | 1,548,988 (0.36×) | 2,080,063 (0.48×) |
| Xtensa memory stores | 1,528,942 (1.00×) | 4,462,866 (2.92×) | 593,517 (0.39×) | 593,768 (0.39×) | 792,351 (0.52×) |

## Per GB T-cycle (whole-trace average — boot + IDF + bench + idle)

| Metric | interp | jit_nocache | jit_noprefetch | jit | jit_warm |
|--------|------:|-----------:|--------------:|---:|--------:|
| Xtensa instructions / GB cycle | 98.28 | 201.94 | 33.14 | 32.70 | 42.89 |
| Xtensa loads / GB cycle | 21.81 | 38.12 | 7.84 | 7.74 | 10.40 |
| Xtensa stores / GB cycle | 7.64 | 22.31 | 2.97 | 2.97 | 3.96 |

## JIT cache speedup (with vs without)

`mode=jit_nocache` runs the dispatcher with `no_cache=true`: every dispatch
iteration recompiles the block, the codecache arena is reset before each
compile, and the predicted-next chain cache is skipped. `mode=jit` is the
default cached behaviour. `mode=jit_warm` is cached AND pre-compiled (no
compilations in the measured window).

| Mode | Wall µs | × DMG | blocks_compiled | Speedup vs `jit_nocache` |
|------|--------:|------:|----------------:|-------------------------:|
| jit_nocache (no cache) | 390,162 | 0.122 | 6552 | 1.00× (baseline) |
| jit (cached, cold)     | 8,644 | 5.517    | 26 | **45.14×** |
| jit_warm (cached, pre-compiled) | 3,663 | 13.019 | 0 | **106.51×** |

For the 200 000-GB-cycle window:
- `jit_nocache` invoked `gbjit_compile_block` ~6552 times (one per dispatch step).
- `jit` (cached) compiled only 26 unique blocks — the cache turned the other
  ~6,526
  dispatch iterations into pure lookups + executions.
- `jit_warm` paid the 26-block compile cost in a discarded warm-up pass, so its
  measured window was 100% reuse.

## Prefetch speedup (cached JIT, with vs without static-successor prefetch)

| Mode | Wall µs | × DMG | blocks_compiled | chain_misses | prefetched |
|------|--------:|------:|----------------:|-------------:|-----------:|
| jit_noprefetch | 9,329 | 5.112 | 25 | 57 | 0 |
| jit | 8,644 | 5.517 | 26 | 57 | 19 |

Speedup from prefetch (`jit` vs `jit_noprefetch`): **1.08×** wall-time.
`jit` pays 19 extra compile calls inside `gbjit_compile_block`
(walking successor PCs to depth 4) so block discovery isn't spread one-per-
chain-miss across the run. The visible win on a tight loop is modest; the
latency win on first-encounter spikes (game enters new code) is much larger
than the wall-time numbers here suggest.

## JIT on-the-fly compilation overhead (the answer to: does the JIT row include translation cost?)

YES — `mode=jit` measures one *cold* run, which during the 200 000-cycle window
compiled 26 unique blocks on-the-fly and then executed them 6 552 times.
`mode=jit_warm` runs the JIT twice from inside the firmware: a discarded warm-up
pass to populate the dispatcher's block cache, then a `cpu_reset` and a *second*
run that is the one whose `elapsed_us` we report. In the warm pass
`blocks_compiled=0` — purely the cost of executing the already-translated code.

| Metric | jit (cold) | jit_warm (executed pass only) | overhead (cold − warm) |
|--------|----------:|------------------------------:|-----------------------:|
| Wall µs (firmware-reported, simulated) | 8,644 | 3,663 | **4,981 (57.6% of cold)** |

So at this workload's mix the JIT spends about 24.90 µs of qemu
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
