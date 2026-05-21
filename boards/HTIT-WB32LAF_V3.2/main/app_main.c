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
#include "qemu_lcd.h"

#ifndef GBJIT_QEMU_LCD
#define GBJIT_QEMU_LCD 0
#endif

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
#elif defined(BOARD_ROM_TETRIS)
/* EMBED_FILES mangles non-alphanumerics in the path basename to '_', so
 * roms/Tetris_JUE_V1.1.gb → _binary_Tetris_JUE_V1_1_gb_{start,end}. */
extern const uint8_t rom_start[] asm("_binary_Tetris_JUE_V1_1_gb_start");
extern const uint8_t rom_end  [] asm("_binary_Tetris_JUE_V1_1_gb_end");
static const char *ROM_LABEL = "tetris";
#else
extern const uint8_t rom_start[] asm("_binary_sml_gb_start");
extern const uint8_t rom_end  [] asm("_binary_sml_gb_end");
static const char *ROM_LABEL = "sml";
#endif

static cpu_state s_cpu;
static mmu s_mmu;

#ifdef DEBUG
static char  s_serial_buf[128];
static int   s_serial_len;
#endif
/* --- Optional wall-clock frame-rate pacer ---------------------------
 *
 * Compile-time enabled via -DGBJIT_FRAME_LIMIT_FPS=<n> (see
 * main/CMakeLists.txt). Default 0 = disabled, no code emitted, no
 * runtime cost. When non-zero, the pacer fires from inside ppu_tick at
 * every frame boundary (right after frame_seq increments) and busy-
 * waits until the next 1/FPS-second tick of an anchor monotonic
 * clock, so frame N completes at anchor + N * PERIOD. If the
 * emulator falls more than 2 frames behind (genuine workload slip),
 * re-anchor at the current time so we don't sprint to catch up.
 *
 * The wait is a busy spin on esp_timer_get_time() rather than
 * vTaskDelay. The dispatcher is the only Core 0 task in our app, so
 * vTaskDelay would just hand control back to the idle task; busy-
 * waiting is no more wasteful and is precise to the microsecond.
 * (FreeRTOS tick at 100 Hz can't represent the sub-tick waits this
 * loop produces anyway.) */
#if GBJIT_FRAME_LIMIT_FPS > 0
#define PACER_FRAME_PERIOD_US (1000000 / GBJIT_FRAME_LIMIT_FPS)
static int64_t s_pacer_anchor_us;
static uint32_t s_pacer_frame_count;
static void frame_pacer(struct mmu *m) {
    (void)m;
    s_pacer_frame_count++;
    int64_t target = s_pacer_anchor_us +
                     (int64_t)s_pacer_frame_count * PACER_FRAME_PERIOD_US;
    int64_t now = esp_timer_get_time();
    /* Behind-by-more-than-two-frames: don't sprint to catch up, just
     * re-anchor here and keep producing at real rate from now on. */
    if (now > target + 2 * PACER_FRAME_PERIOD_US) {
        s_pacer_anchor_us = now;
        s_pacer_frame_count = 0;
        return;
    }
    while (now < target) {
        now = esp_timer_get_time();
    }
}
#endif

/* --- FPS probe ------------------------------------------------------
 *
 * Opt-in via -DGBJIT_FPS_PROBE=1. A frame-complete callback (Core 0,
 * fired from ppu_tick once per emulated frame) measures how many GB
 * frames the emulator produces per wall-clock second — i.e. emulation
 * throughput. The lowest 1-second window and the worst single frame
 * are tracked internally every second (compare-only, no I/O); a
 * summary is logged only every FPS_LOG_PERIOD_S seconds so the serial
 * write — which blocks Core 0 — barely perturbs the number it reports.
 * The first 3 s are skipped as warm-up (boot + cold JIT compilation).
 * Logging uses ESP_LOGE so it survives the release ERROR log level. */
