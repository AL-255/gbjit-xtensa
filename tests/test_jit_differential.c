/* Differential test: run the same GB ROM through the interpreter and through
 * the JIT (which on host drives the Xtensa sim), and confirm that the final
 * cpu_state matches. This proves the JIT pipeline is correct end-to-end:
 *   block discovery → codegen → codecache → sim execution → cpu_state update.
 *
 * The v0 JIT emits one CALLX0 to sm83_step per op, so the JIT result must
 * exactly match the interpreter result modulo timing of any extra book-
 * keeping. We assert full equality on the register file. */

#include "cpu_state.h"
#include "memory.h"
#include "sm83_interp.h"
#include "dispatcher.h"
#include <stdio.h>
#include <string.h>

static int failed = 0;

#define EQ(a, b, label) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "FAIL %s: got %llu expected %llu\n", \
                label, (unsigned long long)(a), (unsigned long long)(b)); \
        failed++; \
    } \
} while (0)

static void run_interp(const u8 *prog, size_t prog_len, cpu_state *out_cpu, mmu *out_mmu) {
    mmu_init(out_mmu);
    memcpy(out_mmu->rom + 0x0100, prog, prog_len);
    cpu_reset(out_cpu, out_mmu);
    int steps = 0;
    while (!out_cpu->halted && steps < 1000) { sm83_step(out_cpu); steps++; }
}

static void run_jit(const u8 *prog, size_t prog_len, cpu_state *out_cpu, mmu *out_mmu) {
    mmu_init(out_mmu);
    memcpy(out_mmu->rom + 0x0100, prog, prog_len);
    cpu_reset(out_cpu, out_mmu);

    gbjit_dispatcher d;
    if (!gbjit_dispatcher_init(&d, out_cpu)) {
        fprintf(stderr, "JIT init failed\n"); failed++; return;
    }
    /* Cap by cycles; pick a number > anything 50-step program needs. */
    gbjit_dispatcher_run_until(&d, 100000);
    gbjit_dispatcher_shutdown(&d);
}

static void check_match(const char *label, const cpu_state *a, const cpu_state *b) {
    EQ(a->a,  b->a,  label);
    EQ(a->f,  b->f,  label);
    EQ(a->b,  b->b,  label);
    EQ(a->c,  b->c,  label);
    EQ(a->d,  b->d,  label);
    EQ(a->e,  b->e,  label);
    EQ(a->h,  b->h,  label);
    EQ(a->l,  b->l,  label);
    EQ(a->sp, b->sp, label);
    EQ(a->pc, b->pc, label);
    EQ((u32)a->halted, (u32)b->halted, label);
    EQ(a->cycles, b->cycles, label);
}

