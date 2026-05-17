#include "ppu.h"
#include "cpu_state.h"
#include "memory.h"

/* Timing constants — Pan Docs §"Rendering / PPU modes":
 *   • each scanline is 456 dots (≡ T-cycles at 1 dot/T-cycle on DMG)
 *   • a frame is 154 scanlines (144 visible + 10 VBlank), 70 224 dots total
 *   • intra-scanline modes (for visible lines 0..143):
 *       mode 2 (OAM scan)    dots 0..79     (80 dots)
 *       mode 3 (Pixel draw)  dots 80..251   (172 dots, simplified — real
 *                                            hardware varies 168..289)
 *       mode 0 (HBlank)      dots 252..455  (204 dots)
 *   • VBlank (lines 144..153) is mode 1 the whole way.
 *
 * STAT IRQ sources (Pan Docs §"LCD Status Register"):
 *   bit 3  HBlank (mode 0) interrupt
 *   bit 4  VBlank (mode 1) interrupt
 *   bit 5  OAM    (mode 2) interrupt
 *   bit 6  LY==LYC coincidence interrupt
 * All STAT sources OR into IF bit 1.
 */
#define DOTS_PER_LINE     456u
#define LINES_PER_FRAME   154u
#define DOTS_PER_FRAME    (DOTS_PER_LINE * LINES_PER_FRAME)  /* 70224 */
#define VISIBLE_LINES     144u
#define MODE2_DOTS        80u
#define MODE3_DOTS        172u
#define MODE3_END         (MODE2_DOTS + MODE3_DOTS)          /* 252 */

void ppu_tick(struct cpu_state *cpu) {
    if (!cpu || !cpu->mmu) return;
    mmu *m = cpu->mmu;

    /* Compute current LY and intra-scanline dot position. */
    u32 frame_dots = (u32)(cpu->cycles % DOTS_PER_FRAME);
    u8  ly         = (u8)(frame_dots / DOTS_PER_LINE);
    u32 line_dot   = frame_dots % DOTS_PER_LINE;

    /* Pick mode. Bit 7 of LCDC enables the LCD; if it's off, real hardware
     * reports mode 0 and LY=0. We respect the LCDC-off case so games that
     * cleanly disable the LCD during VRAM uploads don't see spurious VBlank
     * IRQs while it's off. */
    u8 lcdc = m->io[0x40];
    u8 mode;
    if (!(lcdc & 0x80u)) {
        ly   = 0;
        mode = 0;
    } else if (ly >= VISIBLE_LINES) {
        mode = 1;
    } else if (line_dot < MODE2_DOTS) {
        mode = 2;
    } else if (line_dot < MODE3_END) {
        mode = 3;
    } else {
        mode = 0;
    }

    /* Write LY and STAT back to IO. STAT bit 2 is the LY==LYC coincidence
     * flag, set whenever the current LY equals LYC; the upper STAT bits are
     * source-enable flags written by the game and must be preserved. */
    u8 lyc = m->io[0x45];
    u8 stat = (u8)((m->io[0x41] & 0xF8u) | (mode & 0x03u) | ((ly == lyc) ? 0x04u : 0x00u));
    m->io[0x41] = stat;
    m->io[0x44] = ly;

    /* Edge-detect transitions vs. the previous tick to fire IRQs exactly
     * once per event. */
    u8 prev_ly   = m->ppu_last_ly;
    u8 prev_mode = m->ppu_last_mode;

    /* VBlank IRQ: fires on the rising edge into line 144. */
    if (lcdc & 0x80u) {
        bool enter_line = (prev_ly != ly);
        if (enter_line && ly == VISIBLE_LINES) {
            m->io[0x0F] |= INT_VBLANK;
        }
        /* STAT IRQs — one per mode-change transition, and one per LY==LYC
         * rising edge. We OR into IF; real hardware uses a level-triggered
         * STAT line, but for unblocking polling loops the edge model is
         * sufficient. */
        if (prev_mode != mode) {
            if (mode == 0 && (stat & 0x08u)) m->io[0x0F] |= INT_LCD;
            if (mode == 1 && (stat & 0x10u)) m->io[0x0F] |= INT_LCD;
            if (mode == 2 && (stat & 0x20u)) m->io[0x0F] |= INT_LCD;
        }
        if (enter_line && ly == lyc && (stat & 0x40u)) {
            m->io[0x0F] |= INT_LCD;
        }
    }

    m->ppu_last_ly   = ly;
    m->ppu_last_mode = mode;
}
