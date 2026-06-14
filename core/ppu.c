#include "ppu.h"
#include "cpu_state.h"
#include "memory.h"
#include <string.h>   /* memcpy — frame buffer swap, tile-row pack/unpack */
#include "profiler.h"

/* Fast tile-row renderer.
 *
 * When GBJIT_PPU_FAST_BG_RENDERER is on (default 1), ppu_draw_line
 * uses two lookup tables to expand a 2-byte tile-row (b0, b1) into
 * 8 pre-palette colour IDs and 8 post-palette shades using two
 * 4-bit-keyed lookups instead of an 8-iteration bit-extract loop.
 *
 * Tables, indexed by [b0_nibble][b1_nibble], pack 4 results per u32
 * (one byte per pixel, MSB-first as on the LCD):
 *   bg_cid_lut    : 4 colour IDs (0..3) per entry.  Static, built once.
 *   bg_shade_lut  : 4 post-BGP shades per entry.    Rebuilt on BGP edges.
 *
 * Total memory: 2 KB. The shade LUT is BGP-keyed, so when the game
 * writes a new BGP we lazily rebuild on the next scanline that needs
 * it. */
#ifndef GBJIT_PPU_FAST_BG_RENDERER
#define GBJIT_PPU_FAST_BG_RENDERER 1
#endif

#if GBJIT_PPU_FAST_BG_RENDERER
static u32 bg_cid_lut[16][16];
static u32 bg_shade_lut[16][16];
static u8  bg_shade_lut_bgp = 0xFF;          /* invalid sentinel so first call builds */
static bool bg_lut_inited = false;

static void build_bg_cid_lut(void) {
    /* Each entry handles 4 pixels (one nibble of b0 + one of b1).
     * The display puts MSB of the byte on the left, so within the
     * 4-pixel group pixel 0 (leftmost) uses bit 3 of each nibble. */
    for (int b0 = 0; b0 < 16; b0++) {
        for (int b1 = 0; b1 < 16; b1++) {
            u32 packed = 0;
            for (int p = 0; p < 4; p++) {
                int bit = 3 - p;
                u8 cid = (u8)((((b1 >> bit) & 1u) << 1) | ((b0 >> bit) & 1u));
                packed |= ((u32)cid << (p * 8));
            }
            bg_cid_lut[b0][b1] = packed;
        }
    }
    bg_lut_inited = true;
}

static void rebuild_shade_lut(u8 bgp) {
    u8 shade[4] = {
        (u8)(bgp & 3), (u8)((bgp >> 2) & 3),
        (u8)((bgp >> 4) & 3), (u8)((bgp >> 6) & 3),
    };
    for (int b0 = 0; b0 < 16; b0++) {
        for (int b1 = 0; b1 < 16; b1++) {
            u32 cids = bg_cid_lut[b0][b1];
            u32 packed = ((u32)shade[(cids >>  0) & 3])
                       | ((u32)shade[(cids >>  8) & 3] <<  8)
                       | ((u32)shade[(cids >> 16) & 3] << 16)
                       | ((u32)shade[(cids >> 24) & 3] << 24);
            bg_shade_lut[b0][b1] = packed;
        }
    }
    bg_shade_lut_bgp = bgp;
}

static inline void ensure_bg_luts(u8 bgp) {
    if (!bg_lut_inited) build_bg_cid_lut();
    if (bgp != bg_shade_lut_bgp) rebuild_shade_lut(bgp);
}
#endif

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

/* Diagnostic: skip the entire per-line render to measure how much of
 * the per-frame wall time is actually consumed by PPU drawing vs CPU
 * dispatch + state-machine work. NOT a release flag — leaves the
 * framebuffer stale, so the OLED shows whatever was there. */
#ifndef GBJIT_PPU_SKIP_DRAW
#define GBJIT_PPU_SKIP_DRAW 0
#endif

/* Crop the rendered scanline range. The Heltec OLED shows the centre
 * 128×64 region of the 160×144 GB screen, i.e. GB rows 40..103 and
 * columns 16..143. Pixels outside the visible band can stay stale in
 * the framebuffer without affecting the displayed image. Set the
 * GBJIT_PPU_DRAW_MIN_LY / MAX_LY (rows) and MIN_X / MAX_X (columns)
 * macros at build time to gate drawing. The PPU state machine still
 * walks all scanlines; only the per-pixel work is gated. Defaults
 * are the full GB frame, so by default behaviour is unchanged. */
