#include "memory.h"
#include "cpu_state.h"
#include "ppu.h"
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

void gb_mmu_init(mmu *m) {
    /* Free any previously-allocated heap rom first — otherwise re-init'ing
     * the same mmu (the JIT warm-pass harness does this between passes)
     * leaks the buffer and we eventually exhaust internal SRAM. Safe on
     * first use because static mmu instances are zero-initialised, and
     * rom_free(NULL) is a no-op. */
    rom_free(m->rom);
    rom_free(m->cart_ram);
    memset(m, 0, sizeof(*m));
    m->boot_rom_disabled = 1; /* skip boot ROM — start at $0100 */
    m->mbc = MBC_NONE;
    /* Post-boot DMG IO defaults (Pan Docs §"Power-Up Sequence"). Most
     * games tolerate zero-init but a few read these regs before writing
     * them. The joypad ($FF00) in particular must read "no buttons,
     * no row selected" or SML's main loop interprets the all-zero
     * default as "all buttons + both rows" which short-circuits its
     * scene/menu logic. */
    m->io[0x00] = 0xCF; /* JOYP — high bits 1 by ISA, low 4 bits = 1 (no key) */
    m->io[0x05] = 0x00; /* TIMA  */
    m->io[0x06] = 0x00; /* TMA   */
    m->io[0x07] = 0x00; /* TAC   */
    m->io[0x40] = 0x91; /* LCDC — post-DMG-boot (LCD on, BG on, win/obj off) */
    m->io[0x41] = 0x85; /* STAT  */
    m->io[0x44] = 0x00; /* LY    */
    m->io[0x47] = 0xFC; /* BGP   — default grey palette */
    m->io[0x48] = 0xFF; /* OBP0  */
    m->io[0x49] = 0xFF; /* OBP1  */
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
    rom_free(m->cart_ram);
    m->cart_ram = NULL;
    m->cart_ram_size = 0;
}

/* External-RAM size from the cart header byte $0149 (Pan Docs §"RAM size"). */
static u32 cart_ram_bytes(u8 code) {
    switch (code) {
        case 0x01: return  2u * 1024u;   /* 2 KB (unofficial)         */
        case 0x02: return  8u * 1024u;   /* 8 KB  — 1 bank            */
        case 0x03: return 32u * 1024u;   /* 32 KB — 4 banks           */
        case 0x04: return 128u * 1024u;  /* 128 KB — 16 banks         */
        case 0x05: return 64u * 1024u;   /* 64 KB — 8 banks           */
        default:   return 0u;
    }
}

/* MBC3 RTC latch: snapshot a deterministic clock derived from elapsed CPU
 * cycles (4.194304 MHz) into the latched registers. Deterministic so the JIT
 * and interpreter — which share this mmu — always read identical values. */
