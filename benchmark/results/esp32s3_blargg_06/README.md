# Blargg 06-ld_r_r on real ESP32-S3

Hardware: ESP32-S3 (QFN56) rev v0.2, 8 MB embedded flash, CP210x USB-UART
bridge. CPU @ 240 MHz.
Firmware: `port/esp32s3/` built with `-DBENCH_ROM=blargg_06`.
Workload: tight `LD r,r` correctness loop, 5 M GB-cycle budget.

| Mode | Throughput | × DMG | PC at budget exhaustion |
|------|-----------:|------:|------------------------:|
| interp | 9.125 MHz | 2.176× | $CC5F |
| jit (cached) | 3.587 MHz | 0.855× | $CC5F |

Dispatcher stats (jit): 98 blocks, 83 544 executions, 80 335 chain
hits / 3 209 misses (96.2 % prediction), 66 prefetched.

## Why the JIT loses here

This is the opposite story from [SML](../esp32s3_sml/README.md): on a
tight, all-CPU loop the per-block dispatcher overhead exceeds what the
per-op interp loop pays in switch-table dispatch. With ~850 executions
per unique block and ~100 ops compiled per block on average, the
break-even point for the JIT lies past where this workload spends its
time. SML wins because its hot paths include IO/memory reads through
the JIT-inlined fast path and a large amount of code reuse via the
chain-hit predictor; Blargg 06 has neither.

Worth investigating but not chasing for now — the JIT's purpose on
this project is to run *real games*, not Blargg's CPU test harness.

## Reproduce

```sh
cd port/esp32s3
source $IDF_PATH/export.sh
idf.py set-target esp32s3
rm -rf build
idf.py -DBENCH_ROM=blargg_06 -DCMAKE_C_FLAGS="-DBENCH_MODE_INTERP_ONLY=1" build
idf.py -p /dev/ttyUSB0 flash
# or
idf.py -DBENCH_ROM=blargg_06 -DCMAKE_C_FLAGS="-DBENCH_MODE_JIT_ONLY=1" build
```
