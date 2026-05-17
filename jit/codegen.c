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

/* --- Stack frame (24 bytes, 16-byte aligned).
 *   +20  saved a0       (return address)
 *   +16  unused (alignment) */
#define FRAME_SIZE 32
#define FRAME_OFF_A0 28

/* Limits. */
#define MAX_OPS_PER_BLOCK 32

/* Generous size budget per op. Worst-case inline is currently ALU with eager
 * flags (~24 Xtensa instructions = 72 bytes). Helper fallback is ~32 bytes.
 * Round up to 96 to leave headroom for future inlining additions. */
#define BYTES_PER_OP 96
#define PROLOGUE_EPILOGUE_BYTES 128
#define LITERAL_POOL_BYTES (LITERAL_COUNT * 4)

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

/* Try to inline a GB op. Returns true on success, false to request the
 * helper-fallback path. */
static bool inline_op(xt_emit *e, u8 opcode, u16 pc, mmu *m) {
    (void)pc;
    /* NOP */
    if (opcode == 0x00) { emit_advance(e, 1, 4); return true; }

    /* LD r,n8 — 0x06 0x0E 0x16 0x1E 0x26 0x2E 0x3E  (NOT 0x36 = LD (HL),n8) */
    if (opcode == 0x06 || opcode == 0x0E || opcode == 0x16 || opcode == 0x1E ||
        opcode == 0x26 || opcode == 0x2E || opcode == 0x3E) {
        u8 dst = (opcode >> 3) & 7;
        int off = reg8_offset(dst);
        if (off < 0) return false;
        u8 imm = m->rom[(pc + 1) & 0x7FFFu];
        /* movi a2, imm ; s8i a2, a13, off */
        xt_movi(e, 2, (i32)imm);
        xt_s8i(e, 2, 13, (u32)off);
        emit_advance(e, 2, 8);
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
        u8 lo = m->rom[(pc + 1) & 0x7FFFu];
        u8 hi = m->rom[(pc + 2) & 0x7FFFu];
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
        u8 group = (opcode >> 3) & 0x7;   /* 0..7 → ADD..CP */
        u8 src   = opcode & 7;
        if (src == 6) return false;       /* (HL) variant: helper */
        if (group == 1 || group == 3) return false; /* ADC/SBC not yet inlined */
        int src_off = reg8_offset(src);

        /* Load operands. a2=A, a3=r. For A,A skip the second load. */
        xt_l8ui(e, 2, 13, OFF_A);
        if (src == 7) xt_mov(e, 3, 2);
        else          xt_l8ui(e, 3, 13, (u32)src_off);

        u8 a6_dst = 6;  /* F accumulator */
        xt_movi(e, a6_dst, 0);

        switch (group) {
            case 0: /* ADD */ {
                xt_add(e, 4, 2, 3);                 /* a4 = full sum */
                xt_extui(e, 5, 4, 0, 7);            /* a5 = sum & 0xFF (new A) */
                xt_s8i(e, 5, 13, OFF_A);
                emit_zflag(e, 5, a6_dst, 7);
                /* N = 0 — already 0 */
                emit_hflag_add(e, 2, 3, a6_dst, 7, 8);
                emit_cflag_from_bit8(e, 4, a6_dst, 7);
                break;
            }
            case 2: /* SUB */ {
                xt_sub(e, 4, 2, 3);                 /* a4 = signed difference */
                xt_extui(e, 5, 4, 0, 7);
                xt_s8i(e, 5, 13, OFF_A);
                emit_zflag(e, 5, a6_dst, 7);
                emit_setflag_const(e, a6_dst, FLAG_N, 7);
                emit_hflag_sub(e, 2, 3, a6_dst, 7, 8);
                /* C: bit 8 of (a4) is 1 iff A < r (negative result) */
                emit_cflag_from_bit8(e, 4, a6_dst, 7);
                break;
            }
            case 4: /* AND */ {
                xt_and(e, 5, 2, 3);
                xt_s8i(e, 5, 13, OFF_A);
                emit_zflag(e, 5, a6_dst, 7);
                emit_setflag_const(e, a6_dst, FLAG_H, 7);  /* AND always sets H */
                break;
            }
            case 5: /* XOR */ {
                xt_xor(e, 5, 2, 3);
                xt_s8i(e, 5, 13, OFF_A);
                emit_zflag(e, 5, a6_dst, 7);
                /* N=0, H=0, C=0 */
                break;
            }
            case 6: /* OR */ {
                xt_or(e, 5, 2, 3);
                xt_s8i(e, 5, 13, OFF_A);
                emit_zflag(e, 5, a6_dst, 7);
                /* N=0, H=0, C=0 */
                break;
            }
            case 7: /* CP — like SUB but doesn't write A */ {
                xt_sub(e, 4, 2, 3);
                xt_extui(e, 5, 4, 0, 7);
                /* (no S8I) */
                emit_zflag(e, 5, a6_dst, 7);
                emit_setflag_const(e, a6_dst, FLAG_N, 7);
                emit_hflag_sub(e, 2, 3, a6_dst, 7, 8);
                emit_cflag_from_bit8(e, 4, a6_dst, 7);
                break;
            }
            default: return false;
        }

        xt_s8i(e, a6_dst, 13, OFF_F);
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
        u8 opcode = cpu->mmu->rom[cur & 0x7FFFu];
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

    /* Literal pool layout: one u32 per literal_id, in order. */
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
    wp = align_up_4(wp);
    u32 entry_off = wp;

    xt_emit e;
    xt_init(&e, base + entry_off, total - entry_off);

    /* --- Prologue ---
     *   addi a1, a1, -FRAME_SIZE
     *   s32i a0, a1, FRAME_OFF_A0
     *   l32r a13, =cpu_base
     *   l16ui a11, a13, OFF_PC
     *   movi  a12, 0                   ; cycles delta = 0          */
    xt_addi(&e, 1, 1, -FRAME_SIZE);
    xt_s32i(&e, 0, 1, FRAME_OFF_A0);
    {
        u32 pc_off = entry_off + e.len;
        emit_l32r_at(&e, 13, lit_off[ADDR_CPU_BASE], pc_off);
    }
    xt_l16ui(&e, 11, 13, OFF_PC);
    xt_movi (&e, 12, 0);

    /* --- Body --- */
    bool exited_early = false;
    for (u32 i = 0; i < n_ops; i++) {
        u8 opcode = ops_opcode[i];
        u16 op_pc = ops_pc[i];

        if (inline_op(&e, opcode, op_pc, cpu->mmu)) {
            /* HALT exits the block early to give the dispatcher a chance to
             * notice cpu->halted. */
            if (opcode == 0x76) { exited_early = true; break; }
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

    /* --- Epilogue --- */
    emit_sync_state(&e);
    xt_l32i(&e, 0, 1, FRAME_OFF_A0);
    xt_addi(&e, 1, 1, FRAME_SIZE);
    xt_ret(&e);

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
