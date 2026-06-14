#ifndef SM83_INTERP_H
#define SM83_INTERP_H

#include "cpu_state.h"

/* Execute exactly one instruction. Returns the number of T-cycles consumed
   (4, 8, 12, 16, 20, or 24). Updates cpu->cycles in addition. */
u32 sm83_step(cpu_state *cpu);

/* Op-fallback variant for the native JIT helper path: executes one opcode
   WITHOUT the leading interrupt service (the dispatcher services interrupts at
   block boundaries). Avoids dragging the deep service->ppu call chain into the
   JIT block's borrowed Xtensa register window. */
u32 sm83_step_noirq(cpu_state *cpu);

/* Run until cpu->cycles reaches `until`. Returns cycles actually executed. */
u64 sm83_run_until(cpu_state *cpu, u64 until);

/* Service pending interrupts. Called by sm83_step before opcode fetch.
   Returns nonzero if an interrupt was serviced (and consumed 20 cycles). */
u32 sm83_service_interrupts(cpu_state *cpu);

#endif
