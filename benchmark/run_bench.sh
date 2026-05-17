#!/usr/bin/env bash
# Drives qemu-system-xtensa to produce per-mode Xtensa instruction counts.
#
# Builds three firmware variants:
#   interp_only — app_main runs only the SM83 reference interpreter
#   jit_only    — app_main runs only the JIT dispatcher
#   both        — runs interp then jit (used only for sanity checking)
#
# For each variant we:
#   - Run qemu untraced for a clean throughput number ([BENCH] line).
#   - Run qemu with `-d in_asm,exec,nochain` and post-process the trace.
#
# The output table per mode includes:
#   - GB cycles consumed
#   - Wall time (firmware-reported, simulated)
#   - Throughput (MHz of T-cycles, × DMG real-time)
#   - Xtensa TBs executed
#   - Xtensa instructions executed
#   - Load / store instructions (memory-fetch proxy)
#
# Adjust BENCH_CYCLES_BUDGET below if traces are too large or too small.

set -euo pipefail

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESULTS_DIR="$BENCH_DIR/results"
PORT_DIR="$BENCH_DIR/../port/esp32s3"
BUILD_DIR="$PORT_DIR/build"
BUDGET="${BENCH_CYCLES_BUDGET:-200000}"

if ! command -v qemu-system-xtensa >/dev/null; then
    echo "ERR: source ESP-IDF export.sh first" >&2
    exit 2
fi

mkdir -p "$RESULTS_DIR"

build_firmware() {
    local mode="$1"
    local extra_def=""
    case "$mode" in
        interp_only)      extra_def="-DBENCH_MODE_INTERP_ONLY=1" ;;
        jit_only)         extra_def="-DBENCH_MODE_JIT_ONLY=1" ;;
        jit_warm_only)    extra_def="-DBENCH_MODE_JIT_WARM_ONLY=1" ;;
        jit_nocache_only) extra_def="-DBENCH_MODE_JIT_NOCACHE_ONLY=1" ;;
        both)             extra_def="" ;;
        *) echo "ERR: unknown mode $mode" >&2; exit 1 ;;
    esac
    echo "[bench] building firmware: $mode (budget=$BUDGET)..." >&2
    (cd "$PORT_DIR" && idf.py -DBENCH_CYCLES_BUDGET="$BUDGET" $extra_def fullclean >/dev/null \
                   && idf.py -DBENCH_CYCLES_BUDGET="$BUDGET" $extra_def build) >/tmp/gbjit_bench_build.log 2>&1 \
        || { tail -30 /tmp/gbjit_bench_build.log; exit 3; }
    # Generate qemu_efuse.bin via a short `idf.py qemu` (cleanly killed).
    (cd "$PORT_DIR" && timeout 3 idf.py qemu >/dev/null 2>&1 || true)
    rm -f "$BUILD_DIR/qemu_flash.bin"
    esptool --chip=esp32s3 merge-bin \
        --output="$BUILD_DIR/qemu_flash.bin" --pad-to-size=2MB \
        --flash-mode dio --flash-freq 80m --flash-size 2MB \
        0x0     "$BUILD_DIR/bootloader/bootloader.bin" \
        0x8000  "$BUILD_DIR/partition_table/partition-table.bin" \
        0x10000 "$BUILD_DIR/gbjit_s3.bin" >/dev/null
}

QEMU_BASE_ARGS=(
    -M esp32s3 -m 32M -nographic -no-reboot
    -drive "file=$BUILD_DIR/qemu_flash.bin,if=mtd,format=raw"
    -drive "file=$BUILD_DIR/qemu_efuse.bin,if=none,format=raw,id=efuse"
    -global driver=nvram.esp32s3.efuse,property=drive,value=efuse
    -global driver=timer.esp32s3.timg,property=wdt_disable,value=true
    -serial mon:stdio
)

run_pass() {
    local mode="$1"
    local lines_log="$RESULTS_DIR/${mode}_bench_lines.txt"
    local trace_log="$RESULTS_DIR/${mode}_qemu_trace.log"

    echo "[bench] $mode: untraced timing run..." >&2
    timeout 60 qemu-system-xtensa "${QEMU_BASE_ARGS[@]}" 2>&1 \
        | grep '\[BENCH\]' > "$lines_log" || true

    echo "[bench] $mode: tracing with -d in_asm,exec,nochain..." >&2
    rm -f "$trace_log"
    # 60s is generous — once app_main reaches its vTaskDelay suspend, the
    # idle task's WAITI keeps qemu effectively quiet and the trace file
    # stops growing.
    timeout 60 qemu-system-xtensa "${QEMU_BASE_ARGS[@]}" \
        -d in_asm,exec,nochain -D "$trace_log" \
        > "$RESULTS_DIR/${mode}_qemu_stdout.txt" 2>&1 || true

    if [[ ! -s "$trace_log" ]]; then
        echo "ERR: $mode produced no trace" >&2; exit 4
    fi
    echo "[bench] $mode: trace size = $(du -h "$trace_log" | awk '{print $1}')"
}

# --- Run all four modes, separately, with rebuilds in between. ----------
for mode in interp_only jit_nocache_only jit_only jit_warm_only; do
    build_firmware "$mode"
    run_pass "$mode"
done

# --- Analyse each. -------------------------------------------------------
"$BENCH_DIR/analyze.py" "$RESULTS_DIR/interp_only_qemu_trace.log" \
    "$RESULTS_DIR/interp_only_bench_lines.txt" \
    --label "interp" --out "$RESULTS_DIR/interp.md"

"$BENCH_DIR/analyze.py" "$RESULTS_DIR/jit_only_qemu_trace.log" \
    "$RESULTS_DIR/jit_only_bench_lines.txt" \
    --label "jit" --out "$RESULTS_DIR/jit.md"

# Combine into a single summary.
"$BENCH_DIR/combine.py" \
    interp        "$RESULTS_DIR/interp_only_bench_lines.txt"      "$RESULTS_DIR/interp_only_qemu_trace.log" \
    jit_nocache   "$RESULTS_DIR/jit_nocache_only_bench_lines.txt" "$RESULTS_DIR/jit_nocache_only_qemu_trace.log" \
    jit           "$RESULTS_DIR/jit_only_bench_lines.txt"         "$RESULTS_DIR/jit_only_qemu_trace.log" \
    jit_warm      "$RESULTS_DIR/jit_warm_only_bench_lines.txt"    "$RESULTS_DIR/jit_warm_only_qemu_trace.log" \
    > "$RESULTS_DIR/summary.md"

echo "[bench] done — see $RESULTS_DIR/summary.md"
