#!/usr/bin/env python3
"""Parse a qemu-system-xtensa trace produced with `-d in_asm,exec,nochain`
and produce a per-mode benchmark table.

Trace format (interleaved):

    ----------------
    IN:
    0x42004800:  005116      entry sp, 16
    0x42004803:  028d        mov.n a8, a2
    ...

    Trace 0: 0x77a4a0000100 [00000000/0000000040000400/00018004/ff008200]
    Trace 1: 0x77a4a40000c0 [00000000/0000000040000454/00018004/ff008200]
    ...

The `IN:` blocks define translation blocks (TBs): we record for each
guest PC the list of (addr, mnemonic) instructions inside. The
`Trace N: PHYS [..../GUEST_PC/.../...]` lines are TB executions. We
extract GUEST_PC from the bracket field and look up that TB.

Each mode (interp / jit) runs sequentially, so we attribute trace
events by *index* — the dividing point is found in the firmware
[BENCH] lines.

(Note: qemu's "Trace" line format varies across versions. This script
parses the version shipped with IDF v6.0.1 / qemu_xtensa esp_develop_9.2.x.)
"""

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path


# Memory-fetch / store mnemonics emitted by our JIT and IDF's compiler.
LOAD_MNEMONICS  = {"l8ui", "l16ui", "l16si", "l32i", "l32i.n", "l32r"}
STORE_MNEMONICS = {"s8i",  "s16i",            "s32i", "s32i.n"}


def parse_in_asm(path):
    """Yield (tb_guest_pc, list_of_(addr, mnem)) for each IN: block."""
    tb_pc = None
    insns = []
    # qemu in_asm lines look like:  "0x40000400:  j\t0x40000454"
    # First token after ':' is the mnemonic; the rest are operands.
    line_re = re.compile(r"^0x([0-9a-fA-F]+):\s+(\S+)")
    in_block = False
    with open(path) as f:
        for line in f:
            if line.startswith("----------"):
                if tb_pc is not None:
                    yield tb_pc, insns
                tb_pc = None
                insns = []
                in_block = False
                continue
            if line.startswith("IN:"):
                in_block = True
                continue
            m = line_re.match(line)
            if m and in_block:
                addr = int(m.group(1), 16)
                mnem = m.group(2).lower()
                if tb_pc is None:
                    tb_pc = addr
                insns.append((addr, mnem))
    if tb_pc is not None and insns:
        yield tb_pc, insns


