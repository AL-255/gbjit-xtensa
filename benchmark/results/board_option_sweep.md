# Board-side option sweep — HTIT-WB32LAF_V3.2

ROM: Super Mario Land (sml.gb). Each configuration was built with
`benchmark/bench_options_board.py`, flashed to the Heltec board, then
sampled for 22 seconds with the first 6 seconds discarded (boot + SML
title screen). 16 useful fps samples per row.

| Config | min | avg | max | Δmin | Δavg | Notes |
|---|---:|---:|---:|---:|---:|---|
| **baseline** (all defaults) | **57** | 65.8 | 76 | 0 | 0.0 | 2-slot predictor + JP HL inlined + halt-inner-loop + sync PPU |
| `GBJIT_CHAIN_PREDICTOR_WAYS=1` | 56 | 66.1 | 76 | −1 | +0.3 | within noise — SML's hot inner loop alternates predictably so the 1-way prediction still wins ~80% of the time |
| `GBJIT_INLINE_JP_HL=0` | 56 | 65.9 | 76 | −1 | +0.1 | within noise — `JP HL` is rare in SML |
| `GBJIT_DISPATCHER_HALT_INNER_LOOP=0` | **48** | 56.6 | 75 | **−9** | **−9.2** | **clear regression**. SML spends most cycles in HALT; the tight inner loop is load-bearing |
| `GBJIT_FRAMEBUFFER_DOUBLE_BUFFER=1` | 58 | 65.6 | 76 | **+1** | −0.2 | small improvement in min (smooths the back-buffer memcpy across the frame) at the cost of 23 KB DRAM |
| `GBJIT_PPU_ASYNC=1` | **0** | 63.1 | 81 | −57 | −2.7 | **highest peak (81) but pathological dips**. OLED task on Core 1 gets starved during PPU bursts, dropping frames to 0-15 fps mid-run. Plus the known BG-correctness race |

## Conclusions

- **None of the toggles bring the minimum above 60 fps**. Baseline floor is 57 fps; double-buffering nudges it to 58 fps.
- `GBJIT_DISPATCHER_HALT_INNER_LOOP` is the single most important optimisation — turning it off costs nearly 10 fps minimum and 10 fps average.
- `GBJIT_CHAIN_PREDICTOR_WAYS=2` and `GBJIT_INLINE_JP_HL=1` are near-noise on SML. They presumably help more on Blargg or ROMs with denser conditional-branch hot loops.
- `GBJIT_FRAMEBUFFER_DOUBLE_BUFFER=1` is essentially free in throughput terms — the memcpy distributes the per-frame cost rather than concentrating it.
- `GBJIT_PPU_ASYNC=1` is **not safe in its current form** — it produces frame-rate stalls AND breaks BG scrolling. Needs the cycle-stamped IO write queue before it's usable.

## To get min > 60 fps

The toggle sweep shows there's no free lunch in the current option set. To raise the floor by another 3-5 fps would need new work:

1. **Lazy flag materialisation** (task #20) — most arithmetic-heavy SML inner loops carry the F-register update on every op. Defer it until a flag is consumed.
2. **Inline more SM83 ops** — `ADD HL, rr`, `RET cc`, `CALL cc` still fall through to `sm83_step`. Each is moderate per-call cost in the dispatch helper.
3. **Direct block linking** — at the end of one JIT block, jump directly to the next block's entry instead of returning to the dispatcher loop. Saves the ~30 Xtensa cycles of dispatcher overhead per chain hit.
4. **Tighter halt-loop cycle increment** — currently advances 4 GB cycles per iter; bumping to 16 or 64 cycles (with explicit IRQ-latency cap) would 4–16× the halt-loop throughput in exchange for slightly imprecise IRQ wake-up timing.

Reproduce: `python3 benchmark/bench_options_board.py` (board on `/dev/ttyUSB0`; export `GBJIT_PORT` to override).
