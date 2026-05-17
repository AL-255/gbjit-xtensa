/* JIT codegen.
 *
 * Strategy: walk forward from the entry PC until a block terminator
 * (branch/halt/stop). For each GB op, either inline it (fast path) or fall
 * back to a CALLX0 to the reference interpreter (`sm83_step`).
 *
 * Block layout in memory (4-byte aligned, lives in codecache arena):
 *
 *   +----------------------+ <- code base (4-aligned)
 *   |  literal pool        |   one u32 per literal_id (LITERAL_COUNT)
 *   +----------------------+ <- entry_off
 *   |  prologue            |   stack frame; a13 = cpu_state, a11 = PC, a12 = cycles_delta
 *   |  body                |   per-op: inline or CALLX0
 *   |  epilogue            |   sync PC + cycles, restore a0, dealloc, RET
 *   +----------------------+
 *
 * CALL0 ABI registers we use throughout a block:
 *   a0  = saved return address (in stack frame; clobbered by CALLX0)
 *   a1  = host stack pointer
 *   a2  = scratch / argument for CALLX0
 *   a3..a10 = scratch for inlined op bodies (loaded JIT from cpu_state)
 *   a11 = guest PC (kept in-register across inlined ops; synced on helper call / block exit)
 *   a12 = cycles delta accumulator (added to cpu->cycles on sync)
 *   a13 = cpu_state base address (reloaded from literal pool after CALLX0)
 *   a14, a15 = scratch
 */

#include "codegen.h"
#include "sm83_decoder.h"
#include "emit_xtensa.h"
#include "memory.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

/* --- cpu_state field offsets (resolved at codegen time on the host running
 * this code; on the ESP32-S3 these are the offsets for the target build). */
#define OFF_F    (offsetof(cpu_state, f))
#define OFF_A    (offsetof(cpu_state, a))
#define OFF_C    (offsetof(cpu_state, c))
#define OFF_B    (offsetof(cpu_state, b))
#define OFF_E    (offsetof(cpu_state, e))
#define OFF_D    (offsetof(cpu_state, d))
#define OFF_L    (offsetof(cpu_state, l))
#define OFF_H    (offsetof(cpu_state, h))
#define OFF_BC   (offsetof(cpu_state, bc))    /* = OFF_C on LE host */
#define OFF_DE   (offsetof(cpu_state, de))
#define OFF_HL   (offsetof(cpu_state, hl))
#define OFF_SP   (offsetof(cpu_state, sp))
#define OFF_PC   (offsetof(cpu_state, pc))
#define OFF_HALTED (offsetof(cpu_state, halted))
#define OFF_CYCLES (offsetof(cpu_state, cycles))
#define OFF_JITRETPC (offsetof(cpu_state, jit_ret_pc))

/* The JIT block runs *without* its own Xtensa stack frame: it leaves a1
 * untouched so the host's window-overflow handler keeps spilling to the
 * caller's (enter_block_native's) save area. The return PC is stashed in
 * cpu_state->jit_ret_pc instead of on the stack. */

/* Limits. */
#define MAX_OPS_PER_BLOCK 32

/* Generous size budget per op. Worst-case inline is currently ALU with eager
 * flags (~24 Xtensa instructions = 72 bytes). Helper fallback is ~32 bytes.
 * Round up to 96 to leave headroom for future inlining additions. */
#define BYTES_PER_OP 96
#define PROLOGUE_EPILOGUE_BYTES 128
/* Extra literal slots reserved per block for inline ops that need a
 * precomputed 32-bit constant (typically a host pointer into mmu state). */
#define MAX_EXTRA_LITERALS 32
#define LITERAL_POOL_BYTES ((LITERAL_COUNT + MAX_EXTRA_LITERALS) * 4)

/* Runtime-allocated literal pool slot tracker. */
typedef struct {
    u8 *base;
    u32 next_off;   /* next free byte offset within `base` */
    u32 limit;      /* end of pool (exclusive) */
} lit_ctx;

static i32 lit_alloc_u32(lit_ctx *L, u32 value) {
    if (L->next_off + 4 > L->limit) return -1;
    u32 off = L->next_off;
    L->base[off + 0] = (u8)(value & 0xFFu);
    L->base[off + 1] = (u8)((value >> 8) & 0xFFu);
    L->base[off + 2] = (u8)((value >> 16) & 0xFFu);
    L->base[off + 3] = (u8)((value >> 24) & 0xFFu);
    L->next_off += 4;
    return (i32)off;
}

/* Compute the target byte address (= literal value) for a GB memory access
 * at known address `addr`, if it falls in a JIT-inlinable region (WRAM or
 * HRAM). Returns 0 if not inlinable. */
static u32 inlinable_byte_addr(u32 mmu_base, u16 addr) {
    if (addr >= 0xC000u && addr < 0xE000u) {
        return mmu_base + (u32)offsetof(mmu, wram) + (u32)(addr - 0xC000u);
    }
    if (addr >= 0xFF80u && addr < 0xFFFFu) {
        return mmu_base + (u32)offsetof(mmu, hram) + (u32)(addr - 0xFF80u);
    }
    return 0;
}

/* L32R helper: encode a load of the literal at `lit_off` into reg `at` when
 * the L32R instruction itself lives at `pc_off`, both being offsets from the
 * block's `code` base. */
static void emit_l32r_at(xt_emit *e, u8 at, u32 lit_off, u32 pc_off) {
    u32 pc_after = pc_off + 3;
    u32 pc_aligned = pc_after & ~3u;
    assert(lit_off < pc_aligned);
    u32 dist = pc_aligned - lit_off;
    assert((dist & 3) == 0);
    u32 imm16 = 0x10000u - (dist >> 2);   /* two's complement, 16-bit field */
    xt_l32r(e, at, imm16);
}

static u32 align_up_4(u32 v) { return (v + 3u) & ~3u; }

/* --- Code-emission helpers ---------------------------------------------- */

/* Sync PC and cycles accumulator into cpu_state. */
static void emit_sync_state(xt_emit *e) {
    /* s16i a11, a13, OFF_PC */
    xt_s16i(e, 11, 13, OFF_PC);
    /* l32i a14, a13, OFF_CYCLES ; add a14, a14, a12 ; s32i a14, a13, OFF_CYCLES
     * (we update only the low 32 bits of the u64 — sufficient for any
     * reasonable session; ~4G cycles ≈ 17 minutes of GB time.) */
    xt_l32i(e, 14, 13, OFF_CYCLES);
    xt_add (e, 14, 14, 12);
    xt_s32i(e, 14, 13, OFF_CYCLES);
}

/* Emit a helper call. Arguments must already be in a2/a3/... per CALL0 ABI.
 * Lit_pool_off is the byte offset of the helper's literal in the literal
 * pool; entry_off is the code-base-relative offset where emission started. */
static void emit_callx0_helper(xt_emit *e, u32 helper_lit_off, u32 entry_off) {
    u32 pc_off = entry_off + e->len;
    emit_l32r_at(e, 14, helper_lit_off, pc_off);
    xt_callx0(e, 14);
}

/* Reload state after a helper call: a13 = cpu_base, a11 = PC, a12 = 0. */
static void emit_reload_state(xt_emit *e, u32 cpu_lit_off, u32 entry_off) {
    u32 pc_off = entry_off + e->len;
    emit_l32r_at(e, 13, cpu_lit_off, pc_off);
    xt_l16ui(e, 11, 13, OFF_PC);
    xt_movi(e, 12, 0);
}

/* --- Inlined-op codegen ------------------------------------------------- */

/* Map a 3-bit GB reg index (0..7 = B,C,D,E,H,L,(HL),A) to its byte offset
 * in cpu_state. Returns -1 for (HL) which is a memory access. */
static int reg8_offset(u8 r) {
    switch (r & 7) {
        case 0: return OFF_B;
        case 1: return OFF_C;
        case 2: return OFF_D;
        case 3: return OFF_E;
        case 4: return OFF_H;
        case 5: return OFF_L;
        case 6: return -1;
        case 7: return OFF_A;
    }
    return -1;
}

/* Advance our tracked PC by `n_bytes` and cycle accumulator by `n_cycles`. */
static void emit_advance(xt_emit *e, u8 n_bytes, u8 n_cycles) {
    xt_addi(e, 11, 11, n_bytes);
    xt_addi(e, 12, 12, n_cycles);
}

/* Load a 16-bit unsigned value into `dst`. Uses `scratch` (must differ from
 * dst) when the value doesn't fit in MOVI's 12-bit signed range. Emits
 * either 1 or 4 Xtensa instructions. */
