#!/usr/bin/env bash
# Sweep JIT codecache arena sizes through qemu-xtensa in parallel.
#
# Each arena size gets its own out-of-tree build dir under /tmp so the
# IDF builds (and the resulting qemu_flash.bin) don't step on each
# other. All sizes run concurrently — the host has 32 cores and each
# IDF build saturates at most a few of them.
#
# Output: benchmark/results/cache_sweep.csv (the plot script reads it).
#
# Usage:
#   . $IDF_PATH/export.sh                          # IDF + qemu-xtensa on PATH
#   ./benchmark/run_cache_sweep.sh [rom] [budget]  # default rom=sml, budget=1M

set -euo pipefail

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT_DIR="$BENCH_DIR/../port/esp32s3"
RESULTS_DIR="$BENCH_DIR/results"
ROM="${1:-sml}"
BUDGET="${2:-${BENCH_CYCLES_BUDGET:-1000000}}"

# Arena sizes (KB) to sweep. < 8 KB risks the arena filling before
# SML's hot blocks are all compiled; > 128 KB trips the latent codegen
# bug for ROMs with > 100 unique blocks (Blargg 06) but is fine for
# SML, which compiles ~55-77 blocks.
ARENAS=( 4 8 16 32 48 64 96 128 192 )

mkdir -p "$RESULTS_DIR"
CSV="$RESULTS_DIR/cache_sweep.csv"
TMP_BUILDS="/tmp/gbjit_sweep"
mkdir -p "$TMP_BUILDS"

if ! command -v qemu-system-xtensa >/dev/null; then
    echo "ERR: qemu-system-xtensa not on PATH (source IDF export.sh)" >&2
    exit 2
fi

# Cache the qemu efuse — same across all arena sizes, generated once.
EFUSE_CACHE="$TMP_BUILDS/qemu_efuse.bin"
if [[ ! -f "$EFUSE_CACHE" ]]; then
    echo "[sweep] generating qemu_efuse.bin (one-shot idf.py qemu)..." >&2
    (cd "$PORT_DIR" && timeout 10 idf.py qemu >/dev/null 2>&1 || true)
    if [[ -f "$PORT_DIR/build/qemu_efuse.bin" ]]; then
        cp "$PORT_DIR/build/qemu_efuse.bin" "$EFUSE_CACHE"
    else
        echo "ERR: failed to generate qemu_efuse.bin" >&2; exit 5
    fi
fi

run_one() {
    local KB="$1"
    local BUILD_DIR="$TMP_BUILDS/${KB}kb"
    local LOG="$TMP_BUILDS/${KB}kb.log"
    local PARTIAL="$TMP_BUILDS/${KB}kb.csv"

    echo "[sweep] arena=${KB}KB rom=${ROM} budget=${BUDGET}..." >&2
    # idf.py honours -B for the build dir. Per-arena dirs let us run all
    # configurations concurrently without write conflicts.
    (cd "$PORT_DIR" && \
        idf.py -B "$BUILD_DIR" \
               -DBENCH_CYCLES_BUDGET="$BUDGET" -DBENCH_ROM="$ROM" \
               -DBENCH_MODE_JIT_ONLY=1 -DGBJIT_ARENA_KB="$KB" build) \
        >"$LOG" 2>&1 \
        || { tail -30 "$LOG"; echo "[sweep] arena=${KB}KB build failed" >&2; \
             echo "$ROM,$KB,,,,,,,," >"$PARTIAL"; return; }

    # Stage qemu_flash.bin + efuse into the per-arena build dir.
    rm -f "$BUILD_DIR/qemu_flash.bin"
    esptool --chip=esp32s3 merge-bin \
        --output="$BUILD_DIR/qemu_flash.bin" --pad-to-size=2MB \
        --flash-mode dio --flash-freq 80m --flash-size 2MB \
        0x0     "$BUILD_DIR/bootloader/bootloader.bin" \
        0x8000  "$BUILD_DIR/partition_table/partition-table.bin" \
        0x10000 "$BUILD_DIR/gbjit_s3.bin" >/dev/null 2>&1
    cp "$EFUSE_CACHE" "$BUILD_DIR/qemu_efuse.bin"

    local BENCH_LINE
    BENCH_LINE="$(timeout 240 qemu-system-xtensa \
        -M esp32s3 -m 32M -nographic -no-reboot \
        -drive "file=$BUILD_DIR/qemu_flash.bin,if=mtd,format=raw" \
        -drive "file=$BUILD_DIR/qemu_efuse.bin,if=none,format=raw,id=efuse" \
        -global driver=nvram.esp32s3.efuse,property=drive,value=efuse \
        -global driver=timer.esp32s3.timg,property=wdt_disable,value=true \
        -serial mon:stdio 2>&1 | grep -m1 '\[BENCH\] mode=jit' || true)"

    if [[ -z "$BENCH_LINE" ]]; then
        echo "[sweep] arena=${KB}KB: no BENCH line (qemu crash/timeout)" >&2
        echo "$ROM,$KB,,,,,,,," >"$PARTIAL"
        return
    fi
    echo "[sweep] arena=${KB}KB OK" >&2

    local CYC=$(echo "$BENCH_LINE" | grep -oE 'cycles=[0-9]+' | grep -oE '[0-9]+')
    local ELAPSED=$(echo "$BENCH_LINE" | grep -oE 'elapsed_us=[0-9]+' | grep -oE '[0-9]+')
    local MHZ=$(echo "$BENCH_LINE" | grep -oE 'mhz=[0-9.]+' | grep -oE '[0-9.]+')
    local DMG=$(echo "$BENCH_LINE" | grep -oE 'dmg_x=[0-9.]+' | grep -oE '[0-9.]+')
    local BC=$(echo "$BENCH_LINE" | grep -oE 'blocks_compiled=[0-9]+' | grep -oE '[0-9]+')
    local BE=$(echo "$BENCH_LINE" | grep -oE 'blocks_executed=[0-9]+' | grep -oE '[0-9]+')
    local CH=$(echo "$BENCH_LINE" | grep -oE 'chain_hits=[0-9]+' | grep -oE '[0-9]+')
    local CM=$(echo "$BENCH_LINE" | grep -oE 'chain_misses=[0-9]+' | grep -oE '[0-9]+')

    echo "$ROM,$KB,$CYC,$ELAPSED,$MHZ,$DMG,$BC,$BE,$CH,$CM" >"$PARTIAL"
}

