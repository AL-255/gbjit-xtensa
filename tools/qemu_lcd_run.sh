#!/usr/bin/env bash
#
# Build the Heltec board firmware in QEMU virtual-LCD mode, launch it
# under ESP-IDF's QEMU, and open the host-side LCD viewer.
#
#   tools/qemu_lcd_run.sh [extra qemu_lcd.py args...]
#
# The firmware (GBJIT_QEMU_LCD=1) streams the full 160x144 Game Boy
# framebuffer out UART1. ESP-IDF's `idf.py qemu` maps UART0 to the
# terminal (console/monitor) and the appended `-serial` to UART1 — a
# TCP chardev the viewer connects to. Everything runs on the
# IDF-shipped QEMU, so a reproduction only needs a working ESP-IDF.
#
# Quit by exiting QEMU (Ctrl-A x) or closing the viewer window.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BOARD="$ROOT/boards/HTIT-WB32LAF_V3.2"
PORT="${GBJIT_QEMU_LCD_PORT:-5556}"

# Bring ESP-IDF onto PATH if it isn't already.
if ! command -v idf.py >/dev/null 2>&1; then
    : "${IDF_PATH:?ESP-IDF not on PATH — source \$IDF_PATH/export.sh first}"
    # shellcheck disable=SC1091
    . "$IDF_PATH/export.sh" >/dev/null 2>&1
fi

cd "$BOARD"
echo "[qemu-lcd] building firmware (GBJIT_QEMU_LCD=1) …"
idf.py -DGBJIT_QEMU_LCD=1 build

echo "[qemu-lcd] starting viewer (waits for QEMU on tcp:$PORT) …"
python3 "$ROOT/tools/qemu_lcd.py" --port "$PORT" --wait "$@" &
VIEWER_PID=$!
trap 'kill $VIEWER_PID 2>/dev/null || true' EXIT

echo "[qemu-lcd] launching QEMU — UART0=console, UART1=tcp:$PORT"
echo "[qemu-lcd] quit with Ctrl-A x (in this terminal) or close the viewer"
idf.py qemu --qemu-extra-args "-serial tcp::$PORT,server,nowait"
