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

static cpu_state s_cpu;
static mmu s_mmu;

static void serial_sink(void *ctx, uint8_t b) {
    (void)ctx;
    /* Echo Blargg-style serial output to UART. */
    putchar((int)b);
}

void app_main(void) {
    ESP_LOGI(TAG, "boot — gbjit-xtensa target build");

    mmu_init(&s_mmu);
    memcpy(s_mmu.rom + 0x0100, ROM_ALU, sizeof(ROM_ALU));
    s_mmu.serial_sink = serial_sink;
    cpu_reset(&s_cpu, &s_mmu);

    gbjit_dispatcher d;
    if (!gbjit_dispatcher_init(&d, &s_cpu)) {
        ESP_LOGE(TAG, "dispatcher_init FAILED — likely out of EXEC heap");
        return;
    }
    ESP_LOGI(TAG, "dispatcher armed; running ALU ROM");

    int64_t t0 = esp_timer_get_time();
    gbjit_dispatcher_run_until(&d, 100000);
    int64_t t1 = esp_timer_get_time();

    ESP_LOGI(TAG, "halted=%d pc=$%04X cycles=%" PRIu64,
             s_cpu.halted, s_cpu.pc, s_cpu.cycles);
    ESP_LOGI(TAG, "end state: A=$%02X F=$%02X B=$%02X",
             s_cpu.a, s_cpu.f, s_cpu.b);

    /* Hard-coded expected end state (matches the host differential test).
     * After HALT (1 byte), PC has advanced to 0x010B; cycle count =
     * 8+8+4+4+4+4+4+4+4 = 44 for the 11-byte ROM. */
    const int ok =
        (s_cpu.halted == 1) &&
        (s_cpu.a == 0x00) &&
        (s_cpu.f == 0x70) &&
        (s_cpu.b == 0x13) &&
        (s_cpu.pc == 0x010B) &&
        (s_cpu.cycles == 44);

    ESP_LOGI(TAG, "JIT elapsed %" PRId64 " us  blocks_compiled=%" PRIu64
                  "  blocks_executed=%" PRIu64
                  "  chain_hits=%" PRIu64 "  chain_misses=%" PRIu64,
             t1 - t0,
             d.blocks_compiled, d.blocks_executed,
             d.chain_hits, d.chain_misses);

    if (ok) {
        ESP_LOGI(TAG, "RESULT: PASS");
    } else {
        ESP_LOGE(TAG, "RESULT: FAIL — JIT output does not match expected");
    }

    gbjit_dispatcher_shutdown(&d);
}
