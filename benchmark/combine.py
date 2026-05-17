#!/usr/bin/env python3
"""Combine the per-mode analyser outputs into a single comparison table.

Usage:
    combine.py  interp_bench_lines.txt  jit_bench_lines.txt \\
                interp_qemu_trace.log    jit_qemu_trace.log
"""

import re
import sys
import importlib.util
from pathlib import Path

# Reuse the per-mode analyser as a library.
_here = Path(__file__).parent
spec = importlib.util.spec_from_file_location("analyze", _here / "analyze.py")
analyze = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analyze)


def parse_bench_line(path):
    out = {}
    bench_re = re.compile(r"\[BENCH\] mode=(\w+) (.*)")
    with open(path) as f:
        for line in f:
            m = bench_re.search(line)
            if not m: continue
            out[m.group(1)] = dict(re.findall(r"(\w+)=([\w.0-9]+)", m.group(2)))
    return out


def count_trace(trace_path):
    """Return (per_region_tbs, per_region_instrs, loads, stores) totals."""
    tb_table = {}
    for pc, insns in analyze.parse_in_asm(trace_path):
        tb_table[pc] = [m for (_, m) in insns]

    trace_re = re.compile(r"^Trace\s+\d+:\s+0x[0-9a-fA-F]+\s+\[[0-9a-fA-F]+/([0-9a-fA-F]+)/")
    from collections import defaultdict
    tbs    = defaultdict(int)
    instrs = defaultdict(int)
    loads  = defaultdict(int)
    stores = defaultdict(int)
    with open(trace_path) as f:
        for line in f:
            m = trace_re.match(line)
            if not m: continue
            pc = int(m.group(1), 16)
            if pc not in tb_table: continue
            cat = analyze.categorize_region(pc)
            mnems = tb_table[pc]
            tbs[cat]    += 1
            instrs[cat] += len(mnems)
            loads[cat]  += sum(1 for x in mnems if x in analyze.LOAD_MNEMONICS)
            stores[cat] += sum(1 for x in mnems if x in analyze.STORE_MNEMONICS)
    return tbs, instrs, loads, stores


