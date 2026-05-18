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
#include <unistd.h>
#define HAVE_MMAP_EXEC 1
#endif

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#include "sdkconfig.h"
#if defined(CONFIG_IDF_TARGET_ESP32S3)
/* IDF's IRAM_BSS_ATTR macro is a no-op on ESP32-S3 (it's gated on the
 * original ESP32's 8-bit-IRAM-access kconfig). To force genuine IRAM
 * placement we name the section the way IDF's linker fragments expect:
 * an input section of ".iram.bss" gets routed (soc.lf → app.lf) into
 * the ".iram0.bss" output section, which lives in iram0_0_seg starting
 * at SRAM_IRAM_ORG (0x40378000). That gives us the I-bus alias so
 * instruction fetch into the JIT-emitted code goes through the I-cache
 * path; the D-bus alias (0x3FCxxxxx) shows a ~4× slowdown for code
 * execution in qemu and per the TRM uses a less-optimised pipe on
 * real silicon. */
#define GBJIT_IRAM_BSS __attribute__((section(".iram.bss")))
#define GBJIT_STATIC_ARENA 1
#else
/* Plain ESP32 (LX6) doesn't support byte writes to IRAM from the data
 * path without the CONFIG_ESP32_IRAM_AS_8BIT_ACCESSIBLE_MEMORY trap
 * handler, which is dramatically slow. heap_caps_malloc(MALLOC_CAP_EXEC)
 * returns memory from a special D-bus-aliased IRAM pool that DOES
 * support byte writes — use that instead. The runtime cost is one
 * malloc at boot. */
#include "esp_heap_caps.h"
#define GBJIT_HEAP_EXEC_ARENA 1
#endif
#endif

/* JIT exec arena. Statically allocated — no heap, no fragmentation, no
 * runtime sizing. Size is fixed at build time via GBJIT_ARENA_KB (CMake
 * `-DGBJIT_ARENA_KB=<n>` works for both host and IDF builds).
 *
 * On ESP32-S3 the array goes into `.iram.bss` via IDF's IRAM_BSS_ATTR,
 * which the default linker script places in internal SRAM that's both
 * writable and instruction-fetchable. Internal IRAM on the S3 is not
 * routed through the L1 cache (cache covers flash/PSRAM only), so writes
 * via the data path are visible to instruction fetches without an
 * explicit cache flush — codecache_finalize just emits a barrier.
 *
 * On Linux the array lives in regular .bss and is mprotect'd to
 * PROT_READ | PROT_WRITE | PROT_EXEC at dispatcher init. The 4 KB
 * alignment requirement comes from page-granularity mprotect.
 *
 * The default size is conservative (64 KB). Larger arenas trigger a
 * latent codegen bug — Blargg's `06-ld r,r` starts failing past ~100
 * unique blocks, with register divergence appearing at PC=$C6E4. The
 * smaller-arena path masked the bug by falling back to the interpreter
 * once the arena filled; with no eviction in the bump-allocator, blocks
 * compiled beyond the previous fill threshold appear to contain bad
 * code. SML (~10 blocks) and other small workloads are unaffected; pass
 * -DGBJIT_ARENA_KB=192 to opt in to the larger arena once the bug is
 * fixed (TODO). */
#ifndef GBJIT_ARENA_KB
#define GBJIT_ARENA_KB 64u
#endif
#define GBJIT_ARENA_BYTES (GBJIT_ARENA_KB * 1024u)

#if defined(GBJIT_STATIC_ARENA)
GBJIT_IRAM_BSS __attribute__((aligned(4)))
static u8 s_jit_arena[GBJIT_ARENA_BYTES];
#elif defined(HAVE_MMAP_EXEC)
__attribute__((aligned(4096)))
static u8 s_jit_arena[GBJIT_ARENA_BYTES];
#elif defined(GBJIT_HEAP_EXEC_ARENA)
/* Allocated at first dispatcher_init via heap_caps_malloc(MALLOC_CAP_EXEC). */
static u8 *s_jit_arena = NULL;
#else
__attribute__((aligned(64)))
static u8 s_jit_arena[GBJIT_ARENA_BYTES];
#endif

