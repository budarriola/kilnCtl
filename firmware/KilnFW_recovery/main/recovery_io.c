// recovery_io.c -- see recovery_io.h.
#include "recovery_io.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "recovery_io";

// App/drivers/Kconfig defaults (KILNCTL_I2C_SDA_IO/SCL_IO, SX1509_I2C_ADDR,
// SX1509_RESET_IO).
#define I2C_PORT       I2C_NUM_0
#define I2C_SDA_GPIO   8
#define I2C_SCL_GPIO   9
#define I2C_CLK_HZ     100000
#define SX1509_ADDR    0x3Eu
#define SX1509_RESET_GPIO 10

// App/drivers/hw/SX1509.h register map. 16-bit values go out as one
// transfer starting at the bank-B (high byte, pins 15:8) address, high byte
// first (auto-increment is the power-on default).
#define SX1509_REG_DIR_B  0x0Eu
#define SX1509_REG_DATA_B 0x10u
#define SX1509_REG_RESET  0x7Du

// settings.h: SX1509_RELAY1_PIN..4 = 0..3; SX1509_LCD_DC_PIN = 15,
// SX1509_LCD_RESET_PIN = 14 (CONFIG_KILNCTL_DISPLAY_SWAP_DC_RESET off, the
// default, per DISPLAY_ST7796_WIRING.md).
#define RELAY_MASK   ((uint16_t)0x000Fu)
// IO5 = SX1509_IO2_PIN, the opto-isolated output to J25: kiln_io.c's
// KILN_IO_OUTPUT_MASK drives it as an output too (RegDir 0x3FD0 there with
// the LCD pins), so recovery does the same and holds it low.
#define IO2_J25_BIT  ((uint16_t)(1u << 5))
#define HOLD_MASK    ((uint16_t)(RELAY_MASK | IO2_J25_BIT))
#define LCD_DC_BIT   ((uint16_t)(1u << 15))
#define LCD_RST_BIT  ((uint16_t)(1u << 14))
#define LCD_MASK     ((uint16_t)(LCD_DC_BIT | LCD_RST_BIT))

// Output latches loaded before the pins become outputs: relays low, LCD D/C
// high (data) and ~RESET high (not held in reset) -- kiln_io.c's
// KILN_IO_SAFE_DATA. RegDir: 1 = input, so outputs are the cleared bits.
#define SAFE_DATA    ((uint16_t)LCD_MASK)
#define OUT_DIR_MASK ((uint16_t)(HOLD_MASK | LCD_MASK))
#define DIR_VALUE    ((uint16_t)~OUT_DIR_MASK)

#define HOLD_ATTEMPTS 3

static bool s_i2c_ready = false;
static bool s_verified = false;
static bool s_fault = true; // pessimistic until the hold is verified
static uint16_t s_data = SAFE_DATA;

static esp_err_t write16(uint8_t reg, uint16_t v)
{
    uint8_t buf[3] = {reg, (uint8_t)(v >> 8), (uint8_t)v};
    return i2c_master_write_to_device(I2C_PORT, SX1509_ADDR, buf, sizeof(buf), pdMS_TO_TICKS(100));
}

static esp_err_t read16(uint8_t reg, uint16_t *out)
{
    uint8_t rx[2] = {0, 0};
    esp_err_t err = i2c_master_write_read_device(I2C_PORT, SX1509_ADDR, &reg, 1, rx, 2,
                                                  pdMS_TO_TICKS(100));
    if (err == ESP_OK) {
        *out = (uint16_t)((rx[0] << 8) | rx[1]);
    }
    return err;
}

static esp_err_t i2c_bring_up(void)
{
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_CLK_HZ,
    };
    esp_err_t err = i2c_param_config(I2C_PORT, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_driver_install(I2C_PORT, cfg.mode, 0, 0, 0);
}

