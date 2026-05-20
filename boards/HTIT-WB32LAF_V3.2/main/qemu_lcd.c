/* See qemu_lcd.h. */

#include "qemu_lcd.h"

#ifndef GBJIT_QEMU_LCD
#define GBJIT_QEMU_LCD 0
#endif

#if GBJIT_QEMU_LCD

#include "cpu_state.h"
#include "memory.h"

#include "driver/uart.h"
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
static bool s_uart_ready = false;

/* Frame-complete callback. ppu.c invokes this on Core 0, from inside
 * ppu_tick, at the HBLANK→VBLANK transition of line 143 — the instant
 * the framebuffer holds a finished frame.
 *
 * Packing and streaming from here is *race-free by construction*: it
 * runs on the same core as the PPU, so the framebuffer cannot be
 * mid-update while we read it. The previous design ran this on a
 * Core-1 task that polled `frame_seq` and then read `framebuffer`
 * while Core 0's PPU was already drawing the next frame into it —
 * an unsynchronised cross-core read that tore the streamed image
 * (a flickering band near the top, where the new frame's freshly
 * drawn rows met the previous frame's). There is no second core in
 * this path now, so there is nothing to tear. */
static void qemu_lcd_frame_cb(struct mmu *m) {
    if (!s_uart_ready) return;
    static u8 packed[GB_PACKED];
    const u8 *fb = m->framebuffer;
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

void qemu_lcd_start(struct cpu_state *cpu) {
    const uart_config_t cfg = {
        .baud_rate  = 115200,             /* nominal — QEMU chardev is untimed */
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    /* TX-only: a generous TX ring (2x a packet) so uart_write_bytes from
     * the frame callback rarely blocks the dispatcher; no RX buffer. */
    if (uart_driver_install(QEMU_LCD_UART, 256, 2 * (GB_PACKED + 4), 0,
                            NULL, 0) != ESP_OK
        || uart_param_config(QEMU_LCD_UART, &cfg) != ESP_OK
        || uart_set_pin(QEMU_LCD_UART, QEMU_LCD_TX_PIN, QEMU_LCD_RX_PIN,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGE(TAG, "UART%d init failed — virtual LCD disabled",
                 QEMU_LCD_UART);
        return;
    }
    s_uart_ready = true;
    /* Stream each frame from the PPU's own core — see qemu_lcd_frame_cb. */
    cpu->mmu->frame_complete_cb = qemu_lcd_frame_cb;
    ESP_LOGI(TAG, "virtual LCD streaming %dx%d on UART%d (Core 0, sync)",
             GB_W, GB_H, QEMU_LCD_UART);
}

#else  /* !GBJIT_QEMU_LCD */

void qemu_lcd_start(struct cpu_state *cpu) { (void)cpu; }

#endif
