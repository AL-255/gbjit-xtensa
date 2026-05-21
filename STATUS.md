# GBJIT-Xtensa — Status

Snapshot after the optimization pass driven by real Blargg cpu_instrs ROMs.

## Milestone status

| Milestone | State | Tests |
|-----------|-------|-------|
| **M1** — Host scaffolding + SM83 reference interpreter | ✅ done | `interp_smoke` |
| **M2** — Xtensa LX7 encoder (bit-accurate vs `xtensa-esp32s3-elf-as`) | ✅ done | `encoder_smoke`, `encoder_bits`, `xtensa_sim` |
| **M3** — Block-based JIT, host-validated through the Xtensa sim | ✅ done | `jit_differential` |
| **M4** — ESP32-S3 / ESP-IDF port, runs in `qemu-system-xtensa` | ✅ done | `scripts/run_qemu_s3.sh` |
| **M5** — JIT optimization passes | ✅ done (extensive) | see "Inlined ops" below |
| **M6** — SMC / cache-invalidation correctness | ✅ done | `smc` |

`ctest --test-dir build` — 6/6 green. **10 of 11** Blargg cpu_instrs
sub-tests pass end-to-end through the host JIT (`02-interrupts` has a
mid-block interrupt-servicing limitation; the host interpreter doesn't
pass it either).

## Inlined opcodes (M5)

Every op below is emitted as native Xtensa code with no `sm83_step`
helper round-trip. Memory accesses use a **runtime WRAM range check**
(`HL >> 13 == 6`) — fast path is 5–8 instructions, helper fallback on
anything outside `$C000..$DFFF`.

