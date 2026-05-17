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
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(__linux__) && !defined(ESP_PLATFORM)
#include <sys/mman.h>
#define HAVE_MMAP_EXEC 1
#endif

#define ARENA_CAP_DEFAULT (64u * 1024u)

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

/* --- Helper resolver: on host, returns tokens; on target, real pointers.
 * The dispatcher owns the policy. */
static u32 host_helper_addr(helper_id id) { return (u32)id; }

/* Block linked-list pointer hack: we stash next-in-bucket inside chain_lit_off.
 * For the actual implementation, we use a separate `next` ptr. */
typedef struct dispatcher_bucket {
    gbjit_block *b;
    struct dispatcher_bucket *next;
} dispatcher_bucket;

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
    /* Walk buckets and free blocks. */
    for (u32 i = 0; i < GBJIT_BLOCK_BUCKETS; i++) {
        dispatcher_bucket *b = (dispatcher_bucket *)d->buckets[i];
        while (b) {
            dispatcher_bucket *next = b->next;
            gbjit_block_free(b->b);
            free(b);
            b = next;
        }
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
}

#if defined(ESP_PLATFORM)
/* On the target, execute the block by calling into it directly. */
typedef void (*block_entry_fn)(cpu_state *);
static void enter_block_native(gbjit_block *b, cpu_state *cpu) {
    block_entry_fn fn = (block_entry_fn)(b->code + b->entry_off);
    fn(cpu);
}
#else
/* On the host, drive the sim instead. The sim's CALLX0 dispatches via thunk
 * to the actual C helper. */
static void thunk_dispatch(xt_sim *s, u32 fn_token);
static u8 *thunk_translate(xt_sim *s, u32 addr);
static u32 thunk_read_literal(xt_sim *s, u32 addr);

typedef struct sim_context {
    cpu_state *cpu;
    gbjit_block *block;
} sim_context;

static u8 *thunk_translate(xt_sim *s, u32 addr) {
    /* The block's code is at offset 0 in our sim address space. Literal
     * loads land at offsets within the block's `code`. Stack accesses for
     * a1 use a separate scratch region. */
    sim_context *ctx = (sim_context *)s->user;
    /* If the address looks like a code-relative offset, it's within the
     * block. Otherwise it might be a stack address (we set a1 to point at
     * a host-local stack buffer). The sim represents addresses as 32-bit
     * "tokens" — we use a special "STACK_BASE" sentinel for stack. */
    if (addr < ctx->block->code_size) return ctx->block->code + addr;
    /* Stack region: sim a1 is set to 0x80000000; we route those accesses
     * to a static buffer. */
    static u8 stack_buf[256];
    if ((addr & 0xFFFFFF00u) == 0x80000000u) return stack_buf + (addr & 0xFF);
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
    switch ((helper_id)fn_token) {
        case HELPER_SM83_STEP: {
            sm83_step(ctx->cpu);
            s->a[2] = 0;
            return;
        }
        case HELPER_MMU_READ8: {
            /* a2 = mmu*, a3 = addr */
            u8 v = mmu_read8((mmu *)(uintptr_t)s->a[2], (u16)s->a[3]);
            s->a[2] = v;
            return;
        }
        case HELPER_MMU_WRITE8: {
            mmu_write8((mmu *)(uintptr_t)s->a[2], (u16)s->a[3], (u8)s->a[4]);
            return;
        }
        default: return;
    }
}

static void enter_block_sim(gbjit_block *b, cpu_state *cpu) {
    xt_sim s;
    xt_sim_init(&s, b->code + b->entry_off, b->code_size - b->entry_off);
    s.translate = thunk_translate;
    s.read_literal = thunk_read_literal;
    s.call_thunk = thunk_dispatch;

    sim_context ctx = { cpu, b };
    s.user = &ctx;

    /* Set up a1 = stack base sentinel, a2 = cpu, a0 = return sentinel. */
    s.a[0] = 0;                  /* sentinel — sim returns when RET is hit */
    s.a[1] = 0x80000080u;        /* stack pointer (mid-stack) */
    s.a[2] = (u32)(uintptr_t)cpu;/* truncated; we don't dereference on host */

    /* Cap steps generously: prologue + n_ops*5 + epilogue. */
    u32 cap = 64 + b->n_ops * 16;
    xt_sim_run(&s, cap);
}
#endif

void gbjit_dispatcher_run_until(gbjit_dispatcher *d, u64 until) {
    cpu_state *cpu = d->cpu;
    while (cpu->cycles < until) {
        if (d->interp_fallback) {
            sm83_step(cpu);
            continue;
        }
        gbjit_block *b = find_block(d, cpu->pc);
        if (!b) {
            b = gbjit_compile_block(&d->cc, cpu, cpu->pc, host_helper_addr);
            if (!b) {
                /* Codegen failed (cache full?) — interpret one step and retry. */
                sm83_step(cpu);
                continue;
            }
            insert_block(d, b);
            d->blocks_compiled++;
        }

#if defined(ESP_PLATFORM)
        enter_block_native(b, cpu);
#else
        enter_block_sim(b, cpu);
#endif
        d->blocks_executed++;

        if (cpu->halted || cpu->stopped) break;
    }
}
