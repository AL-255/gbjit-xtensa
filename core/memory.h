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

#define ROM_SIZE_MAX (256u * 1024u)
#define ROM_BANK_SIZE (16u * 1024u)
#define ROM_DEFAULT_BYTES (32u * 1024u)  /* allocated by gb_mmu_init */
#define VRAM_SIZE  (8u  * 1024u)
#define WRAM_SIZE  (8u  * 1024u)
#define OAM_SIZE   160u
#define HRAM_SIZE  127u

/* MBC types we care about. */
typedef enum {
    MBC_NONE = 0,
    MBC_1    = 1,
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

    /* Set by mmu_write8 whenever MBC bank-control writes change the bank
     * currently mapped at $4000..$7FFF. The JIT dispatcher polls this
     * flag before entering each block and invalidates any cached blocks
     * compiled from the banked region — without that, a cached block can
     * execute stale immediates / branch targets from the previous bank
     * (SML's main loop diverges at PC=$7FF3 ~668k cycles in). The
     * interpreter ignores this flag (it always re-reads through
     * mmu_read8). */
    u8       rom_bank_dirty;

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

    /* BG/window/sprite render uses a per-frame snapshot of the IO regs
     * it consumes. Without this, async PPU mode (PPU thread on Core 1,
     * dispatcher on Core 0) sees the IO bytes Core 0 wrote at the
     * moment of the read — which can be from a different frame than
     * the one PPU is currently drawing. Latched at the end of VBlank
     * (LY=0 → mode 2) alongside ppu_latched_wy. Sprites read OAM
     * which is filled atomically by the OAM-DMA path so no latch
     * needed there. */
    u8  ppu_latched_lcdc;
    u8  ppu_latched_scx;
    u8  ppu_latched_scy;
    u8  ppu_latched_bgp;
    u8  ppu_latched_obp0;
    u8  ppu_latched_obp1;
    u8  ppu_latched_wx;
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
    u8  window_line;
    u32 frame_seq;

    /* Serial output capture — Blargg test ROMs write ASCII to FF01 then $81 to FF02. */
    void (*serial_sink)(void *ctx, u8 byte);
    void *serial_sink_ctx;
} mmu;

void gb_mmu_init(mmu *m);
void mmu_destroy(mmu *m);   /* free heap-allocated ROM buffer */
bool mmu_load_rom(mmu *m, const u8 *data, size_t len);

u8   mmu_read8 (mmu *m, u16 addr);
void mmu_write8(mmu *m, u16 addr, u8  v);
u16  mmu_read16(mmu *m, u16 addr);
void mmu_write16(mmu *m, u16 addr, u16 v);

#endif
