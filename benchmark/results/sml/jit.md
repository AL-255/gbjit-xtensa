# QEMU-Xtensa benchmark — `jit` mode

## Firmware-reported throughput

| Mode | GB cycles | wall (µs) | throughput (MHz of T-cycles) | × DMG real-time |
|------|-----------|-----------|------------------------------|-----------------|
| jit | 200012 | 11176 | 17.897 | 4.267 |

JIT dispatcher: blocks_compiled=10 executed=7631 chain_hits=7580 chain_misses=50

## Xtensa instructions executed (combined: interp run + JIT run)

| Region | TBs executed | Instructions | Load instrs | Store instrs |
|--------|-------------:|-------------:|------------:|-------------:|
| flash XIP (interp + JIT helpers) | 515,606 | 1,712,282 | 474,347 | 120,500 |
| IRAM (JIT-emitted code) | 445,053 | 1,632,995 | 412,544 | 168,880 |
| boot ROM | 1,834,845 | 3,856,058 | 791,728 | 444,826 |
| **total** | **2,795,504** | **7,201,335** | **1,678,619** | **734,206** |

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
