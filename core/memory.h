#ifndef MEMORY_H
#define MEMORY_H

#include "gb_types.h"

/* MMU: cartridge ROM (heap-allocated, sized to the actual cart on
   mmu_load_rom — see ROM_SIZE_MAX), 8 KB VRAM, 8 KB WRAM, OAM, IO,
   HRAM. Minimal MBC1 banking: 5-bit low + 2-bit high ROM bank number,
   ROM-mode only (no RAM bank). MBC0 carts are handled as the degenerate
   case (rom_bank stays at 1 and writes to $0000..$7FFF are ignored).

   Why ROM is a pointer rather than a static array: on the ESP32-S3 the
   JIT exec arena is carved out of internal SRAM, and the worst-case 256
   KB cartridge would eat most of DRAM if we held it as `.bss`. Going
   through a pointer lets us either (a) allocate only what the loaded
   cart needs, or (b) place the cart in PSRAM on real hardware, freeing
   ~200 KB of internal SRAM for JIT-emitted code. */

/* Max cart ROM we accept. 2 MB covers the largest MBC1/MBC3 carts (e.g.
 * 1 MB Pokemon R/B). ROM lives in a heap buffer (PSRAM on device), never a
 * static array, so raising this costs nothing until a big cart is actually
 * loaded. NOTE: the device's embedded-autoboot path is bounded by the app
 * flash partition (~1.5 MB incl. firmware), so >~1 MB ROMs must come from SD. */
#define ROM_SIZE_MAX (2u * 1024u * 1024u)
#define ROM_BANK_SIZE (16u * 1024u)
#define RAM_BANK_SIZE (8u  * 1024u)
#define ROM_DEFAULT_BYTES (32u * 1024u)  /* allocated by gb_mmu_init */
#define VRAM_SIZE  (8u  * 1024u)
#define WRAM_SIZE  (8u  * 1024u)
#define OAM_SIZE   160u
#define HRAM_SIZE  127u

/* MBC types we care about. */
typedef enum {
    MBC_NONE = 0,
    MBC_1    = 1,
    MBC_3    = 3,
} mbc_type;

