/* GBJIT-Xtensa — ESP32-S3 main app.
 *
 * Runs a baked-in GB ROM through the JIT dispatcher and prints results.
 * Designed to run cleanly under qemu-system-xtensa (no real hardware
 * peripherals required).
 *
 * Test plan:
 *   1. Run an ALU smoke ROM under the JIT.
 *   2. Print the final cpu_state. Expected values are documented inline
 *      and reproduce the host `jit_differential` test exactly.
 *   3. Print summary stats (blocks compiled/executed, chain hits/misses,
 *      cycles, JIT elapsed time).
 */

#include "cpu_state.h"
#include "memory.h"
#include "dispatcher.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "gbjit";

/* Same ALU ROM the host `jit_differential` test uses — see
 * tests/test_jit_differential.c. The expected end state is checked below. */
static const uint8_t ROM_ALU[] = {
    0x3E, 0x42,             /* LD A,$42 */
    0x06, 0x13,             /* LD B,$13 */
    0x80,                   /* ADD A,B  -> A=$55, F=$00 */
    0x90,                   /* SUB B    -> A=$42, F=$40 */
    0xA0,                   /* AND B    -> A=$02, F=$20 */
    0xB0,                   /* OR  B    -> A=$13, F=$00 */
    0xA8,                   /* XOR B    -> A=$00, F=$80 */
    0xB8,                   /* CP  B    -> A unchanged, F=$70 */
    0x76,                   /* HALT */
};

/* CB-prefix ROM — exercises SWAP/SLA/SRL/BIT/SET/RES/RL/RRC. */
static const uint8_t ROM_CB[] = {
    0x3E, 0x42,             /* LD A,$42 */
    0xCB, 0x37,             /* SWAP A */
    0xCB, 0x27,             /* SLA A */
    0xCB, 0x3F,             /* SRL A */
    0xCB, 0x47,             /* BIT 0,A */
    0xCB, 0xE7,             /* SET 4,A */
    0xCB, 0xA7,             /* RES 4,A */
    0xCB, 0x17,             /* RL  A */
    0xCB, 0x0F,             /* RRC A */
    0x76,                   /* HALT */
};

/* Memory ROM — exercises inlined LD (a16),A / LD A,(a16) / LDH for WRAM and HRAM. */
static const uint8_t ROM_MEM[] = {
    0x3E, 0x42,             /* LD A,$42 */
    0xEA, 0x00, 0xC1,       /* LD (C100),A */
    0x3E, 0x00,             /* LD A,$00 */
    0xFA, 0x00, 0xC1,       /* LD A,(C100) */
    0xE0, 0x80,             /* LDH (FF80),A */
    0x3E, 0xAA,             /* LD A,$AA */
    0xF0, 0x80,             /* LDH A,(FF80) */
    0x76,                   /* HALT */
};

/* Fibonacci ROM — exercises the inlined JR cc loop. After 8 iterations
 * starting from a=0,b=1, A=fib(8)=21 ($15), B=fib(7)=13 ($0D). */
static const uint8_t ROM_FIB[] = {
    0x3E, 0x00,             /* LD A,$00 */
    0x06, 0x01,             /* LD B,$01 */
    0x0E, 0x08,             /* LD C,$08 */
    /* loop: */
    0x67,                   /* LD H,A */
    0x80,                   /* ADD A,B */
    0x44,                   /* LD B,H */
    0x0D,                   /* DEC C */
    0x20, 0xFA,             /* JR NZ,-6 */
    0x76,                   /* HALT */
};

static cpu_state s_cpu;
static mmu s_mmu;

static void serial_sink(void *ctx, uint8_t b) {
    (void)ctx;
    /* Echo Blargg-style serial output to UART. */
    putchar((int)b);
}

typedef struct {
    const char *name;
    const uint8_t *rom;
    size_t rom_len;
    uint8_t  exp_a, exp_f, exp_b;
    uint16_t exp_pc;
    uint64_t exp_cycles;
} test_case;

