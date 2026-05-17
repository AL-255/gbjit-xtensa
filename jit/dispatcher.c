/* Dispatcher: the loop that decides whether to interpret or JIT each block.
 *
 * On the host (x86), JIT-emitted Xtensa code cannot run natively, so blocks
 * are executed through xt_sim. On the ESP32-S3 target, the same blocks run
 * directly on the CPU via a CALL0 invocation.
 *
 * The block lookup is a simple chained hash table keyed by guest PC. */

#include "dispatcher.h"
#include "sm83_interp.h"
#include "memory.h"
#include "xtensa_sim.h"
#include "emit_xtensa.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(__linux__) && !defined(ESP_PLATFORM)
#include <sys/mman.h>
#define HAVE_MMAP_EXEC 1
#endif

#define ARENA_CAP_DEFAULT (64u * 1024u)

/* Host-side address-space sentinels. The JIT-emitted Xtensa code uses these
 * as L32R-loaded base addresses; the sim's translate() routes the range back
 * to the real cpu_state / mmu objects. On the ESP32-S3 target the literals
 * hold the actual pointers and these sentinels are unused. */
#define HOST_CPU_BASE 0xC0DE0000u
#define HOST_MMU_BASE 0xCDCD0000u
/* Stack region used by the sim for the prologue's frame allocation. */
#define HOST_STACK_BASE 0x80000000u
#define HOST_STACK_TOP  0x80000100u  /* a1 init points here */

