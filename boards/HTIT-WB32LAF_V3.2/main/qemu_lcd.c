/* See qemu_lcd.h. */

#include "qemu_lcd.h"

#ifndef GBJIT_QEMU_LCD
#define GBJIT_QEMU_LCD 0
#endif

#if GBJIT_QEMU_LCD

#include "cpu_state.h"
#include "memory.h"

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

/* Game Boy DMG screen geometry. */
#define GB_W 160
#define GB_H 144
#define GB_PIXELS (GB_W * GB_H)
#define GB_PACKED (GB_PIXELS / 4)        /* 2 bits/px → 5760 bytes/frame */

/* Secondary UART carrying the framebuffer. UART0 stays the log/monitor
 * console; QEMU maps the 2nd `-serial` chardev to UART1. The TX/RX pin
 * numbers are irrelevant under QEMU (the peripheral is wired straight
 * to the chardev), but must be valid GPIOs for uart_set_pin to accept
 * them — 43/44 are the strapped UART0 pins' neighbours and unused here. */
#define QEMU_LCD_UART      UART_NUM_1
#define QEMU_LCD_TX_PIN    43
#define QEMU_LCD_RX_PIN    44

/* Per-frame packet: 4-byte magic then GB_PACKED bytes of 2-bpp pixels,
 * pixel 0 in the low bits of byte 0. The host viewer scans for the
 * magic to resync, so a dropped/partial packet self-heals. */
static const u8 QEMU_LCD_MAGIC[4] = { 'G', 'B', 'F', '1' };

static const char *TAG = "qemu_lcd";
static struct cpu_state *s_cpu;

static void qemu_lcd_task(void *arg) {
    (void)arg;

    const uart_config_t cfg = {
        .baud_rate  = 115200,             /* nominal — QEMU chardev is untimed */
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    /* TX-only: a generous TX ring (2x a packet) so uart_write_bytes
     * rarely blocks; no RX buffer needed. */
    if (uart_driver_install(QEMU_LCD_UART, 256, 2 * (GB_PACKED + 4), 0,
                            NULL, 0) != ESP_OK
        || uart_param_config(QEMU_LCD_UART, &cfg) != ESP_OK
        || uart_set_pin(QEMU_LCD_UART, QEMU_LCD_TX_PIN, QEMU_LCD_RX_PIN,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGE(TAG, "UART%d init failed — virtual LCD disabled",
                 QEMU_LCD_UART);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "virtual LCD streaming %dx%d on UART%d",
             GB_W, GB_H, QEMU_LCD_UART);

    static u8 packed[GB_PACKED];
    u32 last_seq = 0;
    while (1) {
        u32 cur = s_cpu->mmu->frame_seq;
        if (cur == last_seq) {
            vTaskDelay(1);                /* ~one tick; PPU still composing */
            continue;
        }
        last_seq = cur;

        /* Pack 4 pixels/byte. framebuffer holds post-palette shade
         * 0..3 per pixel; mask to be safe against stray high bits. */
        const u8 *fb = s_cpu->mmu->framebuffer;
        for (int i = 0; i < GB_PACKED; i++) {
            const u8 *q = &fb[i * 4];
            packed[i] = (u8)((q[0] & 3)
                           | ((q[1] & 3) << 2)
                           | ((q[2] & 3) << 4)
                           | ((q[3] & 3) << 6));
        }
        uart_write_bytes(QEMU_LCD_UART, QEMU_LCD_MAGIC, sizeof(QEMU_LCD_MAGIC));
        uart_write_bytes(QEMU_LCD_UART, packed, sizeof(packed));
    }
}

void qemu_lcd_start(struct cpu_state *cpu) {
    s_cpu = cpu;
    /* Core 1, same priority as the PPU/OLED tasks — it spends almost
     * all its time blocked in vTaskDelay between frames. */
    xTaskCreatePinnedToCore(qemu_lcd_task, "gbjit_qlcd", 4096, NULL,
                            tskIDLE_PRIORITY + 1, NULL, 1 /* Core 1 */);
}

#else  /* !GBJIT_QEMU_LCD */

void qemu_lcd_start(struct cpu_state *cpu) { (void)cpu; }

#endif
