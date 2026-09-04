// Datasheet: hardware/mainBoard/parts/TFT35-SPI/v2/Hardware/NS2009.PDF
// (NOT under hardware/datasheets/ -- an audit pass on 2026-08-24 concluded
// this part had no datasheet in the repo because it only looked there, and
// so could not verify the command bytes or the 12-bit unpack against the
// primary source. They were subsequently verified exact: address bytes
// Table 4 p13, command bytes Table 3 p11 / Table 5 p13, and the two-byte
// result layout Figure 8 p14.)
#include "NS2009.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"
#include "stack_margin.h"

static const char *TAG = "NS2009";

/* One command byte out, two data bytes back -- nothing here is ever close to
 * a bus that's momentarily busy with another device's transaction. Same
 * value and reasoning as SX1509_TIMEOUT_MS. */
#define NS2009_TIMEOUT_MS 1000

/* Long enough for one START/address/STOP, short enough that a part that
 * isn't there doesn't stall bring-up -- same as SX1509_PROBE_TIMEOUT_MS. */
#define NS2009_PROBE_TIMEOUT_MS 50

/* Shared bus clock, same reasoning as SX1509_I2C_CLK_HZ: no device on this
 * bus gets to quietly re-rate the wires for everyone else. */
#define NS2009_I2C_CLK_HZ I2C_MASTER_FREQ_HZ

static const uint8_t ns2009_addresses[NS2009_ADDR_COUNT] = {
    NS2009_ADDR_A0_LOW, NS2009_ADDR_A0_HIGH,
};

static esp_err_t ns2009_add_device(NS2009Class *t, uint8_t addr)
{
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = NS2009_I2C_CLK_HZ,
    };
    return i2c_master_bus_add_device(t->bus, &dev_config, &t->dev);
}

static esp_err_t ns2009_transfer(NS2009Class *t, const uint8_t *tx, size_t tx_len, uint8_t *rx,
                                 size_t rx_len)
{
    if (t->owner_initialized) {
        return i2c_owner_transfer(&t->owner, t->dev, tx, tx_len, rx, rx_len, NS2009_TIMEOUT_MS);
    }
    /* Fallback for an instance whose owner task failed to start: the raw
     * i2c_master calls still work, they just aren't serialized behind a
     * queue -- same fallback SX1509 uses. */
    if (tx && tx_len > 0 && rx && rx_len > 0) {
        return i2c_master_transmit_receive(t->dev, tx, tx_len, rx, rx_len, NS2009_TIMEOUT_MS);
    }
    return i2c_master_transmit(t->dev, tx, tx_len, NS2009_TIMEOUT_MS);
}

esp_err_t NS2009_init(NS2009Class *t, i2c_master_bus_handle_t bus, uint8_t addr)
{
    if (!t || !bus) return ESP_ERR_INVALID_ARG;

    if (t->dev || t->owner_initialized) {
        ESP_LOGE(TAG, "init called on an instance that is already up (0x%02X)", t->addr);
        return ESP_ERR_INVALID_STATE;
    }

    memset(t, 0, sizeof(*t));
    t->bus = bus;
    t->addr = addr;

    esp_err_t err = ns2009_add_device(t, addr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device(0x%02X) failed: %s", addr, esp_err_to_name(err));
        return err;
    }

    /* Independent i2c_owner on the same (already-existing) bus handle --
     * this driver never creates or destroys the bus itself, same as SX1509. */
    err = i2c_owner_init(&t->owner, bus, 8, 5, 3072, tskNO_AFFINITY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_owner_init failed: %s", esp_err_to_name(err));
        i2c_master_bus_rm_device(t->dev);
        t->dev = NULL;
        return err;
    }
    t->owner_initialized = true;

    /* i2c_owner_init() is shared with SX1509.c's IO-expander owner
     * (different stack_depth, 4096) and deliberately does NOT register
     * itself for stack-margin reporting -- see i2c_owner.c's comment at
     * its call site. Registering here, under a name distinct from
     * SX1509's, keeps the two independently identifiable in a
     * GET_STACK_MARGIN report instead of colliding under one shared
     * "i2c_owner" name. */
    stack_margin_register("i2c_owner_ns2009", &t->owner.task_handle, 3072);

    ESP_LOGI(TAG, "NS2009 initialized addr=0x%02X", addr);
    return ESP_OK;
}

esp_err_t NS2009_deinit(NS2009Class *t)
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

