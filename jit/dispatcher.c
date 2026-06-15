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
#include "ppu.h"

/* Inline the tight HALT-and-wait-for-IRQ loop. Default on; flip to 0
 * to fall back to the naive "advance 4 cycles, full sm83_service_
 * interrupts call, continue" path. The tight loop is the main reason
 * sync-PPU mode is fast enough to be usable on SML. */
#ifndef GBJIT_DISPATCHER_HALT_INNER_LOOP
#define GBJIT_DISPATCHER_HALT_INNER_LOOP 1
#endif

/* GB cycles to advance per halt-loop iteration. SM83 HALT is
 * semantically "wait for IRQ", so each iter just bumps the cycle
 * counter and re-checks PPU/IRQ state. The fixed 4 below is the
 * real DMG cycle granularity, but for IRQ-bound waits the only
 * observable side effect of a larger step is IRQ latency: an IRQ
 * raised at GB cycle X is noticed up to STEP-1 cycles later. SML's
 * VBlank handler is ~5000 cycles long, so even a 256-cycle step
 * shifts wake-up by <5%. Larger step → fewer halt iters → less
 * dispatcher overhead → higher steady-state fps.
 *
 *   4   : DMG-accurate. Default.
 *   16  : 4x throughput in halt, IRQ accuracy still near-DMG.
 *   64  : 16x throughput, noticeable IRQ slop on timing-sensitive ROMs.
 *   256 : maximal, only safe on VBlank-only ROMs like SML.
 *
 * The step is also a lower bound on the ppu_next_event_cycles check
 * granularity, so very large values may delay scanline transitions.
 * Combined with ppu_tick's delta-driven catch-up this stays
 * functionally correct. */
