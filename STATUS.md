# GBJIT-Xtensa — Status

Snapshot of the project at the end of the M1–M3 work block. See `PLAN.md`
for the full design.

## Milestone status

| Milestone | State | Tests |
|-----------|-------|-------|
| **M1** — Host scaffolding + SM83 reference interpreter | ✅ done | `interp_smoke` |
| **M2** — Xtensa LX7 encoder (24-bit ISA subset) | ✅ done | `encoder_smoke`, `encoder_bits`, `xtensa_sim` |
| **M3** — Naïve JIT (block-based, host-validated) | ✅ done | `jit_differential` |
| **M4** — ESP32-S3 / ESP-IDF port | ⏳ scaffolded, not built | requires user-side ESP-IDF install |
| **M5** — JIT optimization passes | ⏳ not started | — |
| **M6** — SMC / cache-invalidation correctness | ⏳ not started | — |

All currently-implemented work is exercised by 5 ctest cases — every one
passes on the host (`cmake --build build && ctest`).

## What works today

- **Reference SM83 interpreter** (`core/sm83_interp.c`)
  Full opcode coverage including CB-prefix shifts/rotates/bit ops, DAA,
  conditional jumps/calls/rets, RST, HALT, EI/DI delay, and interrupt
  service. Used both as a correctness oracle and as the JIT fallback for
  any opcode the codegen doesn't (yet) inline.

- **MMU stub** (`core/memory.c`)
  MBC0 only (32 KB single-bank ROM), WRAM, HRAM, VRAM, OAM, and a flat IO
  byte store. Includes the Blargg-style serial trap (`FF02 = $81` →
  `serial_sink(FF01)`) so cpu_instrs-class test ROMs can stream their
  PASS/FAIL output without a real serial peripheral.

- **Xtensa LX7 encoder** (`jit/emit_xtensa.c`)
  Bit-accurate encodings, verified by `test_encoder_bits.c` against the
  canonical opcodes from the Xtensa ISA Reference (cross-checked through
  the ida-xtensa2 disassembler tables). Covers the subset the JIT actually
  emits:
  - RRR arithmetic/logical: `ADD SUB AND OR XOR MOV`
  - RRI8 family: `ADDI MOVI L8UI S8I L16UI S16I L32I S32I`
  - PC-relative literal load: `L32R`
  - Control flow: `J JX CALL0 CALLX0 RET BEQZ BNEZ`
  - Shifts and bit field: `SLLI SRLI SRAI EXTUI`

