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
# ESP-IDF is found automatically: if idf.py isn't already on PATH the
# script sources $IDF_PATH/export.sh, or the newest
# ~/.espressif/*/esp-idf/export.sh — so a bare `tools/qemu_lcd_run.sh`
# works from a fresh shell.
#
# Quit by exiting QEMU (Ctrl-A x) or closing the viewer window.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BOARD="$ROOT/boards/HTIT-WB32LAF_V3.2"
PORT="${GBJIT_QEMU_LCD_PORT:-5556}"

# Bring ESP-IDF onto PATH if it isn't already. Try, in order:
#   1. idf.py already on PATH (nothing to do)
#   2. $IDF_PATH/export.sh, if IDF_PATH is set
#   3. auto-discovered ~/.espressif/*/esp-idf/export.sh (newest version first)
if ! command -v idf.py >/dev/null 2>&1; then
    idf_export=""
    if [[ -n "${IDF_PATH:-}" && -f "$IDF_PATH/export.sh" ]]; then
        idf_export="$IDF_PATH/export.sh"
    else
        for cand in $(ls -d "$HOME"/.espressif/*/esp-idf/export.sh 2>/dev/null | sort -rV); do
            idf_export="$cand"; break
        done
    fi
    if [[ -z "$idf_export" ]]; then
        echo "[qemu-lcd] ERROR: ESP-IDF not found." >&2
        echo "  Install ESP-IDF, or 'source <idf>/export.sh' before running this." >&2
        exit 2
    fi
    echo "[qemu-lcd] sourcing ESP-IDF: $idf_export"
    # shellcheck disable=SC1090
    . "$idf_export" >/dev/null 2>&1
fi

if ! command -v qemu-system-xtensa >/dev/null 2>&1; then
    echo "[qemu-lcd] ERROR: qemu-system-xtensa not found after sourcing ESP-IDF." >&2
    echo "  Install QEMU support: idf_tools.py install qemu-xtensa" >&2
    exit 2
fi

# Pick a free TCP port for UART1, starting at $PORT. A QEMU that was
# killed (rather than exited cleanly) can leave its listening socket
# held for a while — walk forward until one is actually free so a
# re-run doesn't die with "Address already in use".
port_busy() {
    if command -v ss >/dev/null 2>&1; then
        ss -ltn "( sport = :$1 )" 2>/dev/null | grep -q ":$1 "
    else
        python3 -c "import socket,sys
s=socket.socket()
try: s.bind(('127.0.0.1',int(sys.argv[1]))); sys.exit(1)
except OSError: sys.exit(0)
finally: s.close()" "$1"
    fi
}
tries=0
while port_busy "$PORT"; do
    echo "[qemu-lcd] port $PORT busy — trying $((PORT + 1))"
    PORT=$((PORT + 1)); tries=$((tries + 1))
    [[ $tries -ge 20 ]] && { echo "[qemu-lcd] ERROR: no free port" >&2; exit 2; }
done

cd "$BOARD"
echo "[qemu-lcd] building firmware (GBJIT_QEMU_LCD=1) …"
idf.py -DGBJIT_QEMU_LCD=1 build

echo "[qemu-lcd] starting viewer (waits for QEMU on tcp:$PORT) …"
python3 "$ROOT/tools/qemu_lcd.py" --port "$PORT" --wait "$@" &
VIEWER_PID=$!

# On exit (normal, Ctrl-C, or error) take down both the viewer and any
# QEMU we spawned — an orphaned QEMU would otherwise keep the port and
# the flash image locked.
cleanup() {
    kill "$VIEWER_PID" 2>/dev/null || true
    pkill -f "qemu-system-xtensa.*$BOARD/build/qemu_flash.bin" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

echo "[qemu-lcd] launching QEMU — UART0=console, UART1=tcp:$PORT"
echo "[qemu-lcd] quit with Ctrl-A x (in this terminal) or close the viewer"
idf.py qemu --qemu-extra-args "-serial tcp::$PORT,server,nowait"
