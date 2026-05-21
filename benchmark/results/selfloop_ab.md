# Generic self-loop — A/B (overhead check)

Same firmware config (Tetris, JIT mode 1 / coldness evict, 96 KB arena,
FPS probe), built with the generic self-loop fast path disabled vs
enabled, two reps each on the Heltec board:

| GBJIT_DISPATCHER_SELFLOOP | rep 1 | rep 2 |
|---------------------------|-------|-------|
| 0 (off)                   | 264   | 261   |
| 1 (on)                    | 264   | 261   |

**The self-loop fast path is overhead-neutral** — on vs off is identical
within the demo's run-to-run variance.

The earlier sweep's apparent "273 → 264" drop (commit `a4cc68c`) was
inter-run noise, *not* the self-loop: this A/B shows the self-loop-off
build also lands at 264. The benchmark metric (cumulative avg over a
~40 s Tetris attract-mode capture) varies a few percent run to run as
the demo plays slightly differently.

Why neutral rather than a win on Tetris: the specific HRAM-flag
io-poll-halt patch already fast-paths Tetris's one hot self-loop (~600×
hotter than any other block), so the generic path has nothing hot left
to accelerate here. It is retained because it is correct in all three
JIT modes and accelerates ROMs whose hot busy-loop is not the specific
HRAM-flag shape — at no measured cost when it has nothing to do.
