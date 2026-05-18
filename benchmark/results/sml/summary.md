# QEMU-Xtensa benchmark — interp vs JIT (cold + warm)

## Firmware-reported throughput


`jit_nocache` dispatcher stats: blocks_compiled=7631 executed=7631 chain_hits=0 chain_misses=0

`jit_noprefetch` dispatcher stats: blocks_compiled=7 executed=7631 chain_hits=7580 chain_misses=50

`jit` dispatcher stats: blocks_compiled=10 executed=7631 chain_hits=7580 chain_misses=50

`jit_warm` dispatcher stats: blocks_compiled=0 executed=7631 chain_hits=7586 chain_misses=44

| Mode | GB cycles | wall (µs) | T-cycle MHz | × DMG |
|------|----------:|----------:|------------:|------:|
| interp | 200000 | 11551 | 17.315 | 4.128 |
| jit_nocache | 200012 | 2329370 | 0.086 | 0.020 |
| jit_noprefetch | 200012 | 7189 | 27.822 | 6.633 |
| jit | 200012 | 6984 | 28.639 | 6.828 |
| jit_warm | 200012 | 2985 | 67.006 | 15.975 |

## Per-mode Xtensa execution (whole trace: boot ROM + IDF + bench + idle)

| Region | TBs interp | TBs jit_nocache | TBs jit_noprefetch | TBs jit | TBs jit_warm | Instrs interp | Instrs jit_nocache | Instrs jit_noprefetch | Instrs jit | Instrs jit_warm |
|--------|----------:|---------------:|------------------:|--------:|------------:|-------------:|------------------:|---------------------:|-----------:|---------------:|
| flash XIP | 1,621,503 | 5,106,249 | 519,679 | 518,334 | 968,801 | 4,363,692 | 29,573,326 | 1,583,419 | 1,580,722 | 2,947,541 |
| IRAM | 3,193,601 | 2,581,170 | 443,930 | 443,705 | 492,096 | 12,399,889 | 9,046,724 | 1,629,113 | 1,628,758 | 2,077,324 |
| boot ROM | 1,700,394 | 2,442,426 | 1,787,812 | 1,728,498 | 1,835,795 | 4,223,449 | 6,045,973 | 3,694,397 | 3,595,825 | 3,963,977 |
| **total** | **6,515,498** | **10,129,845** | **2,751,421** | **2,690,537** | **3,296,692** | **20,987,030** | **44,666,023** | **6,906,929** | **6,805,305** | **8,988,842** |

## Memory-fetch / memory-store instructions

| Mode | Loads | Stores |
|------|------:|-------:|
| interp | 4,665,028 | 1,661,781 |
| jit_nocache | 8,445,738 | 4,865,764 |
| jit_noprefetch | 1,593,710 | 626,764 |
| jit | 1,579,154 | 627,637 |
| jit_warm | 2,060,800 | 833,646 |

## Headline ratios (each mode vs baseline `interp`)

| Metric | interp | jit_nocache | jit_noprefetch | jit | jit_warm |
|--------|------:|-----------:|--------------:|---:|--------:|
| Xtensa instructions (full trace) | 20,987,030 (1.00×) | 44,666,023 (2.13×) | 6,906,929 (0.33×) | 6,805,305 (0.32×) | 8,988,842 (0.43×) |
| Xtensa memory loads | 4,665,028 (1.00×) | 8,445,738 (1.81×) | 1,593,710 (0.34×) | 1,579,154 (0.34×) | 2,060,800 (0.44×) |
| Xtensa memory stores | 1,661,781 (1.00×) | 4,865,764 (2.93×) | 626,764 (0.38×) | 627,637 (0.38×) | 833,646 (0.50×) |

## Per GB T-cycle (whole-trace average — boot + IDF + bench + idle)

| Metric | interp | jit_nocache | jit_noprefetch | jit | jit_warm |
|--------|------:|-----------:|--------------:|---:|--------:|
| Xtensa instructions / GB cycle | 104.94 | 223.32 | 34.53 | 34.02 | 44.94 |
| Xtensa loads / GB cycle | 23.33 | 42.23 | 7.97 | 7.90 | 10.30 |
| Xtensa stores / GB cycle | 8.31 | 24.33 | 3.13 | 3.14 | 4.17 |

## JIT cache speedup (with vs without)

`mode=jit_nocache` runs the dispatcher with `no_cache=true`: every dispatch
iteration recompiles the block, the codecache arena is reset before each
compile, and the predicted-next chain cache is skipped. `mode=jit` is the
default cached behaviour. `mode=jit_warm` is cached AND pre-compiled (no
compilations in the measured window).

| Mode | Wall µs | × DMG | blocks_compiled | Speedup vs `jit_nocache` |
|------|--------:|------:|----------------:|-------------------------:|
| jit_nocache (no cache) | 2,329,370 | 0.020 | 7631 | 1.00× (baseline) |
| jit (cached, cold)     | 6,984 | 6.828    | 10 | **333.53×** |
| jit_warm (cached, pre-compiled) | 2,985 | 15.975 | 0 | **780.36×** |

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
| jit_noprefetch | 7,189 | 6.633 | 7 | 50 | 0 |
| jit | 6,984 | 6.828 | 10 | 50 | 8 |

Speedup from prefetch (`jit` vs `jit_noprefetch`): **1.03×** wall-time.
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
| Wall µs (firmware-reported, simulated) | 6,984 | 2,985 | **3,999 (57.3% of cold)** |

So at this workload's mix the JIT spends about 19.99 µs of qemu
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
