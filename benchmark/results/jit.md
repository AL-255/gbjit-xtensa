# QEMU-Xtensa benchmark — `jit` mode

## Firmware-reported throughput

| Mode | GB cycles | wall (µs) | throughput (MHz of T-cycles) | × DMG real-time |
|------|-----------|-----------|------------------------------|-----------------|
| jit | 200020 | 8644 | 23.140 | 5.517 |

JIT dispatcher: blocks_compiled=26 executed=6552 chain_hits=6494 chain_misses=57

## Xtensa instructions executed (combined: interp run + JIT run)

| Region | TBs executed | Instructions | Load instrs | Store instrs |
|--------|-------------:|-------------:|------------:|-------------:|
| flash XIP (interp + JIT helpers) | 616,409 | 1,753,497 | 460,566 | 141,550 |
| IRAM (JIT-emitted code) | 403,759 | 1,499,906 | 393,836 | 163,525 |
| boot ROM | 1,669,375 | 3,286,399 | 694,586 | 288,693 |
| **total** | **2,689,543** | **6,539,802** | **1,548,988** | **593,768** |

## Notes

- Both modes run sequentially in the same boot, so the totals
  above cover the *sum* of one interp run + one JIT run.
- IRAM-region Xtensa code = JIT-emitted block bodies. Anything
  there is JIT-side only; the interp never executes from IRAM.
- Flash-XIP code = pre-compiled C: SM83 interpreter, helpers
  (sm83_step / mmu_read8 / mmu_write8), IDF runtime, libc.
  The JIT shares this region (helper fallback) but takes the
  short path through tinier helpers.
- QEMU has no cache model: every load/store on this trace is
  modelled as a single-cycle access. On real ESP32-S3 silicon,
  flash-XIP loads pay 10–40 wait cycles on icache miss; SRAM
  loads are 1 cycle. The flash-XIP instruction count is an
  upper bound on potentially-stalling fetches.
