#ifndef MEMORY_H
#define MEMORY_H

#include "gb_types.h"

/* MMU: up to 256 KB cartridge ROM (enough for most small commercial titles
   like Super Mario Land, Tetris, Dr Mario), 8 KB VRAM, 8 KB WRAM, OAM, IO,
   HRAM. Minimal MBC1 banking: 5-bit low + 2-bit high ROM bank number,
   ROM-mode only (no RAM bank). MBC0 carts are handled as the degenerate
   case (rom_bank stays at 1 and writes to $0000..$7FFF are ignored). */

#define ROM_SIZE   (256u * 1024u)
#define ROM_BANK_SIZE (16u * 1024u)
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
    u8 rom [ROM_SIZE];
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

    /* Back-pointer to the CPU. ppu_tick uses cpu->cycles as its time base
     * and writes back into io[$44]/io[$41]; left NULL the PPU model is
     * inert and reads return the raw IO bytes. */
    struct cpu_state *cpu;

    /* PPU edge-detection state — last-observed scanline (LY) and STAT mode
     * bits, used so ppu_tick() can fire VBlank / STAT IRQs exactly once
     * per transition. */
    u8 ppu_last_ly;
    u8 ppu_last_mode;

    /* Serial output capture — Blargg test ROMs write ASCII to FF01 then $81 to FF02. */
    void (*serial_sink)(void *ctx, u8 byte);
    void *serial_sink_ctx;
} mmu;

void mmu_init(mmu *m);
bool mmu_load_rom(mmu *m, const u8 *data, size_t len);

u8   mmu_read8 (mmu *m, u16 addr);
void mmu_write8(mmu *m, u16 addr, u8  v);
u16  mmu_read16(mmu *m, u16 addr);
void mmu_write16(mmu *m, u16 addr, u16 v);

#endif