/* Host-side address-space sentinels. The JIT-emitted Xtensa code uses these
 * as L32R-loaded base addresses; the sim's translate() routes the range back
 * to the real cpu_state / mmu objects. On the ESP32-S3 target the literals
 * hold the actual pointers and these sentinels are unused. */
#define HOST_CPU_BASE 0xC0DE0000u
#define HOST_MMU_BASE 0xCDCD0000u
/* Stack region used by the sim for the prologue's frame allocation. */
#define HOST_STACK_BASE 0x80000000u
#define HOST_STACK_TOP  0x80000100u  /* a1 init points here */

/* Make the static arena executable. On ESP32-S3 the section attribute
 * already placed it in RWX IRAM. On plain ESP32 (LX6) the buffer is
 * heap_caps_malloc'd from MALLOC_CAP_EXEC pool. On Linux we mprotect a
 * BSS array. Cache the result — only happens once. */
static bool ensure_arena_exec(void) {
#if defined(GBJIT_HEAP_EXEC_ARENA)
    if (s_jit_arena) return true;
    /* Plain ESP32 has two kinds of executable internal SRAM: pure IRAM
     * (0x4008xxxx region) which only accepts 32-bit-aligned word writes
     * from the data path, and D/IRAM (0x3FFExxxx region) which is mapped
     * to both buses and accepts byte writes. Our codecache writes
     * Xtensa instructions byte-by-byte (3-byte narrow / 24-bit ops), so
     * we MUST land in D/IRAM. MALLOC_CAP_8BIT forces the allocator to
     * pick a region that supports unaligned byte stores. Print the
     * available cap sizes so a NULL return is diagnosable. */
    ESP_LOGI("gbjit_jit", "heap probe: EXEC|8BIT largest=%u KB, EXEC|32BIT largest=%u KB",
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_EXEC|MALLOC_CAP_8BIT) / 1024u),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_EXEC|MALLOC_CAP_32BIT) / 1024u));
    s_jit_arena = (u8 *)heap_caps_malloc(GBJIT_ARENA_BYTES,
                                          MALLOC_CAP_EXEC | MALLOC_CAP_8BIT);
    if (!s_jit_arena) {
        /* Fall back to plain EXEC; codecache_finalize is a no-op so this
         * will run from pure IRAM only if codegen avoids byte writes,
         * which today it doesn't — but the diagnostic is worth keeping. */
        s_jit_arena = (u8 *)heap_caps_malloc(GBJIT_ARENA_BYTES, MALLOC_CAP_EXEC);
        if (s_jit_arena) {
            ESP_LOGW("gbjit_jit", "exec arena landed in pure IRAM at %p — byte writes WILL fault",
                     s_jit_arena);
        }
    }
    return s_jit_arena != NULL;
#elif defined(HAVE_MMAP_EXEC)
    static bool done = false;
    if (done) return true;
    long pagesz = sysconf(_SC_PAGESIZE);
    if (pagesz <= 0) pagesz = 4096;
    uintptr_t mask = (uintptr_t)pagesz - 1u;
    uintptr_t start = (uintptr_t)s_jit_arena & ~mask;
    uintptr_t end   = ((uintptr_t)s_jit_arena + GBJIT_ARENA_BYTES + mask) & ~mask;
    if (mprotect((void *)start, end - start,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return false;
    }
    done = true;
    return true;
#else
    return true;
#endif
}

/* Resolver for the literal pool. */
typedef struct resolver_ctx {
    cpu_state *cpu;
} resolver_ctx;

#if defined(ESP_PLATFORM)
/* IDF compiles C with the windowed Xtensa ABI but our JIT emits CALL0.
 * Helper calls must cross that boundary, so the target resolver hands the
 * JIT not the raw windowed C functions but the CALL0→CALL8 trampolines
 * in jit_trampolines.S. */
