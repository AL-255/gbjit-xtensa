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

#endif
