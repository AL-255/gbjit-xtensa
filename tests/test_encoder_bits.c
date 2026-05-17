/* Bit-accuracy tests for the Xtensa LX7 encoder.
 *
 * Each test encodes a single instruction and verifies the resulting 3-byte
 * little-endian word matches the canonical opcode. Reference values were
 * derived from the Xtensa ISA Reference Manual (cross-checked against
 * ida-xtensa2 disassembler opcode tables).
 *
 * Format reminder for 24-bit instructions:
 *    bits  0.. 3 = op0
 *    bits  4.. 7 = t
 *    bits  8..11 = s
 *    bits 12..15 = r
 *    bits 16..19 = op1
 *    bits 20..23 = op2
 */

#include "emit_xtensa.h"
#include <stdio.h>
#include <string.h>

static int failed = 0;

static u32 readw(const u8 *b) { return (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16); }

#define CHECK(label, expected) do { \
    u32 _got = readw(buf); \
    if (_got != (u32)(expected)) { \
        fprintf(stderr, "FAIL %s: expected 0x%06X got 0x%06X\n", \
                label, (unsigned)(expected), (unsigned)_got); \
        failed++; \
    } \
} while (0)

#define ONE_INSTR(call_, expected, label) do { \
    xt_emit e; xt_init(&e, buf, sizeof(buf)); \
    call_; \
    if (e.len != 3) { fprintf(stderr, "FAIL %s: %u bytes\n", label, (unsigned)e.len); failed++; } \
    else CHECK(label, expected); \
} while (0)

