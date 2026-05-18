#include "ppu.h"
#include "cpu_state.h"
#include "memory.h"

/* PPU state machine — adapted from CrankBoy's peanut_gb (libs/peanut_gb_core.h,
 * MIT licensed; original work copyright Mahyar Koshkouei 2018-2022 and the
 * CrankBoy contributors). We reuse their mode/cycle constants, scanline
 * state machine, LCDC enable/disable handling, LY=LYC compare, and the
 * short-line-153 quirk, retargeted onto our `mmu`/`cpu_state` types. No
 * pixel rendering is performed — the goal is register-level fidelity for
 * games whose CPU half polls LY, STAT, or relies on STAT IRQ timing.
 *
 * Differences vs the original:
 *   - We don't model CGB, so the LCDC_CGB_MASTER_PRIORITY bit is ignored.
 *   - `ppu_tick` is called between dispatcher iterations rather than
 *     after every CPU instruction, so the state-advance loop processes
 *     however many cycles elapsed since the last call.
 *   - Mode-3 sprite/window penalty matches CrankBoy's "fast mode" — a
 *     fixed +24 cycle penalty assuming ~3 sprites/line; the dynamic
 *     per-sprite calculation would need OAM populated in a useful way,
 *     which a CPU-only emulator doesn't necessarily provide. */

/* Reg offsets (Pan Docs §"IO Registers" / Peanut-GB conventions). */
#define LCDC_REG         0x40
#define STAT_REG         0x41
#define SCY_REG          0x42
#define SCX_REG          0x43
#define LY_REG           0x44
#define LYC_REG          0x45
#define WY_REG           0x4A
#define WX_REG           0x4B
#define IF_REG           0x0F

/* STAT bits. */
#define STAT_LYC_INTR    0x40
#define STAT_MODE_2_INTR 0x20
#define STAT_MODE_1_INTR 0x10
#define STAT_MODE_0_INTR 0x08
#define STAT_LYC_COINC   0x04
#define STAT_MODE        0x03
#define STAT_USER_BITS   0xF8

/* LCDC bits. */
#define LCDC_ENABLE        0x80
#define LCDC_WINDOW_ENABLE 0x20
#define LCDC_OBJ_SIZE      0x04
#define LCDC_BG_ENABLE     0x01

/* Mode IDs (= STAT bits 1:0). */
#define LCD_HBLANK         0
#define LCD_VBLANK         1
#define LCD_SEARCH_OAM     2
#define LCD_TRANSFER       3

/* Cycle counts. */
#define LCD_LINE_CYCLES            456
#define LCD_VERT_LINES             154
#define LCD_FRAME_CYCLES           (LCD_LINE_CYCLES * LCD_VERT_LINES)
#define PPU_MODE_2_OAM_CYCLES      80
#define PPU_MODE_3_VRAM_MIN_CYCLES 172
#define PPU_MODE_3_VRAM_MAX_CYCLES 289
#define LCD_HEIGHT                 144

/* IF bits. */
#define INT_VBLANK_BIT 0x01
#define INT_LCDC_BIT   0x02

/* --- Internal helpers -------------------------------------------------- */

static inline void ppu_check_lyc(mmu *m) {
    /* Mirror peanut_gb's __gb_check_lyc: update STAT bit 2. */
    if (m->io[LY_REG] == m->io[LYC_REG])
        m->io[STAT_REG] |= STAT_LYC_COINC;
    else
        m->io[STAT_REG] &= (u8)~STAT_LYC_COINC;
}

static inline void ppu_update_stat_irq(mmu *m) {
    /* Mirror peanut_gb's __gb_update_stat_irq: level-triggered STAT line,
     * rising edge sets IF.LCDC. */
    if (!(m->io[LCDC_REG] & LCDC_ENABLE)) {
        m->ppu_stat_line = 0;
        return;
    }
    u8 stat = m->io[STAT_REG];
    bool line_is_high =
        ((stat & STAT_MODE_0_INTR) && m->ppu_lcd_mode == LCD_HBLANK)     ||
        ((stat & STAT_MODE_1_INTR) && m->ppu_lcd_mode == LCD_VBLANK)     ||
        ((stat & STAT_MODE_2_INTR) && m->ppu_lcd_mode == LCD_SEARCH_OAM) ||
        ((stat & STAT_LYC_INTR)    && (stat & STAT_LYC_COINC));
    if (!m->ppu_stat_line && line_is_high) {
        m->io[IF_REG] |= INT_LCDC_BIT;
    }
    m->ppu_stat_line = line_is_high ? 1 : 0;
}