def categorize_region(addr):
    """Coarse Xtensa address-space classifier for the ESP32-S3 layout."""
    # Flash icache (executable instruction mapping).
    if 0x42000000 <= addr <  0x43000000:
        return "flash_xip"
    # Internal SRAM mapped for instruction fetch (where the JIT code lives).
    if 0x40370000 <= addr <  0x40400000:
        return "iram"
    # Boot ROM.
    if 0x40000000 <= addr <  0x40070000:
        return "rom"
    # Internal SRAM mapped for data.
    if 0x3FC80000 <= addr <  0x3FD00000:
        return "dram"
    # Anything else (RTC RAM, peripherals, etc.)
    return "other"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("trace_log",  help="qemu output from -d in_asm,exec,nochain")
    ap.add_argument("bench_lines", help="firmware [BENCH] lines")
    ap.add_argument("--label", default=None, help="label this mode (interp / jit)")
    ap.add_argument("--out", default=None, help="write the markdown table here (default: stdout)")
    args = ap.parse_args()

    # --------- Build the TB table from IN: blocks. -----------------------
    tb_table = {}      # guest_pc → list of mnemonics
    tb_count = 0
    for pc, insns in parse_in_asm(args.trace_log):
        tb_table[pc] = [m for (_, m) in insns]
        tb_count += 1
    print(f"# parsed {tb_count} translation blocks", file=sys.stderr)

    # --------- Walk the Trace lines, counting executions. ---------------
    #
    # Format:  Trace N: HOST_PTR [.../GUEST_PC_hex/.../...]
    # Guest PC is field index 1 (zero-indexed) inside the brackets.
    trace_re = re.compile(r"^Trace\s+\d+:\s+0x[0-9a-fA-F]+\s+\[[0-9a-fA-F]+/([0-9a-fA-F]+)/")

    per_tb_exec   = defaultdict(int)
    total_traces  = 0
    with open(args.trace_log) as f:
        for line in f:
            m = trace_re.match(line)
            if not m:
                continue
            pc = int(m.group(1), 16)
            per_tb_exec[pc] += 1
            total_traces += 1
    print(f"# parsed {total_traces} TB executions", file=sys.stderr)

    # Some TBs that appear in Trace lines may not be in our IN: dict
    # (e.g. boot ROM TBs were translated before our log started). Drop
    # those quietly; they'd be uninformative.
    unknown_pcs = [pc for pc in per_tb_exec if pc not in tb_table]
    if unknown_pcs:
        print(f"# {len(unknown_pcs)} TB PCs traced but never disassembled "
              f"(likely pre-bench rom/boot); skipping", file=sys.stderr)

    # --------- Read the firmware's [BENCH] lines. ------------------------
    bench = {}
    bench_re = re.compile(r"\[BENCH\] mode=(\w+) (.*)")
    with open(args.bench_lines) as f:
        for line in f:
            m = bench_re.search(line)
            if not m: continue
            mode = m.group(1)
            kv = dict(re.findall(r"(\w+)=([\w.0-9]+)", m.group(2)))
            bench[mode] = kv

    if not bench:
        print("ERR: bench_lines empty", file=sys.stderr)
        sys.exit(1)

    # --------- Aggregate: total Xtensa instructions / memory ops -------
    #
    # The full trace covers BOTH modes back-to-back. We don't have a
    # clean per-mode boundary, but the JIT-emitted code lives in IRAM
    # (0x4037xxxx..0x403Fxxxx) and we can attribute IRAM-PC TBs to JIT.
    # Flash-XIP TBs cover the interpreter + helpers — which are shared
    # by the JIT (when it falls back) and the interp. So:
    #
    #   * jit_only_xtensa_instr = sum over IRAM TBs (executed code that
    #     only exists when the JIT compiles a block — the JIT's emitted
    #     output)
    #   * common_xtensa_instr  = sum over flash-XIP TBs (helpers, FreeRTOS,
    #     ... — executed by either mode)
    #
    # The firmware bookends: interp finishes (prints its [BENCH] line),
    # then jit begins. We can't cleanly split the flash-XIP count, but
    # the *ratio of total* counts between the two passes (estimated from
    # the firmware-reported wall times and a single combined run) gives
    # a reasonable per-mode breakdown.

    cat_instr      = defaultdict(int)
    cat_loads      = defaultdict(int)
    cat_stores     = defaultdict(int)
    cat_tb_exec    = defaultdict(int)

    for pc, count in per_tb_exec.items():
        if pc not in tb_table: continue
        cat = categorize_region(pc)
        mnems = tb_table[pc]
        cat_tb_exec[cat] += count
        cat_instr[cat]   += count * len(mnems)
        cat_loads[cat]   += count * sum(1 for m in mnems if m in LOAD_MNEMONICS)
        cat_stores[cat]  += count * sum(1 for m in mnems if m in STORE_MNEMONICS)

    # --------- Render table ---------------------------------------------
    out = []
    label = args.label or "combined"
    out.append(f"# QEMU-Xtensa benchmark — `{label}` mode\n")
    out.append("## Firmware-reported throughput\n")
    out.append("| Mode | GB cycles | wall (µs) | throughput (MHz of T-cycles) | × DMG real-time |")
    out.append("|------|-----------|-----------|------------------------------|-----------------|")
    for mode, kv in bench.items():
        out.append(f"| {mode} | {kv['cycles']} | {kv['elapsed_us']} | {kv['mhz']} | {kv['dmg_x']} |")
        if "blocks_executed" in kv:
            out.append("")
            out.append(f"JIT dispatcher: blocks_compiled={kv.get('blocks_compiled','?')} "
                       f"executed={kv['blocks_executed']} "
                       f"chain_hits={kv.get('chain_hits','?')} "
                       f"chain_misses={kv.get('chain_misses','?')}")

    out.append("")
    out.append("## Xtensa instructions executed (combined: interp run + JIT run)\n")
    out.append("| Region | TBs executed | Instructions | Load instrs | Store instrs |")
    out.append("|--------|-------------:|-------------:|------------:|-------------:|")
    region_order = ["flash_xip", "iram", "rom", "dram", "other"]
    region_labels = {
        "flash_xip": "flash XIP (interp + JIT helpers)",
        "iram": "IRAM (JIT-emitted code)",
        "rom": "boot ROM",
        "dram": "DRAM",
        "other": "other",
    }
    tot_tb = sum(cat_tb_exec.values())
    tot_i  = sum(cat_instr.values())
    tot_l  = sum(cat_loads.values())
    tot_s  = sum(cat_stores.values())
    for r in region_order:
        if cat_tb_exec[r] == 0: continue
        out.append(f"| {region_labels[r]} | {cat_tb_exec[r]:,} | {cat_instr[r]:,} | {cat_loads[r]:,} | {cat_stores[r]:,} |")
    out.append(f"| **total** | **{tot_tb:,}** | **{tot_i:,}** | **{tot_l:,}** | **{tot_s:,}** |")

    out.append("")
    out.append("## Notes\n")
    out.append("- Both modes run sequentially in the same boot, so the totals")
    out.append("  above cover the *sum* of one interp run + one JIT run.")
    out.append("- IRAM-region Xtensa code = JIT-emitted block bodies. Anything")
    out.append("  there is JIT-side only; the interp never executes from IRAM.")
    out.append("- Flash-XIP code = pre-compiled C: SM83 interpreter, helpers")
    out.append("  (sm83_step / mmu_read8 / mmu_write8), IDF runtime, libc.")
    out.append("  The JIT shares this region (helper fallback) but takes the")
    out.append("  short path through tinier helpers.")
    out.append("- QEMU has no cache model: every load/store on this trace is")
    out.append("  modelled as a single-cycle access. On real ESP32-S3 silicon,")
    out.append("  flash-XIP loads pay 10–40 wait cycles on icache miss; SRAM")
    out.append("  loads are 1 cycle. The flash-XIP instruction count is an")
    out.append("  upper bound on potentially-stalling fetches.")

    text = "\n".join(out) + "\n"
    if args.out:
        Path(args.out).write_text(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
