// recovery_lcd.c -- see recovery_lcd.h.
#include "recovery_lcd.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "recovery_lcd";

// --- Real, hardware-confirmed pin facts (see recovery_lcd.h) -------------
#define LCD_SPI_HOST   SPI2_HOST
#define LCD_SCLK_GPIO  12
#define LCD_MOSI_GPIO  11
#define LCD_CS_GPIO    21
#define I2C_SDA_GPIO   8
#define I2C_SCL_GPIO   9
#define I2C_PORT       I2C_NUM_0
#define SX1509_ADDR    0x3Eu

// SX1509 register map subset (App/drivers/hw/SX1509.h)
#define SX1509_REG_DIR_B  0x0Eu
#define SX1509_REG_DATA_B 0x10u
#define SX1509_LCD_DC_BIT   (1u << 6) // pin 14 -> bank B bit 6
#define SX1509_LCD_RST_BIT  (1u << 7) // pin 15 -> bank B bit 7

// ILI9488 command set (subset)
#define ILI9488_CMD_SWRESET 0x01
#define ILI9488_CMD_SLPOUT  0x11
#define ILI9488_CMD_DISPON  0x29
#define ILI9488_CMD_CASET   0x2A
#define ILI9488_CMD_RASET   0x2B
#define ILI9488_CMD_RAMWR   0x2C
#define ILI9488_CMD_MADCTL  0x36
#define ILI9488_CMD_COLMOD  0x3A

#define PANEL_W 320
#define PANEL_H 480

static spi_device_handle_t s_spi;
static bool s_i2c_ready = false;
static uint8_t s_sx1509_data_shadow = 0xFF; // power-on default: all outputs high

static esp_err_t sx1509_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_write_to_device(I2C_PORT, SX1509_ADDR, buf, sizeof(buf), pdMS_TO_TICKS(100));
}

static esp_err_t sx1509_init_dc_rst(void)
{
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    esp_err_t err = i2c_param_config(I2C_PORT, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_driver_install(I2C_PORT, cfg.mode, 0, 0, 0);
    if (err != ESP_OK) {
        return err;
    }
    // Both DC and RESET as outputs (SX1509 RegDir: 1 = input, 0 = output --
    // opposite of the usual convention, see SX1509.h). Read-modify-write
    // against the bank's other bits (which this board does not use) would
    // need a read first; simplest safe assumption for a two-bit-only driver
    // is to drive only these two bits low (output) and leave the rest as
    // power-on default (all inputs), which is what writing 0xFF with these
    // two bits cleared achieves.
    uint8_t dir_b = (uint8_t)(0xFFu & ~(SX1509_LCD_DC_BIT | SX1509_LCD_RST_BIT));
    err = sx1509_write_reg(SX1509_REG_DIR_B, dir_b);
    if (err != ESP_OK) {
        return err;
    }
    s_i2c_ready = true;
    return ESP_OK;
}

static esp_err_t sx1509_set_pin(uint8_t bit, bool high)
{
    if (high) {
        s_sx1509_data_shadow |= bit;
    } else {
        s_sx1509_data_shadow &= (uint8_t)~bit;
    }
    return sx1509_write_reg(SX1509_REG_DATA_B, s_sx1509_data_shadow);
}

static esp_err_t lcd_dc(bool data)
{
    return sx1509_set_pin(SX1509_LCD_DC_BIT, data);
}

static esp_err_t lcd_reset_pulse(void)
{
    esp_err_t err = sx1509_set_pin(SX1509_LCD_RST_BIT, false);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    err = sx1509_set_pin(SX1509_LCD_RST_BIT, true);
    vTaskDelay(pdMS_TO_TICKS(120));
    return err;
}

static esp_err_t spi_init(void)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = LCD_MOSI_GPIO,
        .miso_io_num = -1,
        .sclk_io_num = LCD_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = PANEL_W * 3,
    };
    esp_err_t err = spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 20 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = LCD_CS_GPIO,
        .queue_size = 4,
    };
    return spi_bus_add_device(LCD_SPI_HOST, &dev_cfg, &s_spi);
}

