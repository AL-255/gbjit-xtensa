#!/usr/bin/env bash
# Sweep {JIT codecache arena size} x {dispatcher mode} through qemu-
# xtensa. Two phases:
#   1) Parallel idf.py builds (CPU-bound, contention doesn't matter).
#   2) Parallel qemu runs with PER-CPU AFFINITY (taskset -c <cpu>) and
#      a worker-pool concurrency = nproc, so each CPU runs at most one
#      qemu at a time. When a job finishes, that CPU picks up the next
#      pending one. This keeps qemu's wall-clock-tied virtual timer
#      uncontended, so the throughput readings are stable.
#
# Modes:
#   - jit            cached, cold start
#   - jit_warm       cached, pre-compiled in a discarded warm-up pass
#   - jit_noprefetch cached, skips static-successor prefetch
#   - interp         reference baseline (arena-independent)
#
# Output: benchmark/results/cache_sweep.csv. plot_cache_sweep.py reads
# it and renders one line per mode.
#
# Usage:
#   . $IDF_PATH/export.sh
#   ./benchmark/run_cache_sweep.sh [rom] [budget]    # default sml, 1M

set -euo pipefail

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT_DIR="$BENCH_DIR/../port/esp32s3"
RESULTS_DIR="$BENCH_DIR/results"
ROM="${1:-sml}"
BUDGET="${2:-${BENCH_CYCLES_BUDGET:-1000000}}"

ARENAS=( 4 8 16 32 48 64 96 128 192 )
JIT_MODES=( jit jit_warm jit_noprefetch )
NPROC=$(nproc)

mkdir -p "$RESULTS_DIR"
CSV="$RESULTS_DIR/cache_sweep.csv"
TMP_BUILDS="/tmp/gbjit_sweep"
mkdir -p "$TMP_BUILDS"

if ! command -v qemu-system-xtensa >/dev/null; then
    echo "ERR: qemu-system-xtensa not on PATH (source IDF export.sh)" >&2
    exit 2
fi

EFUSE_CACHE="$TMP_BUILDS/qemu_efuse.bin"
if [[ ! -f "$EFUSE_CACHE" ]]; then
    echo "[sweep] generating qemu_efuse.bin..." >&2
    (cd "$PORT_DIR" && timeout 10 idf.py qemu >/dev/null 2>&1 || true)
    if [[ -f "$PORT_DIR/build/qemu_efuse.bin" ]]; then
        cp "$PORT_DIR/build/qemu_efuse.bin" "$EFUSE_CACHE"
    else
        echo "ERR: failed to generate qemu_efuse.bin" >&2; exit 5
    fi
fi

mode_define() {
    case "$1" in
        jit)            echo "BENCH_MODE_JIT_ONLY=1" ;;
        jit_warm)       echo "BENCH_MODE_JIT_WARM_ONLY=1" ;;
        jit_noprefetch) echo "BENCH_MODE_JIT_NOPREFETCH_ONLY=1" ;;
        interp)         echo "BENCH_MODE_INTERP_ONLY=1" ;;
        *) echo "ERR: unknown mode $1" >&2; exit 1 ;;
    esac
}

build_one() {
    local MODE="$1" KB="$2"
    local TAG="${MODE}_${KB}kb"
    local BUILD_DIR="$TMP_BUILDS/${TAG}"
    local LOG="$TMP_BUILDS/${TAG}.log"
    local DEFINE_FLAG; DEFINE_FLAG="-D$(mode_define "$MODE")"

    echo "[build] $TAG..." >&2
    # GBJIT_PPU_ASYNC=0: keep the PPU on Core 0 so JIT block-compile
    # order is deterministic. Async PPU + qemu dual-core would give
    # run-to-run jitter we don't want in this measurement.
    (cd "$PORT_DIR" && \
        idf.py -B "$BUILD_DIR" \
               -DBENCH_CYCLES_BUDGET="$BUDGET" -DBENCH_ROM="$ROM" \
               -DGBJIT_PPU_ASYNC=0 \
               $DEFINE_FLAG -DGBJIT_ARENA_KB="$KB" build) \
        >"$LOG" 2>&1 \
        || { tail -10 "$LOG"; echo "[build] $TAG: FAILED" >&2; return 1; }

    rm -f "$BUILD_DIR/qemu_flash.bin"
    esptool --chip=esp32s3 merge-bin \
        --output="$BUILD_DIR/qemu_flash.bin" --pad-to-size=2MB \
        --flash-mode dio --flash-freq 80m --flash-size 2MB \
        0x0     "$BUILD_DIR/bootloader/bootloader.bin" \
        0x8000  "$BUILD_DIR/partition_table/partition-table.bin" \
        0x10000 "$BUILD_DIR/gbjit_s3.bin" >/dev/null 2>&1
    cp "$EFUSE_CACHE" "$BUILD_DIR/qemu_efuse.bin"
}

