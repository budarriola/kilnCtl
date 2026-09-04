// SX1509 write/config API -- owner-only. ROADMAP.md M15 A1.
//
// SX1509.h's INCLUDE_DIRS "." makes every .c file in this component able to
// quote-include SX1509.h (or, before this split, see the whole raw driver)
// regardless of any component-level PRIV_INCLUDE_DIRS setting -- the
// component is one flat directory, and #include "..." always searches the
// including file's own directory first. PRIV_INCLUDE_DIRS only fences off
// OTHER components; it cannot fence off sibling .c files in this one. History
// (TODO.md/ROADMAP.md, six incidents) is a research pass that found relay/IO
// writes reaching the expander directly instead of through kiln_io_owner.c,
// because nothing stopped a file from including SX1509.h and calling
// SX1509_write_reg()/SX1509_set_dir()/etc for itself.
//
// So the fence here is a compile-time guard instead of a directory boundary:
// every declaration below is behind SX1509_OWNER_BUILD, which only
// kiln_io.c, kiln_io_owner.c, SX1509.c itself, and main.c's I2C bring-up
// define (each with `#define SX1509_OWNER_BUILD` immediately before this
// include). Any other .c file that includes this header gets a hard #error
// naming the module to go through instead, rather than a silent extra
// capability. It's a speed bump, not a sandbox -- nothing stops a file from
// adding the #define itself -- but it turns "reach past kiln_io_owner" from
// a one-line accident into a one-line lie a reviewer/diff can see.
#ifndef SX1509_INTERNAL_H
#define SX1509_INTERNAL_H

#ifndef SX1509_OWNER_BUILD
#error "SX1509_internal.h is owner-only (kiln_io.c/kiln_io_owner.c/SX1509.c/main.c bring-up). Route relay/IO writes through kiln_io_owner.c instead of including this header -- see its top-of-file comment and ROADMAP.md M15 A1."
#endif

#include "SX1509.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Attach the expander as another device on an already-created I2C bus. addr
 * must be one of the four SX1509_ADDR_* values. irq_gpio / reset_gpio may be
 * -1 if not wired; on this board they are GPIO7 and GPIO10.
 *
 * Does NOT touch the expander's pins: the part comes out of reset with every
 * pin an input, which is the safe state, and yanking a relay gate or the
 * display's ~RESET around just because the firmware restarted is exactly the
 * glitch this ordering exists to avoid. Configuring the board is kiln_io_init's
 * job, and it does it in the safe order. */
esp_err_t SX1509_init(SX1509Class *e, i2c_master_bus_handle_t bus, uint8_t addr, int irq_gpio,
                      int reset_gpio);
esp_err_t SX1509_deinit(SX1509Class *e);

/* Single-call bootstrap used by app_main: probe at the Kconfig-configured
 * address (SX1509_I2C_ADDR), SX1509_init with the Kconfig IRQ/RESET GPIOs, then
 * one register read-back so a part that's configured but not actually present
 * shows up as an error line at boot -- forwarded to the PC like any other
 * ESP_LOGx -- rather than only failing later on the first command from the GUI.
 * Logs and returns the error rather than asserting. */
esp_err_t SX1509_start(SX1509Class *e, i2c_master_bus_handle_t bus);

/* Re-target this instance at a different address without a reboot: swaps the
 * I2C device handle, keeping the same bus and owner task. The target is probed
 * first -- if nothing answers there the instance stays where it is and
 * ESP_ERR_NOT_FOUND comes back, rather than silently pointing at a part that
 * isn't installed and then burning a bus reset plus a retry on every
 * subsequent transfer. All shadows are reset to the power-on values, since a
 * different chip's registers have nothing to do with the old one's. */
esp_err_t SX1509_set_address(SX1509Class *e, uint8_t addr);

/* Probe all four possible addresses on `bus` and report which ACK. out_addrs
 * receives up to max_addrs of them; *out_count is always the number actually
 * found (never more than max_addrs). Either output may be NULL. Probes the bus
 * directly rather than through an i2c_owner queue, exactly like i2c_scan_bus --
 * fine between transfers, but don't call it concurrently with heavy traffic. */
esp_err_t SX1509_scan(i2c_master_bus_handle_t bus, uint8_t *out_addrs, size_t max_addrs,
                      size_t *out_count);

/* --- Raw register access. The escape hatch behind the UART's SX_WRITE_REG /
 * SX_READ_REG debug subcommands, reached only through kiln_io_owner.c's
 * kiln_io_owner_command_sx_write_reg(). ---
 *
 * SX1509_write_reg does NOT verify: it is generic, and only the caller knows
 * whether the register it just wrote is one the part will read back unchanged
 * (RegDir, RegPullUp, ...) or one where a read means something else entirely
 * (RegData reads pins, RegInterruptSource is write-1-to-clear, RegReset reads
 * nothing useful). It does retry the transfer itself up to
 * I2C_WRITE_RETRY_ATTEMPTS on a transport error. The typed setters below are
 * the ones that verify.
 *
 * It does keep the shadows honest: a raw write to RegData/RegDir/RegPullUp/
 * RegPullDown/RegOpenDrain/RegLEDDriverEnable updates the matching shadow byte,
 * so poking a register from the PC can't desynchronize the driver. */
esp_err_t SX1509_write_reg(SX1509Class *e, uint8_t reg, uint8_t value);

/* 16-bit register pair WRITE. `reg` is the BANK B (lower) address of the pair,
 * e.g. SX1509_REG_DATA_B. The value goes on the wire high byte first -- bank B
 * holds I/O[15:8] and sits at the lower address, so auto-increment delivers it
 * first. Bit N of `value` is always pin N regardless of that. */
esp_err_t SX1509_write_reg16(SX1509Class *e, uint8_t reg, uint16_t value);