static void *alloc_exec_arena(u32 cap) {
#if HAVE_MMAP_EXEC
    void *p = mmap(NULL, cap, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    return p;
#elif defined(ESP_PLATFORM)
    extern void *heap_caps_malloc(size_t, u32);
    return heap_caps_malloc(cap, (1u << 5) | (1u << 11) | (1u << 1));
#else
    return malloc(cap);
#endif
}

static void free_exec_arena(void *p, u32 cap) {
#if HAVE_MMAP_EXEC
    munmap(p, cap);
#else
    (void)cap; free(p);
#endif
}

/* Resolver for the literal pool. */
typedef struct host_resolver_ctx {
    cpu_state *cpu;
} host_resolver_ctx;

static u32 host_helper_addr(literal_id id, void *user) {
    (void)user;
    switch (id) {
        case ADDR_CPU_BASE: return HOST_CPU_BASE;
        case ADDR_MMU_BASE: return HOST_MMU_BASE;
        /* For function tokens, return the literal_id value itself so the
         * sim's call_thunk can dispatch via the same enum. */
        case HELPER_SM83_STEP:  return (u32)HELPER_SM83_STEP;
        case HELPER_MMU_READ8:  return (u32)HELPER_MMU_READ8;
        case HELPER_MMU_WRITE8: return (u32)HELPER_MMU_WRITE8;
        default: return 0;
    }
}

/* Linked-list bucket nodes for block lookup. */
typedef struct dispatcher_bucket {
    gbjit_block *b;
    struct dispatcher_bucket *next;
} dispatcher_bucket;

/* SMC page list nodes — one per (block, page) overlap. */
typedef struct smc_page_node {
    gbjit_block *b;
    struct smc_page_node *next;
} smc_page_node;

bool gbjit_dispatcher_init(gbjit_dispatcher *d, cpu_state *cpu) {
    memset(d, 0, sizeof(*d));
    d->cpu = cpu;
    d->arena_cap = ARENA_CAP_DEFAULT;
    d->arena = alloc_exec_arena(d->arena_cap);
    if (!d->arena) return false;
    codecache_init(&d->cc, (u8 *)d->arena, d->arena_cap);
    d->interp_fallback = false;
    return true;
}

void gbjit_dispatcher_shutdown(gbjit_dispatcher *d) {
    for (u32 i = 0; i < GBJIT_BLOCK_BUCKETS; i++) {
        dispatcher_bucket *b = (dispatcher_bucket *)d->buckets[i];
        while (b) {
            dispatcher_bucket *next = b->next;
            gbjit_block_free(b->b);
            free(b);
            b = next;
        }
        d->buckets[i] = NULL;
    }
    for (u32 p = 0; p < GBJIT_SMC_PAGE_COUNT; p++) {
        smc_page_node *n = (smc_page_node *)d->smc_pages[p];
        while (n) { smc_page_node *next = n->next; free(n); n = next; }
        d->smc_pages[p] = NULL;
    }
    if (d->arena) free_exec_arena(d->arena, d->arena_cap);
    d->arena = NULL;
}

static gbjit_block *find_block(gbjit_dispatcher *d, u16 pc) {
    u32 idx = pc & (GBJIT_BLOCK_BUCKETS - 1u);
    dispatcher_bucket *b = (dispatcher_bucket *)d->buckets[idx];
    while (b) {
        if (b->b->gb_pc_start == pc) return b->b;
        b = b->next;
    }
    return NULL;
}

static void insert_block(gbjit_dispatcher *d, gbjit_block *block) {
    u32 idx = block->gb_pc_start & (GBJIT_BLOCK_BUCKETS - 1u);
    dispatcher_bucket *b = (dispatcher_bucket *)calloc(1, sizeof(*b));
    b->b = block;
    b->next = (dispatcher_bucket *)d->buckets[idx];
    d->buckets[idx] = (gbjit_block *)b;

    /* Register the block on every 256-byte SMC page it overlaps. */
    u32 first_page = block->gb_pc_start >> GBJIT_SMC_PAGE_SHIFT;
    u32 last_page  = (block->gb_pc_end > 0 ? (u32)((block->gb_pc_end - 1) >> GBJIT_SMC_PAGE_SHIFT)
                                           : first_page);
    if (last_page >= GBJIT_SMC_PAGE_COUNT) last_page = GBJIT_SMC_PAGE_COUNT - 1;
    for (u32 p = first_page; p <= last_page; p++) {
        smc_page_node *node = (smc_page_node *)calloc(1, sizeof(*node));
        node->b = block;
        node->next = (smc_page_node *)d->smc_pages[p];
        d->smc_pages[p] = node;
    }
}

/* Remove all bucket entries pointing at `block` (block is being invalidated). */
static void remove_block_from_buckets(gbjit_dispatcher *d, gbjit_block *block) {
    u32 idx = block->gb_pc_start & (GBJIT_BLOCK_BUCKETS - 1u);
    dispatcher_bucket **pp = (dispatcher_bucket **)&d->buckets[idx];
    while (*pp) {
        if ((*pp)->b == block) {
            dispatcher_bucket *gone = *pp;
            *pp = gone->next;
            free(gone);
            return;
        }
        pp = &(*pp)->next;
    }
}

/* Walk every SMC page and prune entries pointing at the given block. */
static void remove_block_from_smc(gbjit_dispatcher *d, gbjit_block *block) {
    u32 first_page = block->gb_pc_start >> GBJIT_SMC_PAGE_SHIFT;
    u32 last_page  = (block->gb_pc_end > 0 ? (u32)((block->gb_pc_end - 1) >> GBJIT_SMC_PAGE_SHIFT)
                                           : first_page);
    if (last_page >= GBJIT_SMC_PAGE_COUNT) last_page = GBJIT_SMC_PAGE_COUNT - 1;
    for (u32 p = first_page; p <= last_page; p++) {
        smc_page_node **pp = (smc_page_node **)&d->smc_pages[p];
        while (*pp) {
            if ((*pp)->b == block) {
                smc_page_node *gone = *pp;
                *pp = gone->next;
                free(gone);
            } else {
                pp = &(*pp)->next;
            }
        }
    }
}

/* Clear any predicted_next pointers in the bucket table that reference
 * `block` — those would otherwise dangle after the block is freed. */
static void clear_dangling_predictions(gbjit_dispatcher *d, gbjit_block *block) {
    for (u32 i = 0; i < GBJIT_BLOCK_BUCKETS; i++) {
        dispatcher_bucket *bk = (dispatcher_bucket *)d->buckets[i];
        while (bk) {
            if (bk->b->predicted_next == block) {
                bk->b->predicted_next = NULL;
                bk->b->predicted_next_pc = 0;
            }
            bk = bk->next;
        }
    }
}

void gbjit_dispatcher_invalidate_addr(gbjit_dispatcher *d, u16 gb_addr) {
    u32 page = (u32)gb_addr >> GBJIT_SMC_PAGE_SHIFT;
    if (page >= GBJIT_SMC_PAGE_COUNT) return;
    smc_page_node *node = (smc_page_node *)d->smc_pages[page];
    d->smc_pages[page] = NULL;          /* detach list — we'll free as we go */
    while (node) {
        smc_page_node *next = node->next;
        gbjit_block *blk = node->b;
        remove_block_from_smc(d, blk);
        remove_block_from_buckets(d, blk);
        clear_dangling_predictions(d, blk);
        gbjit_block_free(blk);
        free(node);
        d->smc_invalidations++;
        node = next;
    }
}

#if defined(ESP_PLATFORM)
typedef void (*block_entry_fn)(cpu_state *);
static void enter_block_native(gbjit_block *b, cpu_state *cpu) {
    block_entry_fn fn = (block_entry_fn)(b->code + b->entry_off);
    fn(cpu);
}
#else

typedef struct sim_context {
    cpu_state *cpu;
    gbjit_block *block;
    u8 stack_buf[256];
} sim_context;

static u8 *thunk_translate(xt_sim *s, u32 addr) {
    sim_context *ctx = (sim_context *)s->user;
    if (addr >= HOST_CPU_BASE && addr < HOST_CPU_BASE + sizeof(cpu_state)) {
        return ((u8 *)ctx->cpu) + (addr - HOST_CPU_BASE);
    }
    if (addr >= HOST_MMU_BASE && addr < HOST_MMU_BASE + sizeof(mmu)) {
        return ((u8 *)ctx->cpu->mmu) + (addr - HOST_MMU_BASE);
    }
    if (addr < ctx->block->code_size) {
        /* In-block read (literal load handled by read_literal). */
        return ctx->block->code + addr;
    }
    if (addr >= HOST_STACK_BASE && addr < HOST_STACK_BASE + sizeof(ctx->stack_buf)) {
        return ctx->stack_buf + (addr - HOST_STACK_BASE);
    }
    return NULL;
}

static u32 thunk_read_literal(xt_sim *s, u32 addr) {
    sim_context *ctx = (sim_context *)s->user;
    if (addr + 4 > ctx->block->code_size) return 0;
    const u8 *p = ctx->block->code + addr;
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void thunk_dispatch(xt_sim *s, u32 fn_token) {
    sim_context *ctx = (sim_context *)s->user;
    switch ((literal_id)fn_token) {
        case HELPER_SM83_STEP: {
            sm83_step(ctx->cpu);
            return;
        }
        case HELPER_MMU_READ8: {
            u8 v = mmu_read8(ctx->cpu->mmu, (u16)s->a[3]);
            s->a[2] = v;
            return;
        }
        case HELPER_MMU_WRITE8: {
            mmu_write8(ctx->cpu->mmu, (u16)s->a[3], (u8)s->a[4]);
            return;
        }
        default: return;
    }
}

static void enter_block_sim(gbjit_block *b, cpu_state *cpu) {
    xt_sim s;
    xt_sim_init(&s, b->code, b->code_size);
    s.pc = b->entry_off;
    s.translate = thunk_translate;
    s.read_literal = thunk_read_literal;
    s.call_thunk = thunk_dispatch;

    static sim_context ctx_storage; /* avoid stack-pointer aliasing in tests */
    ctx_storage.cpu = cpu;
    ctx_storage.block = b;
    memset(ctx_storage.stack_buf, 0, sizeof(ctx_storage.stack_buf));
    s.user = &ctx_storage;

    s.a[0] = 0;                          /* sentinel — sim returns on RET */
    s.a[1] = HOST_STACK_TOP;
    s.a[2] = HOST_CPU_BASE;              /* in case the block reads a2 */

    /* Cap steps: prologue + body + epilogue. Real bound is hard to compute
     * up-front; use a generous multiplier. */
    u32 cap = 256 + b->n_ops * 64;
    xt_sim_run(&s, cap);
    if (s.status != XT_SIM_RETURNED) {
        fprintf(stderr, "[gbjit] block at GB pc=%04X stopped status=%d sim_pc=%u\n",
                b->gb_pc_start, (int)s.status, (unsigned)s.pc);
    }
}
#endif

void gbjit_dispatcher_run_until(gbjit_dispatcher *d, u64 until) {
    cpu_state *cpu = d->cpu;
    gbjit_block *prev = NULL;

    while (cpu->cycles < until) {
        if (d->interp_fallback) {
            sm83_step(cpu);
            continue;
        }

        /* Block chaining fast-path: if the previous block recorded its most
         * recent successor and the current PC matches the recorded target,
         * reuse the cached block pointer and skip the hash lookup. */
        gbjit_block *b = NULL;
        if (prev && prev->predicted_next && prev->predicted_next_pc == cpu->pc) {
            b = prev->predicted_next;
            d->chain_hits++;
        } else {
            b = find_block(d, cpu->pc);
            if (prev) d->chain_misses++;
        }

        if (!b) {
            host_resolver_ctx hr = { cpu };
            b = gbjit_compile_block(&d->cc, cpu, cpu->pc, host_helper_addr, &hr);
            if (!b) {
                sm83_step(cpu);
                prev = NULL;
                continue;
            }
            insert_block(d, b);
            d->blocks_compiled++;
        }

        /* Record the successor link on the previous block (regardless of
         * cache-hit status, so a stale link doesn't stick). */
        if (prev && (!prev->predicted_next || prev->predicted_next_pc != cpu->pc)) {
            prev->predicted_next = b;
            prev->predicted_next_pc = cpu->pc;
        }

#if defined(ESP_PLATFORM)
        enter_block_native(b, cpu);
#else
        enter_block_sim(b, cpu);
#endif
        d->blocks_executed++;
        prev = b;

        if (cpu->halted || cpu->stopped) break;
    }
}
