// SX1509B 16-channel level-shifting I2C I/O expander driver (U5 on the kiln
// main board, 0x3E).
//
// Like PCF8575 (the part this replaces) and SSD1306, this driver does NOT
// create the I2C bus: ESP-IDF allows exactly one i2c_master_bus_handle_t per
// physical bus, so the caller passes in the already-created handle.
// SX1509_init attaches this device to it via i2c_master_bus_add_device() and
// creates its own i2c_owner_t wrapping the same bus handle, so its transfers
// get their own FIFO-ordered queue independent of anything else on the wires.
//
// What makes this part different from the PCF8575 it replaces, and why the API
// is bigger:
//
//   - It is a *real* GPIO expander, not a quasi-bidirectional port. There is a
//     direction register (RegDir), separate pull-up and pull-down registers, an
//     open-drain register, a hardware input debouncer, per-pin edge-sensitive
//     interrupts with a maskable ~INT output, and a per-pin LED/PWM driver.
//     None of that existed on the PCF8575, where "write a 1 and hope" was the
//     whole model.
//   - RegDir polarity is the opposite of what most people guess: **1 = input**,
//     0 = output, and every pin comes out of reset as an input (0xFFFF). That
//     is the safe direction and this driver never quietly changes it.
//   - Every 16-bit quantity is a *pair* of 8-bit registers, bank B (I/O[15:8])
//     at the LOWER address and bank A (I/O[7:0]) at the higher one -- e.g.
//     RegDataB = 0x10, RegDataA = 0x11. With the part's address auto-increment
//     on (RegMisc bit 1 = 0, the power-on default, which this driver relies on
//     and never turns off), one transfer starting at the B address moves the
//     high byte first. So the wire order for a uint16_t is BIG-endian, which is
//     the reverse of the PCF8575's two port bytes and the reverse of the
//     little-endian u16 the UART protocol carries. SX1509_write_reg16 /
//     SX1509_read_reg16 are the only places that byte order is spelled out;
//     everything above them speaks uint16_t with bit N = pin N.
//
// Addressing: ADDR1/ADDR0 select one of four addresses -- 0x3E (both to GND,
// which is how this board is strapped), 0x3F, 0x70, 0x71. They are NOT
// contiguous, so SX1509_scan walks a table rather than a range. All four are
// selectable at runtime via SX1509_set_address, same as the old expander
// driver, since nothing but the strapping stops a differently-populated board
// from using them.
//
// This file is the *chip*. It knows nothing about relays, thermocouples or the
// display -- what each pin means on this board lives in kiln_io.h.
#ifndef SX1509_H
#define SX1509_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "i2c_owner.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SX1509_PIN_COUNT 16u

/* The four addresses ADDR[1:0] can select. Deliberately a list and not a
 * range: 0x3E/0x3F and 0x70/0x71 are two separate pairs. */
#define SX1509_ADDR_00 0x3Eu /* ADDR1=0 ADDR0=0 -- this board */
#define SX1509_ADDR_01 0x3Fu
#define SX1509_ADDR_10 0x70u
#define SX1509_ADDR_11 0x71u
#define SX1509_ADDR_COUNT 4u

/* Power-on register values this driver seeds its shadows with. RegDir comes up
 * all-ones (every pin an input) and RegData all-ones; the datasheet's footnote
 * on the register table is explicit that a pin later configured as an output
 * takes 1 as its default latch value. Seeding the shadows with these means the
 * first read-modify-write of a single pin can't invent state for the other 15. */
#define SX1509_DIR_POWER_ON_STATE  0xFFFFu
#define SX1509_DATA_POWER_ON_STATE 0xFFFFu

/* --- Register map (SX1509B, 16-channel). Bank B is always the LOWER address;
 * see the byte-order note in the header comment above. --- */
#define SX1509_REG_INPUT_DISABLE_B 0x00u
#define SX1509_REG_INPUT_DISABLE_A 0x01u
#define SX1509_REG_LONG_SLEW_B     0x02u
#define SX1509_REG_LONG_SLEW_A     0x03u
#define SX1509_REG_LOW_DRIVE_B     0x04u
#define SX1509_REG_LOW_DRIVE_A     0x05u
#define SX1509_REG_PULLUP_B        0x06u
#define SX1509_REG_PULLUP_A        0x07u
#define SX1509_REG_PULLDOWN_B      0x08u
#define SX1509_REG_PULLDOWN_A      0x09u
#define SX1509_REG_OPEN_DRAIN_B    0x0Au
#define SX1509_REG_OPEN_DRAIN_A    0x0Bu
#define SX1509_REG_POLARITY_B      0x0Cu
#define SX1509_REG_POLARITY_A      0x0Du
#define SX1509_REG_DIR_B           0x0Eu
#define SX1509_REG_DIR_A           0x0Fu
#define SX1509_REG_DATA_B          0x10u
#define SX1509_REG_DATA_A          0x11u
#define SX1509_REG_INT_MASK_B      0x12u
#define SX1509_REG_INT_MASK_A      0x13u
/* Four consecutive sense registers, most-significant pins first:
 * 0x14 = I/O[15:12], 0x15 = I/O[11:8], 0x16 = I/O[7:4], 0x17 = I/O[3:0]. */