static void mbc3_latch_rtc(mmu *m) {
    u64 cyc = m->cpu ? m->cpu->cycles : 0u;
    u64 secs = cyc / 4194304ull;
    m->rtc[0] = (u8)(secs % 60u);
    m->rtc[1] = (u8)((secs / 60u) % 60u);
    m->rtc[2] = (u8)((secs / 3600u) % 24u);
    u32 days = (u32)(secs / 86400ull);
    m->rtc[3] = (u8)(days & 0xFFu);
    m->rtc[4] = (u8)((days >> 8) & 0x01u);
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
    /* Decode cartridge header (Pan Docs $0147 / $0148 / $0149). */
    u8 cart_type = (len > 0x147u) ? data[0x147] : 0;
    u8 rom_size_code = (len > 0x148u) ? data[0x148] : 0;
    u8 ram_size_code = (len > 0x149u) ? data[0x149] : 0;
    switch (cart_type) {
        case 0x00:                                          /* ROM only */
            m->mbc = MBC_NONE;
            break;
        case 0x01: case 0x02: case 0x03:                    /* MBC1 family */
            m->mbc = MBC_1;
            break;
        case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: /* MBC3 family */
            m->mbc = MBC_3;
            break;
        default:
            /* Treat unknown MBCs as MBC1 for a best effort. */
            m->mbc = MBC_1;
            break;
    }
    /* Battery-backed RAM cart types (save RAM). Recorded for a future
     * persist-to-flash hook; not yet wired. */
    m->has_battery = (cart_type == 0x03 || cart_type == 0x06 ||
                      cart_type == 0x09 || cart_type == 0x0D ||
                      cart_type == 0x0F || cart_type == 0x10 ||
                      cart_type == 0x13) ? 1u : 0u;
    m->rom_banks = (rom_size_code <= 8u) ? (u16)(2u << rom_size_code) : 2u;
    /* Don't trust the header above the file size — clamp. */
    u16 actual_banks = (u16)((len + ROM_BANK_SIZE - 1u) / ROM_BANK_SIZE);
    if (actual_banks < m->rom_banks) m->rom_banks = actual_banks;
    m->rom_bank = 1;

    /* External RAM. MBC2 has 512x4 bits built in (not modelled here); for the
     * MBC1/MBC3 carts we support, allocate per the header RAM-size byte. */
    rom_free(m->cart_ram);
    m->cart_ram = NULL;
    m->cart_ram_size = 0;
    u32 ram_bytes = cart_ram_bytes(ram_size_code);
    if (ram_bytes > 0u) {
        m->cart_ram = (u8 *)rom_alloc(ram_bytes);
        if (m->cart_ram) {
            m->cart_ram_size = ram_bytes;
            memset(m->cart_ram, 0xFFu, ram_bytes);   /* uninit SRAM reads as $FF */
        }
    }
    m->ram_bank = 0;
    m->ram_enable = 0;
    m->rtc_latch = 0;
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
    if (addr < 0xC000u) {                              /* external RAM / RTC */
        if (m->cart_ram && m->ram_enable) {
            if (m->mbc == MBC_3 && m->ram_bank >= 0x08u && m->ram_bank <= 0x0Cu)
                return m->rtc[m->ram_bank - 0x08u];    /* MBC3 RTC register */
            u32 off = ((u32)(m->ram_bank & 0x03u) << 13) + (u32)(addr - 0xA000u);
            if (off < m->cart_ram_size) return m->cart_ram[off];
        }
        return 0xFFu;                                  /* RAM disabled / none */
    }
    if (addr < 0xE000u) return m->wram[addr - 0xC000u];
    if (addr < 0xFE00u) return m->wram[(addr - 0xE000u) & 0x1FFFu];
    if (addr < 0xFEA0u) return m->oam[addr - 0xFE00u];
    if (addr < 0xFF00u) return 0xFFu;
    if (addr < 0xFF80u) {
        u8 io_addr = (u8)(addr - 0xFF00u);
        /* JOYP ($FF00) — Pan Docs §"Joypad Input". The CPU writes bits
         * 4 (P14) and 5 (P15) to select direction or action row; reads
         * return the row-select bits PLUS the 4 button bits of the
         * selected row (0 = pressed). Bits 6,7 are always 1.
         *
         * `buttons` is active-high (1 = pressed) with the Paperboy bit
         * layout. Convert to the active-low JOYP nibble for whichever
         * row(s) are selected. */
        if (io_addr == 0x00) {
            u8 sel      = m->io[0x00];
            u8 action_lo = (u8)(~m->buttons)        & 0x0Fu; /* A,B,Sel,Start */
            u8 dir_lo    = (u8)(~(m->buttons >> 4)) & 0x0Fu; /* R,L,Up,Down */
            u8 lo4 = 0x0Fu;
            if (!(sel & 0x20u)) lo4 &= action_lo; /* P15 low -> action row */
            if (!(sel & 0x10u)) lo4 &= dir_lo;    /* P14 low -> direction row */
            return (u8)((sel & 0x30u) | 0xC0u | lo4);
        }
        /* LY ($FF44) and STAT ($FF41) are maintained by ppu_tick from
         * cpu->cycles; reads just return the cached IO byte. */
        {
            u8 val = m->io[io_addr];
            if (m->io_read_cb)
                val = m->io_read_cb(m->io_cb_ctx, addr, val);
            return val;
        }
    }
    if (addr < 0xFFFFu) return m->hram[addr - 0xFF80u];
    return m->ie;
}

