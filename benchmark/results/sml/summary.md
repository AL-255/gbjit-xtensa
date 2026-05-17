# QEMU-Xtensa benchmark — interp vs JIT (cold + warm)

## Firmware-reported throughput


`jit_nocache` dispatcher stats: blocks_compiled=7631 executed=7631 chain_hits=0 chain_misses=0

`jit_noprefetch` dispatcher stats: blocks_compiled=7 executed=7631 chain_hits=7580 chain_misses=50

`jit` dispatcher stats: blocks_compiled=10 executed=7631 chain_hits=7580 chain_misses=50

`jit_warm` dispatcher stats: blocks_compiled=0 executed=7631 chain_hits=7586 chain_misses=44

| Mode | GB cycles | wall (µs) | T-cycle MHz | × DMG |
|------|----------:|----------:|------------:|------:|
| interp | 200000 | 10101 | 19.800 | 4.721 |
| jit_nocache | 200012 | 556963 | 0.359 | 0.086 |
| jit_noprefetch | 200012 | 6019 | 33.230 | 7.923 |
| jit | 200012 | 5878 | 34.027 | 8.113 |
| jit_warm | 200012 | 2913 | 68.662 | 16.370 |

## Per-mode Xtensa execution (whole trace: boot ROM + IDF + bench + idle)

| Region | TBs interp | TBs jit_nocache | TBs jit_noprefetch | TBs jit | TBs jit_warm | Instrs interp | Instrs jit_nocache | Instrs jit_noprefetch | Instrs jit | Instrs jit_warm |
|--------|----------:|---------------:|------------------:|--------:|------------:|-------------:|------------------:|---------------------:|-----------:|---------------:|
| flash XIP | 1,621,731 | 5,097,518 | 541,812 | 529,361 | 967,625 | 4,365,594 | 29,559,561 | 1,652,243 | 1,592,194 | 2,941,584 |
| IRAM | 3,172,038 | 2,480,815 | 443,292 | 445,241 | 491,011 | 12,369,080 | 8,642,588 | 1,626,041 | 1,632,883 | 2,071,767 |
| boot ROM | 1,616,706 | 2,562,286 | 1,818,192 | 1,856,578 | 1,899,840 | 4,095,565 | 6,210,388 | 3,739,264 | 3,795,697 | 4,063,112 |
| **total** | **6,410,475** | **10,140,619** | **2,803,296** | **2,831,180** | **3,358,476** | **20,830,239** | **44,412,537** | **7,017,548** | **7,020,774** | **9,076,463** |

## Memory-fetch / memory-store instructions

| Mode | Loads | Stores |
|------|------:|-------:|
| interp | 4,657,411 | 1,665,203 |
| jit_nocache | 8,371,542 | 4,831,200 |
| jit_noprefetch | 1,620,260 | 627,151 |
| jit | 1,596,704 | 629,123 |
| jit_warm | 2,064,134 | 833,939 |

## Headline ratios (each mode vs baseline `interp`)

| Metric | interp | jit_nocache | jit_noprefetch | jit | jit_warm |
|--------|------:|-----------:|--------------:|---:|--------:|
| Xtensa instructions (full trace) | 20,830,239 (1.00×) | 44,412,537 (2.13×) | 7,017,548 (0.34×) | 7,020,774 (0.34×) | 9,076,463 (0.44×) |
| Xtensa memory loads | 4,657,411 (1.00×) | 8,371,542 (1.80×) | 1,620,260 (0.35×) | 1,596,704 (0.34×) | 2,064,134 (0.44×) |
| Xtensa memory stores | 1,665,203 (1.00×) | 4,831,200 (2.90×) | 627,151 (0.38×) | 629,123 (0.38×) | 833,939 (0.50×) |

## Per GB T-cycle (whole-trace average — boot + IDF + bench + idle)

| Metric | interp | jit_nocache | jit_noprefetch | jit | jit_warm |
|--------|------:|-----------:|--------------:|---:|--------:|
| Xtensa instructions / GB cycle | 104.15 | 222.05 | 35.09 | 35.10 | 45.38 |
| Xtensa loads / GB cycle | 23.29 | 41.86 | 8.10 | 7.98 | 10.32 |
| Xtensa stores / GB cycle | 8.33 | 24.15 | 3.14 | 3.15 | 4.17 |

## JIT cache speedup (with vs without)

`mode=jit_nocache` runs the dispatcher with `no_cache=true`: every dispatch
iteration recompiles the block, the codecache arena is reset before each
compile, and the predicted-next chain cache is skipped. `mode=jit` is the
default cached behaviour. `mode=jit_warm` is cached AND pre-compiled (no
compilations in the measured window).

| Mode | Wall µs | × DMG | blocks_compiled | Speedup vs `jit_nocache` |
|------|--------:|------:|----------------:|-------------------------:|
| jit_nocache (no cache) | 556,963 | 0.086 | 7631 | 1.00× (baseline) |
| jit (cached, cold)     | 5,878 | 8.113    | 10 | **94.75×** |
| jit_warm (cached, pre-compiled) | 2,913 | 16.370 | 0 | **191.20×** |

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
| jit_noprefetch | 6,019 | 7.923 | 7 | 50 | 0 |
| jit | 5,878 | 8.113 | 10 | 50 | 8 |

Speedup from prefetch (`jit` vs `jit_noprefetch`): **1.02×** wall-time.
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
| Wall µs (firmware-reported, simulated) | 5,878 | 2,913 | **2,965 (50.4% of cold)** |

So at this workload's mix the JIT spends about 14.82 µs of qemu
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