#define SX1509_REG_SENSE_HIGH_B    0x14u
#define SX1509_REG_SENSE_LOW_B     0x15u
#define SX1509_REG_SENSE_HIGH_A    0x16u
#define SX1509_REG_SENSE_LOW_A     0x17u
#define SX1509_REG_INT_SOURCE_B    0x18u
#define SX1509_REG_INT_SOURCE_A    0x19u
#define SX1509_REG_EVENT_STATUS_B  0x1Au
#define SX1509_REG_EVENT_STATUS_A  0x1Bu
#define SX1509_REG_LEVEL_SHIFTER_1 0x1Cu
#define SX1509_REG_LEVEL_SHIFTER_2 0x1Du
#define SX1509_REG_CLOCK           0x1Eu
#define SX1509_REG_MISC            0x1Fu
#define SX1509_REG_LED_ENABLE_B    0x20u
#define SX1509_REG_LED_ENABLE_A    0x21u
#define SX1509_REG_DEBOUNCE_CONFIG 0x22u
#define SX1509_REG_DEBOUNCE_EN_B   0x23u
#define SX1509_REG_DEBOUNCE_EN_A   0x24u
#define SX1509_REG_KEY_CONFIG_1    0x25u
#define SX1509_REG_KEY_CONFIG_2    0x26u
#define SX1509_REG_KEY_DATA_1      0x27u
#define SX1509_REG_KEY_DATA_2      0x28u
#define SX1509_REG_HIGH_INPUT_B    0x69u
#define SX1509_REG_HIGH_INPUT_A    0x6Au
#define SX1509_REG_RESET           0x7Du
#define SX1509_REG_TEST_1          0x7Eu /* datasheet: "not to be written" */
#define SX1509_REG_TEST_2          0x7Fu

/* RegReset: writing these two values back to back, and only back to back, is
 * the software equivalent of a power-on reset. */
#define SX1509_RESET_MAGIC_1 0x12u
#define SX1509_RESET_MAGIC_2 0x34u

/* RegClock[6:5] -- oscillator source. 00 (OFF) is the default, and with it OFF
 * the debouncer, the keypad engine and every LED driver are dead, which is the
 * single most common reason "I enabled the LED driver and nothing happened". */
#define SX1509_CLOCK_OSC_OFF      (0u << 5)
#define SX1509_CLOCK_OSC_EXTERNAL (1u << 5)
#define SX1509_CLOCK_OSC_INTERNAL (2u << 5) /* internal 2 MHz */

/* RegMisc bits this driver cares about. */
#define SX1509_MISC_LED_CLK_SHIFT 4u   /* [6:4] ClkX = fOSC / 2^(n-1); 0 = LED off */
#define SX1509_MISC_LED_CLK_MASK  0x70u
#define SX1509_MISC_NO_AUTOINC    0x02u /* 1 = do NOT auto-increment; must stay 0 */
#define SX1509_MISC_NO_AUTOCLEAR  0x01u /* 1 = RegData reads do NOT clear ~INT */

/* RegSense encoding, two bits per pin. The four sense registers together form
 * one 32-bit field with pin N at bits [2N+1:2N] -- see SX1509_set_interrupt. */
