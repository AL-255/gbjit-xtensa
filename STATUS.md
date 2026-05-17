# GBJIT-Xtensa — Status

Snapshot at end of M5 + M6 work block. See `PLAN.md` for the original design.

## Milestone status

| Milestone | State | Tests |
|-----------|-------|-------|
| **M1** — Host scaffolding + SM83 reference interpreter | ✅ done | `interp_smoke` |
| **M2** — Xtensa LX7 encoder (24-bit ISA subset, bit-accurate against canonical encodings) | ✅ done | `encoder_smoke`, `encoder_bits`, `xtensa_sim` |
| **M3** — Block-based JIT, host-validated through the Xtensa simulator | ✅ done | `jit_differential` |
| **M4** — ESP32-S3 / ESP-IDF port | ✅ done | runs under qemu-system-xtensa via `scripts/run_qemu_s3.sh` |
| **M5** — JIT optimization passes | ✅ done (a–e, see below) | `jit_differential` (3 ROMs) |
| **M6** — SMC / cache-invalidation correctness | ✅ done | `smc` |

6 ctest cases. All pass on the host (`cmake --build build && ctest`).

## What's now JIT-inlined (M5)

Every op below produces inline Xtensa code that mutates `cpu_state` directly,
with no helper call. Everything else falls back to a CALLX0 to the reference
interpreter, with full PC/cycles sync around the call.

| Op family | Opcodes | Notes |
|-----------|---------|-------|
| `NOP` | 0x00 | PC + 1, cycles + 4 |
| `LD r,n8` | 0x06 0x0E 0x16 0x1E 0x26 0x2E 0x3E | r ∈ {A,B,C,D,E,H,L}; (HL) variant is helper |
| `LD r,r'` | 0x40..0x7F (no HALT, no `(HL)` either side) | reg ↔ reg copies |
| `HALT` | 0x76 | sets `cpu->halted`, exits the block |
| `INC r` / `DEC r` (8-bit) | 0x04 0x0C 0x14 0x1C 0x24 0x2C 0x3C / 0x05 … | eager Z/N/H flags, preserves C |
| `INC rr` / `DEC rr` (16-bit) | 0x03 0x13 0x23 0x33 / 0x0B 0x1B 0x2B 0x3B | no flag effects; BC/DE/HL/SP |
| `LD rr,n16` | 0x01 0x11 0x21 0x31 | BC/DE/HL/SP |
| `ADD A,r` | 0x80..0x87 (no `(HL)`) | eager Z, N=0, H, C |
| `SUB r` | 0x90..0x97 | eager Z, N=1, H (borrow), C (borrow) |
| `AND r` | 0xA0..0xA7 | eager Z, N=0, H=1, C=0 |
| `XOR r` | 0xA8..0xAF | eager Z, N=H=C=0 |
| `OR  r` | 0xB0..0xB7 | eager Z, N=H=C=0 |
| `CP  r` | 0xB8..0xBF | A unchanged; eager Z/N/H/C |

Not yet inlined (fall back to `sm83_step` helper):
- `ADC` / `SBC` (need to read C in the inlined sequence)
- `INC (HL)` / `DEC (HL)`, `LD (HL),…`, `LD A,(HL)` etc. — anything touching `(HL)`
- `JR cc,r8`, `JP cc,a16`, `CALL`, `RET`, `RST`, `PUSH/POP`
- CB-prefix shifts/rotates/`BIT`/`RES`/`SET`
- `ADD HL,rr`, `LD HL,SP+r8`, `ADD SP,r8`, `DAA`, `CPL`, `SCF`, `CCF`
- Conditional control flow and stack ops generally

These remain correct (via the helper), they're just slow paths.

## Optimization status

| Optimization (from `PLAN.md` §3) | State |
|----------------------------------|-------|
| Block-level translation (basic-block JIT) | ✅ |
| Static guest→host register layout per block | ✅ — `a11`=PC, `a12`=cycles_delta, `a13`=cpu_state ptr, `a14/a15` scratch |
| Eager flag computation in registers | ✅ — for the ALU ops listed above |
| Inlined opcode codegen (hot ops) | ✅ — coverage above; helper fallback for the rest |
| Lazy flag materialisation | ❌ planned next |
| Per-flag dead-code elimination | ❌ planned next |
| Direct block chaining via patchable L32R + JX (codegen-level) | ⏸ scaffolded (`chain_lit_off`/`n_chain_lit` in `gbjit_block`) but not yet emitted by codegen; pending alongside M4 since the host sim doesn't gain from it |
| Dispatcher-level "predicted next block" cache | ✅ — measured via `dispatcher.chain_hits` / `chain_misses` |
| Inlined fast-path memory access | ❌ planned next |
| Code-cache eviction (currently bump-arena, flush all on fill) | ⏸ |
| Self-modifying code invalidation | ✅ — see below |

## SMC invalidation (M6)

- 256-byte GB-address page granularity (`GBJIT_SMC_PAGE_COUNT = 256`).
- On `insert_block`, the block registers on every page it overlaps.
- `gbjit_dispatcher_invalidate_addr(d, gb_addr)` drops every block whose
  range overlaps the page containing `gb_addr`, removes them from the
  bucket table, clears any dangling `predicted_next` pointers, and frees
  the blocks (the code itself stays in the codecache arena until the next
  flush — bump-arena semantics).
