#!/usr/bin/env bash
# Investigate the prefetcher's actual win: bench the same SML workload
# with prefetch_depth ∈ {0, 1, 2, 4, 8} at a small + medium + large
# arena, and dump throughput + blocks_compiled + prefetched counters.
#
# 5 depths × 3 arenas = 15 builds. Parallel idf.py builds, then one
# qemu per host CPU via taskset so virtual-timer readings are clean.
#
# Output: benchmark/results/prefetch_sweep.csv.

set -euo pipefail

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT_DIR="$BENCH_DIR/../port/esp32s3"
RESULTS_DIR="$BENCH_DIR/results"
ROM="${1:-sml}"
BUDGET="${2:-${BENCH_CYCLES_BUDGET:-1000000}}"

ARENAS=( 48 96 192 )
DEPTHS=( 0 1 2 4 8 )
NPROC=$(nproc)

mkdir -p "$RESULTS_DIR"
CSV="$RESULTS_DIR/prefetch_sweep.csv"
TMP_BUILDS="/tmp/gbjit_pf_sweep"
mkdir -p "$TMP_BUILDS"

if ! command -v qemu-system-xtensa >/dev/null; then
    echo "ERR: qemu-system-xtensa not on PATH (source IDF export.sh)" >&2; exit 2
fi

# Use the existing efuse from the cache-size sweep if present.
EFUSE_CACHE="/tmp/gbjit_sweep/qemu_efuse.bin"
if [[ ! -f "$EFUSE_CACHE" ]]; then
    EFUSE_CACHE="$TMP_BUILDS/qemu_efuse.bin"
    (cd "$PORT_DIR" && timeout 10 idf.py qemu >/dev/null 2>&1 || true)
    cp "$PORT_DIR/build/qemu_efuse.bin" "$EFUSE_CACHE"
fi

build_one() {
    local KB="$1" DEPTH="$2"
    local TAG="d${DEPTH}_${KB}kb"
    local BUILD_DIR="$TMP_BUILDS/${TAG}"
    local LOG="$TMP_BUILDS/${TAG}.log"

    echo "[build] $TAG" >&2
    (cd "$PORT_DIR" && \
        idf.py -B "$BUILD_DIR" \
               -DBENCH_CYCLES_BUDGET="$BUDGET" -DBENCH_ROM="$ROM" \
               -DGBJIT_PPU_ASYNC=0 -DBENCH_MODE_JIT_ONLY=1 \
               -DGBJIT_ARENA_KB="$KB" -DBENCH_PREFETCH_DEPTH="$DEPTH" build) \
        >"$LOG" 2>&1 \
        || { tail -10 "$LOG"; echo "[build] $TAG FAILED" >&2; return 1; }

    rm -f "$BUILD_DIR/qemu_flash.bin"
    esptool --chip=esp32s3 merge-bin \
        --output="$BUILD_DIR/qemu_flash.bin" --pad-to-size=2MB \
        --flash-mode dio --flash-freq 80m --flash-size 2MB \
        0x0     "$BUILD_DIR/bootloader/bootloader.bin" \
        0x8000  "$BUILD_DIR/partition_table/partition-table.bin" \
        0x10000 "$BUILD_DIR/gbjit_s3.bin" >/dev/null 2>&1
    cp "$EFUSE_CACHE" "$BUILD_DIR/qemu_efuse.bin"
}

run_one_qemu() {
    local CPU="$1" KB="$2" DEPTH="$3"
    local TAG="d${DEPTH}_${KB}kb"
    local BUILD_DIR="$TMP_BUILDS/${TAG}"
    local PARTIAL="$TMP_BUILDS/${TAG}.csv"

    if [[ ! -f "$BUILD_DIR/qemu_flash.bin" ]]; then
        echo "$ROM,$KB,$DEPTH,,,,,,," >"$PARTIAL"; return
    fi

    local BENCH_LINE
    BENCH_LINE="$(timeout 240 taskset -c "$CPU" qemu-system-xtensa \
        -M esp32s3 -m 32M -nographic -no-reboot \
        -drive "file=$BUILD_DIR/qemu_flash.bin,if=mtd,format=raw" \
        -drive "file=$BUILD_DIR/qemu_efuse.bin,if=none,format=raw,id=efuse" \
        -global driver=nvram.esp32s3.efuse,property=drive,value=efuse \
        -global driver=timer.esp32s3.timg,property=wdt_disable,value=true \
        -serial mon:stdio 2>&1 | grep -m1 '\[BENCH\]' || true)"

    if [[ -z "$BENCH_LINE" ]]; then
        echo "[qemu cpu=$CPU] $TAG: no BENCH line" >&2
        echo "$ROM,$KB,$DEPTH,,,,,,," >"$PARTIAL"; return
    fi
    local MHZ=$(echo "$BENCH_LINE" | grep -oE 'mhz=[0-9.]+' | grep -oE '[0-9.]+')
    echo "[qemu cpu=$CPU] $TAG: ${MHZ} MHz" >&2

    local DMG=$(echo "$BENCH_LINE" | grep -oE 'dmg_x=[0-9.]+' | grep -oE '[0-9.]+')
    local BC=$(echo "$BENCH_LINE" | grep -oE 'blocks_compiled=[0-9]+' | grep -oE '[0-9]+')
    local BE=$(echo "$BENCH_LINE" | grep -oE 'blocks_executed=[0-9]+' | grep -oE '[0-9]+')
    local CH=$(echo "$BENCH_LINE" | grep -oE 'chain_hits=[0-9]+' | grep -oE '[0-9]+')
    local CM=$(echo "$BENCH_LINE" | grep -oE 'chain_misses=[0-9]+' | grep -oE '[0-9]+')
    local PF=$(echo "$BENCH_LINE" | grep -oE 'prefetched=[0-9]+' | grep -oE '[0-9]+')

    echo "$ROM,$KB,$DEPTH,$MHZ,$DMG,$BC,$BE,$CH,$CM,$PF" >"$PARTIAL"
}
export -f run_one_qemu
export TMP_BUILDS ROM

echo "[sweep] phase 1 — parallel builds"
for KB in "${ARENAS[@]}"; do
    for D in "${DEPTHS[@]}"; do
        build_one "$KB" "$D" &
    done
done
wait

echo "[sweep] phase 2 — qemu runs ($NPROC workers, one per CPU)"
JOBLIST="$TMP_BUILDS/joblist.txt"; : > "$JOBLIST"
i=0
for KB in "${ARENAS[@]}"; do
    for D in "${DEPTHS[@]}"; do
        echo "$((i % NPROC)) $KB $D" >> "$JOBLIST"
        i=$((i + 1))
    done
done
xargs -a "$JOBLIST" -n 3 -P "$NPROC" bash -c 'run_one_qemu "$@"' _

echo "rom,arena_kb,prefetch_depth,mhz,dmg_x,blocks_compiled,blocks_executed,chain_hits,chain_misses,prefetched" > "$CSV"
for KB in "${ARENAS[@]}"; do
    for D in "${DEPTHS[@]}"; do
        [[ -f "$TMP_BUILDS/d${D}_${KB}kb.csv" ]] && cat "$TMP_BUILDS/d${D}_${KB}kb.csv" >>"$CSV"
    done
done

echo "[sweep] done — see $CSV"
column -ts, "$CSV"