#ifndef GBJIT_PPU_DRAW_MIN_LY
#define GBJIT_PPU_DRAW_MIN_LY 0
#endif
#ifndef GBJIT_PPU_DRAW_MAX_LY
#define GBJIT_PPU_DRAW_MAX_LY 144
#endif
#ifndef GBJIT_PPU_DRAW_MIN_X
#define GBJIT_PPU_DRAW_MIN_X 0
#endif
#ifndef GBJIT_PPU_DRAW_MAX_X
#define GBJIT_PPU_DRAW_MAX_X 160
#endif

/* Render scanline `ly` into m->framebuffer. Called at LCD_TRANSFER →
 * LCD_HBLANK; reads OAM, VRAM, BGP/OBP0/OBP1, LCDC, SCY/SCX, WY/WX
 * which must reflect the final state for this line. */
static void ppu_draw_line(mmu *m, u8 ly) {
#if GBJIT_PPU_SKIP_DRAW
    (void)m; (void)ly;
    return;
#endif
#if GBJIT_PPU_DRAW_MIN_LY > 0 || GBJIT_PPU_DRAW_MAX_LY < 144
    /* Row hidden by the display crop — skip the per-line render.
     * The framebuffer slot stays at whatever was last written. The
     * preprocessor #if avoids emitting an always-false `u8 < 0`
     * comparison when the macros are left at their no-crop defaults. */
    if (ly < (u8)GBJIT_PPU_DRAW_MIN_LY || ly >= (u8)GBJIT_PPU_DRAW_MAX_LY) {
        return;
    }
#endif
#if GBJIT_FRAMEBUFFER_DOUBLE_BUFFER
    u8 *line = &m->framebuffer_back[(u32)ly * 160];
#else
    u8 *line = &m->framebuffer[(u32)ly * 160];
#endif
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
        const u8 *tilemap_row = &m->vram[tilemap_base + tile_row * 32];
        u16 row_off = (u16)(pixel_row * 2);
        const int min_x = GBJIT_PPU_DRAW_MIN_X;
        const int max_x = GBJIT_PPU_DRAW_MAX_X;
#if GBJIT_PPU_FAST_BG_RENDERER
        ensure_bg_luts(bgp);
        int x = min_x;
        u8 bg_x = (u8)(scx + min_x);
        u8 pc   = (u8)(bg_x & 7u);
        /* Leading partial tile (pc != 0): slow per-pixel until we
         * reach an 8-pixel boundary in the framebuffer. */
        if (pc != 0) {
            u8 tile_col = (u8)((bg_x >> 3) & 0x1Fu);
            u8 tile_id  = tilemap_row[tile_col];
            u16 tile_addr = unsigned_tiles
                          ? (u16)(tile_id * 16)
                          : (u16)(0x1000 + (i8)tile_id * 16);
            u8 b0 = m->vram[tile_addr + row_off];
            u8 b1 = m->vram[tile_addr + row_off + 1u];
            for (; pc < 8 && x < max_x; pc++, x++, bg_x++) {
                u8 bit = (u8)(7u - pc);
                u8 cid = (u8)((((b1 >> bit) & 1u) << 1) | ((b0 >> bit) & 1u));
                bg_color_id[x] = cid;
                line[x] = bg_shade[cid];
            }
        }
        /* Aligned fast path: 8 pixels per tile via two 4-bit LUT lookups. */
        while (x + 8 <= max_x) {
            u8 tile_col = (u8)((bg_x >> 3) & 0x1Fu);
            u8 tile_id  = tilemap_row[tile_col];
            u16 tile_addr = unsigned_tiles
                          ? (u16)(tile_id * 16)
                          : (u16)(0x1000 + (i8)tile_id * 16);
            u8 b0 = m->vram[tile_addr + row_off];
            u8 b1 = m->vram[tile_addr + row_off + 1u];
            u32 cids_h = bg_cid_lut[b0 >> 4][b1 >> 4];
            u32 cids_l = bg_cid_lut[b0 & 0xF][b1 & 0xF];
            u32 sha_h  = bg_shade_lut[b0 >> 4][b1 >> 4];
            u32 sha_l  = bg_shade_lut[b0 & 0xF][b1 & 0xF];
            memcpy(&bg_color_id[x],     &cids_h, 4);
            memcpy(&bg_color_id[x + 4], &cids_l, 4);
            memcpy(&line[x],            &sha_h,  4);
            memcpy(&line[x + 4],        &sha_l,  4);
            x += 8;
            bg_x += 8;
        }
        /* Trailing partial tile. */
        while (x < max_x) {
            u8 tile_col = (u8)((bg_x >> 3) & 0x1Fu);
            u8 tile_id  = tilemap_row[tile_col];
            u16 tile_addr = unsigned_tiles
                          ? (u16)(tile_id * 16)
                          : (u16)(0x1000 + (i8)tile_id * 16);
            u8 b0 = m->vram[tile_addr + row_off];
            u8 b1 = m->vram[tile_addr + row_off + 1u];
            for (pc = 0; pc < 8 && x < max_x; pc++, x++, bg_x++) {
                u8 bit = (u8)(7u - pc);
                u8 cid = (u8)((((b1 >> bit) & 1u) << 1) | ((b0 >> bit) & 1u));
                bg_color_id[x] = cid;
                line[x] = bg_shade[cid];
            }
        }
#else
        /* Per-pixel bit-extract path (legacy). */
        int x = min_x;
        u8 bg_x = (u8)(scx + min_x);
        u8 pc   = (u8)(bg_x & 7u);
        while (x < max_x) {
            u8 tile_col = (u8)((bg_x >> 3) & 0x1Fu);
            u8 tile_id  = tilemap_row[tile_col];
            u16 tile_addr;
            if (unsigned_tiles) {
                tile_addr = (u16)(tile_id * 16);
            } else {
                tile_addr = (u16)(0x1000 + (i8)tile_id * 16);
            }
            u8 b0 = m->vram[tile_addr + row_off];
            u8 b1 = m->vram[tile_addr + row_off + 1u];
            for (; pc < 8 && x < max_x; pc++, x++, bg_x++) {
                u8 bit = (u8)(7u - pc);
                u8 cid = (u8)((((b1 >> bit) & 1u) << 1) | ((b0 >> bit) & 1u));
                bg_color_id[x] = cid;
                line[x] = bg_shade[cid];
            }
            pc = 0;
        }
#endif
    } else {
        for (int x = GBJIT_PPU_DRAW_MIN_X; x < GBJIT_PPU_DRAW_MAX_X; x++) {
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
            if (start_x < GBJIT_PPU_DRAW_MIN_X) start_x = GBJIT_PPU_DRAW_MIN_X;
            int end_x = GBJIT_PPU_DRAW_MAX_X;
            u16 tilemap_base = (lcdc & LCDC_WINDOW_TILEMAP_HI) ? 0x1C00 : 0x1800;
            bool unsigned_tiles = (lcdc & LCDC_BG_TILEDATA_LO) != 0;
            u8 wy_internal = m->window_line;
            u8 tile_row = (u8)(wy_internal >> 3);
            u8 pixel_row = (u8)(wy_internal & 7);
            const u8 *tilemap_row = &m->vram[tilemap_base + tile_row * 32];
            u16 row_off = (u16)(pixel_row * 2);
            int x = start_x;
            int win_x = x - (int)wx + 7;
            u8 pc = (u8)(win_x & 7);
            while (x < end_x) {
                u8 tile_col = (u8)((win_x >> 3) & 0x1F);
                u8 tile_id  = tilemap_row[tile_col];
                u16 tile_addr;
                if (unsigned_tiles) {
                    tile_addr = (u16)(tile_id * 16);
                } else {
                    tile_addr = (u16)(0x1000 + (i8)tile_id * 16);
                }
                u8 b0 = m->vram[tile_addr + row_off];
                u8 b1 = m->vram[tile_addr + row_off + 1u];
                for (; pc < 8 && x < end_x; pc++, x++, win_x++) {
                    u8 bit = (u8)(7u - pc);
                    u8 cid = (u8)((((b1 >> bit) & 1u) << 1) | ((b0 >> bit) & 1u));
                    bg_color_id[x] = cid;
                    line[x] = bg_shade[cid];
                }
                pc = 0;
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
            /* Load the row's two bitplane bytes once; bit-extract per
             * pixel below. Same logic as fetch_tile_pixel but pulled
             * out of the 8-iteration inner loop. */
            u16 row_addr = (u16)(tile_addr + (u16)(row_in_sprite * 2));
            u8 b0 = m->vram[row_addr];
            u8 b1 = m->vram[row_addr + 1u];
            bool flip_x = (vis[s].attr & OAM_FLIP_X) != 0;
            bool bg_prio = (vis[s].attr & OAM_BG_PRIO) != 0;
            /* Early skip if the whole sprite is outside the column crop. */
            if (left >= GBJIT_PPU_DRAW_MAX_X || left + 8 <= GBJIT_PPU_DRAW_MIN_X) {
                continue;
            }
            for (int px = 0; px < 8; px++) {
                int x = left + px;
                if (x < GBJIT_PPU_DRAW_MIN_X || x >= GBJIT_PPU_DRAW_MAX_X) continue;
                u8 col_in_sprite = flip_x ? (u8)(7 - px) : (u8)px;
                u8 bit = (u8)(7u - col_in_sprite);
                u8 cid = (u8)((((b1 >> bit) & 1u) << 1) | ((b0 >> bit) & 1u));
                if (cid == 0) continue;                    /* sprite color 0 = transparent */
                if (bg_prio && bg_color_id[x] != 0) continue;
                line[x] = pal_shade[cid];
            }
        }
    }
}