static void emit_load_u16(xt_emit *e, u8 dst, u16 value, u8 scratch) {
    if (value <= 2047) {
        xt_movi(e, dst, (i32)value);
        return;
    }
    u8 lo = (u8)(value & 0xFFu);
    u8 hi = (u8)((value >> 8) & 0xFFu);
    xt_movi(e, dst, (i32)lo);
    xt_movi(e, scratch, (i32)hi);
    xt_slli(e, scratch, scratch, 8);
    xt_or(e, dst, dst, scratch);
}

/* Patch a previously-emitted BEQZ/BNEZ at byte offset `br_pos` (within e->buf)
 * so that its branch target lands at byte offset `target_pos`.
 *
 * The encoded imm12 field is at bits 12..23 of the 24-bit instruction word,
 * and the target = (br_pc + 4 + imm12).  We solve for imm12 = target - br_pc - 4. */
static void patch_branch_to(xt_emit *e, u32 br_pos, u32 target_pos) {
    i32 imm12 = (i32)target_pos - (i32)br_pos - 4;
    assert(imm12 >= -2048 && imm12 <= 2047);
    u32 imm12_u = (u32)imm12 & 0xFFFu;
    u32 w = (u32)e->buf[br_pos]
          | ((u32)e->buf[br_pos + 1] << 8)
          | ((u32)e->buf[br_pos + 2] << 16);
    w &= ~(0xFFFu << 12);
    w |= (imm12_u << 12);
    e->buf[br_pos + 0] = (u8)(w & 0xFFu);
    e->buf[br_pos + 1] = (u8)((w >> 8) & 0xFFu);
    e->buf[br_pos + 2] = (u8)((w >> 16) & 0xFFu);
}

/* Patch a previously-emitted J at `j_pos` so it lands at `target_pos`.
 * J encodes the 18-bit signed offset in bits 6..23 with target = pc + 4 + imm18. */
static void patch_j_to(xt_emit *e, u32 j_pos, u32 target_pos) {
    i32 imm18 = (i32)target_pos - (i32)j_pos - 4;
    assert(imm18 >= -(1 << 17) && imm18 < (1 << 17));
    u32 imm18_u = (u32)imm18 & 0x3FFFFu;
    u32 w = (u32)e->buf[j_pos]
          | ((u32)e->buf[j_pos + 1] << 8)
          | ((u32)e->buf[j_pos + 2] << 16);
    w &= ~(0x3FFFFu << 6);
    w |= (imm18_u << 6);
    e->buf[j_pos + 0] = (u8)(w & 0xFFu);
    e->buf[j_pos + 1] = (u8)((w >> 8) & 0xFFu);
    e->buf[j_pos + 2] = (u8)((w >> 16) & 0xFFu);
}

/* --- Flag-bit helpers used by the ALU inliners.
 *
 * Each pushes its computed flag bit (already in the correct F position) into
 * dst_reg. Caller initialises dst_reg to 0 and ORs each contribution. */

/* Z bit (0x80): set if value_reg & 0xFF == 0. Uses scratch_reg. */
static void emit_zflag(xt_emit *e, u8 value_reg, u8 dst_reg, u8 scratch) {
    /* tmp = value - 1; bit 8 of tmp is 1 iff value was 0 (assuming value ≤ 0xFF) */
    xt_addi(e, scratch, value_reg, -1);
    xt_extui(e, scratch, scratch, 8, 0);   /* extract bit 8 (width 1) */
    xt_slli(e, scratch, scratch, 7);        /* shift into Z position */
    xt_or(e, dst_reg, dst_reg, scratch);
}

/* Set bit N of dst_reg unconditionally. */
static void emit_setflag_const(xt_emit *e, u8 dst_reg, u8 flag_bit, u8 scratch) {
    /* Load the constant into scratch, then OR. MOVI fits all flag bits. */
    xt_movi(e, scratch, flag_bit);
    xt_or(e, dst_reg, dst_reg, scratch);
}

/* H bit (0x20) for ADD: set if low nibble overflows. Args: a_reg, r_reg. */
static void emit_hflag_add(xt_emit *e, u8 a_reg, u8 r_reg, u8 dst_reg, u8 scratch_a, u8 scratch_b) {
    xt_extui(e, scratch_a, a_reg, 0, 3);    /* a & 0xF */
    xt_extui(e, scratch_b, r_reg, 0, 3);    /* r & 0xF */
    xt_add(e, scratch_a, scratch_a, scratch_b);
    xt_extui(e, scratch_a, scratch_a, 4, 0); /* bit 4 */
    xt_slli(e, scratch_a, scratch_a, 5);     /* shift to H position */
    xt_or(e, dst_reg, dst_reg, scratch_a);
}

/* H bit for SUB: set if low nibble borrows (low_A < low_r). */
static void emit_hflag_sub(xt_emit *e, u8 a_reg, u8 r_reg, u8 dst_reg, u8 scratch_a, u8 scratch_b) {
    xt_extui(e, scratch_a, a_reg, 0, 3);
    xt_extui(e, scratch_b, r_reg, 0, 3);
    xt_sub(e, scratch_a, scratch_a, scratch_b);
    xt_extui(e, scratch_a, scratch_a, 4, 0); /* bit 4 of (possibly negative) result */
    xt_slli(e, scratch_a, scratch_a, 5);
    xt_or(e, dst_reg, dst_reg, scratch_a);
}

/* C bit (0x10): set if bit 8 of `sum_reg` is 1. */
static void emit_cflag_from_bit8(xt_emit *e, u8 sum_reg, u8 dst_reg, u8 scratch) {
    xt_extui(e, scratch, sum_reg, 8, 0);
    xt_slli(e, scratch, scratch, 4);
    xt_or(e, dst_reg, dst_reg, scratch);
}

/* Context passed to inline_op so inlined paths can resolve mmu pointers
 * via the literal pool and emit L32R at the right PC offset. */
typedef struct {
    mmu *m;
    lit_ctx *L;
    u32 entry_off;
    u32 mmu_base_value;
    const u32 *lit_off;        /* fixed-literal offsets (LITERAL_COUNT entries) */
} inline_ctx;

/* Try to inline a GB op. Returns true on success, false to request the
 * helper-fallback path. */
