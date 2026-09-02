// See FT6336U.h for the full header comment, including exactly which
// vendor sources this was written from and the explicit UNVALIDATED ON
// HARDWARE warning -- repeated here because a reader of the .c is not
// guaranteed to have read the .h first:
//
//   *** UNVALIDATED ON HARDWARE. No MSP4031 module has ever been connected
//   *** to this board to run this code against real FT6336U silicon. Every
//   *** register address and read sequence below came from
//   *** Datasheets/4.0inch_SPI_Module_ST7796_MSP4030_MSP4031_V1.0_Keep/
//   *** (DFT6336UDataSheetV1.1.pdf, FT6336U_Register.xlsx, and the vendor's
//   *** own Demo_ESP32/FT6336-arduino/FT6336.cpp reference driver, which
//   *** this file's TD_STATUS/TOUCH1 read sequence follows directly), not
//   *** from bench measurement. This file is dead code at runtime on every
//   *** board that exists today -- nothing in main.c constructs an
//   *** FT6336UClass, and FT6336U_start's caller is nobody.
#include "FT6336U.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"
#include "touch_dev.h"

static const char *TAG = "FT6336U";

/* Same reasoning and same values as NS2009_TIMEOUT_MS/NS2009_PROBE_TIMEOUT_MS
 * -- one small register read/write, nothing here is ever close to a bus
 * that's momentarily busy with another device's transaction. */
#define FT6336U_TIMEOUT_MS 1000
#define FT6336U_PROBE_TIMEOUT_MS 50

/* Shared bus clock, same reasoning as NS2009_I2C_CLK_HZ: no device on this
 * bus gets to quietly re-rate the wires for everyone else. */
#define FT6336U_I2C_CLK_HZ I2C_MASTER_FREQ_HZ

static esp_err_t ft6336u_add_device(FT6336UClass *t)
{
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = FT6336U_ADDR,
        .scl_speed_hz = FT6336U_I2C_CLK_HZ,
    };
    return i2c_master_bus_add_device(t->bus, &dev_config, &t->dev);
}

static esp_err_t ft6336u_transfer(FT6336UClass *t, const uint8_t *tx, size_t tx_len, uint8_t *rx,
                                  size_t rx_len)
{
    if (t->owner_initialized) {
        return i2c_owner_transfer(&t->owner, t->dev, tx, tx_len, rx, rx_len, FT6336U_TIMEOUT_MS);
    }
    /* Fallback for an instance whose owner task failed to start -- same
     * fallback NS2009/SX1509 use. */
    if (tx && tx_len > 0 && rx && rx_len > 0) {
        return i2c_master_transmit_receive(t->dev, tx, tx_len, rx, rx_len, FT6336U_TIMEOUT_MS);
    }
    return i2c_master_transmit(t->dev, tx, tx_len, FT6336U_TIMEOUT_MS);
}

/* One register block read: write the 1-byte register address, then read
 * `len` bytes back -- vendor reference driver's readBlockData(). Retries on
 * a transport error, same policy as NS2009_read_axis. */
static esp_err_t ft6336u_read_reg(FT6336UClass *t, uint8_t reg, uint8_t *buf, size_t len)
{
    esp_err_t err = ESP_FAIL;
    for (unsigned attempt = 1; attempt <= I2C_WRITE_RETRY_ATTEMPTS; ++attempt) {
        err = ft6336u_transfer(t, &reg, 1, buf, len);
        if (err == ESP_OK) return ESP_OK;
        ESP_LOGW(TAG, "read reg 0x%02X (%u bytes) failed (attempt %u/%u): %s", reg,
                 (unsigned)len, attempt, (unsigned)I2C_WRITE_RETRY_ATTEMPTS, esp_err_to_name(err));
    }
    return err;
}

esp_err_t FT6336U_init(FT6336UClass *t, i2c_master_bus_handle_t bus)
{
    if (!t || !bus) return ESP_ERR_INVALID_ARG;

    if (t->dev || t->owner_initialized) {
        ESP_LOGE(TAG, "init called on an instance that is already up");
        return ESP_ERR_INVALID_STATE;
    }

    memset(t, 0, sizeof(*t));
    t->bus = bus;

    esp_err_t err = ft6336u_add_device(t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device(0x%02X) failed: %s", FT6336U_ADDR,
                 esp_err_to_name(err));
        return err;
    }

    /* Independent i2c_owner on the same (already-existing) bus handle --
     * this driver never creates or destroys the bus itself, same as
     * NS2009/SX1509. */
    err = i2c_owner_init(&t->owner, bus, 8, 5, 3072, tskNO_AFFINITY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_owner_init failed: %s", esp_err_to_name(err));
        i2c_master_bus_rm_device(t->dev);
        t->dev = NULL;
        return err;
    }
    t->owner_initialized = true;

    ESP_LOGI(TAG, "FT6336U initialized addr=0x%02X (UNVALIDATED ON HARDWARE)", FT6336U_ADDR);
    return ESP_OK;
}