/* --- PPU render offload (per-line register log + deferred render) ------
 *
 * GBJIT_PPU_OFFLOAD decouples *rendering* from *timing*. The PPU state
 * machine (LY/STAT/IF/mode timing) MUST stay synchronous and in lockstep
 * with the CPU for interrupt parity — that part never moves. Only the
 * per-pixel rendering of the 144 scanlines (~11.5 ms, the static
 * bottleneck) is deferred to one batch at VBlank, so it can later run on
 * the second core (Increment 2).
 *
 * Raster effects (SML's HUD/level split scrolls SCX/SCY per scanline,
 * dmg-acid2 rewrites palettes per line, etc.) mean each scanline must be
 * rendered with the register values it had *at that line's* mode3→0. So at
 * each mode3→0 we don't render — we snapshot the line's registers into
 * s_line_log[ly]. The single batch render at VBlank then replays line by
 * line from the log.
 *
 * VRAM/OAM are NOT snapshotted per line here: this batch render uses the
 * end-of-frame VRAM/OAM. That is bit-exact with the synchronous renderer
 * for any game that only mutates VRAM/OAM during VBlank (the common case,
 * and what real DMG hardware forces). A game that streams VRAM/OAM mid-
 * frame (during HBlank) would diverge — the host parity harness gates for
 * exactly that before this is trusted on device. window_line is logged per
 * line (it advances only on window-active lines), so the replay needs no
 * live PPU state. */
