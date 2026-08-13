// PCF8575 16-bit quasi-bidirectional I2C I/O expander driver.
//
// Like SSD1306 (and unlike DcDac) this driver does NOT create its own I2C bus:
// the ESP32's I2C0 peripheral is already claimed by DcDac_init, and ESP-IDF
// only allows one i2c_master_bus_handle_t per physical bus. The caller passes
// in the already-created bus handle; PCF8575_init attaches this device to it
// via i2c_master_bus_add_device() and creates its own i2c_owner_t wrapping the
// same bus handle, so its transfers get their own FIFO-ordered queue
// independent of any other device's traffic on the same wires.
//
// Addressing: the part's three address pins select one of eight addresses,
// 0x20 (A2=A1=A0=0, the pulled-down default on this board) through 0x27. All
// eight are supported -- the compile-time default comes from Kconfig
// (KILNCTL_PCF8575_I2C_ADDR), and PCF8575_set_address() re-targets a live
// driver instance at any of them at runtime without a reboot, which is what
// the UART bridge's SET_ADDRESS subcommand and the GUI's address selector use.
//
// Port semantics (from the datasheet, and the usual gotcha with this part):
// the pins are quasi-bidirectional, not a port with a separate direction
// register. Writing a 1 turns the strong pull-down off and leaves only a weak
// (~100 uA) current source pulling high -- that is the "input" state, and it
// is also the power-on state of all 16 pins. Writing a 0 drives the pin hard
// low. So to read a pin, first write a 1 to it (PCF8575_write_pin / the shadow
// register below), then read the port; a pin currently driven low by this
// device always reads back 0 regardless of what's wired to it.
#ifndef PCF8575_H
#define PCF8575_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "i2c_owner.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Address pins A2/A1/A0 select 0x20..0x27; all three are pulled down on this
 * board, giving 0x20. */
#define PCF8575_ADDR_MIN 0x20u
#define PCF8575_ADDR_MAX 0x27u
#define PCF8575_ADDR_COUNT (PCF8575_ADDR_MAX - PCF8575_ADDR_MIN + 1u)
#define PCF8575_DEFAULT_I2C_ADDR PCF8575_ADDR_MIN

#define PCF8575_PIN_COUNT 16u

/* Power-on state of every pin: high (weak pull-up / input). The shadow
 * register is seeded with this so the first read-modify-write (set/clear/
 * toggle a single pin) can't accidentally drive the other 15 pins low. */
#define PCF8575_PORT_POWER_ON_STATE 0xFFFFu

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    i2c_owner_t owner;
    bool owner_initialized;
    uint8_t addr;
    /* Last value written to the port. The PCF8575 has no readable output
     * register -- a read returns the *pin* states, which for a pin driven low
     * is indistinguishable from an external device pulling it low -- so
     * read-modify-write of a single pin has to work off this shadow, not off
     * a read-back. */
    uint16_t shadow;
} PCF8575Class;

/* Attaches the expander as another device on an already-created I2C bus (see
 * the header comment above). addr must be in 0x20..0x27. Does not touch the
 * pins: the port is left in whatever state it's already in (all-high after a
 * power-on reset), and the shadow is seeded to match. */
esp_err_t PCF8575_init(PCF8575Class *exp, i2c_master_bus_handle_t bus, uint8_t addr);
esp_err_t PCF8575_deinit(PCF8575Class *exp);

/* Single-call bootstrap used by app_main: PCF8575_init at the
 * Kconfig-configured address, plus one read-back to confirm the part actually
 * answers. Logs and returns the error rather than asserting -- the expander
 * isn't critical enough to abort app_main over. */
esp_err_t PCF8575_start(PCF8575Class *exp, i2c_master_bus_handle_t bus);

/* Re-target this instance at a different address (0x20..0x27) without a
 * reboot: removes the old i2c device handle and adds one for the new address,
 * keeping the same bus and owner task. The shadow is reset to the power-on
 * state, since the newly addressed part's outputs have nothing to do with the
 * old one's. No-op (ESP_OK) if addr is already the current address.
 *
 * The target address is probed first: if nothing answers there, the instance
 * is left on its current address and ESP_ERR_NOT_FOUND is returned, rather
 * than silently pointing at a part that isn't installed. */
esp_err_t PCF8575_set_address(PCF8575Class *exp, uint8_t addr);

/* Probe every address in 0x20..0x27 on `bus` and report which ones ACK.
 * out_addrs receives up to max_addrs of them; *out_count is always the number
 * actually found (never more than max_addrs). Either output may be NULL if not
 * wanted. Note this probes the bus directly rather than going through an
 * i2c_owner queue, exactly like i2c_scan_bus does -- fine between transfers,
 * but don't call it concurrently with heavy traffic on the same bus. */
esp_err_t PCF8575_scan(i2c_master_bus_handle_t bus, uint8_t *out_addrs, size_t max_addrs,
                       size_t *out_count);

/* Whole-port write. Bit N = pin N (bits 0-7 = P00-P07, bits 8-15 = P10-P17),
 * 1 = weak-high/input, 0 = driven low. Updates the shadow on success.
 *
 * Every write is read back and verified before returning: the bits written as
 * 0 must read back 0 (a driven-low pin always does). Bits written as 1 are not
 * checked -- they are the input state, so they legitimately read whatever is
 * wired to them. A NACK, a failed read-back or a mismatch is logged and the
 * whole write is retried, up to I2C_WRITE_RETRY_ATTEMPTS times; if it still
 * doesn't verify the last error is returned (ESP_ERR_INVALID_RESPONSE for a
 * mismatch). */
esp_err_t PCF8575_write_port(PCF8575Class *exp, uint16_t value);

/* Whole-port read: the actual pin states, which for any pin this device is
 * currently driving low always read 0 (see the header comment). */
esp_err_t PCF8575_read_port(PCF8575Class *exp, uint16_t *out_value);

/* Single-pin helpers. write_pin is a read-modify-write of the *shadow*, not of
 * the part, so it never clobbers pins based on a stale external reading. */
esp_err_t PCF8575_write_pin(PCF8575Class *exp, uint8_t pin, bool level);
esp_err_t PCF8575_read_pin(PCF8575Class *exp, uint8_t pin, bool *out_level);

/* Mask helpers, all shadow-based read-modify-write of the whole port. */
esp_err_t PCF8575_set_mask(PCF8575Class *exp, uint16_t mask);
esp_err_t PCF8575_clear_mask(PCF8575Class *exp, uint16_t mask);
esp_err_t PCF8575_toggle_mask(PCF8575Class *exp, uint16_t mask);

/* Last value written (see PCF8575Class::shadow) -- not a device read. */
uint16_t PCF8575_get_shadow(const PCF8575Class *exp);

#ifdef __cplusplus
}
#endif

#endif // PCF8575_H
