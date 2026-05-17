#include "sm83_interp.h"
#include "sm83_decoder.h"
#include "memory.h"
#include "ppu.h"

/* --- Fetch helpers --- */
static inline u8 fetch8(cpu_state *cpu) {
    u8 v = mmu_read8(cpu->mmu, cpu->pc);
    cpu->pc = (u16)(cpu->pc + 1);
    return v;
}
static inline u16 fetch16(cpu_state *cpu) {
    u8 lo = fetch8(cpu);
    u8 hi = fetch8(cpu);
    return (u16)(lo | (hi << 8));
}

/* --- Flag helpers --- */
static inline void set_flags(cpu_state *cpu, bool z, bool n, bool h, bool c) {
    u8 f = 0;
    if (z) f |= FLAG_Z;
    if (n) f |= FLAG_N;
    if (h) f |= FLAG_H;
    if (c) f |= FLAG_C;
    cpu->f = f;
}
static inline void set_zn0h(cpu_state *cpu, u8 v) {
    /* Some ops set N=0, leave C unchanged. */
    cpu->f = (cpu->f & FLAG_C) | (v == 0 ? FLAG_Z : 0);
}

/* --- Stack helpers --- */
static inline void push16(cpu_state *cpu, u16 v) {
    cpu->sp = (u16)(cpu->sp - 2);
    mmu_write16(cpu->mmu, cpu->sp, v);
}
static inline u16 pop16(cpu_state *cpu) {
    u16 v = mmu_read16(cpu->mmu, cpu->sp);
    cpu->sp = (u16)(cpu->sp + 2);
    return v;
}

/* --- ALU --- */
static inline void alu_add(cpu_state *cpu, u8 v) {
    u16 r = (u16)cpu->a + v;
    bool h = ((cpu->a & 0xF) + (v & 0xF)) > 0xF;
    cpu->a = (u8)r;
    set_flags(cpu, cpu->a == 0, false, h, r > 0xFF);
}
static inline void alu_adc(cpu_state *cpu, u8 v) {
    u8 c = (cpu->f & FLAG_C) ? 1 : 0;
    u16 r = (u16)cpu->a + v + c;
    bool h = ((cpu->a & 0xF) + (v & 0xF) + c) > 0xF;
    cpu->a = (u8)r;
    set_flags(cpu, cpu->a == 0, false, h, r > 0xFF);
}
static inline void alu_sub(cpu_state *cpu, u8 v) {
    i16 r = (i16)cpu->a - v;
    bool h = ((cpu->a & 0xF) - (v & 0xF)) < 0;
    cpu->a = (u8)r;
    set_flags(cpu, cpu->a == 0, true, h, r < 0);
}
static inline void alu_sbc(cpu_state *cpu, u8 v) {
    u8 c = (cpu->f & FLAG_C) ? 1 : 0;
    i16 r = (i16)cpu->a - v - c;
    bool h = ((cpu->a & 0xF) - (v & 0xF) - c) < 0;
    cpu->a = (u8)r;
    set_flags(cpu, cpu->a == 0, true, h, r < 0);
}
static inline void alu_and(cpu_state *cpu, u8 v) {
    cpu->a &= v;
    set_flags(cpu, cpu->a == 0, false, true, false);
}
static inline void alu_xor(cpu_state *cpu, u8 v) {
    cpu->a ^= v;
    set_flags(cpu, cpu->a == 0, false, false, false);
}
static inline void alu_or(cpu_state *cpu, u8 v) {
    cpu->a |= v;
    set_flags(cpu, cpu->a == 0, false, false, false);
}
static inline void alu_cp(cpu_state *cpu, u8 v) {
    i16 r = (i16)cpu->a - v;
    bool h = ((cpu->a & 0xF) - (v & 0xF)) < 0;
    set_flags(cpu, ((u8)r) == 0, true, h, r < 0);
}

