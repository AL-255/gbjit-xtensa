#ifndef PPU_H
#define PPU_H

/* Minimal PPU timing model. No pixel rendering — just the scanline counter
 * (LY at $FF44), the STAT mode bits (low two bits of $FF41), and the VBlank /
 * STAT interrupt requests in IF ($FF0F). Enough behaviour for the CPU half of
 * commercial games to keep advancing past LY/STAT polling loops and to
 * receive their VBlank IRQs.
 *
 * Modelled after the public scanline-timing description in
 *   - Pan Docs §"Rendering"            (https://gbdev.io/pandocs/Rendering.html)
 *   - Imran Nazar's tutorial part 5/6  ("Gameboy Emulation in JavaScript: GPU
 *     Timings"), which is the cleanest minimal reference.
 * Same constants and same scanline-state machine as e.g. SameBoy/Gearboy/
 * Pyboy use; this file simply distils the timing skeleton without any of the
 * rendering code those projects also carry. No code is copied — only the
 * publicly documented timing arithmetic.
 */

#include "gb_types.h"

struct cpu_state;

/* Advance the PPU model up to cpu->cycles. Idempotent and cheap — safe to
 * call before every CPU step or block dispatch. Writes the current LY into
 * io[$44] and the current mode bits into io[$41], and OR-sets IF bits in
 * io[$0F] for any VBlank / STAT-source interrupts whose conditions just
 * became true since the previous call. */
void ppu_tick(struct cpu_state *cpu);

/* Force-advance the PPU model up to cpu->cycles, bypassing the fast-path
 * early return. Call this immediately before mutating a PPU/timer IO
 * register that takes effect at cpu->cycles — without it, the next
 * ppu_tick would compute its delta from an older ppu_last_cpu_cycles and
 * mis-attribute the pre-write cycles to the post-write state (e.g. the
 * cycles between a stale last_cyc and an LCDC ON-edge get re-processed in
 * the new ON state, drifting LY/STAT versus the reference interpreter).
 * Also called before inlined PPU/timer IO reads in the JIT so the io[]
 * byte reflects the state at cpu->cycles. */
void ppu_flush(struct cpu_state *cpu);

/* Re-evaluate LY=LYC coincidence + the STAT IRQ line after a CPU write to
 * LYC ($FF45). Must be called after ppu_flush has brought the PPU up to
 * cpu->cycles. Fixes dropped STAT/LYC interrupts when a raster handler
 * repoints LYC at the next scanline (e.g. dmg-acid2). */
void ppu_sync_lyc(struct cpu_state *cpu);

/* PPU render offload (see core/ppu.c). When enabled, the per-pixel render
 * of the 144 scanlines is deferred out of the synchronous PPU state machine
 * so it can be batched (and later moved to the second core). Each scanline's
 * registers are logged at its mode3→0; VRAM/OAM are flushed lazily — the MMU
 * calls ppu_offload_flush() just before any mid-frame VRAM/OAM write so the
 * captured lines render against the VRAM/OAM they had at capture time. */
/* GBJIT_PPU_OFFLOAD_THREAD moves the deferred render onto a second core.
 * It implies GBJIT_PPU_OFFLOAD. */
#ifndef GBJIT_PPU_OFFLOAD_THREAD
#define GBJIT_PPU_OFFLOAD_THREAD 0
#endif
#ifndef GBJIT_PPU_OFFLOAD
#  if GBJIT_PPU_OFFLOAD_THREAD
#    define GBJIT_PPU_OFFLOAD 1
#  else
#    define GBJIT_PPU_OFFLOAD 0
#  endif
#endif

#if GBJIT_PPU_OFFLOAD
struct mmu;
/* Nonzero while captured-but-undrawn scanlines exist; the MMU checks this
 * inline and only calls ppu_offload_flush() when a flush is actually due. */
extern int gbjit_ppu_have_pending;
void ppu_offload_flush(struct mmu *m);
#endif

/* Set nonzero by gbjit_run_frame() while a frame is emulated only to catch up
 * timing (it won't be displayed). ppu_draw_line then skips the ~11 ms per-pixel
 * scanline render while keeping the PPU state machine and window_line exact.
 * Cleared for every displayed frame. */
extern int gbjit_ppu_skip_pixels;

/* Render-thread lifecycle + the emul-core sync point. When offload threading
 * is off these are no-ops, so callers need no #ifdefs. ppu_offload_render_wait
 * MUST be called by the emulation core before it reads the framebuffer or runs
 * another frame, so it never races the render thread. */
#if GBJIT_PPU_OFFLOAD_THREAD
void ppu_offload_thread_start(void);
void ppu_offload_thread_stop(void);
void ppu_offload_render_wait(void);
#else
static inline void ppu_offload_thread_start(void) {}
static inline void ppu_offload_thread_stop(void) {}
static inline void ppu_offload_render_wait(void) {}
#endif

#endif
