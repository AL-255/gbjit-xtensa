/* See oled_task.h. */

#include "oled_task.h"
#include "board.h"
#include "ssd1306.h"

#include "cpu_state.h"
#include "memory.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "oled";

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
    uint32_t frames_pushed = 0;
    uint32_t next_log_ms = 1000;
    uint32_t boot_ms = (uint32_t)(esp_log_timestamp());
    while (1) {
        uint32_t cur = s_cpu->mmu->frame_seq;
        if (cur != last_seq) {
            last_seq = cur;
            compose_frame(s_cpu->mmu->framebuffer);
            ssd1306_blit(s_page_buf);
            frames_pushed++;
        } else {
            vTaskDelay(pdMS_TO_TICKS(4));
        }
        uint32_t now_ms = (uint32_t)(esp_log_timestamp());
        if (now_ms - boot_ms >= next_log_ms) {
            ESP_LOGI(TAG, "frames=%lu  ppu_seq=%lu  pc=0x%04X",
                     (unsigned long)frames_pushed,
                     (unsigned long)cur,
                     (unsigned)s_cpu->pc);
            next_log_ms += 1000;
        }
    }
}

void oled_task_start(struct cpu_state *cpu) {
    s_cpu = cpu;
    /* Pin to Core 0: Core 1 already hosts ppu_thread (with PPU async
     * enabled). Core 0 runs the dispatcher; the OLED task here only
     * wakes up every ~16ms to push a frame, so the dispatcher's hot
     * loop is barely impacted. */
    xTaskCreatePinnedToCore(
        oled_task, "gbjit_oled", 4096, NULL,
        tskIDLE_PRIORITY + 1, NULL,
        0 /* Core 0 */);
}
