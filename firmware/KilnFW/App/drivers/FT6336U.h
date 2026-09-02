// FT6336U capacitive touch controller, I2C, on the MSP4031 module (NOT the
// hardware attached to this board today -- see docs/DISPLAY_ST7796_PLAN.md
// section "STOP -- read this before connecting the MSP4031 to J2" and
// section 7 "Touch abstraction"). The attached hardware is the resistive
// NS2009 (NS2009.h); this file exists so the driver is ready to be wired in
// on the day the MSP4031 module actually gets connected, and is unreachable
// dead code until then -- nothing in main.c constructs an FT6336UClass, and
// no Kconfig default selects it.
//
// UNVALIDATED ON HARDWARE. Written from the vendor material shipped with
// this panel family under
// firmware/KilnFW/Datasheets/4.0inch_SPI_Module_ST7796_MSP4030_MSP4031_V1.0_Keep/:
//   - 4-Driver_IC_Data_Sheet/DFT6336UDataSheetV1.1.pdf (register semantics)
//   - 4-Driver_IC_Data_Sheet/FT6336U_Register.xlsx (register map)
//   - Demo_ESP32/FT6336-arduino/FT6336.cpp,.h (the vendor's own reference
//     driver -- this file's register addresses and read sequence follow it
//     directly; see FT6336U.c's top-of-file comment for exactly which
//     numbers came from where)
// No MSP4031 module has ever been connected to this board to run any of
// this against real silicon. Treat every register address and bit as
// vendor-documentation-derived, not bench-proven, until it is.
//
// Shaped like NS2009.c on purpose (see that file's own header comment) --
// same division of labour: the caller passes in an already-created I2C bus
// handle, FT6336U_start attaches this device to it via
// i2c_master_bus_add_device() and creates its own i2c_owner_t wrapping the
// same bus handle for an independent FIFO-ordered transfer queue.
//
// Unlike NS2009, this part reports coordinates ALREADY IN PANEL SPACE (its
// own on-chip touch processor does the analog-to-pixel work) rather than
// raw ADC counts -- see touch_dev.h's header comment for why that makes
// this a `self_calibrating` touch_dev_t and why it must never be run
// through touch_cal_store.h's per-board affine fit.
#ifndef FT6336U_H
#define FT6336U_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "i2c_owner.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Vendor reference driver's FT6336_ADDR (FT6336.h) -- fixed, no strap
 * option documented for this part (contrast NS2009's two-address A0
 * strap). */
#define FT6336U_ADDR 0x38u

/* Register map, vendor reference driver (FT6336.h) cross-checked against
 * FT6336U_Register.xlsx:
 *   0x00  DEVICE_MODE -- normal operating mode is 0x00; not written by this
 *         driver (the part powers up in normal mode per the datasheet's
 *         power-up sequence; there is nothing here that needs it changed).
 *   0x02  TD_STATUS -- low nibble is the number of touch points currently
 *         valid (0-2; the datasheet documents 3+ as a transient invalid
 *         read, same as the vendor driver's `touches < 3` gate).
 *   0x03  TOUCH1_XH  -- first touch point, 4-byte block starting here:
 *         [0]=XH ([7:6]=event flag, [3:0]=X[11:8]), [1]=XL (X[7:0]),
 *         [2]=YH ([7:6]=touch ID, [3:0]=Y[11:8]), [3]=YL (Y[7:0]). Event
 *         flag / touch ID bits ([7:6] of XH and YH respectively) are not
 *         decoded by this driver (same as the vendor reference), only the
 *         12-bit X/Y pair.
 *   0x09  TOUCH2_XH -- second touch point, identical 4-byte layout. This
 *         driver deliberately never reads it: LVGL's indev here is
 *         LV_INDEV_TYPE_POINTER (single point), so a second touch point has
 *         nowhere to go -- see FT6336U_read()'s header comment.
 *   0xA8  FOCALTECH_ID -- fixed part-identity byte, expected 0x11.
 *   0x9F  CIPHER_MID -- fixed part-identity byte, expected 0x26.
 *   0xA3  CIPHER_HIGH -- fixed part-identity byte, expected 0x64.
 *         These three are read once by FT6336U_start() (mirroring the
 *         vendor reference driver's reset()/begin()) so that ANY I2C device
 *         that happens to answer at 0x38 is not silently accepted as an
 *         FT6336U -- i2c_master_probe() alone only proves something is
 *         there, not what it is. */