#if GBJIT_FPS_PROBE
#ifndef FPS_LOG_PERIOD_S
#define FPS_LOG_PERIOD_S 10
#endif
static void fps_probe(struct mmu *m) {
    (void)m;
    static bool     started, warm;
    static int64_t  t_anchor, t_win, t_prev;
    static uint32_t total, win_frames, log_div;
    static int      min_win = 1 << 30;
    static int64_t  worst_frame_us;

    int64_t now = esp_timer_get_time();
    if (!started) { started = true; t_anchor = t_win = t_prev = now; return; }

    if (!warm) {
        if (now - t_anchor < 3000000) { t_prev = now; return; }
        warm = true;
        t_anchor = t_win = t_prev = now;       /* re-anchor past warm-up */
        return;
    }

    int64_t frame_us = now - t_prev;
    t_prev = now;
    if (frame_us > worst_frame_us) worst_frame_us = frame_us;
    total++;
    win_frames++;

    if (now - t_win >= 1000000) {
        int win_fps = (int)((int64_t)win_frames * 1000000 / (now - t_win));
        if (win_fps < min_win) min_win = win_fps;
        t_win = now;
        win_frames = 0;
        if (++log_div >= FPS_LOG_PERIOD_S) {
            log_div = 0;
            int avg = (int)((int64_t)total * 1000000 / (now - t_anchor));
            ESP_LOGE("fps", "%s avg=%d min=%d worst=%lldms (%llds)",
                     ROM_LABEL, avg, min_win,
                     (long long)(worst_frame_us / 1000),
                     (long long)((now - t_anchor) / 1000000));
        }
    }
}
#endif

#ifdef DEBUG
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
#endif

void app_main(void) {
    ESP_LOGI(TAG, "boot — gbjit-xtensa @ HTIT-WB32LAF_V3.2");
    ESP_LOGI(TAG, "rom = %s, size = %u bytes", ROM_LABEL,
             (unsigned)(rom_end - rom_start));

    gb_mmu_init(&s_mmu);
    if (!mmu_load_rom(&s_mmu, rom_start, (size_t)(rom_end - rom_start))) {
        ESP_LOGE(TAG, "mmu_load_rom failed");
        return;
    }
#ifdef DEBUG
    /* Blargg test-ROM serial sink to UART. Release builds drop this —
     * SML doesn't use FF02/FF01 anyway. */
    s_mmu.serial_sink = serial_sink;
#endif
#if GBJIT_FRAME_LIMIT_FPS > 0
    /* Wall-clock frame-rate cap, opt-in at build time. */
    s_pacer_anchor_us = esp_timer_get_time();
    s_pacer_frame_count = 0;
    s_mmu.frame_complete_cb = frame_pacer;
#elif GBJIT_FPS_PROBE
    /* Throughput measurement (mutually exclusive with the pacer — a
     * capped frame rate would make the FPS number meaningless). */
    s_mmu.frame_complete_cb = fps_probe;
#endif
    cpu_reset(&s_cpu, &s_mmu);

    /* Display path. The physical I2C OLED can't be exercised under
     * QEMU (no SSD1306 device model), so a QEMU build streams the
     * full 160x144 framebuffer out UART1 to a host-side viewer
     * instead. Hardware builds bring up the real OLED task. */
#if GBJIT_QEMU_LCD
    qemu_lcd_start(&s_cpu);
#else
    /* Bring up the OLED task first (Core 1, low prio) — it'll sleep
     * until the PPU bumps frame_seq. */
    oled_task_start(&s_cpu);
#endif

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
#if GBJIT_BOARD_INTERP
    /* Benchmark baseline: route every block through the reference
     * interpreter (sm83_step) instead of the JIT — measures the
     * no-JIT floor for the FPS sweep. Opt-in, off by default. */
    disp.interp_fallback = true;
#endif
    ESP_LOGI(TAG, "starting CPU — Core 0 dispatcher, Core 1 PPU+OLED, prefetch off");
    gbjit_dispatcher_run_until(&disp, ~(uint64_t)0);
    ESP_LOGI(TAG, "dispatcher returned (pc=%04X halted=%d cycles=%" PRIu64 ")",
             s_cpu.pc, s_cpu.halted, s_cpu.cycles);
}