static esp_err_t send_cmd(uint8_t cmd)
{
    esp_err_t err = lcd_dc(false);
    if (err != ESP_OK) {
        return err;
    }
    spi_transaction_t t = {.length = 8, .tx_buffer = &cmd};
    return spi_device_transmit(s_spi, &t);
}

static esp_err_t send_data(const uint8_t *buf, size_t len)
{
    if (len == 0) {
        return ESP_OK;
    }
    esp_err_t err = lcd_dc(true);
    if (err != ESP_OK) {
        return err;
    }
    spi_transaction_t t = {.length = len * 8, .tx_buffer = buf};
    return spi_device_transmit(s_spi, &t);
}

static esp_err_t write_cmd_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    esp_err_t err = send_cmd(cmd);
    if (err != ESP_OK) {
        return err;
    }
    return send_data(data, len);
}

static esp_err_t panel_init(void)
{
    esp_err_t err = write_cmd_data(ILI9488_CMD_SWRESET, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(150));

    uint8_t colmod = 0x66; // 18 bpp / 3 bytes-per-pixel -- the only mode
                            // confirmed on this board's actual ILI9488.
    err = write_cmd_data(ILI9488_CMD_COLMOD, &colmod, 1);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t madctl = 0x48; // portrait, RGB order -- unrotated native geometry
    err = write_cmd_data(ILI9488_CMD_MADCTL, &madctl, 1);
    if (err != ESP_OK) {
        return err;
    }
    err = write_cmd_data(ILI9488_CMD_SLPOUT, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    err = write_cmd_data(ILI9488_CMD_DISPON, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    return err;
}

static esp_err_t set_window(int x0, int y0, int x1, int y1)
{
    uint8_t caset[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
    uint8_t raset[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
    esp_err_t err = write_cmd_data(ILI9488_CMD_CASET, caset, sizeof(caset));
    if (err != ESP_OK) {
        return err;
    }
    return write_cmd_data(ILI9488_CMD_RASET, raset, sizeof(raset));
}

static esp_err_t fill_screen(uint8_t r, uint8_t g, uint8_t b)
{
    esp_err_t err = set_window(0, 0, PANEL_W - 1, PANEL_H - 1);
    if (err != ESP_OK) {
        return err;
    }
    err = send_cmd(ILI9488_CMD_RAMWR);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t line[PANEL_W * 3];
    for (int i = 0; i < PANEL_W; i++) {
        line[i * 3 + 0] = r;
        line[i * 3 + 1] = g;
        line[i * 3 + 2] = b;
    }
    for (int y = 0; y < PANEL_H; y++) {
        esp_err_t e = send_data(line, sizeof(line));
        if (e != ESP_OK) {
            return e;
        }
    }
    return ESP_OK;
}

// Tiny fixed 5x7 font, digits/uppercase/space/colon only -- enough for one
// short recovery-mode message. Each glyph is 5 columns of 7 bits (row 0 =
// top). Deliberately not a general font: this exists to draw exactly one
// string, not to become a text-rendering subsystem in the one image that
// must not accumulate surface area.
struct glyph { char c; uint8_t cols[5]; };
static const struct glyph FONT[] = {
    {'A', {0x7E,0x11,0x11,0x11,0x7E}}, {'C', {0x3E,0x41,0x41,0x41,0x22}},
    {'D', {0x7F,0x41,0x41,0x22,0x1C}}, {'E', {0x7F,0x49,0x49,0x49,0x41}},
    {'F', {0x7F,0x09,0x09,0x09,0x01}}, {'I', {0x00,0x41,0x7F,0x41,0x00}},
    {'K', {0x7F,0x08,0x14,0x22,0x41}}, {'M', {0x7F,0x02,0x04,0x02,0x7F}},
    {'N', {0x7F,0x04,0x08,0x10,0x7F}}, {'O', {0x3E,0x41,0x41,0x41,0x3E}},
    {'P', {0x7F,0x09,0x09,0x09,0x06}}, {'R', {0x7F,0x09,0x19,0x29,0x46}},
    {'S', {0x46,0x49,0x49,0x49,0x31}}, {'V', {0x1F,0x20,0x40,0x20,0x1F}},
    {'W', {0x3F,0x40,0x38,0x40,0x3F}}, {'Y', {0x07,0x08,0x70,0x08,0x07}},
    {'H', {0x7F,0x08,0x08,0x08,0x7F}}, {'T', {0x01,0x01,0x7F,0x01,0x01}},
    {'U', {0x3F,0x40,0x40,0x40,0x3F}}, {'G', {0x3E,0x41,0x49,0x49,0x3A}},
    {'.', {0x00,0x60,0x60,0x00,0x00}}, {' ', {0x00,0x00,0x00,0x00,0x00}},
};

static const uint8_t *glyph_cols(char c)
{
    for (size_t i = 0; i < sizeof(FONT) / sizeof(FONT[0]); i++) {
        if (FONT[i].c == c) {
            return FONT[i].cols;
        }
    }
    return NULL; // unknown char -- caller skips it (blank column run)
}

#define TEXT_SCALE 4
#define TEXT_FG_R 255
#define TEXT_FG_G 255
#define TEXT_FG_B 255
#define TEXT_BG_R 128
#define TEXT_BG_G 0
#define TEXT_BG_B 0

static esp_err_t draw_string(const char *s, int x0, int y0)
{
    int x = x0;
    for (const char *p = s; *p; p++) {
        const uint8_t *cols = glyph_cols(*p);
        for (int col = 0; col < 5; col++) {
            uint8_t bits = cols ? cols[col] : 0;
            esp_err_t err = set_window(x, y0, x + TEXT_SCALE - 1, y0 + 7 * TEXT_SCALE - 1);
            if (err != ESP_OK) {
                return err;
            }
            err = send_cmd(ILI9488_CMD_RAMWR);
            if (err != ESP_OK) {
                return err;
            }
            uint8_t px_fg[3] = {TEXT_FG_R, TEXT_FG_G, TEXT_FG_B};
            uint8_t px_bg[3] = {TEXT_BG_R, TEXT_BG_G, TEXT_BG_B};
            for (int row = 0; row < 7; row++) {
                const uint8_t *px = (bits & (1u << row)) ? px_fg : px_bg;
                uint8_t line[3 * TEXT_SCALE];
                for (int i = 0; i < TEXT_SCALE; i++) {
                    memcpy(&line[i * 3], px, 3);
                }
                for (int rep = 0; rep < TEXT_SCALE; rep++) {
                    err = send_data(line, sizeof(line));
                    if (err != ESP_OK) {
                        return err;
                    }
                }
            }
            x += TEXT_SCALE;
        }
        x += TEXT_SCALE; // one blank column of inter-glyph spacing
    }
    return ESP_OK;
}

void recovery_lcd_show_message(void)
{
    if (sx1509_init_dc_rst() != ESP_OK) {
        ESP_LOGE(TAG, "SX1509 bring-up failed -- panel stays whatever it last showed");
        return;
    }
    if (spi_init() != ESP_OK) {
        ESP_LOGE(TAG, "panel SPI bus init failed");
        return;
    }
    if (lcd_reset_pulse() != ESP_OK || panel_init() != ESP_OK) {
        ESP_LOGE(TAG, "panel init sequence failed");
        return;
    }
    if (fill_screen(TEXT_BG_R, TEXT_BG_G, TEXT_BG_B) != ESP_OK) {
        ESP_LOGE(TAG, "panel fill failed");
        return;
    }
    if (draw_string("RECOVERY MODE", 20, 200) != ESP_OK) {
        ESP_LOGE(TAG, "panel text draw failed");
        return;
    }
    (void)draw_string("PUSH FIRMWARE", 20, 200 + 7 * TEXT_SCALE + 16);
    ESP_LOGI(TAG, "recovery message drawn");
}
