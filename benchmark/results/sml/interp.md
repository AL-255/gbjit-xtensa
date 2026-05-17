# QEMU-Xtensa benchmark — `interp` mode

## Firmware-reported throughput

| Mode | GB cycles | wall (µs) | throughput (MHz of T-cycles) | × DMG real-time |
|------|-----------|-----------|------------------------------|-----------------|
| interp | 200000 | 8713 | 22.954 | 5.473 |

## Xtensa instructions executed (combined: interp run + JIT run)

| Region | TBs executed | Instructions | Load instrs | Store instrs |
|--------|-------------:|-------------:|------------:|-------------:|
| flash XIP (interp + JIT helpers) | 1,618,057 | 4,614,117 | 1,123,366 | 298,800 |
| IRAM (JIT-emitted code) | 3,190,191 | 12,392,425 | 2,844,789 | 1,046,912 |
| boot ROM | 1,772,706 | 4,430,875 | 842,101 | 423,806 |
| **total** | **6,580,954** | **21,437,417** | **4,810,256** | **1,769,518** |

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
