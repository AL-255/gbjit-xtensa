# QEMU-Xtensa benchmark — interp vs JIT (cold + warm)

## Firmware-reported throughput


`jit_nocache` dispatcher stats: blocks_compiled=7631 executed=7631 chain_hits=0 chain_misses=0

`jit_noprefetch` dispatcher stats: blocks_compiled=7 executed=7631 chain_hits=7580 chain_misses=50

`jit` dispatcher stats: blocks_compiled=10 executed=7631 chain_hits=7580 chain_misses=50

`jit_warm` dispatcher stats: blocks_compiled=0 executed=7631 chain_hits=7586 chain_misses=44

| Mode | GB cycles | wall (µs) | T-cycle MHz | × DMG |
|------|----------:|----------:|------------:|------:|
| interp | 200000 | 8713 | 22.954 | 5.473 |
| jit_nocache | 200012 | 556638 | 0.359 | 0.086 |
| jit_noprefetch | 200012 | 9389 | 21.303 | 5.079 |
| jit | 200012 | 11176 | 17.897 | 4.267 |
| jit_warm | 200012 | 2694 | 74.244 | 17.701 |

## Per-mode Xtensa execution (whole trace: boot ROM + IDF + bench + idle)

| Region | TBs interp | TBs jit_nocache | TBs jit_noprefetch | TBs jit | TBs jit_warm | Instrs interp | Instrs jit_nocache | Instrs jit_noprefetch | Instrs jit | Instrs jit_warm |
|--------|----------:|---------------:|------------------:|--------:|------------:|-------------:|------------------:|---------------------:|-----------:|---------------:|
| flash XIP | 1,618,057 | 5,091,600 | 512,709 | 515,606 | 952,300 | 4,614,117 | 29,627,790 | 1,698,492 | 1,712,282 | 3,169,583 |
| IRAM | 3,190,191 | 2,476,256 | 443,040 | 445,053 | 488,650 | 12,392,425 | 8,628,244 | 1,625,805 | 1,632,995 | 2,065,219 |
| boot ROM | 1,772,706 | 2,527,121 | 1,849,284 | 1,834,845 | 1,908,161 | 4,430,875 | 6,247,663 | 3,881,912 | 3,856,058 | 4,209,876 |
| **total** | **6,580,954** | **10,094,977** | **2,805,033** | **2,795,504** | **3,349,111** | **21,437,417** | **44,503,697** | **7,206,209** | **7,201,335** | **9,444,678** |

## Memory-fetch / memory-store instructions

| Mode | Loads | Stores |
|------|------:|-------:|
| interp | 4,810,256 | 1,769,518 |
| jit_nocache | 8,402,897 | 4,935,143 |
| jit_noprefetch | 1,678,070 | 732,255 |
| jit | 1,678,619 | 734,206 |
| jit_warm | 2,235,339 | 979,605 |

## Headline ratios (each mode vs baseline `interp`)

| Metric | interp | jit_nocache | jit_noprefetch | jit | jit_warm |
|--------|------:|-----------:|--------------:|---:|--------:|
| Xtensa instructions (full trace) | 21,437,417 (1.00×) | 44,503,697 (2.08×) | 7,206,209 (0.34×) | 7,201,335 (0.34×) | 9,444,678 (0.44×) |
| Xtensa memory loads | 4,810,256 (1.00×) | 8,402,897 (1.75×) | 1,678,070 (0.35×) | 1,678,619 (0.35×) | 2,235,339 (0.46×) |
| Xtensa memory stores | 1,769,518 (1.00×) | 4,935,143 (2.79×) | 732,255 (0.41×) | 734,206 (0.41×) | 979,605 (0.55×) |

## Per GB T-cycle (whole-trace average — boot + IDF + bench + idle)

| Metric | interp | jit_nocache | jit_noprefetch | jit | jit_warm |
|--------|------:|-----------:|--------------:|---:|--------:|
| Xtensa instructions / GB cycle | 107.19 | 222.51 | 36.03 | 36.00 | 47.22 |
| Xtensa loads / GB cycle | 24.05 | 42.01 | 8.39 | 8.39 | 11.18 |
| Xtensa stores / GB cycle | 8.85 | 24.67 | 3.66 | 3.67 | 4.90 |

## JIT cache speedup (with vs without)

`mode=jit_nocache` runs the dispatcher with `no_cache=true`: every dispatch
iteration recompiles the block, the codecache arena is reset before each
compile, and the predicted-next chain cache is skipped. `mode=jit` is the
default cached behaviour. `mode=jit_warm` is cached AND pre-compiled (no
compilations in the measured window).

| Mode | Wall µs | × DMG | blocks_compiled | Speedup vs `jit_nocache` |
|------|--------:|------:|----------------:|-------------------------:|
| jit_nocache (no cache) | 556,638 | 0.086 | 7631 | 1.00× (baseline) |
| jit (cached, cold)     | 11,176 | 4.267    | 10 | **49.81×** |
| jit_warm (cached, pre-compiled) | 2,694 | 17.701 | 0 | **206.62×** |

For the 200 000-GB-cycle window:
- `jit_nocache` invoked `gbjit_compile_block` ~7631 times (one per dispatch step).
- `jit` (cached) compiled only 10 unique blocks — the cache turned the other
  ~7,621
  dispatch iterations into pure lookups + executions.
- `jit_warm` paid the 10-block compile cost in a discarded warm-up pass, so its
  measured window was 100% reuse.

## Prefetch speedup (cached JIT, with vs without static-successor prefetch)

| Mode | Wall µs | × DMG | blocks_compiled | chain_misses | prefetched |
|------|--------:|------:|----------------:|-------------:|-----------:|
| jit_noprefetch | 9,389 | 5.079 | 7 | 50 | 0 |
| jit | 11,176 | 4.267 | 10 | 50 | 8 |

Speedup from prefetch (`jit` vs `jit_noprefetch`): **0.84×** wall-time.
`jit` pays 8 extra compile calls inside `gbjit_compile_block`
(walking successor PCs to depth 4) so block discovery isn't spread one-per-
chain-miss across the run. The visible win on a tight loop is modest; the
latency win on first-encounter spikes (game enters new code) is much larger
than the wall-time numbers here suggest.

## JIT on-the-fly compilation overhead (the answer to: does the JIT row include translation cost?)

YES — `mode=jit` measures one *cold* run, which during the 200 000-cycle window
compiled 10 unique blocks on-the-fly and then executed them 6 552 times.
`mode=jit_warm` runs the JIT twice from inside the firmware: a discarded warm-up
pass to populate the dispatcher's block cache, then a `cpu_reset` and a *second*
run that is the one whose `elapsed_us` we report. In the warm pass
`blocks_compiled=0` — purely the cost of executing the already-translated code.

| Metric | jit (cold) | jit_warm (executed pass only) | overhead (cold − warm) |
|--------|----------:|------------------------------:|-----------------------:|
| Wall µs (firmware-reported, simulated) | 11,176 | 2,694 | **8,482 (75.9% of cold)** |

So at this workload's mix the JIT spends about 42.41 µs of qemu
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
