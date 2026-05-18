#ifndef DISPATCHER_H
#define DISPATCHER_H

#include "gb_types.h"
#include "cpu_state.h"
#include "codecache.h"
#include "codegen.h"

#define GBJIT_BLOCK_BUCKETS 1024u

/* SMC page granularity: 256 bytes per page → 256 pages cover 64 KB GB addr. */
#define GBJIT_SMC_PAGE_SHIFT 8
#define GBJIT_SMC_PAGE_COUNT 256

typedef struct gbjit_dispatcher {
    cpu_state  *cpu;
    codecache   cc;
    void       *arena;       /* backing memory (executable) for the codecache */
    u32         arena_cap;

    /* Block lookup: chained hash table indexed by gb_pc & (BUCKETS-1). */
    gbjit_block *buckets[GBJIT_BLOCK_BUCKETS];

    /* SMC page tracking. Each page holds a linked list of bucket nodes for
     * blocks whose [gb_pc_start, gb_pc_end) overlaps that page. */
    void *smc_pages[GBJIT_SMC_PAGE_COUNT];

    /* Stats. */
    u64 smc_invalidations;

    /* Stats. */
    u64 blocks_compiled;
    u64 blocks_executed;
    u64 cache_flushes;
    u64 chain_hits;
    u64 chain_misses;

    /* Falls back to interpreter when codegen unavailable. */
    bool interp_fallback;

    /* Demonstration toggle: when set, every dispatch iteration recompiles
     * the block from scratch — the cache (bucket table + predicted_next)
     * is bypassed and the codecache arena is wiped before each compile.
     * Used by the bench harness to quantify the JIT cache's value. */
    bool no_cache;

    /* Static-successor prefetcher. When `prefetch_enabled` is true, every
     * fresh compile is followed by recursive pre-compilation of the new
     * block's statically-known taken-target / fall-through PCs, up to
     * `prefetch_depth` levels (default 4). Disable by clearing
     * `prefetch_enabled` for benchmarking. */
    bool prefetch_enabled;
    u8   prefetch_depth;
    /* Stats. */
    u64  prefetched_blocks;        /* compiled by the prefetcher (not the hot path) */
    u64  prefetch_already_cached;  /* successor was already in the bucket table */

    /* Evict-on-fill policy. When `evict_on_full` is true, a failed
     * codecache allocation (arena full) triggers a full wipe of every
     * cached block + a codecache_reset; the dispatcher then retries
     * the compile into a fresh arena. Without this, a too-small arena
     * stays stuck at "no more compiles" and the dispatcher falls back
     * to the interp helper for every new PC.
     *
     * Hot blocks naturally re-warm after each wipe via the lazy chain-
     * miss compile path. The thrashing cost is bounded: each wipe lets
     * us compile ~arena_capacity_in_blocks fresh blocks before another
     * wipe, so the worst-case overhead is proportional to (working set
     * size / arena capacity in blocks). */
    bool evict_on_full;
    u64  arena_resets;
} gbjit_dispatcher;

bool gbjit_dispatcher_init(gbjit_dispatcher *d, cpu_state *cpu);
void gbjit_dispatcher_shutdown(gbjit_dispatcher *d);

/* Run until cpu->cycles >= until. */
void gbjit_dispatcher_run_until(gbjit_dispatcher *d, u64 until);

/* Drop every compiled block whose source range overlaps the 256-byte page
 * containing `gb_addr`. The dispatcher will re-compile fresh blocks the next
 * time those PCs are entered. Safe to call from anywhere; the affected
 * blocks must NOT be currently executing (the dispatcher's run loop is
 * single-threaded so this is naturally true between block exits). */
void gbjit_dispatcher_invalidate_addr(gbjit_dispatcher *d, u16 gb_addr);

#endif
