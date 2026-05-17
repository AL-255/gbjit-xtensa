/* GBJIT-Xtensa — ESP32-S3 main app + benchmark.
 *
 * Loads the embedded Blargg `06-ld r,r.gb` sub-test ROM (32 KB MBC0,
 * baked into the firmware via IDF's EMBED_FILES) and runs it through the
 * JIT dispatcher, capturing the Blargg serial output and measuring
 * throughput in GB-cycles per second of wall time.
 *
 * Compare against:
 *   ./build/gbjit_host --interp --max-cycles <N> roms/06-ld_r_r.gb
 *   ./build/gbjit_host --jit    --max-cycles <N> roms/06-ld_r_r.gb
 *
 * to see how the same JIT performs on emulated Xtensa LX7 versus the
 * sim-driven host build.
 */

#include "cpu_state.h"
#include "memory.h"
#include "sm83_interp.h"
#include "dispatcher.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "gbjit";

/* Cap test runtime so qemu sessions stay bounded. The Blargg ROMs reach
 * "Passed"/"Failed" output within ~100M GB-cycles, but this is GB-cycles,
 * which on a slow JIT could be many real seconds. */
#define BENCH_CYCLES_BUDGET (50ull * 1000ull * 1000ull)

/* Embedded ROM (added via main/CMakeLists.txt EMBED_FILES). */
extern const uint8_t blargg_rom_start[] asm("_binary_blargg_06_gb_start");
extern const uint8_t blargg_rom_end  [] asm("_binary_blargg_06_gb_end");

static cpu_state s_cpu;
static mmu s_mmu;

/* Capture Blargg serial output line-by-line. */
static char  s_serial_buf[128];
static int   s_serial_len;
static void serial_sink(void *ctx, uint8_t b) {
    (void)ctx;
    if (b == '\n') {
        if (s_serial_len) {
            s_serial_buf[s_serial_len] = 0;
            ESP_LOGI(TAG, "  serial: %s", s_serial_buf);
            s_serial_len = 0;
        }
        return;
    }
    if (s_serial_len < (int)sizeof(s_serial_buf) - 1) {
        s_serial_buf[s_serial_len++] = (char)b;
    }
}

void app_main(void) {
    size_t rom_len = (size_t)(blargg_rom_end - blargg_rom_start);
    ESP_LOGI(TAG, "embedded ROM size = %zu bytes", rom_len);
    if (rom_len > ROM_SIZE) {
        ESP_LOGE(TAG, "ROM too large for MBC0 MMU (limit %u)", (unsigned)ROM_SIZE);
        return;
    }

    mmu_init(&s_mmu);
    memcpy(s_mmu.rom, blargg_rom_start, rom_len);
    s_mmu.serial_sink = serial_sink;
    cpu_reset(&s_cpu, &s_mmu);

    gbjit_dispatcher d;
    if (!gbjit_dispatcher_init(&d, &s_cpu)) {
        ESP_LOGE(TAG, "dispatcher_init FAILED — out of EXEC heap");
        return;
    }
    ESP_LOGI(TAG, "running JIT for %" PRIu64 " GB-cycles…", (u64)BENCH_CYCLES_BUDGET);

    int64_t t0 = esp_timer_get_time();
    gbjit_dispatcher_run_until(&d, BENCH_CYCLES_BUDGET);
    int64_t t1 = esp_timer_get_time();
    int64_t elapsed_us = t1 - t0;

    /* Throughput: GB-cycles per second of wall time. */
    double mhz = (double)s_cpu.cycles * 1e6 / (double)elapsed_us / 1e6;
    double real_dmg_ratio = mhz / 4.194304;   /* DMG runs at ~4.194 MHz of T-cycles */

    ESP_LOGI(TAG, "done — pc=$%04X cycles=%" PRIu64 " elapsed=%" PRId64 " us",
             s_cpu.pc, s_cpu.cycles, elapsed_us);
    ESP_LOGI(TAG, "       blocks_compiled=%" PRIu64 " blocks_executed=%" PRIu64
                  " chain_hits=%" PRIu64 " chain_misses=%" PRIu64,
             d.blocks_compiled, d.blocks_executed, d.chain_hits, d.chain_misses);
    ESP_LOGI(TAG, "       throughput = %.2f MHz (T-cycles)  = %.2fx DMG",
             mhz, real_dmg_ratio);

    if (s_serial_len) {
        s_serial_buf[s_serial_len] = 0;
        ESP_LOGI(TAG, "  serial-tail: %s", s_serial_buf);
    }

    ESP_LOGI(TAG, "RESULT: DONE");

    /* The Xtensa windowed-ABI call chain through helpers leaves stale spill
     * bookkeeping that trips up app_main's teardown. Park here so qemu
     * captures the benchmark output cleanly; the dispatcher state was
     * already torn down logically (we just don't free its arena). */
    while (1) {
        for (volatile int i = 0; i < 10000000; i++) {}
    }
}
