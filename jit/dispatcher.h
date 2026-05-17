#ifndef DISPATCHER_H
#define DISPATCHER_H

#include "gb_types.h"
#include "cpu_state.h"
#include "codecache.h"
#include "codegen.h"

#define GBJIT_BLOCK_BUCKETS 1024u

typedef struct gbjit_dispatcher {
    cpu_state  *cpu;
    codecache   cc;
    void       *arena;       /* backing memory (executable) for the codecache */
    u32         arena_cap;

    /* Block lookup: chained hash table indexed by gb_pc & (BUCKETS-1). */
    gbjit_block *buckets[GBJIT_BLOCK_BUCKETS];

    /* Stats. */
    u64 blocks_compiled;
    u64 blocks_executed;
    u64 cache_flushes;

    /* Falls back to interpreter when codegen unavailable. */
    bool interp_fallback;
} gbjit_dispatcher;

bool gbjit_dispatcher_init(gbjit_dispatcher *d, cpu_state *cpu);
void gbjit_dispatcher_shutdown(gbjit_dispatcher *d);

/* Run until cpu->cycles >= until. */
void gbjit_dispatcher_run_until(gbjit_dispatcher *d, u64 until);

#endif