/* Flag a self-modifying-code write. If a JIT block was compiled from
 * the 256-byte page containing `addr` (state 1), promote it to dirty
 * (state 2) and raise the global flag the dispatcher polls. Pages with
 * no compiled code (state 0) cost one load + branch and nothing more —
 * which is every write under the pure interpreter. */
static inline void smc_mark(mmu *m, u16 addr) {
    u32 page = addr >> 8;
    u8 *st = &m->jit_page_state[page];
    if (*st == 0u) return;                       /* no code here — cheapest path */
    /* Only a write that lands inside the page's compiled-code byte range can
     * actually modify a block. A write to the data part of a code+data page
     * (e.g. HRAM variables sharing the OAM-DMA routine's page) is ignored — no
     * flush, no invalidation. */
    if (addr < m->jit_page_code_lo[page] || addr >= m->jit_page_code_hi[page])
        return;
    if (*st == 1u) {
        *st = 2u;
        m->jit_smc_dirty = 1u;
        m->jit_page_wlo[page] = addr;
        m->jit_page_whi[page] = addr;
    } else { /* *st == 2u: already dirty — widen the written-address range */
        if (addr < m->jit_page_wlo[page]) m->jit_page_wlo[page] = addr;
        if (addr > m->jit_page_whi[page]) m->jit_page_whi[page] = addr;
    }
}

