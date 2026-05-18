# JIT prefetch + evict policy investigation

Question: can we make the JIT faster by (a) prefetching more eagerly or
(b) evicting cached blocks when the arena fills, instead of falling
back to the interpreter?

Workload: SML, qemu-S3, 1 M GB-cycle window, GBJIT_PPU_ASYNC=0 for
determinism, one qemu instance per host CPU via `taskset -c`.

## Experiment 1 — prefetch depth sweep

Vary `prefetch_depth` ∈ {0, 1, 2, 4, 8} at three arena sizes. Dataset
in `prefetch_sweep.csv`.

| arena | d=0 | d=1 | d=2 | d=4 | d=8 |
|------:|----:|----:|----:|----:|----:|
| 48 KB | **19.43** | 18.90 | 18.50 | 15.32 | 18.53 |
| 96 KB | **24.18** | 23.84 | 23.37 | 22.26 | 21.93 |
| 192 KB | 22.23 | 21.92 | **23.03** | 21.89 | 22.06 |

Throughput in MHz. **depth=0 (prefetch off) wins or ties** in every
arena size. The prefetched-blocks counter grows linearly with depth
(d=8: 107 of 126 compiled blocks were prefetched at 96 KB, 85%), but
the throughput trends *downwards*. Two reasons:

1. The walker visits both `succ_pc[0]` (taken/CALL-target) and
   `succ_pc[1]` (fallthru/CALL-return). The fallthru of a conditional
   JR/JP is hit only when the branch is *not* taken — in tight loops
   that's the loop exit, executed once after thousands of iterations.
   Prefetching it eagerly is pure waste.
2. Each prefetched block does a full `gbjit_compile_block` pass —
   walks ops, emits code, writes the literal pool. At qemu speeds
   that's tens of µs per block; for 100+ prefetched blocks per
   benchmark run the overhead exceeds the saved compile cost on the
   chain-miss path.

**Conclusion (1):** the prefetcher does not pay for itself on steady-
state throughput. Its only remaining value is first-encounter latency
on cold code, and depth=4 walking both successors is the historical
default that minimises that without over-compiling.

## Experiment 2 — evict-on-fill policy

Added `evict_on_full`: when `codecache_alloc` returns NULL, the
dispatcher wipes every cached block + resets the codecache and retries
the failed compile. Without it, the failed compile falls back to
`sm83_step` for that PC.

Hypothesis: with eviction, even a too-small arena keeps running JIT
code; without it, large workloads degrade to interp.

The cache-sweep numbers with `evict_on_full=true` (vs. the historical
no-eviction baseline):

| arena | baseline jit | +evict | Δ |
|------:|-------------:|-------:|--:|
|  4 KB |  5.01 MHz | 2.30 MHz | **−54 %** |
|  8 KB |  6.60     | 2.88     | −56 % |
| 16 KB |  6.98     | 3.83     | −45 % |
| 32 KB |  6.79     | 10.07    | +48 % |
| 48 KB |  9.69     | 12.58    | +30 % |
| 64 KB | 11.42     | 12.56    | +10 % |
| 96 KB | 14.49     | 12.20    | −16 % |
| 128 KB| 13.98     | 12.41    | −11 % |
| 192 KB| 14.38     | 13.85    | −4 %  |

At small arenas the policy thrashes — each fresh compile fills the
arena, triggers a full wipe, and the next compile starts on an empty
arena (an entire 1 M-cycle benchmark window completes with 1 000+
compiles instead of the working set's 130). At large arenas the
working set fits without evictions, so the eviction path isn't even
reached — but the slightly different prefetch interaction costs a
few percent.

The sweet spot is 32–48 KB where the working set doesn't fit but is
close enough that ~3 evictions over the window amortise. This is
exactly the regime where falling back to interp would also be slow.

**Conclusion (2):** evict-on-fill is the right behaviour *only* when
the working set is just barely too large — a small operating range.
For SML's actual working set (~130 blocks at ~500 B each = ~60 KB) at
the default 64 KB arena, the policy is unnecessary. Kept as an opt-in
toggle (`d->evict_on_full = true`) for future workloads with > arena
working sets where the alternative is interp-fallback for every novel
PC, but **defaulted off** because it makes the small-arena case dra-
matically worse.

## Takeaways for tuning

- Don't increase the prefetch depth past 4. Walking deeper compiles
  blocks the bench rarely or never executes.
- Don't drop the prefetch to `succ_pc[0]` only — that path-prunes
  the CALL return point, which is otherwise only reachable via the
  dynamic RET dispatch and tanks warm-mode by 30-50 %.
- Don't auto-enable evict_on_fill. The aggressive policy is correct
  only in the narrow range where the working set exceeds the arena by
  ≤ ~2×. Outside that range it makes things worse.

The honest single biggest performance lever for this workload is
**arena size**: bumping from 64 KB to 96 KB lifts cold-JIT throughput
~25 % (11.4 → 14.5 MHz) and warm-JIT throughput stays at 25–27 MHz
across 64–192 KB. The prefetcher and evictor are second-order.