static inline u8 alu_inc8(cpu_state *cpu, u8 v) {
    u8 r = (u8)(v + 1);
    cpu->f = (cpu->f & FLAG_C)
           | (r == 0 ? FLAG_Z : 0)
           | (((v & 0xF) + 1 > 0xF) ? FLAG_H : 0);
    return r;
}
static inline u8 alu_dec8(cpu_state *cpu, u8 v) {
    u8 r = (u8)(v - 1);
    cpu->f = (cpu->f & FLAG_C)
           | (r == 0 ? FLAG_Z : 0)
           | FLAG_N
           | (((v & 0xF) == 0) ? FLAG_H : 0);
    return r;
}

static inline void alu_add_hl(cpu_state *cpu, u16 v) {
    u32 r = (u32)cpu->hl + v;
    bool h = ((cpu->hl & 0x0FFFu) + (v & 0x0FFFu)) > 0x0FFFu;
    bool c = r > 0xFFFFu;
    cpu->f = (cpu->f & FLAG_Z) | (h ? FLAG_H : 0) | (c ? FLAG_C : 0);
    cpu->hl = (u16)r;
}

static inline u16 alu_add_sp_r8(cpu_state *cpu, i8 r8) {
    u16 sp = cpu->sp;
    u16 v = (u16)r8;
    u16 r = (u16)(sp + v);
    /* Flags computed on the low 8 bits, treating r8 as unsigned for the carry test. */
    bool h = ((sp & 0x0F) + (v & 0x0F)) > 0x0F;
    bool c = ((sp & 0xFF) + (v & 0xFF)) > 0xFF;
    set_flags(cpu, false, false, h, c);
    return r;
}

/* --- Rotates / shifts (used by RLCA/RRCA/RLA/RRA and CB-prefixed) --- */
static inline u8 op_rlc(cpu_state *cpu, u8 v) {
    u8 c = (v >> 7) & 1;
    u8 r = (u8)((v << 1) | c);
    set_flags(cpu, r == 0, false, false, c);
    return r;
}
static inline u8 op_rrc(cpu_state *cpu, u8 v) {
    u8 c = v & 1;
    u8 r = (u8)((v >> 1) | (c << 7));
    set_flags(cpu, r == 0, false, false, c);
    return r;
}
static inline u8 op_rl(cpu_state *cpu, u8 v) {
    u8 c_in = (cpu->f & FLAG_C) ? 1 : 0;
    u8 c_out = (v >> 7) & 1;
    u8 r = (u8)((v << 1) | c_in);
    set_flags(cpu, r == 0, false, false, c_out);
    return r;
}
static inline u8 op_rr(cpu_state *cpu, u8 v) {
    u8 c_in = (cpu->f & FLAG_C) ? 1 : 0;
    u8 c_out = v & 1;
    u8 r = (u8)((v >> 1) | (c_in << 7));
    set_flags(cpu, r == 0, false, false, c_out);
    return r;
}
static inline u8 op_sla(cpu_state *cpu, u8 v) {
    u8 c = (v >> 7) & 1;
    u8 r = (u8)(v << 1);
    set_flags(cpu, r == 0, false, false, c);
    return r;
}
static inline u8 op_sra(cpu_state *cpu, u8 v) {
    u8 c = v & 1;
    u8 r = (u8)((v >> 1) | (v & 0x80));
    set_flags(cpu, r == 0, false, false, c);
    return r;
}
static inline u8 op_srl(cpu_state *cpu, u8 v) {
    u8 c = v & 1;
    u8 r = (u8)(v >> 1);
    set_flags(cpu, r == 0, false, false, c);
    return r;
}
static inline u8 op_swap(cpu_state *cpu, u8 v) {
    u8 r = (u8)((v >> 4) | (v << 4));
    set_flags(cpu, r == 0, false, false, false);
    return r;
}
static inline void op_bit(cpu_state *cpu, u8 bit, u8 v) {
    bool z = ((v >> bit) & 1) == 0;
    cpu->f = (cpu->f & FLAG_C)
           | (z ? FLAG_Z : 0)
           | FLAG_H;
}

