# Running gbjit-xtensa

Four ways to run, from quickest to most hardware-specific:

1. [Host build](#1-host-build--run) — JIT/interpreter on your PC (no ESP-IDF)
2. [Unit tests](#2-unit-tests)
3. [QEMU virtual LCD](#3-qemu-virtual-lcd) — watch a game run, no board needed
4. [Real hardware](#4-real-hardware-heltec-esp32-s3) — flash the Heltec ESP32-S3 board

Plus the [benchmark sweeps](#5-benchmarks) at the end.

---

## 1. Host build & run

Builds `gbjit_host`, a PC executable that runs a ROM through either the
reference interpreter or the JIT (JIT-emitted Xtensa code is executed
by the bundled `xt_sim` interpreter). Needs only CMake + a C compiler.

```sh
cmake -B build
cmake --build build
```

Run a ROM:

```sh
# reference interpreter
./build/gbjit_host --interp --max-cycles 50000000 roms/sml_rev1.gb

# host JIT
./build/gbjit_host --jit --max-cycles 50000000 roms/sml_rev1.gb
```

Flags:

| Flag | Meaning |
|------|---------|
| `--interp` | reference SM83 interpreter (default) |
| `--jit` | host JIT |
| `--no-cache` | JIT with the block cache disabled (recompile every block) |
| `--no-prefetch` | JIT with the static-successor prefetcher off |
| `--max-cycles N` | stop after N T-cycles (default 200M) |
| `--dump-fb C F.pgm` | run to cycle C, write the 160×144 framebuffer as a PGM |

The host build caps its address space at 500 MB (`RLIMIT_AS`) so a
runaway JIT bug can't take the machine down — override with
`GBJIT_RLIMIT_MB=0` (off) or any MB value.

### Compile-time options

Every JIT/PPU toggle is a CMake `-D` flag; the C sources carry the
default. Pass any subset:

```sh
cmake -B build -DGBJIT_HALT_STEP_CYCLES=1024 -DGBJIT_CHAIN_PREDICTOR_WAYS=1
```

The full list (defaults in parentheses) lives in
`benchmark/results/board_option_sweep.md`; the load-bearing ones are
`GBJIT_PPU_ASYNC` (0), `GBJIT_HALT_STEP_CYCLES` (4),
`GBJIT_DISPATCHER_HALT_INNER_LOOP` (1), `GBJIT_CHAIN_PREDICTOR_WAYS` (2).

---

## 2. Unit tests

```sh
cmake -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

Six tests: `interp_smoke`, `encoder_smoke`, `encoder_bits`,
`xtensa_sim`, `jit_differential`, `smc`.

> The default build type is Release, which defines `NDEBUG` and
> compiles out `assert()`. To catch JIT codegen range bugs (e.g. an
> out-of-range Xtensa immediate) run an assert-enabled build:
> `cmake -B build-dbg -DCMAKE_BUILD_TYPE=Debug && cmake --build build-dbg && ctest --test-dir build-dbg`

---

## 3. QEMU virtual LCD

Runs the ESP32-S3 firmware in ESP-IDF's QEMU and shows the Game Boy
screen in a window — no physical board. QEMU has no SSD1306 model, so
the firmware streams the full 160×144 framebuffer out UART1 and a
host viewer renders it.

Prerequisite: ESP-IDF v6.x installed and `export.sh` sourced (the QEMU
binary ships with ESP-IDF).

```sh
source $IDF_PATH/export.sh
tools/qemu_lcd_run.sh
```

That builds the Heltec firmware with `-DGBJIT_QEMU_LCD=1`, launches
`idf.py qemu` (UART0 = console in the terminal, UART1 = the LCD
stream on `tcp:5556`), and opens the viewer window. Quit with
`Ctrl-A x` in the terminal or by closing the viewer.

Headless frame capture (no window):

```sh
# terminal 1: build + run qemu
cd boards/HTIT-WB32LAF_V3.2
idf.py -DGBJIT_QEMU_LCD=1 build
idf.py qemu --qemu-extra-args "-serial tcp::5556,server,nowait"

# terminal 2: dump every frame as a PPM
python3 tools/qemu_lcd.py --port 5556 --wait --save-dir /tmp/frames
```

`tools/qemu_lcd.py` is pure standard library (tkinter) — it runs under
any Python, including ESP-IDF's bundled interpreter. `--scale N` sets
the window upscale (default 3).

---

## 4. Real hardware (Heltec ESP32-S3)

Target board: `boards/HTIT-WB32LAF_V3.2` — a Heltec-style ESP32-S3
with an onboard 128×64 I²C SSD1306 OLED. Pin map and architecture are
in `boards/HTIT-WB32LAF_V3.2/README.md`.

```sh
source $IDF_PATH/export.sh                # ESP-IDF v6.x
cd boards/HTIT-WB32LAF_V3.2
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

A plain `idf.py build` produces the tuned release firmware: sync PPU
(correct BG scrolling), 1024-cycle halt step, the per-line render
cropped to the OLED's centre 128×64 window, and the OLED refreshing at
its native I²C rate. Override any knob on the command line, e.g.
`idf.py -DGBJIT_PPU_ASYNC=1 build`.

Swap the embedded ROM with `-DBOARD_ROM=blargg_06` (default is Super
Mario Land). Optional 60 fps wall-clock cap: `-DGBJIT_FRAME_LIMIT_FPS=60`.

The serial console is silent in the release build (`ERROR` log level);
build with the bench harness (next section) or re-enable INFO logs to
see the per-second fps line.

---

## 5. Benchmarks

### Board option sweep (real hardware)

`benchmark/bench_options_board.py` builds the Heltec firmware once per
compile-time toggle, flashes the board, captures 32 s of the
per-second fps log, and reports min/avg/max. Results and analysis land
in `benchmark/results/board_option_sweep.md`.

```sh
source $IDF_PATH/export.sh
python3 benchmark/bench_options_board.py        # board on /dev/ttyUSB0
```

### Tetris FPS arena sweep (real hardware)

`benchmark/fps_sweep.sh` flashes the Heltec board and measures Tetris
frames/wall-second for the interpreter baseline and JIT modes 0/1/2
(no-eviction / coldness / circular) across 32–128 KB code-cache arenas;
`benchmark/fps_plot.py` renders the avg/min plots. Results land in
`benchmark/results/tetris_arena_sweep.csv` + `bench_avg.png` / `bench_min.png`.

```sh
source $IDF_PATH/export.sh
bash benchmark/fps_sweep.sh        # board on /dev/ttyUSB0, ~40 min
python3 benchmark/fps_plot.py
```

Best measured config is coldness eviction (`-DGBJIT_JIT_EVICT=1`) at a
96 KB arena — 264 fps vs the 74 fps interpreter baseline.

### QEMU instruction-count bench

`benchmark/run_bench.sh` drives `qemu-system-xtensa` on the
`port/esp32s3` headless harness to produce per-mode Xtensa
instruction counts (interp vs JIT vs JIT-no-cache, …).

```sh
source $IDF_PATH/export.sh
benchmark/run_bench.sh
```

---

## Layout reference

| Path | What it is |
|------|------------|
| `core/` | portable SM83 interpreter, MMU, PPU timing model |
| `jit/` | Xtensa JIT — codegen, dispatcher, code cache, `xt_sim` |
| `port/host_linux/` | `gbjit_host` entry point |
| `port/esp32s3/` | headless ESP32-S3 benchmark harness |
| `boards/HTIT-WB32LAF_V3.2/` | Heltec board firmware (OLED + game) |
| `tools/` | `qemu_lcd.py` viewer, framebuffer ABX, peanut_gb golden ref |
| `benchmark/` | bench scripts + results |
| `roms/` | Blargg `cpu_instrs` sub-tests + Super Mario Land |
