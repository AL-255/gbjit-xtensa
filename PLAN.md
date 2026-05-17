# GBJIT-Xtensa — Plan (CPU Phase)

## 1. Scope of this phase
- **In**: SM83 (GB CPU) core, JIT translator to Xtensa LX7 machine code, code cache, basic block discovery, opcode-level correctness against test ROMs (Blargg `cpu_instrs`, `instr_timing`).
- **Out (for now)**: PPU, APU, MBC banking beyond the minimum needed to load tests, input, save states.
- **Target host**: ESP32-S3 (dual-core Xtensa LX7 @ 240 MHz, 512 KB SRAM, ≥2 MB PSRAM, ≥4 MB flash). Initial bring-up will also be runnable on the host Linux box via a portable "interpreter reference" so we can diff JIT vs reference output before flashing.

## 2. Target architecture notes (the things that bite)
- **ESP32-S3 uses the CALL0 ABI** (no register windows), 16 GP regs `a0..a15`. `a0` = return, `a1` = SP, `a15` = frame pointer in some configs. We'll keep `a2..a13` as our working pool.
- **Executable memory**: code cache must live in IRAM, allocated via `heap_caps_malloc(..., MALLOC_CAP_EXEC | MALLOC_CAP_32BIT | MALLOC_CAP_INTERNAL)`. After writing a block, flush with `Cache_WriteBack_Addr` + `Cache_Invalidate_ICache_Items` (ESP-IDF rom funcs) or the IDF wrapper.
- **Instruction encoding**: 24-bit narrow + 16-bit "density" instructions. We'll only emit a curated subset — easier and good enough.
- **Branch range**: `J` is ±128 KB, `BEQZ/BNEZ` etc. are short. For inter-block jumps we use indirect through a register (`JX`), and block-chaining via a small trampoline slot at the end of each block we can patch.
- **Alignment**: code in IRAM should be 4-byte aligned; literals (constant pool) loaded via `L32R` need to be in a literal pool reachable within ±256 KB.

## 3. JIT design

### 3.1 Translation unit
- **Basic blocks**: start at a PC, translate forward until any control-flow op (`JR/JP/CALL/RET/RST`, conditional or not), HALT/STOP, or a memory write that could target self-modifying code regions (HRAM/WRAM mirrors). Cap block length (~64 GB instructions) to bound translation cost.

### 3.2 Guest → host register mapping (static, per-block; spill on call out)
| GB | Xtensa | Notes |
|---|---|---|
| A | a3 (low 8) | mask after arith |
| F (flags as bits) | a4 | lazy flags possible (see 3.4) |
| BC | a5 | pack BC into low 16, or split B=a5h, C=a5l via shifts |
| DE | a6 | same |
| HL | a7 | same — HL is hot, keep packed |
| SP | a8 | 16-bit, masked |
| PC | a9 | only written at block exit / on interrupt |
| cycles accumulator | a10 | added per-instruction immediate |
| `cpu_state*` ptr | a11 | base for memory ops + helper calls |
| scratch | a12, a13 | |

Prologue loads the regs from the `cpu_state` struct; epilogue writes them back. Inside a block we stay in registers.

### 3.3 Memory access
- Generated code calls into C helpers `mem_read8`, `mem_write8`, `mem_read16`, `mem_write16`. These do MMIO dispatch and MBC handling.
- Optimization later: inline fast-paths for known-safe regions (WRAM, HRAM) via a 256-entry page table indexed by high byte of address — emit ~6 instructions of inline check + load, fallback `CALLX0` to the helper.

### 3.4 Flag handling — three modes, escalate as needed
1. **Eager** (v0): compute Z/N/H/C as bits in F after every flag-affecting op. Simple, slow.
2. **Lazy** (v1): keep `last_op`, `last_result`, `last_operand_a`, `last_operand_b` in scratch slots. Materialize F only when a `PUSH AF`, `LD (nn),A`-style, or conditional branch on flags needs it. Massive win because most flag updates die before being read.
3. **Per-flag dead-code elim** (v1.5): track which flags the next consumer needs; skip computing the rest.

### 3.5 Block exits and chaining
- Every block ends by writing `PC` and the cycle delta back, then either:
  - **Return to dispatcher** (always works), or
  - **Direct chain**: at the tail of the block, a 2-instruction stub `L32R a12, target_block_ptr; JX a12`. The literal is patched in-place when the target block is compiled. Unknown targets stay as "return to dispatcher" until resolved.
- This keeps tight loops (e.g. Nintendo logo scroll) running without dispatcher overhead.

