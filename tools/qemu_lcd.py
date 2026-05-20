#!/usr/bin/env python3
"""
QEMU virtual-LCD viewer for gbjit-xtensa.

Renders the Game Boy framebuffer that the firmware (built with
-DGBJIT_QEMU_LCD=1) streams out of the emulated UART1. QEMU maps that
UART to a TCP chardev; this script connects to it, decodes the framed
2-bpp packets, and shows the 160x144 DMG screen in a window.

Packet wire format (see boards/HTIT-WB32LAF_V3.2/main/qemu_lcd.c):
    4 bytes     magic "GBF1"
    5760 bytes  2 bits/pixel, pixel 0 in the low bits of byte 0,
                row-major, 160x144 — shade 0 (lightest) .. 3 (darkest)

Pure standard library — only tkinter, which renders the frames as
P6 PPM images. No third-party packages, so it runs under any Python
(including ESP-IDF's bundled interpreter).

Usage:
    python3 tools/qemu_lcd.py [--host H] [--port P] [--scale N]
                              [--wait] [--save-dir DIR] [--frames N]

--wait        retry the connection until QEMU's socket is up.
--save-dir    headless: write each frame as DIR/frameNNNNN.ppm and
              skip the window (no display needed).
--frames N    headless: exit after N frames (smoke tests / CI).
"""
import argparse
import os
import socket
import sys
import threading
import time

GB_W, GB_H = 160, 144
PACKED = GB_W * GB_H // 4          # 2 bpp
MAGIC = b"GBF1"

# Classic DMG-green 4-shade palette, lightest -> darkest.
PALETTE = [
    (0x9B, 0xBC, 0x0F),
    (0x8B, 0xAC, 0x0F),
    (0x30, 0x62, 0x30),
    (0x0F, 0x38, 0x0F),
]

# Per-packed-byte expansion: byte value -> 12 RGB bytes (4 pixels).
_BYTE_TO_RGB = [
    bytes(c for k in (0, 2, 4, 6) for c in PALETTE[(b >> k) & 3])
    for b in range(256)
]
_PPM_HEADER = b"P6\n%d %d\n255\n" % (GB_W, GB_H)


def connect(host, port, wait):
    deadline = time.time() + 60
    while True:
        try:
            s = socket.create_connection((host, port), timeout=2)
            s.settimeout(5)
            return s
        except OSError:
            if not wait or time.time() > deadline:
                raise
            time.sleep(0.4)


def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("QEMU serial closed")
        buf += chunk
    return bytes(buf)


def next_frame(sock):
    """Resync on the 4-byte magic, then return the 2-bpp payload."""
    window = b""
    while True:
        b = sock.recv(1)
        if not b:
            raise EOFError("QEMU serial closed")
        window = (window + b)[-4:]
        if window == MAGIC:
            return recv_exact(sock, PACKED)


def to_ppm(packed):
    """2-bpp packed bytes -> a complete P6 PPM image (bytes)."""
    rows = bytearray(_PPM_HEADER)
    b2rgb = _BYTE_TO_RGB
    for byte in packed:
        rows += b2rgb[byte]
    return bytes(rows)


class FrameReader(threading.Thread):
    """Background thread: keeps `self.latest` pointing at the freshest
    decoded PPM frame so the GUI never blocks on the socket."""

    def __init__(self, sock):
        super().__init__(daemon=True)
        self.sock = sock
        self.latest = None
        self.count = 0
        self.alive = True

    def run(self):
        try:
            while self.alive:
                payload = next_frame(self.sock)
                self.latest = to_ppm(payload)
                self.count += 1
        except (EOFError, OSError):
            self.alive = False


def run_window(reader, scale):
    import tkinter as tk

    root = tk.Tk()
    root.title("gbjit-xtensa — QEMU virtual LCD (160x144)")
    root.resizable(False, False)
    label = tk.Label(root, borderwidth=0)
    label.pack()
    status = tk.Label(root, anchor="w", font=("monospace", 9))
    status.pack(fill="x")

    state = {"shown": 0, "t0": time.time(), "img": None}

    def tick():
        if not reader.alive and reader.latest is None:
            status.config(text=" QEMU serial closed")
            root.after(500, tick)
            return
        ppm = reader.latest
        if ppm is not None:
            img = tk.PhotoImage(data=ppm, format="ppm")
            if scale > 1:
                img = img.zoom(scale)
            label.config(image=img)
            state["img"] = img             # keep a ref alive
            state["shown"] += 1
            dt = time.time() - state["t0"]
            if dt >= 1.0:
                fps = state["shown"] / dt
                status.config(text=f" stream {reader.count:6d} frames | "
                                    f"viewer {fps:5.1f} fps")
                state["shown"] = 0
                state["t0"] = time.time()
        root.after(16, tick)               # ~60 Hz GUI refresh

    root.after(16, tick)
    root.mainloop()
    reader.alive = False
    return 0


def run_headless(reader, save_dir, max_frames=0):
    os.makedirs(save_dir, exist_ok=True)
    saved = 0
    last = -1
    stop = f"{max_frames} frames" if max_frames else "Ctrl-C"
    print(f"saving frames to {save_dir}/ — stops after {stop}")
    try:
        while reader.alive or reader.latest is not None:
            if reader.count != last and reader.latest is not None:
                last = reader.count
                path = os.path.join(save_dir, f"frame{saved:05d}.ppm")
                with open(path, "wb") as f:
                    f.write(reader.latest)
                saved += 1
                if max_frames and saved >= max_frames:
                    break
            else:
                time.sleep(0.01)
    except KeyboardInterrupt:
        pass
    print(f"saved {saved} frames")
    return 0


def main():
    ap = argparse.ArgumentParser(description="gbjit-xtensa QEMU LCD viewer")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5556,
                    help="QEMU UART1 TCP chardev port (default 5556)")
    ap.add_argument("--scale", type=int, default=3,
                    help="integer upscale for the window (default 3)")
    ap.add_argument("--wait", action="store_true",
                    help="retry the connection until QEMU is up")
    ap.add_argument("--save-dir",
                    help="headless: write each frame as a PPM here")
    ap.add_argument("--frames", type=int, default=0,
                    help="headless: exit after this many frames (0 = run "
                         "until Ctrl-C); useful for smoke tests / CI")
    args = ap.parse_args()

    try:
        sock = connect(args.host, args.port, args.wait)
    except OSError as e:
        print(f"error: cannot connect to {args.host}:{args.port} — {e}",
              file=sys.stderr)
        return 2
    print(f"connected to QEMU LCD stream at {args.host}:{args.port}")

    reader = FrameReader(sock)
    reader.start()

    if args.save_dir:
        return run_headless(reader, args.save_dir, args.frames)
    return run_window(reader, args.scale)


if __name__ == "__main__":
    sys.exit(main())
