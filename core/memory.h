#ifndef MEMORY_H
#define MEMORY_H

#include "gb_types.h"

/* Minimal MMU for bring-up: 32 KB ROM (MBC0), 8 KB VRAM, 8 KB WRAM, OAM, IO,
   HRAM. No banking. Enough for Blargg cpu_instrs individual sub-tests
   (32 KB single-bank variants). MBC1 will be slotted in later. */

#define ROM_SIZE   (32u * 1024u)
#define VRAM_SIZE  (8u  * 1024u)
#define WRAM_SIZE  (8u  * 1024u)
#define OAM_SIZE   160u
#define HRAM_SIZE  127u

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
