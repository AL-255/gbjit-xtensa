#include "memory.h"
#include "cpu_state.h"
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

/* Bring `struct cpu_state` into scope for the LY fake-read in mmu_read8. */
typedef struct cpu_state cpu_state_t;

/* Cart ROM is heap-allocated so its DRAM footprint matches the actual
 * cart size, not the worst case. On ESP32-S3 we prefer PSRAM if it's
 * available — ROM is read-only and accessed via the JIT helper path,
 * cache-friendly, and moving it out of internal SRAM is what frees up
 * room for the JIT exec arena. */
static void *rom_alloc(size_t bytes) {
#if defined(ESP_PLATFORM)
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p;
#else
    return malloc(bytes);
#endif
}

static void rom_free(void *p) {
#if defined(ESP_PLATFORM)
    if (p) heap_caps_free(p);
#else
    free(p);
#endif
}

void mmu_init(mmu *m) {
    /* Free any previously-allocated heap rom first — otherwise re-init'ing
     * the same mmu (the JIT warm-pass harness does this between passes)
     * leaks the buffer and we eventually exhaust internal SRAM. Safe on
     * first use because static mmu instances are zero-initialised, and
     * rom_free(NULL) is a no-op. */
    rom_free(m->rom);
    memset(m, 0, sizeof(*m));
    m->boot_rom_disabled = 1; /* skip boot ROM — start at $0100 */
    m->mbc = MBC_NONE;
    m->rom_bank = 1;
    m->rom_banks = 2;        /* default: 32 KB single-bank cart */
    /* Allocate a default-sized buffer so callers that write into m->rom
     * before mmu_load_rom (the unit tests do) have somewhere to write. */
    m->rom_capacity = ROM_DEFAULT_BYTES;
    m->rom = (u8 *)rom_alloc(m->rom_capacity);
    if (m->rom) memset(m->rom, 0xFF, m->rom_capacity);
    else        m->rom_capacity = 0;
}

void mmu_destroy(mmu *m) {
    if (!m) return;
    rom_free(m->rom);
    m->rom = NULL;
    m->rom_capacity = 0;
}

bool mmu_load_rom(mmu *m, const u8 *data, size_t len) {
    if (len == 0 || len > ROM_SIZE_MAX) return false;
    /* Round up to a whole 16 KB bank so the high-region access path
     * (rom_bank * ROM_BANK_SIZE + offset) never reads past the buffer. */
    size_t need = (len + ROM_BANK_SIZE - 1u) & ~(size_t)(ROM_BANK_SIZE - 1u);
    if (need > m->rom_capacity) {
        rom_free(m->rom);
        m->rom = (u8 *)rom_alloc(need);
        if (!m->rom) { m->rom_capacity = 0; return false; }
        m->rom_capacity = (u32)need;
    }
    memset(m->rom, 0xFF, m->rom_capacity);
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
        if (m->rom_capacity && off >= m->rom_capacity) off %= m->rom_capacity;
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
        /* LY ($FF44) and STAT ($FF41) are maintained by ppu_tick from
         * cpu->cycles; reads just return the cached IO byte. */
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
