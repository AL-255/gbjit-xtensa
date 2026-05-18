/* See oled_task.h. */

#include "oled_task.h"
#include "board.h"
#include "ssd1306.h"

#include "cpu_state.h"
#include "memory.h"
#include "dispatcher.h"

/* Profiling mode: app_main.c stashes a pointer to the live dispatcher
 * here so we can include its counters in the per-second log. */
gbjit_dispatcher *g_dispatcher = NULL;

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <string.h>

static const char *TAG = "oled";

/* 5x7 font, MSB-bottom column order (matches SSD1306 page-byte layout:
 * one byte = a vertical 8-pixel column with bit 0 at the top). Only the
 * glyphs we render (digits + a couple labels) — kept tiny on purpose. */
static const uint8_t font5x7[][5] = {
    /* '0' */ {0x3E, 0x51, 0x49, 0x45, 0x3E},
    /* '1' */ {0x00, 0x42, 0x7F, 0x40, 0x00},
    /* '2' */ {0x42, 0x61, 0x51, 0x49, 0x46},
    /* '3' */ {0x21, 0x41, 0x45, 0x4B, 0x31},
    /* '4' */ {0x18, 0x14, 0x12, 0x7F, 0x10},
    /* '5' */ {0x27, 0x45, 0x45, 0x45, 0x39},
    /* '6' */ {0x3C, 0x4A, 0x49, 0x49, 0x30},
    /* '7' */ {0x01, 0x71, 0x09, 0x05, 0x03},
    /* '8' */ {0x36, 0x49, 0x49, 0x49, 0x36},
    /* '9' */ {0x06, 0x49, 0x49, 0x29, 0x1E},
};
#define FONT_W 5
#define FONT_H 7
#define CHAR_W (FONT_W + 1)   /* one-column gap between glyphs */

/* Draw a single decimal digit (0..9) at column `col`, page 0 (top
 * 8 rows of the screen). Overwrites the 5x7 box: pre-clears the entire
 * char cell to 0 (dark) then sets the glyph bits, so the digit reads
 * cleanly over whatever the GB renderer wrote underneath. */
static void draw_digit(uint8_t *page_buf, int col, int digit) {
    if (digit < 0 || digit > 9) return;
    uint8_t *page0 = &page_buf[0 * BOARD_OLED_WIDTH];
    /* Clear the full CHAR_W cell to 0 first so a leftover BG pixel
     * from compose_frame doesn't bleed through. */
    for (int c = 0; c < CHAR_W; c++) {
        if (col + c < BOARD_OLED_WIDTH) page0[col + c] = 0;
    }
    for (int c = 0; c < FONT_W; c++) {
        if (col + c >= BOARD_OLED_WIDTH) break;
        page0[col + c] = font5x7[digit][c];
    }
}

/* Render `n` (0..999) right-aligned into the top-left 18-pixel-wide
 * box. Up to 3 digits, with leading-zero suppression. */
static void draw_fps(uint8_t *page_buf, int n) {
    if (n < 0) n = 0;
    if (n > 999) n = 999;
    int d100 = n / 100;
    int d10  = (n / 10) % 10;
    int d1   = n % 10;
    /* Always-draw rightmost digit at col 12. Conditionally draw the
     * tens digit at col 6 (only if n >= 10) and hundreds at col 0
     * (only if n >= 100). Leading positions get cleared so a previous
     * frame's "120" doesn't leave a phantom hundreds digit behind a
     * later "60". */
    uint8_t *page0 = &page_buf[0 * BOARD_OLED_WIDTH];
    for (int c = 0; c < 3 * CHAR_W; c++) page0[c] = 0;
    if (d100) draw_digit(page_buf, 0, d100);
    if (n >= 10) draw_digit(page_buf, CHAR_W, d10);
    draw_digit(page_buf, 2 * CHAR_W, d1);
}

/* Centre crop offsets: the 128x64 OLED window covers GB pixels
 * (CROP_X .. CROP_X+127, CROP_Y .. CROP_Y+63). For a 160x144 GB screen
 * that's columns 16..143 and rows 40..103 — the centre of the play
 * area, which captures Mario / the action box / most HUDs in DMG
 * games. Tweak here if you want the whole screen scrolled into view. */
#define CROP_X  ((160 - BOARD_OLED_WIDTH) / 2)    /* = 16 */
#define CROP_Y  ((144 - BOARD_OLED_HEIGHT) / 2)   /* = 40 */

/* Shade threshold: GB shade 0 = lightest, 3 = darkest. The OLED is
 * black-on-blue (typical Heltec). To match standard "ink on paper"
 * appearance we light pixels whose shade >= THRESHOLD. */
#define SHADE_THRESHOLD 2

static struct cpu_state *s_cpu;
static uint8_t s_page_buf[BOARD_OLED_PAGES * BOARD_OLED_WIDTH];

/* Convert a 128x64 region of the PPU framebuffer to the SSD1306's
 * page-major 1bpp format. */
