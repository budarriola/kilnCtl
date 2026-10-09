// NS2009 4-wire resistive touch screen controller with I2C interface, on the
// BIGTREETECH TFT35-SPI V2.1 panel (J2), alongside the ILI9488 this board
// already drives -- see ILI9488.h and docs/HARDWARE.md.
//
// Like SX1509 (this file follows that driver's shape closely), this does NOT
// create the I2C bus: the caller passes in the already-created handle and
// NS2009_start attaches this device to it via i2c_master_bus_add_device(),
// then creates its own i2c_owner_t wrapping the same bus handle so its
// transfers get their own FIFO-ordered queue independent of anything else on
// the wires (the SX1509 expander, in practice).
//
// Unlike SX1509, there is no register file and nothing to shadow: every
// transaction is "write one command byte, read back the conversion result",
// per NS2009 DataSheet V1.1 section "Digital Interface". The command byte
// selects which of X/Y/Z1/Z2 to measure and drives the panel's resistive
// switches for the duration of that one conversion -- there is no
// free-running or interrupt-driven mode this driver uses; NS2009_read_axis
// is a blocking, one-shot ADC read.
//
// Wiring is not fully settled on this board revision (docs/HARDWARE.md,
// "Display (J2)"): whether SDA/SCL are on J2 pins 2/3 as the board silkscreen
// names them or swapped, as the panel module's own pinout claims, is an open
// question the schematic and the module disagree on. This driver does not
// take a side -- NS2009_start probes both of the part's two possible I2C
// addresses (A0 strapped either way) on whatever bus it is given, exactly
// like SX1509_scan sweeps its four; if neither answers, the caller is told
// there is no touch controller rather than the driver guessing at a fix for
// a wiring problem it cannot see.
#ifndef NS2009_H
#define NS2009_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "i2c_owner.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Table 4 (address byte) of the datasheet: the top 5 bits are the fixed
 * code "10010", bit1 is the A0 strap, bit0 is R/W (handled by the I2C
 * peripheral's normal 7-bit addressing, not spelled out here). A0 low is
 * what every BIGTREETECH TFT35 schematic found for this module shows. */
#define NS2009_ADDR_A0_LOW  0x48u
#define NS2009_ADDR_A0_HIGH 0x49u
#define NS2009_ADDR_COUNT   2u

/* Table 5 (command byte): C3..C0 select the measurement, bit2 is PD0, bit1
 * is M (mode), bit0 is reserved (write 0). C2=1 selects "short driver, auto
 * power down, low power mode" (Table 3) -- the part re-enters power-down
 * between conversions on its own, which matters here because this driver
 * polls it from a task rather than reacting to PENIRQ. PD0=0 (Table 6)
 * leaves the pen-interrupt function enabled, for a future revision that
 * wires PENIRQ up; this driver does not use it. M=0 selects the full 12-bit
 * conversion. */
#define NS2009_CMD_MEASURE_X  0xC0u /* C3210 = 1100 */
#define NS2009_CMD_MEASURE_Y  0xD0u /* C3210 = 1101 */
#define NS2009_CMD_MEASURE_Z1 0xE0u /* C3210 = 1110 */
#define NS2009_CMD_MEASURE_Z2 0xF0u /* C3210 = 1111 */

/* 12-bit ADC, so a raw reading is always < 4096; anything read back at or
 * above this is either a bus error slipping through as zero-filled bytes
 * read as garbage, or the part not actually present. */
#define NS2009_ADC_MAX 4095u

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    i2c_owner_t owner;
    bool owner_initialized;
    uint8_t addr;
} NS2009Class;

/* Attach the touch controller as another device on an already-created I2C
 * bus, at a specific address. Does no conversion and touches no panel
 * state -- just brings the transport up, same division of labour as
 * SX1509_init. */
esp_err_t NS2009_init(NS2009Class *t, i2c_master_bus_handle_t bus, uint8_t addr);
esp_err_t NS2009_deinit(NS2009Class *t);

/* Single-call bootstrap used by app_main: probes NS2009_ADDR_A0_LOW then
 * NS2009_ADDR_A0_HIGH (whichever answers first wins) and NS2009_init's at
 * that address. Logs and returns ESP_ERR_NOT_FOUND rather than asserting if
 * neither answers -- covers both a genuinely unpopulated touch controller
 * and the open SDA/SCL-swap question from docs/HARDWARE.md, either of which
 * leaves this board's touch input absent but every other subsystem fine. */
esp_err_t NS2009_start(NS2009Class *t, i2c_master_bus_handle_t bus);

/* One blocking command+read cycle: write the measurement command byte, then
 * read back 2 bytes and unpack the left-justified 12-bit result (datasheet
 * "Read Command": "next 2 bytes is the 12bit ... redundant 4bits zero", i.e.
 * value = (byte0 << 4) | (byte1 >> 4)). `cmd` must be one of the
 * NS2009_CMD_MEASURE_* constants. Retries the transfer itself up to
 * I2C_WRITE_RETRY_ATTEMPTS on a transport error, same policy as SX1509's raw
 * accessors. */
esp_err_t NS2009_read_axis(NS2009Class *t, uint8_t cmd, uint16_t *out_value);

/* Convenience wrapper used by screen_idle: reads X, Y and Z1 in one call
 * (three back-to-back transactions -- there is no way to get all three from
 * the part in fewer, since each command byte selects a single measurement)
 * and applies the CONFIG_KILNCTL_TOUCH_Z1_MAX_THRESHOLD pressure gate (see
 * Kconfig) to decide *out_pressed. On THIS board revision a touch reads a
 * HIGH Z1, the opposite of the NS2009 datasheet's typical application --
 * see the Kconfig help text and NS2009.c's NS2009_read for the bench
 * measurements behind that. No physical unit conversion is attempted.
 * `out_z1` may be NULL for a caller that only wants pressed/x/y (screen_idle
 * doesn't care about the raw pressure count once *out_pressed is decided;
 * lvgl_port.c's touch calibration diagnostics do). */
esp_err_t NS2009_read(NS2009Class *t, bool *out_pressed, uint16_t *out_x, uint16_t *out_y,
                       uint16_t *out_z1);

/* touch_dev_read_fn-shaped adapter over NS2009_read() (touch_dev.h), so
 * main.c can build a touch_dev_t around an NS2009Class* the same way it does
 * for FT6336U -- see FT6336U.h's FT6336U_touch_dev_read(). */
esp_err_t NS2009_touch_dev_read(void *ctx, bool *out_pressed, uint16_t *out_x, uint16_t *out_y,
                                 uint16_t *out_z1);

#ifdef __cplusplus
}
#endif

#endif // NS2009_H
