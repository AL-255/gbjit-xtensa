#!/usr/bin/env bash
# Build and run the ESP32-S3 JIT target under qemu-system-xtensa.
# Expects ESP-IDF to be sourced (or IDF_PATH set + idf.py on PATH).
#
# Usage: ./scripts/run_qemu_s3.sh [--timeout SECONDS]
# Exit code: 0 if the binary prints "RESULT: PASS", 1 otherwise.

set -euo pipefail

TIMEOUT=15
if [[ "${1:-}" == "--timeout" ]]; then TIMEOUT="$2"; shift 2; fi

PORT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/port/esp32s3"
BUILD="$PORT_DIR/build"

if ! command -v idf.py >/dev/null; then
    echo "ERR: source ESP-IDF export.sh first (e.g. \`. ~/.espressif/v*/esp-idf/export.sh\`)" >&2
    exit 2
fi

(cd "$PORT_DIR" && idf.py build) >/tmp/gbjit_build.log 2>&1 \
  || { tail -30 /tmp/gbjit_build.log; exit 3; }

# Re-merge to capture the latest app bin.
rm -f "$BUILD/qemu_flash.bin"
esptool --chip=esp32s3 merge-bin \
    --output="$BUILD/qemu_flash.bin" --pad-to-size=2MB \
    --flash-mode dio --flash-freq 80m --flash-size 2MB \
    0x0     "$BUILD/bootloader/bootloader.bin" \
    0x8000  "$BUILD/partition_table/partition-table.bin" \
    0x10000 "$BUILD/gbjit_s3.bin" >/dev/null

LOG=$(mktemp)
timeout "$TIMEOUT" qemu-system-xtensa \
    -M esp32s3 -m 32M -nographic -no-reboot \
    -drive file="$BUILD/qemu_flash.bin",if=mtd,format=raw \
    -drive file="$BUILD/qemu_efuse.bin",if=none,format=raw,id=efuse \
    -global driver=nvram.esp32s3.efuse,property=drive,value=efuse \
    -global driver=timer.esp32s3.timg,property=wdt_disable,value=true \
    -serial mon:stdio 2>&1 | tee "$LOG" | grep -E '(gbjit|Guru|panic|RESULT)' || true

if grep -q 'RESULT: PASS' "$LOG"; then
    rm -f "$LOG"
    exit 0
fi
echo
echo "FAILED — full log in $LOG"
exit 1
