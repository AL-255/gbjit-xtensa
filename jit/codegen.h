#ifndef CODEGEN_H
#define CODEGEN_H

#include "gb_types.h"
#include "cpu_state.h"
#include "codecache.h"

/* Tokens placed in a block's literal pool. The codegen treats all of these
 * as opaque 32-bit values; the dispatcher (host or target) decides what they
 * mean.
 *
 *   ADDR_*  literals hold base addresses used as a target for L32R + memory
 *           access (cpu_state and mmu structs). On the host these are
 *           "sentinels" recognised by the xt_sim translate callback; on the
 *           ESP32-S3 target they are the real physical pointers.
 *
 *   HELPER_* literals hold function targets for CALLX0. On the host the sim
 *           routes them through a thunk; on the target the CPU calls them
 *           directly.                                                       */
typedef enum {
    ADDR_CPU_BASE = 0,
    ADDR_MMU_BASE,
    HELPER_SM83_STEP,
    HELPER_MMU_READ8,
    HELPER_MMU_WRITE8,
    LITERAL_COUNT
} literal_id;

/* For backwards-compat readability. */
typedef literal_id helper_id;
#define HELPER_COUNT LITERAL_COUNT

typedef u32 (*jit_helper_addr_fn)(literal_id id, void *user);

typedef struct gbjit_block {
    u16  gb_pc_start;
    u16  gb_pc_end;       /* exclusive — fall-through PC */
    u32  n_ops;           /* number of GB instructions in the block */
    u8  *code;            /* base of literal pool + code (4-byte aligned) */
    u32  code_size;       /* total bytes incl. literal pool */
    u32  entry_off;       /* byte offset within `code` of the entry instruction */
    /* For chaining (M5): list of L32R literal offsets that hold block-entry
     * function pointers. Patched when the target block is compiled. */
    u32 *chain_lit_off;
    u32  n_chain_lit;
    /* Soft "predicted-next" cache (host fast-path). When a block falls
     * through, the dispatcher updates this to the next block's pointer;
     * on subsequent executions of *this* block, the dispatcher skips
     * the hash lookup if cpu->pc matches one of `predicted_next_pc[i]`.
     *
     * Compile-time configurable via -DGBJIT_CHAIN_PREDICTOR_WAYS=<N>:
     *   1 → classic single-slot cache. Cheap lookup, but conditional
     *       branches alternating taken/fall-through evict each other on
     *       every iteration → ~50% hit rate on the worst SML inner loops.
     *   2 → two-way direct-mapped + 1-bit round-robin victim. The
     *       extra compare in the hot path is offset by the find_block
     *       calls it eliminates (90%+ hit rate on SML).
     * Default is 2; bump higher if you want more associativity. */
#ifndef GBJIT_CHAIN_PREDICTOR_WAYS
#define GBJIT_CHAIN_PREDICTOR_WAYS 2
#endif
    struct gbjit_block *predicted_next[GBJIT_CHAIN_PREDICTOR_WAYS];
    u16                 predicted_next_pc[GBJIT_CHAIN_PREDICTOR_WAYS];
    u8                  predicted_next_victim;
    /* Statically-known successor PCs (used by the prefetcher to compile
     * downstream blocks eagerly at this block's compile time). 0xFFFF
     * means "no static target — dynamic (RET / JP(HL)) or end of block".
     *   succ_pc[0] = taken branch target (JR n8, JP a16, CALL a16, RST nn,
     *                or fall-through of a non-terminator MAX_OPS exit).
     *   succ_pc[1] = fall-through PC for conditional branches (JR cc,
     *                JP cc, CALL cc); 0xFFFF for unconditional terminators. */
    u16  succ_pc[2];

    /* Coldness tag for the evicting code cache (GBJIT_JIT_EVICT=1). Set to
     * the dispatcher's monotonically-increasing epoch each time the block
     * is compiled or executed; the evictor drops the block with the
     * lowest tag (least-recently-used). Unused when eviction is off. */
    u64  tag;

    /* 1 iff this block is a "coarse-spin-safe" self-loop: its terminator
     * is a conditional JR back to gb_pc_start and every body op is
     * register-pure or an HRAM read — no IO-register read (LY/STAT
     * advance with the PPU, so such a loop must not be spun without
     * ticking it), no stack/RAM op. The dispatcher's self-loop fast
     * path re-enters such a block directly, bounded by an iteration
     * cap, instead of paying the outer-loop overhead per iteration. */
    u8   self_loop;
} gbjit_block;

/* Compile from `pc` for one basic block. Allocates from `cc`. `helper_addr`
 * is the literal-pool resolver. `user` is passed through to it. Returns NULL
 * on error. */
gbjit_block *gbjit_compile_block(codecache *cc, cpu_state *cpu, u16 pc,
                                  jit_helper_addr_fn helper_addr,
                                  void *user);

void gbjit_block_free(gbjit_block *b);

#endif