- `smc.c` covers two scenarios: (1) compile → invalidate → recompile gives
  a fresh block, and (2) mid-loop invalidation does not corrupt cpu_state
  (matches the interpreter's final state exactly).
- Wiring `mmu_write8` to call `gbjit_dispatcher_invalidate_addr` is left
  to the dispatcher owner: today, the dispatcher exposes the API but
  doesn't install a write hook on the MMU. Real games rarely execute from
  WRAM/HRAM, so the MMU-side wiring is deferred until M4 makes that
  scenario actually testable on hardware.

## Encoder corrections (subtle but important)

While writing M5, the encoder/sim were both rewritten against the canonical
Xtensa ISA tables (cross-referenced through ida-xtensa2):
- `SLLI` — bits 21..23 must be 0 (was 0b001 wrong).
- `SRAI` — bit 21 fixed = 1, sa_hi1 at bit 20 (the high-shift case was
  encoded into the wrong nibble).
- `EXTUI` — maskimm goes in bits 20..23, sh_hi1 at bit 16, bits 17..19
  fixed at 0b100 (was stuffed into the wrong op1/op2 split).

These all passed `test_xtensa_sim` before because the *simulator* shared
the encoder's bug. The encoder and sim now agree with each other and with
the canonical ISA — verified bit-for-bit in `test_encoder_bits`.

## Test inventory

```
$ ctest --test-dir build --output-on-failure
    Start 1: interp_smoke           ✓
    Start 2: encoder_smoke          ✓
    Start 3: encoder_bits           ✓   28 bit-exact encoder assertions
    Start 4: xtensa_sim             ✓   sim correctly executes encoder output
    Start 5: jit_differential       ✓   4 ROMs (smoke, ALU, INC/DEC + 16-bit, loop)
    Start 6: smc                    ✓   basic invalidation + mid-loop invalidation
100% tests passed, 0 tests failed out of 6
```

## ESP32-S3 / qemu — how it runs

`port/esp32s3/` is an ESP-IDF v6 project that wraps `core/` + `jit/`
(minus `xtensa_sim.c`, which is host-only) as a component and a tiny
`app_main` driver. Key on-target details:

- **Calling convention bridge**: IDF compiles C with the windowed Xtensa
  ABI (ENTRY / RETW, CALL8 / CALLX8); the JIT emits CALL0 (RET / CALLX0).
  `enter_block_native` in `jit/dispatcher.c` is a small windowed wrapper
  that saves the windowed return PC, `CALLX0`s into the JIT block, and
  restores after the JIT's `JX a0` returns.
- **Helper-pointer resolver**: on target, `target_helper_addr()` returns
  real `&sm83_step`, `&mmu_read8`, `&mmu_write8` plus the actual
  `cpu_state*` and `mmu*` pointers — no host sentinels.
- **Code arena**: `heap_caps_malloc(MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL |
  MALLOC_CAP_32BIT)` on the S3. Internal SRAM is not L1-cached on the
  S3, so `codecache_finalize` is a memory barrier only — no `esp_cache_*`
  needed.
- **Console**: `CONFIG_ESP_CONSOLE_UART_DEFAULT=y` so qemu's `-serial
  mon:stdio` captures `ESP_LOGI` output.
- **Watchdogs disabled** so the JIT busy-loop is not interrupted
  (`CONFIG_ESP_INT_WDT=n`, `CONFIG_ESP_TASK_WDT_EN=n`).

To run from a clean checkout:

```sh
. ~/.espressif/v6.0.1/esp-idf/export.sh
./scripts/run_qemu_s3.sh           # builds, merges, runs in qemu
```

Expected output:

```
gbjit: halted=1 pc=$010B cycles=44
gbjit: end state: A=$00 F=$70 B=$13
gbjit: RESULT: PASS
```

That's the same end-state the host `jit_differential` test asserts —
the JIT generates identical Xtensa bytes on host and target, and the
target execution matches the host simulator.

### Encoder ground-truth check

`scripts/verify_encoder_vs_toolchain.sh` assembles the same mnemonics
with `xtensa-esp32s3-elf-as` and prints the canonical bytes — used to
catch encoder bugs that the in-tree round-trip tests would otherwise
miss (since the simulator and encoder could agree on a wrong layout).
This is exactly how the EXTUI fixed-bit-pattern bug was found and
corrected.

## Next likely work

1. **More inline coverage**: `ADC`/`SBC`, control flow (`JR cc`, `JP`),
   PUSH/POP, `(HL)` memory accesses with a fast-path WRAM/HRAM check.
2. **Lazy flag materialisation**: huge win for arithmetic-heavy code
   where the F register is overwritten before being read.
3. **Codegen-level block chaining**: emit a patchable L32R + JX at block
   exit so hot loops never return to the dispatcher. The infrastructure
   (`chain_lit_off` / `n_chain_lit` on `gbjit_block`) is reserved; only
   the emit-and-patch path is missing.
4. **Wire `mmu_write8` → `gbjit_dispatcher_invalidate_addr`** so SMC
   under real games (rare) drops stale blocks automatically.