int main(void) {
    u8 buf[8];

    /* RRR: ADD a3, a4, a5  -> 0x800000 | 3<<12 | 4<<8 | 5<<4 = 0x803450 */
    ONE_INSTR(xt_add(&e, 3, 4, 5), 0x803450, "ADD a3,a4,a5");
    /* SUB a3, a4, a5 -> 0xC03450 */
    ONE_INSTR(xt_sub(&e, 3, 4, 5), 0xC03450, "SUB a3,a4,a5");
    /* AND a3, a4, a5 -> 0x103450 */
    ONE_INSTR(xt_and(&e, 3, 4, 5), 0x103450, "AND a3,a4,a5");
    /* OR  a3, a4, a5 -> 0x203450 */
    ONE_INSTR(xt_or (&e, 3, 4, 5), 0x203450, "OR  a3,a4,a5");
    /* XOR a3, a4, a5 -> 0x303450 */
    ONE_INSTR(xt_xor(&e, 3, 4, 5), 0x303450, "XOR a3,a4,a5");
    /* MOV a3, a4 := OR a3, a4, a4 -> 0x203440 */
    ONE_INSTR(xt_mov(&e, 3, 4), 0x203440, "MOV a3,a4");

    /* RRI8 ADDI a3, a4, -1 -> 0xFF c0 42  (imm8=0xFF, r=C, s=4, t=3, op0=2) */
    ONE_INSTR(xt_addi(&e, 3, 4, -1), 0xFFC432, "ADDI a3,a4,-1");
    /* ADDI a3, a4, 16 -> 0x10 c0 42 = 0x10C432 */
    ONE_INSTR(xt_addi(&e, 3, 4, 16), 0x10C432, "ADDI a3,a4,16");

    /* MOVI a4, 0x42  (imm=0x042; low8=0x42 at bits 16..23, hi4=0 at s bits 8..11)
       -> 0x42 a0 42 = 0x42A042 */
    ONE_INSTR(xt_movi(&e, 4, 0x42), 0x42A042, "MOVI a4,0x42");
    /* MOVI a4, -1 (imm=0xFFF, low8=0xFF, hi4=0xF) -> 0xFF AF 42 = 0xFFAF42 */
    ONE_INSTR(xt_movi(&e, 4, -1), 0xFFAF42, "MOVI a4,-1");

    /* L8UI a3, a11, 0x10  -> imm8=0x10, r=0, s=11, t=3, op0=2 = 0x10 0B 32 = 0x100B32 */
    ONE_INSTR(xt_l8ui(&e, 3, 11, 0x10), 0x100B32, "L8UI a3,a11,16");
    /* S8I  a3, a11, 0x10  -> r=4 -> 0x10 4B 32 = 0x104B32 */
    ONE_INSTR(xt_s8i (&e, 3, 11, 0x10), 0x104B32, "S8I  a3,a11,16");
    /* L32I a3, a11, 0x10  (off encoded /4 = 0x04) r=2 -> 0x04 2B 32 = 0x042B32 */
    ONE_INSTR(xt_l32i(&e, 3, 11, 0x10), 0x042B32, "L32I a3,a11,16");
    /* S32I a3, a11, 0x10  r=6 -> 0x04 6B 32 = 0x046B32 */
    ONE_INSTR(xt_s32i(&e, 3, 11, 0x10), 0x046B32, "S32I a3,a11,16");
    /* L16UI a3, a11, 0x10 (off /2 = 0x08) r=1 -> 0x08 1B 32 = 0x081B32 */
    ONE_INSTR(xt_l16ui(&e, 3, 11, 0x10), 0x081B32, "L16UI a3,a11,16");
    /* S16I a3, a11, 0x10  r=5 -> 0x08 5B 32 = 0x085B32 */
    ONE_INSTR(xt_s16i (&e, 3, 11, 0x10), 0x085B32, "S16I a3,a11,16");

    /* JX a4: op0=0, t=A, s=4, rest 0  -> 0x0004A0 */
    ONE_INSTR(xt_jx(&e, 4), 0x0004A0, "JX a4");
    /* CALLX0 a4: t=C, s=4 -> 0x0004C0 */
    ONE_INSTR(xt_callx0(&e, 4), 0x0004C0, "CALLX0 a4");
    /* RET: fixed 0x000080 */
    ONE_INSTR(xt_ret(&e), 0x000080, "RET");

    /* BEQZ a3, +4: target = PC + 4 means off (rel-4) = 0, imm12 = 0
        word = (0 << 12) | (3 << 8) | (1 << 4) | 6 = 0x000316 */
    ONE_INSTR(xt_beqz(&e, 3, 4), 0x000316, "BEQZ a3,+4");
    /* BNEZ a3, +4: sel=5 -> 0x000356 */
    ONE_INSTR(xt_bnez(&e, 3, 4), 0x000356, "BNEZ a3,+4");

    /* J +4: off = (rel-4) = 0, imm18=0; opcode bits 0..5 = 0x26
       word = (0 << 6) | 0x26 = 0x000026 */
    ONE_INSTR(xt_j(&e, 4), 0x000026, "J +4");
    /* J +0x10 -> off=0xC, imm18=0xC; word = (0xC<<6)|0x26 = 0x000326 */
    ONE_INSTR(xt_j(&e, 0x10), 0x000326, "J +16");

    /* L32R a3, label at (PC & ~3) - 4: imm16 = 0xFFFF (i.e., -1 in -262140..-4 range)
       Encoding: imm16 in bits 8..23, t in bits 4..7, op0=1.
       Here we pass lit_offset = 0xFFFF: word = 0xFFFF31 */
    ONE_INSTR(xt_l32r(&e, 3, 0xFFFF), 0xFFFF31, "L32R a3,-4");

    /* SLLI a3, a4, 1: sa1 = 31 = 0x1F, sa_lo4 = 0xF, sa_hi1 = 1
        op2 = 0x1 | (1<<3) = 0x9, op1 = 1, r=3, s=4, t=0xF
        word = (9 << 20) | (1 << 16) | (3 << 12) | (4 << 8) | (0xF << 4)
             = 0x9134F0 (but op0 = 0 implicit)
    */
    ONE_INSTR(xt_slli(&e, 3, 4, 1), 0x9134F0, "SLLI a3,a4,1");
    /* SLLI a3, a4, 16: sa1 = 16 = 0x10, lo4=0, hi1=1; op2=0x9, t=0
        word = (9<<20) | (1<<16) | (3<<12) | (4<<8) | 0 = 0x913400 */
    ONE_INSTR(xt_slli(&e, 3, 4, 16), 0x913400, "SLLI a3,a4,16");
    /* SRLI a3, a4, 8: op2=4, op1=1, r=3, s=8, t=4
        word = (4<<20) | (1<<16) | (3<<12) | (8<<8) | (4<<4) = 0x413840 */
    ONE_INSTR(xt_srli(&e, 3, 4, 8), 0x413840, "SRLI a3,a4,8");
    /* SRAI a3, a4, 1: sa_lo4=1, hi1=0; op2=2, op1=1, r=3, s=1, t=4
        word = (2<<20) | (1<<16) | (3<<12) | (1<<8) | (4<<4) = 0x213140 */
    ONE_INSTR(xt_srai(&e, 3, 4, 1), 0x213140, "SRAI a3,a4,1");

    /* EXTUI a3, a4, 0, 7  (extract 8 bits at shift 0 — i.e., AND with 0xFF):
        sh_lo4=0, sh_hi1=0 → op2=4; op1 = mask = 7
        word = (4<<20) | (7<<16) | (3<<12) | (0<<8) | (4<<4) = 0x473040 */
    ONE_INSTR(xt_extui(&e, 3, 4, 0, 7), 0x473040, "EXTUI a3,a4,0,7");
    /* EXTUI a3, a4, 8, 7 (byte at shift 8 -> extract second byte):
        sh_lo4=8, sh_hi1=0 → op2=4
        word = (4<<20) | (7<<16) | (3<<12) | (8<<8) | (4<<4) = 0x473840 */
    ONE_INSTR(xt_extui(&e, 3, 4, 8, 7), 0x473840, "EXTUI a3,a4,8,7");

    if (failed) {
        fprintf(stderr, "%d encoder check(s) failed\n", failed);
        return 1;
    }
    printf("encoder bits: OK\n");
    return 0;
}
