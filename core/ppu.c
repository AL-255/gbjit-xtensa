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
#define LCDC_ENABLE              0x80
#define LCDC_WINDOW_TILEMAP_HI   0x40
#define LCDC_WINDOW_ENABLE       0x20
#define LCDC_BG_TILEDATA_LO      0x10
#define LCDC_BG_TILEMAP_HI       0x08
#define LCDC_OBJ_SIZE            0x04
#define LCDC_OBJ_ENABLE          0x02
#define LCDC_BG_ENABLE           0x01

#define BGP_REG                  0x47
#define OBP0_REG                 0x48
#define OBP1_REG                 0x49

/* OAM attribute bits. */
#define OAM_BG_PRIO              0x80
#define OAM_FLIP_Y               0x40
#define OAM_FLIP_X               0x20
#define OAM_PAL_1                0x10  /* DMG palette select (0=OBP0, 1=OBP1) */

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
#ifdef GBJIT_PPU_ASYNC
        __atomic_fetch_or(&m->io[IF_REG], INT_LCDC_BIT, __ATOMIC_RELAXED);
#else
        m->io[IF_REG] |= INT_LCDC_BIT;
#endif
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

/* --- Scanline renderer ------------------------------------------------- */

/* Fetch a pixel (0..3 color index, pre-palette) from a tile at the
 * supplied VRAM-relative tile-data address. `row` is 0..7. `col` is 0..7
 * counted left-to-right (bit 7 of byte is leftmost pixel on DMG). */
static inline u8 fetch_tile_pixel(const u8 *vram, u16 tile_addr_vram_rel,
                                  u8 row, u8 col) {
    u16 line_addr = (u16)(tile_addr_vram_rel + row * 2);
    u8 b0 = vram[line_addr];
    u8 b1 = vram[line_addr + 1];
    u8 bit = (u8)(7 - col);
    return (u8)(((b1 >> bit) & 1) << 1 | ((b0 >> bit) & 1));
}

/* Render scanline `ly` into m->framebuffer. Called at LCD_TRANSFER →
 * LCD_HBLANK; reads OAM, VRAM, BGP/OBP0/OBP1, LCDC, SCY/SCX, WY/WX
 * which must reflect the final state for this line. */
