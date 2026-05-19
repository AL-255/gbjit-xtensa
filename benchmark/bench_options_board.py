#!/usr/bin/env python3
"""
Sweep the GBJIT_* compile-time toggles on the Heltec board.

For each configuration: rebuilds the firmware with the named flag,
flashes the board, captures the per-second OLED log over 20 s, and
reports min/avg/max fps from the steady-state portion of the run.

Run from the repo root:
    python3 benchmark/bench_options_board.py

The script temporarily patches sdkconfig.defaults + the gbjit
component CMakeLists to enable DEBUG=1 and INFO-level logs (the
release defaults strip both), and restores them on exit.
"""
import os, sys, subprocess, time, re, json, signal
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BOARD = ROOT / "boards/HTIT-WB32LAF_V3.2"
COMPONENT_CMAKE = BOARD / "components/gbjit/CMakeLists.txt"
MAIN_CMAKE = BOARD / "main/CMakeLists.txt"
SDKCONFIG_DEFAULTS = BOARD / "sdkconfig.defaults"
PORT = os.environ.get("GBJIT_PORT", "/dev/ttyUSB0")
SAMPLE_TIME = 22       # seconds of serial capture per config
SKIP_FIRST_S = 6       # discard the first N seconds (boot + title screen)
IDF_EXPORT = Path.home() / ".espressif/v6.0.1/esp-idf/export.sh"

# (label, dict-of-cmake-defines).
# Order matters only for the report; the baseline goes first as the
# reference point for the relative deltas in the summary table.
CONFIGS = [
    ("baseline",                {}),
    ("halt_step_16",            {"GBJIT_HALT_STEP_CYCLES": 16}),
    ("halt_step_64",            {"GBJIT_HALT_STEP_CYCLES": 64}),
    ("halt_step_256",           {"GBJIT_HALT_STEP_CYCLES": 256}),
    ("double_buffer_on",        {"GBJIT_FRAMEBUFFER_DOUBLE_BUFFER": 1}),
    ("hs64_doublebuf",          {"GBJIT_HALT_STEP_CYCLES": 64,
                                 "GBJIT_FRAMEBUFFER_DOUBLE_BUFFER": 1}),
]

def patch_for_bench():
    """Enable DEBUG=1 (gbjit + main components) and INFO logs so the
    per-second oled_task fps line reaches the UART. Returns the
    original file texts so restore_after_bench can put them back."""
    sdk = SDKCONFIG_DEFAULTS.read_text()
    sdk_new = sdk
    sdk_new = sdk_new.replace("CONFIG_LOG_DEFAULT_LEVEL_ERROR=y",
                              "CONFIG_LOG_DEFAULT_LEVEL_INFO=y")
    sdk_new = sdk_new.replace("CONFIG_BOOTLOADER_LOG_LEVEL_ERROR=y",
                              "CONFIG_BOOTLOADER_LOG_LEVEL_INFO=y")
    SDKCONFIG_DEFAULTS.write_text(sdk_new)

    patch_line = ("\n# bench harness: re-enable DEBUG counters\n"
                  "target_compile_definitions(${COMPONENT_LIB} PRIVATE DEBUG=1)\n")
    cml_gbjit = COMPONENT_CMAKE.read_text()
    if "PRIVATE DEBUG=1" not in cml_gbjit:
        COMPONENT_CMAKE.write_text(cml_gbjit + patch_line)
    cml_main = MAIN_CMAKE.read_text()
    if "PRIVATE DEBUG=1" not in cml_main:
        MAIN_CMAKE.write_text(cml_main + patch_line)
    return sdk, cml_gbjit, cml_main

def restore_after_bench(orig_sdk, orig_gbjit, orig_main):
    SDKCONFIG_DEFAULTS.write_text(orig_sdk)
    COMPONENT_CMAKE.write_text(orig_gbjit)
    MAIN_CMAKE.write_text(orig_main)