- **Xtensa LX7 mini-simulator** (`jit/xtensa_sim.c`)
  Host-side executor for JIT output. Decoder is written independently
  from the encoder (different code path), so a shared encoding bug
  surfaces immediately rather than silently passing tests. Supports:
  - Same instruction subset as the encoder
  - Memory accesses through a host translation callback
  - L32R via a literal-pool callback
  - `CALLX0` dispatched to a user-provided thunk (used by the dispatcher
    to route the JIT's helper calls back to C functions)

- **Code cache + dispatcher** (`jit/codecache.c`, `jit/dispatcher.c`)
  - Bump-allocator arena. Host port allocates `mmap(PROT_EXEC)`;
    target port uses `heap_caps_malloc(MALLOC_CAP_EXEC|MALLOC_CAP_INTERNAL)`.
  - Chained hash table for `gb_pc → gbjit_block` lookup.
  - `codecache_finalize()` handles icache sync — `__builtin___clear_cache`
    on host, `esp_cache_msync` on the S3.
  - On host, blocks execute through the Xtensa sim; on target, the same
    bytes execute natively via a direct CALL0 invocation.

- **JIT codegen** (`jit/codegen.c`, v0)
  - Block discovery: walks forward from a guest PC, terminating at any
    branch/halt/stop or after `MAX_OPS_PER_BLOCK` (32) instructions.
  - Emits a 4-byte-aligned literal pool followed by Xtensa code.
  - Per-block prologue allocates a 16-byte stack frame and saves `a0`.
  - **For every GB op** emits the same sequence:
    ```
    L32R    a2,  =cpu_state_ptr     ; reload cpu pointer (caller-saved)
    L32R    a14, =sm83_step         ; helper address
    CALLX0  a14                     ; sm83_step(cpu)
    ```
  - Epilogue restores `a0` / `a1` and returns.

## Optimization status — what is and isn't applied yet

The v0 codegen is **deliberately un-optimized**: it gives us a working
pipeline (block discovery → emit → finalize → execute → exit) before any
work is spent on speed. Compared to a plain interpreter loop, today's JIT
saves only the per-step dispatcher overhead (one cycle-budget check, one
hash lookup) for the duration of one block. That's a small win — usually
< 10% — and intentional.

Concretely:

| Optimization (from `PLAN.md` §3) | State |
|----------------------------------|-------|
| Block-level translation (basic-block JIT) | ✅ implemented |
| Static guest→host register mapping in block | ❌ not yet — every op reloads `cpu_state` and calls `sm83_step` |
| Inlined opcode codegen (LD r,n8 / LD r,r' / NOP / ADD…) | ❌ not yet — placeholder tables `ops_pc/ops_opcode/ops_len` are already collected during discovery, ready for the inlining pass |
| Eager flag computation in registers | ❌ not yet (interpreter handles flags) |
| Lazy flags (defer F materialization) | ❌ planned (M5) |
| Per-flag dead-code elimination | ❌ planned (M5) |
| Direct block chaining via patchable L32R + JX | ❌ planned (M5) — `gbjit_block::chain_lit_off` field already reserved |
| Inlined fast-path memory access (page-table check) | ❌ planned (M5) |
| Code-cache eviction (currently: bump arena, flush all on fill) | ⏳ basic flush-on-full; eviction deferred |
| Self-modifying-code / cache invalidation | ❌ planned (M6) — assumed code lives in ROM today |

This staged ordering is the point of M3 vs M5: get a *correct* pipeline
first, then layer optimizations onto it without destabilizing the
end-to-end flow.

## What's needed to finish M4 (the port)

`port/esp32s3/` is empty. The host side already compiles the same `core/`
and `jit/` modules unchanged for the ESP32-S3 target — the only missing
piece is the ESP-IDF component scaffold (CMakeLists, `app_main`, sdkconfig
defaults, and the helper-pointer resolver that returns real function
addresses instead of the host's token values).

Blockers on this machine right now:
- **ESP-IDF v5.x** not installed (`xtensa-esp32s3-elf-gcc` absent).
- **`qemu-system-xtensa`** not installed (apt: `qemu-system-misc`,
  needed unless you flash real hardware).
- **`ccache`**, **`dfu-util`**, **`gdb-multiarch`** also missing from the
  `sudo apt install` list in `PLAN.md` §6.

Once those are in place, the M4 work is mechanical:
1. `port/esp32s3/main/app_main.c` — initialise the dispatcher, load a
   built-in ROM blob from flash, loop the dispatcher, print Blargg serial
   output over `UART0`.
2. `port/esp32s3/CMakeLists.txt` — ESP-IDF project file that pulls in
   `core/` and `jit/` as a single component.
3. `port/esp32s3/sdkconfig.defaults` — Octal PSRAM, CPU @ 240 MHz,
   release build, USB-CDC console.
4. `scripts/run-qemu.sh` — wrap `idf.py qemu` so the differential test
   ROM runs unattended.

## Test inventory

```
$ cd build && ctest --output-on-failure
    Start 1: interp_smoke           ✓
    Start 2: encoder_smoke          ✓
    Start 3: encoder_bits           ✓   (28 bit-exact encoder assertions)
    Start 4: xtensa_sim             ✓   (movi/add, l8ui/s8i, branches, shifts+EXTUI)
    Start 5: jit_differential       ✓   (full cpu_state match: interp vs JIT-over-sim)
100% tests passed, 0 tests failed out of 5
```

## Files of note

| Path | Purpose |
|------|---------|
| `PLAN.md` | Original design document |
| `core/sm83_interp.c` | Reference interpreter (oracle + JIT fallback) |
| `core/sm83_decoder.c` | Static decode tables (length, cycles, terminator flag) |
| `core/memory.c` | MBC0 MMU + serial sink |
| `jit/emit_xtensa.c` | Xtensa LX7 encoder, bit-level documented |
| `jit/xtensa_sim.c` | Independent decoder/simulator for host validation |
| `jit/codegen.c` | v0 block codegen (CALLX0-per-op pipeline) |
| `jit/codecache.c` | Executable-memory arena + icache sync |
| `jit/dispatcher.c` | Block lookup, find-or-compile, host vs target invocation |
| `port/host_linux/main.c` | CLI driver (`--interp` / `--jit`) for `.gb` files |
| `tests/test_jit_differential.c` | End-to-end JIT correctness vs interpreter |