/* Compute the mode-3 length on entry from mode 2. CrankBoy's "fast mode" —
 * fixed +24 cycle sprite penalty (assumes ~3 sprites/line average); plus
 * the SCX & 7 scroll alignment penalty; plus +6 if the window is visible
 * on this scanline. */
static inline u16 compute_mode3_cycles(mmu *m) {
    u16 cycles = PPU_MODE_3_VRAM_MIN_CYCLES;
    u8 scx_mod8 = m->io[SCX_REG] & 7u;
    cycles = (u16)(cycles + scx_mod8);
    bool win_visible = (m->io[LCDC_REG] & LCDC_WINDOW_ENABLE)
                    && (m->io[WX_REG] <= 166u)
                    && (m->io[LY_REG] >= m->ppu_latched_wy);
    if (m->io[LCDC_REG] & LCDC_BG_ENABLE) {
        /* DMG: window also gated on BG enable. */
        if (!win_visible) win_visible = false;
    }
    if (win_visible) cycles = (u16)(cycles + 6);
    cycles = (u16)(cycles + 24);   /* fixed sprite penalty (3 sprites avg) */
    if (cycles > PPU_MODE_3_VRAM_MAX_CYCLES) cycles = PPU_MODE_3_VRAM_MAX_CYCLES;
    return cycles;
}

/* Recompute the cycle at which the next state transition is due. The
 * dispatcher uses this to early-return from ppu_tick when nothing will
 * change. */
static inline void ppu_recompute_next_event(mmu *m, u64 base_cycles) {
    u32 remaining;
    if (!(m->io[LCDC_REG] & LCDC_ENABLE)) {
        remaining = (u32)(LCD_FRAME_CYCLES - m->ppu_lcd_off_count);
    } else {
        switch (m->ppu_lcd_mode) {
        case LCD_HBLANK:
            remaining = (u32)(m->ppu_mode0_cycles - m->ppu_lcd_count);
            break;
        case LCD_VBLANK:
            remaining = (u32)(LCD_LINE_CYCLES - m->ppu_lcd_count);
            break;
        case LCD_SEARCH_OAM:
            remaining = (u32)(PPU_MODE_2_OAM_CYCLES - m->ppu_lcd_count);
            break;
        case LCD_TRANSFER:
            remaining = (u32)(m->ppu_mode3_cycles - m->ppu_lcd_count);
            break;
        default:
            remaining = 1;
            break;
        }
    }
    if ((i32)remaining <= 0) remaining = 1;
    m->ppu_next_event_cycles = base_cycles + remaining;
}

/* Apply an LCDC enable/disable edge. Called from inside ppu_tick when
 * `last_lcdc` differs from the current value. Mirrors peanut_gb's
 * mmu_write8 case 0x40 branch. */
static void ppu_lcdc_edge(mmu *m, u8 prev, u8 curr) {
    bool was_on  = (prev & LCDC_ENABLE) != 0;
    bool is_on   = (curr & LCDC_ENABLE) != 0;
    if (was_on && !is_on) {
        m->ppu_lcd_off_count = 0;
        m->io[LY_REG] = 0;
        m->ppu_lcd_count = 0;
        m->ppu_lcd_mode = LCD_HBLANK;
        m->io[STAT_REG] &= (u8)~(STAT_MODE | STAT_LYC_COINC);
        m->ppu_stat_line = 0;
        ppu_check_lyc(m);
    } else if (!was_on && is_on) {
        m->ppu_lcd_count = 4;
        m->io[LY_REG] = 0;
        m->ppu_lcd_mode = LCD_SEARCH_OAM;
        m->io[STAT_REG] = (u8)((m->io[STAT_REG] & ~STAT_MODE) | LCD_SEARCH_OAM);
        m->ppu_stat_line = 0;
        ppu_check_lyc(m);
        ppu_update_stat_irq(m);
    }
}

/* --- Main entry -------------------------------------------------------- */

