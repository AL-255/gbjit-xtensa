#!/usr/bin/env python3
"""Render benchmark/results/cache_sweep.csv as a PNG.

The CSV carries one row per (mode, arena_kb) combination — the sweep
script runs jit / jit_warm / jit_noprefetch across the same arena
range plus a single interp baseline (arena_kb=0). We draw one line per
JIT mode and the interp baseline as a horizontal reference."""

import csv
import os
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
CSV_PATH = os.path.join(HERE, "results", "cache_sweep.csv")
PNG_PATH = os.path.join(HERE, "results", "cache_sweep.png")

# Display name + line style per mode. Keys must match the `mode` column
# values written by run_cache_sweep.sh.
MODE_STYLES = {
    "jit":            {"label": "JIT (cached, cold)",        "color": "C0", "marker": "o"},
    "jit_warm":       {"label": "JIT (cached, pre-compiled)", "color": "C2", "marker": "s"},
    "jit_noprefetch": {"label": "JIT (cached, no prefetch)",  "color": "C1", "marker": "^"},
}


def load_rows(path):
    """Returns (jit_rows_by_mode, interp_mhz). jit_rows_by_mode maps
    mode → list of dicts sorted by arena_kb."""
    by_mode = defaultdict(list)
    interp_mhz = None
    with open(path) as f:
        for r in csv.DictReader(f):
            mode = r.get("mode") or "jit"
            if not r.get("mhz"):
                continue
            mhz = float(r["mhz"])
            arena = int(r.get("arena_kb") or 0)
            if mode == "interp":
                interp_mhz = mhz
                continue
            by_mode[mode].append({
                "arena_kb": arena,
                "mhz": mhz,
                "dmg_x": float(r.get("dmg_x") or 0),
                "blocks_compiled": int(r.get("blocks_compiled") or 0),
                "rom": r.get("rom", ""),
            })
    for k in by_mode:
        by_mode[k].sort(key=lambda r: r["arena_kb"])
    return by_mode, interp_mhz


def main():
    if not os.path.exists(CSV_PATH):
        sys.exit(f"missing {CSV_PATH} — run benchmark/run_cache_sweep.sh first")
    by_mode, interp_mhz = load_rows(CSV_PATH)
    if not by_mode:
        sys.exit("no JIT rows in sweep CSV")

    rom = next(iter(by_mode.values()))[0]["rom"]
    # x ticks from whichever mode has the most points (they should
    # match, but be tolerant).
    all_arenas = sorted({r["arena_kb"] for rows in by_mode.values() for r in rows})

    fig, ax = plt.subplots(figsize=(8.0, 4.8))
    ax.set_xticks(all_arenas)
    ax.set_xticklabels([str(k) for k in all_arenas])
    ax.set_xlabel("JIT codecache arena size (KB)")
    ax.set_ylabel("T-cycle throughput (MHz, qemu)")
    ax.grid(True, which="both", alpha=0.25)

    for mode in ("jit", "jit_warm", "jit_noprefetch"):
        rows = by_mode.get(mode)
        if not rows:
            continue
        style = MODE_STYLES[mode]
        kb  = [r["arena_kb"] for r in rows]
        mhz = [r["mhz"] for r in rows]
        ax.plot(kb, mhz, linestyle="-", color=style["color"], marker=style["marker"],
                linewidth=2, markersize=6, label=style["label"])

    if interp_mhz is not None:
        ax.axhline(interp_mhz, color="C3", linestyle="--", linewidth=1.5,
                   alpha=0.8, label=f"interp baseline ({interp_mhz:.2f} MHz)")

    # Annotate blocks_compiled on the cold `jit` line so the working-
    # set inflection is visible. (jit_warm reports 0 because the
    # measured pass has no compiles; jit_noprefetch closely tracks
    # jit.)
    for r in by_mode.get("jit", []):
        ax.annotate(f"{r['blocks_compiled']} blk",
                    xy=(r["arena_kb"], r["mhz"]),
                    xytext=(0, 9), textcoords="offset points",
                    fontsize=8, ha="center", color="gray")

    # Secondary y axis showing the same scale in × DMG real-time units.
    ax_dmg = ax.twinx()
    ax_dmg.set_ylabel("× DMG real-time")
    ax_dmg.set_ylim(ax.get_ylim()[0] / 4.194304, ax.get_ylim()[1] / 4.194304)

    ax.set_title(f"gbjit-xtensa — JIT cache size vs throughput (qemu-S3, {rom})")
    ax.legend(loc="lower right", framealpha=0.9, fontsize=9)
    fig.tight_layout()
    fig.savefig(PNG_PATH, dpi=130, bbox_inches="tight")
    print(f"wrote {PNG_PATH}")


if __name__ == "__main__":
    main()
