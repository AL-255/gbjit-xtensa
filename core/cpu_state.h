#ifndef CPU_STATE_H
#define CPU_STATE_H

#include "gb_types.h"

/* SM83 flag bits in F. */
#define FLAG_Z 0x80u
#define FLAG_N 0x40u
#define FLAG_H 0x20u
#define FLAG_C 0x10u

/* Interrupt sources (IF/IE bits). */
#define INT_VBLANK  0x01u
#define INT_LCD     0x02u
#define INT_TIMER   0x04u
#define INT_SERIAL  0x08u
#define INT_JOYPAD  0x10u

struct mmu;

typedef struct cpu_state {
    /* Guest 8-bit registers. Laid out as pairs in memory order suitable for
       16-bit aliased access on a little-endian host: AF, BC, DE, HL. */
    union { struct { u8 f, a; }; u16 af; };
    union { struct { u8 c, b; }; u16 bc; };
    union { struct { u8 e, d; }; u16 de; };
    union { struct { u8 l, h; }; u16 hl; };
    u16 sp;
    u16 pc;

    /* Interrupt master enable + delayed enable (EI takes effect after next op). */
    u8 ime;
    u8 ime_pending;

    /* HALT/STOP state. */
    u8 halted;
    u8 stopped;

    /* Cycle accumulator (T-cycles since boot or reset). */
    u64 cycles;

    /* Interrupt flag/enable (mirrors of IF/IE memory regs; the MMU still owns
       the canonical bytes but caches them here for fast checks). */
    u8 if_reg;
    u8 ie_reg;

    /* MMU back-pointer. */
    struct mmu *mmu;
} cpu_state;

void cpu_reset(cpu_state *cpu, struct mmu *mmu);

/* Pack/unpack F as flags. */
static inline u8 cpu_get_f(const cpu_state *cpu) { return cpu->f & 0xF0u; }
static inline void cpu_set_f(cpu_state *cpu, u8 v) { cpu->f = v & 0xF0u; }

#endif
