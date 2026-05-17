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

    cpu_state cpu_i, cpu_j;
    mmu m_i, m_j;
    run_interp(prog, sizeof(prog), &cpu_i, &m_i);
    run_jit   (prog, sizeof(prog), &cpu_j, &m_j);

    check_match("smoke rom", &cpu_i, &cpu_j);

    /* Also verify memory contents match where it matters. */
    if (m_i.wram[0] != m_j.wram[0]) {
        fprintf(stderr, "FAIL wram[0]: interp=%02X jit=%02X\n", m_i.wram[0], m_j.wram[0]);
        failed++;
    }

    if (failed) { fprintf(stderr, "%d differential check(s) failed\n", failed); return 1; }
    printf("jit differential: OK  (PC=%04X A=%02X cycles=%llu)\n",
           cpu_j.pc, cpu_j.a, (unsigned long long)cpu_j.cycles);
    return 0;
}