esp_err_t FT6336U_deinit(FT6336UClass *t)
{
    if (!t) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ESP_OK;

    i2c_master_dev_handle_t dev = t->dev;
    t->dev = NULL;

    if (t->owner_initialized) {
        esp_err_t sub = i2c_owner_deinit(&t->owner);
        if (sub != ESP_OK) err = sub;
        t->owner_initialized = false;
    }
    if (dev) {
        esp_err_t sub = i2c_master_bus_rm_device(dev);
        if (sub != ESP_OK) err = sub;
    }
    /* The bus belongs to whoever created it; never delete it here. */
    return err;
}

esp_err_t FT6336U_start(FT6336UClass *t, i2c_master_bus_handle_t bus)
{
    if (!t || !bus) return ESP_ERR_INVALID_ARG;

    esp_err_t probe_err = i2c_master_probe(bus, FT6336U_ADDR, FT6336U_PROBE_TIMEOUT_MS);
    if (probe_err == ESP_OK) {
        esp_err_t err = FT6336U_init(t, bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "FT6336U_init failed: %s", esp_err_to_name(err));
            return err;
        }
        return ESP_OK;
    }

    /* Expected on every board that exists today: the MSP4031 module (the
     * only carrier of this part) is not connected -- see this file's
     * top-of-file UNVALIDATED note. Same "absent controller is not an
     * error, just no touch this boot" posture as NS2009_start. */
    ESP_LOGI(TAG, "no FT6336U at 0x%02X -- expected unless the MSP4031 module is connected "
                  "(see docs/DISPLAY_ST7796_PLAN.md); touch input via this controller "
                  "unavailable this boot",
             FT6336U_ADDR);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t FT6336U_read(FT6336UClass *t, bool *out_pressed, uint16_t *out_x, uint16_t *out_y,
                        uint16_t *out_z1)
{
    if (!t || !t->dev || !out_pressed || !out_x || !out_y) return ESP_ERR_INVALID_ARG;

    uint8_t status = 0;
    esp_err_t err = ft6336u_read_reg(t, FT6336U_REG_TD_STATUS, &status, 1);
    if (err != ESP_OK) return err;

    /* Low nibble is the valid touch-point count (vendor reference driver's
     * `touches = pointInfo; isTouched = touches > 0 && touches < 3`) --
     * FT6336U_MAX_VALID_TOUCH_COUNT bounds it the same way. */
    uint8_t touch_count = status & 0x0Fu;
    bool pressed = (touch_count > 0) && (touch_count <= FT6336U_MAX_VALID_TOUCH_COUNT);

    *out_pressed = pressed;
    /* Capacitive controller: no analog pressure channel to report. See
     * touch_dev.h's TOUCH_DEV_NO_PRESSURE_SENTINEL comment -- a fabricated
     * plausible-looking value here is exactly the bug class this sentinel
     * exists to avoid. */
    if (out_z1) *out_z1 = TOUCH_DEV_NO_PRESSURE_SENTINEL;

    if (!pressed) {
        *out_x = 0;
        *out_y = 0;
        return ESP_OK;
    }

    /* First touch point only -- 4-byte block at FT6336U_REG_TOUCH1_XH:
     * [0]=XH ([3:0]=X[11:8]), [1]=XL, [2]=YH ([3:0]=Y[11:8]), [3]=YL.
     * The event-flag bits in XH[7:6]/YH[7:6] are not decoded, same as the
     * vendor reference driver. The SECOND touch point (register block at
     * FT6336U_REG_TOUCH2_XH = 0x09) is deliberately never read: this
     * board's LVGL indev is LV_INDEV_TYPE_POINTER, single-point, so a
     * second point has nowhere to go -- see touch_dev.h. */
    uint8_t block[4] = { 0, 0, 0, 0 };
    err = ft6336u_read_reg(t, FT6336U_REG_TOUCH1_XH, block, sizeof(block));
    if (err != ESP_OK) return err;

    uint16_t x = (uint16_t)(((uint16_t)(block[0] & 0x0Fu) << 8) | block[1]);
    uint16_t y = (uint16_t)(((uint16_t)(block[2] & 0x0Fu) << 8) | block[3]);

    *out_x = x;
    *out_y = y;

    ESP_LOGD(TAG, "touch_count=%u x=%u y=%u", touch_count, x, y);
    return ESP_OK;
}

esp_err_t FT6336U_touch_dev_read(void *ctx, bool *out_pressed, uint16_t *out_x, uint16_t *out_y,
                                  uint16_t *out_z1)
{
    return FT6336U_read((FT6336UClass *)ctx, out_pressed, out_x, out_y, out_z1);
}
