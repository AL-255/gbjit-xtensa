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

## Does the generic self-loop subsume io-poll-halt? — No

A second sweep built with `is_io_poll_halt` disabled (`GBJIT_IO_POLL_HALT=0`),
leaving only the generic self-loop to carry Tetris's FF85 wait:

| io-poll-halt | mode 0 / 96 KB | mode 1 / 96 KB | mode 1 / 128 KB | mode 2 / 96 KB |
|--------------|---------------:|---------------:|----------------:|---------------:|
| on           | 165            | **264**        | **268**         | 191            |
| off          | 147            | 195            | 198             | 158            |

Disabling io-poll-halt costs **~26 %** on the best config. The generic
self-loop does *not* subsume it: io-poll-halt converts the FF85 wait
into a HALT fast-forward (skip ~1024 idle cycles per step), whereas the
generic self-loop *spins* the loop — 64 real block executions per outer
trip. For an IRQ-wait that idles thousands of cycles per frame,
fast-forwarding beats spinning. Both fast paths are therefore kept on
(`is_dec_a_loop` likewise — its O(1) closed form is also beyond what
spinning can do); the generic self-loop is the fallback for loop shapes
neither specific path matches.