static bool inline_op(xt_emit *e, u8 opcode, u16 pc, inline_ctx *ictx) {
    mmu *m = ictx->m;
    (void)pc;
    /* NOP */
    if (opcode == 0x00) { emit_advance(e, 1, 4); return true; }

    /* LD r,n8 — 0x06 0x0E 0x16 0x1E 0x26 0x2E 0x3E  (NOT 0x36 = LD (HL),n8) */
    if (opcode == 0x06 || opcode == 0x0E || opcode == 0x16 || opcode == 0x1E ||
        opcode == 0x26 || opcode == 0x2E || opcode == 0x3E) {
        u8 dst = (opcode >> 3) & 7;
        int off = reg8_offset(dst);
        if (off < 0) return false;
        u8 imm = mmu_read8(m, (u16)(pc + 1));
        /* movi a2, imm ; s8i a2, a13, off */
        xt_movi(e, 2, (i32)imm);
        xt_s8i(e, 2, 13, (u32)off);
        emit_advance(e, 2, 8);
        return true;
    }

    /* --- LD r, (HL) (0x46, 0x4E, 0x56, 0x5E, 0x66, 0x6E, 0x7E) and
     *     LD (HL), r (0x70..0x77 except 0x76) with WRAM fast-path.
     *
     * Real GB code hits (HL) constantly. Helper fallback through sm83_step
     * is several us per access. Inline a runtime range check: if HL is in
     * WRAM ($C000..$DFFF), load/store directly from cpu->mmu->wram[]; else
     * branch to the helper fallback.
     *
     * (HRAM-via-(HL) is rare and the range check is more complex, so we
     * skip that branch and just send HRAM accesses to the helper.) */
    if ((opcode >= 0x46 && opcode <= 0x7E && (opcode & 7) == 6 && opcode != 0x76) ||
        (opcode >= 0x70 && opcode <= 0x77 && opcode != 0x76)) {
        bool is_load = (opcode & 0xF8) != 0x70;
        u8 reg_idx = is_load ? ((opcode >> 3) & 7) : (opcode & 7);
        int reg_off = reg8_offset(reg_idx);
        if (reg_off < 0) return false;

        /* WRAM-base literal: mmu_base + offsetof(mmu, wram) - 0xC000.
         * Adding HL to this value yields the real byte pointer for any HL
         * in [0xC000, 0xE000). */
        u32 wram_base_minus_C000 =
            ictx->mmu_base_value + (u32)offsetof(mmu, wram) - 0xC000u;
        i32 wram_lit = lit_alloc_u32(ictx->L, wram_base_minus_C000);
        if (wram_lit < 0) return false;

        /* Load HL. */
        xt_l16ui(e, 3, 13, OFF_HL);

        /* WRAM range check: (HL >> 13) == 0b110 = 6. */
        xt_extui(e, 4, 3, 13, 2);
        xt_addi(e, 4, 4, -6);

        /* Branch to slow path if NOT in WRAM. */
        u32 br_to_slow = e->len;
        xt_bnez(e, 4, 4);   /* placeholder offset */

        /* --- Fast path --- */
        u32 pc_off = ictx->entry_off + e->len;
        emit_l32r_at(e, 5, (u32)wram_lit, pc_off);
        xt_add(e, 4, 5, 3);              /* a4 = wram_base - 0xC000 + HL */
        if (is_load) {
            xt_l8ui(e, 2, 4, 0);
            xt_s8i(e, 2, 13, (u32)reg_off);
        } else {
            xt_l8ui(e, 2, 13, (u32)reg_off);
            xt_s8i(e, 2, 4, 0);
        }
        /* Advance for fast path. */
        xt_addi(e, 11, 11, 1);
        xt_addi(e, 12, 12, 8);

        /* Jump to end (skip slow path). */
        u32 j_to_end = e->len;
        xt_j(e, 4);

        /* --- Slow path: standard helper invocation. --- */
        u32 slow_pos = e->len;
        emit_sync_state(e);
        xt_mov(e, 2, 13);
        emit_callx0_helper(e, ictx->lit_off[HELPER_SM83_STEP], ictx->entry_off);
        emit_reload_state(e, ictx->lit_off[ADDR_CPU_BASE], ictx->entry_off);

        u32 end_pos = e->len;
        patch_branch_to(e, br_to_slow, slow_pos);
        patch_j_to(e, j_to_end, end_pos);
        return true;
    }

    /* LD r,r' — 0x40..0x7F excluding 0x76 (HALT). Either operand being (HL)
     * (index 6) needs memory access → fall back to helper. */
    if (opcode >= 0x40 && opcode <= 0x7F && opcode != 0x76) {
        u8 dst = (opcode >> 3) & 7;
        u8 src = opcode & 7;
        int dst_off = reg8_offset(dst);
        int src_off = reg8_offset(src);
        if (dst_off < 0 || src_off < 0) return false;
        /* l8ui a2, a13, src_off ; s8i a2, a13, dst_off */
        xt_l8ui(e, 2, 13, (u32)src_off);
        xt_s8i (e, 2, 13, (u32)dst_off);
        emit_advance(e, 1, 4);
        return true;
    }

    /* --- CPL (0x2F): A = ~A; F = (F & (Z|C)) | N | H. */
    if (opcode == 0x2F) {
        xt_l8ui(e, 2, 13, OFF_A);
        xt_movi(e, 3, 0xFF);
        xt_xor(e, 2, 2, 3);                /* a2 = ~A (low 8) */
        xt_extui(e, 2, 2, 0, 7);
        xt_s8i(e, 2, 13, OFF_A);
        xt_l8ui(e, 4, 13, OFF_F);
        xt_movi(e, 5, FLAG_Z | FLAG_C);
        xt_and(e, 4, 4, 5);                 /* preserve Z, C */
        xt_movi(e, 5, FLAG_N | FLAG_H);
        xt_or(e, 4, 4, 5);
        xt_s8i(e, 4, 13, OFF_F);
        emit_advance(e, 1, 4);
        return true;
    }

    /* --- SCF (0x37): F = (F & Z) | C; CCF (0x3F): F = (F & Z) ^ C. */
    if (opcode == 0x37 || opcode == 0x3F) {
        bool is_ccf = (opcode == 0x3F);
        xt_l8ui(e, 4, 13, OFF_F);
        xt_movi(e, 5, FLAG_Z | (is_ccf ? FLAG_C : 0));
        xt_and(e, 4, 4, 5);                 /* keep Z, plus C if CCF (to be inverted) */
        xt_movi(e, 5, FLAG_C);
        if (is_ccf) {
            xt_xor(e, 4, 4, 5);             /* toggle C */
        } else {
            xt_or(e, 4, 4, 5);              /* set C */
        }
        xt_s8i(e, 4, 13, OFF_F);
        emit_advance(e, 1, 4);
        return true;
    }

    /* --- RLCA (0x07) / RRCA (0x0F) / RLA (0x17) / RRA (0x1F).
     * Like the CB shifts on A, but Z is always 0. */
    if (opcode == 0x07 || opcode == 0x0F || opcode == 0x17 || opcode == 0x1F) {
        xt_l8ui(e, 2, 13, OFF_A);

        if (opcode == 0x07) {           /* RLCA */
            xt_extui(e, 6, 2, 7, 0);
            xt_slli(e, 5, 2, 1);
            xt_or(e, 5, 5, 6);
            xt_extui(e, 5, 5, 0, 7);
        } else if (opcode == 0x0F) {    /* RRCA */
            xt_extui(e, 6, 2, 0, 0);
            xt_srli(e, 5, 2, 1);
            xt_slli(e, 7, 6, 7);
            xt_or(e, 5, 5, 7);
        } else if (opcode == 0x17) {    /* RLA */
            xt_l8ui(e, 4, 13, OFF_F);
            xt_extui(e, 6, 2, 7, 0);
            xt_extui(e, 7, 4, 4, 0);
            xt_slli(e, 5, 2, 1);
            xt_or(e, 5, 5, 7);
            xt_extui(e, 5, 5, 0, 7);
        } else {                        /* RRA */
            xt_l8ui(e, 4, 13, OFF_F);
            xt_extui(e, 6, 2, 0, 0);
            xt_extui(e, 7, 4, 4, 0);
            xt_srli(e, 5, 2, 1);
            xt_slli(e, 7, 7, 7);
            xt_or(e, 5, 5, 7);
        }
        xt_s8i(e, 5, 13, OFF_A);

        /* F: Z=0, N=0, H=0, C from a6. */
        xt_movi(e, 4, 0);
        xt_slli(e, 6, 6, 4);
        xt_or(e, 4, 4, 6);
        xt_s8i(e, 4, 13, OFF_F);
        emit_advance(e, 1, 4);
        return true;
    }

    /* --- LD (HL), n8 (0x36): write immediate byte to (HL). WRAM fast-path. */
    if (opcode == 0x36) {
        u8 imm = mmu_read8(m, (u16)(pc + 1));

        u32 wram_base_minus_C000 =
            ictx->mmu_base_value + (u32)offsetof(mmu, wram) - 0xC000u;
        i32 wram_lit = lit_alloc_u32(ictx->L, wram_base_minus_C000);
        if (wram_lit < 0) return false;

        xt_l16ui(e, 9, 13, OFF_HL);
        xt_extui(e, 4, 9, 13, 2);
        xt_addi(e, 4, 4, -6);

        u32 br_to_slow = e->len;
        xt_bnez(e, 4, 4);

        u32 pc_off = ictx->entry_off + e->len;
        emit_l32r_at(e, 5, (u32)wram_lit, pc_off);
        xt_add(e, 4, 5, 9);
        xt_movi(e, 2, (i32)imm);
        xt_s8i(e, 2, 4, 0);
        emit_advance(e, 2, 12);

        u32 j_to_end = e->len;
        xt_j(e, 4);

        u32 slow_pos = e->len;
        emit_sync_state(e);
        xt_mov(e, 2, 13);
        emit_callx0_helper(e, ictx->lit_off[HELPER_SM83_STEP], ictx->entry_off);
        emit_reload_state(e, ictx->lit_off[ADDR_CPU_BASE], ictx->entry_off);

        u32 end_pos = e->len;
        patch_branch_to(e, br_to_slow, slow_pos);
        patch_j_to(e, j_to_end, end_pos);
        return true;
    }

    /* HALT — set cpu->halted = 1 and exit. The block exit path is handled
     * by the caller (we just write halted and let emit_advance bump PC). */
    if (opcode == 0x76) {
        xt_movi(e, 2, 1);
        xt_s8i (e, 2, 13, OFF_HALTED);
        emit_advance(e, 1, 4);
        return true;
    }

    /* --- INC r (8-bit) — 0x04, 0x0C, 0x14, 0x1C, 0x24, 0x2C, 0x3C.
     *     DEC r (8-bit) — 0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x3D.
     *     INC/DEC (HL) at 0x34/0x35 needs memory access — fall through.
     *
     * Flags: Z, N=(0 for INC, 1 for DEC), H computed; C preserved. */
    if (opcode == 0x04 || opcode == 0x0C || opcode == 0x14 || opcode == 0x1C ||
        opcode == 0x24 || opcode == 0x2C || opcode == 0x3C ||
        opcode == 0x05 || opcode == 0x0D || opcode == 0x15 || opcode == 0x1D ||
        opcode == 0x25 || opcode == 0x2D || opcode == 0x3D) {
        bool is_dec = (opcode & 1) != 0;
        u8 r = (opcode >> 3) & 7;
        if (r == 6) return false;  /* (HL) */
        int r_off = reg8_offset(r);

        xt_l8ui(e, 2, 13, (u32)r_off);                   /* a2 = old r */
        if (is_dec) xt_addi(e, 3, 2, -1);
        else        xt_addi(e, 3, 2,  1);
        xt_extui(e, 3, 3, 0, 7);                          /* a3 = new r & 0xFF */
        xt_s8i(e, 3, 13, (u32)r_off);

        /* F: preserve C, replace Z|N|H. */
        xt_l8ui(e, 4, 13, OFF_F);
        xt_movi(e, 5, FLAG_C);
        xt_and(e, 4, 4, 5);                               /* a4 = old F & C */

        emit_zflag(e, 3, 4, 7);

        if (is_dec) {
            emit_setflag_const(e, 4, FLAG_N, 7);
            /* H = (old_r & 0xF) == 0 → use the borrow approach: bit 4 of
             * (old & 0xF) - 1 is 1 iff (old & 0xF) was 0. */
            xt_extui(e, 7, 2, 0, 3);                      /* a7 = old & 0xF */
            xt_addi(e, 7, 7, -1);
            xt_extui(e, 7, 7, 4, 0);
            xt_slli(e, 7, 7, 5);
            xt_or(e, 4, 4, 7);
        } else {
            /* H = (old & 0xF) + 1 > 0xF → bit 4 of (old & 0xF) + 1. */
            xt_extui(e, 7, 2, 0, 3);
            xt_addi(e, 7, 7, 1);
            xt_extui(e, 7, 7, 4, 0);
            xt_slli(e, 7, 7, 5);
            xt_or(e, 4, 4, 7);
        }

        xt_s8i(e, 4, 13, OFF_F);
        emit_advance(e, 1, 4);
        return true;
    }

    /* --- INC rr / DEC rr (16-bit) — no flag effects.
     *   0x03 INC BC   0x0B DEC BC
     *   0x13 INC DE   0x1B DEC DE
     *   0x23 INC HL   0x2B DEC HL
     *   0x33 INC SP   0x3B DEC SP                                          */
    if (opcode == 0x03 || opcode == 0x13 || opcode == 0x23 || opcode == 0x33 ||
        opcode == 0x0B || opcode == 0x1B || opcode == 0x2B || opcode == 0x3B) {
        bool is_dec = ((opcode & 0x0F) == 0x0B);
        u8 pair = (opcode >> 4) & 3;
        u32 off = OFF_BC;
        switch (pair) {
            case 0: off = OFF_BC; break;
            case 1: off = OFF_DE; break;
            case 2: off = OFF_HL; break;
            case 3: off = OFF_SP; break;
        }
        xt_l16ui(e, 2, 13, off);
        xt_addi(e, 2, 2, is_dec ? -1 : 1);
        xt_s16i(e, 2, 13, off);                          /* writes only low 16 bits */
        emit_advance(e, 1, 8);
        return true;
    }

    /* --- JR r8 (0x18, unconditional) — block terminator. */
    if (opcode == 0x18) {
        i8 off = (i8)mmu_read8(m, (u16)(pc + 1));
        u16 target = (u16)(pc + 2 + off);
        emit_load_u16(e, 11, target, 2);
        xt_addi(e, 12, 12, 12);
        return true;
    }

    /* --- JR cc, r8 (0x20/0x28/0x30/0x38) — conditional, block terminator.
     *
     * Layout emitted:
     *   l8ui  a4, a13, OFF_F            ; load F
     *   movi  a5, flag_mask
     *   and   a4, a4, a5                ; a4 = F & flag_mask  (Z or C bit isolated)
     *   B(N)EZ a4, .fallthrough          ; skip taken-block if branch NOT taken
     *   <taken-block>: load target into a11, addi a12, 12
     *   J     .end
     *   .fallthrough: load fallthrough into a11, addi a12, 8
     *   .end:
     */
    if (opcode == 0x20 || opcode == 0x28 || opcode == 0x30 || opcode == 0x38) {
        i8 off = (i8)mmu_read8(m, (u16)(pc + 1));
        u16 target      = (u16)(pc + 2 + off);
        u16 fallthrough = (u16)(pc + 2);
        u8 cc = (opcode >> 3) & 3;
        u8 flag_mask = (cc < 2) ? FLAG_Z : FLAG_C;
        bool taken_when_zero = ((cc & 1) == 0);   /* NZ/NC vs Z/C */

        xt_l8ui(e, 4, 13, OFF_F);
        xt_movi(e, 5, (i32)flag_mask);
        xt_and(e, 4, 4, 5);

        /* Branch over taken-block when condition NOT met. */
        u32 br_pos = e->len;
        if (taken_when_zero) xt_bnez(e, 4, 4);    /* taken=a4==0 → skip when a4!=0 */
        else                 xt_beqz(e, 4, 4);    /* taken=a4!=0 → skip when a4==0 */

        /* Taken-block. */
        emit_load_u16(e, 11, target, 2);
        xt_addi(e, 12, 12, 12);

        /* Jump to end. */
        u32 j_pos = e->len;
        xt_j(e, 4);

        /* Fallthrough-block (target of the conditional branch). */
        u32 fallthrough_pos = e->len;
        emit_load_u16(e, 11, fallthrough, 2);
        xt_addi(e, 12, 12, 8);

        u32 end_pos = e->len;
        patch_branch_to(e, br_pos, fallthrough_pos);
        patch_j_to(e, j_pos, end_pos);
        return true;
    }

    /* --- CALL a16 (0xCD) and RST nn (0xC7/CF/D7/DF/E7/EF/F7/FF) — push return
     * PC and jump. WRAM-stack fast path; helper fallback for HRAM stacks. */
    if (opcode == 0xCD || (opcode & 0xC7) == 0xC7) {
        u16 ret_pc, target;
        if (opcode == 0xCD) {
            u8 lo = mmu_read8(m, (u16)(pc + 1));
            u8 hi = mmu_read8(m, (u16)(pc + 2));
            target = (u16)(lo | (hi << 8));
            ret_pc = (u16)(pc + 3);
        } else {
            target = (u16)(opcode & 0x38);   /* RST $00/$08/.../$38 */
            ret_pc = (u16)(pc + 1);
        }
        u8 lo_ret = (u8)(ret_pc & 0xFF);
        u8 hi_ret = (u8)((ret_pc >> 8) & 0xFF);
        u8 cycles_taken = (opcode == 0xCD) ? 24 : 16;

        u32 wram_base_minus_C000 =
            ictx->mmu_base_value + (u32)offsetof(mmu, wram) - 0xC000u;
        i32 wram_lit = lit_alloc_u32(ictx->L, wram_base_minus_C000);
        if (wram_lit < 0) return false;

        /* a4 = SP, a8 = new_SP (= SP - 2). */
        xt_l16ui(e, 4, 13, OFF_SP);
        xt_addi(e, 8, 4, -2);
        xt_extui(e, 8, 8, 0, 15);

        /* WRAM range check on new_SP. */
        xt_extui(e, 5, 8, 13, 2);
        xt_addi(e, 5, 5, -6);

        u32 br_to_slow = e->len;
        xt_bnez(e, 5, 4);

        /* Fast path. */
        xt_s16i(e, 8, 13, OFF_SP);
        u32 pc_off = ictx->entry_off + e->len;
        emit_l32r_at(e, 6, (u32)wram_lit, pc_off);
        xt_add(e, 5, 6, 8);                  /* byte ptr at SP */
        xt_movi(e, 7, (i32)lo_ret);
        xt_s8i(e, 7, 5, 0);
        xt_movi(e, 7, (i32)hi_ret);
        xt_s8i(e, 7, 5, 1);
        emit_load_u16(e, 11, target, 2);
        xt_addi(e, 12, 12, cycles_taken);

        u32 j_to_end = e->len;
        xt_j(e, 4);

        /* Slow path. */
        u32 slow_pos = e->len;
        emit_sync_state(e);
        xt_mov(e, 2, 13);
        emit_callx0_helper(e, ictx->lit_off[HELPER_SM83_STEP], ictx->entry_off);
        emit_reload_state(e, ictx->lit_off[ADDR_CPU_BASE], ictx->entry_off);

        u32 end_pos = e->len;
        patch_branch_to(e, br_to_slow, slow_pos);
        patch_j_to(e, j_to_end, end_pos);
        return true;
    }

    /* --- PUSH rr (0xC5 BC, 0xD5 DE, 0xE5 HL, 0xF5 AF). WRAM-stack fast-path. */
    if (opcode == 0xC5 || opcode == 0xD5 || opcode == 0xE5 || opcode == 0xF5) {
        u8 pair = (opcode >> 4) & 3;
        u32 hi_off = 0, lo_off = 0;
        bool is_af = (pair == 3);
        switch (pair) {
            case 0: hi_off = OFF_B; lo_off = OFF_C; break;
            case 1: hi_off = OFF_D; lo_off = OFF_E; break;
            case 2: hi_off = OFF_H; lo_off = OFF_L; break;
            case 3: hi_off = OFF_A; lo_off = OFF_F; break;
        }

        u32 wram_base_minus_C000 =
            ictx->mmu_base_value + (u32)offsetof(mmu, wram) - 0xC000u;
        i32 wram_lit = lit_alloc_u32(ictx->L, wram_base_minus_C000);
        if (wram_lit < 0) return false;

        xt_l16ui(e, 4, 13, OFF_SP);
        xt_addi(e, 8, 4, -2);
        xt_extui(e, 8, 8, 0, 15);
        xt_extui(e, 5, 8, 13, 2);
        xt_addi(e, 5, 5, -6);

        u32 br_to_slow = e->len;
        xt_bnez(e, 5, 4);

        /* Fast path. */
        xt_s16i(e, 8, 13, OFF_SP);
        u32 pc_off = ictx->entry_off + e->len;
        emit_l32r_at(e, 6, (u32)wram_lit, pc_off);
        xt_add(e, 5, 6, 8);
        xt_l8ui(e, 7, 13, lo_off);
        if (is_af) {                       /* PUSH AF: F's low nibble = 0 */
            xt_movi(e, 9, 0xF0);
            xt_and(e, 7, 7, 9);
        }
        xt_s8i(e, 7, 5, 0);
        xt_l8ui(e, 7, 13, hi_off);
        xt_s8i(e, 7, 5, 1);
        emit_advance(e, 1, 16);

        u32 j_to_end = e->len;
        xt_j(e, 4);

        /* Slow path. */
        u32 slow_pos = e->len;
        emit_sync_state(e);
        xt_mov(e, 2, 13);
        emit_callx0_helper(e, ictx->lit_off[HELPER_SM83_STEP], ictx->entry_off);
        emit_reload_state(e, ictx->lit_off[ADDR_CPU_BASE], ictx->entry_off);

        u32 end_pos = e->len;
        patch_branch_to(e, br_to_slow, slow_pos);
        patch_j_to(e, j_to_end, end_pos);
        return true;
    }

    /* --- POP rr (0xC1 BC, 0xD1 DE, 0xE1 HL, 0xF1 AF). */
    if (opcode == 0xC1 || opcode == 0xD1 || opcode == 0xE1 || opcode == 0xF1) {
        u8 pair = (opcode >> 4) & 3;
        u32 hi_off = 0, lo_off = 0;
        bool is_af = (pair == 3);
        switch (pair) {
            case 0: hi_off = OFF_B; lo_off = OFF_C; break;
            case 1: hi_off = OFF_D; lo_off = OFF_E; break;
            case 2: hi_off = OFF_H; lo_off = OFF_L; break;
            case 3: hi_off = OFF_A; lo_off = OFF_F; break;
        }

        u32 wram_base_minus_C000 =
            ictx->mmu_base_value + (u32)offsetof(mmu, wram) - 0xC000u;
        i32 wram_lit = lit_alloc_u32(ictx->L, wram_base_minus_C000);
        if (wram_lit < 0) return false;

        xt_l16ui(e, 4, 13, OFF_SP);
        xt_extui(e, 5, 4, 13, 2);
        xt_addi(e, 5, 5, -6);

        u32 br_to_slow = e->len;
        xt_bnez(e, 5, 4);

        /* Fast path: a5 = byte ptr; load low → lo_off, high → hi_off. */
        u32 pc_off = ictx->entry_off + e->len;
        emit_l32r_at(e, 6, (u32)wram_lit, pc_off);
        xt_add(e, 5, 6, 4);
        xt_l8ui(e, 7, 5, 0);
        if (is_af) {
            xt_movi(e, 9, 0xF0);
            xt_and(e, 7, 7, 9);
        }
        xt_s8i(e, 7, 13, lo_off);
        xt_l8ui(e, 7, 5, 1);
        xt_s8i(e, 7, 13, hi_off);
        /* SP += 2 */
        xt_addi(e, 4, 4, 2);
        xt_extui(e, 4, 4, 0, 15);
        xt_s16i(e, 4, 13, OFF_SP);
        emit_advance(e, 1, 12);

        u32 j_to_end = e->len;
        xt_j(e, 4);

        u32 slow_pos = e->len;
        emit_sync_state(e);
        xt_mov(e, 2, 13);
        emit_callx0_helper(e, ictx->lit_off[HELPER_SM83_STEP], ictx->entry_off);
        emit_reload_state(e, ictx->lit_off[ADDR_CPU_BASE], ictx->entry_off);

        u32 end_pos = e->len;
        patch_branch_to(e, br_to_slow, slow_pos);
        patch_j_to(e, j_to_end, end_pos);
        return true;
    }

    /* --- LD (HL+),A (0x22), LD (HL-),A (0x32), LD A,(HL+) (0x2A), LD A,(HL-) (0x3A).
     * WRAM fast-path; HL is updated by ±1 after the access. */
    if (opcode == 0x22 || opcode == 0x32 || opcode == 0x2A || opcode == 0x3A) {
        bool is_load = (opcode == 0x2A || opcode == 0x3A);
        int hl_delta = (opcode == 0x32 || opcode == 0x3A) ? -1 : 1;

        u32 wram_base_minus_C000 =
            ictx->mmu_base_value + (u32)offsetof(mmu, wram) - 0xC000u;
        i32 wram_lit = lit_alloc_u32(ictx->L, wram_base_minus_C000);
        if (wram_lit < 0) return false;

        xt_l16ui(e, 9, 13, OFF_HL);
        xt_extui(e, 4, 9, 13, 2);
        xt_addi(e, 4, 4, -6);

        u32 br_to_slow = e->len;
        xt_bnez(e, 4, 4);

        /* Fast path. */
        u32 pc_off = ictx->entry_off + e->len;
        emit_l32r_at(e, 5, (u32)wram_lit, pc_off);
        xt_add(e, 4, 5, 9);
        if (is_load) {
            xt_l8ui(e, 2, 4, 0);
            xt_s8i(e, 2, 13, OFF_A);
        } else {
            xt_l8ui(e, 2, 13, OFF_A);
            xt_s8i(e, 2, 4, 0);
        }
        /* HL ± 1 */
        xt_addi(e, 9, 9, hl_delta);
        xt_extui(e, 9, 9, 0, 15);
        xt_s16i(e, 9, 13, OFF_HL);
        emit_advance(e, 1, 8);

        u32 j_to_end = e->len;
        xt_j(e, 4);

        /* Slow path. */
        u32 slow_pos = e->len;
        emit_sync_state(e);
        xt_mov(e, 2, 13);
        emit_callx0_helper(e, ictx->lit_off[HELPER_SM83_STEP], ictx->entry_off);
        emit_reload_state(e, ictx->lit_off[ADDR_CPU_BASE], ictx->entry_off);

        u32 end_pos = e->len;
        patch_branch_to(e, br_to_slow, slow_pos);
        patch_j_to(e, j_to_end, end_pos);
        return true;
    }

    /* --- RET (0xC9) — pop return PC. WRAM-stack fast path. */
    if (opcode == 0xC9) {
        u32 wram_base_minus_C000 =
            ictx->mmu_base_value + (u32)offsetof(mmu, wram) - 0xC000u;
        i32 wram_lit = lit_alloc_u32(ictx->L, wram_base_minus_C000);
        if (wram_lit < 0) return false;

        xt_l16ui(e, 4, 13, OFF_SP);
        xt_extui(e, 5, 4, 13, 2);
        xt_addi(e, 5, 5, -6);

        u32 br_to_slow = e->len;
        xt_bnez(e, 5, 4);

        /* Fast path: a11 = M16[SP], SP += 2. */
        u32 pc_off = ictx->entry_off + e->len;
        emit_l32r_at(e, 6, (u32)wram_lit, pc_off);
        xt_add(e, 5, 6, 4);
        xt_l8ui(e, 7, 5, 0);
        xt_l8ui(e, 8, 5, 1);
        xt_slli(e, 8, 8, 8);
        xt_or(e, 11, 7, 8);                  /* new PC */
        xt_addi(e, 4, 4, 2);
        xt_extui(e, 4, 4, 0, 15);
        xt_s16i(e, 4, 13, OFF_SP);
        xt_addi(e, 12, 12, 16);

        u32 j_to_end = e->len;
        xt_j(e, 4);

        u32 slow_pos = e->len;
        emit_sync_state(e);
        xt_mov(e, 2, 13);
        emit_callx0_helper(e, ictx->lit_off[HELPER_SM83_STEP], ictx->entry_off);
        emit_reload_state(e, ictx->lit_off[ADDR_CPU_BASE], ictx->entry_off);

        u32 end_pos = e->len;
        patch_branch_to(e, br_to_slow, slow_pos);
        patch_j_to(e, j_to_end, end_pos);
        return true;
    }

    /* --- JP a16 (0xC3) — unconditional, block terminator. */
    if (opcode == 0xC3) {
        u8 lo = mmu_read8(m, (u16)(pc + 1));
        u8 hi = mmu_read8(m, (u16)(pc + 2));
        u16 target = (u16)(lo | (hi << 8));
        emit_load_u16(e, 11, target, 2);
        xt_addi(e, 12, 12, 16);
        return true;
    }

    /* --- JP cc, a16 (0xC2/0xCA/0xD2/0xDA). */
    if (opcode == 0xC2 || opcode == 0xCA || opcode == 0xD2 || opcode == 0xDA) {
        u8 lo = mmu_read8(m, (u16)(pc + 1));
        u8 hi = mmu_read8(m, (u16)(pc + 2));
        u16 target      = (u16)(lo | (hi << 8));
        u16 fallthrough = (u16)(pc + 3);
        u8 cc = (opcode >> 4) & 1; /* 0=Z-group (NZ/Z), 1=C-group (NC/C) */
        u8 flag_mask = cc ? FLAG_C : FLAG_Z;
        bool taken_when_zero = ((opcode & 0x08) == 0); /* 0xC2/0xD2 = NZ/NC */

        xt_l8ui(e, 4, 13, OFF_F);
        xt_movi(e, 5, (i32)flag_mask);
        xt_and(e, 4, 4, 5);

        u32 br_pos = e->len;
        if (taken_when_zero) xt_bnez(e, 4, 4);
        else                 xt_beqz(e, 4, 4);

        emit_load_u16(e, 11, target, 2);
        xt_addi(e, 12, 12, 16);

        u32 j_pos = e->len;
        xt_j(e, 4);

        u32 fallthrough_pos = e->len;
        emit_load_u16(e, 11, fallthrough, 2);
        xt_addi(e, 12, 12, 12);

        u32 end_pos = e->len;
        patch_branch_to(e, br_pos, fallthrough_pos);
        patch_j_to(e, j_pos, end_pos);
        return true;
    }

    /* --- LD A, (a16) (0xFA) / LD (a16), A (0xEA) — absolute 16-bit address.
     *
     * Inlined only if a16 lands in a region whose byte storage is just a
     * flat array on the mmu struct (WRAM or HRAM). IO writes (FF00..FF7F)
     * stay on the helper because they carry side-effects (serial trap,
     * timer regs, IF/IE, etc.). Region-check is fully resolved at codegen
     * time — no runtime branch. */
    if (opcode == 0xFA || opcode == 0xEA) {
        u8 lo = mmu_read8(m, (u16)(pc + 1));
        u8 hi = mmu_read8(m, (u16)(pc + 2));
        u16 a16 = (u16)(lo | (hi << 8));
        u32 byte_addr = inlinable_byte_addr(ictx->mmu_base_value, a16);
        if (!byte_addr) return false;          /* IO / VRAM / ECHO → helper */
        i32 lit_off = lit_alloc_u32(ictx->L, byte_addr);
        if (lit_off < 0) return false;         /* pool full → helper */
        u32 pc_off = ictx->entry_off + e->len;
        emit_l32r_at(e, 2, (u32)lit_off, pc_off);
        if (opcode == 0xFA) {                  /* LD A,(a16) */
            xt_l8ui(e, 3, 2, 0);
            xt_s8i(e, 3, 13, OFF_A);
        } else {                               /* LD (a16),A */
            xt_l8ui(e, 3, 13, OFF_A);
            xt_s8i(e, 3, 2, 0);
        }
        emit_advance(e, 3, 16);
        return true;
    }

    /* --- LDH (n8), A (0xE0)  /  LDH A, (n8) (0xF0).
     * GB address = 0xFF00 | n8. Only inline when n8 ≥ 0x80 (HRAM range);
     * leave IO accesses (0xFF00..0xFF7F) on the helper. */
    if (opcode == 0xE0 || opcode == 0xF0) {
        u8 n8 = mmu_read8(m, (u16)(pc + 1));
        if (n8 < 0x80) return false;
        u16 a16 = (u16)(0xFF00u | n8);
        u32 byte_addr = inlinable_byte_addr(ictx->mmu_base_value, a16);
        if (!byte_addr) return false;
        i32 lit_off = lit_alloc_u32(ictx->L, byte_addr);
        if (lit_off < 0) return false;
        u32 pc_off = ictx->entry_off + e->len;
        emit_l32r_at(e, 2, (u32)lit_off, pc_off);
        if (opcode == 0xF0) {                  /* LDH A,(n8) */
            xt_l8ui(e, 3, 2, 0);
            xt_s8i(e, 3, 13, OFF_A);
        } else {                               /* LDH (n8),A */
            xt_l8ui(e, 3, 13, OFF_A);
            xt_s8i(e, 3, 2, 0);
        }
        emit_advance(e, 2, 12);
        return true;
    }

    /* --- LD rr, n16 — 0x01 BC, 0x11 DE, 0x21 HL, 0x31 SP. */
    if (opcode == 0x01 || opcode == 0x11 || opcode == 0x21 || opcode == 0x31) {
        u8 pair = (opcode >> 4) & 3;
        u32 off = OFF_BC;
        switch (pair) {
            case 0: off = OFF_BC; break;
            case 1: off = OFF_DE; break;
            case 2: off = OFF_HL; break;
            case 3: off = OFF_SP; break;
        }
        u8 lo = mmu_read8(m, (u16)(pc + 1));
        u8 hi = mmu_read8(m, (u16)(pc + 2));
        u32 imm16 = ((u32)hi << 8) | lo;
        /* Build imm16 in a2 via two MOVIs (each ≤ 255 fits in MOVI's signed 12). */
        xt_movi(e, 2, (i32)lo);
        if (hi != 0) {
            xt_movi(e, 3, (i32)hi);
            xt_slli(e, 3, 3, 8);
            xt_or(e, 2, 2, 3);
        }
        (void)imm16;
        xt_s16i(e, 2, 13, off);
        emit_advance(e, 3, 12);
        return true;
    }

    /* --- CB-prefix ops (0xCB <sub>).
     *
     * The sub-byte's structure:
     *   bits 6..7 : group  (0 = shift/rotate, 1 = BIT, 2 = RES, 3 = SET)
     *   bits 3..5 : op-within-group (0..7)
     *   bits 0..2 : source register (B,C,D,E,H,L,(HL),A)
     *
     * Non-(HL) variants are fully inlined with eager flags. (HL) variants
     * (src==6) fall back to the helper.                                    */
    if (opcode == 0xCB) {
        u8 sub = mmu_read8(m, (u16)(pc + 1));
        u8 reg = sub & 7;
        u8 grp = (sub >> 6) & 3;
        u8 sub_op = (sub >> 3) & 7;
        if (reg == 6) return false;        /* (HL) — helper */
        int reg_off = reg8_offset(reg);

        /* Load operand value into a2. */
        xt_l8ui(e, 2, 13, (u32)reg_off);

        if (grp == 0) {
            /* --- Shift/rotate. We compute the new value in a5 and the new
             * carry bit (0 or 1) in a6. Old F lives in a4 (loaded only
             * when needed for RL/RR).                                      */
            switch (sub_op) {
                case 0: /* RLC */
                    xt_extui(e, 6, 2, 7, 0);        /* new_C = bit 7 */
                    xt_slli(e, 5, 2, 1);
                    xt_or(e, 5, 5, 6);              /* fold bit 7 into bit 0 */
                    xt_extui(e, 5, 5, 0, 7);        /* mask to 8 bits */
                    break;
                case 1: /* RRC */
                    xt_extui(e, 6, 2, 0, 0);        /* new_C = bit 0 */
                    xt_srli(e, 5, 2, 1);
                    xt_slli(e, 7, 6, 7);            /* bit 0 → position 7 */
                    xt_or(e, 5, 5, 7);
                    break;
                case 2: /* RL — through carry */
                    xt_l8ui(e, 4, 13, OFF_F);
                    xt_extui(e, 6, 2, 7, 0);        /* new_C = bit 7 */
                    xt_extui(e, 7, 4, 4, 0);        /* old C from F bit 4 */
                    xt_slli(e, 5, 2, 1);
                    xt_or(e, 5, 5, 7);
                    xt_extui(e, 5, 5, 0, 7);
                    break;
                case 3: /* RR — through carry */
                    xt_l8ui(e, 4, 13, OFF_F);
                    xt_extui(e, 6, 2, 0, 0);        /* new_C = bit 0 */
                    xt_extui(e, 7, 4, 4, 0);        /* old C */
                    xt_srli(e, 5, 2, 1);
                    xt_slli(e, 7, 7, 7);            /* old_C → bit 7 */
                    xt_or(e, 5, 5, 7);
                    break;
                case 4: /* SLA */
                    xt_extui(e, 6, 2, 7, 0);        /* new_C = bit 7 */
                    xt_slli(e, 5, 2, 1);
                    xt_extui(e, 5, 5, 0, 7);        /* mask to 8 bits */
                    break;
                case 5: /* SRA — arithmetic right (preserve bit 7) */
                    xt_extui(e, 6, 2, 0, 0);        /* new_C = bit 0 */
                    xt_srli(e, 5, 2, 1);
                    xt_extui(e, 7, 2, 7, 0);        /* old sign bit */
                    xt_slli(e, 7, 7, 7);
                    xt_or(e, 5, 5, 7);
                    break;
                case 6: /* SWAP nibbles */
                    xt_slli(e, 7, 2, 4);
                    xt_extui(e, 7, 7, 0, 7);
                    xt_srli(e, 5, 2, 4);
                    xt_or(e, 5, 5, 7);
                    xt_movi(e, 6, 0);              /* C = 0 */
                    break;
                case 7: /* SRL — logical right */
                    xt_extui(e, 6, 2, 0, 0);
                    xt_srli(e, 5, 2, 1);
                    break;
                default: return false;
            }

            xt_s8i(e, 5, 13, (u32)reg_off);

            /* F = Z | (C in position 4). N = 0, H = 0. */
            xt_movi(e, 4, 0);
            emit_zflag(e, 5, 4, 7);
            xt_slli(e, 6, 6, 4);                   /* C bit → F bit 4 */
            xt_or(e, 4, 4, 6);
            xt_s8i(e, 4, 13, OFF_F);
            emit_advance(e, 2, 8);
            return true;
        }

        if (grp == 1) {  /* BIT b, r — sets Z if bit b of r is 0, N=0, H=1, C preserved */
            u8 bit = sub_op;
            xt_extui(e, 3, 2, bit, 0);             /* a3 = (r >> bit) & 1 */
            xt_l8ui(e, 4, 13, OFF_F);
            xt_movi(e, 5, FLAG_C);
            xt_and(e, 4, 4, 5);                    /* a4 = old F & C */
            emit_zflag(e, 3, 4, 7);
            emit_setflag_const(e, 4, FLAG_H, 7);
            xt_s8i(e, 4, 13, OFF_F);
            emit_advance(e, 2, 8);
            return true;
        }

        if (grp == 2) {  /* RES b, r — clear bit b of r; no flag effects */
            u8 bit = sub_op;
            u8 mask = (u8)~(1u << bit);
            xt_movi(e, 3, (i32)mask);
            xt_and(e, 2, 2, 3);
            xt_s8i(e, 2, 13, (u32)reg_off);
            emit_advance(e, 2, 8);
            return true;
        }

        if (grp == 3) {  /* SET b, r */
            u8 bit = sub_op;
            u8 mask = (u8)(1u << bit);
            xt_movi(e, 3, (i32)mask);
            xt_or(e, 2, 2, 3);
            xt_s8i(e, 2, 13, (u32)reg_off);
            emit_advance(e, 2, 8);
            return true;
        }
    }

    /* --- ALU operand-in-`a3` body (factored from `ALU A,r` and `ALU A,n8`).
     * Caller arranges a2=A and a3=operand and supplies the group (0..7
     * matching the standard SM83 encoding: 0=ADD 1=ADC 2=SUB 3=SBC
     * 4=AND 5=XOR 6=OR 7=CP). Returns false if the group is unsupported
     * (ADC/SBC currently not inlined). On success the function writes the
     * new A (except for CP) and a fresh F. */
    /* Forward declare via a sentinel return; see end of function. */
#define ALU_INLINE(group_var)                                              \
    do {                                                                    \
        u8 _grp = (group_var);                                              \
        u8 _f = 6;                                                          \
        xt_movi(e, _f, 0);                                                  \
        switch (_grp) {                                                     \
            case 0: /* ADD */                                                \
                xt_add(e, 4, 2, 3);                                         \
                xt_extui(e, 5, 4, 0, 7);                                    \
                xt_s8i(e, 5, 13, OFF_A);                                    \
                emit_zflag(e, 5, _f, 7);                                    \
                emit_hflag_add(e, 2, 3, _f, 7, 8);                          \
                emit_cflag_from_bit8(e, 4, _f, 7);                          \
                break;                                                      \
            case 1: /* ADC */ {                                              \
                /* a7 = old C bit (extracted from F). */                    \
                xt_l8ui(e, 7, 13, OFF_F);                                   \
                xt_extui(e, 7, 7, 4, 0);                                    \
                xt_add(e, 4, 2, 3);                                         \
                xt_add(e, 4, 4, 7);            /* full sum = A + r + C */   \
                xt_extui(e, 5, 4, 0, 7);                                    \
                xt_s8i(e, 5, 13, OFF_A);                                    \
                emit_zflag(e, 5, _f, 8);                                    \
                /* H = bit 4 of (lo(A) + lo(r) + C). */                     \
                xt_extui(e, 8, 2, 0, 3);                                    \
                xt_extui(e, 9, 3, 0, 3);                                    \
                xt_add(e, 8, 8, 9);                                         \
                xt_add(e, 8, 8, 7);                                         \
                xt_extui(e, 8, 8, 4, 0);                                    \
                xt_slli(e, 8, 8, 5);                                        \
                xt_or(e, _f, _f, 8);                                        \
                /* C = bit 8 of full sum. */                                \
                xt_extui(e, 8, 4, 8, 0);                                    \
                xt_slli(e, 8, 8, 4);                                        \
                xt_or(e, _f, _f, 8);                                        \
                break;                                                      \
            }                                                               \
            case 2: /* SUB */                                                \
                xt_sub(e, 4, 2, 3);                                         \
                xt_extui(e, 5, 4, 0, 7);                                    \
                xt_s8i(e, 5, 13, OFF_A);                                    \
                emit_zflag(e, 5, _f, 7);                                    \
                emit_setflag_const(e, _f, FLAG_N, 7);                       \
                emit_hflag_sub(e, 2, 3, _f, 7, 8);                          \
                emit_cflag_from_bit8(e, 4, _f, 7);                          \
                break;                                                      \
            case 3: /* SBC */ {                                              \
                xt_l8ui(e, 7, 13, OFF_F);                                   \
                xt_extui(e, 7, 7, 4, 0);                                    \
                xt_sub(e, 4, 2, 3);                                         \
                xt_sub(e, 4, 4, 7);                                         \
                xt_extui(e, 5, 4, 0, 7);                                    \
                xt_s8i(e, 5, 13, OFF_A);                                    \
                emit_zflag(e, 5, _f, 8);                                    \
                emit_setflag_const(e, _f, FLAG_N, 8);                       \
                /* H = bit 4 of (lo(A) - lo(r) - C). */                     \
                xt_extui(e, 8, 2, 0, 3);                                    \
                xt_extui(e, 9, 3, 0, 3);                                    \
                xt_sub(e, 8, 8, 9);                                         \
                xt_sub(e, 8, 8, 7);                                         \
                xt_extui(e, 8, 8, 4, 0);                                    \
                xt_slli(e, 8, 8, 5);                                        \
                xt_or(e, _f, _f, 8);                                        \
                /* C = bit 8 of full diff. */                               \
                xt_extui(e, 8, 4, 8, 0);                                    \
                xt_slli(e, 8, 8, 4);                                        \
                xt_or(e, _f, _f, 8);                                        \
                break;                                                      \
            }                                                               \
            case 4: /* AND */                                                \
                xt_and(e, 5, 2, 3);                                         \
                xt_s8i(e, 5, 13, OFF_A);                                    \
                emit_zflag(e, 5, _f, 7);                                    \
                emit_setflag_const(e, _f, FLAG_H, 7);                       \
                break;                                                      \
            case 5: /* XOR */                                                \
                xt_xor(e, 5, 2, 3);                                         \
                xt_s8i(e, 5, 13, OFF_A);                                    \
                emit_zflag(e, 5, _f, 7);                                    \
                break;                                                      \
            case 6: /* OR */                                                 \
                xt_or(e, 5, 2, 3);                                          \
                xt_s8i(e, 5, 13, OFF_A);                                    \
                emit_zflag(e, 5, _f, 7);                                    \
                break;                                                      \
            case 7: /* CP — like SUB but no A write */                       \
                xt_sub(e, 4, 2, 3);                                         \
                xt_extui(e, 5, 4, 0, 7);                                    \
                emit_zflag(e, 5, _f, 7);                                    \
                emit_setflag_const(e, _f, FLAG_N, 7);                       \
                emit_hflag_sub(e, 2, 3, _f, 7, 8);                          \
                emit_cflag_from_bit8(e, 4, _f, 7);                          \
                break;                                                      \
            default: return false;                                          \
        }                                                                   \
        xt_s8i(e, _f, 13, OFF_F);                                           \
    } while (0)

    /* --- ALU A,n8 — 0xC6 ADD, 0xCE ADC, 0xD6 SUB, 0xDE SBC,
     *                0xE6 AND, 0xEE XOR, 0xF6 OR,  0xFE CP. */
    if (opcode == 0xC6 || opcode == 0xCE || opcode == 0xD6 || opcode == 0xDE ||
        opcode == 0xE6 || opcode == 0xEE || opcode == 0xF6 || opcode == 0xFE) {
        u8 imm = mmu_read8(m, (u16)(pc + 1));
        u8 group = (opcode >> 3) & 0x7;
        xt_l8ui(e, 2, 13, OFF_A);
        xt_movi(e, 3, (i32)imm);
        ALU_INLINE(group);
        emit_advance(e, 2, 8);
        return true;
    }

    /* --- ALU A,(HL) — 0x86, 0x8E, 0x96, 0x9E, 0xA6, 0xAE, 0xB6, 0xBE.
     * Same WRAM fast-path / helper-fallback pattern as LD r,(HL). */
    if ((opcode >= 0x86 && opcode <= 0xBE) && ((opcode & 7) == 6)) {
        u8 group = (opcode >> 3) & 0x7;

        u32 wram_base_minus_C000 =
            ictx->mmu_base_value + (u32)offsetof(mmu, wram) - 0xC000u;
        i32 wram_lit = lit_alloc_u32(ictx->L, wram_base_minus_C000);
        if (wram_lit < 0) return false;

        xt_l16ui(e, 9, 13, OFF_HL);           /* keep HL in a9 across the inline */
        xt_extui(e, 4, 9, 13, 2);
        xt_addi(e, 4, 4, -6);

        u32 br_to_slow = e->len;
        xt_bnez(e, 4, 4);

        /* Fast path: load (HL) into a3, do ALU(A, (HL)). */
        u32 pc_off = ictx->entry_off + e->len;
        emit_l32r_at(e, 5, (u32)wram_lit, pc_off);
        xt_add(e, 4, 5, 9);
        xt_l8ui(e, 3, 4, 0);                 /* a3 = mem[HL] */
        xt_l8ui(e, 2, 13, OFF_A);            /* a2 = A */
        ALU_INLINE(group);
        emit_advance(e, 1, 8);

        u32 j_to_end = e->len;
        xt_j(e, 4);

        /* Slow path. */
        u32 slow_pos = e->len;
        emit_sync_state(e);
        xt_mov(e, 2, 13);
        emit_callx0_helper(e, ictx->lit_off[HELPER_SM83_STEP], ictx->entry_off);
        emit_reload_state(e, ictx->lit_off[ADDR_CPU_BASE], ictx->entry_off);

        u32 end_pos = e->len;
        patch_branch_to(e, br_to_slow, slow_pos);
        patch_j_to(e, j_to_end, end_pos);
        return true;
    }

    /* --- ALU A,r — 0x80..0xBF.
     *
     * Layout: opcodes 0x80+r..0x87+r form a group of 8 (one per source reg).
     * Source reg index r=6 is (HL) which needs memory access — fall through.
     *
     *   0x80..0x87 ADD A,r
     *   0x88..0x8F ADC A,r  (not yet inlined — uses C flag)
     *   0x90..0x97 SUB r
     *   0x98..0x9F SBC A,r  (not yet inlined — uses C flag)
     *   0xA0..0xA7 AND r
     *   0xA8..0xAF XOR r
     *   0xB0..0xB7 OR  r
     *   0xB8..0xBF CP  r
     */
    if (opcode >= 0x80 && opcode <= 0xBF) {
        u8 group = (opcode >> 3) & 0x7;
        u8 src   = opcode & 7;
        if (src == 6) return false;     /* (HL) variant: helper */
        int src_off = reg8_offset(src);

        xt_l8ui(e, 2, 13, OFF_A);
        if (src == 7) xt_mov(e, 3, 2);
        else          xt_l8ui(e, 3, 13, (u32)src_off);

        ALU_INLINE(group);
        emit_advance(e, 1, 4);
        return true;
    }

    return false;
}

