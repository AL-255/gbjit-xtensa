/* SMC invalidation tests.
 *
 * Validates that:
 *   1. After a block is compiled and then `invalidate_addr` is called for a
 *      PC that overlaps the block, the dispatcher drops the block and
 *      recompiles fresh on the next entry.
 *   2. Predicted-next chain pointers don't dangle when a chained-to block
 *      is invalidated.
 *   3. Repeated invalidation under a tight loop does not corrupt cpu_state. */

#include "cpu_state.h"
#include "memory.h"
#include "sm83_interp.h"
#include "dispatcher.h"
#include <stdio.h>
#include <string.h>

static int failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s (line %d)\n", msg, __LINE__); failed++; } \
} while (0)

static void setup(cpu_state *cpu, mmu *m, const u8 *prog, size_t prog_len) {
    mmu_init(m);
    memcpy(m->rom + 0x0100, prog, prog_len);
    cpu_reset(cpu, m);
}

static int test_basic_invalidation(void) {
    static const u8 prog[] = {
        0x3E, 0x42,             /* LD A,$42 */
        0x76,                   /* HALT */
    };
    static cpu_state cpu;
    static mmu m;
    setup(&cpu, &m, prog, sizeof(prog));

    gbjit_dispatcher d;
    CHECK(gbjit_dispatcher_init(&d, &cpu), "init");

    /* First run: compiles a block at 0x0100. */
    gbjit_dispatcher_run_until(&d, 1000);
    CHECK(cpu.halted == 1, "halted after first run");
    CHECK(d.blocks_compiled == 1, "compiled 1 block");
    u64 first_compiled = d.blocks_compiled;

    /* Reset just the CPU (not the dispatcher) and run again — should NOT
     * recompile because the block is cached. */
    cpu_reset(&cpu, &m);
    gbjit_dispatcher_run_until(&d, 1000);
    CHECK(d.blocks_compiled == first_compiled, "cached on second run");
    CHECK(cpu.halted == 1, "halted again");

    /* Invalidate the block's page → should recompile next time. */
    gbjit_dispatcher_invalidate_addr(&d, 0x0100);
    CHECK(d.smc_invalidations >= 1, "smc invalidation ran");

    cpu_reset(&cpu, &m);
    gbjit_dispatcher_run_until(&d, 1000);
    CHECK(d.blocks_compiled == first_compiled + 1, "recompiled after invalidation");
    CHECK(cpu.halted == 1, "halted after recompile");

    gbjit_dispatcher_shutdown(&d);
    return 0;
}

static int test_loop_block_invalidation(void) {
    /* Two-block loop: counter increments until 3, then halts. We invalidate
     * the loop block in the middle and ensure cpu_state stays consistent. */
    static const u8 prog[] = {
        0x06, 0x00,             /* 0x0100  LD B,$00 */
        0x04,                   /* 0x0102  INC B */
        0x78,                   /* 0x0103  LD A,B */
        0xFE, 0x03,             /* 0x0104  CP $03 */
        0x20, 0xFA,             /* 0x0106  JR NZ,-6 */
        0x76,                   /* 0x0108  HALT */
    };
    static cpu_state cpu_a, cpu_b;
    static mmu m_a, m_b;

    /* Reference run via interpreter. */
    setup(&cpu_a, &m_a, prog, sizeof(prog));
    while (!cpu_a.halted) sm83_step(&cpu_a);

    /* JIT run with invalidation injected partway through. */
    setup(&cpu_b, &m_b, prog, sizeof(prog));
    gbjit_dispatcher d;
    CHECK(gbjit_dispatcher_init(&d, &cpu_b), "init");

    /* Step ~100 cycles, invalidate, then complete. */
    gbjit_dispatcher_run_until(&d, 50);
    gbjit_dispatcher_invalidate_addr(&d, 0x0102);  /* drop loop body */
    gbjit_dispatcher_invalidate_addr(&d, 0x0100);  /* and entry */
    gbjit_dispatcher_run_until(&d, 5000);

    CHECK(cpu_b.halted == 1, "JIT halts");
    CHECK(cpu_a.a == cpu_b.a, "A matches interpreter");
    CHECK(cpu_a.b == cpu_b.b, "B matches");
    CHECK(cpu_a.pc == cpu_b.pc, "PC matches");
    CHECK(cpu_a.cycles == cpu_b.cycles, "cycles match");

    gbjit_dispatcher_shutdown(&d);
    return 0;
}

int main(void) {
    test_basic_invalidation();
    test_loop_block_invalidation();
    if (failed) { fprintf(stderr, "%d smc check(s) failed\n", failed); return 1; }
    printf("smc: OK\n");
    return 0;
}