/* DAA — Binary-coded-decimal correction after add/sub. The canonical algorithm. */
static inline void op_daa(cpu_state *cpu) {
    u8 a = cpu->a;
    bool n = (cpu->f & FLAG_N) != 0;
    bool h = (cpu->f & FLAG_H) != 0;
    bool c = (cpu->f & FLAG_C) != 0;
    if (!n) {
        if (c || a > 0x99) { a = (u8)(a + 0x60); c = true; }
        if (h || (a & 0x0F) > 0x09) { a = (u8)(a + 0x06); }
    } else {
        if (c) a = (u8)(a - 0x60);
        if (h) a = (u8)(a - 0x06);
    }
    cpu->a = a;
    cpu->f = (cpu->f & FLAG_N)
           | (a == 0 ? FLAG_Z : 0)
           | (c ? FLAG_C : 0);
}

/* --- Interrupts --- */
static const u16 int_vector[5] = { 0x40, 0x48, 0x50, 0x58, 0x60 };

u32 sm83_service_interrupts(cpu_state *cpu) {
    /* Drive the PPU timing model forward — it reads cpu->cycles and may
     * raise IF bits ahead of the dispatch check below. */
    ppu_tick(cpu);
    /* Refresh cached IF/IE from MMU IO + IE byte. */
    cpu->if_reg = cpu->mmu->io[0x0F];
    cpu->ie_reg = cpu->mmu->ie;
    u8 pending = (u8)(cpu->if_reg & cpu->ie_reg & 0x1F);
    if (!pending) return 0;
    if (cpu->halted) cpu->halted = 0; /* wake from HALT regardless of IME */
    if (!cpu->ime) return 0;
    cpu->ime = 0;
    /* Find lowest-set bit. */
    for (int i = 0; i < 5; i++) {
        u8 mask = (u8)(1u << i);
        if (pending & mask) {
            cpu->mmu->io[0x0F] = (u8)(cpu->if_reg & ~mask);
            push16(cpu, cpu->pc);
            cpu->pc = int_vector[i];
            cpu->cycles += 20;
            return 20;
        }
    }
    return 0;
}

/* --- 8-bit register access by encoding r ∈ {B,C,D,E,H,L,(HL),A} indices 0..7 --- */
static inline u8 reg8_read(cpu_state *cpu, u8 r) {
    switch (r & 7) {
        case 0: return cpu->b;
        case 1: return cpu->c;
        case 2: return cpu->d;
        case 3: return cpu->e;
        case 4: return cpu->h;
        case 5: return cpu->l;
        case 6: return mmu_read8(cpu->mmu, cpu->hl);
        default: return cpu->a;
    }
}
static inline void reg8_write(cpu_state *cpu, u8 r, u8 v) {
    switch (r & 7) {
        case 0: cpu->b = v; break;
        case 1: cpu->c = v; break;
        case 2: cpu->d = v; break;
        case 3: cpu->e = v; break;
        case 4: cpu->h = v; break;
        case 5: cpu->l = v; break;
        case 6: mmu_write8(cpu->mmu, cpu->hl, v); break;
        default: cpu->a = v; break;
    }
}