#define SX1509_SENSE_NONE    0u
#define SX1509_SENSE_RISING  1u
#define SX1509_SENSE_FALLING 2u
#define SX1509_SENSE_BOTH    3u
#define SX1509_SENSE_FOR_PIN(pin, mode) ((uint32_t)(mode) << (2u * (uint32_t)(pin)))

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    i2c_owner_t owner;
    bool owner_initialized;
    uint8_t addr;

    /* Shadows of the registers that single-pin and masked operations have to
     * read-modify-write. They are kept here rather than re-read from the part
     * for two reasons: a read costs a full I2C round trip on a bus the display
     * is already contending for, and RegData in particular reads back *pin*
     * states -- for an input pin that is the outside world, not what this
     * driver last drove, so an RMW built on a read-back would happily latch
     * whatever a floating terminal block happened to be doing. */
    uint16_t dir_shadow;  /* RegDir,        bit = 1 -> input */
    uint16_t data_shadow; /* RegData,       last value written */
    uint16_t pu_shadow;   /* RegPullUp */
    uint16_t pd_shadow;   /* RegPullDown */
    uint16_t od_shadow;   /* RegOpenDrain */
    uint16_t led_shadow;  /* RegLEDDriverEnable */

    /* GPIO numbers for the two out-of-band lines, or -1 if unused. The IRQ pin
     * is configured as an input with a pull-up here (~INT is open-drain) but no
     * ISR is installed: which task wants to hear about an edge, and how, is not
     * this driver's business -- see SX1509_get_irq_gpio(). */
    int irq_gpio;
    int reset_gpio;

    /* The internal 2 MHz oscillator is off at reset and is only switched on the
     * first time something needs it (LED driver or debounce). See
     * SX1509_led_driver / SX1509_set_debounce. */
    bool osc_enabled;
    uint8_t led_clock_div; /* RegMisc[6:4] currently programmed, 0 = LED clk off */

    /* Serializes the read-modify-write sequences above. i2c_owner already
     * serializes individual *transfers*, but "compute from shadow, then write"
     * spans two steps and two callers (the UART bridge task and the display
     * driver's D/C toggling) can interleave inside it. */
    SemaphoreHandle_t lock;
} SX1509Class;

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
 * SX_READ_REG debug subcommands. ---
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
 * so poking a register from the PC can't desynchronize the driver.
 *
 * SX1509_read_regs reads `len` consecutive registers in one transfer and so
 * depends on the part's address auto-increment being on (RegMisc bit 1 = 0,
 * the default; this driver never sets it). */
esp_err_t SX1509_write_reg(SX1509Class *e, uint8_t reg, uint8_t value);
esp_err_t SX1509_read_reg(SX1509Class *e, uint8_t reg, uint8_t *out_value);
esp_err_t SX1509_read_regs(SX1509Class *e, uint8_t reg, uint8_t *out, size_t len);

/* 16-bit register pair access. `reg` is the BANK B (lower) address of the pair,
 * e.g. SX1509_REG_DATA_B. The value goes on the wire high byte first -- bank B
 * holds I/O[15:8] and sits at the lower address, so auto-increment delivers it
 * first. Bit N of `value` is always pin N regardless of that. */
esp_err_t SX1509_write_reg16(SX1509Class *e, uint8_t reg, uint16_t value);
esp_err_t SX1509_read_reg16(SX1509Class *e, uint8_t reg, uint16_t *out_value);

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

/* Which pins have fired since the last clear. ~INT stays low until every bit in
 * RegInterruptSource is cleared, so pass clear = true unless you intend to
 * leave the line asserted. Clearing is write-1-to-clear (writing back exactly
 * the bits that were read), so it cannot lose an edge that arrived between the
 * read and the write -- that pin's bit is simply still set afterwards.
 *
 * Note this driver turns OFF the part's "auto-clear ~INT when RegData is read"
 * behaviour (RegMisc bit 0 = 1) so that ordinary polling of the port doesn't
 * silently consume interrupt sources; clearing is always explicit. */
esp_err_t SX1509_get_interrupt_source(SX1509Class *e, uint16_t *out_mask, bool clear);

/* --- Port I/O. --- */

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

/* Whole-port read of RegData: actual pin states, including for pins this device
 * is driving. */
esp_err_t SX1509_read_port(SX1509Class *e, uint16_t *out_value);

esp_err_t SX1509_write_pin(SX1509Class *e, uint8_t pin, bool level);
esp_err_t SX1509_read_pin(SX1509Class *e, uint8_t pin, bool *out_level);

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

/* Last value *written* to RegData -- not a device read. */
uint16_t SX1509_get_shadow(const SX1509Class *e);

/* Direction shadow, same convention as SX1509_set_dir (1 = input). */
uint16_t SX1509_get_dir_shadow(const SX1509Class *e);

/* The ~INT GPIO number (or -1). Exposed so the owner of the expander -- the
 * UART bridge task today -- can install whatever it wants on it: a GPIO ISR
 * that gives a semaphore, a task notification, an event group bit. The driver
 * deliberately installs nothing and assumes no task structure; it only
 * configures the pin as a pulled-up input, since ~INT is open-drain. */
int SX1509_get_irq_gpio(const SX1509Class *e);

/* True if ~INT is currently asserted (the line is low). Reads the GPIO, not the
 * expander -- no I2C traffic. False if no IRQ GPIO is configured. */
bool SX1509_irq_asserted(const SX1509Class *e);

#ifdef __cplusplus
}
#endif

#endif // SX1509_H