#if GBJIT_PPU_OFFLOAD
typedef struct {
    u8 lcdc, bgp, scx, scy, obp0, obp1, wx;
    u8 latched_wy;
    u8 window_line;   /* window line counter value at this scanline's draw */
} ppu_lstate;

static ppu_lstate s_line_log[LCD_HEIGHT];
static int s_cap_count;   /* scanlines captured so far this frame (0..144) */
static int s_rendered;    /* scanlines already drawn this frame (0..s_cap_count) */

/* Fast no-op gate for the MMU write hooks: nonzero only while captured-but-
 * undrawn scanlines exist (i.e. between an active-frame mode3→0 and the next
 * flush/VBlank). The common case — VRAM/OAM written during VBlank — sees
 * this clear and skips the cross-TU flush call entirely. */
int gbjit_ppu_have_pending = 0;

#ifdef GBJIT_PPU_OFFLOAD_DIAG
/* Host-only diagnostic: how often does VRAM change *during* the visible
 * frame (between line 0's capture and VBlank)? That is exactly what makes
 * the single-snapshot batch render diverge from the per-line renderer. */
#include <stdio.h>
static u8  s_diag_vram0[VRAM_SIZE];
static u64 s_diag_frames = 0, s_diag_midframe_frames = 0, s_diag_total_changed = 0;
static void diag_capture_vram0(mmu *m) { memcpy(s_diag_vram0, m->vram, VRAM_SIZE); }
static void diag_check_vram(mmu *m) {
    s_diag_frames++;
    u32 changed = 0;
    for (u32 i = 0; i < VRAM_SIZE; i++) if (s_diag_vram0[i] != m->vram[i]) changed++;
    if (changed) { s_diag_midframe_frames++; s_diag_total_changed += changed; }
}
__attribute__((destructor)) static void diag_report(void) {
    fprintf(stderr, "[offload-diag] frames=%llu midframe_vram_frames=%llu (%.1f%%) avg_changed_bytes=%.1f\n",
            (unsigned long long)s_diag_frames,
            (unsigned long long)s_diag_midframe_frames,
            s_diag_frames ? 100.0 * s_diag_midframe_frames / s_diag_frames : 0.0,
            s_diag_midframe_frames ? (double)s_diag_total_changed / s_diag_midframe_frames : 0.0);
}
#endif

/* Mirror of ppu_draw_line's window-active predicate (lines 441-444): the
 * window's internal line counter advances iff the window would render on
 * this scanline. Capture uses this to keep window_line in step without
 * drawing. */
static bool ppu_window_active(mmu *m, u8 ly) {
    u8 lcdc = m->io[LCDC_REG];
    if (!(lcdc & LCDC_WINDOW_ENABLE)) return false;
    if (!(lcdc & LCDC_BG_ENABLE))     return false;
    if (ly < m->ppu_latched_wy)       return false;
    if (m->io[WX_REG] >= 167)         return false;
    return true;
}

/* Called at mode3→0 instead of ppu_draw_line: record the per-line register
 * state, then advance window_line exactly as ppu_draw_line would have. VRAM
 * and OAM are NOT snapshotted — they are flushed lazily (see below). */