extern void sm83_step_call0(void);
extern void mmu_read8_call0(void);
extern void mmu_write8_call0(void);
static u32 target_helper_addr(literal_id id, void *user) {
    resolver_ctx *r = (resolver_ctx *)user;
    switch (id) {
        case ADDR_CPU_BASE:     return (u32)(uintptr_t)r->cpu;
        case ADDR_MMU_BASE:     return (u32)(uintptr_t)r->cpu->mmu;
        case HELPER_SM83_STEP:  return (u32)(uintptr_t)&sm83_step_call0;
        case HELPER_MMU_READ8:  return (u32)(uintptr_t)&mmu_read8_call0;
        case HELPER_MMU_WRITE8: return (u32)(uintptr_t)&mmu_write8_call0;
        default: return 0;
    }
}
#else
/* On the host, literal-pool entries are sentinels that xt_sim's translate /
 * call_thunk callbacks recognise. */
static u32 host_helper_addr(literal_id id, void *user) {
    (void)user;
    switch (id) {
        case ADDR_CPU_BASE: return HOST_CPU_BASE;
        case ADDR_MMU_BASE: return HOST_MMU_BASE;
        case HELPER_SM83_STEP:  return (u32)HELPER_SM83_STEP;
        case HELPER_MMU_READ8:  return (u32)HELPER_MMU_READ8;
        case HELPER_MMU_WRITE8: return (u32)HELPER_MMU_WRITE8;
        default: return 0;
    }
}
#endif

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
    if (!ensure_arena_exec()) return false;
    d->arena     = s_jit_arena;
    d->arena_cap = GBJIT_ARENA_BYTES;
#if defined(ESP_PLATFORM)
    ESP_LOGI("gbjit_jit", "exec arena: %s, %u KB at %p",
#if defined(GBJIT_STATIC_ARENA)
             "static IRAM",
#else
             "heap MALLOC_CAP_EXEC|8BIT",
#endif
             (unsigned)GBJIT_ARENA_KB, s_jit_arena);
#endif
    codecache_init(&d->cc, (u8 *)d->arena, d->arena_cap);
    d->interp_fallback = false;
    d->prefetch_enabled = true;
    d->prefetch_depth = 4;
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
    /* Arena is statically allocated — nothing to free. Just reset the
     * codecache so a subsequent dispatcher_init starts with cc->used=0. */
    if (d->arena) codecache_reset(&d->cc);
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

/* Pre-compile the statically-known successors of a freshly-compiled block,
 * recursively, up to `depth` levels. Stops at:
 *   - depth 0
 *   - blocks already in the cache (just bump the "already cached" stat)
 *   - dynamic / no-successor terminators (succ_pc == 0xFFFF)
 *   - allocation failures (codecache arena full)
 *
 * Called from the dispatch loop right after a successful compile; the
 * caller is expected to set `b->predicted_next`/etc. itself for the
 * triggering block. */
