# QEMU-Xtensa benchmark — `jit` mode

## Firmware-reported throughput

| Mode | GB cycles | wall (µs) | throughput (MHz of T-cycles) | × DMG real-time |
|------|-----------|-----------|------------------------------|-----------------|
| jit | 200020 | 8660 | 23.097 | 5.507 |

JIT dispatcher: blocks_compiled=25 executed=6552 chain_hits=6494 chain_misses=57

## Xtensa instructions executed (combined: interp run + JIT run)

| Region | TBs executed | Instructions | Load instrs | Store instrs |
|--------|-------------:|-------------:|------------:|-------------:|
| flash XIP (interp + JIT helpers) | 591,462 | 1,714,782 | 446,223 | 140,812 |
| IRAM (JIT-emitted code) | 401,324 | 1,490,793 | 391,861 | 162,899 |
| boot ROM | 1,751,937 | 3,408,507 | 696,818 | 287,775 |
| **total** | **2,744,723** | **6,614,082** | **1,534,902** | **591,486** |

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
