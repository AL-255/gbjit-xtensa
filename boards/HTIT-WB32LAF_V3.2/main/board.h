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
#define BOARD_OLED_I2C_HZ     1000000 /* 1 MHz Fast-mode+. SSD1306 datasheet
                                       * §"AC characteristics" rates the
                                       * I²C interface to 400 kHz @ 5 V but
                                       * the chip-internal timing has plenty
                                       * of headroom — Heltec's V3 OLED
                                       * driven by the S3's native I²C
                                       * controller is reliable up to ~1 MHz.
                                       * Full-frame blit (1024 bytes payload
                                       * + control + ACK overhead) drops
                                       * from ~23 ms at 400 kHz to ~10 ms
                                       * here, raising the OLED refresh
                                       * ceiling from ~50 fps to ~100 fps
                                       * so the I²C path stops being the
                                       * scroll-smoothness bottleneck.
                                       * If your panel garbles at this
                                       * rate, drop back to 400000 (or
                                       * try 800000 as a middle ground). */

#define BOARD_OLED_WIDTH      128
#define BOARD_OLED_HEIGHT     64
#define BOARD_OLED_PAGES      (BOARD_OLED_HEIGHT / 8)

#endif
