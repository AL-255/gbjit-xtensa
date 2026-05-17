# QEMU-Xtensa benchmark — interp vs JIT (cold + warm)

## Firmware-reported throughput


`jit` dispatcher stats: blocks_compiled=25 executed=6552 chain_hits=6494 chain_misses=57

`jit_warm` dispatcher stats: blocks_compiled=0 executed=6552 chain_hits=6517 chain_misses=34

| Mode | GB cycles | wall (µs) | T-cycle MHz | × DMG |
|------|----------:|----------:|------------:|------:|
| interp | 200004 | 6791 | 29.451 | 7.022 |
| jit | 200020 | 8660 | 23.097 | 5.507 |
| jit_warm | 200020 | 3558 | 56.217 | 13.403 |

## Per-mode Xtensa execution (whole trace: boot ROM + IDF + bench + idle)

| Region | TBs interp | TBs jit | TBs jit_warm | Instrs interp | Instrs jit | Instrs jit_warm |
|--------|----------:|--------:|------------:|-------------:|-----------:|---------------:|
| flash XIP | 1,421,227 | 591,462 | 1,105,386 | 3,538,232 | 1,714,782 | 3,158,117 |
| IRAM | 3,133,543 | 401,324 | 443,386 | 12,219,034 | 1,490,793 | 1,899,104 |
| boot ROM | 1,463,948 | 1,751,937 | 1,687,834 | 3,576,536 | 3,408,507 | 3,347,720 |
| **total** | **6,018,718** | **2,744,723** | **3,236,606** | **19,333,802** | **6,614,082** | **8,404,941** |

## Memory-fetch / memory-store instructions

| Mode | Loads | Stores |
|------|------:|-------:|
| interp | 4,343,090 | 1,522,673 |
| jit | 1,534,902 | 591,486 |
| jit_warm | 2,025,041 | 789,776 |

## Headline ratios (each mode vs baseline `interp`)

| Metric | interp | jit | jit_warm |
|--------|------:|---:|--------:|
| Xtensa instructions (full trace) | 19,333,802 (1.00×) | 6,614,082 (0.34×) | 8,404,941 (0.43×) |
| Xtensa memory loads | 4,343,090 (1.00×) | 1,534,902 (0.35×) | 2,025,041 (0.47×) |
| Xtensa memory stores | 1,522,673 (1.00×) | 591,486 (0.39×) | 789,776 (0.52×) |

## Per GB T-cycle (whole-trace average — boot + IDF + bench + idle)

| Metric | interp | jit | jit_warm |
|--------|------:|---:|--------:|
| Xtensa instructions / GB cycle | 96.67 | 33.07 | 42.02 |
| Xtensa loads / GB cycle | 21.72 | 7.67 | 10.12 |
| Xtensa stores / GB cycle | 7.61 | 2.96 | 3.95 |

## JIT on-the-fly compilation overhead (the answer to: does the JIT row include translation cost?)

YES — `mode=jit` measures one *cold* run, which during the 200 000-cycle window
compiled 25 unique blocks on-the-fly and then executed them 6 552 times.
`mode=jit_warm` runs the JIT twice from inside the firmware: a discarded warm-up
pass to populate the dispatcher's block cache, then a `cpu_reset` and a *second*
run that is the one whose `elapsed_us` we report. In the warm pass
`blocks_compiled=0` — purely the cost of executing the already-translated code.

| Metric | jit (cold) | jit_warm (executed pass only) | overhead (cold − warm) |
|--------|----------:|------------------------------:|-----------------------:|
| Wall µs (firmware-reported, simulated) | 8,660 | 3,558 | **5,102 (58.9% of cold)** |

So at this workload's mix the JIT spends about 25.51 µs of qemu
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