| Family | Opcodes |
|---|---|
| Constant flags / nop | NOP, CPL, SCF, CCF, HALT |
| Single-A rotates | RLCA, RRCA, RLA, RRA |
| Reg moves | LD r,n8; LD r,r′ (no-(HL) operands) |
| Reg INC/DEC | 8-bit INC/DEC r (Z/N/H eager, C preserved); 16-bit INC/DEC rr |
| 16-bit imm load | LD rr,n16 |
| Reg ALU | ADD/ADC/SUB/SBC/AND/XOR/OR/CP A,r (eager Z/N/H/C) |
| Imm ALU | ADD/ADC/SUB/SBC/AND/XOR/OR/CP A,n8 |
| (HL) ALU | ADD/ADC/SUB/SBC/AND/XOR/OR/CP A,(HL) — WRAM fast-path |
| (HL) loads/stores | LD r,(HL); LD (HL),r; LD (HL),n8; LD A,(HL±); LD (HL±),A |
| Absolute mem | LD A,(a16); LD (a16),A (WRAM/HRAM, decided at codegen time) |
| IO (HRAM only) | LDH (n8),A; LDH A,(n8) for n8 ≥ \$80 |
| Stack | PUSH/POP BC/DE/HL/AF (AF masks F's low nibble); CALL a16; RET; RST nn |
| Control flow | JR r8; JR cc,r8; JP a16; JP cc,a16 (all forward-branch patched) |
| CB shifts/rotates | RLC/RRC/RL/RR/SLA/SRA/SWAP/SRL on r (eager flags) |
| CB bit ops | BIT b,r; RES b,r; SET b,r (non-(HL)) |

Still in the helper path (= correct, slower):

- `JP (HL)`, `JR cc` to an unaligned target outside our `±2048` branch
  range (block bails out on the assert; not seen in practice).
- IO-range LDH and `LD (a16),A` to IO (\$FF00..\$FF7F) — side effects.
- `(HL)` for the CB ops, INC (HL), DEC (HL), conditional CALL/RET.
- `DAA`, `ADD HL,rr`, `ADD SP,r8`, `LD HL,SP+r8`, `LD SP,HL`, `LD (a16),SP`.
- `LD (C),A` / `LD A,(C)` (IO via C register).
- `EI` / `DI` / `RETI` / `STOP`.

These all stay correct via the `sm83_step` fall-back; only their
throughput is sub-optimal.

## Optimization status

| Optimization (from `PLAN.md` §3) | State |
|----------------------------------|-------|
| Block-level translation (basic-block JIT) | ✅ |
| Static guest→host register layout per block | ✅ — a11 = PC, a12 = cycles delta, a13 = cpu_state ptr |
| Eager flag computation in registers | ✅ for the ALU + INC/DEC + CB groups above |
| Inlined opcode codegen (hot ops) | ✅ — coverage table above |
| WRAM fast-path for `(HL)` / stack accesses | ✅ — 6-instr runtime check, helper fallback otherwise |
| Codegen-time region resolution for absolute LD | ✅ — no runtime branch for known a16 |
| Dispatcher-level predicted-next chain cache | ✅ — ~94 % hit rate on the Blargg sub-tests |
| Codegen-level direct chaining (patchable L32R+JX) | ⏸ scaffolded, not emitted yet |
| Code-cache reservation trimming (`codecache_trim`) | ✅ — blocks emit ~54 % of their worst-case budget; trimming the slack ~doubles how much of a working set fits an arena |
| Code-cache eviction (`GBJIT_JIT_EVICT` 0/1/2 = bump / coldness-LRU / circular-FIFO) | ✅ — coldness eviction is the fastest config on Tetris |
| OAM-DMA `DEC A;JR NZ` closed form (`is_dec_a_loop`) | ✅ — computes the wait's end state in O(1), no iteration |
| HRAM-flag busy-wait fast path (`is_io_poll_halt`) | ✅ — Tetris's hot FF85 wait → HALT fast-forward; ~+42 % board fps (`GBJIT_IO_POLL_HALT`) |
| Generic dispatcher self-loop fast path (`GBJIT_DISPATCHER_SELFLOOP`) | ✅ — re-enters any register/HRAM-pure self-loop block directly; JIT modes 0/1/2, overhead-neutral |
| Internal-clock serial transfer completion | ✅ — non-Blargg ROMs no longer hang on the never-clearing serial start bit |
| Lazy flag materialisation | ❌ planned |
| Per-flag dead-code elimination | ❌ planned |

## Benchmark — Blargg `06-ld r,r.gb`

Host throughput from `gbjit_host --interp/--jit` and target throughput
from `app_main` running under `qemu-system-xtensa` with the same ROM
baked in:

| Mode | Wall time | T-cycle throughput | vs DMG real-time |
|------|-----------|--------------------|--------------------|
| **Host x86_64 interpreter** (cc -O2) | 42 ms | 1,195 MHz | 285× |
| **Host x86_64 → Xtensa sim → JIT** | 157 ms (best 100ms after opt) | 320–450 MHz | 76–107× |
| **qemu-system-xtensa → JIT (native Xtensa exec)** | 2.0 s | 24 MHz | 5.8× |

Per-ROM throughput in the host JIT pipeline:

```
01-special                81×    07-jr,jp,call,ret,rst  107×
02-interrupts            111×    08-misc instrs         111×
03-op sp,hl               78×    09-op r,r               35×
04-op r,imm               72×    10-bit ops             105×
05-op rp                  60×    11-op a,(hl)            42×
06-ld r,r                107×
```

The qemu number is bounded by qemu's instruction-by-instruction Xtensa
emulation overhead (typically 5–10× slower than the real S3 at 240 MHz).
At 24 MHz emulated, that's still 5.8× DMG and the same JIT on a real S3
should clear 60–100 MHz of T-cycles (15–25× DMG).

## Benchmark — Tetris on the Heltec board

`benchmark/fps_sweep.sh` flashes the HTIT-WB32LAF_V3.2 board and measures
Tetris frames/wall-second across the JIT code-cache modes and arena
sizes (`benchmark/fps_plot.py` renders the plots in `benchmark/results/`):

| arena | no-eviction | coldness/LRU | circular/FIFO |
|------:|------------:|-------------:|--------------:|
| 64 KB | 166 | 138 | 124 |
| 96 KB | 165 | **264** | 191 |
| 128 KB | 166 | **268** | 214 |

Interpreter baseline 74 fps. Best config: **coldness eviction + 96 KB →
264 fps** (3.6× the interpreter, ~4.4× a real Game Boy), same IRAM as
the no-eviction default. No-eviction caps at ~166 because its
interpreter-fallback tail can never become JIT-resident; eviction has no
such cap. Below ~56 KB every JIT mode is slower than the interpreter —
the cache is too small to be worth it.

Tetris also exercises the busy-loop fast paths: the `is_io_poll_halt`
HRAM-flag patch is worth ~+42 % here (an A/B with it disabled measured
264 → 195 fps, see `benchmark/results/selfloop_ab.md`).

## ESP32-S3 build & run

```sh
. ~/.espressif/v6.0.1/esp-idf/export.sh
./scripts/run_qemu_s3.sh          # build + qemu + RESULT: PASS check
./scripts/verify_encoder_vs_toolchain.sh
```

The IDF project at `port/esp32s3/` wraps `core/` + `jit/` as a single
component. Highlights of the target wiring:

- `jit_trampolines.S` — CALL0→CALL8 shims so the JIT's CALL0-ABI helper
  invocations transition cleanly into IDF's windowed-ABI helpers.
- `dispatcher.c::enter_block_native` — windowed→CALL0 bridge into the
  JIT block.
- `target_helper_addr` returns the real `&sm83_step` / `&mmu_read8` /
  `&mmu_write8` addresses (via the trampolines) and the real
  `cpu_state*` / `mmu*` pointers.
- `app_main.c` embeds `06-ld_r_r.gb` via `EMBED_FILES`, runs the JIT for
  a fixed cycle budget, captures serial output, and prints throughput.

Important constraint surfaced and fixed: the JIT block runs without an
Xtensa stack frame (it does not modify `a1`) — modifying `a1` while
nested inside a windowed parent's call chain misroutes the window-
overflow handler's spill target. The JIT instead stashes its return PC
in `cpu_state.jit_ret_pc`.

## Test inventory

```
$ ctest --test-dir build --output-on-failure
    Start 1: interp_smoke           ✓
    Start 2: encoder_smoke          ✓
    Start 3: encoder_bits           ✓   bit-exact vs xtensa-esp32s3-elf-as
    Start 4: xtensa_sim             ✓   sim executes encoder output
    Start 5: jit_differential       ✓   smoke / alu / mem / fib / loop / inc
    Start 6: smc                    ✓   invalidation + mid-loop SMC
100% tests passed, 0 tests failed out of 6
```

Plus `tests/test_stepwise_diff` (manual): lockstep interp/JIT block
comparison, used to bisect the WRAM-execution and EI-delay bugs.

## Next likely work

1. **Codegen-level block chaining**: emit a patchable `L32R + JX` at
   block exit so hot loops never return to the dispatcher. Reserved
   slots (`gbjit_block::chain_lit_off` / `n_chain_lit`) are in place.
2. **Lazy flag materialisation**: defer F computation; biggest single
   win for arithmetic-heavy code that overwrites F before reading it.
3. **Real hardware run**: same firmware, an actual ESP32-S3 board, vs
   the qemu number above.
4. **PPU / APU**: the actual goal beyond this CPU-phase scaffold.
