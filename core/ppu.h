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

#endif