gbjit_block *gbjit_compile_block(codecache *cc, cpu_state *cpu, u16 pc_start,
                                  jit_helper_addr_fn helper_addr, void *user) {
    /* Walk forward to discover block end + collect op list. */
    u16 ops_pc[MAX_OPS_PER_BLOCK];
    u8  ops_opcode[MAX_OPS_PER_BLOCK];
    u8  ops_len[MAX_OPS_PER_BLOCK];
    u32 n_ops = 0;
    u16 cur = pc_start;
    while (n_ops < MAX_OPS_PER_BLOCK) {
        u8 opcode = mmu_read8(cpu->mmu, cur);
        const sm83_op_info *info = sm83_decode(opcode);
        ops_pc[n_ops] = cur;
        ops_opcode[n_ops] = opcode;
        ops_len[n_ops] = info->length;
        n_ops++;
        cur = (u16)(cur + info->length);
        if (sm83_terminates_block(opcode)) break;
    }

    /* Reserve. */
    u32 lit_bytes = LITERAL_POOL_BYTES;
    u32 code_bytes = PROLOGUE_EPILOGUE_BYTES + n_ops * BYTES_PER_OP;
    u32 total = align_up_4(lit_bytes) + align_up_4(code_bytes);
    u8 *base = codecache_alloc(cc, total);
    if (!base) return NULL;
    memset(base, 0, total);

    /* Literal pool layout: one u32 per literal_id at fixed offsets, followed
     * by MAX_EXTRA_LITERALS slots reserved for dynamic per-access literals. */
    u32 lit_off[LITERAL_COUNT];
    u32 wp = 0;
    for (literal_id l = 0; l < LITERAL_COUNT; l++) {
        lit_off[l] = wp;
        u32 v = helper_addr(l, user);
        base[wp + 0] = (u8)(v & 0xFF);
        base[wp + 1] = (u8)((v >> 8) & 0xFF);
        base[wp + 2] = (u8)((v >> 16) & 0xFF);
        base[wp + 3] = (u8)((v >> 24) & 0xFF);
        wp += 4;
    }
    lit_ctx L = { base, wp, wp + (u32)(MAX_EXTRA_LITERALS * 4) };
    wp = L.limit;
    wp = align_up_4(wp);
    u32 entry_off = wp;
    u32 mmu_base_value = helper_addr(ADDR_MMU_BASE, user);

    xt_emit e;
    xt_init(&e, base + entry_off, total - entry_off);

    /* --- Prologue ---
     *   l32r  a13, =cpu_base
     *   s32i  a0,  a13, OFF_JITRETPC   ; save return PC into cpu_state
     *   l16ui a11, a13, OFF_PC
     *   movi  a12, 0                                                 */
    {
        u32 pc_off = entry_off + e.len;
        emit_l32r_at(&e, 13, lit_off[ADDR_CPU_BASE], pc_off);
    }
    xt_s32i(&e, 0, 13, OFF_JITRETPC);
    xt_l16ui(&e, 11, 13, OFF_PC);
    xt_movi (&e, 12, 0);

    /* --- Body --- */
    bool exited_early = false;
    for (u32 i = 0; i < n_ops; i++) {
        u8 opcode = ops_opcode[i];
        u16 op_pc = ops_pc[i];

        inline_ctx ictx = { cpu->mmu, &L, entry_off, mmu_base_value, lit_off };
        if (inline_op(&e, opcode, op_pc, &ictx)) {
            /* Any terminator op (HALT/JR/JP/...) — once inlined the block
             * has already set PC + cycles itself, so we can break out and
             * skip straight to the epilogue. */
            if (sm83_terminates_block(opcode)) { exited_early = true; break; }
            continue;
        }

        /* Helper fallback: sync state → CALLX0 sm83_step(cpu) → reload state */
        emit_sync_state(&e);
        /* arg: a2 = cpu_base */
        xt_mov(&e, 2, 13);
        emit_callx0_helper(&e, lit_off[HELPER_SM83_STEP], entry_off);
        emit_reload_state(&e, lit_off[ADDR_CPU_BASE], entry_off);
    }
    (void)exited_early;

    /* --- Epilogue: sync PC + cycles, reload return PC, JX. --- */
    emit_sync_state(&e);
    /* Reload cpu_base in case the last op was a helper. */
    {
        u32 pc_off = entry_off + e.len;
        emit_l32r_at(&e, 13, lit_off[ADDR_CPU_BASE], pc_off);
    }
    xt_l32i(&e, 0, 13, OFF_JITRETPC);
    xt_jx(&e, 0);

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
