#!/usr/bin/env python3
"""Render benchmark/results/cache_sweep.csv as a PNG.

Two y-axes — throughput in MHz of T-cycles on the left and blocks-
compiled count on the right — so you can see the throughput plateau
right where blocks_compiled stops growing (i.e. the working set fits).

Run after benchmark/run_cache_sweep.sh."""

import csv
import os
import sys

import matplotlib
matplotlib.use("Agg")  # headless
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
CSV_PATH = os.path.join(HERE, "results", "cache_sweep.csv")
PNG_PATH = os.path.join(HERE, "results", "cache_sweep.png")


def load_rows(path):
    """Load sweep CSV. An arena_kb of 0 (special sentinel value written by
    run_cache_sweep.sh after a one-shot BENCH_MODE_INTERP_ONLY run) is
    pulled out separately as the interp baseline; everything else is a
    JIT cache-size point."""
    rows = []
    interp_mhz = None
    with open(path) as f:
        reader = csv.DictReader(f)
        for r in reader:
            if not r["arena_kb"] or not r["mhz"]:
                continue
            kb = int(r["arena_kb"])
            mhz = float(r["mhz"])
            if kb == 0:
                interp_mhz = mhz
                continue
            rows.append({
                "arena_kb": kb,
                "mhz": mhz,
                "dmg_x": float(r["dmg_x"]),
                "blocks_compiled": int(r["blocks_compiled"] or 0),
                "blocks_executed": int(r["blocks_executed"] or 0),
                "chain_hits": int(r["chain_hits"] or 0),
                "chain_misses": int(r["chain_misses"] or 0),
                "rom": r["rom"],
            })
    rows.sort(key=lambda r: r["arena_kb"])
    return rows, interp_mhz


def main():
    if not os.path.exists(CSV_PATH):
        sys.exit(f"missing {CSV_PATH} — run benchmark/run_cache_sweep.sh first")
    rows, interp_mhz = load_rows(CSV_PATH)
    if not rows:
        sys.exit("no usable rows in sweep CSV")

    rom = rows[0]["rom"]
    kb     = [r["arena_kb"] for r in rows]
    mhz    = [r["mhz"] for r in rows]
    dmg    = [r["dmg_x"] for r in rows]
    blocks = [r["blocks_compiled"] for r in rows]

    fig, ax_throughput = plt.subplots(figsize=(7.5, 4.5))
    ax_throughput.set_xscale("log", base=2)
    ax_throughput.set_xticks(kb)
    ax_throughput.set_xticklabels([str(k) for k in kb])
    ax_throughput.set_xlabel("JIT codecache arena size (KB)")
    ax_throughput.set_ylabel("T-cycle throughput (MHz, qemu)", color="C0")
    line_mhz, = ax_throughput.plot(kb, mhz, "o-", color="C0", linewidth=2,
                                   markersize=7, label="JIT (cached)")
    ax_throughput.tick_params(axis="y", labelcolor="C0")
    ax_throughput.grid(True, which="both", alpha=0.25)

    # Interp baseline as a horizontal reference — anything above the line
    # is the JIT actually beating the per-op switch. Falls below the line
    # in arena-too-small regions where the dispatcher keeps recompiling.
    if interp_mhz is not None:
        ax_throughput.axhline(interp_mhz, color="C3", linestyle="--",
                              linewidth=1.5, alpha=0.8,
                              label=f"interp baseline ({interp_mhz:.2f} MHz)")
        ax_throughput.legend(loc="lower right", framealpha=0.9, fontsize=9)

    # Secondary axis: DMG multiplier label on the same series.
    ax_dmg = ax_throughput.twinx()
    ax_dmg.set_ylabel("× DMG real-time", color="C0")
    ax_dmg.set_ylim(ax_throughput.get_ylim()[0] / 4.194304,
                    ax_throughput.get_ylim()[1] / 4.194304)
    ax_dmg.spines["right"].set_color("C0")
    ax_dmg.tick_params(axis="y", labelcolor="C0")

    # Annotate blocks_compiled at each point so the working-set inflection
    # is visible — the throughput plateau coincides with blocks_compiled
    # saturating.
    for x, y, b in zip(kb, mhz, blocks):
        ax_throughput.annotate(f"{b} blk",
                               xy=(x, y), xytext=(0, 8),
                               textcoords="offset points",
                               fontsize=8, ha="center", color="gray")

    fig.suptitle(f"gbjit-xtensa — JIT cache size vs throughput (qemu-S3, {rom})",
                 fontsize=11)
    fig.tight_layout()
    fig.savefig(PNG_PATH, dpi=130, bbox_inches="tight")
    print(f"wrote {PNG_PATH}")


if __name__ == "__main__":
    main()
