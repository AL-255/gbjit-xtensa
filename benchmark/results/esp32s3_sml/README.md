# Super Mario Land on real ESP32-S3

Hardware: ESP32-S3 (QFN56) rev v0.2, 8 MB embedded flash, no PSRAM,
CP210x USB-UART bridge. CPU @ 240 MHz.
Firmware: `port/esp32s3/` built with `-DBENCH_ROM=sml`.
Workload: SML's boot through to the start of the main game loop
(`PC=$01D4`), 5 M GB-cycle budget (~1.2 s of emulated DMG time).

| Mode | Throughput | × DMG | PC at budget exhaustion | Notes |
|------|-----------:|------:|------------------------:|------|
| interp | 8.864 MHz | 2.113× | $01D4 | reference run |
| jit_nocache | 0.391 MHz | 0.093× | $01D4 | every block recompiled (cache-effect lower bound) |
| **jit (cached, cold)** | **14.480 MHz** | **3.452×** | $01D4 | one-shot compile per block, then chain hits |
| jit_warm (cached, pre-compiled) | 14.702 MHz | 3.505× | $01D4 | warm-up pass discarded, measured pass has 0 compiles |

Dispatcher stats (cold `jit`): 55 unique blocks, 205 640 executions,
203 976 chain hits / 1 635 misses (99.2 % prediction rate), 37
prefetched.

## What changed

A prior bake of this firmware reported `jit: 3.644 MHz / 0.869× DMG,
pc=$FFBC, halted=0` — i.e. the dispatcher kept handing control back to
the same HRAM-resident block forever. Three correctness bugs were
keeping SML stuck spinning in the OAM-DMA wait routine instead of
reaching the real game loop; once fixed the JIT pulls ahead of the
interpreter as expected.

1. **OAM DMA was a no-op.** `mmu_write8` for `$FF46` stored the byte
   to `io[$46]` and returned without copying `$XX00..$XX9F` into OAM.
   Now does the 160-byte copy synchronously via `mmu_read8`; the
   caller's 40-iter wait loop still covers the GB-cycle timing.

2. **MBC1 bank-switch invalidation.** The JIT cached blocks compiled
   from `$4000..$7FFF` with bank-N's immediates and absolute branch
   targets baked in. When MBC1 swapped to bank M the cached blocks
   silently executed bank-N code — SML's main loop diverged at
   `PC=$7FF3` ~668 k cycles in, with a `JP a16` reading the wrong
   target ($8013 in VRAM instead of $6A49 in ROM). The MMU now sets a
   `rom_bank_dirty` flag on every bank-control write; the dispatcher
   invalidates SMC pages `0x40..0x7F` before the next block enter
   when the flag is set.

3. **Block break after PPU/OAM IO writes.** Inlined `LDH A,($FF44)`
   reads `io[$44]` directly without ticking the PPU. When LCDC was
   written earlier in the same block (via the helper, which *does*
   tick PPU before the write), the subsequent inlined LY read got
   stale data. The walker now terminates the block after `LDH (n),A`
   to `$FF40` / `$FF41` / `$FF45` / `$FF46`, so the dispatcher's
   per-block `sm83_service_interrupts` surfaces the new IO state to
   the next inlined read.

Also: the DEC-A;JR NZ,-3 closed-form fast path collapses the
40-iteration OAM-DMA wait body into one block call; generic back-edge
inlining is hard-disabled (it caused a real-S3 regression of its own).

## Comparison with plain ESP32 (LX6)

On the plain ESP32 D0WD-V3 the JIT loses to the interp because IRAM
byte writes go through a CPU trap handler — see
[`../esp32_sml/README.md`](../esp32_sml/README.md). The S3's LX7 has
native byte-writable IRAM, no trap, and that gap closes:

| Board | Interp | JIT (cached) | Speed-up |
|-------|-------:|-------------:|---------:|
| ESP32 D0WD-V3 (LX6, 240 MHz) | 1.58× DMG | 0.71× DMG | **0.45×** |
| ESP32-S3 (LX7, 240 MHz) | 2.11× DMG | 3.45× DMG | **1.63×** |

## Reproduce

```sh
cd port/esp32s3
source $IDF_PATH/export.sh
idf.py set-target esp32s3

# clean, then build for the mode of interest (cache reuse across
# CMAKE_C_FLAGS values is unreliable — `rm -rf build` between modes):
rm -rf build
idf.py -DBENCH_ROM=sml -DCMAKE_C_FLAGS="-DBENCH_MODE_INTERP_ONLY=1" build
idf.py -p /dev/ttyUSB0 flash

# or
idf.py -DBENCH_ROM=sml -DCMAKE_C_FLAGS="-DBENCH_MODE_JIT_ONLY=1" build
# or
idf.py -DBENCH_ROM=sml -DCMAKE_C_FLAGS="-DBENCH_MODE_JIT_WARM_ONLY=1" build
# or
idf.py -DBENCH_ROM=sml -DCMAKE_C_FLAGS="-DBENCH_MODE_JIT_NOCACHE_ONLY=1" build
```

The `[BENCH] mode=…` line on UART is the result. The default
all-modes build prints all five lines in sequence but the
inter-mode post-teardown path currently panics (the dispatcher's
codecache arena is shared and not safe to reuse across runs); use
one of the per-mode `_ONLY` flags to get a clean line per build.