/* CB-prefix opcode handler. cycles passed back via *extra_cycles. */
static u32 exec_cb(cpu_state *cpu) {
    u8 op = fetch8(cpu);
    u8 reg = op & 7;
    u8 grp = (op >> 6) & 3;     /* 0 = shift/rotate, 1 = BIT, 2 = RES, 3 = SET */
    u8 sub = (op >> 3) & 7;     /* within group */
    u8 v = reg8_read(cpu, reg);
    u32 extra = (reg == 6) ? 16 : 8;
    u8 r;
    switch (grp) {
        case 0:
            switch (sub) {
                case 0: r = op_rlc(cpu, v); break;
                case 1: r = op_rrc(cpu, v); break;
                case 2: r = op_rl(cpu, v); break;
                case 3: r = op_rr(cpu, v); break;
                case 4: r = op_sla(cpu, v); break;
                case 5: r = op_sra(cpu, v); break;
                case 6: r = op_swap(cpu, v); break;
                default: r = op_srl(cpu, v); break;
            }
            reg8_write(cpu, reg, r);
            break;
        case 1: /* BIT */
            op_bit(cpu, sub, v);
            extra = (reg == 6) ? 12 : 8;
            break;
        case 2: /* RES */
            reg8_write(cpu, reg, (u8)(v & ~(1u << sub)));
            break;
        case 3: /* SET */
            reg8_write(cpu, reg, (u8)(v | (1u << sub)));
            break;
    }
    return extra;
}

/* --- Condition codes for conditional branches --- */
static inline bool cond(cpu_state *cpu, u8 cc) {
    switch (cc & 3) {
        case 0: return (cpu->f & FLAG_Z) == 0;  /* NZ */
        case 1: return (cpu->f & FLAG_Z) != 0;  /* Z  */
        case 2: return (cpu->f & FLAG_C) == 0;  /* NC */
        default: return (cpu->f & FLAG_C) != 0; /* C  */
    }
}