static void ppu_draw_line(mmu *m, u8 ly) {
    u8 *line = &m->framebuffer[(u32)ly * 160];
    u8 lcdc = m->io[LCDC_REG];

    /* DMG: if BG_ENABLE is clear, BG (and window) render as color 0. We
     * fill the line with shade-0 (BGP[0]) up front so sprites can still
     * draw over it. */
    u8 bgp = m->io[BGP_REG];
    u8 bg_shade[4] = {
        (u8)(bgp & 3), (u8)((bgp >> 2) & 3),
        (u8)((bgp >> 4) & 3), (u8)((bgp >> 6) & 3),
    };
    u8 bg_color_id[160];                /* raw 0..3 before palette, for sprite/bg priority */

    if (lcdc & LCDC_BG_ENABLE) {
        u8 scx = m->io[SCX_REG], scy = m->io[SCY_REG];
        u16 tilemap_base = (lcdc & LCDC_BG_TILEMAP_HI) ? 0x1C00 : 0x1800;
        bool unsigned_tiles = (lcdc & LCDC_BG_TILEDATA_LO) != 0;
        u8 bg_y = (u8)(ly + scy);
        u8 tile_row = (u8)(bg_y >> 3);
        u8 pixel_row = (u8)(bg_y & 7);
        for (int x = 0; x < 160; x++) {
            u8 bg_x = (u8)(x + scx);
            u8 tile_col = (u8)(bg_x >> 3);
            u8 pixel_col = (u8)(bg_x & 7);
            u8 tile_id = m->vram[tilemap_base + tile_row * 32 + tile_col];
            u16 tile_addr;
            if (unsigned_tiles) {
                tile_addr = (u16)(tile_id * 16);                /* base $8000 */
            } else {
                tile_addr = (u16)(0x1000 + (i8)tile_id * 16);   /* base $9000 */
            }
            u8 cid = fetch_tile_pixel(m->vram, tile_addr, pixel_row, pixel_col);
            bg_color_id[x] = cid;
            line[x] = bg_shade[cid];
        }
    } else {
        for (int x = 0; x < 160; x++) {
            bg_color_id[x] = 0;
            line[x] = bg_shade[0];
        }
    }

    /* Window — same tile-data base as BG (LCDC bit 4), distinct tile-
     * map (LCDC bit 6). DMG: window only renders if BG_ENABLE is set
     * too (yes, the BG_ENABLE bit gates the window in DMG mode). */
    if ((lcdc & LCDC_WINDOW_ENABLE) && (lcdc & LCDC_BG_ENABLE)
            && ly >= m->ppu_latched_wy) {
        u8 wx = m->io[WX_REG];
        if (wx < 167) {
            int start_x = (wx >= 7) ? (wx - 7) : 0;
            u16 tilemap_base = (lcdc & LCDC_WINDOW_TILEMAP_HI) ? 0x1C00 : 0x1800;
            bool unsigned_tiles = (lcdc & LCDC_BG_TILEDATA_LO) != 0;
            u8 wy_internal = m->window_line;
            u8 tile_row = (u8)(wy_internal >> 3);
            u8 pixel_row = (u8)(wy_internal & 7);
            for (int x = start_x; x < 160; x++) {
                int win_x = x - (int)wx + 7;
                if (win_x < 0) continue;
                u8 tile_col = (u8)((win_x >> 3) & 0x1F);
                u8 pixel_col = (u8)(win_x & 7);
                u8 tile_id = m->vram[tilemap_base + tile_row * 32 + tile_col];
                u16 tile_addr;
                if (unsigned_tiles) {
                    tile_addr = (u16)(tile_id * 16);
                } else {
                    tile_addr = (u16)(0x1000 + (i8)tile_id * 16);
                }
                u8 cid = fetch_tile_pixel(m->vram, tile_addr, pixel_row, pixel_col);
                bg_color_id[x] = cid;
                line[x] = bg_shade[cid];
            }
            m->window_line++;
        }
    }

    /* Sprites — OAM walk, pick up to 10 visible on this scanline by
     * OAM index order, then render in OAM-index order with later (lower
     * OAM index) sprites overwriting earlier ones at equal X, except
     * lower X wins. DMG: stable-sort by X ascending; render in reverse
     * so lowest-X is on top. Color 0 of the sprite palette is always
     * transparent. */
    if (lcdc & LCDC_OBJ_ENABLE) {
        u8 obj_h = (lcdc & LCDC_OBJ_SIZE) ? 16 : 8;
        typedef struct { u8 y, x, tile, attr, oam_idx; } visible_obj;
        visible_obj vis[10];
        int nvis = 0;
        for (int i = 0; i < 40 && nvis < 10; i++) {
            u8 sy = m->oam[i * 4 + 0];
            int top = (int)sy - 16;
            if ((int)ly < top || (int)ly >= top + obj_h) continue;
            vis[nvis].y    = sy;
            vis[nvis].x    = m->oam[i * 4 + 1];
            vis[nvis].tile = m->oam[i * 4 + 2];
            vis[nvis].attr = m->oam[i * 4 + 3];
            vis[nvis].oam_idx = (u8)i;
            nvis++;
        }
        /* Sort by X ascending, stable on OAM index. Insertion sort,
         * nvis <= 10 so cheap. */
        for (int i = 1; i < nvis; i++) {
            for (int j = i; j > 0 && vis[j].x < vis[j-1].x; j--) {
                visible_obj t = vis[j];
                vis[j] = vis[j-1]; vis[j-1] = t;
            }
        }
        /* Render in reverse (lower X / earlier OAM wins). */
        u8 obp0 = m->io[OBP0_REG], obp1 = m->io[OBP1_REG];
        for (int s = nvis - 1; s >= 0; s--) {
            int top = (int)vis[s].y - 16;
            int left = (int)vis[s].x - 8;
            int row_in_sprite = (int)ly - top;
            if (vis[s].attr & OAM_FLIP_Y) row_in_sprite = (obj_h - 1) - row_in_sprite;
            u8 tile = vis[s].tile;
            if (obj_h == 16) tile &= 0xFE;            /* low bit cleared in 8x16 mode */
            u16 tile_addr = (u16)(tile * 16);
            u8 pal = (vis[s].attr & OAM_PAL_1) ? obp1 : obp0;
            u8 pal_shade[4] = {
                0, (u8)((pal >> 2) & 3), (u8)((pal >> 4) & 3), (u8)((pal >> 6) & 3),
            };
            for (int px = 0; px < 8; px++) {
                int x = left + px;
                if (x < 0 || x >= 160) continue;
                u8 col_in_sprite = (vis[s].attr & OAM_FLIP_X) ? (u8)(7 - px) : (u8)px;
                u8 cid = fetch_tile_pixel(m->vram, tile_addr,
                                          (u8)row_in_sprite, col_in_sprite);
                if (cid == 0) continue;                    /* sprite color 0 = transparent */
                if ((vis[s].attr & OAM_BG_PRIO) && bg_color_id[x] != 0) continue;
                line[x] = pal_shade[cid];
            }
        }
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
                /* Scanline is now stable — push pixels into the
                 * framebuffer. Cheap to skip on lines where there's no
                 * downstream consumer (frame_seq goes unread); we'd
                 * still pay for the LY/STAT machinery either way. */
                if (m->io[LY_REG] < LCD_HEIGHT) {
                    ppu_draw_line(m, m->io[LY_REG]);
                }
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
#ifdef GBJIT_PPU_ASYNC
                    __atomic_fetch_or(&m->io[IF_REG], INT_VBLANK_BIT, __ATOMIC_RELAXED);
#else
                    m->io[IF_REG] |= INT_VBLANK_BIT;
#endif
                    /* Frame complete — bump the seq so display tasks
                     * waiting on it can pull the freshly-drawn frame. */
                    m->frame_seq++;
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
                    /* End of VBlank — latch WY and reset the window-
                     * line counter for the next frame. */
                    m->ppu_latched_wy = m->io[WY_REG];
                    m->window_line = 0;
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
