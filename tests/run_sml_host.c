/* Long-run SML soak under the host JIT.
 *
 * Loads a ROM and runs it through the dispatcher for a large cycle
 * budget. Built Debug, the emit_xtensa.c bounds asserts are live — a
 * codecache budget overflow trips emit24()'s assert immediately
 * instead of silently corrupting memory (as it would in a release
 * board build). Used to hunt the "random crash on a long run" bug. */

#include "cpu_state.h"
#include "memory.h"
#include "dispatcher.h"
#include <stdio.h>
#include <stdlib.h>

static int read_file(const char *path, u8 **out, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (long)ROM_SIZE_MAX) { fclose(f); return -2; }
    u8 *buf = (u8 *)malloc((size_t)sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) { free(buf); fclose(f); return -3; }
    fclose(f);
    *out = buf; *len = (size_t)sz;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s rom.gb [Mcycles]\n", argv[0]); return 1; }
    u64 budget = (argc >= 3 ? (u64)strtoull(argv[2], NULL, 10) : 2000ull) * 1000000ull;

    u8 *rom = NULL; size_t rom_len;
    if (read_file(argv[1], &rom, &rom_len) != 0) { fprintf(stderr, "can't read rom\n"); return 2; }

    static mmu m;
    static cpu_state cpu;
    gb_mmu_init(&m);
    mmu_load_rom(&m, rom, rom_len);
    cpu_reset(&cpu, &m);

    gbjit_dispatcher d;
    if (!gbjit_dispatcher_init(&d, &cpu)) { fprintf(stderr, "jit init fail\n"); return 3; }
    d.prefetch_enabled = false;   /* match the board firmware */

    /* Run in 4M-cycle slices so progress is visible. */
    u64 step = 4ull * 1000000ull;
    for (u64 c = 0; c < budget; c += step) {
        gbjit_dispatcher_run_until(&d, cpu.cycles + step);
        if (cpu.stopped) { printf("STOPPED at cycle %llu\n", (unsigned long long)cpu.cycles); break; }
        if ((c % (200ull * 1000000ull)) == 0) {
            printf("  %4llu Mcyc  pc=%04X frame=%u blocks=%llu\n",
                   (unsigned long long)(cpu.cycles / 1000000ull), cpu.pc,
                   m.frame_seq, (unsigned long long)d.blocks_compiled);
            fflush(stdout);
        }
    }
    /* Workload profile. blocks_compiled counts *every* compile, so with
     * eviction it includes recompiles; blocks_executed counts every
     * block run. exec/compile is the amortisation factor — how many
     * times the average compiled block runs before it is dropped. A
     * value near 1 means the JIT pays its full compile cost per run. */
    u64 bc = d.blocks_compiled, be = d.blocks_executed;
    printf("done: cycles=%llu pc=%04X frames=%u\n"
           "  blocks_compiled=%llu blocks_executed=%llu exec/compile=%.1f\n"
           "  chain_hits=%llu chain_misses=%llu interp_steps=%llu\n",
           (unsigned long long)cpu.cycles, cpu.pc, m.frame_seq,
           (unsigned long long)bc, (unsigned long long)be,
           bc ? (double)be / (double)bc : 0.0,
           (unsigned long long)d.chain_hits,
           (unsigned long long)d.chain_misses,
           (unsigned long long)d.interp_steps);
    {
        extern u64 gbjit_cc_reserved, gbjit_cc_actual, gbjit_cc_n;
        if (gbjit_cc_n)
            printf("  codecache: %llu compiles, reserved=%llu actual=%llu "
                   "density=%.0f%% (avg reserved=%llu actual=%llu B/block)\n",
                   (unsigned long long)gbjit_cc_n,
                   (unsigned long long)gbjit_cc_reserved,
                   (unsigned long long)gbjit_cc_actual,
                   100.0 * (double)gbjit_cc_actual / (double)gbjit_cc_reserved,
                   (unsigned long long)(gbjit_cc_reserved / gbjit_cc_n),
                   (unsigned long long)(gbjit_cc_actual / gbjit_cc_n));
    }
    gbjit_dispatcher_shutdown(&d);
    free(rom);
    return 0;
}
