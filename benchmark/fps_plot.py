#!/usr/bin/env python3
"""Plot the gbjit-xtensa board FPS benchmark produced by fps_sweep.sh —
average and lowest-1s framerate vs JIT arena size, one line per eviction
mode, with the interpreter baseline drawn as an hline. Run from the repo
root: python3 benchmark/fps_plot.py"""
import csv
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

CSV = "benchmark/results/tetris_arena_sweep.csv"
rows = list(csv.DictReader(open(CSV)))
interp = next(r for r in rows if r["mode"] == "interp")
i_avg, i_min = int(interp["avg_fps"]), int(interp["min_fps"])

MODES = {
    "mode0": ("no eviction (bump)",    "tab:blue"),
    "mode1": ("coldness / LRU evict",  "tab:orange"),
    "mode2": ("circular / FIFO evict", "tab:green"),
}
data = {m: {"kb": [], "avg": [], "min": []} for m in MODES}
for r in rows:
    if r["mode"] in MODES:
        d = data[r["mode"]]
        d["kb"].append(int(r["arena_kb"]))
        d["avg"].append(int(r["avg_fps"]))
        d["min"].append(int(r["min_fps"]))

for metric, label, base, fname in [
    ("avg", "average",             i_avg, "benchmark/results/bench_avg.png"),
    ("min", "lowest (1 s window)", i_min, "benchmark/results/bench_min.png"),
]:
    plt.figure(figsize=(8, 5))
    for m, (name, colour) in MODES.items():
        plt.plot(data[m]["kb"], data[m][metric], marker="o", color=colour, label=name)
    plt.axhline(base, ls="--", color="dimgray",
                label=f"interpreter baseline ({base} fps)")
    plt.axhline(59.7, ls=":", color="firebrick", alpha=0.7,
                label="real Game Boy (59.7 fps)")
    plt.xlabel("JIT code-cache arena (KB)")
    plt.ylabel(f"{label} framerate (fps)")
    plt.title(f"Tetris on HTIT-WB32LAF_V3.2 — {label} FPS vs JIT arena size")
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.ylim(bottom=0)
    plt.savefig(fname, dpi=120, bbox_inches="tight")
    print("wrote", fname)
