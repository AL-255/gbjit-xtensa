# QEMU-Xtensa benchmark — `jit` mode

## Firmware-reported throughput

| Mode | GB cycles | wall (µs) | throughput (MHz of T-cycles) | × DMG real-time |
|------|-----------|-----------|------------------------------|-----------------|
| jit | 200012 | 5878 | 34.027 | 8.113 |

JIT dispatcher: blocks_compiled=10 executed=7631 chain_hits=7580 chain_misses=50

## Xtensa instructions executed (combined: interp run + JIT run)

| Region | TBs executed | Instructions | Load instrs | Store instrs |
|--------|-------------:|-------------:|------------:|-------------:|
| flash XIP (interp + JIT helpers) | 529,361 | 1,592,194 | 404,548 | 121,299 |
| IRAM (JIT-emitted code) | 445,241 | 1,632,883 | 412,912 | 168,857 |
| boot ROM | 1,856,578 | 3,795,697 | 779,244 | 338,967 |
| **total** | **2,831,180** | **7,020,774** | **1,596,704** | **629,123** |

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