static bool run_case(const test_case *tc) {
    mmu_init(&s_mmu);
    memcpy(s_mmu.rom + 0x0100, tc->rom, tc->rom_len);
    s_mmu.serial_sink = serial_sink;
    cpu_reset(&s_cpu, &s_mmu);

    gbjit_dispatcher d;
    if (!gbjit_dispatcher_init(&d, &s_cpu)) {
        ESP_LOGE(TAG, "[%s] dispatcher_init FAILED — out of EXEC heap?", tc->name);
        return false;
    }

    int64_t t0 = esp_timer_get_time();
    gbjit_dispatcher_run_until(&d, 100000);
    int64_t t1 = esp_timer_get_time();

    ESP_LOGI(TAG,
        "[%s] halted=%d pc=$%04X cycles=%" PRIu64 " A=$%02X F=$%02X B=$%02X "
        "blocks(c/e)=%" PRIu64 "/%" PRIu64 " chain(h/m)=%" PRIu64 "/%" PRIu64
        " jit_us=%" PRId64,
        tc->name, s_cpu.halted, s_cpu.pc, s_cpu.cycles,
        s_cpu.a, s_cpu.f, s_cpu.b,
        d.blocks_compiled, d.blocks_executed,
        d.chain_hits, d.chain_misses,
        t1 - t0);

    bool ok =
        s_cpu.halted == 1 &&
        s_cpu.a == tc->exp_a &&
        s_cpu.f == tc->exp_f &&
        s_cpu.b == tc->exp_b &&
        s_cpu.pc == tc->exp_pc &&
        s_cpu.cycles == tc->exp_cycles;

    gbjit_dispatcher_shutdown(&d);
    return ok;
}

void app_main(void) {
    ESP_LOGI(TAG, "boot — gbjit-xtensa target build");

    /* After HALT (1 byte), PC advances past it. Expected values mirror
     * the host jit_differential test. */
    static const test_case cases[] = {
        {
            .name = "alu",
            .rom = ROM_ALU, .rom_len = sizeof(ROM_ALU),
            .exp_a = 0x00, .exp_f = 0x70, .exp_b = 0x13,
            .exp_pc = 0x010B, .exp_cycles = 44,
        },
        {
            /* mem: A round-trips $42 through WRAM and HRAM.
             * ROM is 17 bytes (PC = $100 + 17 = $111 after HALT).
             * Cycles: 8+16+8+16+12+8+12+4 = 84. */
            .name = "mem",
            .rom = ROM_MEM, .rom_len = sizeof(ROM_MEM),
            .exp_a = 0x42, .exp_f = 0xB0 /* F unchanged from reset */, .exp_b = 0x00,
            .exp_pc = 0x0111, .exp_cycles = 84,
        },
        {
            /* CB ops: ROM is 19 bytes. Final A=$24, F=$00.
             * Cycles: 8 (LD) + 8*8 (CB) + 4 (HALT) = 76. */
            .name = "cb",
            .rom = ROM_CB, .rom_len = sizeof(ROM_CB),
            .exp_a = 0x24, .exp_f = 0x00, .exp_b = 0x00,
            .exp_pc = 0x0113, .exp_cycles = 76,
        },
        {
            .name = "fib",
            .rom = ROM_FIB, .rom_len = sizeof(ROM_FIB),
            /* Final fib(8): A=$15, B=$0D. F = $C0 (Z|N from last DEC C). */
            .exp_a = 0x15, .exp_f = 0xC0, .exp_b = 0x0D,
            /* PC: 0x0100 + 13 bytes (HALT at 0x010C, advance → 0x010D). */
            .exp_pc = 0x010D,
            /* Cycles: 8+8+8 + 7*(4+4+4+4+12) + (4+4+4+4+8) + 4 = 248 */
            .exp_cycles = 248,
        },
    };

    int failed = 0;
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        if (!run_case(&cases[i])) {
            ESP_LOGE(TAG, "[%s] RESULT: FAIL", cases[i].name);
            failed++;
        } else {
            ESP_LOGI(TAG, "[%s] RESULT: PASS", cases[i].name);
        }
    }

    if (failed == 0) {
        ESP_LOGI(TAG, "RESULT: PASS  (%zu cases)", sizeof(cases)/sizeof(cases[0]));
    } else {
        ESP_LOGE(TAG, "RESULT: FAIL  (%d/%zu cases failed)",
                 failed, sizeof(cases)/sizeof(cases[0]));
    }
}
