/* JIT codegen — v0.
 *
 * Strategy: walk forward from the entry PC until a block terminator
 * (branch/halt/stop). For each GB op, emit Xtensa code that calls into the
 * reference interpreter (`sm83_step`). This proves the JIT pipeline
 * end-to-end (block discovery, codegen, codecache, execution, exit).
 *
 * Optimization layer (M5): replace selected CALLX0 sites with inlined Xtensa
 * code, starting with the hot opcodes (LD r,n8 / LD r,r' / NOP / ADD).
 *
 * Block layout in memory (4-byte aligned, lives in codecache arena):
 *
 *   +----------------------+ <- code base (4-aligned)
 *   |  literal pool        |   one u32 per helper + one u32 for cpu_state ptr
 *   +----------------------+ <- entry_off
 *   |  prologue            |   stack frame; cpu_state ptr in a2; save a0
 *   |  body                |   per-op: L32R helper; CALLX0; reload a2
 *   |  epilogue            |   restore a0, dealloc, RET
 *   +----------------------+
 *
 * Calling convention (CALL0 ABI):
 *   - Caller passes arg in a2.
 *   - Block function takes cpu_state* in a2.
 *   - a0 = return address. Caller-saved; we save in stack frame across CALLX0.
 *   - a1 = stack pointer.
 *   - a2..a15 caller-saved across CALLX0.
 */

#include "codegen.h"
#include "sm83_decoder.h"
#include "emit_xtensa.h"
#include "memory.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

/* Stack frame layout (16 bytes, 16-byte aligned per CALL0 ABI):
 *   +12  saved a0       (return address)
 *   + 8  saved cpu*     (a2 input; reloaded after each CALLX0)
 *   + 4  unused (alignment / future scratch)
 *   + 0  unused                                                       */
#define FRAME_SIZE 16
#define FRAME_OFF_A0   12
#define FRAME_OFF_CPU   8

/* Max GB instructions per block. Conservative; bounds codegen growth. */
#define MAX_OPS_PER_BLOCK 32

/* Worst-case Xtensa bytes per GB op = 4 instructions = 12 bytes. Plus
 * prologue/epilogue (~32B) and literal pool. */
#define BYTES_PER_OP 12
#define PROLOGUE_EPILOGUE_BYTES 64
#define LITERAL_POOL_BYTES ((HELPER_COUNT + 1) * 4)

/* Block-static info we want to track. */
typedef struct {
    u32 lit_off_cpu;                 /* offset of cpu_state-ptr literal in code */
    u32 lit_off_helper[HELPER_COUNT];/* offset of each helper literal */
} block_layout;

/* Emit a load of literal at `lit_off` (offset within `code` base) into reg
 * `at`. L32R encoding: imm16 = ((label - ((PC+3) & ~3)) >> 2). Negative.
 *
 * `pc_off` is the byte offset of the L32R instruction within `code`. */
static void emit_l32r_at(xt_emit *e, u8 at, u32 lit_off, u32 pc_off) {
    u32 pc_after = pc_off + 3;
    u32 pc_aligned = pc_after & ~3u;
    /* Literal must be BEFORE PC_aligned. */
    assert(lit_off < pc_aligned);
    u32 dist = pc_aligned - lit_off;
    assert((dist & 3) == 0);
    u32 imm16 = 0x10000u - (dist >> 2);  /* two's complement of (-dist/4) in 16 bits */
    xt_l32r(e, at, imm16);
}

static u32 align_up_4(u32 v) { return (v + 3u) & ~3u; }

gbjit_block *gbjit_compile_block(codecache *cc, cpu_state *cpu, u16 pc_start,
                                  jit_helper_addr_fn helper_addr) {
    /* Walk forward to discover block end + collect op list. The op tables
     * are placeholders for the M5 inlining pass; for now we only need n_ops
     * and the terminating PC. */
    u16 ops_pc[MAX_OPS_PER_BLOCK];
    u8  ops_opcode[MAX_OPS_PER_BLOCK];
    u8  ops_len[MAX_OPS_PER_BLOCK];
    (void)ops_pc; (void)ops_opcode; (void)ops_len;
    u32 n_ops = 0;
    u16 cur = pc_start;
    while (n_ops < MAX_OPS_PER_BLOCK) {
        u8 opcode = cpu->mmu->rom[cur & 0x7FFFu];  /* assume executing from ROM */
        const sm83_op_info *info = sm83_decode(opcode);
        ops_pc[n_ops] = cur;
        ops_opcode[n_ops] = opcode;
        ops_len[n_ops] = info->length;
        n_ops++;
        cur = (u16)(cur + info->length);
        if (sm83_terminates_block(opcode)) break;
    }

    /* Compute size estimate and reserve. */
    u32 lit_bytes = LITERAL_POOL_BYTES;
    u32 code_bytes = PROLOGUE_EPILOGUE_BYTES + n_ops * BYTES_PER_OP;
    u32 total = align_up_4(lit_bytes) + align_up_4(code_bytes);
    u8 *base = codecache_alloc(cc, total);
    if (!base) return NULL;
    memset(base, 0, total);

    /* Lay out literal pool first. */
    block_layout L;
    u32 wp = 0;
    L.lit_off_cpu = wp;
    *(u32 *)(base + wp) = (u32)(uintptr_t)cpu;
    wp += 4;
    for (helper_id h = 0; h < HELPER_COUNT; h++) {
        L.lit_off_helper[h] = wp;
        *(u32 *)(base + wp) = helper_addr(h);
        wp += 4;
    }
    wp = align_up_4(wp);
    u32 entry_off = wp;

    /* Emit code from `entry_off`. */
    xt_emit e;
    xt_init(&e, base + entry_off, total - entry_off);

    /* --- Prologue --- */
    /* addi a1, a1, -FRAME_SIZE */
    xt_addi(&e, 1, 1, -FRAME_SIZE);
    /* s32i a0, a1, FRAME_OFF_A0 */
    xt_s32i(&e, 0, 1, FRAME_OFF_A0);
    /* s32i a2, a1, FRAME_OFF_CPU  (save cpu_state pointer) */
    xt_s32i(&e, 2, 1, FRAME_OFF_CPU);

    /* --- Body: for each op, call sm83_step --- */
    for (u32 i = 0; i < n_ops; i++) {
        /* Reload cpu_state ptr into a2 */
        u32 pc_off = entry_off + e.len;
        emit_l32r_at(&e, 2, L.lit_off_cpu, pc_off);
        /* Load helper sm83_step into a14 */
        pc_off = entry_off + e.len;
        emit_l32r_at(&e, 14, L.lit_off_helper[HELPER_SM83_STEP], pc_off);
        /* CALLX0 a14 */
        xt_callx0(&e, 14);
    }

    /* --- Epilogue --- */
    xt_l32i(&e, 0, 1, FRAME_OFF_A0);
    xt_addi(&e, 1, 1, FRAME_SIZE);
    xt_ret(&e);

    /* Trim used bytes; codecache_finalize handles cache sync. */
    codecache_finalize(cc, base + entry_off, e.len);

    gbjit_block *b = (gbjit_block *)calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->gb_pc_start = pc_start;
    b->gb_pc_end = cur;
    b->n_ops = n_ops;
    b->code = base;
    b->code_size = total;
    b->entry_off = entry_off;
    return b;
}

void gbjit_block_free(gbjit_block *b) {
    if (!b) return;
    free(b->chain_lit_off);
    free(b);
}
