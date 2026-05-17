#include "memory.h"
#include "cpu_state.h"
#include <string.h>

void mmu_init(mmu *m) {
    memset(m, 0, sizeof(*m));
    m->boot_rom_disabled = 1; /* skip boot ROM — start at $0100 */
}

bool mmu_load_rom(mmu *m, const u8 *data, size_t len) {
    if (len == 0 || len > ROM_SIZE) return false;
    memcpy(m->rom, data, len);
    return true;
}

/* Region decode. The DMG memory map:
   0000-3FFF  ROM bank 0
   4000-7FFF  ROM bank N (we serve bank 0 since MBC0)
   8000-9FFF  VRAM
   A000-BFFF  External RAM (none in MBC0)
   C000-DFFF  WRAM
   E000-FDFF  Echo of C000-DDFF
   FE00-FE9F  OAM
   FEA0-FEFF  Prohibited (returns FF)
   FF00-FF7F  IO
   FF80-FFFE  HRAM
   FFFF       IE
*/

u8 mmu_read8(mmu *m, u16 addr) {
    if (addr < 0x8000u) return m->rom[addr & 0x7FFFu];
    if (addr < 0xA000u) return m->vram[addr - 0x8000u];
    if (addr < 0xC000u) return 0xFFu; /* no external RAM */
    if (addr < 0xE000u) return m->wram[addr - 0xC000u];
    if (addr < 0xFE00u) return m->wram[(addr - 0xE000u) & 0x1FFFu];
    if (addr < 0xFEA0u) return m->oam[addr - 0xFE00u];
    if (addr < 0xFF00u) return 0xFFu;
    if (addr < 0xFF80u) {
        u8 io_addr = (u8)(addr - 0xFF00u);
        return m->io[io_addr];
    }
    if (addr < 0xFFFFu) return m->hram[addr - 0xFF80u];
    return m->ie;
}

void mmu_write8(mmu *m, u16 addr, u8 v) {
    if (addr < 0x8000u) return; /* ROM is read-only in MBC0 */
    if (addr < 0xA000u) { m->vram[addr - 0x8000u] = v; return; }
    if (addr < 0xC000u) return;
    if (addr < 0xE000u) { m->wram[addr - 0xC000u] = v; return; }
    if (addr < 0xFE00u) { m->wram[(addr - 0xE000u) & 0x1FFFu] = v; return; }
    if (addr < 0xFEA0u) { m->oam[addr - 0xFE00u] = v; return; }
    if (addr < 0xFF00u) return;
    if (addr < 0xFF80u) {
        u8 io_addr = (u8)(addr - 0xFF00u);
        m->io[io_addr] = v;
        /* Blargg serial trap: writing $81 to FF02 transmits FF01. */
        if (addr == 0xFF02u && v == 0x81u && m->serial_sink) {
            m->serial_sink(m->serial_sink_ctx, m->io[0x01]);
            /* Self-clear the transfer-start bit. */
            m->io[0x02] = (u8)(v & 0x7Fu);
            /* Also request a serial interrupt for completeness. */
            m->io[0x0F] |= INT_SERIAL;
        }
        return;
    }
    if (addr < 0xFFFFu) { m->hram[addr - 0xFF80u] = v; return; }
    m->ie = v;
}

u16 mmu_read16(mmu *m, u16 addr) {
    u8 lo = mmu_read8(m, addr);
    u8 hi = mmu_read8(m, (u16)(addr + 1));
    return (u16)(lo | (hi << 8));
}

void mmu_write16(mmu *m, u16 addr, u16 v) {
    mmu_write8(m, addr,            (u8)(v & 0xFFu));
    mmu_write8(m, (u16)(addr + 1), (u8)(v >> 8));
}