static void ppu_capture_line(mmu *m, u8 ly) {
    ppu_lstate *L = &s_line_log[ly];
    L->lcdc       = m->io[LCDC_REG];
    L->bgp        = m->io[BGP_REG];
    L->scx        = m->io[SCX_REG];
    L->scy        = m->io[SCY_REG];
    L->obp0       = m->io[OBP0_REG];
    L->obp1       = m->io[OBP1_REG];
    L->wx         = m->io[WX_REG];
    L->latched_wy = m->ppu_latched_wy;
    L->window_line = m->window_line;
    s_cap_count   = (int)ly + 1;
    gbjit_ppu_have_pending = 1;
#ifdef GBJIT_PPU_OFFLOAD_DIAG
    if (ly == 0) diag_capture_vram0(m);
#endif
    if (ppu_window_active(m, ly)) m->window_line++;
}

/* Replay captured scanlines [from,to) through ppu_draw_line using the
 * registers in `log` and m's CURRENT VRAM/OAM, writing m->framebuffer.
 * The live io[]/latched_wy/window_line are saved and restored so the PPU
 * state machine is undisturbed (VBlank STAT/IF/LY are already set by the
 * caller). `m` and `log` may be the live mmu+s_line_log (synchronous flush)
 * or a render-thread snapshot (mmu copy + log copy on core 1). */
static void ppu_render_lines(mmu *m, const ppu_lstate *log, int from, int to) {
    if (from >= to) return;
    u8 s_lcdc = m->io[LCDC_REG], s_bgp = m->io[BGP_REG];
    u8 s_scx = m->io[SCX_REG],   s_scy = m->io[SCY_REG];
    u8 s_obp0 = m->io[OBP0_REG],  s_obp1 = m->io[OBP1_REG];
    u8 s_wx = m->io[WX_REG];
    u8 s_wy = m->ppu_latched_wy,  s_wl = m->window_line;

    for (int ly = from; ly < to; ly++) {
        const ppu_lstate *L = &log[ly];
        m->io[LCDC_REG] = L->lcdc;
        m->io[BGP_REG]  = L->bgp;
        m->io[SCX_REG]  = L->scx;
        m->io[SCY_REG]  = L->scy;
        m->io[OBP0_REG] = L->obp0;
        m->io[OBP1_REG] = L->obp1;
        m->io[WX_REG]   = L->wx;
        m->ppu_latched_wy = L->latched_wy;
        m->window_line    = L->window_line;
        ppu_draw_line(m, (u8)ly);
    }

    m->io[LCDC_REG] = s_lcdc; m->io[BGP_REG] = s_bgp;
    m->io[SCX_REG]  = s_scx;  m->io[SCY_REG] = s_scy;
    m->io[OBP0_REG] = s_obp0; m->io[OBP1_REG] = s_obp1;
    m->io[WX_REG]   = s_wx;
    m->ppu_latched_wy = s_wy;  m->window_line = s_wl;
}

/* Flush captured-but-undrawn scanlines NOW with the current VRAM/OAM.
 * Called from the MMU just before a mid-frame VRAM/OAM write (or OAM DMA)
 * lands: every line captured since the last flush was captured under this
 * same VRAM/OAM (the write we're about to apply is the first change since),
 * so they render bit-exactly as the synchronous per-line renderer would.
 * The capture/flush boundary is block-granular on both sides, matching the
 * reference renderer's own block-granular draw point. */
void ppu_offload_flush(struct mmu *m) {
    if (s_rendered < s_cap_count) {
        ppu_render_lines(m, s_line_log, s_rendered, s_cap_count);
        s_rendered = s_cap_count;
    }
    gbjit_ppu_have_pending = 0;
}

#if GBJIT_PPU_OFFLOAD_THREAD
/* --- Render thread (core 1 on ESP, pthread on host) -------------------- *
 *
 * The end-of-frame batch render is handed to a second core. Safety rests
 * on one rule: the render thread and the emulation core NEVER touch the
 * shared render state (framebuffer, the global bg-shade LUT, VRAM/OAM/log)
 * at the same time. The handoff is a private snapshot + two semaphores:
 *
 *   kick (emul core, at VBlank): wait for the previous render to finish,
 *     copy VRAM+OAM+log into a private render context, signal "go", return.
 *   render thread: on "go", render [from,to) into its private framebuffer,
 *     copy those lines into the live framebuffer, signal "done".
 *   render_wait (emul core, before it next reads the framebuffer / kicks):
 *     wait "done".
 *
 * Because the render thread reads ONLY the snapshot, the emul core may keep
 * mutating live VRAM/OAM (the next frame) the instant kick returns. The
 * emul core only renders (mid-frame flush) while the thread is idle — it
 * always render_wait()s before resuming, so the two never run ppu_draw_line
 * (hence touch the bg-shade LUT) concurrently. */