# Also need an interp baseline for the plot's hline — one extra build,
# launched alongside the sweep with arena=0 sentinel.
run_interp() {
    local BUILD_DIR="$TMP_BUILDS/interp"
    local LOG="$TMP_BUILDS/interp.log"
    local PARTIAL="$TMP_BUILDS/interp.csv"

    echo "[sweep] interp baseline..." >&2
    (cd "$PORT_DIR" && \
        idf.py -B "$BUILD_DIR" \
               -DBENCH_CYCLES_BUDGET="$BUDGET" -DBENCH_ROM="$ROM" \
               -DBENCH_MODE_INTERP_ONLY=1 build) \
        >"$LOG" 2>&1 \
        || { tail -30 "$LOG"; echo "$ROM,0,,,,,,,," >"$PARTIAL"; return; }

    rm -f "$BUILD_DIR/qemu_flash.bin"
    esptool --chip=esp32s3 merge-bin \
        --output="$BUILD_DIR/qemu_flash.bin" --pad-to-size=2MB \
        --flash-mode dio --flash-freq 80m --flash-size 2MB \
        0x0     "$BUILD_DIR/bootloader/bootloader.bin" \
        0x8000  "$BUILD_DIR/partition_table/partition-table.bin" \
        0x10000 "$BUILD_DIR/gbjit_s3.bin" >/dev/null 2>&1
    cp "$EFUSE_CACHE" "$BUILD_DIR/qemu_efuse.bin"

    local BENCH_LINE
    BENCH_LINE="$(timeout 240 qemu-system-xtensa \
        -M esp32s3 -m 32M -nographic -no-reboot \
        -drive "file=$BUILD_DIR/qemu_flash.bin,if=mtd,format=raw" \
        -drive "file=$BUILD_DIR/qemu_efuse.bin,if=none,format=raw,id=efuse" \
        -global driver=nvram.esp32s3.efuse,property=drive,value=efuse \
        -global driver=timer.esp32s3.timg,property=wdt_disable,value=true \
        -serial mon:stdio 2>&1 | grep -m1 '\[BENCH\] mode=interp' || true)"

    if [[ -z "$BENCH_LINE" ]]; then
        echo "[sweep] interp baseline: no BENCH line" >&2
        echo "$ROM,0,,,,,,,," >"$PARTIAL"
        return
    fi
    echo "[sweep] interp baseline OK" >&2

    local CYC=$(echo "$BENCH_LINE" | grep -oE 'cycles=[0-9]+' | grep -oE '[0-9]+')
    local ELAPSED=$(echo "$BENCH_LINE" | grep -oE 'elapsed_us=[0-9]+' | grep -oE '[0-9]+')
    local MHZ=$(echo "$BENCH_LINE" | grep -oE 'mhz=[0-9.]+' | grep -oE '[0-9.]+')
    local DMG=$(echo "$BENCH_LINE" | grep -oE 'dmg_x=[0-9.]+' | grep -oE '[0-9.]+')
    # arena_kb=0 is the sentinel for interp baseline in the CSV.
    echo "$ROM,0,$CYC,$ELAPSED,$MHZ,$DMG,,,," >"$PARTIAL"
}

# Fan out everything in parallel.
for KB in "${ARENAS[@]}"; do
    run_one "$KB" &
done
run_interp &
wait
echo "[sweep] all jobs finished, merging CSV"

# Merge partials in arena_kb order. Interp row first (sentinel 0) so the
# plot script can find it.
echo "rom,arena_kb,gb_cycles,elapsed_us,mhz,dmg_x,blocks_compiled,blocks_executed,chain_hits,chain_misses" > "$CSV"
if [[ -f "$TMP_BUILDS/interp.csv" ]]; then cat "$TMP_BUILDS/interp.csv" >>"$CSV"; fi
for KB in "${ARENAS[@]}"; do
    [[ -f "$TMP_BUILDS/${KB}kb.csv" ]] && cat "$TMP_BUILDS/${KB}kb.csv" >>"$CSV"
done

echo "[sweep] done — see $CSV"
echo "[sweep] generate plot: python3 $BENCH_DIR/plot_cache_sweep.py"
