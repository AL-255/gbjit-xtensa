/* Minimal SSD1306 128x64 driver over I2C (ESP-IDF v6 new i2c_master API).
 *
 * Init sequence + page-by-page frame push. No partial updates; we always
 * blit a full 128x8-page buffer. The display is set up in horizontal
 * addressing mode so a single 1024-byte transfer fills the screen. */

#ifndef SSD1306_H
#define SSD1306_H

#include <stdint.h>
#include <stdbool.h>

/* Initialise the I2C bus (GPIOs from board.h) and the OLED. Returns
 * true on success. Idempotent: safe to call twice. */
bool ssd1306_init(void);

/* Push a fully-prepared 1bpp framebuffer (1024 bytes = 128 cols × 8
 * pages). Bit layout matches the SSD1306's GDDRAM page format:
 * page p, column c, bit y%8 corresponds to pixel (c, p*8 + y%8). */
void ssd1306_blit(const uint8_t *page_buf_1024);

/* Clear the display (writes a zeroed buffer). */
void ssd1306_clear(void);

#endif
