#ifndef BOARD_H
#define BOARD_H

/* HTIT-WB32LAF_V3.2 — ESP32-S3 dev board with onboard I2C SSD1306
 * 128x64 OLED. Pin map per the user-supplied wiring. */

#define BOARD_OLED_SDA        17
#define BOARD_OLED_SCL        18
#define BOARD_OLED_RST        21

/* Vext rail control on Heltec V3-class boards: pulling this GPIO LOW
 * powers the OLED + peripherals; it floats high (rail off) at boot. If
 * your variant doesn't have Vext-gated power, set BOARD_VEXT_PIN to -1
 * and the init path becomes a no-op. */
#define BOARD_VEXT_PIN        36
#define BOARD_VEXT_ON_LEVEL   0

#define BOARD_OLED_I2C_PORT   0
#define BOARD_OLED_I2C_ADDR   0x3C    /* SSD1306 default */
#define BOARD_OLED_I2C_HZ     400000  /* 400 kHz fast-mode — full-frame
                                       * blit (~1 KB) at this rate is
                                       * ~20 ms, fast enough for the GB's
                                       * 60 Hz frame rate. */

#define BOARD_OLED_WIDTH      128
#define BOARD_OLED_HEIGHT     64
#define BOARD_OLED_PAGES      (BOARD_OLED_HEIGHT / 8)

#endif
