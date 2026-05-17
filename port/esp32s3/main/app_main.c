/* GBJIT-Xtensa — ESP32-S3 benchmark harness.
 *
 * Runs the embedded Blargg `06-ld r,r.gb` sub-test through both the
 * reference interpreter and the JIT and prints a single parseable
 * `[BENCH]` line per mode. benchmark/run_bench.sh consumes those lines.
 *
 * After both modes finish, the program parks in a busy loop. This
 * lets qemu's `-d ...` instrumentation finish flushing its output
 * file without racing with a panicking unwind through the windowed
 * call chain (see notes in dispatcher.c).
 */

#include "cpu_state.h"
#include "memory.h"
#include "sm83_interp.h"
#include "dispatcher.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "gbjit";

/* Cycle budget. 5M is enough for the Blargg ROM to print its "06-ld r,r" /
 * "Passed" sequence under the JIT, and keeps qemu-trace files tractable. */
#ifndef BENCH_CYCLES_BUDGET
#define BENCH_CYCLES_BUDGET (5ull * 1000ull * 1000ull)
#endif

extern const uint8_t blargg_rom_start[] asm("_binary_blargg_06_gb_start");
extern const uint8_t blargg_rom_end  [] asm("_binary_blargg_06_gb_end");

static cpu_state s_cpu;
static mmu s_mmu;
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

static void load_rom_and_reset(void) {
    size_t rom_len = (size_t)(blargg_rom_end - blargg_rom_start);
    mmu_init(&s_mmu);
    memcpy(s_mmu.rom, blargg_rom_start, rom_len);
    s_mmu.serial_sink = serial_sink;
    cpu_reset(&s_cpu, &s_mmu);
    s_serial_len = 0;
}

static void run_interp(void) {
    load_rom_and_reset();
    ESP_LOGI(TAG, "interp: starting (budget=%" PRIu64 " GB-cycles)", (uint64_t)BENCH_CYCLES_BUDGET);
    int64_t t0 = esp_timer_get_time();
    sm83_run_until(&s_cpu, BENCH_CYCLES_BUDGET);
    int64_t t1 = esp_timer_get_time();
    int64_t us = t1 - t0;
    double mhz = (double)s_cpu.cycles / (double)us;
    ESP_LOGI(TAG,
        "[BENCH] mode=interp cycles=%" PRIu64 " elapsed_us=%" PRId64
        " mhz=%.3f dmg_x=%.3f pc=0x%04X halted=%d",
        s_cpu.cycles, us, mhz, mhz / 4.194304, s_cpu.pc, s_cpu.halted);
}

/* Run the JIT and report a `[BENCH] mode=<label>` line. If `warm_only` is
 * true, an additional dry-run is performed first to populate the JIT's
 * block cache, then cpu_state is reset and the timed run starts with all
 * 25 blocks already compiled — so the measured window contains ZERO
 * `gbjit_compile_block` calls. The difference between cold and warm is
 * the on-the-fly translation overhead. */
static void run_jit_labelled(const char *label, bool warm_only) {
    load_rom_and_reset();
    gbjit_dispatcher d;
    if (!gbjit_dispatcher_init(&d, &s_cpu)) {
        ESP_LOGE(TAG, "jit: dispatcher_init FAILED");
        return;
    }
    if (warm_only) {
        /* Dry-run: compile every block the workload touches. */
        gbjit_dispatcher_run_until(&d, BENCH_CYCLES_BUDGET);
        /* Reset just cpu_state — keep the dispatcher's compiled-block table. */
        load_rom_and_reset();
        d.cpu = &s_cpu;
        d.blocks_compiled = 0;
        d.blocks_executed = 0;
        d.chain_hits = 0;
        d.chain_misses = 0;
    }
    ESP_LOGI(TAG, "%s: starting (budget=%" PRIu64 " GB-cycles)", label, (uint64_t)BENCH_CYCLES_BUDGET);
    int64_t t0 = esp_timer_get_time();
    gbjit_dispatcher_run_until(&d, BENCH_CYCLES_BUDGET);
    int64_t t1 = esp_timer_get_time();
    int64_t us = t1 - t0;
    double mhz = (double)s_cpu.cycles / (double)us;
    ESP_LOGI(TAG,
        "[BENCH] mode=%s cycles=%" PRIu64 " elapsed_us=%" PRId64
        " mhz=%.3f dmg_x=%.3f pc=0x%04X halted=%d"
        " blocks_compiled=%" PRIu64 " blocks_executed=%" PRIu64
        " chain_hits=%" PRIu64 " chain_misses=%" PRIu64
        " prefetched=%" PRIu64,
        label, s_cpu.cycles, us, mhz, mhz / 4.194304, s_cpu.pc, s_cpu.halted,
        d.blocks_compiled, d.blocks_executed, d.chain_hits, d.chain_misses,
        d.prefetched_blocks);
    /* Intentionally leak the dispatcher's arena: gbjit_dispatcher_shutdown
     * tries to free per-block book-keeping that has dangling predicted_next
     * pointers across multiple runs, and a clean teardown isn't worth a
     * second crash hunt for this bench harness. */
}