esp_err_t NS2009_start(NS2009Class *t, i2c_master_bus_handle_t bus)
{
    if (!t || !bus) return ESP_ERR_INVALID_ARG;

    for (size_t i = 0; i < NS2009_ADDR_COUNT; ++i) {
        uint8_t addr = ns2009_addresses[i];
        esp_err_t probe_err = i2c_master_probe(bus, addr, NS2009_PROBE_TIMEOUT_MS);
        if (probe_err == ESP_OK) {
            esp_err_t err = NS2009_init(t, bus, addr);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "NS2009_init(0x%02X) failed: %s", addr, esp_err_to_name(err));
                return err;
            }
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "no NS2009 at 0x%02X or 0x%02X -- touch controller absent, or SDA/SCL swapped "
                  "on J2 (see docs/HARDWARE.md); touch input unavailable this boot",
             NS2009_ADDR_A0_LOW, NS2009_ADDR_A0_HIGH);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t NS2009_read_axis(NS2009Class *t, uint8_t cmd, uint16_t *out_value)
{
    if (!t || !t->dev || !out_value) return ESP_ERR_INVALID_ARG;

    esp_err_t err = ESP_FAIL;
    uint8_t rx[2] = { 0, 0 };
    for (unsigned attempt = 1; attempt <= I2C_WRITE_RETRY_ATTEMPTS; ++attempt) {
        err = ns2009_transfer(t, &cmd, 1, rx, sizeof(rx));
        if (err == ESP_OK) {
            *out_value = (uint16_t)(((uint16_t)rx[0] << 4) | (rx[1] >> 4));
            return ESP_OK;
        }
        ESP_LOGW(TAG, "measure cmd 0x%02X failed (attempt %u/%u): %s", cmd, attempt,
                 (unsigned)I2C_WRITE_RETRY_ATTEMPTS, esp_err_to_name(err));
    }
    return err;
}

esp_err_t NS2009_read(NS2009Class *t, bool *out_pressed, uint16_t *out_x, uint16_t *out_y,
                       uint16_t *out_z1)
{
    if (!t || !out_pressed || !out_x || !out_y) return ESP_ERR_INVALID_ARG;

    uint16_t z1 = NS2009_ADC_MAX;
    esp_err_t err = NS2009_read_axis(t, NS2009_CMD_MEASURE_Z1, &z1);
    if (err != ESP_OK) return err;

    /* NOT "touch drives Z1 low, untouched floats near full scale" as
     * originally assumed -- bench testing 2026-08-19 (device log, NS2009 diagnostic)
     * found the opposite on this board revision: untouched floats near
     * *zero* (0-30 continuously with nothing touching the panel) and a real
     * firm press drove Z1 to 1047.
     *
     * Checked against the primary source 2026-08-24
     * (hardware/mainBoard/parts/TFT35-SPI/v2/Hardware/NS2009.PDF): the
     * datasheet never states what an UNTOUCHED Z1 measurement reads as. It
     * documents the touched-state mux/driver config for the Z1 ADC path
     * (Table 3) and, separately, the polarity of the digital PENIRQ pin
     * (touched = XP pulled low = PENIRQ low) -- but PENIRQ is a different
     * circuit, not this ADC value. So there was never a documented default
     * for the bench measurement to contradict; it is the only evidence
     * available, not a deviation from spec. So the gate compares the other
     * direction, and the threshold sits between those two measured bands
     * (KILNCTL_TOUCH_Z1_MAX_THRESHOLD's Kconfig help text has the numbers
     * and the reasoning for where the default landed). */
    uint16_t threshold = (uint16_t)TOUCH_Z1_MAX_THRESHOLD;
    bool pressed = (z1 > threshold);
    *out_pressed = pressed;
    if (out_z1) *out_z1 = z1;

    /* Routine polling chatter (2026-08-20, TODO.md diagnosability pass): this
     * fires roughly once per touch poll (~700ms). At ESP_LOGI with a
     * diag_counter % 20 gate it was still flooding both the device log ring
     * and the PC-side view badly enough to rotate out genuinely important
     * lines within seconds -- including the boot-time esp_reset_reason() line
     * in App/main.c, which is the whole reason a spontaneous-reboot
     * investigation went looking here. Steady-state "nothing is touching the
     * panel" is not worth INFO; a real press/release transition still is, so
     * that edge is logged separately at INFO below regardless of level. */
    static unsigned diag_counter = 0;
    if ((diag_counter++ % 20) == 0) {
        ESP_LOGD(TAG, "Z1=%u threshold=%u pressed=%d", z1, (unsigned)TOUCH_Z1_MAX_THRESHOLD, pressed);
    }

    static bool last_pressed = false;
    if (pressed != last_pressed) {
        ESP_LOGI(TAG, "touch %s (Z1=%u threshold=%u)", pressed ? "PRESSED" : "released", z1,
                 (unsigned)TOUCH_Z1_MAX_THRESHOLD);
        last_pressed = pressed;
    }

    if (!pressed) {
        *out_x = 0;
        *out_y = 0;
        return ESP_OK;
    }

    uint16_t x = 0, y = 0;
    err = NS2009_read_axis(t, NS2009_CMD_MEASURE_X, &x);
    if (err != ESP_OK) return err;
    err = NS2009_read_axis(t, NS2009_CMD_MEASURE_Y, &y);
    if (err != ESP_OK) return err;

    /* Per-sample coordinates while held are still routine chatter at the same
     * ~700ms cadence as Z1 above -- demoted alongside it. */
    ESP_LOGD(TAG, "raw_x=%u raw_y=%u z1=%u", x, y, z1);

    *out_x = x;
    *out_y = y;
    return ESP_OK;
}

esp_err_t NS2009_touch_dev_read(void *ctx, bool *out_pressed, uint16_t *out_x, uint16_t *out_y,
                                 uint16_t *out_z1)
{
    return NS2009_read((NS2009Class *)ctx, out_pressed, out_x, out_y, out_z1);
}