#include <stdlib.h>   /* malloc/free (host); free on ESP routes to heap_caps */
#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
typedef SemaphoreHandle_t ofl_sem;
#define OFL_SEM_NEW()   xSemaphoreCreateBinary()
#define OFL_SEM_GIVE(s) xSemaphoreGive(s)
#define OFL_SEM_WAIT(s) ((void)xSemaphoreTake((s), portMAX_DELAY))
#else
#include <pthread.h>
#include <semaphore.h>
typedef sem_t ofl_sem;
#define OFL_SEM_NEW()   ofl_host_sem_new()
#define OFL_SEM_GIVE(s) sem_post(&(s))
#define OFL_SEM_WAIT(s) ofl_host_sem_wait(&(s))
static sem_t ofl_host_sem_new(void) { sem_t s; sem_init(&s, 0, 0); return s; }
static void  ofl_host_sem_wait(sem_t *s) { while (sem_wait(s) != 0) {} }
#endif

static mmu        *s_render_mmu;             /* private ctx: snapshot vram/oam + own fb */
static ppu_lstate  s_render_log[LCD_HEIGHT]; /* snapshot of the per-line register log */
static mmu        *s_render_dst;             /* live mmu to copy finished scanlines into */
static int         s_render_from, s_render_to;
static volatile bool s_render_inflight = false;
static volatile bool s_render_started  = false;
static volatile bool s_render_run      = false;
static ofl_sem s_sem_go, s_sem_done;

/* The actual render, run on the render thread. */
static void render_worker(void) {
    ppu_render_lines(s_render_mmu, s_render_log, s_render_from, s_render_to);
    if (s_render_to > s_render_from) {
        memcpy(&s_render_dst->framebuffer[(size_t)s_render_from * 160],
               &s_render_mmu->framebuffer[(size_t)s_render_from * 160],
               (size_t)(s_render_to - s_render_from) * 160);
    }
}

#if defined(ESP_PLATFORM)
static TaskHandle_t s_render_task;
static void render_task_fn(void *arg) {
    (void)arg;
    while (s_render_run) {
        OFL_SEM_WAIT(s_sem_go);
        if (!s_render_run) break;
        render_worker();
        OFL_SEM_GIVE(s_sem_done);
    }
    vTaskDelete(NULL);
}
#else
static pthread_t s_render_pthread;
static void *render_task_fn(void *arg) {
    (void)arg;
    while (s_render_run) {
        OFL_SEM_WAIT(s_sem_go);
        if (!s_render_run) break;
        render_worker();
        OFL_SEM_GIVE(s_sem_done);
    }
    return NULL;
}
#endif

/* Block until the in-flight render (if any) has finished. */
void ppu_offload_render_wait(void) {
    if (s_render_inflight) {
        OFL_SEM_WAIT(s_sem_done);
        s_render_inflight = false;
    }
}

/* Hand scanlines [from,to) to the render thread, snapshotting the state it
 * needs. If the thread isn't up, render synchronously on the caller. */
static void ppu_offload_kick(mmu *src, int from, int to) {
    if (!s_render_started) {
        ppu_render_lines(src, s_line_log, from, to);
        return;
    }
    ppu_offload_render_wait();                 /* render ctx now free to reuse */
    memcpy(s_render_mmu->vram, src->vram, VRAM_SIZE);
    memcpy(s_render_mmu->oam,  src->oam,  OAM_SIZE);
    memcpy(s_render_log, s_line_log, sizeof(s_render_log));
    s_render_dst  = src;
    s_render_from = from;
    s_render_to   = to;
    s_render_inflight = true;
    OFL_SEM_GIVE(s_sem_go);
}