static void compose_frame(const uint8_t *fb_160x144) {
    memset(s_page_buf, 0, sizeof(s_page_buf));
    for (int oled_y = 0; oled_y < BOARD_OLED_HEIGHT; oled_y++) {
        int gb_y = oled_y + CROP_Y;
        if (gb_y < 0 || gb_y >= 144) continue;
        const uint8_t *src_row = &fb_160x144[gb_y * 160 + CROP_X];
        uint8_t *page = &s_page_buf[(oled_y / 8) * BOARD_OLED_WIDTH];
        uint8_t bit = (uint8_t)(1u << (oled_y % 8));
        for (int oled_x = 0; oled_x < BOARD_OLED_WIDTH; oled_x++) {
            if (src_row[oled_x] >= SHADE_THRESHOLD) {
                page[oled_x] |= bit;
            }
        }
    }
}

static void oled_task(void *arg) {
    (void)arg;
    if (!ssd1306_init()) {
        ESP_LOGE(TAG, "ssd1306_init failed — task exiting");
        vTaskDelete(NULL);
        return;
    }
    ssd1306_clear();

    uint32_t last_seq = 0;

    /* JIT-rendered fps = how fast the PPU completes frames, sampled
     * over a sliding 1-second window of mmu->frame_seq increments.
     * Updated once per second and held until the next sample. This is
     * the GB-emulation rate, NOT the OLED push rate (which is bounded
     * by I2C throughput). */
    int64_t fps_window_start_us = esp_timer_get_time();
    uint32_t fps_window_seq_start = s_cpu->mmu->frame_seq;
    int current_fps = 0;

    uint32_t next_log_ms = 1000;
    uint32_t boot_ms = (uint32_t)(esp_log_timestamp());
    while (1) {
        uint32_t cur = s_cpu->mmu->frame_seq;

        /* Roll the fps window every ~1s. */
        int64_t now_us = esp_timer_get_time();
        if (now_us - fps_window_start_us >= 1000000) {
            uint32_t produced = cur - fps_window_seq_start;
            /* Scale to per-second; the window may overshoot by a few
             * ms but the rounding error stays under 1 fps. */
            current_fps = (int)((int64_t)produced * 1000000 /
                                (now_us - fps_window_start_us));
            fps_window_start_us = now_us;
            fps_window_seq_start = cur;
        }

        if (cur != last_seq) {
            last_seq = cur;
            compose_frame(s_cpu->mmu->framebuffer);
            /* Overlay the JIT-rendered fps in the top-left corner BEFORE
             * blitting. This intentionally clobbers the underlying PPU
             * pixels in that 18x7 region — small price for a live perf
             * readout. */
            draw_fps(s_page_buf, current_fps);
            ssd1306_blit(s_page_buf);
        } else {
            vTaskDelay(pdMS_TO_TICKS(4));
        }
        uint32_t now_ms = (uint32_t)(esp_log_timestamp());
        if (now_ms - boot_ms >= next_log_ms) {
            if (g_dispatcher) {
                /* Snapshot counters, then compute deltas over the past
                 * second so we see RATES, not running totals. */
                static uint64_t last_compiled, last_executed;
                static uint64_t last_chain_hits, last_chain_misses;
                uint64_t bc = g_dispatcher->blocks_compiled;
                uint64_t be = g_dispatcher->blocks_executed;
                uint64_t ch = g_dispatcher->chain_hits;
                uint64_t cm = g_dispatcher->chain_misses;
                ESP_LOGI(TAG, "fps=%d ppu=%lu pc=%04X | exec/s=%lu compile/s=%lu hit/s=%lu miss/s=%lu",
                         current_fps,
                         (unsigned long)cur, (unsigned)s_cpu->pc,
                         (unsigned long)(be - last_executed),
                         (unsigned long)(bc - last_compiled),
                         (unsigned long)(ch - last_chain_hits),
                         (unsigned long)(cm - last_chain_misses));
                last_compiled = bc; last_executed = be;
                last_chain_hits = ch; last_chain_misses = cm;
            } else {
                ESP_LOGI(TAG, "fps=%d ppu=%lu pc=%04X", current_fps,
                         (unsigned long)cur, (unsigned)s_cpu->pc);
            }
            next_log_ms += 1000;
        }
    }
}

void oled_task_start(struct cpu_state *cpu) {
    s_cpu = cpu;
    /* Pin to Core 1, sharing with ppu_thread. Both are mostly idle —
     * ppu_tick early-returns on every call where cpu->cycles hasn't
     * crossed the next state-machine deadline, and oled_task spends
     * most of its time in vTaskDelay between frames + blocked on the
     * I2C-master semaphore during a blit. Putting them both on Core 1
     * gives the dispatcher uncontested Core 0 for its busy loop. */
    xTaskCreatePinnedToCore(
        oled_task, "gbjit_oled", 4096, NULL,
        tskIDLE_PRIORITY + 1, NULL,
        1 /* Core 1 */);
}