void ppu_tick(struct cpu_state *cpu) {
    if (!cpu || !cpu->mmu) return;
    mmu *m = cpu->mmu;

    /* Fast-path early-return — most dispatcher iterations span fewer
     * cycles than the next state transition. The deadline is reset
     * after every transition (or every LCDC edge). */
    if (cpu->cycles < m->ppu_next_event_cycles
            && m->io[LCDC_REG] == m->ppu_last_lcdc) {
        return;
    }

    /* LCDC enable/disable edge detection — games typically write LCDC
     * via mmu_write8 which the JIT routes through its helper, so by the
     * time we get here the byte in m->io[$40] is current. */
    u8 lcdc = m->io[LCDC_REG];
    if (lcdc != m->ppu_last_lcdc) {
        ppu_lcdc_edge(m, m->ppu_last_lcdc, lcdc);
        m->ppu_last_lcdc = lcdc;
    }

    /* Advance the PPU by however many cycles have passed since last call. */
    u32 delta = (u32)(cpu->cycles - m->ppu_last_cpu_cycles);
    m->ppu_last_cpu_cycles = cpu->cycles;

    if (!(lcdc & LCDC_ENABLE)) {
        m->ppu_lcd_off_count += delta;
        while (m->ppu_lcd_off_count >= LCD_FRAME_CYCLES)
            m->ppu_lcd_off_count -= LCD_FRAME_CYCLES;
        ppu_recompute_next_event(m, cpu->cycles);
        return;
    }

    m->ppu_lcd_count = (u16)(m->ppu_lcd_count + delta);

    /* Short-line-153 fix: when in VBlank on line 153, LY wraps to 0 a
     * few cycles into the line so LY=LYC=0 IRQs fire before frame end. */
    if (m->ppu_lcd_mode == LCD_VBLANK && m->io[LY_REG] == 153) {
        m->io[LY_REG] = 0;
        ppu_check_lyc(m);
        ppu_update_stat_irq(m);
    }

    /* State-machine loop. CrankBoy processes at most one transition per
     * call (their inst_cycles is small); ours can be a full block worth
     * of cycles (~200), so we loop to drain any number of transitions. */
    for (;;) {
        bool transitioned = false;
        switch (m->ppu_lcd_mode) {
        case LCD_SEARCH_OAM:
            if (m->ppu_lcd_count >= PPU_MODE_2_OAM_CYCLES) {
                m->ppu_lcd_count = (u16)(m->ppu_lcd_count - PPU_MODE_2_OAM_CYCLES);
                m->ppu_lcd_mode = LCD_TRANSFER;
                m->io[STAT_REG] = (u8)((m->io[STAT_REG] & ~STAT_MODE) | LCD_TRANSFER);
                m->ppu_mode3_cycles = compute_mode3_cycles(m);
                m->ppu_mode0_cycles = (u16)(LCD_LINE_CYCLES
                                          - PPU_MODE_2_OAM_CYCLES
                                          - m->ppu_mode3_cycles);
                ppu_update_stat_irq(m);
                transitioned = true;
            }
            break;
        case LCD_TRANSFER:
            if (m->ppu_lcd_count >= m->ppu_mode3_cycles) {
                m->ppu_lcd_count = (u16)(m->ppu_lcd_count - m->ppu_mode3_cycles);
                m->ppu_lcd_mode = LCD_HBLANK;
                m->io[STAT_REG] = (u8)((m->io[STAT_REG] & ~STAT_MODE) | LCD_HBLANK);
                ppu_update_stat_irq(m);
                transitioned = true;
            }
            break;
        case LCD_HBLANK:
            if (m->ppu_lcd_count >= m->ppu_mode0_cycles) {
                m->ppu_lcd_count = (u16)(m->ppu_lcd_count - m->ppu_mode0_cycles);
                m->io[LY_REG]++;
                if (m->io[LY_REG] == LCD_HEIGHT) {
                    m->ppu_lcd_mode = LCD_VBLANK;
                    m->io[STAT_REG] = (u8)((m->io[STAT_REG] & ~STAT_MODE) | LCD_VBLANK);
                    m->io[IF_REG] |= INT_VBLANK_BIT;
                    ppu_update_stat_irq(m);
                    ppu_check_lyc(m);
                    ppu_update_stat_irq(m);
                } else {
                    m->ppu_lcd_mode = LCD_SEARCH_OAM;
                    m->io[STAT_REG] = (u8)((m->io[STAT_REG] & ~STAT_MODE) | LCD_SEARCH_OAM);
                    ppu_check_lyc(m);
                    ppu_update_stat_irq(m);
                }
                transitioned = true;
            }
            break;
        case LCD_VBLANK:
            if (m->ppu_lcd_count >= LCD_LINE_CYCLES) {
                m->ppu_lcd_count = (u16)(m->ppu_lcd_count - LCD_LINE_CYCLES);
                if (m->io[LY_REG] == 0) {
                    /* End of VBlank — latch WY for the next frame. */
                    m->ppu_latched_wy = m->io[WY_REG];
                    m->ppu_lcd_mode = LCD_SEARCH_OAM;
                    m->io[STAT_REG] = (u8)((m->io[STAT_REG] & ~STAT_MODE) | LCD_SEARCH_OAM);
                    ppu_check_lyc(m);
                    ppu_update_stat_irq(m);
                } else {
                    m->io[LY_REG]++;
                    ppu_check_lyc(m);
                    ppu_update_stat_irq(m);
                }
                transitioned = true;
            }
            break;
        }
        if (!transitioned) break;
    }

    ppu_recompute_next_event(m, cpu->cycles);
}