typedef struct mmu {
    u8 *rom;            /* heap-backed cart ROM. gb_mmu_init allocates */
                        /* ROM_DEFAULT_BYTES; mmu_load_rom resizes to */
                        /* the cart's actual rounded-up bank count. */
    u32 rom_capacity;   /* bytes actually allocated for *rom */
    u8 vram[VRAM_SIZE];
    u8 wram[WRAM_SIZE];
    u8 oam [OAM_SIZE];
    u8 hram[HRAM_SIZE];

    /* IO area $FF00..$FF7F. We only model the bits relevant to the CPU:
       serial transfer (FF01/FF02), interrupt flag (FF0F), timer regs (FF04..FF07).
       Everything else is a flat byte store. */
    u8 io[0x80];

    u8 ie;          /* FF FF interrupt enable */
    u8 boot_rom_disabled;

    /* Cartridge banking state. `rom_bank` is the index of the bank mapped
     * at $4000..$7FFF (1-based — bank 0 is permanently at $0000..$3FFF).
     * `rom_banks` is the number of 16 KB banks the loaded ROM actually
     * has; bank-switch writes are masked to this. */
    mbc_type mbc;
    u8       rom_bank;
    u16      rom_banks;

    /* External cartridge RAM ($A000..$BFFF), heap-backed (PSRAM on device),
     * NULL if the cart has none. `cart_ram_size` is the allocated byte count.
     * `ram_bank` selects the 8 KB RAM bank (0..3) — or, on MBC3, an RTC
     * register ($08..$0C). `ram_enable` gates all RAM/RTC access (set by a
     * $0A write to $0000..$1FFF). `has_battery` marks save-backed RAM (for a
     * future persist-to-flash hook; not yet wired). */
    u8      *cart_ram;
    u32      cart_ram_size;
    u8       ram_bank;
    u8       ram_enable;
    u8       has_battery;
    /* MBC3 real-time clock — latched copy of S,M,H,DayLo,DayHi. Ticked
     * deterministically from cpu->cycles on a latch ($6000..$7FFF 0->1), so
     * the JIT and interpreter read identical values. Pokemon R/B (no timer)
     * never touch it; present so MBC3+TIMER carts don't fault. */
    u8       rtc[5];
    u8       rtc_latch;

    /* Joypad button mask. Bit per button, 1 = pressed:
     *   bit 0 = A, 1 = B, 2 = Select, 3 = Start
     *   bit 4 = Right, 5 = Left, 6 = Up, 7 = Down
     * The JOYP ($FF00) read consults this together with the P14/P15 row
     * select the CPU wrote into io[0x00]. Default 0 = no buttons pressed
     * (matches the previous behaviour where reads always returned 0xCF
     * in the low nibble). Test harnesses can drive scripted inputs by
     * mutating this directly between dispatcher iterations. */
    u8       buttons;

    /* Set by mmu_write8 whenever MBC bank-control writes change the bank
     * currently mapped at $4000..$7FFF. The JIT dispatcher polls this
     * flag before entering each block and invalidates any cached blocks
     * compiled from the banked region — without that, a cached block can
     * execute stale immediates / branch targets from the previous bank
     * (SML's main loop diverges at PC=$7FF3 ~668k cycles in). The
     * interpreter ignores this flag (it always re-reads through
     * mmu_read8). */
    u8       rom_bank_dirty;

    /* Self-modifying-code tracking for the JIT. Games that run code from
     * RAM (WRAM / HRAM) and then overwrite it — blargg's per-instruction
     * test runner rewrites its WRAM code between sub-tests — leave the
     * JIT holding a stale translation of the old bytes. ROM-region SMC
     * is already covered by rom_bank_dirty; this covers RAM.
     *
     * `jit_page_state` is indexed by 256-byte GB page (addr >> 8):
     *   0 = no JIT block compiled from this page
     *   1 = a block was compiled here (clean)
     *   2 = a block was compiled here AND the page has since been written
     * The dispatcher sets state 1 in insert_block; mmu_write8 promotes
     * 1 → 2 (and raises jit_smc_dirty) on a write to a code page; the
     * dispatcher polls jit_smc_dirty before each block, invalidates the
     * dirty pages, and resets their state. Writes to pages with state 0
     * (the overwhelming majority — stack, variables) cost just one array
     * load + branch, so the common case stays cheap. The interpreter
     * never sets state 1, so it pays nothing. */
    u8       jit_smc_dirty;
    u8       jit_page_state[256];
    /* For each dirty (state 2) page, the inclusive range of GB addresses
     * written since it went dirty. The dispatcher invalidates only the blocks
     * whose byte range actually overlaps [wlo,whi] instead of every block on
     * the page — so code and data sharing a 256-byte page (e.g. SML's OAM-DMA
     * routine + HRAM variables) no longer churns the JIT. Valid only where
     * jit_page_state == 2. */
    u16      jit_page_wlo[256];
    u16      jit_page_whi[256];
    /* Per-page bounding box [lo,hi) of the GB addresses actually covered by
     * compiled blocks on that page. smc_mark only flags SMC when a write lands
     * inside this range — a write to the data part of a code+data page never
     * raises jit_smc_dirty, so it costs one compare and nothing more (no flush,
     * no invalidation scan). hi==0 means "no code on this page". Grown by
     * insert_block; recomputed from survivors by invalidate_page_range. */
    u16      jit_page_code_lo[256];
    u16      jit_page_code_hi[256];

    /* Back-pointer to the CPU. ppu_tick uses cpu->cycles as its time base
     * and writes back into io[$44]/io[$41]; left NULL the PPU model is
     * inert and reads return the raw IO bytes. */
    struct cpu_state *cpu;

    /* PPU state machine — adapted from CrankBoy's peanut_gb (MIT
     * licensed). Stateful mode 2 → 3 → 0 → 2 walk per scanline, with
     * dedicated VBlank handling for lines 144-153, level-triggered STAT
     * IRQ, LY=LYC coincidence, and the short-line-153 LY=0-wrap quirk.
     * See core/ppu.c for the full state machine.
     *
     * `ppu_lcd_count` is the dot count within the current mode (0..456
     * range); `ppu_lcd_mode` is the current STAT mode (0=HBlank,
     * 1=VBlank, 2=OAM-scan, 3=Transfer). `ppu_lcd_off_count` ticks
     * frame cycles while the LCD is disabled. `ppu_mode3_cycles` and
     * `ppu_mode0_cycles` are the per-scanline lengths set at the
     * mode-2 → 3 transition (depend on SCX + OAM sprite layout).
     * `ppu_stat_line` mirrors the level-triggered STAT interrupt
     * line so we only fire IF.LCDC on a rising edge.
     * `ppu_last_lcdc` is used to detect LCDC enable/disable
     * transitions without hooking mmu_write8. */
    u8  ppu_lcd_mode;
    u8  ppu_stat_line;
    u8  ppu_last_lcdc;
    u8  ppu_latched_wy;
    u16 ppu_lcd_count;
    u16 ppu_mode3_cycles;
    u16 ppu_mode0_cycles;
    u32 ppu_lcd_off_count;

    /* Last value of cpu->cycles seen by ppu_tick; the difference between
     * the current cpu->cycles and this is the GB cycles to advance the
     * PPU state machine by. */
    u64 ppu_last_cpu_cycles;

    /* Timer state — DIV (FF04) ticks at 16384 Hz unconditionally; TIMA
     * (FF05) ticks at the rate selected by TAC (FF07) bits 0..1 when
     * bit 2 is set, overflowing into IF.TIMER. Driven from the same
     * cpu->cycles delta as the PPU, accumulated in these counters so we
     * don't have to schedule per-tick callbacks. */
    u64 timer_last_cycles;
    u32 timer_div_acc;
    u32 timer_tima_acc;

    /* Cycle deadline past which ppu_tick MUST run its state machine;
     * until then it can early-return. ppu_tick() runs on every dispatch
     * iteration via sm83_service_interrupts, but most iterations span
     * far fewer cycles than the shortest PPU state transition (80 dots
     * entering mode 3) — those iterations only advance lcd_count
     * without crossing any boundary and skip the heavy work. */
    u64 ppu_next_event_cycles;

    /* Software framebuffer — one byte per pixel, value 0..3 is the GB
     * shade *after* palette translation (0 = white, 3 = black). Filled
     * one scanline at a time at the mode-3 → mode-0 transition in
     * ppu_draw_line(). The display layer (boards/<board>/oled_task.c
     * for the Heltec board) reads this at VBlank.
     *
     * `window_line` is the GB PPU's internal window line counter: it
     * only advances on scanlines where the window is actually drawn,
     * and is reset at the start of every frame. The frame_seq counter
     * is incremented at the LCD_HBLANK → LCD_VBLANK transition so the
     * display task can detect "new frame ready". */
    u8  framebuffer[160 * 144];

    /* Optional second buffer for tear-free OLED reads. Enabled at
     * build time with -DGBJIT_FRAMEBUFFER_DOUBLE_BUFFER=1. When on,
     * ppu_draw_line writes scanlines to framebuffer_back; at the
     * LCD_HBLANK → LCD_VBLANK transition (end of line 143) we memcpy
     * back → front and bump frame_seq, so any reader gating on
     * frame_seq sees only fully-composed frames. Costs ~23 KB DRAM
     * plus a ~50 µs memcpy per frame. Off by default — sync-PPU mode
     * makes the framebuffer race much less visible. */
#ifndef GBJIT_FRAMEBUFFER_DOUBLE_BUFFER
#define GBJIT_FRAMEBUFFER_DOUBLE_BUFFER 0
#endif
#if GBJIT_FRAMEBUFFER_DOUBLE_BUFFER
    u8  framebuffer_back[160 * 144];
#endif

    u8  window_line;
    u32 frame_seq;

    /* Serial output capture — Blargg test ROMs write ASCII to FF01 then $81 to FF02. */
    void (*serial_sink)(void *ctx, u8 byte);
    void *serial_sink_ctx;

    /* Optional frame-complete hook. ppu.c calls this every time the
     * PPU finishes a frame (right after frame_seq is bumped at the
     * mode-0 → mode-1 transition for line 144). NULL by default; the
     * board firmware sets it to a wall-clock pacer to lock the
     * emulation rate to ~60 fps (boards/HTIT-WB32LAF_V3.2/main has
     * the implementation). Host harnesses leave it NULL so unit tests
     * and the differential benches run as fast as the host allows. */
    void (*frame_complete_cb)(struct mmu *m);

    /* Joypad state — active-high bitmask of currently pressed buttons.
     * Bit layout mirrors the Paperboy GB_BTN_* defines:
     *   bit 0 = A,  bit 1 = B,  bit 2 = SELECT, bit 3 = START
     *   bit 4 = RIGHT, bit 5 = LEFT, bit 6 = UP, bit 7 = DOWN
     * Use mmu_set_joypad_state() to update. mmu_read8($FF00) converts
     * to the active-low JOYP format the game expects. */
    u8 joypad_state;

    /* Optional IO read/write callbacks for peripheral emulation (e.g.
     * APU). Called from mmu_read8/mmu_write8 for addresses in the IO
     * region $FF00..$FF7F (excluding $FF00 JOYP which is handled
     * separately). NULL by default (no-op). The read callback receives
     * the current raw io[] byte and may return a modified value; the
     * write callback is called after the io[] store. */
    u8   (*io_read_cb)(void *ctx, u16 addr, u8 val);
    void (*io_write_cb)(void *ctx, u16 addr, u8 val);
    void *io_cb_ctx;
} mmu;

void gb_mmu_init(mmu *m);
void mmu_destroy(mmu *m);   /* free heap-allocated ROM buffer */
bool mmu_load_rom(mmu *m, const u8 *data, size_t len);

/* Set the joypad button state (active-high Paperboy bitmask). */
void mmu_set_joypad_state(mmu *m, u8 state);

u8   mmu_read8 (mmu *m, u16 addr);
void mmu_write8(mmu *m, u16 addr, u8  v);
u16  mmu_read16(mmu *m, u16 addr);
void mmu_write16(mmu *m, u16 addr, u16 v);

#endif
