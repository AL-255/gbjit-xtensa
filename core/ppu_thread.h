/* Optional dual-core PPU runner — moves ppu_tick off the dispatcher
 * thread (Core 0) onto a FreeRTOS task pinned to Core 1.
 *
 * Enabled by defining `GBJIT_PPU_ASYNC` at build time (only on ESP-IDF
 * dual-core targets). When enabled:
 *   - core/sm83_interp.c skips its in-line `ppu_tick(cpu)` call —
 *     Core 1's task is responsible for advancing the PPU state machine.
 *   - core/ppu.c writes IF bits via __atomic_fetch_or so they coexist
 *     with Core 0 clearing IF on IRQ servicing.
 *   - core/sm83_interp.c clears IF bits via __atomic_fetch_and to avoid
 *     losing PPU-driven sets between read and write-back.
 *
 * The host build and any unicore target leave GBJIT_PPU_ASYNC undefined
 * and continue to tick the PPU synchronously from Core 0. */

#ifndef GBJIT_PPU_THREAD_H
#define GBJIT_PPU_THREAD_H

struct cpu_state;

/* Always callable from any TU. When GBJIT_PPU_ASYNC was not defined
 * during the build of ppu_thread.c, both functions are no-ops. */
void ppu_thread_start(struct cpu_state *cpu);
void ppu_thread_stop(void);

#endif
