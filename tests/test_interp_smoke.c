/* Smoke test: build a tiny ROM in memory, run it through the interpreter,
   and assert final register state. This validates that fetch/decode/dispatch
   work and a handful of opcodes do what they should.

   ROM (starts at 0x0100, since cpu_reset jumps there):

     0100: 3E 42        LD  A, $42
     0102: 06 03        LD  B, $03
     0104: 80           ADD A, B          ; A = $45
     0105: 21 00 C0     LD  HL, $C000     ; WRAM
     0108: 77           LD  (HL), A
     0109: 36 7E        LD  (HL), $7E
     010B: 3E 81        LD  A, $81
     010D: E0 02        LDH ($FF02), A    ; Blargg serial transmit trigger
     010F: 3E 39        LD  A, $39
     0111: E0 01        LDH ($FF01), A    ; (set after — irrelevant)
     0113: 76           HALT
*/

#include "cpu_state.h"
#include "memory.h"
#include "sm83_interp.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static char serial_buf[16];
static int  serial_n;

static void on_serial(void *ctx, u8 b) {
    (void)ctx;
    if (serial_n < (int)sizeof(serial_buf)) serial_buf[serial_n++] = (char)b;
}

int main(void) {
    static const u8 prog[] = {
        0x3E, 0x42,             /* LD A,$42 */
        0x06, 0x03,             /* LD B,$03 */
        0x80,                   /* ADD A,B */
        0x21, 0x00, 0xC0,       /* LD HL,$C000 */
        0x77,                   /* LD (HL),A */
        0x36, 0x7E,             /* LD (HL),$7E */
        0x3E, 0x55,             /* LD A,$55 */
        0xE0, 0x01,             /* LDH ($FF01),A   ; serial data = 'U' */
        0x3E, 0x81,             /* LD A,$81 */
        0xE0, 0x02,             /* LDH ($FF02),A   ; trigger serial */
        0x76,                   /* HALT */
    };

    static mmu m;
    mmu_init(&m);
    /* Place program at 0x0100. */
    memcpy(m.rom + 0x0100, prog, sizeof(prog));
    m.serial_sink = on_serial;

    cpu_state cpu;
    cpu_reset(&cpu, &m);

    /* Run for a generous cycle budget; HALT will stop forward progress. */
    int steps = 0;
    while (!cpu.halted && steps < 200) { sm83_step(&cpu); steps++; }

    if (!cpu.halted) {
        fprintf(stderr, "FAIL: CPU did not reach HALT in %d steps (PC=%04X)\n", steps, cpu.pc);
        return 1;
    }
    if (cpu.a != 0x81) {
        fprintf(stderr, "FAIL: expected A=0x81 (last LD before HALT) got A=%02X\n", cpu.a);
        return 1;
    }
    if (cpu.b != 0x03) {
        fprintf(stderr, "FAIL: expected B=0x03 got B=%02X\n", cpu.b);
        return 1;
    }
    if (cpu.hl != 0xC000) {
        fprintf(stderr, "FAIL: expected HL=0xC000 got HL=%04X\n", cpu.hl);
        return 1;
    }
    if (m.wram[0] != 0x7E) {
        fprintf(stderr, "FAIL: expected (C000)=0x7E got %02X\n", m.wram[0]);
        return 1;
    }
    if (serial_n != 1 || (u8)serial_buf[0] != 0x55) {
        fprintf(stderr, "FAIL: expected serial output 0x55 (1 byte), got %d bytes (first=%02X)\n",
                serial_n, serial_n ? (u8)serial_buf[0] : 0);
        return 1;
    }
    printf("smoke: OK  (PC=%04X, A=%02X, HL=%04X, cycles=%llu)\n",
           cpu.pc, cpu.a, cpu.hl, (unsigned long long)cpu.cycles);
    return 0;
}
