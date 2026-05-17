# gbjit-xtensa

A JIT-translating Game Boy CPU emulator targeting **ESP32-S3 / Xtensa
LX7**. GB instructions are compiled at runtime into native Xtensa
machine code; blocks live in IRAM via `MALLOC_CAP_EXEC` and are entered
through a CALL0 / windowed-ABI bridge.

Current scope is the **CPU phase**. The reference interpreter handles
the full SM83 ISA; the JIT inlines the hot opcodes (see the
[STATUS.md](STATUS.md) coverage table) and falls back to the
interpreter helper for the rest. 10 of the 11
[Blargg `cpu_instrs`](https://github.com/retrio/gb-test-roms)
sub-tests pass end-to-end through the JIT, both on the host pipeline
and on a real ESP32-S3 running under `qemu-system-xtensa`.

```
gbjit: [06-ld r,r] halted=1 pc=$CC5F cycles=50000000 elapsed=2049 ms
       blocks_compiled=102 blocks_executed=89294 chain_hits=84535 chain_misses=4759
       throughput = 24.48 MHz (T-cycles) = 5.84× DMG
gbjit: RESULT: PASS
```

> See [PLAN.md](PLAN.md) for the original design and
> [STATUS.md](STATUS.md) for the latest progress, benchmarks per ROM,
> and notes on outstanding work.

## Quick start — host

```sh
sudo apt install build-essential cmake ninja-build python3            # one-time
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure                            # 6/6
```

Run a `.gb` ROM through the interpreter or the JIT (driven by the
in-tree Xtensa simulator):

```sh
./build/gbjit_host --interp --max-cycles 100000000 roms/06-ld_r_r.gb
./build/gbjit_host --jit    --max-cycles 100000000 roms/06-ld_r_r.gb
```

To fetch the Blargg sub-tests into `roms/`, see [roms/README.md](roms/README.md).

## Quick start — ESP32-S3 (qemu)

You need **ESP-IDF v6.x** with its bundled Xtensa toolchain and
`qemu-system-xtensa`. After running the IDF installer for the
`esp32s3` target:

```sh
. ~/.espressif/v6.0.1/esp-idf/export.sh
./scripts/run_qemu_s3.sh             # build + merge + qemu, exits 0 on PASS
```

The firmware embeds `06-ld_r_r.gb`, drives it through the JIT, and
prints throughput over UART.

To validate that the encoder emits the same bytes the IDF assembler
does:

```sh
./scripts/verify_encoder_vs_toolchain.sh
```

## Layout

```
core/                portable: SM83 interp, decoder tables, MMU
jit/                 portable: Xtensa LX7 encoder, codegen, code-cache,
                     dispatcher (incl. a small Xtensa sim used on host
                     to execute JIT output without real Xtensa)
port/host_linux/     CLI driver — `gbjit_host`
port/esp32s3/        ESP-IDF v6 project (component + main + sdkconfig)
tests/               ctest cases + manual stepwise-diff harness
scripts/             qemu runner + encoder-vs-toolchain validator
roms/                git-ignored; fetch script in roms/README.md
```

## Test inventory

```
$ ctest --test-dir build
    interp_smoke        — interp executes a hand-built ROM correctly
    encoder_smoke       — Xtensa encoder API sanity
    encoder_bits        — bit-exact output vs canonical Xtensa encodings
    xtensa_sim          — host sim executes encoder output
    jit_differential    — JIT cpu_state matches interpreter
                          over 4 hand-built ROMs + 1 loop ROM
    smc                 — invalidate-on-write correctness
```

Plus `tests/test_stepwise_diff <rom.gb>` — manual lockstep
interp/JIT comparator, used to bisect divergences when a real ROM
exposes a JIT bug.

## Notes

- **Why both an encoder and an in-tree simulator?** The host is x86; it
  can't execute the Xtensa bytes the JIT emits. The simulator (a small
  decoder written independently from the encoder) lets `ctest` actually
  *run* the generated code on every host build, catching encoder bugs
  long before the IDF image is even built. The simulator's coverage is
  limited to the subset of Xtensa the JIT emits.

- **JIT block ABI on target.** The JIT block runs under CALL0; IDF's C
  helpers are windowed. The bridge is in
  [`port/esp32s3/components/gbjit/jit_trampolines.S`](port/esp32s3/components/gbjit/jit_trampolines.S)
  and [`jit/dispatcher.c::enter_block_native`](jit/dispatcher.c). The
  JIT block *deliberately does not modify* `a1` — that breaks the
  window-overflow handler's spill-target calculation. The block's
  return PC is stashed in `cpu_state.jit_ret_pc` instead.

- **Known JIT-correctness limitation.** `02-interrupts.gb` checks
  cycle-accurate mid-block interrupt servicing. The JIT only services
  interrupts between blocks (not per op), so it can fail Blargg test
  #2 there. The reference interpreter also doesn't fully pass that ROM
  (it fails on the timer test, which the MMU stub doesn't implement
  yet). All other 10 sub-tests pass identically under interp and JIT.