u32 sm83_step(cpu_state *cpu) {
    /* EI takes effect after the next instruction. */
    bool ei_pending_clear = (cpu->ime_pending != 0);

    if (sm83_service_interrupts(cpu)) {
        if (ei_pending_clear) { cpu->ime = 1; cpu->ime_pending = 0; }
        return 20;
    }

    if (cpu->halted) {
        cpu->cycles += 4;
        if (ei_pending_clear) { cpu->ime = 1; cpu->ime_pending = 0; }
        return 4;
    }

    u8 op = fetch8(cpu);
    u32 cycles = sm83_decode(op)->cycles_notaken;
    bool branch_taken = false;

    switch (op) {
        case 0x00: break; /* NOP */
        case 0x01: cpu->bc = fetch16(cpu); break;
        case 0x02: mmu_write8(cpu->mmu, cpu->bc, cpu->a); break;
        case 0x03: cpu->bc = (u16)(cpu->bc + 1); break;
        case 0x04: cpu->b = alu_inc8(cpu, cpu->b); break;
        case 0x05: cpu->b = alu_dec8(cpu, cpu->b); break;
        case 0x06: cpu->b = fetch8(cpu); break;
        case 0x07: { /* RLCA */
            u8 c = (cpu->a >> 7) & 1;
            cpu->a = (u8)((cpu->a << 1) | c);
            set_flags(cpu, false, false, false, c);
            break;
        }
        case 0x08: { /* LD (a16),SP */
            u16 a = fetch16(cpu);
            mmu_write16(cpu->mmu, a, cpu->sp);
            break;
        }
        case 0x09: alu_add_hl(cpu, cpu->bc); break;
        case 0x0A: cpu->a = mmu_read8(cpu->mmu, cpu->bc); break;
        case 0x0B: cpu->bc = (u16)(cpu->bc - 1); break;
        case 0x0C: cpu->c = alu_inc8(cpu, cpu->c); break;
        case 0x0D: cpu->c = alu_dec8(cpu, cpu->c); break;
        case 0x0E: cpu->c = fetch8(cpu); break;
        case 0x0F: { /* RRCA */
            u8 c = cpu->a & 1;
            cpu->a = (u8)((cpu->a >> 1) | (c << 7));
            set_flags(cpu, false, false, false, c);
            break;
        }

        case 0x10: /* STOP */
            cpu->stopped = 1;
            fetch8(cpu); /* second byte */
            break;
        case 0x11: cpu->de = fetch16(cpu); break;
        case 0x12: mmu_write8(cpu->mmu, cpu->de, cpu->a); break;
        case 0x13: cpu->de = (u16)(cpu->de + 1); break;
        case 0x14: cpu->d = alu_inc8(cpu, cpu->d); break;
        case 0x15: cpu->d = alu_dec8(cpu, cpu->d); break;
        case 0x16: cpu->d = fetch8(cpu); break;
        case 0x17: { /* RLA */
            u8 c_in = (cpu->f & FLAG_C) ? 1 : 0;
            u8 c_out = (cpu->a >> 7) & 1;
            cpu->a = (u8)((cpu->a << 1) | c_in);
            set_flags(cpu, false, false, false, c_out);
            break;
        }
        case 0x18: { /* JR r8 */
            i8 off = (i8)fetch8(cpu);
            cpu->pc = (u16)(cpu->pc + off);
            break;
        }
        case 0x19: alu_add_hl(cpu, cpu->de); break;
        case 0x1A: cpu->a = mmu_read8(cpu->mmu, cpu->de); break;
        case 0x1B: cpu->de = (u16)(cpu->de - 1); break;
        case 0x1C: cpu->e = alu_inc8(cpu, cpu->e); break;
        case 0x1D: cpu->e = alu_dec8(cpu, cpu->e); break;
        case 0x1E: cpu->e = fetch8(cpu); break;
        case 0x1F: { /* RRA */
            u8 c_in = (cpu->f & FLAG_C) ? 1 : 0;
            u8 c_out = cpu->a & 1;
            cpu->a = (u8)((cpu->a >> 1) | (c_in << 7));
            set_flags(cpu, false, false, false, c_out);
            break;
        }

        case 0x20: case 0x28: case 0x30: case 0x38: { /* JR cc,r8 */
            i8 off = (i8)fetch8(cpu);
            u8 cc = (op >> 3) & 3;
            if (cond(cpu, cc)) { cpu->pc = (u16)(cpu->pc + off); branch_taken = true; }
            break;
        }
        case 0x21: cpu->hl = fetch16(cpu); break;
        case 0x22: mmu_write8(cpu->mmu, cpu->hl, cpu->a); cpu->hl = (u16)(cpu->hl + 1); break;
        case 0x23: cpu->hl = (u16)(cpu->hl + 1); break;
        case 0x24: cpu->h = alu_inc8(cpu, cpu->h); break;
        case 0x25: cpu->h = alu_dec8(cpu, cpu->h); break;
        case 0x26: cpu->h = fetch8(cpu); break;
        case 0x27: op_daa(cpu); break;
        case 0x29: alu_add_hl(cpu, cpu->hl); break;
        case 0x2A: cpu->a = mmu_read8(cpu->mmu, cpu->hl); cpu->hl = (u16)(cpu->hl + 1); break;
        case 0x2B: cpu->hl = (u16)(cpu->hl - 1); break;
        case 0x2C: cpu->l = alu_inc8(cpu, cpu->l); break;
        case 0x2D: cpu->l = alu_dec8(cpu, cpu->l); break;
        case 0x2E: cpu->l = fetch8(cpu); break;
        case 0x2F: cpu->a = (u8)~cpu->a; cpu->f |= FLAG_N | FLAG_H; break; /* CPL */

        case 0x31: cpu->sp = fetch16(cpu); break;
        case 0x32: mmu_write8(cpu->mmu, cpu->hl, cpu->a); cpu->hl = (u16)(cpu->hl - 1); break;
        case 0x33: cpu->sp = (u16)(cpu->sp + 1); break;
        case 0x34: { /* INC (HL) */
            u8 v = mmu_read8(cpu->mmu, cpu->hl);
            mmu_write8(cpu->mmu, cpu->hl, alu_inc8(cpu, v));
            break;
        }
        case 0x35: { /* DEC (HL) */
            u8 v = mmu_read8(cpu->mmu, cpu->hl);
            mmu_write8(cpu->mmu, cpu->hl, alu_dec8(cpu, v));
            break;
        }
        case 0x36: mmu_write8(cpu->mmu, cpu->hl, fetch8(cpu)); break;
        case 0x37: /* SCF */
            cpu->f = (cpu->f & FLAG_Z) | FLAG_C;
            break;
        case 0x39: alu_add_hl(cpu, cpu->sp); break;
        case 0x3A: cpu->a = mmu_read8(cpu->mmu, cpu->hl); cpu->hl = (u16)(cpu->hl - 1); break;
        case 0x3B: cpu->sp = (u16)(cpu->sp - 1); break;
        case 0x3C: cpu->a = alu_inc8(cpu, cpu->a); break;
        case 0x3D: cpu->a = alu_dec8(cpu, cpu->a); break;
        case 0x3E: cpu->a = fetch8(cpu); break;
        case 0x3F: /* CCF */
            cpu->f = (cpu->f & FLAG_Z) | (((cpu->f & FLAG_C) ^ FLAG_C));
            break;

        case 0x76: cpu->halted = 1; break; /* HALT */

        /* 0x40..0x7F LD r,r' (with 0x76 = HALT carved out). */
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
        case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75:           case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            u8 dst = (op >> 3) & 7;
            u8 src = op & 7;
            reg8_write(cpu, dst, reg8_read(cpu, src));
            break;
        }

        /* 0x80..0xBF ALU A,r */
        case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
            alu_add(cpu, reg8_read(cpu, op & 7)); break;
        case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F:
            alu_adc(cpu, reg8_read(cpu, op & 7)); break;
        case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
            alu_sub(cpu, reg8_read(cpu, op & 7)); break;
        case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
            alu_sbc(cpu, reg8_read(cpu, op & 7)); break;
        case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: case 0xA6: case 0xA7:
            alu_and(cpu, reg8_read(cpu, op & 7)); break;
        case 0xA8: case 0xA9: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
            alu_xor(cpu, reg8_read(cpu, op & 7)); break;
        case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            alu_or(cpu, reg8_read(cpu, op & 7)); break;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            alu_cp(cpu, reg8_read(cpu, op & 7)); break;

        case 0xC0: case 0xC8: case 0xD0: case 0xD8: { /* RET cc */
            u8 cc = (op >> 3) & 3;
            if (cond(cpu, cc)) { cpu->pc = pop16(cpu); branch_taken = true; }
            break;
        }
        case 0xC1: cpu->bc = pop16(cpu); break;
        case 0xC2: case 0xCA: case 0xD2: case 0xDA: { /* JP cc,a16 */
            u16 a = fetch16(cpu);
            u8 cc = (op >> 3) & 3;
            if (cond(cpu, cc)) { cpu->pc = a; branch_taken = true; }
            break;
        }
        case 0xC3: cpu->pc = fetch16(cpu); break; /* JP a16 */
        case 0xC4: case 0xCC: case 0xD4: case 0xDC: { /* CALL cc,a16 */
            u16 a = fetch16(cpu);
            u8 cc = (op >> 3) & 3;
            if (cond(cpu, cc)) { push16(cpu, cpu->pc); cpu->pc = a; branch_taken = true; }
            break;
        }
        case 0xC5: push16(cpu, cpu->bc); break;
        case 0xC6: alu_add(cpu, fetch8(cpu)); break;
        case 0xC7: case 0xCF: case 0xD7: case 0xDF:
        case 0xE7: case 0xEF: case 0xF7: case 0xFF: { /* RST n */
            push16(cpu, cpu->pc);
            cpu->pc = (u16)(op & 0x38);
            break;
        }
        case 0xC9: cpu->pc = pop16(cpu); break; /* RET */
        case 0xCB: cycles = exec_cb(cpu); break;
        case 0xCD: { /* CALL a16 */
            u16 a = fetch16(cpu);
            push16(cpu, cpu->pc);
            cpu->pc = a;
            break;
        }
        case 0xCE: alu_adc(cpu, fetch8(cpu)); break;
        case 0xD1: cpu->de = pop16(cpu); break;
        case 0xD5: push16(cpu, cpu->de); break;
        case 0xD6: alu_sub(cpu, fetch8(cpu)); break;
        case 0xD9: cpu->pc = pop16(cpu); cpu->ime = 1; break; /* RETI */
        case 0xDE: alu_sbc(cpu, fetch8(cpu)); break;

        case 0xE0: mmu_write8(cpu->mmu, (u16)(0xFF00u + fetch8(cpu)), cpu->a); break;
        case 0xE1: cpu->hl = pop16(cpu); break;
        case 0xE2: mmu_write8(cpu->mmu, (u16)(0xFF00u + cpu->c), cpu->a); break;
        case 0xE5: push16(cpu, cpu->hl); break;
        case 0xE6: alu_and(cpu, fetch8(cpu)); break;
        case 0xE8: cpu->sp = alu_add_sp_r8(cpu, (i8)fetch8(cpu)); break;
        case 0xE9: cpu->pc = cpu->hl; break; /* JP (HL) */
        case 0xEA: { u16 a = fetch16(cpu); mmu_write8(cpu->mmu, a, cpu->a); break; }
        case 0xEE: alu_xor(cpu, fetch8(cpu)); break;

        case 0xF0: cpu->a = mmu_read8(cpu->mmu, (u16)(0xFF00u + fetch8(cpu))); break;
        case 0xF1: cpu->af = pop16(cpu); cpu->f &= 0xF0u; break;
        case 0xF2: cpu->a = mmu_read8(cpu->mmu, (u16)(0xFF00u + cpu->c)); break;
        case 0xF3: cpu->ime = 0; cpu->ime_pending = 0; break; /* DI */
        case 0xF5: push16(cpu, (u16)(cpu->af & 0xFFF0u)); break;
        case 0xF6: alu_or(cpu, fetch8(cpu)); break;
        case 0xF8: { /* LD HL,SP+r8 */
            i8 off = (i8)fetch8(cpu);
            cpu->hl = alu_add_sp_r8(cpu, off);
            break;
        }
        case 0xF9: cpu->sp = cpu->hl; break;
        case 0xFA: { u16 a = fetch16(cpu); cpu->a = mmu_read8(cpu->mmu, a); break; }
        case 0xFB: cpu->ime_pending = 1; break; /* EI */
        case 0xFE: alu_cp(cpu, fetch8(cpu)); break;

        default:
            /* Illegal opcode — treat as NOP for now; real hw locks up. */
            break;
    }

    if (branch_taken) cycles = sm83_decode(op)->cycles_taken;
    cpu->cycles += cycles;

    if (ei_pending_clear) { cpu->ime = 1; cpu->ime_pending = 0; }
    return cycles;
}

u64 sm83_run_until(cpu_state *cpu, u64 until) {
    u64 start = cpu->cycles;
    while (cpu->cycles < until) sm83_step(cpu);
    return cpu->cycles - start;
}

void cpu_reset(cpu_state *cpu, struct mmu *m) {
    /* Post-boot DMG state. */
    cpu->a = 0x01; cpu->f = 0xB0;
    cpu->b = 0x00; cpu->c = 0x13;
    cpu->d = 0x00; cpu->e = 0xD8;
    cpu->h = 0x01; cpu->l = 0x4D;
    cpu->sp = 0xFFFE;
    cpu->pc = 0x0100;
    cpu->ime = 0; cpu->ime_pending = 0;
    cpu->halted = 0; cpu->stopped = 0;
    cpu->cycles = 0;
    cpu->mmu = m;
    if (m) m->cpu = cpu;
}
