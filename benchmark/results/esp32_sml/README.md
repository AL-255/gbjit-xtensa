# Super Mario Land on real ESP32-D0WD-V3

Hardware: ESP32-D0WD-V3 v3.1 dev board (CP210x USB serial), 4 MB flash, no PSRAM.
Firmware: `port/esp32/` built with `-DBENCH_ROM=sml`, CPU @ 240 MHz.
Workload: SML's boot/title-screen busy loops, 5 M GB-cycle budget (~1.2 s of
emulated DMG time).

| Mode | Throughput | × DMG | PC at budget exhaustion | Notes |
|------|-----------:|------:|------------------------:|------|
| **interp** | **6.610 MHz** | **1.576×** | $01D4 | Clean run, `[BENCH] done` prints |
| jit (cold) | 2.977 MHz | 0.710× | $FFBC (HRAM-resident code) | Workload completes, post-bench assert |

JIT stats: 33 blocks compiled, 27 193 executions, 26 964 chain hits / 229
misses (99.2% prediction rate), 25 prefetched.

Notable inversion: the interpreter beats the JIT here by 2.2×. The reason
is the LX6's IRAM byte-write trap handler — codecache writes 100s of bytes
byte-by-byte into IRAM during compile, and each unaligned IRAM byte access
costs ~167 CPU cycles through the LoadStoreError exception handler. For 33
blocks at ~300 bytes each that's ~10 000 trapped bytes; at 240 MHz this is
~7 ms of pure handler overhead. The JIT-emitted code runs fast once
compiled, but the boot/polling workload here doesn't amortise the compile
cost enough to overcome it.

On the ESP32-S3 (LX7) this overhead disappears entirely — IRAM is byte-
writable natively, no trap handler involved, and the JIT pulls ahead of
interp as expected.

## Reproduce

```
cd port/esp32
source $IDF_PATH/export.sh
idf.py set-target esp32
idf.py -DBENCH_ROM=sml -DBENCH_MODE_INTERP_ONLY=1 build   # for interp
# or
idf.py -DBENCH_ROM=sml -DBENCH_MODE_JIT_ONLY=1 build      # for jit
idf.py -p /dev/ttyUSB0 flash
```
