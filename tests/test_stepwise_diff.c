/* Step-by-step differential check.
 *
 * Runs the interpreter and the JIT (one block at a time) in lockstep over a
 * ROM. After each JIT block, snapshots the cpu_state, then re-runs the
 * interpreter the same number of cycles and compares. Reports the first
 * divergence and the GB PC where it happened. */

#include "cpu_state.h"
#include "memory.h"
#include "sm83_interp.h"
#include "dispatcher.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *prog_path;

static int read_file(const char *path, u8 **out, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (long)ROM_SIZE) { fclose(f); return -2; }
    u8 *buf = (u8*)malloc((size_t)sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) { free(buf); fclose(f); return -3; }
    fclose(f);
    *out = buf;
    *len = (size_t)sz;
    return 0;
}

static bool diff_state(const cpu_state *a, const cpu_state *b, int step) {
    if (a->a != b->a || a->f != b->f || a->b != b->b || a->c != b->c ||
        a->d != b->d || a->e != b->e || a->h != b->h || a->l != b->l ||
        a->sp != b->sp || a->pc != b->pc) {
        fprintf(stderr, "DIFF at step %d:\n", step);
        fprintf(stderr, "  interp: A=%02X F=%02X BC=%04X DE=%04X HL=%04X SP=%04X PC=%04X cyc=%llu\n",
                a->a, a->f, a->bc, a->de, a->hl, a->sp, a->pc, (unsigned long long)a->cycles);
        fprintf(stderr, "  jit:    A=%02X F=%02X BC=%04X DE=%04X HL=%04X SP=%04X PC=%04X cyc=%llu\n",
                b->a, b->f, b->bc, b->de, b->hl, b->sp, b->pc, (unsigned long long)b->cycles);
        return true;
    }
    return false;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s rom.gb [max_steps]\n", argv[0]); return 1; }
    prog_path = argv[1];
    int max_steps = (argc >= 3) ? atoi(argv[2]) : 1000;

    u8 *rom = NULL; size_t rom_len;
    if (read_file(prog_path, &rom, &rom_len) != 0) { fprintf(stderr, "can't read %s\n", prog_path); return 2; }

    static mmu m_i, m_j;
    static cpu_state cpu_i, cpu_j;
    mmu_init(&m_i); mmu_load_rom(&m_i, rom, rom_len); cpu_reset(&cpu_i, &m_i);
    mmu_init(&m_j); mmu_load_rom(&m_j, rom, rom_len); cpu_reset(&cpu_j, &m_j);

    gbjit_dispatcher d;
    if (!gbjit_dispatcher_init(&d, &cpu_j)) { fprintf(stderr, "jit init fail\n"); return 3; }

    /* Step the JIT one block at a time, then catch the interpreter up to
     * the same cycle count, and compare. */
    int step = 0;
    int trace_from = (argc >= 4) ? atoi(argv[3]) : -1;
    while (step < max_steps && !cpu_j.halted && !cpu_i.halted) {
        u64 before = cpu_j.cycles;
        u16 jit_pc_before = cpu_j.pc;
        u8  jit_op_before = mmu_read8(cpu_j.mmu, jit_pc_before);
        gbjit_dispatcher_run_until(&d, before + 1);  /* runs at least one block */
        u64 delta = cpu_j.cycles - before;

        /* Advance interpreter by `delta` cycles. */
        u64 target = cpu_i.cycles + delta;
        while (cpu_i.cycles < target) sm83_step(&cpu_i);

        if (trace_from >= 0 && step >= trace_from && step < trace_from + 8) {
            fprintf(stderr, "step %d (block at jit PC=%04X op=%02X delta=%llu):\n",
                    step, jit_pc_before, jit_op_before, (unsigned long long)delta);
            fprintf(stderr, "  interp: A=%02X F=%02X BC=%04X DE=%04X HL=%04X SP=%04X PC=%04X cyc=%llu\n",
                    cpu_i.a, cpu_i.f, cpu_i.bc, cpu_i.de, cpu_i.hl, cpu_i.sp, cpu_i.pc, (unsigned long long)cpu_i.cycles);
            fprintf(stderr, "  jit:    A=%02X F=%02X BC=%04X DE=%04X HL=%04X SP=%04X PC=%04X cyc=%llu\n",
                    cpu_j.a, cpu_j.f, cpu_j.bc, cpu_j.de, cpu_j.hl, cpu_j.sp, cpu_j.pc, (unsigned long long)cpu_j.cycles);
        }
        if (diff_state(&cpu_i, &cpu_j, step)) {
            fprintf(stderr, "stepwise: DIVERGED at block step %d\n", step);
            return 1;
        }
        step++;
    }
    fprintf(stderr, "stepwise: %d blocks in agreement (jit_halted=%d interp_halted=%d cycles=%llu)\n",
            step, cpu_j.halted, cpu_i.halted, (unsigned long long)cpu_j.cycles);
    gbjit_dispatcher_shutdown(&d);
    free(rom);
    return 0;
}