#ifndef GBJIT_HALT_STEP_CYCLES
#define GBJIT_HALT_STEP_CYCLES 4
#endif
#include "xtensa_sim.h"
#include "emit_xtensa.h"
#include "gbjit_debug.h"
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
#include "esp_heap_caps.h"   /* heap_caps_malloc(MALLOC_CAP_SPIRAM) for the bbc */
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
extern void sm83_step_noirq_call0(void);
extern void mmu_read8_call0(void);
extern void mmu_write8_call0(void);
static u32 target_helper_addr(literal_id id, void *user) {
    resolver_ctx *r = (resolver_ctx *)user;
    switch (id) {
        case ADDR_CPU_BASE:     return (u32)(uintptr_t)r->cpu;
        case ADDR_MMU_BASE:     return (u32)(uintptr_t)r->cpu->mmu;
        /* Op-fallback uses the no-interrupt-service variant so the deep
         * service->ppu chain never runs in the block's borrowed window. */
        case HELPER_SM83_STEP:  return (u32)(uintptr_t)&sm83_step_noirq_call0;
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

/* Code-cache eviction callbacks — defined past the run-loop statics they
 * touch; see the definitions. One per policy (GBJIT_JIT_EVICT). */
#if GBJIT_JIT_EVICT == 1
static u32  dispatcher_evict_coldest(void *ctx);
#elif GBJIT_JIT_EVICT == 2
static void dispatcher_evict_range(void *ctx, u32 start, u32 end);
#endif

/* ---- PSRAM-backed compiled-block byte cache (bbc) --------------------------
 * Stores the emitted bytes of every block keyed by (pc, rom_bank). Blocks are
 * position-independent (only the soft predictor chains them, hard chaining is
 * unused), and their literal pool holds only stable absolute addresses (cpu /
 * mmu / helper pointers) plus block-relative L32R offsets — so the bytes can be
 * memcpy'd to any IRAM address and executed. On a compile miss we first try to
 * COPY a cached block into IRAM ("rehydrate") instead of recompiling; the
 * executable arena stays small (<= GBJIT_ARENA_KB, codegen-safe) while the byte
 * cache lives in roomy PSRAM. Definitions follow find_block; the data types and
 * bbc_create are needed by gbjit_dispatcher_init/shutdown below. */
#if defined(ESP_PLATFORM)
#define BBC_ALLOC(sz) heap_caps_malloc((sz), MALLOC_CAP_SPIRAM)
#define BBC_FREE(p)   heap_caps_free(p)
#else
#define BBC_ALLOC(sz) malloc(sz)
#define BBC_FREE(p)   free(p)
#endif

#ifndef GBJIT_BBC_STORE_KB
#define GBJIT_BBC_STORE_KB 512u
#endif
#ifndef GBJIT_BBC_MAX_ENTRIES
#define GBJIT_BBC_MAX_ENTRIES 12288u
#endif
#define GBJIT_BBC_BUCKETS 8192u   /* power of two */

typedef struct {
    u16 pc;
    u8  bank;
    u8  self_loop;
    u16 gb_pc_end;
    u16 succ0, succ1;
    u32 n_ops;
    u32 code_size;
    u32 entry_off;
    u32 store_off;
    int next;           /* hash chain: next entry index, or -1 */
} bbc_entry;

struct gbjit_bbc {
    bbc_entry *entries;
    u32        n_entries;
    u32        max_entries;
    u8        *store;
    u32        store_used;
    u32        store_cap;
    int        buckets[GBJIT_BBC_BUCKETS];
    bool       full;    /* entries or store exhausted -> stop caching */
};

static struct gbjit_bbc *bbc_create(void);

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
#if GBJIT_JIT_EVICT == 1
    /* Coldness: free-list allocator evicts our coldest cached block. */
    d->cc.evict     = dispatcher_evict_coldest;
    d->cc.evict_ctx = d;
#elif GBJIT_JIT_EVICT == 2
    /* Circular: ring allocator evicts whatever it is about to overwrite. */
    d->cc.evict_range = dispatcher_evict_range;
    d->cc.evict_ctx   = d;
#endif
    d->interp_fallback = false;
    /* PSRAM-backed compiled-block byte cache. Optional — on failure the
     * dispatcher just recompiles on every miss as before. */
    d->bbc = bbc_create();
#if defined(ESP_PLATFORM)
    ESP_LOGI("gbjit_jit", "block byte-cache: %s (%u KB store, %u entries)",
             d->bbc ? "PSRAM" : "DISABLED",
             (unsigned)GBJIT_BBC_STORE_KB, (unsigned)GBJIT_BBC_MAX_ENTRIES);
#endif
    /* Prefetch defaults — depth 4, walking both succ_pc[0] (the
     * taken / unconditional / CALL-target path) and succ_pc[1] (the
     * fallthru of conditional JR/JP and the return point of CALL).
     *
     * The benchmark/run_prefetch_sweep.sh sweep confirmed this is at
     * best a marginal steady-state win (often inside the noise vs.
     * depth=0) — its main benefit is first-encounter latency on cold
     * code. Specifically:
     *   - Skipping succ_pc[1] kills warm-mode performance because
     *     CALL return points are only reachable via dynamic RET,
     *     never via static prefetch.
     *   - depth=2 reduces eager compile work but loses ~3-7 % in
     *     several measured points.
     *   - depth=8 just wastes arena.
     * Kept at the historical default (4, both) — the empirical sweet
     * spot for SML's branching pattern. */
    d->prefetch_enabled = true;
    d->prefetch_depth = 4;
    /* Evict-on-fill defaults OFF. The simple "wipe and retry" policy
     * tested in the cache sweep was a net loss: tiny arenas (4-16 KB)
     * thrashed catastrophically (every fresh compile fills the arena,
     * triggers a wipe, repeat — 8x slower than falling back to interp);
     * medium/large arenas saw no benefit because SML's working set
     * (~130 blocks) fits comfortably in the 64 KB default. Kept as an
     * opt-in runtime toggle for future workloads whose working set
     * exceeds the arena, where the alternative (interp fallback for
     * every uncompiled block) would be worse than thrashing. */
    d->evict_on_full = false;

    /* Per-frame compile budget. Default from GBJIT_COMPILE_BUDGET (0 = off).
     * Tunable at runtime via the backend setter. See the field comment. */
#ifndef GBJIT_COMPILE_BUDGET
#define GBJIT_COMPILE_BUDGET 0
#endif
    d->compile_budget = GBJIT_COMPILE_BUDGET;
    d->compiles_this_run = 0;
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
    if (d->bbc) {
        if (d->bbc->entries) BBC_FREE(d->bbc->entries);
        if (d->bbc->store)   BBC_FREE(d->bbc->store);
        BBC_FREE(d->bbc);
        d->bbc = NULL;
    }
    /* Arena is statically allocated — nothing to free. Just reset the
     * codecache so a subsequent dispatcher_init starts with cc->used=0. */
    if (d->arena) codecache_reset(&d->cc);
    d->arena = NULL;
}

/* Look up a resident block by (pc, rom_bank). Keying on bank as well as pc lets
 * blocks compiled under different ROM banks at the same banked-window PC coexist,
 * removing the need to wipe the table on every MBC bank flip. `bank` is the
 * caller's wanted compile bank (bbc_bank_for); 0 for the fixed bank / RAM. */
static gbjit_block *find_block(gbjit_dispatcher *d, u16 pc, u8 bank) {
    u32 idx = pc & (GBJIT_BLOCK_BUCKETS - 1u);
    dispatcher_bucket *b = (dispatcher_bucket *)d->buckets[idx];
    while (b) {
        if (b->b->gb_pc_start == pc && b->b->rom_bank == bank) return b->b;
        b = b->next;
    }
    return NULL;
}

/* bbc data types + macros are defined above gbjit_dispatcher_init. */
static struct gbjit_bbc *bbc_create(void) {
#if defined(GBJIT_BBC_DISABLE)
    return NULL;   /* compile-time off: dispatcher recompiles on every miss */
#endif
    struct gbjit_bbc *c = (struct gbjit_bbc *)BBC_ALLOC(sizeof(*c));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    c->max_entries = GBJIT_BBC_MAX_ENTRIES;
    c->store_cap   = GBJIT_BBC_STORE_KB * 1024u;
    c->entries = (bbc_entry *)BBC_ALLOC(c->max_entries * sizeof(bbc_entry));
    c->store   = (u8 *)BBC_ALLOC(c->store_cap);
    if (!c->entries || !c->store) {
        if (c->entries) BBC_FREE(c->entries);
        if (c->store)   BBC_FREE(c->store);
        BBC_FREE(c);
        return NULL;
    }
    for (u32 i = 0; i < GBJIT_BBC_BUCKETS; i++) c->buckets[i] = -1;
    return c;
}

/* Bank a block at `pc` should be keyed under: the live ROM bank for the banked
 * window $4000..$7FFF, else 0 (fixed bank / RAM are bank-agnostic). */
static inline u8 bbc_bank_for(cpu_state *cpu, u16 pc) {
    return (pc >= 0x4000u && pc < 0x8000u) ? cpu->mmu->rom_bank : 0u;
}

static bbc_entry *bbc_get(struct gbjit_bbc *c, u16 pc, u8 bank) {
    int i = c->buckets[pc & (GBJIT_BBC_BUCKETS - 1u)];
    while (i >= 0) {
        bbc_entry *e = &c->entries[i];
        if (e->pc == pc && e->bank == bank) return e;
        i = e->next;
    }
    return NULL;
}

static void bbc_put(struct gbjit_bbc *c, const gbjit_block *b, u8 bank) {
    if (c->full) return;
    u32 sz = b->code_size;
    u32 aligned = (sz + 3u) & ~3u;
    if (c->n_entries >= c->max_entries || c->store_used + aligned > c->store_cap) {
        c->full = true;                 /* cache saturated — leave it stable */
        return;
    }
    u32 off = c->store_used;
    memcpy(c->store + off, b->code, sz);
    c->store_used += aligned;
    int idx = (int)c->n_entries++;
    bbc_entry *e = &c->entries[idx];
    e->pc = b->gb_pc_start; e->bank = bank; e->self_loop = b->self_loop;
    e->gb_pc_end = b->gb_pc_end; e->succ0 = b->succ_pc[0]; e->succ1 = b->succ_pc[1];
    e->n_ops = b->n_ops; e->code_size = sz; e->entry_off = b->entry_off;
    e->store_off = off;
    u32 h = e->pc & (GBJIT_BBC_BUCKETS - 1u);
    e->next = c->buckets[h]; c->buckets[h] = idx;
}

/* Drop every cached block whose start PC lies in the given 256-byte page —
 * used when self-modifying RAM code on that page is overwritten. (Banked-ROM
 * pages are never written, so this only fires for RAM pages.) */
static void bbc_invalidate_page(struct gbjit_bbc *c, u32 page) {
    u16 lo = (u16)(page << GBJIT_SMC_PAGE_SHIFT);
    u16 hi = (u16)(lo + (1u << GBJIT_SMC_PAGE_SHIFT) - 1u);
    for (u32 h = 0; h < GBJIT_BBC_BUCKETS; h++) {
        int *pp = &c->buckets[h];
        while (*pp >= 0) {
            bbc_entry *e = &c->entries[*pp];
            if (e->pc >= lo && e->pc <= hi) {
                e->pc = 0xFFFFu;        /* tombstone: unmatchable, keep store */
                *pp = e->next;
            } else {
                pp = &e->next;
            }
        }
    }
}

/* Drop only the cached blocks whose byte range [pc, gb_pc_end) overlaps the
 * written range [lo,hi] — the byte-range-precise counterpart of
 * bbc_invalidate_page. Used for SMC so a data write near (but not into) a
 * cached RAM code block doesn't evict it. */
static void bbc_invalidate_range(struct gbjit_bbc *c, u16 lo, u16 hi) {
    for (u32 h = 0; h < GBJIT_BBC_BUCKETS; h++) {
        int *pp = &c->buckets[h];
        while (*pp >= 0) {
            bbc_entry *e = &c->entries[*pp];
            u16 end = e->gb_pc_end ? e->gb_pc_end : (u16)(e->pc + 1u);
            if (e->pc <= hi && lo < end) {
                e->pc = 0xFFFFu;        /* tombstone: unmatchable, keep store */
                *pp = e->next;
            } else {
                pp = &e->next;
            }
        }
    }
}

/* Re-materialise a cached block: allocate IRAM, copy the stored bytes in, and
 * build a fresh block struct. Returns NULL if IRAM can't be allocated (caller
 * falls back to a full compile). */
static gbjit_block *bbc_rehydrate(gbjit_dispatcher *d, const bbc_entry *e) {
    gbjit_block *b = (gbjit_block *)calloc(1, sizeof(*b));
    if (!b) return NULL;
    u8 *code = codecache_alloc(&d->cc, e->code_size);
    if (!code) { free(b); return NULL; }
    memcpy(code, d->bbc->store + e->store_off, e->code_size);
    codecache_finalize(&d->cc, code, e->code_size);
    b->gb_pc_start = e->pc;
    b->gb_pc_end   = e->gb_pc_end;
    b->n_ops       = e->n_ops;
    b->code        = code;
    b->code_size   = e->code_size;
    b->entry_off   = e->entry_off;
    b->self_loop   = e->self_loop;
    b->succ_pc[0]  = e->succ0;
    b->succ_pc[1]  = e->succ1;
    b->rom_bank    = e->bank;
    return b;
}

static void insert_block(gbjit_dispatcher *d, gbjit_block *block) {
    u32 idx = block->gb_pc_start & (GBJIT_BLOCK_BUCKETS - 1u);
#if GBJIT_JIT_EVICT == 1
    /* Stamp it hot so a prefetch-driven compile right after can't pick a
     * just-built (never-executed) block as the coldest victim. */
    block->tag = ++d->jit_epoch;
#endif
    dispatcher_bucket *b = (dispatcher_bucket *)calloc(1, sizeof(*b));
    b->b = block;
    b->next = (dispatcher_bucket *)d->buckets[idx];
    d->buckets[idx] = (gbjit_block *)b;

    /* Register the block on every 256-byte SMC page it overlaps, and
     * tell the mmu that page now holds JIT code (state 0 → 1) so a
     * later write to it raises jit_smc_dirty. Leave state 2 alone — a
     * write may have landed between compile and insert. */
    u32 first_page = block->gb_pc_start >> GBJIT_SMC_PAGE_SHIFT;
    u32 last_page  = (block->gb_pc_end > 0 ? (u32)((block->gb_pc_end - 1) >> GBJIT_SMC_PAGE_SHIFT)
                                           : first_page);
    if (last_page >= GBJIT_SMC_PAGE_COUNT) last_page = GBJIT_SMC_PAGE_COUNT - 1;
    mmu *bm = (d->cpu) ? d->cpu->mmu : NULL;
    for (u32 p = first_page; p <= last_page; p++) {
        smc_page_node *node = (smc_page_node *)calloc(1, sizeof(*node));
        node->b = block;
        node->next = (smc_page_node *)d->smc_pages[p];
        d->smc_pages[p] = node;
        if (bm) {
            if (bm->jit_page_state[p] == 0u) bm->jit_page_state[p] = 1u;
            /* Grow the page's compiled-code byte-range box by this block's
             * intersection with the page, so smc_mark ignores data writes
             * outside it (the lever that stops code+data-page churn). */
            u16 p_base = (u16)(p << GBJIT_SMC_PAGE_SHIFT);
            u32 p_end  = (u32)p_base + (1u << GBJIT_SMC_PAGE_SHIFT);
            u16 lo_c = block->gb_pc_start > p_base ? block->gb_pc_start : p_base;
            u32 bend = block->gb_pc_end ? block->gb_pc_end
                                        : (u32)block->gb_pc_start + 1u;
            u16 hi_c = (u16)(bend < p_end ? bend : p_end);
            if (bm->jit_page_code_hi[p] == 0u) {
                bm->jit_page_code_lo[p] = lo_c;
                bm->jit_page_code_hi[p] = hi_c;
            } else {
                if (lo_c < bm->jit_page_code_lo[p]) bm->jit_page_code_lo[p] = lo_c;
                if (hi_c > bm->jit_page_code_hi[p]) bm->jit_page_code_hi[p] = hi_c;
            }
        }
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
            for (int s = 0; s < GBJIT_CHAIN_PREDICTOR_WAYS; s++) {
                if (bk->b->predicted_next[s] == block) {
                    bk->b->predicted_next[s] = NULL;
                    bk->b->predicted_next_pc[s] = 0;
                }
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
    /* Respect the per-frame compile budget: prefetch is speculative work, so it
     * is the first thing to yield when the frame's compile quota is spent. */
    if (d->compile_budget && d->compiles_this_run >= d->compile_budget) return;
    /* Walk BOTH succ_pc[0] (unconditional / taken / CALL-target) and
     * succ_pc[1] (JR-cc fallthru / CALL return point). Dropping the
     * second-successor walk was tempting — the depth=4 + both-succ
     * default did churn through fallthru blocks that the bench never
     * hit — but doing so collapsed CALL-return prefetching, which
     * tanked warm-mode throughput by ~50% in the medium-arena range
     * because the post-CALL block only ever reaches the cache via a
     * dynamic RET. Keeping both, dropping depth to 2 to bound the
     * tree size, is the empirical sweet spot. */
    for (int i = 0; i < 2; i++) {
        u16 pc = b->succ_pc[i];
        if (pc == 0xFFFFu) continue;
        if (find_block(d, pc, bbc_bank_for(d->cpu, pc))) {
            GBJIT_STAT_INC(d, prefetch_already_cached);
            continue;
        }
        resolver_ctx hr = { d->cpu };
        gbjit_block *nb;
#if defined(ESP_PLATFORM)
        nb = gbjit_compile_block(&d->cc, d->cpu, pc, target_helper_addr, &hr);
#else
        nb = gbjit_compile_block(&d->cc, d->cpu, pc, host_helper_addr, &hr);
#endif
        if (!nb) return;
        insert_block(d, nb);
        GBJIT_STAT_INC(d, blocks_compiled);
        GBJIT_STAT_INC(d, prefetched_blocks);
        d->compiles_this_run++;
        if (d->compile_budget && d->compiles_this_run >= d->compile_budget) return;
        prefetch_successors(d, nb, depth - 1);
    }
}

/* Drop every cached block + free the codecache arena. Called from the
 * dispatch loop when codecache_alloc returns NULL (arena full) — the
 * dispatcher then retries the failing compile into the now-empty
 * arena. Hot blocks naturally re-compile on chain miss the next time
 * they execute. */
static void evict_all(gbjit_dispatcher *d) {
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
    codecache_reset(&d->cc);
    GBJIT_STAT_INC(d, arena_resets);
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
#if GBJIT_JIT_EVICT
        /* Return the block's arena span to the free list so the bytes
         * are reusable — invalidation, unlike a bump-mode reset, frees
         * one block at a time. */
        codecache_free(&d->cc, (u32)(blk->code - d->cc.base), blk->code_size);
#endif
        gbjit_block_free(blk);
        free(node);
        GBJIT_STAT_INC(d, smc_invalidations);
        node = next;
    }
}

/* Byte-range-precise SMC invalidation: drop only the blocks on `page` whose
 * code range [gb_pc_start, gb_pc_end) overlaps the written range [lo,hi].
 * Blocks on the same page that were NOT touched survive (re-attached to the
 * page list). Returns the number of survivors — the caller leaves the page
 * marked as code (state 1) when any remain. This is what stops code+data
 * sharing a 256-byte page (HRAM/WRAM) from churning the JIT. */
static u32 invalidate_page_range(gbjit_dispatcher *d, u32 page, u16 lo, u16 hi) {
    smc_page_node *node = (smc_page_node *)d->smc_pages[page];
    d->smc_pages[page] = NULL;          /* detach; rebuild survivors below */
    smc_page_node *survivors = NULL;
    u32 nsurv = 0;
    /* Recompute the page's code box from the survivors so a write to the
     * just-invalidated bytes won't keep re-dirtying the page. */
    u16 p_base = (u16)(page << GBJIT_SMC_PAGE_SHIFT);
    u32 p_end  = (u32)p_base + (1u << GBJIT_SMC_PAGE_SHIFT);
    u16 box_lo = 0; u16 box_hi = 0;     /* hi==0 => empty */
    while (node) {
        smc_page_node *next = node->next;
        gbjit_block *blk = node->b;
        u16 end = blk->gb_pc_end ? blk->gb_pc_end : (u16)(blk->gb_pc_start + 1u);
        bool overlap = (blk->gb_pc_start <= hi) && (lo < end);
        if (overlap) {
            /* remove_block_from_smc prunes this block's nodes from ALL pages
             * (including survivor lists already written back for earlier
             * pages), so a multi-page block can't leave a dangling node. */
            remove_block_from_smc(d, blk);
            remove_block_from_buckets(d, blk);
            clear_dangling_predictions(d, blk);
#if GBJIT_JIT_EVICT
            codecache_free(&d->cc, (u32)(blk->code - d->cc.base), blk->code_size);
#endif
            gbjit_block_free(blk);
            free(node);
            GBJIT_STAT_INC(d, smc_invalidations);
        } else {
            node->next = survivors;
            survivors = node;
            nsurv++;
            /* Extend the recomputed code box by this survivor's page slice. */
            u16 lo_c = blk->gb_pc_start > p_base ? blk->gb_pc_start : p_base;
            u16 hi_c = (u16)((u32)end < p_end ? end : p_end);
            if (box_hi == 0u) { box_lo = lo_c; box_hi = hi_c; }
            else { if (lo_c < box_lo) box_lo = lo_c; if (hi_c > box_hi) box_hi = hi_c; }
        }
        node = next;
    }
    d->smc_pages[page] = survivors;
    if (d->cpu && d->cpu->mmu) {
        d->cpu->mmu->jit_page_code_lo[page] = box_lo;
        d->cpu->mmu->jit_page_code_hi[page] = box_hi;   /* 0 => page now empty */
    }
    return nsurv;
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
 * its own stack frame) inherits the dispatcher's `a1`. Forcing
 * `enter_block_native` to be a real function call gives the JIT block
 * its own `a1` (this function's frame).
 *
 * Saving the windowed return PC across the CALL0 is the subtle part.
 * The Xtensa window-overflow handler spills a function's a0..a3 to
 * *its own SP - 16* and (for a CALL8 frame) a4..a7 to *SP - 32* — i.e.
 * into the TOP of the callee's frame. `enter_block_native`'s natural
 * frame is just 32 bytes (`entry a1,32`, no locals), which means its
 * ENTIRE frame doubles as `run_until`'s a0..a7 spill area. Saving our
 * return PC anywhere in a 32-byte frame (the old code used offset 16)
 * lands it squarely on `run_until`'s spilled a0 slot: when a deep
 * helper call from inside a JIT block triggers a window overflow, the
 * handler and our manual save clobber each other, and the `retw` below
 * returns to a garbage PC — control flow derails and faults somewhere
 * unrelated (observed as a bogus `gbjit_block_free` crash).
 *
 * Fix: force a large frame with `pad[]` so there is real frame space
 * BELOW the caller's save area, and stash the return PC at the very
 * bottom (`a1 + 0`). The bottom of the frame is below run_until's spill
 * area (always the TOP 16/32/48 bytes), below our own a0..a7 spill
 * area (always BELOW our SP), and untouched by helper frames (also
 * below our SP) — the only safe a1-relative slot. `a1` is the one
 * register that survives the CALL0, so an a1-relative save is the only
 * option for restoring a0 after the call. */
__attribute__((noinline))
static void enter_block_native(gbjit_block *b, cpu_state *cpu) {
    uint32_t fn = (uint32_t)(uintptr_t)(b->code + b->entry_off);
    /* Padding to inflate our frame well past the caller's worst-case
     * save area (48 bytes for a CALL12 caller). The compiler places
     * locals BELOW that save area, so with this present `a1 + 0` is
     * guaranteed to sit in our own private frame space. `volatile` +
     * the "memory" clobber keep it from being optimised away. */
    volatile uint32_t pad[12];
    pad[0] = fn;
    /* Flush all register windows to the stack before running the JIT block.
     * The block runs in THIS function's borrowed window (reached by CALLX0, no
     * ENTRY of its own). A deep helper call issues a windowed `call8` from that
     * borrowed window; if the chain nests deep enough to wrap the Xtensa window
     * file, the register-window OVERFLOW spills frames to a1-relative save areas
     * and — because the block has no frame of its own — can clobber live data,
     * corrupting CPU state on real silicon (the host simulator models no
     * windowing, so it never reproduces this).
     *
     * The PRIMARY fix for the SML "Start -> BONUS GAME" corruption is to keep
     * the deepest helper chain out of the borrowed window entirely: the JIT
     * op-fallback now calls sm83_step_noirq (no leading sm83_service_interrupts
     * -> ppu_tick -> ppu_advance -> ppu_draw_line), since the dispatcher already
     * services interrupts between blocks at its own real frame. This spill is
     * kept as complementary defence-in-depth for the remaining shallower helper
     * paths (e.g. mmu_write8 to a PPU register -> ppu_flush). */
    extern void xthal_window_spill(void);
    xthal_window_spill();
    /* Pin `cpu` into a2 — CALL0 callees receive their first argument there.
     * Pin `fn` into a8 — CALLX0's target register, free across the call. */
    register uint32_t a2_cpu asm("a2") = (uint32_t)(uintptr_t)cpu;
    register uint32_t a8_fn  asm("a8") = fn;
    asm volatile (
        "s32i a0, a1, 0\n"      /* save windowed return PC at frame bottom */
        "callx0 %1\n"           /* CALL0 into the JIT block */
        "l32i a0, a1, 0\n"      /* restore windowed return PC */
        : "+r"(a2_cpu)
        : "r"(a8_fn)
        : "a3","a4","a5","a6","a7","a9","a10","a11","a12","a13","a14","a15",
          "memory"
    );
    (void)b;
    (void)pad;
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
            /* Match the device op-fallback (no leading interrupt service). */
            sm83_step_noirq(ctx->cpu);
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

    /* Cap steps: prologue + body + epilogue, plus headroom for any
     * internal back-edge loops the block may execute. A back-edge-
     * inlined HRAM polling loop can iterate hundreds of times inside
     * one call (e.g. SML's OAM-DMA wait counter from $28 down to 0).
     * 1M total Xtensa steps comfortably covers any sane CPU-only loop
     * while still catching genuinely runaway emitted code. The cap is
     * host-only; on the real chip the JIT block runs until its emit
     * exits naturally. */
    (void)b;
    u32 cap = 1u << 20;   /* ~1 M Xtensa instructions per block call */
    xt_sim_run(&s, cap);
    if (s.status != XT_SIM_RETURNED) {
        fprintf(stderr, "[gbjit] block at GB pc=%04X stopped status=%d sim_pc=%u\n",
                b->gb_pc_start, (int)s.status, (unsigned)s.pc);
    }
}
#endif

/* Branch-prediction macros for the dispatcher hot loop. These are
 * compile-time hints only — GCC uses them to lay out the generated
 * code so the predicted path is straight-line and the unlikely arms
 * become branch-taken-cold. No runtime behavioural change. */
#ifndef likely
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

/* Hot-loop state for gbjit_dispatcher_run_until, parked in .bss rather
 * than on the stack.
 *
 * The dispatcher calls into JIT-emitted code through enter_block_native,
 * a windowed-ABI `call8`. Deep helper-call nesting under the JIT block
 * can trigger an Xtensa window-overflow exception; the overflow handler
 * spills the overflowed frames' registers into stack-frame save areas.
 * On ESP32-S3 those spill offsets overlap whatever stack slots the
 * compiler picked for run_until's live locals — so the cached `d`
 * pointer would come back corrupted after the call (classic symptom:
 * `d` reloads as a flash address 0x3C02xxxx or near-NULL, and the next
 * `d->blocks_executed++` faults with LoadProhibited / LoadStorePIFAddr).
 *
 * A stack-padding workaround didn't reliably move the slots clear of
 * the spill window. Keeping the state in .bss instead is robust: the
 * window-overflow handler only ever writes stack frames, never .bss,
 * so these survive the call and the post-call reload is always sane.
 * run_until is not reentrant (one dispatch loop per core), so file-
 * static storage is fine. */
static gbjit_dispatcher *s_run_d;
static cpu_state        *s_run_cpu;
static gbjit_block      *s_run_prev;
static u64               s_run_until;

/* Both eviction callbacks run inside gbjit_compile_block, between block
 * executions, so no evicted block can be running. The one block reachable
 * from outside the bucket table is the run loop's `s_run_prev` (chain
 * predecessor); null it if it is a victim so the post-compile chain
 * update can't touch freed memory. */
#if GBJIT_JIT_EVICT == 1
/* Coldness: the free-list allocator calls this when no free span fits.
 * Drop the coldest cached block (lowest last-use tag — approximate LRU)
 * and hand its span back. Returns bytes freed, or 0 if nothing is left
 * to evict (request larger than the whole arena → interp fallback). */
static u32 dispatcher_evict_coldest(void *ctx) {
    gbjit_dispatcher *d = (gbjit_dispatcher *)ctx;
    gbjit_block *victim = NULL;
    for (u32 i = 0; i < GBJIT_BLOCK_BUCKETS; i++) {
        for (dispatcher_bucket *bk = (dispatcher_bucket *)d->buckets[i];
             bk; bk = bk->next) {
            if (!victim || bk->b->tag < victim->tag) victim = bk->b;
        }
    }
    if (!victim) return 0;
    u32 off  = (u32)(victim->code - d->cc.base);
    u32 size = victim->code_size;
    remove_block_from_smc(d, victim);
    remove_block_from_buckets(d, victim);
    clear_dangling_predictions(d, victim);
    if (victim == s_run_prev) s_run_prev = NULL;
    gbjit_block_free(victim);
    codecache_free(&d->cc, off, size);
    return size;
}
#elif GBJIT_JIT_EVICT == 2
/* Circular: the ring allocator calls this with the arena byte range it
 * is about to overwrite. Drop every cached block overlapping it (the
 * oldest-compiled run). No tag, no policy — pure FIFO by ring position. */
static void dispatcher_evict_range(void *ctx, u32 start, u32 end) {
    gbjit_dispatcher *d = (gbjit_dispatcher *)ctx;
    for (;;) {
        gbjit_block *victim = NULL;
        for (u32 i = 0; i < GBJIT_BLOCK_BUCKETS && !victim; i++) {
            for (dispatcher_bucket *bk = (dispatcher_bucket *)d->buckets[i];
                 bk; bk = bk->next) {
                u32 o = (u32)(bk->b->code - d->cc.base);
                if (o < end && o + bk->b->code_size > start) {
                    victim = bk->b;
                    break;
                }
            }
        }
        if (!victim) break;
        remove_block_from_smc(d, victim);
        remove_block_from_buckets(d, victim);
        clear_dangling_predictions(d, victim);
        if (victim == s_run_prev) s_run_prev = NULL;
        gbjit_block_free(victim);
    }
}
#endif

__attribute__((hot))
void gbjit_dispatcher_run_until(gbjit_dispatcher *d_param, u64 until_param) {
    s_run_d     = d_param;
    s_run_until = until_param;
    s_run_cpu   = d_param->cpu;
    s_run_prev  = NULL;
    d_param->compiles_this_run = 0;   /* reset per-frame compile budget */
#define d     s_run_d
#define until s_run_until
#define cpu   s_run_cpu
#define prev  s_run_prev

    while (cpu->cycles < until) {
        /* MBC bank switched since last iteration. Blocks from the banked ROM
         * window ($4000..$7FFF) are (pc,rom_bank)-keyed (see find_block), so
         * old- and new-bank blocks coexist — the next lookup just uses the new
         * bank's key. No wipe: the old bank's blocks stay resident and are
         * re-found instantly on the next flip back (the coldness evictor
         * reclaims them only if the arena needs the space). This kills the
         * dominant scroll thrash — SML ping-pongs between 2 banks and the old
         * wipe re-invalidated + re-rehydrated ~60 blocks per flip. */
        if (unlikely(cpu->mmu->rom_bank_dirty)) {
            d->bank_flips++;
            cpu->mmu->rom_bank_dirty = 0;
            prev = NULL;   /* predicted target may be a different bank now */
        }
        /* Self-modifying code: a RAM page that a JIT block was compiled
         * from has since been written (mmu_write8 set jit_page_state to
         * 2 and raised jit_smc_dirty). Invalidate every block on each
         * such page so the next dispatch recompiles from the new bytes.
         * Games that run + rewrite code in WRAM/HRAM (blargg's per-
         * instruction test runner) depend on this; without it the JIT
         * executes a stale translation of the old code. */
        if (unlikely(cpu->mmu->jit_smc_dirty)) {
            d->smc_flushes++;
            u8 *st = cpu->mmu->jit_page_state;
            for (u32 p = 0; p < GBJIT_SMC_PAGE_COUNT; p++) {
                if (st[p] == 2u) {
                    /* Byte-range-precise: only invalidate blocks whose code
                     * actually overlaps the addresses written this round, so a
                     * data write that merely shares a 256-byte page with code
                     * (HRAM/WRAM) doesn't evict the code. */
                    u16 wlo = cpu->mmu->jit_page_wlo[p];
                    u16 whi = cpu->mmu->jit_page_whi[p];
                    u32 survivors = invalidate_page_range(d, p, wlo, whi);
                    /* Drop only the byte-cache copies in the written range — the
                     * code there changed; rehydrating it would run stale bytes.
                     * (Banked-ROM pages are never written, so RAM SMC only.) */
                    if (d->bbc) bbc_invalidate_range(d->bbc, wlo, whi);
                    /* Keep the page marked as code if untouched blocks remain;
                     * otherwise clear it (insert_block re-marks on recompile). */
                    st[p] = survivors ? 1u : 0u;
                }
            }
            cpu->mmu->jit_smc_dirty = 0;
            prev = NULL;
        }
        /* Service pending interrupts and wake from HALT before each block.
         * The JIT inlines HALT as a simple `cpu->halted = 1; exit block`,
         * so we depend on the dispatcher to re-enter the interrupt path
         * that the reference interpreter normally runs at the top of
         * sm83_step.
         *
         * Fast-path skip (GBJIT_DISPATCHER_SERVICE_SHORTCUT, default 1):
         * if the PPU deadline hasn't been crossed, LCDC is unchanged,
         * no IRQ is pending, and we're not in any of the slow-path
         * states (halted, ime_pending), there's no work for
         * sm83_service_interrupts to do — skip the call. ppu_tick
         * itself has an internal early return for the same conditions,
         * but a function call to find that out still costs the entry
         * and the IF/IE/halt branches inside the function. The inline
         * version below is ~5 instructions and lets the compiler keep
         * the cpu_state base in a register across iterations. */
#ifndef GBJIT_DISPATCHER_SERVICE_SHORTCUT
#define GBJIT_DISPATCHER_SERVICE_SHORTCUT 0
#endif
#if GBJIT_DISPATCHER_SERVICE_SHORTCUT
        {
            mmu *_m = cpu->mmu;
            bool _need_service = cpu->halted
                || cpu->ime_pending
                || gb_cycles_reached(cpu->cycles, _m->ppu_next_event_cycles)
                || _m->io[0x40] != _m->ppu_last_lcdc
                || (_m->io[0x0F] & _m->ie & 0x1Fu) != 0;
            if (unlikely(_need_service)) {
                if (sm83_service_interrupts(cpu)) prev = NULL;
            }
        }
#else
        if (unlikely(sm83_service_interrupts(cpu))) {
            prev = NULL;
        }
#endif
        if (unlikely(cpu->halted)) {
#if GBJIT_DISPATCHER_HALT_INNER_LOOP
            /* Tight halt loop. SML and similar HALT-and-wait-for-VBlank
             * games spend most of their wall time here, and the
             * sm83_service_interrupts call above is most of the
             * per-iter cost. Stay inside this loop until either an IRQ
             * is pending, the PPU's next-event deadline is reached
             * (state machine might raise IF), or the dispatcher's
             * cycle budget runs out. On each pass we advance
             * cpu->cycles by 4 (the JIT's HALT cycle cost) and run the
             * cheap-fast-path checks inline; the heavy ppu_tick + IRQ
             * dispatch only fire when one of those conditions hits.
             *
             * Compile-time gated: -DGBJIT_DISPATCHER_HALT_INNER_LOOP=0
             * falls back to the naive "advance 4, continue" path. */
            mmu *m_halt = cpu->mmu;
            do {
                cpu->cycles += GBJIT_HALT_STEP_CYCLES;
                if (gb_cycles_reached(cpu->cycles, m_halt->ppu_next_event_cycles)
                        || m_halt->io[0x40] != m_halt->ppu_last_lcdc) {
                    ppu_tick(cpu);
                }
                if ((m_halt->io[0x0F] & m_halt->ie & 0x1Fu) != 0) {
                    if (sm83_service_interrupts(cpu)) prev = NULL;
                    break;
                }
            } while (cpu->halted && cpu->cycles < until);
            continue;
#else
            cpu->cycles += GBJIT_HALT_STEP_CYCLES;
            continue;
#endif
        }
        /* EI delayed-enable: when ime_pending is set, run the next op via
         * the interpreter so that the reference path handles the ime promotion
         * (ime becomes 1 after exactly one op following EI). Inlined ops
         * don't touch ime_pending. */
        if (unlikely(cpu->ime_pending)) {
            sm83_step(cpu);
            prev = NULL;
            continue;
        }
        if (unlikely(d->interp_fallback)) {
            sm83_step(cpu);
            continue;
        }

        /* Block chaining fast-path: if the previous block recorded its most
         * recent successor and the current PC matches the recorded target,
         * reuse the cached block pointer and skip the hash lookup. The
         * `no_cache` mode disables this — useful for showing the cache's
         * value vs always-recompile-on-encounter behaviour. */
        gbjit_block *b = NULL;
        if (likely(!d->no_cache)) {
            /* Bank the next block must match: blocks are (pc,bank)-keyed, so a
             * cached pointer / bucket entry for a different ROM bank at this PC
             * must NOT be reused (it would run the wrong bank's baked-in
             * immediates — silent corruption). */
            u8 want_bank = bbc_bank_for(cpu, cpu->pc);
            if (likely(prev)) {
                /* N-way predicted-next cache. The first slot is the
                 * hot path; on a typical conditional branch alternating
                 * between two successors, the post-update logic below
                 * keeps the most-recently-taken target in slot 0, so
                 * the common case is just one compare. */
                bool hit = false;
                for (int _i = 0; _i < GBJIT_CHAIN_PREDICTOR_WAYS; _i++) {
                    if (prev->predicted_next_pc[_i] == cpu->pc
                            && prev->predicted_next[_i]
                            && prev->predicted_next[_i]->rom_bank == want_bank) {
                        b = prev->predicted_next[_i];
                        hit = true;
                        break;
                    }
                }
                if (likely(hit)) {
                    GBJIT_STAT_INC(d, chain_hits);
                } else {
                    b = find_block(d, cpu->pc, want_bank);
                    GBJIT_STAT_INC(d, chain_misses);
                }
            } else {
                b = find_block(d, cpu->pc, want_bank);
            }
        }

        if (unlikely(!b)) {
            /* PSRAM byte-cache fast path: if this (pc,bank) block was compiled
             * before, re-materialise it by COPY into IRAM instead of paying a
             * full recompile. This is what turns the eviction / bank-flip
             * churn into cheap copies. */
            if (likely(d->bbc && !d->no_cache)) {
                bbc_entry *_e = bbc_get(d->bbc, cpu->pc, bbc_bank_for(cpu, cpu->pc));
                if (_e) {
                    b = bbc_rehydrate(d, _e);
                    if (b) {
                        insert_block(d, b);
                        d->bbc_hits++;
#ifdef GBJIT_BBC_VERIFY
                        {
                            static u8 vbuf[65536] __attribute__((aligned(8)));
                            codecache tcc; codecache_init(&tcc, vbuf, sizeof(vbuf));
                            resolver_ctx vhr = { cpu };
#if defined(ESP_PLATFORM)
                            gbjit_block *fr = gbjit_compile_block(&tcc, cpu, cpu->pc, target_helper_addr, &vhr);
#else
                            gbjit_block *fr = gbjit_compile_block(&tcc, cpu, cpu->pc, host_helper_addr, &vhr);
#endif
                            if (fr) {
                                if (fr->code_size != b->code_size) {
                                    fprintf(stderr, "[bbc-verify] pc=%04X bank=%u SIZE rehy=%u fresh=%u\n",
                                            cpu->pc, _e->bank, b->code_size, fr->code_size);
                                } else if (memcmp(fr->code, b->code, fr->code_size) != 0) {
                                    u32 o=0; while (o<fr->code_size && fr->code[o]==b->code[o]) o++;
                                    fprintf(stderr, "[bbc-verify] pc=%04X bank=%u DIFF@+%u rehy=%02X fresh=%02X sz=%u entoff=%u/%u\n",
                                            cpu->pc, _e->bank, o, b->code[o], fr->code[o], fr->code_size,
                                            b->entry_off, fr->entry_off);
                                }
                                gbjit_block_free(fr);
                            }
                        }
#endif
                    }
                }
            }
            if (unlikely(!b)) {
                /* Per-frame compile budget: once this run has compiled its quota
                 * of fresh blocks, interpret the overflow instead of paying more
                 * compile cost this frame. The block compiles on a later frame
                 * when budget is available, spreading a burst (level load) so no
                 * single frame stalls. Rehydration above is cheap and already
                 * handled it if possible. */
                if (d->compile_budget
                        && d->compiles_this_run >= d->compile_budget) {
                    sm83_step(cpu);
                    GBJIT_STAT_INC(d, interp_steps);
                    prev = NULL;
                    continue;
                }
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
                /* Arena full → evict-all + retry once. With evict_on_full
                 * the dispatcher prefers wiping the cache over falling
                 * back to the interp helper; hot blocks will lazy-recompile
                 * on chain miss. Disable evict_on_full to get the old
                 * fixed-size fixed-content behaviour. */
                if (!b && d->evict_on_full) {
                    evict_all(d);
                    prev = NULL;
#if defined(ESP_PLATFORM)
                    b = gbjit_compile_block(&d->cc, cpu, cpu->pc, target_helper_addr, &hr);
#else
                    b = gbjit_compile_block(&d->cc, cpu, cpu->pc, host_helper_addr, &hr);
#endif
                }
                if (!b) {
                    sm83_step(cpu);
                    GBJIT_STAT_INC(d, interp_steps);
                    prev = NULL;
                    continue;
                }
                if (!d->no_cache) insert_block(d, b);
                GBJIT_STAT_INC(d, blocks_compiled);
                d->compiles_this_run++;
                /* Persist the freshly-emitted bytes so future evictions/bank
                 * flips can rehydrate by copy. */
                if (d->bbc && !d->no_cache) {
                    bbc_put(d->bbc, b, bbc_bank_for(cpu, cpu->pc));
                    d->bbc_misses++;
                }
                if (d->prefetch_enabled) {
                    prefetch_successors(d, b, d->prefetch_depth);
                }
            }
        }

        /* Record the successor link on the previous block (regardless
         * of cache-hit status, so a stale link doesn't stick). Skip in
         * no_cache mode — the previous block has already been
         * overwritten. If the target already lives in one of the slots,
         * leave everything alone; otherwise overwrite the round-robin
         * victim. */
        if (!d->no_cache && prev) {
            bool present = false;
            for (int _i = 0; _i < GBJIT_CHAIN_PREDICTOR_WAYS; _i++) {
                if (prev->predicted_next_pc[_i] == cpu->pc
                        && prev->predicted_next[_i] == b) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                u8 v = (u8)(prev->predicted_next_victim
                            % GBJIT_CHAIN_PREDICTOR_WAYS);
                prev->predicted_next[v] = b;
                prev->predicted_next_pc[v] = cpu->pc;
                prev->predicted_next_victim = (u8)(v + 1u);
            }
        }

#if GBJIT_JIT_EVICT == 1
        b->tag = ++d->jit_epoch;          /* mark hot for the evictor */
#endif
        u16 sp_before_block = cpu->sp;
#if defined(ESP_PLATFORM)
        enter_block_native(b, cpu);
        __asm__ volatile("" :::
            "a2","a3","a4","a5","a6","a7",
            "a8","a9","a10","a11","a12","a13","a14","a15",
            "memory");
#else
        enter_block_sim(b, cpu);
#endif
        GBJIT_STAT_INC(d, blocks_executed);

        if (d->no_cache) {
            gbjit_block_free(b);
            prev = NULL;
        } else {
            prev = b;
        }

        /* Self-loop fast path. A block flagged `self_loop` at compile
         * time — a conditional JR back to its own start with a
         * register/HRAM-pure body — is a busy-wait or delay loop. While
         * it stays taken, re-enter the same already-compiled block
         * directly, up to GBJIT_SELFLOOP_MAX times, skipping the
         * outer-iteration preamble.
         *
         * This is deliberately the *exact* structure of the block-batch
         * below — enter_block in a tight loop, no interleaved C call —
         * so it carries the chain-batch's proven safety. The PPU is not
         * ticked inside; the bounded spin leaves it at most a batch's
         * worth of cycles stale, which the outer loop's service catches
         * up on exit. The compile-time flag already excludes LY/STAT
         * polls (which must see the PPU advance) and stack ops; entry
         * is further gated on a clean state. GBJIT_DISPATCHER_SELFLOOP=0
         * disables it. */
#ifndef GBJIT_DISPATCHER_SELFLOOP
#define GBJIT_DISPATCHER_SELFLOOP 1
#endif
#ifndef GBJIT_SELFLOOP_MAX
#define GBJIT_SELFLOOP_MAX 64
#endif
#if GBJIT_DISPATCHER_SELFLOOP
        if (likely(prev && !d->no_cache) && b->self_loop
                && cpu->pc == b->gb_pc_start
                && cpu->sp == sp_before_block
                && !cpu->halted && !cpu->ime_pending && !cpu->stopped
                && !cpu->mmu->jit_smc_dirty && !cpu->mmu->rom_bank_dirty
                && (cpu->mmu->io[0x0F] & cpu->mmu->ie & 0x1Fu) == 0
                && cpu->cycles < until) {
            for (int _spin = 0; _spin < GBJIT_SELFLOOP_MAX; _spin++) {
#if defined(ESP_PLATFORM)
                enter_block_native(b, cpu);
                __asm__ volatile("" ::: "a2","a3","a4","a5","a6","a7",
                    "a8","a9","a10","a11","a12","a13","a14","a15","memory");
#else
                enter_block_sim(b, cpu);
#endif
                GBJIT_STAT_INC(d, blocks_executed);
                if (cpu->pc != b->gb_pc_start
                        || cpu->sp != sp_before_block
                        || cpu->cycles >= until)
                    break;
            }
            continue;
        }
#endif

        /* Block-batching fast path: while the just-executed block has a
         * cached successor matching the new PC, call it directly
         * without going through the dispatcher's outer-iter checks
         * (rom_bank_dirty, sm83_service_interrupts, halt, ime_pending,
         * interp_fallback, chain-update, etc.).
         *
         * Compile-time gated via GBJIT_DISPATCHER_CHAIN_BATCH — set to
         * the max number of extra blocks per outer iter. 0 disables
         * the batch entirely (every block goes through the slow
         * dispatcher loop). 4 is a reasonable default — small enough
         * that IRQ-wake latency stays bounded to a few hundred GB
         * cycles, big enough to amortise the outer-loop fat. */
#ifndef GBJIT_DISPATCHER_CHAIN_BATCH
#define GBJIT_DISPATCHER_CHAIN_BATCH 4
#endif
#if GBJIT_DISPATCHER_CHAIN_BATCH > 0 && defined(ESP_PLATFORM)
        if (likely(prev && !d->no_cache)) {
            for (int _batch = 0; _batch < GBJIT_DISPATCHER_CHAIN_BATCH; _batch++) {
                if (cpu->cycles >= until) break;
                if (cpu->halted || cpu->ime_pending) break;
                /* Bail to the outer loop if the just-run block wrote a
                 * JIT code page, or switched the MBC ROM bank — in
                 * either case the chained successor may now be a stale
                 * translation. The outer loop's rom_bank_dirty /
                 * jit_smc_dirty handlers invalidate the affected blocks;
                 * the batch must not enter one first. */
                if (cpu->mmu->jit_smc_dirty || cpu->mmu->rom_bank_dirty) break;
                gbjit_block *_next = NULL;
                for (int _i = 0; _i < GBJIT_CHAIN_PREDICTOR_WAYS; _i++) {
                    if (prev->predicted_next_pc[_i] == cpu->pc
                            && prev->predicted_next[_i]) {
                        _next = prev->predicted_next[_i];
                        break;
                    }
                }
                if (!_next) break;
                GBJIT_STAT_INC(d, chain_hits);
#if GBJIT_JIT_EVICT == 1
                _next->tag = ++d->jit_epoch;
#endif
                enter_block_native(_next, cpu);
                __asm__ volatile("" :::
                    "a2","a3","a4","a5","a6","a7",
                    "a8","a9","a10","a11","a12","a13","a14","a15",
                    "memory");
                GBJIT_STAT_INC(d, blocks_executed);
                prev = _next;
            }
        }
#endif

        /* `stopped` is unrecoverable — STOP halts the CPU clock until
         * reset. HALT alone is not terminal: the next iteration's
         * sm83_service_interrupts will tick the PPU and may raise an
         * IRQ that clears `halted` and resumes execution. Don't break
         * on halted — sm83_run_until handles it the same way. */
        if (unlikely(cpu->stopped)) break;
    }
#undef d
#undef until
#undef cpu
#undef prev
}
