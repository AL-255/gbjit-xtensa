/* CALL0 → CALL8 trampolines.
 *
 * The JIT block runs as a CALL0-ABI function. The helpers it calls
 * (sm83_step, mmu_read8, mmu_write8) are windowed-ABI C functions. These
 * shims bridge the gap.
 *
 * Crucially, these shims do NOT modify a1. Modifying a1 from a CALL0
 * function executing inside a windowed parent's stack frame would shift
 * the parent's spill-target (a1+0..15) and corrupt either the parent's
 * own save area or unrelated memory. Caller's a0 lives in the safe
 * a0..a7 range of the current window — preserved across CALL8 — so we
 * just stash it implicitly by doing nothing.
 *
 * Caller's a0 is preserved across CALL8 because it's in phys (WB+0..WB+7).
 * Caller's a8..a15 are clobbered. Args go into a10..a12 which become the
 * callee's a2..a4 after the CALL8 window rotation.                          */

    .text

    .global sm83_step_call0
    .type   sm83_step_call0, @function
sm83_step_call0:
    mov     a10, a2                 /* cpu → callee a2 */
    call8   sm83_step
    jx      a0
    .size   sm83_step_call0, .-sm83_step_call0

    /* Op-fallback variant: no leading interrupt service in the callee, so the
     * deep service->ppu_tick->ppu_advance->ppu_draw_line chain never runs in
     * the JIT block's borrowed register window. */
    .global sm83_step_noirq_call0
    .type   sm83_step_noirq_call0, @function
sm83_step_noirq_call0:
    mov     a10, a2                 /* cpu → callee a2 */
    call8   sm83_step_noirq
    jx      a0
    .size   sm83_step_noirq_call0, .-sm83_step_noirq_call0

    .global mmu_read8_call0
    .type   mmu_read8_call0, @function
mmu_read8_call0:
    mov     a10, a2                 /* mmu  */
    mov     a11, a3                 /* addr */
    call8   mmu_read8
    mov     a2,  a10                /* return value (callee's a2 = caller's a10) */
    jx      a0
    .size   mmu_read8_call0, .-mmu_read8_call0

    .global mmu_write8_call0
    .type   mmu_write8_call0, @function
mmu_write8_call0:
    mov     a10, a2                 /* mmu  */
    mov     a11, a3                 /* addr */
    mov     a12, a4                 /* val  */
    call8   mmu_write8
    jx      a0
    .size   mmu_write8_call0, .-mmu_write8_call0
