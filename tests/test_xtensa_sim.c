/* Test the Xtensa LX7 simulator against hand-written instruction sequences.
 *
 * This is the safety net for the encoder + JIT pipeline: it executes the
 * actual bytes our encoder produces and checks the resulting register state. */

#include "emit_xtensa.h"
#include "xtensa_sim.h"
#include <stdio.h>
#include <string.h>

static int failed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s (line %d)\n", msg, __LINE__); failed++; } \
} while (0)

static u8 *translate_flat(xt_sim *s, u32 addr) {
    /* Address is an offset into a 64 KB scratch buffer pinned by user data. */
    u8 *base = (u8 *)s->user;
    if (addr >= 65536) return NULL;
    return base + addr;
}

static int test_movi_add(void) {
    u8 code[64]; xt_emit e; xt_init(&e, code, sizeof(code));
    /* a2 = 100; a3 = 23; a4 = a2 + a3; RET */
    xt_movi(&e, 2, 100);
    xt_movi(&e, 3, 23);
    xt_add(&e, 4, 2, 3);
    xt_ret(&e);

    xt_sim s; xt_sim_init(&s, code, e.len);
    s.a[0] = 0;
    xt_sim_run(&s, 100);
    CHECK(s.status == XT_SIM_RETURNED, "movi+add returned");
    CHECK(s.a[4] == 123, "100 + 23 == 123");
    return 0;
}

static int test_loads_stores(void) {
    u8 mem[64];
    memset(mem, 0xCC, sizeof(mem));
    mem[0] = 0x55;

    u8 code[64]; xt_emit e; xt_init(&e, code, sizeof(code));
    /* a11 = 0 (mem base via translate). a3 = M8[a11+0]; M8[a11+1] = a3; RET */
    xt_movi(&e, 11, 0);
    xt_l8ui(&e, 3, 11, 0);   /* a3 = 0x55 */
    xt_s8i (&e, 3, 11, 1);   /* mem[1] = 0x55 */
    xt_ret(&e);

    xt_sim s; xt_sim_init(&s, code, e.len);
    s.translate = translate_flat;
    s.user = mem;
    s.a[0] = 0;
    xt_sim_run(&s, 100);
    CHECK(s.status == XT_SIM_RETURNED, "loads_stores returned");
    CHECK(s.a[3] == 0x55, "loaded 0x55");
    CHECK(mem[1] == 0x55, "stored 0x55 to mem[1]");
    return 0;
}

static int test_branches(void) {
    u8 code[64]; xt_emit e; xt_init(&e, code, sizeof(code));
    /* a2 = 0; BNEZ a2,+6 (skip); a3 = 1; J +3 (skip next); a3 = 2; RET */
    xt_movi(&e, 2, 0);              /* +3 */
    /* offset to "a3 = 2" segment from start of bnez = bnez_pc + 4 + off */
    /* We'll patch a forward jump: bnez taken when a2 != 0 (won't be taken). */
    xt_bnez(&e, 2, 6);              /* +3 */
    xt_movi(&e, 3, 1);              /* +3 (executed) */
    xt_j(&e, 6);                    /* +3 (jumps past the next 3 bytes) */
    xt_movi(&e, 3, 2);              /* +3 (skipped) */
    xt_ret(&e);                     /* +3 */

    xt_sim s; xt_sim_init(&s, code, e.len);
    s.a[0] = 0;
    xt_sim_run(&s, 100);
    CHECK(s.status == XT_SIM_RETURNED, "branches returned");
    CHECK(s.a[3] == 1, "fell through to a3=1, jumped over a3=2");

    /* Same with a2 = 7 -> BNEZ taken, should land on a3=2 (at offset 12).
       BNEZ is at offset 3, target = 12, so rel = 9. */
    xt_init(&e, code, sizeof(code));
    xt_movi(&e, 2, 7);              /* 0 */
    xt_bnez(&e, 2, 9);              /* 3 -> jumps to 12 */
    xt_movi(&e, 3, 1);              /* 6 */
    xt_j(&e, 6);                    /* 9 -> jumps to 15 */
    xt_movi(&e, 3, 2);              /* 12 */
    xt_ret(&e);                     /* 15 */

    xt_sim_init(&s, code, e.len);
    s.a[0] = 0;
    xt_sim_run(&s, 100);
    CHECK(s.status == XT_SIM_RETURNED, "branches taken returned");
    CHECK(s.a[3] == 2, "BNEZ taken landed on a3=2");
    return 0;
}

static int test_shifts_extui(void) {
    u8 code[64]; xt_emit e; xt_init(&e, code, sizeof(code));
    /* a3 = 0xABCD; a4 = a3 >> 8; a5 = extract byte 0; a6 = extract byte 1; RET */
    xt_movi(&e, 3, 0x7CD);   /* 0x7CD; we'll OR upper byte in */
    /* For 0xABCD we need a value > 12-bit MOVI range. Use SLLI to build. */
    /* a3 = 0xAB; a3 = a3 << 8 -> 0xAB00; a4 = 0xCD; a3 = a3 | a4 = 0xABCD */
    xt_init(&e, code, sizeof(code));
    xt_movi(&e, 3, 0xAB);
    xt_slli(&e, 3, 3, 8);
    xt_movi(&e, 4, 0xCD);
    xt_or  (&e, 3, 3, 4);
    /* a5 = a3 & 0xFF (extract byte 0) — emit as EXTUI a5, a3, 0, 7 */
    xt_extui(&e, 5, 3, 0, 7);
    /* a6 = (a3 >> 8) & 0xFF — emit as EXTUI a6, a3, 8, 7 */
    xt_extui(&e, 6, 3, 8, 7);
    xt_ret(&e);

    xt_sim s; xt_sim_init(&s, code, e.len);
    s.a[0] = 0;
    xt_sim_run(&s, 100);
    CHECK(s.status == XT_SIM_RETURNED, "shifts returned");
    CHECK(s.a[3] == 0xABCD, "built 0xABCD via SLLI+OR");
    CHECK(s.a[5] == 0xCD, "EXTUI byte 0 = 0xCD");
    CHECK(s.a[6] == 0xAB, "EXTUI byte 1 = 0xAB");
    return 0;
}

int main(void) {
    test_movi_add();
    test_loads_stores();
    test_branches();
    test_shifts_extui();

    if (failed) { fprintf(stderr, "%d xtensa_sim check(s) failed\n", failed); return 1; }
    printf("xtensa_sim: OK\n");
    return 0;
}
