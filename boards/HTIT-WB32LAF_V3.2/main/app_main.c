/* HTIT-WB32LAF_V3.2 entry point: boot the JIT, run a Game Boy ROM,
 * and stream the PPU output to the onboard I2C SSD1306 OLED.
 *
 *   Core 0:  app_main → gbjit_dispatcher_run_until (the GB CPU)
 *   Core 1:  ppu_thread (spins on ppu_tick, generating scanlines)
 *   Core 1:  oled_task (sleeps until PPU bumps frame_seq, then pushes)
 *
 * Core 0 is reserved for the dispatcher's tight loop. Core 1 hosts
 * both the PPU thread and the OLED display task; the OLED task is low
 * priority and yields between frames, so the dispatcher dominates.
 */

#include "cpu_state.h"
#include "memory.h"
#include "sm83_interp.h"
#include "dispatcher.h"
#include "ppu_thread.h"

#include "board.h"
#include "oled_task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "gbjit";

/* The ROM is supplied via EMBED_FILES in main/CMakeLists.txt. Default
 * is Super Mario Land; pass -DBOARD_ROM=blargg_06 to use the Blargg
 * cpu_instrs sub-test instead. */
#if defined(BOARD_ROM_BLARGG)
extern const uint8_t rom_start[] asm("_binary_blargg_06_gb_start");
extern const uint8_t rom_end  [] asm("_binary_blargg_06_gb_end");
static const char *ROM_LABEL = "blargg_06";
#else
extern const uint8_t rom_start[] asm("_binary_sml_gb_start");
extern const uint8_t rom_end  [] asm("_binary_sml_gb_end");
static const char *ROM_LABEL = "sml";
#endif

static cpu_state s_cpu;
static mmu s_mmu;

static char  s_serial_buf[128];
static int   s_serial_len;
static void serial_sink(void *ctx, uint8_t b) {
    (void)ctx;
    if (b == '\n' || s_serial_len >= (int)sizeof(s_serial_buf) - 1) {
        s_serial_buf[s_serial_len] = 0;
        ESP_LOGI("gb_serial", "%s", s_serial_buf);
        s_serial_len = 0;
        return;
    }
    if (b >= 0x20 && b < 0x7F) {
        s_serial_buf[s_serial_len++] = (char)b;
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "boot — gbjit-xtensa @ HTIT-WB32LAF_V3.2");
    ESP_LOGI(TAG, "rom = %s, size = %u bytes", ROM_LABEL,
             (unsigned)(rom_end - rom_start));

    gb_mmu_init(&s_mmu);
    if (!mmu_load_rom(&s_mmu, rom_start, (size_t)(rom_end - rom_start))) {
        ESP_LOGE(TAG, "mmu_load_rom failed");
        return;
    }
    s_mmu.serial_sink = serial_sink;
    cpu_reset(&s_cpu, &s_mmu);

    /* Bring up the OLED task first (Core 1, low prio) — it'll sleep
     * until the PPU bumps frame_seq. */
    oled_task_start(&s_cpu);

    /* PPU on Core 1 alongside the OLED task. The dispatcher on Core 0
     * won't call ppu_tick under GBJIT_PPU_ASYNC; Core 1's task does. */
    ppu_thread_start(&s_cpu);

    /* Run the GB CPU forever. The dispatcher self-terminates on HALT
     * but ticks halted cycles so PPU keeps advancing — eventually a
     * VBlank IRQ wakes the GB CPU. The 4 G-cycle budget here is just
     * "run effectively forever" — at 4 MHz DMG that's ~34 minutes
     * simulated, vastly longer than we'll typically watch the OLED. */
    static gbjit_dispatcher disp;
    if (!gbjit_dispatcher_init(&disp, &s_cpu)) {
        ESP_LOGE(TAG, "dispatcher_init failed");
        return;
    }
    /* Make the dispatcher reachable from oled_task so the per-second
     * log can dump JIT counters (profiling build only). */
    extern gbjit_dispatcher *g_dispatcher;
    g_dispatcher = &disp;
    /* Disable the static-successor prefetcher. The cache-size sweep
     * (benchmark/results/cache_sweep.csv) found that at the 64 KB
     * default arena the no-prefetch path is 11 % faster than the
     * historical depth=4 default — the prefetcher compiles fallthru
     * blocks that this workload never executes and the eager-compile
     * cost outweighs the first-encounter latency it saves. The lazy
     * chain-miss compile path picks up new blocks on demand. */
    disp.prefetch_enabled = false;
    ESP_LOGI(TAG, "starting CPU — Core 0 dispatcher, Core 1 PPU+OLED, prefetch off");
    gbjit_dispatcher_run_until(&disp, ~(uint64_t)0);
    ESP_LOGI(TAG, "dispatcher returned (pc=%04X halted=%d cycles=%" PRIu64 ")",
             s_cpu.pc, s_cpu.halted, s_cpu.cycles);
}