int main(void) {
    /* Same smoke ROM the interp test uses, sans serial. */
    static const u8 prog[] = {
        0x3E, 0x42,             /* LD A,$42 */
        0x06, 0x03,             /* LD B,$03 */
        0x80,                   /* ADD A,B  -> A = $45, flags computed */
        0x21, 0x00, 0xC0,       /* LD HL,$C000 */
        0x77,                   /* LD (HL),A */
        0x36, 0x7E,             /* LD (HL),$7E */
        0x3E, 0x55,             /* LD A,$55 */
        0x76,                   /* HALT */
    };

    static cpu_state cpu_i, cpu_j;
    static mmu m_i, m_j;
    run_interp(prog, sizeof(prog), &cpu_i, &m_i);
    run_jit   (prog, sizeof(prog), &cpu_j, &m_j);

    check_match("smoke rom", &cpu_i, &cpu_j);

    /* Also verify memory contents match where it matters. */
    if (m_i.wram[0] != m_j.wram[0]) {
        fprintf(stderr, "FAIL wram[0]: interp=%02X jit=%02X\n", m_i.wram[0], m_j.wram[0]);
        failed++;
    }

    /* ALU-heavy ROM: exercises ADD/SUB/AND/OR/XOR/CP inlined paths and the
     * resulting F register. */
    static const u8 alu_prog[] = {
        0x3E, 0x42,             /* LD A,$42 */
        0x06, 0x13,             /* LD B,$13 */
        0x80,                   /* ADD A,B  -> A=$55, F=$00 */
        0x90,                   /* SUB B    -> A=$42, F=$40 */
        0xA0,                   /* AND B    -> A=$02, F=$20 */
        0xB0,                   /* OR  B    -> A=$13, F=$00 */
        0xA8,                   /* XOR B    -> A=$00, F=$80 */
        0xB8,                   /* CP  B    -> A unchanged, F=$70 */
        0x76,                   /* HALT */
    };
    static cpu_state cpu_i2, cpu_j2;
    static mmu m_i2, m_j2;
    run_interp(alu_prog, sizeof(alu_prog), &cpu_i2, &m_i2);
    run_jit   (alu_prog, sizeof(alu_prog), &cpu_j2, &m_j2);
    check_match("alu rom", &cpu_i2, &cpu_j2);

    /* Fibonacci ROM — exercises the inlined JR cc loop, INC/DEC flags, and
     * cross-iteration register state. After 8 iterations starting from
     * a=0,b=1, A=fib(8)=21 ($15), B=fib(7)=13 ($0D), C=0. */
    static const u8 fib_prog[] = {
        0x3E, 0x00,             /* LD A,$00 */
        0x06, 0x01,             /* LD B,$01 */
        0x0E, 0x08,             /* LD C,$08 */
        /* loop @ 0x0106: */
        0x67,                   /* LD H,A   (temp = a) */
        0x80,                   /* ADD A,B  (a = a + b) */
        0x44,                   /* LD B,H   (b = temp)        ; 0x44 = LD B,H */
        0x0D,                   /* DEC C */
        0x20, 0xFA,             /* JR NZ,-6  -> back to loop */
        0x76,                   /* HALT */
    };
    static cpu_state cpu_if, cpu_jf;
    static mmu m_if, m_jf;
    run_interp(fib_prog, sizeof(fib_prog), &cpu_if, &m_if);
    run_jit   (fib_prog, sizeof(fib_prog), &cpu_jf, &m_jf);
    check_match("fib rom", &cpu_if, &cpu_jf);
    if (cpu_jf.a != 0x15 || cpu_jf.b != 0x0D) {
        fprintf(stderr, "FAIL fib end: A=%02X B=%02X (expected $15, $0D)\n",
                cpu_jf.a, cpu_jf.b);
        failed++;
    }

    /* Loop ROM — multiple blocks, exercises the dispatcher's chain cache. */
    static const u8 loop_prog[] = {
        0x06, 0x00,             /* 0x0100  LD B,$00 */
        0x04,                   /* 0x0102  INC B */
        0x78,                   /* 0x0103  LD A,B */
        0xFE, 0x03,             /* 0x0104  CP $03 (helper — sets Z if B==3) */
        0x20, 0xFA,             /* 0x0106  JR NZ,-6 (back to 0x0102) */
        0x76,                   /* 0x0108  HALT */
    };
    static cpu_state cpu_il, cpu_jl;
    static mmu m_il, m_jl;
    run_interp(loop_prog, sizeof(loop_prog), &cpu_il, &m_il);
    run_jit   (loop_prog, sizeof(loop_prog), &cpu_jl, &m_jl);
    check_match("loop rom", &cpu_il, &cpu_jl);

    /* INC/DEC + 16-bit ops ROM. */
    static const u8 inc_prog[] = {
        0x01, 0xFF, 0x12,       /* LD BC,$12FF                 (sets B=$12, C=$FF) */
        0x03,                   /* INC BC  -> BC=$1300 (8 cyc, no flags) */
        0x11, 0x00, 0x80,       /* LD DE,$8000 */
        0x1B,                   /* DEC DE -> DE=$7FFF */
        0x21, 0x0F, 0x00,       /* LD HL,$000F */
        0x23,                   /* INC HL -> HL=$0010 */
        0x3E, 0xFF,             /* LD A,$FF (sets A=$FF, F=$B0 still from reset preserved) */
        0x3C,                   /* INC A  -> A=$00 (wraps), F = Z|H | (preserved C) */
        0x3D,                   /* DEC A  -> A=$FF, F = N|H | (preserved C) */
        0x06, 0x10,             /* LD B,$10 */
        0x05,                   /* DEC B -> B=$0F, F = N | H | (preserved C) */
        0x76,                   /* HALT */
    };
    static cpu_state cpu_i3, cpu_j3;
    static mmu m_i3, m_j3;
    run_interp(inc_prog, sizeof(inc_prog), &cpu_i3, &m_i3);
    run_jit   (inc_prog, sizeof(inc_prog), &cpu_j3, &m_j3);
    check_match("inc rom", &cpu_i3, &cpu_j3);

    if (failed) { fprintf(stderr, "%d differential check(s) failed\n", failed); return 1; }
    printf("jit differential: OK (smoke A=%02X | alu A=%02X F=%02X | inc HL=%04X DE=%04X)\n",
           cpu_j.a, cpu_j2.a, cpu_j2.f, cpu_j3.hl, cpu_j3.de);
    return 0;
}