# run_one_qemu: invoked by the worker pool (xargs -P) with three args:
#   $1 = cpu_id (taskset target)
#   $2 = mode
#   $3 = arena KB
# Exported so the xargs sub-shell can find it via `declare -f`.
run_one_qemu() {
    local CPU="$1" MODE="$2" KB="$3"
    local TAG="${MODE}_${KB}kb"
    local BUILD_DIR="$TMP_BUILDS/${TAG}"
    local PARTIAL="$TMP_BUILDS/${TAG}.csv"

    if [[ ! -f "$BUILD_DIR/qemu_flash.bin" ]]; then
        echo "$ROM,$MODE,$KB,,,,,,,," >"$PARTIAL"
        return
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
        echo "$ROM,$MODE,$KB,,,,,,,," >"$PARTIAL"
        return
    fi
    local MHZ_VAL; MHZ_VAL=$(echo "$BENCH_LINE" | grep -oE 'mhz=[0-9.]+' | grep -oE '[0-9.]+')
    echo "[qemu cpu=$CPU] $TAG: ${MHZ_VAL} MHz" >&2

    local CYC=$(echo "$BENCH_LINE" | grep -oE 'cycles=[0-9]+' | grep -oE '[0-9]+')
    local ELAPSED=$(echo "$BENCH_LINE" | grep -oE 'elapsed_us=[0-9]+' | grep -oE '[0-9]+')
    local DMG=$(echo "$BENCH_LINE" | grep -oE 'dmg_x=[0-9.]+' | grep -oE '[0-9.]+')
    local BC=$(echo "$BENCH_LINE" | grep -oE 'blocks_compiled=[0-9]+' | grep -oE '[0-9]+')
    local BE=$(echo "$BENCH_LINE" | grep -oE 'blocks_executed=[0-9]+' | grep -oE '[0-9]+')
    local CH=$(echo "$BENCH_LINE" | grep -oE 'chain_hits=[0-9]+' | grep -oE '[0-9]+')
    local CM=$(echo "$BENCH_LINE" | grep -oE 'chain_misses=[0-9]+' | grep -oE '[0-9]+')

    echo "$ROM,$MODE,$KB,$CYC,$ELAPSED,$MHZ_VAL,$DMG,$BC,$BE,$CH,$CM" >"$PARTIAL"
}
export -f run_one_qemu

# Export the env the worker function needs.
export TMP_BUILDS ROM PORT_DIR

# --- Phase 1: parallel builds.
echo "[sweep] phase 1 — parallel builds" >&2
for MODE in "${JIT_MODES[@]}"; do
    for KB in "${ARENAS[@]}"; do
        build_one "$MODE" "$KB" &
    done
done
build_one interp 0 &
wait
echo "[sweep] phase 1 done" >&2

# --- Phase 2: worker pool of size NPROC; each job pinned to its
# assigned CPU via taskset -c. xargs -P NPROC keeps at most NPROC qemus
# alive. CPU assignment is round-robin over the job list (i % NPROC),
# so when jobs > NPROC the queued ones are pinned to the same CPUs as
# earlier ones and naturally serialise on a CPU as xargs pulls from
# the queue.
echo "[sweep] phase 2 — qemu runs ($NPROC workers, one per CPU)" >&2

JOBLIST="$TMP_BUILDS/joblist.txt"
: > "$JOBLIST"
i=0
echo "$((i % NPROC)) interp 0" >> "$JOBLIST"; i=$((i+1))
for MODE in "${JIT_MODES[@]}"; do
    for KB in "${ARENAS[@]}"; do
        echo "$((i % NPROC)) $MODE $KB" >> "$JOBLIST"
        i=$((i+1))
    done
done

xargs -a "$JOBLIST" -n 3 -P "$NPROC" bash -c 'run_one_qemu "$@"' _

# --- Phase 3: merge partials.
echo "rom,mode,arena_kb,gb_cycles,elapsed_us,mhz,dmg_x,blocks_compiled,blocks_executed,chain_hits,chain_misses" > "$CSV"
if [[ -f "$TMP_BUILDS/interp_0kb.csv" ]]; then cat "$TMP_BUILDS/interp_0kb.csv" >>"$CSV"; fi
for MODE in "${JIT_MODES[@]}"; do
    for KB in "${ARENAS[@]}"; do
        [[ -f "$TMP_BUILDS/${MODE}_${KB}kb.csv" ]] && cat "$TMP_BUILDS/${MODE}_${KB}kb.csv" >>"$CSV"
    done
done

echo "[sweep] done — see $CSV"
echo "[sweep] generate plot: python3 $BENCH_DIR/plot_cache_sweep.py"