def main():
    if len(sys.argv) != 5:
        print(__doc__); sys.exit(1)
    interp_bench, jit_bench, interp_trace, jit_trace = sys.argv[1:5]

    bench_i = parse_bench_line(interp_bench).get("interp")
    bench_j = parse_bench_line(jit_bench).get("jit")
    if not bench_i or not bench_j:
        print("ERR: bench lines missing interp/jit entries", file=sys.stderr); sys.exit(1)

    tbs_i, ins_i, ld_i, st_i = count_trace(interp_trace)
    tbs_j, ins_j, ld_j, st_j = count_trace(jit_trace)

    def total(d): return sum(d.values())

    print("# QEMU-Xtensa benchmark — interp vs JIT (per-mode trace)\n")

    print("## Firmware-reported throughput\n")
    print("| Mode   | GB cycles | wall (µs) | T-cycle MHz | × DMG |")
    print("|--------|----------:|----------:|------------:|------:|")
    print(f"| interp | {bench_i['cycles']} | {bench_i['elapsed_us']} | {bench_i['mhz']} | {bench_i['dmg_x']} |")
    print(f"| jit    | {bench_j['cycles']} | {bench_j['elapsed_us']} | {bench_j['mhz']} | {bench_j['dmg_x']} |")
    if "blocks_executed" in bench_j:
        print()
        print(f"JIT dispatcher: blocks_compiled={bench_j.get('blocks_compiled','?')} "
              f"executed={bench_j['blocks_executed']} "
              f"chain_hits={bench_j.get('chain_hits','?')} "
              f"chain_misses={bench_j.get('chain_misses','?')}")

    print()
    print("## Per-mode Xtensa execution (full trace incl. boot ROM + IDF runtime)\n")
    print("| Region | TBs (interp) | TBs (jit) | Instrs (interp) | Instrs (jit) |")
    print("|--------|-------------:|----------:|----------------:|-------------:|")
    region_order = ["flash_xip", "iram", "rom", "dram", "other"]
    region_labels = {
        "flash_xip": "flash XIP (interp + JIT helpers + IDF)",
        "iram": "IRAM (JIT-emitted code)",
        "rom": "boot ROM",
        "dram": "DRAM data fetches",
        "other": "other",
    }
    for r in region_order:
        if tbs_i[r] == 0 and tbs_j[r] == 0: continue
        print(f"| {region_labels[r]} | {tbs_i[r]:>12,} | {tbs_j[r]:>9,} | {ins_i[r]:>15,} | {ins_j[r]:>12,} |")
    print(f"| **total** | **{total(tbs_i):,}** | **{total(tbs_j):,}** | **{total(ins_i):,}** | **{total(ins_j):,}** |")

    print()
    print("## Memory-fetch / store instructions\n")
    print("| Region | Loads (interp) | Loads (jit) | Stores (interp) | Stores (jit) |")
    print("|--------|---------------:|------------:|----------------:|-------------:|")
    for r in region_order:
        if ld_i[r] == 0 and ld_j[r] == 0 and st_i[r] == 0 and st_j[r] == 0: continue
        print(f"| {region_labels[r]} | {ld_i[r]:>14,} | {ld_j[r]:>11,} | {st_i[r]:>15,} | {st_j[r]:>12,} |")
    print(f"| **total** | **{total(ld_i):,}** | **{total(ld_j):,}** | **{total(st_i):,}** | **{total(st_j):,}** |")

    # Headline ratio + per-GB-cycle figures. Each trace covers exactly one
    # firmware boot (boot ROM + IDF init + 1 bench mode + idle suspend), so
    # the totals are comparable as long as both modes ran for the same
    # GB-cycle budget (which run_bench.sh enforces).
    tot_in_i = total(ins_i); tot_in_j = total(ins_j)
    tot_ld_i = total(ld_i);  tot_ld_j = total(ld_j)
    tot_st_i = total(st_i);  tot_st_j = total(st_j)
    gb_i = float(bench_i["cycles"])
    gb_j = float(bench_j["cycles"])

    print()
    print("## Headline ratios\n")
    print("| Metric | interp | jit | jit / interp |")
    print("|--------|------:|----:|-------------:|")
    if tot_in_j:
        print(f"| Xtensa instructions (full trace) | {tot_in_i:,} | {tot_in_j:,} | "
              f"{tot_in_j/tot_in_i:.2f}× |")
    if tot_ld_j:
        print(f"| Xtensa memory loads | {tot_ld_i:,} | {tot_ld_j:,} | "
              f"{tot_ld_j/tot_ld_i:.2f}× |")
    if tot_st_j:
        print(f"| Xtensa memory stores | {tot_st_i:,} | {tot_st_j:,} | "
              f"{tot_st_j/tot_st_i:.2f}× |")

    print()
    print("## Per GB T-cycle (whole-trace average — boot + IDF + bench)\n")
    print("| Metric | interp | jit |")
    print("|--------|------:|----:|")
    print(f"| Xtensa instructions / GB cycle | {tot_in_i/gb_i:.2f} | {tot_in_j/gb_j:.2f} |")
    print(f"| Xtensa memory loads / GB cycle | {tot_ld_i/gb_i:.2f} | {tot_ld_j/gb_j:.2f} |")
    print(f"| Xtensa memory stores / GB cycle | {tot_st_i/gb_i:.2f} | {tot_st_j/gb_j:.2f} |")

    # Boot ROM TBs are a decent shared-floor proxy: both modes execute
    # essentially the same boot ROM code, so anything above that minimum is
    # mode-specific. (We do NOT do a per-region min here — IRAM activity is
    # vastly different between modes and is *not* a shared floor.)
    boot_floor = min(ins_i["rom"], ins_j["rom"])
    print()
    print("## Approximate boot/IDF floor and bench-only work\n")
    print(f"Boot-ROM floor (shared between modes): ~{boot_floor:,} Xtensa instructions.")
    print()
    print("| Metric | interp | jit |")
    print("|--------|------:|----:|")
    print(f"| Xtensa instructions above boot floor | {tot_in_i-boot_floor:,} | {tot_in_j-boot_floor:,} |")
    print(f"| → per GB cycle | {(tot_in_i-boot_floor)/gb_i:.2f} | {(tot_in_j-boot_floor)/gb_j:.2f} |")

    print()
    print("## Notes\n")
    print("- Each mode is run in its own qemu boot. The trace covers boot ROM,")
    print("  IDF init, the benchmark itself, and the post-bench idle suspend")
    print("  (`vTaskDelay(portMAX_DELAY)`) until qemu hits its 60-second wall")
    print("  timeout. The idle-task `WAITI` keeps the trace growth small after")
    print("  the bench completes, but FreeRTOS scheduler ticks still emit some.")
    print("- IRAM activity in interp mode is FreeRTOS / IDF code that the")
    print("  linker placed in the internal-SRAM-mapped IRAM region; the JIT's")
    print("  arena is not allocated in interp-only builds, so the interp mode")
    print("  is never executing JIT-emitted Xtensa from there.")
    print("- QEMU has no Xtensa cache model: each load is a single emulated")
    print("  cycle. On real ESP32-S3 silicon, instruction fetches from the")
    print("  flash-XIP region cost 1 cycle on icache hit and 10–40 cycles on")
    print("  miss. The flash-XIP load counts above are therefore an upper")
    print("  bound on the wait-cycle penalty real hardware would pay; multiply")
    print("  by an expected miss rate (1–5 % typical for well-cached code) to")
    print("  estimate actual stall budget.")
    print("- Firmware-reported throughput (`elapsed_us` from `esp_timer`) does")
    print("  NOT track Xtensa instructions one-to-one in qemu — qemu advances")
    print("  the simulated system timer based partly on wall clock, so simpler")
    print("  Xtensa instructions emulate faster per unit simulated-time. The")
    print("  JIT's lower instruction count is the relevant figure for real-")
    print("  hardware performance.")


if __name__ == "__main__":
    main()