#define FT6336U_REG_DEVICE_MODE  0x00u
#define FT6336U_REG_TD_STATUS    0x02u
#define FT6336U_REG_TOUCH1_XH    0x03u
#define FT6336U_REG_CIPHER_MID   0x9Fu
#define FT6336U_REG_CIPHER_HIGH  0xA3u
#define FT6336U_REG_FOCALTECH_ID 0xA8u

/* Expected fixed identity byte values -- vendor reference driver's reset()
 * (Demo_ESP32/FT6336-arduino/FT6336.cpp), cross-checked against
 * FT6336U_Register.xlsx. */
#define FT6336U_EXPECT_FOCALTECH_ID 0x11u
#define FT6336U_EXPECT_CIPHER_MID   0x26u
#define FT6336U_EXPECT_CIPHER_HIGH  0x64u

/* TD_STATUS low nibble: 0 = no touch, 1-2 = that many valid points, 3+ is
 * documented by the vendor reference driver as a transient/invalid read
 * (its `isTouched = touches > 0 && touches < 3` gate) rather than "3 fingers
 * down" -- this part supports at most 2 simultaneous points. */
#define FT6336U_MAX_VALID_TOUCH_COUNT 2u

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    i2c_owner_t owner;
    bool owner_initialized;
} FT6336UClass;

/* Same division of labour as NS2009_init: brings up the transport only,
 * touches no touch-controller state. */
esp_err_t FT6336U_init(FT6336UClass *t, i2c_master_bus_handle_t bus);
esp_err_t FT6336U_deinit(FT6336UClass *t);

/* Single-call bootstrap, same shape as NS2009_start: probes FT6336U_ADDR
 * (this part has only the one address -- no A0 strap to sweep) and
 * FT6336U_init's if it answers. Logs and returns ESP_ERR_NOT_FOUND rather
 * than asserting if it does not -- expected outcome on every board that
 * exists today, since the MSP4031 module is not connected. */
esp_err_t FT6336U_start(FT6336UClass *t, i2c_master_bus_handle_t bus);

/* One register-read cycle: TD_STATUS, then (if a valid point count) the
 * first touch point's 4-byte block. The second touch point is deliberately
 * never read -- see FT6336U_H's REG_TOUCH2_XH comment. `out_x`/`out_y` are
 * the panel-space coordinates this part reports directly (12-bit,
 * 0-4095 range per the register layout, but in practice bounded by the
 * panel's real resolution) -- NOT raw ADC counts, and NOT run through any
 * calibration fit; see touch_dev.h. `out_z1`, if non-NULL, is always set to
 * TOUCH_DEV_NO_PRESSURE_SENTINEL (touch_dev.h): this is a capacitive
 * controller, it has no pressure channel, and reporting a plausible-looking
 * fabricated number in that slot is exactly the "consumer without a
 * producer" bug class this codebase has already shipped four times. */
esp_err_t FT6336U_read(FT6336UClass *t, bool *out_pressed, uint16_t *out_x, uint16_t *out_y,
                        uint16_t *out_z1);

/* touch_dev_read_fn-shaped adapter over FT6336U_read(), so lvgl_port.c can
 * build a touch_dev_t around an FT6336UClass* the same way it does for
 * NS2009 -- see touch_dev.h. */
esp_err_t FT6336U_touch_dev_read(void *ctx, bool *out_pressed, uint16_t *out_x, uint16_t *out_y,
                                  uint16_t *out_z1);

#ifdef __cplusplus
}
#endif

#endif // FT6336U_H