def run(cmd, check=True, cwd=None, capture=True):
    return subprocess.run(["bash", "-c", cmd], cwd=cwd, check=check,
                          stdout=subprocess.PIPE if capture else None,
                          stderr=subprocess.STDOUT)

def idf_cmd(rest):
    return f"source {IDF_EXPORT} > /dev/null 2>&1 && {rest}"

def build_and_flash(defines):
    # Wipe the build cache so the new -D values actually propagate.
    (BOARD / "sdkconfig").unlink(missing_ok=True)
    (BOARD / "build/CMakeCache.txt").unlink(missing_ok=True)
    dflags = " ".join(f"-D{k}={v}" for k, v in defines.items())
    r = run(idf_cmd(f"idf.py {dflags} reconfigure"),
            cwd=BOARD, capture=True)
    r = run(idf_cmd("idf.py build"),
            cwd=BOARD, capture=True)
    r = run(idf_cmd(f"idf.py -p {PORT} -b 460800 flash"),
            cwd=BOARD, capture=True)
    # Drain any flash chatter before opening for monitor.
    time.sleep(0.5)

FPS_RE = re.compile(r"fps=(\d+)")

def capture_fps_samples(duration_s):
    """Read serial for `duration_s`, return list of (t_since_start, fps)."""
    import serial
    s = serial.Serial(PORT, 115200, timeout=0.3)
    samples = []
    start = time.time()
    while time.time() - start < duration_s:
        line = s.readline()
        if not line:
            continue
        try:
            text = line.decode("utf-8", errors="replace").rstrip()
        except Exception:
            continue
        m = FPS_RE.search(text)
        if m:
            samples.append((time.time() - start, int(m.group(1))))
    s.close()
    return samples

def summarize(label, samples):
    """Drop boot + title screen samples, return summary stats."""
    useful = [v for (t, v) in samples if t >= SKIP_FIRST_S]
    n = len(useful)
    if n < 5:
        return None
    vmin, vmax = min(useful), max(useful)
    vavg = sum(useful) / n
    return dict(label=label, samples_used=n, min=vmin, avg=vavg,
                max=vmax, all=useful)

def main():
    print(f"# port={PORT}  sample={SAMPLE_TIME}s  skip={SKIP_FIRST_S}s")
    orig_sdk, orig_gbjit, orig_main = patch_for_bench()
    try:
        results = []
        for label, defines in CONFIGS:
            print(f"\n--- {label}  defines={defines or '{}'} ---", flush=True)
            try:
                build_and_flash(defines)
            except subprocess.CalledProcessError as e:
                print(f"  BUILD/FLASH FAILED: {e}")
                continue
            samples = capture_fps_samples(SAMPLE_TIME)
            stats = summarize(label, samples)
            if stats is None:
                print(f"  not enough samples: got {len(samples)} total")
                continue
            print(f"  n={stats['samples_used']}  min={stats['min']}"
                  f"  avg={stats['avg']:.1f}  max={stats['max']}")
            print(f"  samples={stats['all']}")
            results.append(stats)
        print("\n=== summary ===")
        # Find baseline for relative numbers.
        base = next((r for r in results if r["label"] == "baseline"), None)
        for r in results:
            d_min = (r["min"] - base["min"]) if base else 0
            d_avg = (r["avg"] - base["avg"]) if base else 0.0
            print(f"  {r['label']:22s}  min={r['min']:3d}  avg={r['avg']:5.1f}"
                  f"  max={r['max']:3d}   Δmin={d_min:+d}  Δavg={d_avg:+.1f}")
        # Save JSON for post-hoc analysis.
        out = ROOT / "benchmark/results/board_option_sweep.json"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(results, indent=2))
        print(f"  written {out.relative_to(ROOT)}")
    finally:
        restore_after_bench(orig_sdk, orig_gbjit, orig_main)
        print("\n# restored sdkconfig.defaults and both CMakeLists.txt files")

if __name__ == "__main__":
    main()
