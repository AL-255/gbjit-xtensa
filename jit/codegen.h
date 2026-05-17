#ifndef CODEGEN_H
#define CODEGEN_H

#include "gb_types.h"
#include "cpu_state.h"
#include "codecache.h"

/* Helper IDs used by the codegen — these are 32-bit "tokens" placed in the
 * literal pool. On the ESP32-S3 target the tokens ARE the helper function
 * addresses (so CALLX0 calls them directly). On the host they are small
 * integers and the xt_sim dispatches them via a thunk. The codegen never
 * cares which — it just uses helper_addr() callback. */
typedef enum {
    HELPER_SM83_STEP = 0,
    HELPER_MMU_READ8,
    HELPER_MMU_WRITE8,
    HELPER_COUNT
} helper_id;

typedef u32 (*jit_helper_addr_fn)(helper_id id);

typedef struct gbjit_block {
    u16  gb_pc_start;
    u16  gb_pc_end;       /* exclusive */
    u32  n_ops;           /* number of GB instructions in the block */
    u8  *code;            /* base of literal pool + code (4-byte aligned) */
    u32  code_size;       /* total bytes incl. literal pool */
    u32  entry_off;       /* byte offset within `code` of the entry instruction */
    /* For chaining (M5): list of L32R literal offsets that hold block-entry
     * function pointers. Patched when the target block is compiled. */
    u32 *chain_lit_off;
    u32  n_chain_lit;
} gbjit_block;

/* Compile from `pc` for one basic block. Allocates from `cc`. `helper_addr`
 * is the function-pointer resolver for CALLX0 targets. Returns NULL on error. */
gbjit_block *gbjit_compile_block(codecache *cc, cpu_state *cpu, u16 pc,
                                  jit_helper_addr_fn helper_addr);

void gbjit_block_free(gbjit_block *b);

#endif
