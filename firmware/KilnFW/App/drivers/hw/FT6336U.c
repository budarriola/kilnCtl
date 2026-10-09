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

/* hal_status_t -> esp_err_t. This driver's public API is esp_err_t (unchanged
 * by the HAL migration, so main_boot_early.c's `== ESP_OK` checks and log
 * lines needed no ripple); hal_i2c.h speaks hal_status_t. No shared
 * hal_status_t->esp_err_t mapping exists yet anywhere in App/ (this is the
 * first App/ consumer of a hal_* interface) so this is deliberately local
 * and narrow rather than a speculative shared utility. Only the codes
 * hal_i2c.c/fake_i2c.c can actually return here are mapped explicitly. */
static esp_err_t ft6336u_hal_err(hal_status_t status)
{
    switch (status) {
        case HAL_OK: return ESP_OK;
        case HAL_TIMEOUT: return ESP_ERR_TIMEOUT;
        case HAL_INVALID_ARG: return ESP_ERR_INVALID_ARG;
        case HAL_NO_MEM: return ESP_ERR_NO_MEM;
        case HAL_NOT_READY: return ESP_ERR_INVALID_STATE;
        case HAL_NOT_FOUND: return ESP_ERR_NOT_FOUND;
        case HAL_NOT_SUPPORTED: return ESP_ERR_NOT_SUPPORTED;
        default: return ESP_FAIL; /* HAL_BUSY, HAL_IO, HAL_WEDGED, HAL_VERIFY_FAILED, HAL_INVALID_SIZE */
    }
}

static esp_err_t ft6336u_add_device(FT6336UClass *t)
{
    hal_status_t st = hal_i2c_device_attach(t->bus, &t->dev, FT6336U_ADDR, FT6336U_I2C_CLK_HZ);
    if (st == HAL_OK) t->dev_attached = true;
    return ft6336u_hal_err(st);
}