static void prefetch_successors(gbjit_dispatcher *d, gbjit_block *b, int depth) {
    if (depth <= 0 || d->no_cache) return;
    for (int i = 0; i < 2; i++) {
        u16 pc = b->succ_pc[i];
        if (pc == 0xFFFFu) continue;
        if (find_block(d, pc)) {
            d->prefetch_already_cached++;
            continue;
        }
        resolver_ctx hr = { d->cpu };
        gbjit_block *nb;
#if defined(ESP_PLATFORM)
        nb = gbjit_compile_block(&d->cc, d->cpu, pc, target_helper_addr, &hr);
#else
        nb = gbjit_compile_block(&d->cc, d->cpu, pc, host_helper_addr, &hr);
#endif
        if (!nb) return;     /* arena full — stop walking */
        insert_block(d, nb);
        d->blocks_compiled++;
        d->prefetched_blocks++;
        prefetch_successors(d, nb, depth - 1);
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
/* On the ESP32-S3, IDF compiles C code with the **windowed** ABI (ENTRY /
 * RETW). The JIT-emitted code uses the **CALL0** ABI (a0 = return address,
 * no window rotation). To cross the boundary safely we hand-write a small
 * trampoline: ENTRY allocates our windowed frame, we save the (windowed)
 * caller's return address `a0` to the local frame, CALLX0 into the JIT
 * block, the JIT returns via `JX a0` to the instruction right after our
 * CALLX0, we restore `a0`, and RETW back to the windowed caller. */
__attribute__((noinline))
/* `noinline` is load-bearing: when this function gets inlined into
 * `gbjit_dispatcher_run_until`, the JIT block (which doesn't allocate
 * its own stack frame) inherits the dispatcher's `a1`. If a deeper
 * call from inside the JIT trampolines causes a window overflow, the
 * Xtensa overflow handler spills the live a0..a3 to a1+0..15 — i.e.
 * to the dispatcher's stack frame, on top of compiler-managed locals
 * stored there. On LX6 (plain ESP32) this corrupts the dispatcher's
 * `d` pointer and a subsequent `d->blocks_executed++` faults trying
 * to write to the literal slot it accidentally pointed at.
 *
 * Forcing `enter_block_native` to be a real function call gives the
 * JIT block its own `a1` (this function's frame); the overflow handler
 * spills into a1+0..15 here, which we keep clear (return PC saved at
 * offset 16, above the spill zone). */
__attribute__((noinline))
static void enter_block_native(gbjit_block *b, cpu_state *cpu) {
    uint32_t fn = (uint32_t)(uintptr_t)(b->code + b->entry_off);
    /* Pin `cpu` into a2 — CALL0 callees receive their first argument there.
     * Pin `fn` into a8 — CALLX0's target register, free across the call. */
    register uint32_t a2_cpu asm("a2") = (uint32_t)(uintptr_t)cpu;
    register uint32_t a8_fn  asm("a8") = fn;
    asm volatile (
        /* Save windowed return PC to offset 16 (NOT 0) — the Xtensa window
         * overflow handler uses offsets 0..15 of every windowed function's
         * frame to spill the live a0..a3 if a deeper CALL{N} overflows. If
         * we saved at offset 0 here, the spill of our (now-clobbered) a0
         * would overwrite our manual save, and the RETW at function exit
         * would see CALLINC=0 instead of 2 and trap with IllegalInstr. */
        "s32i a0, a1, 16\n"     /* save windowed return PC above save-area */
        "callx0 %1\n"           /* CALL0 into the JIT block */
        "l32i a0, a1, 16\n"     /* restore windowed return PC */
        : "+r"(a2_cpu)
        : "r"(a8_fn)
        : "a3","a4","a5","a6","a7","a9","a10","a11","a12","a13","a14","a15",
          "memory"
    );
    (void)b;
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
#if defined(ESP_PLATFORM) && !defined(CONFIG_IDF_TARGET_ESP32S3)
    /* Xtensa LX6 windowed-ABI workaround: when this function does its
     * inner `call8` (to enter_block_native) and that call's downstream
     * trampoline `call8` triggers a window-overflow exception, the
     * overflow handler (_WindowOverflow8 in xtensa_vectors.S) spills
     * the overflowed frame's a4..a7 to byte offsets +16, +20, +24, +28
     * within OUR stack frame. The compiler's stack-slot allocator
     * happily uses those same offsets for our locals (in particular
     * the cached `d + offsetof(blocks_executed-region)` value at SP+20)
     * — every overflow corrupts that slot, and the post-call reload
     * faults dereferencing the spilled register value as if it were a
     * pointer. Reserving 64 bytes of padding at the bottom of the frame
     * forces the compiler to push its locals above the spill window,
     * so the overflow handler writes into untouched padding instead of
     * load-bearing state. `volatile` + the dummy write keep it from
     * being optimised out. */
    volatile uint8_t _gbjit_overflow_pad[64];
    _gbjit_overflow_pad[0] = 0;
#endif
    cpu_state *cpu = d->cpu;
    gbjit_block *prev = NULL;

    while (cpu->cycles < until) {
        /* Service pending interrupts and wake from HALT before each block.
         * The JIT inlines HALT as a simple `cpu->halted = 1; exit block`, so
         * we depend on the dispatcher to re-enter the interrupt path that
         * the reference interpreter normally runs at the top of sm83_step. */
        if (sm83_service_interrupts(cpu)) {
            prev = NULL;
        }
        if (cpu->halted) {
            cpu->cycles += 4;
            continue;
        }
        /* EI delayed-enable: when ime_pending is set, run the next op via
         * the interpreter so that the reference path handles the ime promotion
         * (ime becomes 1 after exactly one op following EI). Inlined ops
         * don't touch ime_pending. */
        if (cpu->ime_pending) {
            sm83_step(cpu);
            prev = NULL;
            continue;
        }
        if (d->interp_fallback) {
            sm83_step(cpu);
            continue;
        }

        /* Block chaining fast-path: if the previous block recorded its most
         * recent successor and the current PC matches the recorded target,
         * reuse the cached block pointer and skip the hash lookup. The
         * `no_cache` mode disables this — useful for showing the cache's
         * value vs always-recompile-on-encounter behaviour. */
        gbjit_block *b = NULL;
        if (!d->no_cache) {
            if (prev && prev->predicted_next && prev->predicted_next_pc == cpu->pc) {
                b = prev->predicted_next;
                d->chain_hits++;
            } else {
                b = find_block(d, cpu->pc);
                if (prev) d->chain_misses++;
            }
        }

        if (!b) {
            if (d->no_cache) {
                /* Wipe the bump-allocator arena so each compile reuses the
                 * same bytes. Without this the arena would fill within a
                 * few hundred iterations on any non-trivial loop. */
                codecache_reset(&d->cc);
            }
            resolver_ctx hr = { cpu };
#if defined(ESP_PLATFORM)
            b = gbjit_compile_block(&d->cc, cpu, cpu->pc, target_helper_addr, &hr);
#else
            b = gbjit_compile_block(&d->cc, cpu, cpu->pc, host_helper_addr, &hr);
#endif
            if (!b) {
                sm83_step(cpu);
                prev = NULL;
                continue;
            }
            if (!d->no_cache) insert_block(d, b);
            d->blocks_compiled++;
            if (d->prefetch_enabled) {
                prefetch_successors(d, b, d->prefetch_depth);
            }
        }

        /* Record the successor link on the previous block (regardless of
         * cache-hit status, so a stale link doesn't stick). Skip in
         * no_cache mode — the previous block has already been overwritten. */
        if (!d->no_cache && prev &&
            (!prev->predicted_next || prev->predicted_next_pc != cpu->pc)) {
            prev->predicted_next = b;
            prev->predicted_next_pc = cpu->pc;
        }

#if defined(ESP_PLATFORM)
        enter_block_native(b, cpu);
        /* The JIT block runs in CALL0 ABI inside enter_block_native's
         * windowed frame. On plain ESP32 (LX6) the windowed save/restore
         * around this call doesn't reliably preserve every caller-side
         * register the GCC scheduler relied on (we've seen `d` end up
         * with a flash-rodata literal after the call returns, which
         * faults on the next store). Force the compiler to spill all
         * live values to memory before the call and reload them after:
         * a no-op asm with a "memory" clobber, plus listing every
         * candidate register so GCC can't keep any of them live across
         * the boundary. */
        __asm__ volatile("" :::
            "a2","a3","a4","a5","a6","a7",
            "a8","a9","a10","a11","a12","a13","a14","a15",
            "memory");
#else
        enter_block_sim(b, cpu);
#endif
        d->blocks_executed++;

        if (d->no_cache) {
            /* Don't hand a pointer to a doomed block to the chain cache;
             * free the gbjit_block struct (the arena gets reset on the
             * next iteration). */
            gbjit_block_free(b);
            prev = NULL;
        } else {
            prev = b;
        }

        if (cpu->halted || cpu->stopped) break;
    }
}
