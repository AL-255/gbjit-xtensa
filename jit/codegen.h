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
     * through, the dispatcher updates this to the next block's pointer; on
     * subsequent executions of *this* block, the dispatcher skips the hash
     * lookup if cpu->pc matches `predicted_next_pc`. */
    struct gbjit_block *predicted_next;
    u16                 predicted_next_pc;
    /* Statically-known successor PCs (used by the prefetcher to compile
     * downstream blocks eagerly at this block's compile time). 0xFFFF
     * means "no static target — dynamic (RET / JP(HL)) or end of block".
     *   succ_pc[0] = taken branch target (JR n8, JP a16, CALL a16, RST nn,
     *                or fall-through of a non-terminator MAX_OPS exit).
     *   succ_pc[1] = fall-through PC for conditional branches (JR cc,
     *                JP cc, CALL cc); 0xFFFF for unconditional terminators. */
    u16  succ_pc[2];
} gbjit_block;

/* Compile from `pc` for one basic block. Allocates from `cc`. `helper_addr`
 * is the literal-pool resolver. `user` is passed through to it. Returns NULL
 * on error. */
gbjit_block *gbjit_compile_block(codecache *cc, cpu_state *cpu, u16 pc,
                                  jit_helper_addr_fn helper_addr,
                                  void *user);

void gbjit_block_free(gbjit_block *b);

#endif