void mmu_write8(mmu *m, u16 addr, u8 v) {
    if (addr < 0x8000u) {
        /* MBC3 control registers — see Pan Docs "MBC3". */
        if (m->mbc == MBC_3) {
            if (addr < 0x2000u) {                     /* RAM + timer enable */
                m->ram_enable = ((v & 0x0Fu) == 0x0Au) ? 1u : 0u;
                return;
            }
            if (addr < 0x4000u) {                     /* 7-bit ROM bank */
                u8 b = v & 0x7Fu;
                if (b == 0u) b = 1u;                  /* bank 0 selects 1 */
                if (m->rom_banks && b >= m->rom_banks) b = (u8)(b % m->rom_banks);
                if (b == 0u) b = 1u;
                if (b != m->rom_bank) m->rom_bank_dirty = 1;
                m->rom_bank = b;
                return;
            }
            if (addr < 0x6000u) {                     /* RAM bank (0-3) / RTC sel (8-C) */
                m->ram_bank = v & 0x0Fu;
                return;
            }
            /* $6000..$7FFF: latch clock data — a 0 then 1 latches the RTC. */
            if (m->rtc_latch == 0u && v == 1u) mbc3_latch_rtc(m);
            m->rtc_latch = v;
            return;
        }
        /* MBC1 control register writes — see Pan Docs "Memory Bank
         * Controller 1". For ROM-only carts these are no-ops. */
        if (m->mbc != MBC_1) return;
        if (addr < 0x2000u) {
            /* RAM enable: $0A in the low nibble enables external RAM. */
            m->ram_enable = ((v & 0x0Fu) == 0x0Au) ? 1u : 0u;
            return;
        }
        if (addr < 0x4000u) {
            u8 low5 = v & 0x1Fu;
            if (low5 == 0) low5 = 1;             /* MBC1 quirk */
            u8 next = (u8)((m->rom_bank & 0x60u) | low5);
            if (m->rom_banks) next %= (u8)m->rom_banks;
            if (next != m->rom_bank) m->rom_bank_dirty = 1;
            m->rom_bank = next;
            return;
        }
        if (addr < 0x6000u) {
            u8 hi2 = (u8)((v & 3u) << 5);
            u8 next = (u8)((m->rom_bank & 0x1Fu) | hi2);
            if (m->rom_banks) next %= (u8)m->rom_banks;
            if (next != m->rom_bank) m->rom_bank_dirty = 1;
            m->rom_bank = next;
            return;
        }
        /* $6000..$7FFF: banking mode select. We only model ROM mode (0),
         * which is what almost every small MBC1 cart actually uses. */
        return;
    }
    if (addr < 0xA000u) {
#if GBJIT_PPU_OFFLOAD
        /* Mid-frame VRAM write: render any captured scanlines first, while
         * VRAM still holds the bytes they were captured under. */
        if (gbjit_ppu_have_pending) ppu_offload_flush(m);
#endif
        smc_mark(m, addr); m->vram[addr - 0x8000u] = v; return;
    }
    if (addr < 0xC000u) {                              /* external RAM / RTC */
        if (m->cart_ram && m->ram_enable) {
            if (m->mbc == MBC_3 && m->ram_bank >= 0x08u && m->ram_bank <= 0x0Cu) {
                m->rtc[m->ram_bank - 0x08u] = v;       /* MBC3 RTC register */
                m->cart_ram_dirty = 1u;
                return;
            }
            u32 off = ((u32)(m->ram_bank & 0x03u) << 13) + (u32)(addr - 0xA000u);
            if (off < m->cart_ram_size) {
                /* Only flag dirty on an actual content change: many games re-write
                 * cart RAM with identical bytes every frame, which would otherwise
                 * trigger an endless stream of (flash-stalling) auto-saves. */
                if (m->cart_ram[off] != v) { m->cart_ram[off] = v; m->cart_ram_dirty = 1u; }
            }
        }
        return;
    }
    if (addr < 0xE000u) { smc_mark(m, addr); m->wram[addr - 0xC000u] = v; return; }
    if (addr < 0xFE00u) {
        /* Echo RAM mirrors WRAM $C000..$DDFF — JIT blocks register
         * under the canonical WRAM page, so flag that one. */
        smc_mark(m, (u16)(0xC000u + ((addr - 0xE000u) & 0x1FFFu)));
        m->wram[(addr - 0xE000u) & 0x1FFFu] = v;
        return;
    }
    if (addr < 0xFEA0u) {
#if GBJIT_PPU_OFFLOAD
        if (gbjit_ppu_have_pending) ppu_offload_flush(m);
#endif
        m->oam[addr - 0xFE00u] = v; return;
    }
    if (addr < 0xFF00u) return;
    if (addr < 0xFF80u) {
        u8 io_addr = (u8)(addr - 0xFF00u);
        /* For PPU/timer registers, snap the PPU state machine up to the
         * write cycle before the new value lands. Without this, the next
         * ppu_tick computes its delta from a stale ppu_last_cpu_cycles
         * and re-processes the pre-write cycles in the post-write state
         * — most visibly, an LCDC ON-edge re-runs the OFF-period cycles
         * as if they were in OAM scan, drifting LY/STAT by tens of
         * cycles relative to the reference interpreter (whose per-
         * instruction tick keeps ppu_last_cpu_cycles fresh).
         *
         * Set covers every byte the PPU/timer either reads back into
         * (LCDC/STAT/scrolls/palettes/WY-WX), responds to as an event
         * (DMA), or counts cycles against (DIV/TIMA). FF00/FF02/FF0F
         * are joypad/serial/IF — unrelated to the PPU's cycle accounting
         * and skipped to avoid the timer_tick overhead on every IF read-
         * modify-write the IRQ path emits. */
        if (io_addr == 0x04u || io_addr == 0x05u
                || (io_addr >= 0x40u && io_addr <= 0x4Bu)) {
            ppu_flush(m->cpu);
        }
        /* Timer write quirks (Pan Docs §"Timer Registers"):
         *  - FF04 (DIV): any write resets the visible byte to 0; also
         *    reset the prescaler accumulator so the next tick uses the
         *    fresh baseline. SML uses this to zero DIV between init
         *    passes.
         *  - FF05 (TIMA): a write replaces TIMA and clears the period
         *    accumulator so overflow timing reflects the new value. */
        if (addr == 0xFF04u) { m->io[io_addr] = 0; m->timer_div_acc = 0; return; }
        if (addr == 0xFF05u) { m->io[io_addr] = v; m->timer_tima_acc = 0; return; }
        m->io[io_addr] = v;
        /* LYC ($FF45): the PPU only refreshes the LY=LYC coincidence flag at
         * line transitions, so re-evaluate it now (the ppu_flush above already
         * advanced LY/mode to this cycle). Without this, a raster handler that
         * repoints LYC at the next scanline leaves the old coincidence latched
         * and the STAT IRQ for that line is dropped — see ppu_sync_lyc. */
        if (addr == 0xFF45u) ppu_sync_lyc(m->cpu);
        /* Serial transfer, internal clock (SC $FF02 bit 7 = start, bit 0
         * = internal clock → this GB drives the transfer, so it always
         * completes). With no link cable the 8 bits shift out against an
         * open bus and 0xFF shifts back in. Complete it immediately:
         * clear the start bit, load 0xFF into SB, raise the serial IRQ.
         *
         * This is essential, not cosmetic — a game that starts an
         * internal-clock transfer then busy-waits on SC bit 7 (or HALTs
         * for the serial IRQ) hangs forever otherwise. Tetris's link-
         * cable detection, run right after the copyright screen, does
         * exactly this; without completion it never reaches the title
         * screen. A real cable would take ~4096 cycles, but no
         * commercial game's no-cable path depends on that latency.
         *
         * Slave transfers (bit 0 = 0, external clock) are left pending —
         * with no cable they genuinely never complete, and games time
         * those out themselves. */
        if (addr == 0xFF02u && (v & 0x81u) == 0x81u) {
            /* Blargg's test ROMs print by writing the char to SB then
             * $81 to SC — feed the sink before SB is overwritten. */
            if (m->serial_sink)
                m->serial_sink(m->serial_sink_ctx, m->io[0x01]);
            m->io[0x01] = 0xFFu;                  /* received: open bus */
            m->io[0x02] = (u8)(v & 0x7Fu);        /* clear start bit */
#ifdef GBJIT_PPU_ASYNC
            __atomic_fetch_or(&m->io[0x0F], INT_SERIAL, __ATOMIC_RELAXED);
#else
            m->io[0x0F] |= INT_SERIAL;
#endif
        }
        /* OAM DMA — Pan Docs §"OAM DMA Transfer". Writing $XX to $FF46
         * starts a 160-cycle copy of $XX00..$XX9F into OAM ($FE00..$FE9F).
         * Real hardware bytes the transfer one machine cycle at a time
         * while the CPU continues to run in HRAM; the standard caller
         * uses a `DEC A; JR NZ,-3` wait loop to spin out the 160 cycles.
         * We do the copy synchronously here — the caller's wait loop
         * still consumes the same GB cycles, so observable behaviour is
         * identical for the typical use pattern (and faster than emul-
         * ating the DMA byte-by-byte). */
        if (addr == 0xFF46u) {
#if GBJIT_PPU_OFFLOAD
            /* OAM DMA rewrites all of OAM; flush captured scanlines first. */
            if (gbjit_ppu_have_pending) ppu_offload_flush(m);
#endif
            u16 src = (u16)((u16)v << 8);
            for (u16 i = 0; i < 0xA0u; i++) {
                m->oam[i] = mmu_read8(m, (u16)(src + i));
            }
        }
        if (m->io_write_cb)
            m->io_write_cb(m->io_cb_ctx, addr, v);
        return;
    }
    if (addr < 0xFFFFu) { smc_mark(m, addr); m->hram[addr - 0xFF80u] = v; return; }
    m->ie = v;
}

void mmu_set_joypad_state(mmu *m, u8 state) {
    m->buttons = state;
    m->joypad_state = state;
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