/* --- Pin configuration. All of these are whole-port writes that are read back
 * and compared exactly before returning; a NACK, a failed read-back or a
 * mismatch is logged and the write retried up to I2C_WRITE_RETRY_ATTEMPTS,
 * after which the last error is returned (ESP_ERR_INVALID_RESPONSE for a
 * mismatch). Unlike RegData these are plain R/W configuration registers with
 * nothing external able to influence what they read back, so an exact compare
 * is meaningful. --- */

/* bit = 1 -> input, bit = 0 -> output. This is the part's own RegDir polarity,
 * kept rather than inverted so that anything read off the wire or out of a
 * register dump means the same thing everywhere. */
esp_err_t SX1509_set_dir(SX1509Class *e, uint16_t dir_mask);
esp_err_t SX1509_set_pullup(SX1509Class *e, uint16_t mask);
esp_err_t SX1509_set_pulldown(SX1509Class *e, uint16_t mask);
esp_err_t SX1509_set_open_drain(SX1509Class *e, uint16_t mask);

/* Hardware debouncer. enable_mask is per-pin (and only means anything for pins
 * configured as inputs); config is RegDebounceConfig[2:0], giving
 * 0.5 ms << config at the internal 2 MHz oscillator -- 0 = 0.5 ms up to
 * 7 = 64 ms. Enabling any debouncing switches the internal oscillator on if it
 * isn't already, because the debouncer is clocked from it and does nothing at
 * all while RegClock[6:5] = OFF. */
esp_err_t SX1509_set_debounce(SX1509Class *e, uint16_t enable_mask, uint8_t config);

/* Interrupt configuration.
 *   mask  -- RegInterruptMask, bit = 1 *disables* the interrupt for that pin.
 *            The part's polarity, kept as-is (and the reset value is 0xFFFF,
 *            i.e. everything masked).
 *   sense -- RegSenseHighB/LowB/HighA/LowA flattened into one 32-bit field with
 *            pin N at bits [2N+1:2N]: 0 none, 1 rising, 2 falling, 3 both. Use
 *            SX1509_SENSE_FOR_PIN(). The four registers are consecutive from
 *            0x14 downward in pin order (0x14 = I/O[15:12] ... 0x17 =
 *            I/O[3:0]), which is exactly a 32-bit big-endian write of that
 *            field -- the same "high pins live at the low address" rule as the
 *            16-bit pairs.
 * A pin with sense 0 never raises anything no matter what the mask says. */
esp_err_t SX1509_set_interrupt(SX1509Class *e, uint16_t mask, uint32_t sense);

/* --- Port I/O writes. --- */

/* Whole-port write of RegData. bit N = pin N. Verified on read-back, but only
 * over the bits it is *honest* to verify: pins configured as outputs, not
 * open-drain, and not currently driven by the LED driver. A read of RegData
 * returns pin states, so an input pin reads the outside world, an open-drain
 * pin driven "high" reads whatever is pulling it, and an LED-driven pin reads a
 * sample of a PWM waveform -- none of those disagreeing with the written value
 * is a fault. With no verifiable bits the ACK alone is the check. */
esp_err_t SX1509_write_port(SX1509Class *e, uint16_t value);

/* Atomic read-modify-write against the *shadow*: only the bits in `mask` are
 * taken from `value`, the rest keep their last-written state. No device read is
 * involved, so this is one write transfer (plus the verify read), and two
 * callers touching disjoint masks can't clobber each other. */
esp_err_t SX1509_write_masked(SX1509Class *e, uint16_t mask, uint16_t value);

esp_err_t SX1509_write_pin(SX1509Class *e, uint8_t pin, bool level);

/* Per-pin LED driver (PWM intensity on RegIOnX).
 *
 * Three things have to be true before a pin dims anything, and this call sets
 * up all of them: the oscillator must be running (RegClock[6:5]), the shared
 * LED clock ClkX must not be OFF (RegMisc[6:4]) and RegLEDDriverEnable[pin]
 * must be 1. Both clocks are off at reset for power reasons, and they are
 * switched on lazily here -- the first time a LED driver is actually enabled --
 * rather than in init, because this board's expander otherwise has no use for
 * the oscillator and running a 2 MHz oscillator forever to drive nothing is
 * just quiescent current.
 *
 * Enabling also does the pin housekeeping the datasheet's LED sequence calls
 * for: input buffer disabled, pull-up off, direction set to output. Disabling
 * restores the input buffer and clears the enable bit but leaves the direction
 * alone, so a pin that was made an output stays one rather than flipping back
 * to an input under the caller's feet.
 *
 * intensity is written straight to RegIOnX (0..255). The driver is a current
 * SINK: the pin goes to its ON intensity when RegData[pin] is driven LOW, so
 * light the LED with SX1509_write_pin(e, pin, false), not true. No LED is
 * populated on any expander pin on this board -- this exists for the terminal
 * blocks and for bench use. */
esp_err_t SX1509_led_driver(SX1509Class *e, uint8_t pin, bool enable, uint8_t intensity);

/* hard = true pulses the ~RESET GPIO low (>200 ns per the datasheet; this
 * driver holds it far longer and then waits out the reset recovery), which is
 * the only reset that works on a part whose I2C interface has wedged.
 * hard = false writes 0x12 then 0x34 to RegReset, which the datasheet defines
 * as equivalent to a POR. Either way all shadows go back to the power-on
 * values. Requesting a hard reset with no ~RESET GPIO configured returns
 * ESP_ERR_INVALID_STATE rather than silently doing something else. */
esp_err_t SX1509_reset(SX1509Class *e, bool hard);

#ifdef __cplusplus
}
#endif

#endif // SX1509_INTERNAL_H