void ppu_offload_thread_start(void) {
    if (s_render_started) return;
#if defined(ESP_PLATFORM)
    s_render_mmu = (mmu *)heap_caps_malloc(sizeof(mmu),
                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    s_render_mmu = (mmu *)malloc(sizeof(mmu));
#endif
    if (!s_render_mmu) return;                 /* fall back to synchronous render */
    memset(s_render_mmu, 0, sizeof(mmu));
    s_sem_go   = OFL_SEM_NEW();
    s_sem_done = OFL_SEM_NEW();
    s_render_run = true;
#if defined(ESP_PLATFORM)
    BaseType_t ok = xTaskCreatePinnedToCore(render_task_fn, "gb_render", 4096,
                                            NULL, tskIDLE_PRIORITY + 1,
                                            &s_render_task, 1 /* Core 1 */);
    if (ok != pdPASS) { s_render_run = false; free(s_render_mmu); s_render_mmu = NULL; return; }
#else
    if (pthread_create(&s_render_pthread, NULL, render_task_fn, NULL) != 0) {
        s_render_run = false; free(s_render_mmu); s_render_mmu = NULL; return;
    }
#endif
    s_render_started = true;
}

void ppu_offload_thread_stop(void) {
    if (!s_render_started) return;
    ppu_offload_render_wait();                  /* ensure no render in flight */
#if defined(ESP_PLATFORM)
    /* Device: the render task is long-lived and reused across ROM reloads
     * (thread_start is idempotent). Leaving it parked on the "go" semaphore
     * — rather than vTaskDelete + recreate on every re-init — avoids both a
     * delete/recreate race and re-allocating the 40 KB render context. */
#else
    /* Host: tear the pthread down cleanly so a test binary can exit. */
    s_render_run = false;
    OFL_SEM_GIVE(s_sem_go);
    pthread_join(s_render_pthread, NULL);
    free(s_render_mmu);
    s_render_mmu = NULL;
    s_render_started = false;
#endif
}
#endif /* GBJIT_PPU_OFFLOAD_THREAD */

/* End-of-frame (VBlank): draw whatever scanlines weren't already flushed. */
static void ppu_render_frame_from_log(mmu *m) {
#ifdef GBJIT_PPU_OFFLOAD_DIAG
    diag_check_vram(m);
#endif
#if GBJIT_PPU_OFFLOAD_THREAD
    ppu_offload_kick(m, s_rendered, s_cap_count);
#else
    ppu_render_lines(m, s_line_log, s_rendered, s_cap_count);
#endif
    s_rendered = s_cap_count;
    gbjit_ppu_have_pending = 0;
}
#endif /* GBJIT_PPU_OFFLOAD */

/* --- Timer (FF04..FF07) ------------------------------------------------ */

#define DIV_REG       0x04
#define TIMA_REG      0x05
#define TMA_REG       0x06
#define TAC_REG       0x07
#define INT_TIMER_BIT 0x04

/* TIMA period in T-cycles, indexed by TAC bits 1..0. Pan Docs §"Timer
 * Registers": 00→1024 (4096Hz), 01→16 (262144Hz), 10→64 (65536Hz),
 * 11→256 (16384Hz). DIV (FF04) is always 16384Hz = period 256... wait
 * no, DIV is 16384Hz = period 64 T-cycles per increment of the visible
 * register, which corresponds to a /256 prescaler on the internal
 * 16-bit DIV that ticks every T-cycle. We model the visible byte
 * directly: it ticks every 64 T-cycles. */
static const u16 tima_period_lookup[4] = { 1024, 16, 64, 256 };

static void timer_tick(mmu *m, u64 cycles_now) {
    u32 delta = (u32)(cycles_now - m->timer_last_cycles);
    m->timer_last_cycles = cycles_now;
    if (delta == 0) return;

    /* DIV — always ticking, every 64 T-cycles bumps the visible byte. */
    m->timer_div_acc += delta;
    if (m->timer_div_acc >= 64) {
        u32 ticks = m->timer_div_acc / 64;
        m->timer_div_acc %= 64;
        m->io[DIV_REG] = (u8)(m->io[DIV_REG] + ticks);
    }

    /* TIMA — only if TAC bit 2 is set. Overflow reloads from TMA and
     * raises IF.TIMER. */
    u8 tac = m->io[TAC_REG];
    if (tac & 0x04) {
        u16 period = tima_period_lookup[tac & 3];
        m->timer_tima_acc += delta;
        while (m->timer_tima_acc >= period) {
            m->timer_tima_acc -= period;
            if (m->io[TIMA_REG] == 0xFF) {
                m->io[TIMA_REG] = m->io[TMA_REG];
#ifdef GBJIT_PPU_ASYNC
                __atomic_fetch_or(&m->io[IF_REG], INT_TIMER_BIT, __ATOMIC_RELAXED);
#else
                m->io[IF_REG] |= INT_TIMER_BIT;
#endif
            } else {
                m->io[TIMA_REG]++;
            }
        }
    }
}

/* --- Main entry -------------------------------------------------------- */

/* Body of ppu_tick after the early-return check. Factored out so that
 * ppu_flush() can run it unconditionally — see core/ppu.h for the why.
 * Always advances ppu_last_cpu_cycles to cpu->cycles, applies any LCDC
 * edge currently pending, and drains any state-machine transitions that
 * the accumulated delta crosses. */
static void ppu_advance(struct cpu_state *cpu) {
    mmu *m = cpu->mmu;
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
#if GBJIT_PPU_OFFLOAD
                    ppu_capture_line(m, m->io[LY_REG]);
#else
                    ppu_draw_line(m, m->io[LY_REG]);
#endif
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
#if GBJIT_PPU_OFFLOAD
                    /* The frame's 144 scanlines were captured (not drawn)
                     * at their mode3→0; render them all now, before
                     * frame_seq is bumped so readers still see a complete
                     * frame. (Increment 2 moves this batch to core 1.) */
                    ppu_render_frame_from_log(m);
#endif
#ifdef GBJIT_PPU_ASYNC
                    __atomic_fetch_or(&m->io[IF_REG], INT_VBLANK_BIT, __ATOMIC_RELAXED);
#else
                    m->io[IF_REG] |= INT_VBLANK_BIT;
#endif
                    /* Frame complete. With double-buffering enabled
                     * we publish the back buffer to the front first,
                     * then bump frame_seq — readers that gate on
                     * frame_seq only ever see fully-composed frames.
                     * Without it (default), ppu_draw_line wrote
                     * straight into framebuffer above. */
#if GBJIT_FRAMEBUFFER_DOUBLE_BUFFER
                    memcpy(m->framebuffer, m->framebuffer_back,
                           sizeof(m->framebuffer));
#endif
                    m->frame_seq++;
                    /* Wall-clock frame pacer (60 fps lock). Board
                     * firmwares set this to busy-wait to the next
                     * 16.667 ms tick; the host leaves it NULL so the
                     * benches and tests still run free. The hook
                     * fires before STAT IRQs are evaluated so the
                     * post-VBlank IRQ for the new frame doesn't
                     * include the pacer wait in its dispatch budget. */
                    if (m->frame_complete_cb) m->frame_complete_cb(m);
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
#if GBJIT_PPU_OFFLOAD
                    /* New frame: reset the capture/render cursors. Lines the
                     * PPU doesn't reach this frame (e.g. LCD off mid-frame)
                     * simply stay below s_cap_count and are never replayed. */
                    s_cap_count = 0;
                    s_rendered = 0;
                    gbjit_ppu_have_pending = 0;
#endif
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

void ppu_tick(struct cpu_state *cpu) {
    if (!cpu || !cpu->mmu) return;
    PROF_BEGIN(PROF_PPU);
    mmu *m = cpu->mmu;

    /* Timer first — runs every tick regardless of the PPU early-return,
     * because SML and others poll DIV / wait for TIMA-overflow IRQ even
     * when no PPU state transition is due. The cost is just two adds +
     * a compare on the fast path. */
    timer_tick(m, cpu->cycles);

    /* Fast-path early-return — most dispatcher iterations span fewer
     * cycles than the next state transition. The deadline is reset
     * after every transition (or every LCDC edge). The comparison is
     * wrap-safe: cpu->cycles is a 32-bit free-running counter, so a
     * plain `cycles < deadline` would early-return forever once the
     * counter wraps past the deadline. */
    if (!gb_cycles_reached(cpu->cycles, m->ppu_next_event_cycles)
            && m->io[LCDC_REG] == m->ppu_last_lcdc) {
        PROF_END(PROF_PPU);
        return;
    }

    ppu_advance(cpu);
    PROF_END(PROF_PPU);
}

void ppu_flush(struct cpu_state *cpu) {
    if (!cpu || !cpu->mmu) return;
    PROF_BEGIN(PROF_PPU);
    timer_tick(cpu->mmu, cpu->cycles);
    ppu_advance(cpu);
    PROF_END(PROF_PPU);
}

/* Re-evaluate the LY=LYC coincidence flag and the level-triggered STAT
 * interrupt line after a CPU write to LYC ($FF45). The state machine only
 * refreshes the coincidence flag at LY transitions, so a mid-line LYC
 * write isn't reflected until the next line. That drops STAT IRQs in the
 * common raster-effect pattern where a handler points LYC at the *very
 * next* scanline: the old LY==LYC coincidence stays latched, ppu_stat_line
 * never falls, and the rising edge at the new LY=LYC is lost. dmg-acid2
 * chains its per-line register writes this way (LYC 128->129->130...), so
 * without this only ~1 in 4 of its lines were being serviced and the image
 * flickered through wrong register schedules frame to frame.
 *
 * The caller (mmu_write8) has already flushed the PPU up to cpu->cycles, so
 * LY/mode are current; here we only recompute the LYC-derived bits. */
void ppu_sync_lyc(struct cpu_state *cpu) {
    if (!cpu || !cpu->mmu) return;
    ppu_check_lyc(cpu->mmu);
    ppu_update_stat_irq(cpu->mmu);
}
