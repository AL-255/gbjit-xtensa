# Benchmark — interpreter vs JIT on emulated Xtensa LX7

This folder compares the **reference SM83 interpreter** against the
**JIT** when both are running on the same emulated Xtensa LX7 core
inside `qemu-system-xtensa`. The workload is one of the Blargg
`cpu_instrs` sub-tests (`06-ld r,r.gb`) embedded into the firmware via
ESP-IDF's `EMBED_FILES`.

Per-mode metrics captured:

| Metric | Source |
|--------|--------|
| GB cycles, wall time (simulated), throughput | firmware `[BENCH]` line over UART |
| Xtensa translation blocks (TBs) executed | `qemu -d in_asm,exec,nochain` trace |
| Xtensa instructions executed | TB sizes × execution counts |
| Memory-fetch / memory-store instructions | TBs broken down by mnemonic class |
| Per-region breakdown (IRAM / flash-XIP / ROM / DRAM) | TB PC classifier in `analyze.py` |

QEMU has **no Xtensa cache model**: each load/store costs one emulated
cycle. On real ESP32-S3 silicon, fetches from the flash-XIP region pay
10–40 wait cycles on an icache miss; SRAM (IRAM/DRAM) is single-cycle.
The benchmark's flash-XIP load count therefore *upper-bounds* the
icache-miss penalty on hardware — multiply by an expected miss rate
(typically 1-5 % for well-localised code) to get a wait-cycle estimate.

## Methodology

The benchmark builds **two separate firmware variants** so each mode's
trace contains exactly that mode's work (plus the same boot ROM + IDF
runtime, which we subtract as a shared floor):

- `BENCH_MODE_INTERP_ONLY=1` — `app_main` runs only `sm83_run_until()`.
- `BENCH_MODE_JIT_ONLY=1`    — `app_main` runs only `gbjit_dispatcher_run_until()`.

Each variant is then exercised in two passes:

1. **Untraced** — fast qemu run, just to capture the firmware's own
   `[BENCH] mode=… cycles=… elapsed_us=… mhz=… dmg_x=… …` line.
2. **Traced** — qemu with `-d in_asm,exec,nochain -D file`, captures
   every TB compile + every TB execution.

After both modes finish, `combine.py` joins the two traces into a
single comparison table, including a "mode-specific work (per-mode
total minus the shared startup/runtime floor)" section that takes the
per-region minimum across the two traces as a proxy for boot/IDF cost.

## Running

```sh
. ~/.espressif/v6.0.1/esp-idf/export.sh
cd benchmark
./run_bench.sh                # ~5-10 minutes
cat results/summary.md
```

To use a smaller cycle budget (e.g. for quicker tracing or smaller
trace files), set `BENCH_CYCLES_BUDGET`:

```sh
BENCH_CYCLES_BUDGET=50000 ./run_bench.sh
```

The default 200 000 GB cycles is enough for both modes to run the
opening Blargg "06-ld r,r" header plus the loop that eventually prints
"Passed". A 200 000 GB-cycle traced run produces ~50-200 MB of qemu
log per mode.

## Output

```
benchmark/
├── README.md          — this file
├── run_bench.sh       — driver: builds both firmware variants + traces
├── analyze.py         — parses a single trace, emits per-mode markdown
├── combine.py         — joins both per-mode analyses into summary.md
└── results/
    ├── interp_only_bench_lines.txt   committed, tiny
    ├── jit_only_bench_lines.txt      committed
    ├── interp_only_qemu_trace.log    git-ignored, large
    ├── jit_only_qemu_trace.log       git-ignored, large
    ├── interp.md                     committed, per-mode summary
    ├── jit.md                        committed
    └── summary.md                    committed, the comparison table
```
