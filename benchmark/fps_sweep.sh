#!/usr/bin/env bash
# Tetris FPS benchmark on the HTIT-WB32LAF_V3.2 board: interpreter
# baseline + JIT modes 0/1/2 (no-evict / coldness / circular) across a
# range of code-cache arena sizes. Builds, flashes and measures each
# point; writes benchmark/results/tetris_arena_sweep.csv. Plot it with
# fps_plot.py. Run from the repo root with the ESP-IDF env exported and
# the board on $PORT; needs a GBJIT_FPS_PROBE-capable firmware.
set -u
BD=boards/HTIT-WB32LAF_V3.2
PORT=${PORT:-/dev/ttyUSB0}
CSV=benchmark/results/tetris_arena_sweep.csv
ARENAS="32 48 64 80 96 128"
echo "mode,arena_kb,avg_fps,min_fps" > "$CSV"

measure() {                       # echoes "avg min" from the FPS probe
    stty -F "$PORT" 115200 raw -echo 2>/dev/null
    timeout 47 cat "$PORT" > /tmp/fps_m.log 2>&1
    grep -aoE 'avg=[0-9]+ min=[0-9]+' /tmp/fps_m.log | tail -1 \
        | grep -oE '[0-9]+' | tr '\n' ' '
}

# interpreter baseline (arena size irrelevant)
idf.py -C "$BD" -DGBJIT_FPS_PROBE=1 -DBOARD_ROM=tetris \
       -DGBJIT_BOARD_INTERP=1 -DGBJIT_JIT_EVICT=0 -DGBJIT_ARENA_KB=64 \
       build >/tmp/fps_bld.log 2>&1 \
  && idf.py -C "$BD" -p "$PORT" -b 921600 flash >/dev/null 2>&1
read AVG MIN <<< "$(measure)"
echo "interp,0,${AVG:-0},${MIN:-0}" | tee -a "$CSV"

for MODE in 0 1 2; do
  for KB in $ARENAS; do
    idf.py -C "$BD" -DGBJIT_FPS_PROBE=1 -DBOARD_ROM=tetris \
           -DGBJIT_BOARD_INTERP=0 -DGBJIT_JIT_EVICT=$MODE \
           -DGBJIT_ARENA_KB=$KB build >/tmp/fps_bld.log 2>&1 \
      && idf.py -C "$BD" -p "$PORT" -b 921600 flash >/dev/null 2>&1
    read AVG MIN <<< "$(measure)"
    echo "mode${MODE},${KB},${AVG:-0},${MIN:-0}" | tee -a "$CSV"
  done
done
echo "BENCH COMPLETE"