// Hard reset via the dedicated ~RESET GPIO (SX1509.c SX1509_reset(hard):
// 10 us pulse, then wait out t_RESET), then the software-reset magic pair as
// a belt-and-braces second path.
static void expander_reset(bool first)
{
    if (first) {
        gpio_set_level(SX1509_RESET_GPIO, 1);
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << SX1509_RESET_GPIO,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&io);
        gpio_set_level(SX1509_RESET_GPIO, 1);
    }
    gpio_set_level(SX1509_RESET_GPIO, 0);
    esp_rom_delay_us(10);
    gpio_set_level(SX1509_RESET_GPIO, 1);
    // CONFIG_FREERTOS_HZ=100 makes pdMS_TO_TICKS(5)==0, so busy-wait for real.
    esp_rom_delay_us(5000);

    uint8_t m1[2] = {SX1509_REG_RESET, 0x12};
    uint8_t m2[2] = {SX1509_REG_RESET, 0x34};
    if (i2c_master_write_to_device(I2C_PORT, SX1509_ADDR, m1, 2, pdMS_TO_TICKS(100)) == ESP_OK) {
        (void)i2c_master_write_to_device(I2C_PORT, SX1509_ADDR, m2, 2, pdMS_TO_TICKS(100));
        esp_rom_delay_us(5000);
    }
}

static bool hold_once(bool first)
{
    expander_reset(first);

    // Latches first (pins are inputs after reset, so nothing moves), then
    // directions: kiln_io_init()'s "data before dir" order.
    esp_err_t err = write16(SX1509_REG_DATA_B, SAFE_DATA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SX1509 0x%02X RegData write failed: %s", SX1509_ADDR, esp_err_to_name(err));
        return false;
    }
    err = write16(SX1509_REG_DIR_B, DIR_VALUE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SX1509 RegDir write failed: %s", esp_err_to_name(err));
        return false;
    }
    s_data = SAFE_DATA;

    uint16_t data = 0, dir = 0;
    if (read16(SX1509_REG_DATA_B, &data) != ESP_OK || read16(SX1509_REG_DIR_B, &dir) != ESP_OK) {
        ESP_LOGE(TAG, "SX1509 read-back failed");
        return false;
    }
    bool dir_ok = (uint16_t)(dir & OUT_DIR_MASK) == 0; // relay + LCD pins are outputs
    bool data_ok = (uint16_t)(data & HOLD_MASK) == 0; // relay pins read low
    if (!dir_ok || !data_ok) {
        ESP_LOGE(TAG, "RELAY HOLD MISMATCH: RegDir=0x%04X (want output bits 0x%04X clear) "
                      "RegData=0x%04X (want hold bits 0x%04X low)",
                 dir, OUT_DIR_MASK, data, HOLD_MASK);
        return false;
    }
    ESP_LOGI(TAG, "relays IO0..IO3 and IO5 held low and driven: RegDir=0x%04X RegData=0x%04X", dir, data);
    return true;
}

void recovery_io_hold_relays_off(void)
{
    esp_err_t err = i2c_bring_up();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bring-up failed: %s -- RELAY CTRL FAULT", esp_err_to_name(err));
        return;
    }
    s_i2c_ready = true;

    for (int i = 0; i < HOLD_ATTEMPTS; i++) {
        if (hold_once(i == 0)) {
            s_verified = true;
            s_fault = false;
            return;
        }
        ESP_LOGW(TAG, "relay hold attempt %d/%d failed", i + 1, HOLD_ATTEMPTS);
    }
    s_fault = true;
    ESP_LOGE(TAG, "RELAY CTRL FAULT: could not verify relays low after %d attempts. Recovery "
                  "keeps running (uploads must still work); heaters are NOT confirmed off "
                  "by this image.", HOLD_ATTEMPTS);
}

bool recovery_io_relay_fault(void)
{
    return s_fault;
}

bool recovery_io_relays_verified_off(void)
{
    return s_verified;
}

esp_err_t recovery_io_set_lcd_pins(bool dc_high, bool reset_high)
{
    if (!s_i2c_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    uint16_t next = (uint16_t)(s_data & ~LCD_MASK);
    if (dc_high) {
        next |= LCD_DC_BIT;
    }
    if (reset_high) {
        next |= LCD_RST_BIT;
    }
    if (next == s_data) {
        return ESP_OK;
    }
    // s_data's relay bits are always 0 (SAFE_DATA), so a single 16-bit write
    // can never raise a relay.
    esp_err_t err = write16(SX1509_REG_DATA_B, next);
    if (err == ESP_OK) {
        s_data = next;
    }
    return err;
}