static void run_jit(void)      { run_jit_labelled("jit",      false); }
static void run_jit_warm(void) { run_jit_labelled("jit_warm", true);  }
/* (run_jit_noprefetch is defined below the static helper.) */

/* No-cache variant: dispatcher recompiles the block on every iteration
 * (cache lookup disabled, codecache arena reset before each compile).
 * This isolates exactly what the JIT cache buys us. */
static void run_jit_noprefetch(void) {
    load_rom_and_reset();
    gbjit_dispatcher d;
    if (!gbjit_dispatcher_init(&d, &s_cpu)) {
        ESP_LOGE(TAG, "jit_noprefetch: dispatcher_init FAILED");
        return;
    }
    d.prefetch_enabled = false;
    ESP_LOGI(TAG, "jit_noprefetch: starting (budget=%" PRIu64 " GB-cycles)",
             (uint64_t)BENCH_CYCLES_BUDGET);
    int64_t t0 = esp_timer_get_time();
    gbjit_dispatcher_run_until(&d, BENCH_CYCLES_BUDGET);
    int64_t t1 = esp_timer_get_time();
    int64_t us = t1 - t0;
    double mhz = (double)s_cpu.cycles / (double)us;
    ESP_LOGI(TAG,
        "[BENCH] mode=jit_noprefetch cycles=%" PRIu64 " elapsed_us=%" PRId64
        " mhz=%.3f dmg_x=%.3f pc=0x%04X halted=%d"
        " blocks_compiled=%" PRIu64 " blocks_executed=%" PRIu64
        " chain_hits=%" PRIu64 " chain_misses=%" PRIu64
        " prefetched=%" PRIu64,
        s_cpu.cycles, us, mhz, mhz / 4.194304, s_cpu.pc, s_cpu.halted,
        d.blocks_compiled, d.blocks_executed, d.chain_hits, d.chain_misses,
        d.prefetched_blocks);
}

static void run_jit_nocache(void) {
    load_rom_and_reset();
    gbjit_dispatcher d;
    if (!gbjit_dispatcher_init(&d, &s_cpu)) {
        ESP_LOGE(TAG, "jit_nocache: dispatcher_init FAILED");
        return;
    }
    d.no_cache = true;
    ESP_LOGI(TAG, "jit_nocache: starting (budget=%" PRIu64 " GB-cycles)",
             (uint64_t)BENCH_CYCLES_BUDGET);
    int64_t t0 = esp_timer_get_time();
    gbjit_dispatcher_run_until(&d, BENCH_CYCLES_BUDGET);
    int64_t t1 = esp_timer_get_time();
    int64_t us = t1 - t0;
    double mhz = (double)s_cpu.cycles / (double)us;
    ESP_LOGI(TAG,
        "[BENCH] mode=jit_nocache cycles=%" PRIu64 " elapsed_us=%" PRId64
        " mhz=%.3f dmg_x=%.3f pc=0x%04X halted=%d"
        " blocks_compiled=%" PRIu64 " blocks_executed=%" PRIu64
        " chain_hits=%" PRIu64 " chain_misses=%" PRIu64,
        s_cpu.cycles, us, mhz, mhz / 4.194304, s_cpu.pc, s_cpu.halted,
        d.blocks_compiled, d.blocks_executed, d.chain_hits, d.chain_misses);
}


void app_main(void) {
    ESP_LOGI(TAG, "boot — gbjit-xtensa benchmark");
    ESP_LOGI(TAG, "rom size = %u bytes", (unsigned)(blargg_rom_end - blargg_rom_start));

#if defined(BENCH_MODE_INTERP_ONLY)
    run_interp();
#elif defined(BENCH_MODE_JIT_ONLY)
    run_jit();
#elif defined(BENCH_MODE_JIT_WARM_ONLY)
    run_jit_warm();
#elif defined(BENCH_MODE_JIT_NOCACHE_ONLY)
    run_jit_nocache();
#elif defined(BENCH_MODE_JIT_NOPREFETCH_ONLY)
    run_jit_noprefetch();
#else
    run_interp();
    run_jit_nocache();
    run_jit_noprefetch();
    run_jit();
    run_jit_warm();
#endif

    ESP_LOGI(TAG, "[BENCH] done");

    /* Suspend forever — the FreeRTOS idle task uses `WAITI`, which qemu
     * emulates as effectively zero TBs per second, so the qemu trace
     * stops growing once we get here. */
    while (1) { vTaskDelay(portMAX_DELAY); }
}
