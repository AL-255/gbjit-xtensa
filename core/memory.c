#include "memory.h"
#include "cpu_state.h"
#include <string.h>

/* Bring `struct cpu_state` into scope for the LY fake-read in mmu_read8. */
typedef struct cpu_state cpu_state_t;

void mmu_init(mmu *m) {
    memset(m, 0, sizeof(*m));
    m->boot_rom_disabled = 1; /* skip boot ROM — start at $0100 */
    m->mbc = MBC_NONE;
    m->rom_bank = 1;
    m->rom_banks = 2;        /* default: 32 KB single-bank cart */
}

bool mmu_load_rom(mmu *m, const u8 *data, size_t len) {
    if (len == 0 || len > ROM_SIZE) return false;
    memcpy(m->rom, data, len);
    /* Decode cartridge header (Pan Docs $0147 / $0148). */
    u8 cart_type = (len > 0x147u) ? data[0x147] : 0;
    u8 rom_size_code = (len > 0x148u) ? data[0x148] : 0;
    switch (cart_type) {
        case 0x00:                                          /* ROM only */
            m->mbc = MBC_NONE;
            break;
        case 0x01: case 0x02: case 0x03:                    /* MBC1 family */
            m->mbc = MBC_1;
            break;
        default:
            /* Treat unknown MBCs as MBC1 for a best effort. */
            m->mbc = MBC_1;
            break;
    }
    m->rom_banks = (rom_size_code <= 8u) ? (u16)(2u << rom_size_code) : 2u;
    /* Don't trust the header above the file size — clamp. */
    u16 actual_banks = (u16)((len + ROM_BANK_SIZE - 1u) / ROM_BANK_SIZE);
    if (actual_banks < m->rom_banks) m->rom_banks = actual_banks;
    m->rom_bank = 1;
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
    if (addr < 0x4000u) return m->rom[addr];           /* bank 0 fixed */
    if (addr < 0x8000u) {                              /* banked region */
        u32 off = ((u32)m->rom_bank * ROM_BANK_SIZE) + (addr - 0x4000u);
        if (off >= ROM_SIZE) off &= (ROM_SIZE - 1u);   /* wrap for safety */
        return m->rom[off];
    }
    if (addr < 0xA000u) return m->vram[addr - 0x8000u];
    if (addr < 0xC000u) return 0xFFu; /* no external RAM */
    if (addr < 0xE000u) return m->wram[addr - 0xC000u];
    if (addr < 0xFE00u) return m->wram[(addr - 0xE000u) & 0x1FFFu];
    if (addr < 0xFEA0u) return m->oam[addr - 0xFE00u];
    if (addr < 0xFF00u) return 0xFFu;
    if (addr < 0xFF80u) {
        u8 io_addr = (u8)(addr - 0xFF00u);
        /* FF44 = LY (current PPU scanline). We don't model the PPU, so
         * fake the scanline counter using cpu->cycles to keep VBlank-wait
         * loops progressing through 0..153 like real hardware. */
        if (addr == 0xFF44u && m->cpu) {
            return (u8)((m->cpu->cycles / 456u) % 154u);
        }
        return m->io[io_addr];
    }
    if (addr < 0xFFFFu) return m->hram[addr - 0xFF80u];
    return m->ie;
}

void mmu_write8(mmu *m, u16 addr, u8 v) {
    if (addr < 0x8000u) {
        /* MBC1 control register writes — see Pan Docs "Memory Bank
         * Controller 1". For ROM-only carts these are no-ops. */
        if (m->mbc != MBC_1) return;
        if (addr < 0x2000u) {
            /* RAM enable (we don't model RAM yet) — ignore. */
            return;
        }
        if (addr < 0x4000u) {
            u8 low5 = v & 0x1Fu;
            if (low5 == 0) low5 = 1;             /* MBC1 quirk */
            u8 next = (u8)((m->rom_bank & 0x60u) | low5);
            if (m->rom_banks) next %= (u8)m->rom_banks;
            m->rom_bank = next;
            return;
        }
        if (addr < 0x6000u) {
            u8 hi2 = (u8)((v & 3u) << 5);
            u8 next = (u8)((m->rom_bank & 0x1Fu) | hi2);
            if (m->rom_banks) next %= (u8)m->rom_banks;
            m->rom_bank = next;
            return;
        }
        /* $6000..$7FFF: banking mode select. We only model ROM mode (0),
         * which is what almost every small MBC1 cart actually uses. */
        return;
    }
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