static esp_err_t ft6336u_transfer(FT6336UClass *t, const uint8_t *tx, size_t tx_len, uint8_t *rx,
                                  size_t rx_len)
{
    return ft6336u_hal_err(hal_i2c_transfer(&t->dev, tx, tx_len, rx, rx_len, FT6336U_TIMEOUT_MS));
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

/* PRECONDITION (opus review, J4b): `t` must be zero-initialized (static/
 * global storage, which the .bss segment zeroes for free, or an explicit
 * `= {0}` initializer) before the very first call. The already-up guard
 * right below reads t->dev_attached BEFORE this function's own memset() runs
 * -- deliberately, because the guard exists to stop a double-init call from
 * silently overwriting (and thereby leaking the I2C device handle of) an
 * already-LIVE instance, which memset-first would defeat. That means the
 * field must already be known-zero coming in on a genuinely fresh instance;
 * reading it off uninitialized stack memory instead is undefined behavior,
 * not merely "probably works" -- this is the file's caller contract, not a
 * bug this function can fix internally without an extra field. (NS2009.c has
 * this exact same shape/contract; left untouched here on purpose -- it is
 * the live driver on the only hardware this board has.) */
esp_err_t FT6336U_init(FT6336UClass *t, hal_i2c_bus_t *bus)
{
    if (!t || !bus) return ESP_ERR_INVALID_ARG;

    if (t->dev_attached) {
        ESP_LOGE(TAG, "init called on an instance that is already up");
        return ESP_ERR_INVALID_STATE;
    }

    memset(t, 0, sizeof(*t));
    t->bus = bus;

    /* No owner/queue setup here any more: hal_i2c_bus_init() (the caller's
     * job -- see this file's header comment) already brought up the backend's
     * owner task on `bus`. hal_i2c_device_attach() is this driver's only
     * remaining transport setup step. */
    esp_err_t err = ft6336u_add_device(t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hal_i2c_device_attach(0x%02X) failed: %s", FT6336U_ADDR,
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "FT6336U initialized addr=0x%02X (UNVALIDATED ON HARDWARE)", FT6336U_ADDR);
    return ESP_OK;
}

esp_err_t FT6336U_deinit(FT6336UClass *t)
{
    if (!t) return ESP_ERR_INVALID_ARG;

    /* hal_i2c_device_detach() (interface/hal_i2c.h) now exists -- release
     * the underlying device slot instead of leaking it. Reachable in
     * practice only from FT6336U_start()'s identity-check failure path
     * (this part has never been connected to any board -- see this file's
     * top-of-file UNVALIDATED note), but the shared I2C bus is used by
     * other devices too, so a leaked slot there is still worth avoiding.
     * Tolerate HAL_NOT_READY (already detached / never attached, e.g. a
     * caller that reaches FT6336U_deinit() before FT6336U_init() ever
     * attached anything) -- only log a real backend failure. */
    if (t->dev_attached) {
        hal_status_t st = hal_i2c_device_detach(&t->dev);
        if (st != HAL_OK && st != HAL_NOT_READY) {
            ESP_LOGE(TAG, "hal_i2c_device_detach failed: %s", esp_err_to_name(ft6336u_hal_err(st)));
        }
    }
    t->dev_attached = false;
    memset(&t->dev, 0, sizeof(t->dev));
    /* The bus belongs to whoever created it; never touch it here. */
    return ESP_OK;
}

/* Vendor reference driver's reset() (Demo_ESP32/FT6336-arduino/FT6336.cpp)
 * verifies three fixed identity bytes before trusting the part is really an
 * FT6336U -- i2c_master_probe() only proves SOMETHING answered at 0x38, not
 * WHAT. Values cross-checked by the opus review against both FT6336.cpp and
 * FT6336U_Register.xlsx (J4a).
 *
 * The vendor reset() also drives the RST pin low then high before this read
 * (its own hardware reset sequence) and gates on the INT pin. Neither RST
 * nor INT is wired on this board's harness -- see FT6336U.h's top-of-file
 * note, this part has never been connected -- so this driver does not
 * attempt to drive or read either pin; it relies on whatever power-up state
 * the part is already in when i2c_master_probe() finds it. If a real
 * MSP4031 module needs an explicit RST pulse to answer identity reads
 * correctly, that will show up here as ft6336u_verify_id() failing on real
 * hardware and is exactly the kind of thing DISPLAY_ST7796_PLAN.md's bench
 * bring-up step needs to catch. */
static esp_err_t ft6336u_verify_id(FT6336UClass *t)
{
    uint8_t id = 0;
    esp_err_t err = ft6336u_read_reg(t, FT6336U_REG_FOCALTECH_ID, &id, 1);
    if (err != ESP_OK) return err;
    if (id != FT6336U_EXPECT_FOCALTECH_ID) {
        ESP_LOGE(TAG, "FOCALTECH_ID mismatch: got 0x%02X, expected 0x%02X", id,
                 FT6336U_EXPECT_FOCALTECH_ID);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t cipher_mid = 0;
    err = ft6336u_read_reg(t, FT6336U_REG_CIPHER_MID, &cipher_mid, 1);
    if (err != ESP_OK) return err;
    if (cipher_mid != FT6336U_EXPECT_CIPHER_MID) {
        ESP_LOGE(TAG, "CIPHER_MID mismatch: got 0x%02X, expected 0x%02X", cipher_mid,
                 FT6336U_EXPECT_CIPHER_MID);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t cipher_high = 0;
    err = ft6336u_read_reg(t, FT6336U_REG_CIPHER_HIGH, &cipher_high, 1);
    if (err != ESP_OK) return err;
    if (cipher_high != FT6336U_EXPECT_CIPHER_HIGH) {
        ESP_LOGE(TAG, "CIPHER_HIGH mismatch: got 0x%02X, expected 0x%02X", cipher_high,
                 FT6336U_EXPECT_CIPHER_HIGH);
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

esp_err_t FT6336U_start(FT6336UClass *t, hal_i2c_bus_t *bus)
{
    if (!t || !bus) return ESP_ERR_INVALID_ARG;

    esp_err_t probe_err = ft6336u_hal_err(hal_i2c_probe(bus, FT6336U_ADDR, FT6336U_PROBE_TIMEOUT_MS));
    if (probe_err == ESP_OK) {
        esp_err_t err = FT6336U_init(t, bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "FT6336U_init failed: %s", esp_err_to_name(err));
            return err;
        }

        err = ft6336u_verify_id(t);
        if (err != ESP_OK) {
            /* Something answered at 0x38 but is not an FT6336U (or the
             * identity registers didn't read back as expected) -- back out
             * the transport we just brought up rather than leaving a
             * touch_dev_t pointed at a device that isn't what it claims to
             * be. */
            ESP_LOGE(TAG, "FT6336U identity check failed at 0x%02X: %s", FT6336U_ADDR,
                     esp_err_to_name(err));
            FT6336U_deinit(t);
            /* Distinct from the probe-miss NOT_FOUND below: main_boot_early.c
             * notes STARTUP_FAULT_TOUCH for any code except NOT_FOUND. */
            return ESP_ERR_INVALID_RESPONSE;
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
    if (!t || !t->dev_attached || !out_pressed || !out_x || !out_y) return ESP_ERR_INVALID_ARG;

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
