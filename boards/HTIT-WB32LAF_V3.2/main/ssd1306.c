/* See ssd1306.h. */

#include "ssd1306.h"
#include "board.h"

#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "ssd1306";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_inited;

/* Control-byte prefixes (see SSD1306 datasheet §"I2C-bus interface").
 * Co=0, D/C# = 0: command stream; Co=0, D/C# = 1: data stream. */
#define SSD1306_CTRL_CMD   0x00
#define SSD1306_CTRL_DATA  0x40

static bool ssd1306_cmd(uint8_t c) {
    uint8_t buf[2] = { SSD1306_CTRL_CMD, c };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100) == ESP_OK;
}

static bool ssd1306_cmds(const uint8_t *cmds, size_t n) {
    /* Single transmit with a leading control byte tells the chip
     * "everything that follows is a command stream until STOP". */
    static uint8_t scratch[40];
    if (n + 1 > sizeof(scratch)) return false;
    scratch[0] = SSD1306_CTRL_CMD;
    memcpy(scratch + 1, cmds, n);
    return i2c_master_transmit(s_dev, scratch, n + 1, 100) == ESP_OK;
}

/* Power up Vext, then issue the hardware reset pulse on OLED_RST.
 * Heltec V3-family boards gate OLED + peripherals on a MOSFET driven by
 * GPIO36; that pin floats high at boot (rail OFF), so I2C transactions
 * before this point ACK nothing. Setting it LOW switches the rail on,
 * then we wait ~50 ms for the OLED's internal regulator to come up
 * before talking to it. */
static void ssd1306_hw_reset(void) {
#if BOARD_VEXT_PIN >= 0
    gpio_config_t vext = {
        .pin_bit_mask = 1ULL << BOARD_VEXT_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&vext);
    gpio_set_level(BOARD_VEXT_PIN, BOARD_VEXT_ON_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(50));
#endif

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_OLED_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(BOARD_OLED_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_OLED_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_OLED_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

bool ssd1306_init(void) {
    if (s_inited) return true;

    ssd1306_hw_reset();

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = BOARD_OLED_I2C_PORT,
        .scl_io_num = BOARD_OLED_SCL,
        .sda_io_num = BOARD_OLED_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed");
        return false;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_OLED_I2C_ADDR,
        .scl_speed_hz = BOARD_OLED_I2C_HZ,
    };
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed");
        return false;
    }

    /* Confirm the OLED actually answered before sending init bytes. If
     * the targeted address doesn't ACK, sweep the bus once for a
     * diagnostic — wiring/Vext/pull-up problems show up here instead of
     * as a confusing init-stream failure further down. */
    if (i2c_master_probe(s_bus, BOARD_OLED_I2C_ADDR, 50) != ESP_OK) {
        ESP_LOGW(TAG, "no ACK at 0x%02X — bus scan follows:", BOARD_OLED_I2C_ADDR);
        int found = 0;
        for (uint8_t a = 0x08; a < 0x78; a++) {
            if (i2c_master_probe(s_bus, a, 30) == ESP_OK) {
                ESP_LOGW(TAG, "  i2c device at 0x%02X", a);
                found++;
            }
        }
        if (!found) ESP_LOGE(TAG, "  bus silent — check Vext / pull-ups / RST pin");
        return false;
    }

    /* SSD1306 init sequence — standard 128x64 power-up. Adapted from
     * the datasheet "Application Example" and Adafruit_SSD1306.cpp. */
    static const uint8_t init_seq[] = {
        0xAE,                /* display off */
        0xD5, 0x80,          /* clock div */
        0xA8, 0x3F,          /* multiplex = 64 */
        0xD3, 0x00,          /* display offset = 0 */
        0x40,                /* start line = 0 */
        0x8D, 0x14,          /* charge pump on */
        0x20, 0x00,          /* memory mode = horizontal */
        0xA1,                /* segment remap (col 127 → SEG0) */
        0xC8,                /* COM scan dec (row 63 at top) */
        0xDA, 0x12,          /* COM pins alt, no remap */
        0x81, 0xCF,          /* contrast */
        0xD9, 0xF1,          /* precharge */
        0xDB, 0x40,          /* vcomh */
        0xA4,                /* output follows RAM (not all-on) */
        0xA6,                /* normal (not inverted) */
        0xAF,                /* display on */
    };
    if (!ssd1306_cmds(init_seq, sizeof(init_seq))) {
        ESP_LOGE(TAG, "init cmd stream failed");
        return false;
    }
    s_inited = true;
    ESP_LOGI(TAG, "ssd1306 ready on i2c0 sda=%d scl=%d rst=%d",
             BOARD_OLED_SDA, BOARD_OLED_SCL, BOARD_OLED_RST);
    return true;
}

void ssd1306_blit(const uint8_t *page_buf_1024) {
    if (!s_inited) return;
    /* Window the whole display: column 0..127, page 0..7. */
    uint8_t addr[] = {
        0x21, 0, 127,        /* column address: start, end */
        0x22, 0, 7,          /* page address: start, end */
    };
    if (!ssd1306_cmds(addr, sizeof(addr))) return;

    /* 1024-byte data burst with one leading data-control byte. We
     * use a stack-allocated 1025-byte scratch — the OLED task has 4 KB
     * of stack which is plenty. */
    uint8_t buf[1 + 1024];
    buf[0] = SSD1306_CTRL_DATA;
    memcpy(buf + 1, page_buf_1024, 1024);
    i2c_master_transmit(s_dev, buf, sizeof(buf), 200);
}

void ssd1306_clear(void) {
    if (!s_inited) return;
    static uint8_t zero[1024] = {0};
    ssd1306_blit(zero);
}