### 3.6 Self-modifying code / cache invalidation
- GB cartridge ROM is read-only → cache forever.
- WRAM/HRAM writes invalidate any cached block whose source range overlaps. Track this with a coarse "dirty page" bitmap (1 bit per 256 bytes of GB memory) and a per-page list of blocks. On write, drop matching blocks. We assume game code rarely runs from WRAM (Tetris etc. don't), so this is a slow path.

### 3.7 Code cache management
- Single contiguous IRAM arena (start with 64 KB, tune later).
- On fill, **flush everything** rather than implementing eviction (simple, predictable). Track stats to know if eviction is worth it.

## 4. Module layout
```
gbjit-xtensa/
├── core/                   # portable, builds on host + target
│   ├── sm83_decoder.{c,h}  # opcode tables, decode info (length, flags affected, mem behavior)
│   ├── sm83_interp.{c,h}   # reference interpreter (truth oracle + fallback)
│   ├── cpu_state.h         # guest register file, flags, cycle counter
│   ├── memory.{c,h}        # MMU + MBC stubs (MBC0/MBC1 enough for tests)
│   └── timing.h            # cycle constants
├── jit/
│   ├── ir.{c,h}            # tiny IR (1 op per GB instruction; optimizer passes work on this)
│   ├── passes/             # const-fold, dead-flag-elim, peephole, regalloc
│   ├── emit_xtensa.{c,h}   # Xtensa LX7 encoder (only the ops we use)
│   ├── codecache.{c,h}     # IRAM arena, chain-patching, invalidation
│   └── dispatcher.{c,h}    # find-or-compile, enter JIT, handle exits
├── port/
│   ├── host_linux/         # mmap PROT_EXEC arena, used for unit tests
│   └── esp32s3/            # heap_caps_malloc EXEC arena, cache flush, main.c
├── tests/
│   ├── encoder_tests.c     # round-trip every emitted Xtensa op through a disassembler
│   ├── differential.c      # run N steps under interpreter and JIT, diff cpu_state
│   └── roms/               # Blargg test ROMs (download script)
└── CMakeLists.txt          # builds host target by default; ESP-IDF component for S3
```

Building the JIT itself on the Linux host first (with an `mmap(PROT_EXEC)` arena) lets us run the differential test suite with `gdb` before any flashing. The same `jit/` code compiles for both.

## 5. Phased milestones
1. **M1 — Scaffolding (host only)**: decoder, interpreter, memory stubs, Blargg ROM loader. Pass `cpu_instrs` under the *interpreter*. This is our oracle.
2. **M2 — Xtensa encoder + harness**: emit Xtensa instructions in a buffer; round-trip-test against `xtensa-esp32s3-elf-objdump` to confirm encodings. No JIT execution yet.
3. **M3 — Naïve JIT (host)**: one GB op → many Xtensa ops, eager flags, no chaining, dispatcher round-trip every block. Run via QEMU-xtensa (`qemu-system-xtensa`) to validate JIT-emitted code executes correctly before touching real hardware.
4. **M4 — JIT on ESP32-S3**: IRAM arena, icache flush, get Blargg `cpu_instrs` passing on hardware (serial-out for the results).
5. **M5 — Optimizations**: block chaining → lazy flags → dead-flag elim → memory fast-paths. Measure throughput (target: ≥10× DMG speed sustained on one core, leaving the other for PPU later).
6. **M6 — Cleanup & invalidation correctness**: SMC stress tests, cache-full behavior, long-run soak.

## 6. Toolchain — `sudo apt install …`

```
sudo apt install build-essential git wget curl cmake ninja-build ccache \
                 flex bison gperf \
                 python3 python3-pip python3-venv python3-setuptools \
                 libffi-dev libssl-dev \
                 libusb-1.0-0 dfu-util \
                 qemu-system-misc \
                 gdb-multiarch
```

What each is for:
- `build-essential cmake ninja-build ccache` — host build + ESP-IDF build system.
- `git wget curl` — ESP-IDF install + fetching Blargg ROMs.
- `flex bison gperf python3 python3-{pip,venv,setuptools} libffi-dev libssl-dev` — required by ESP-IDF's `install.sh`.
- `libusb-1.0-0 dfu-util` — flashing the S3 over USB.
- `qemu-system-misc` — provides `qemu-system-xtensa` on Ubuntu/Debian; used in M3 for off-target JIT execution tests.
- `gdb-multiarch` — debugging both host and target images.

**Not via apt** (install separately):
- **ESP-IDF v5.x** via its own installer.
- Blargg test ROMs (small, fetched at build time).
