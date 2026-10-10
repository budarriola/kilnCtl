// kiln_io -- the board layer over the SX1509 expander (U5).
//
// SX1509.c is the chip and knows nothing about this board. This file is the
// other half: it owns what each of the 16 expander pins *means* on the kilnCtl
// main board, and it is the only place that mapping is written down in code.
// The pin numbers themselves come from settings.h (SX1509_RELAY1_PIN and
// friends), which is Kconfig-adjacent and board-authoritative; docs/HARDWARE.md
// is the authority above both.
//
//   IO0..IO3   relay drives, active high through a BSS138 gate resistor
//   IO4        IO_1, opto-isolated INPUT from J24 (U13), 2.2k pull-up on board
//   IO5        IO_2, drives the opto-isolated OUTPUT to J25 (U14) via 390R
//   IO6/IO7    IO_3/IO_4, straight out to the J20 terminal block
//   IO8..IO10  ~DRDY from thermocouple channels 0/1/2, active low
//   IO11..IO13 IO_5/IO_6/IO_7, J21 pins 1-2 and J23 pin 1
//   IO14       LCD_IORQ,  ILI9488 data/command select
//   IO15       LCD_Reset, ILI9488 ~RESET, active low
//
// ** Relay numbering is not K-designator numbering. ** The schematic's
// Relay1..Relay4 nets are the expander's bit order IO0..IO3, and they land on:
//
//   Relay1 = IO0 -> Q3 -> K3 -> J8
//   Relay2 = IO1 -> Q1 -> K1 -> J3
//   Relay3 = IO2 -> Q2 -> K2 -> J4
//   Relay4 = IO3 -> Q5 -> K5 -> J11
//
// This API exposes the schematic's Relay1..4 numbering deliberately, rather
// than silently renumbering to K order: the numbers on the wire, in the GUI, in
// the schematic and in this header then all agree, and the one place the
// translation is needed -- reading a terminal block label -- has the table
// above. K4 belongs to the RP2040 safety processor, in the other ground domain,
// and is not reachable from here at all.
//
// ** The table above is the SCHEMATIC's wiring, not what kiln_io_set_relay()
// actually drives any more. ** Bench testing 2026-08-27 found relay 2 and
// relay 4's physical outputs swapped vs the schematic's own Relay2/Relay4
// nets -- a real PCB-level miswire, not a numbering mismatch (Relay1/Relay3
// read correctly). Compensated in kiln_io.c (kiln_relay_logical_to_pin_bit /
// kiln_io_remap_relay_bits, applied in kiln_io_set_relay_mask()'s write and
// kiln_io_resync_relay_shadow()'s read-back), so kiln_io_set_relay(io, 2, ...)
// now actually energizes IO3/K5/J11 and kiln_io_set_relay(io, 4, ...) energizes
// IO1/K1/J3 -- i.e. this API's relay 2 and relay 4 now correctly land where
// the panel silkscreen and this header's own callers expect, at the cost of
// no longer matching the schematic's Relay2/Relay4 net names literally. See
// hardware/mainBoard/todo.md for the silkscreen-labeling follow-up that would
// make this indirection visible on the board itself.
//
// Relay coils are 12 V (EE2-12NUH) switched low-side by a MOSFET, so an
// expander pin driven HIGH energizes the relay. That is why boot order matters
// and why kiln_io_init loads the data register with every relay bit at 0
// *before* it turns those pins into outputs -- see kiln_io_init.
#ifndef KILN_IO_H
#define KILN_IO_H

#include <stdbool.h>
#include <stdint.h>

#include "SX1509.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KILN_IO_RELAY_COUNT 4u   /* Relay1..Relay4, 1-based */
#define KILN_IO_DIGITAL_COUNT 7u /* IO_1..IO_7, 1-based */
#define KILN_IO_DRDY_COUNT 3u    /* thermocouple channels 0..2 */

/* kiln_io_state_t::flags */
#define KILN_IO_FLAG_INT_ASSERTED 0x01u /* the SX1509's ~INT line is low */
#define KILN_IO_FLAG_I2C_FAILED   0x02u /* the most recent expander transfer failed */
#define KILN_IO_FLAG_RELAY_UNKNOWN 0x04u /* relay_shadow is NOT trustworthy: a coil may be energised (K7 MED-1) */

/* Snapshot of the board's I/O, laid out to match the IO task's READ response
 * payload (see uart_task_ids.h) field for field -- the bridge copies these
 * straight out rather than recomputing them. */
typedef struct {
    uint16_t data;       /* RegData, raw pin states, bit N = expander pin N */
    uint16_t dir;        /* RegDir,  bit N = 1 -> pin N is an input */
    uint8_t relay_shadow;/* bits 0-3 = Relay1..Relay4 as last commanded */
    uint8_t io_levels;   /* bits 0-6 = IO_1..IO_7 pin levels */
    uint8_t drdy_bits;   /* bits 0-2 = channel 0-2 ~DRDY asserted (pin is LOW) */
    uint8_t flags;       /* KILN_IO_FLAG_* */
} kiln_io_state_t;

typedef struct {
    SX1509Class *exp;

    /* Commanded relay state, bits 0-3 = Relay1..Relay4. Kept separately from
     * the expander's data shadow because it is what the operator *asked for*:
     * if a write fails, or if someone pokes RegData directly over the debug
     * subcommands, the two diverge and that difference is worth seeing. It is
     * also what the GUI re-syncs its checkboxes from. */
    uint8_t relay_shadow;

    /* Commanded level of IO_1..IO_7, bits 0-6. Only meaningful for the ones
     * currently configured as outputs; kept for all seven so that flipping an
     * input to an output has a defined level to start from. */
    uint8_t io_shadow;

    /* Cached display control lines. The ILI9488's D/C toggles once per command
     * and every toggle is an I2C transfer on a bus the expander shares with
     * nothing else but is still the slowest thing in the display path -- so
     * kiln_io_lcd_dc compares against this first and does nothing at all when
     * the line is already where it needs to be. */
    bool lcd_dc_data;
    bool lcd_reset_asserted;

    /* Sticky-per-call status of the last expander access, surfaced as
     * KILN_IO_FLAG_I2C_FAILED so a dead expander is visible in a READ response
     * rather than only in the log. */
    bool last_i2c_failed;

    /* CT_COMMISSIONING_PLAN.md step 2 -- the auto idle-offset commissioning
     * action needs "every relay reported off for >= 5 s" as a precondition
     * (five peak-hold time constants). -1 means "not currently all-off" (at
     * least one relay bit is set in relay_shadow, or this has never been
     * true since kiln_io_init()); otherwise the hal_time_now_us() timestamp
     * of the moment relay_shadow most recently BECAME all-zero. Updated only
     * in kiln_io_set_relay_mask()/kiln_io_all_relays_off() (the two places
     * relay_shadow itself changes) and at init (all relays start off). See
     * kiln_io_relays_off_ms() below. */
    int64_t relays_all_off_since_us;

    bool initialized;

    /* K7 MED-1: true when the relay state could not be established from the
     * chip -- an ON write may have landed and the bus then died, so neither
     * the chip nor a safe-off write could be read/verified. relay_shadow keeps
     * its last verified value ON PURPOSE (flipping it to "ON" would disarm the
     * Pico S3 and ESP H9 guards that catch a stuck coil); this flag is the
     * separate honest report. Raised by kiln_io_resync_relay_shadow(), cleared
     * only by a verified chip read or a verified all-off. While set, relay ON
     * is refused and the profile executor's watchdog treats it as a fault
     * (kiln_io_relay_state_unknown()). */
    bool relay_state_unknown;

    /* K7 MED-2: serialises every multi-step access (relay RMW, re-init, LCD
     * lines, IO config) between the owner task, lvgl_port_task and the
     * fail-safe callers outside the owner task. Lock order: this lock, then the
     * SX1509 driver's own lock; never the reverse. NULL (hand-built structs in
     * host tests) means "no lock". */
    SemaphoreHandle_t io_lock;
} kiln_io_t;

/* Configure the whole board and leave it safe.
 *
 * Order matters more here than anywhere else in this driver, and it is:
 *   1. software/hardware reset state assumed or established -- every pin an
 *      input, so nothing is being driven;
 *   2. load RegData with the intended output levels: all four relay bits 0,
 *      IO_2's opto output 0, LCD_Reset HIGH (the panel's reset is active low,
 *      so high = not held in reset), LCD_IORQ high. Writing the data register
 *      while the pins are still inputs only loads the output latches -- no pin
 *      moves;
 *   3. pull-ups on the bare terminal-block inputs and on the ~DRDY lines;
 *   4. RegDir last, which is the moment the outputs actually start driving --
 *      and they start driving the values loaded in step 2.
 * Doing this the obvious way round (direction first, then levels) would drive
 * every relay from the latch's power-on default of 1 for as long as the next
 * I2C transfer takes. On a kiln that is every element on for a few hundred
 * microseconds to milliseconds at every boot, which is the exact class of
 * glitch this ordering exists to prevent.
 *
 * Also enables per-pin interrupts on all the inputs (both edges on the
 * terminal-block and opto inputs, falling only on ~DRDY, which is the edge that
 * means "conversion finished"). It does NOT install an ISR -- see
 * kiln_io_irq_gpio().
 *
 * Debouncing is left OFF. The part's debouncer is clocked from its internal
 * oscillator, so enabling it would mean running that oscillator forever for the
 * benefit of an opto input and a few terminal blocks that the firmware already
 * samples at tens of milliseconds. SX1509_set_debounce is there if a noisy
 * installation ever needs it. */
esp_err_t kiln_io_init(kiln_io_t *io, SX1509Class *exp);

/* relay is 1-4 (Relay1..Relay4 -- see the K/J table at the top of this file).
 * on = true energizes the coil. One verified masked write; the other three
 * relays are untouched. */
esp_err_t kiln_io_set_relay(kiln_io_t *io, uint8_t relay, bool on);

/* Change several relays in a single expander write, so they switch on the same
 * I2C transfer instead of one per relay. mask selects which relays change
 * (bit 0 = Relay1 .. bit 3 = Relay4); value carries the new states in the same
 * bit order. Bits outside 0-3 are ignored. */
esp_err_t kiln_io_set_relay_mask(kiln_io_t *io, uint8_t mask, uint8_t value);

/* Drop all four relays, unconditionally, in one write. This is the fallback
 * state on link loss, on a safety fault and at shutdown, so it is deliberately
 * the shortest path in this file: no reads, no per-relay loop, nothing that can
 * half-succeed. */
esp_err_t kiln_io_all_relays_off(kiln_io_t *io);

/* LOW-3 (FIRING_PATH_AUDIT_2026-10-10): generation bumped at the START of every
 * kiln_io_all_relays_off() call (even a refused/failed one -- fail toward off).
 * An owner command that sampled an older value before queueing predates a
 * fail-safe all-off and must not close a relay afterward. */
uint32_t kiln_io_relay_off_epoch(void);

/* K7 review F3: returned by kiln_io_all_relays_off() when the kiln_io lock could not be taken and
 * the bare, unserialised OFF write succeeded. NOT a verified OFF (the lock holder's own ON write may
 * land after it): relay_shadow is untouched and relay_state_unknown stays raised. Any non-ESP_OK
 * result means "retry the locked all-off"; callers must not record an OFF on it. */
#define KILN_IO_ERR_UNSERIALISED_OFF 0x10C /* == ESP_ERR_NOT_FINISHED */

/* Re-runs bring-up after the expander was reset/POR'd (K7-03): reset (the hard
 * ~RESET pulse when the GPIO is wired, else soft -- K7 NIT-1), relays latched
 * OFF, relay pins back to outputs, verified by chip read-back. Marks the board
 * not-initialised until the read-back passes, so relay ON is refused meanwhile.
 * Relays are never energised by this call. If a step fails after the relay pins
 * became outputs they are returned to inputs (de-energised) so a failed
 * re-init can never leave a driven pin behind the shadow (K7 HIGH-1).
 *
 * Takes the kiln_io lock (bounded wait, ESP_ERR_TIMEOUT), so it is safe to call
 * from fail-safe callers outside the owner task (K7 MED-2).
 *
 * Recovery from a failed re-init (K7 LOW-2): the board stays not-initialised
 * (relay ON refused, OFF always allowed). The profile executor's watchdog calls
 * kiln_io_all_relays_off() every period on a fault, which re-runs this repair
 * when the chip answers; CMD_SX_RESET is the operator's manual retry. */
esp_err_t kiln_io_reinit(kiln_io_t *io);

/* Same as kiln_io_reinit() but performs the expander reset itself under the
 * kiln_io lock (CMD_SX_RESET): hard = pulse the ~RESET GPIO (refused with
 * ESP_ERR_INVALID_STATE if it is not wired), otherwise a soft reset. The user's
 * IO_1..IO_7 configuration and the LCD D/C / ~RESET lines are captured before
 * the reset and re-applied after the verified re-init (K7 LOW-1). If the reset
 * itself fails the relay state is re-derived from the chip, or flagged unknown
 * (K7 LOW-5). Relays are never energised by this call. */
esp_err_t kiln_io_reset_and_reinit(kiln_io_t *io, bool hard);

/* True when the relay state could not be established from the chip (see
 * kiln_io_t::relay_state_unknown). Treat as a fault: retry kiln_io_all_relays_off
 * and assert a fault source; never as "OFF". */
bool kiln_io_relay_state_unknown(const kiln_io_t *io);

/* The expander pin bits (IO0..IO3, see this header's top comment) that are
 * relay drives -- SX1509 pin numbering, not the schematic's Relay1..4
 * numbering. Exists so a caller outside this file (uart_bridge.c's raw
 * SX_WRITE_REG/SX_SET_DIR debug subcommands) can tell whether a raw
 * register write touches a relay pin without duplicating this board's pin
 * map -- this header stays the one place that mapping is written down, per
 * its own top comment. See docs/SAFETY_MODEL.md's "SX_WRITE_REG/SX_SET_DIR
 * can bypass the relay gate entirely" gap. */
uint16_t kiln_io_relay_pin_mask(void);

/* index is 1-7 (IO_1..IO_7). Only meaningful for an I/O currently configured as
 * an output; the level is remembered either way so that a later switch to
 * output starts from a known state. Note IO_1 is wired as an opto-isolated
 * *input* on this board and IO_2 drives an opto-isolated output -- the rest are
 * bare terminal-block pins that can be either. */
esp_err_t kiln_io_set_io(kiln_io_t *io, uint8_t index, bool level);

/* Reconfigure one of IO_1..IO_7. input = true makes it an input (RegDir bit 1),
 * pullup enables the part's internal pull-up and is only applied when input is
 * true -- a pull-up on a push-pull output is just wasted current fighting the
 * driver. Relay pins and the two display pins are not reachable through this
 * call on purpose: nothing good comes of turning a relay drive into an input at
 * runtime. */
esp_err_t kiln_io_set_io_dir(kiln_io_t *io, uint8_t index, bool input, bool pullup);

/* One expander read, expanded into the board's view of itself. Also clears the
 * expander's interrupt source register, which is what releases ~INT -- so a
 * polling reader that never looks at the interrupt source still can't leave the
 * line stuck low. An edge arriving between the data read and the clear keeps
 * its bit and re-asserts ~INT, so nothing is lost. */
esp_err_t kiln_io_read(kiln_io_t *io, kiln_io_state_t *out);

/* channel is 0-2. *out_asserted is true when that MAX31856 is signalling "data
 * ready", which on the wire is the pin being LOW. A missing or unpowered
 * thermocouple daughterboard reads high (kiln_io_init enables the internal
 * pull-ups on these three pins for exactly that reason), i.e. "not ready" --
 * the safe interpretation. */
esp_err_t kiln_io_get_drdy(kiln_io_t *io, uint8_t channel, bool *out_asserted);

/* The ILI9488's data/command select. false = command, true = data.
 *
 * This is the hot path: the display driver calls it once per command, and every
 * call that actually changes something is one I2C transfer that the SPI
 * transaction behind it has to wait for. So it is built to be as small as it
 * can be -- a no-op when the line is already right, and otherwise a single
 * two-byte write of RegDataB (the bank holding I/O[15:8]) computed from the
 * driver's shadow. No read-modify-write round trip, and no read-back
 * verification: doubling the traffic on the one call that runs per display
 * command would be paid on every command forever, and the failure mode it would
 * catch (an expander that stopped answering) shows up immediately anyway, as a
 * display drawing garbage and as errors from every other expander access. */
esp_err_t kiln_io_lcd_dc(kiln_io_t *io, bool data);

/* The ILI9488's hardware reset. asserted = true drives the panel's active-low
 * ~RESET LOW (i.e. holds it in reset); false releases it. Not a hot path, so
 * unlike kiln_io_lcd_dc this one goes through the verified masked write. */
esp_err_t kiln_io_lcd_reset(kiln_io_t *io, bool asserted);

/* The expander's ~INT GPIO (GPIO7 on this board), or -1. Handed out so the task
 * that owns the board -- the UART bridge today -- can install its own ISR,
 * task notification or event-group bit on it. Neither this layer nor the SX1509
 * driver installs anything or assumes any particular task structure. */
int kiln_io_irq_gpio(const kiln_io_t *io);

/* True while ~INT is asserted (low). Reads the ESP32 GPIO, not the expander --
 * no I2C traffic, safe to poll. */
bool kiln_io_irq_asserted(const kiln_io_t *io);

/* Commanded relay state, bits 0-3 = Relay1..Relay4. Not a device read. */
uint8_t kiln_io_get_relay_shadow(const kiln_io_t *io);

/* Milliseconds since relay_shadow most recently became ALL-ZERO, or
 * UINT32_MAX if at least one relay is currently commanded on, or if it has
 * never been all-off since kiln_io_init() (should not happen in practice --
 * init leaves every relay off -- but a NULL/uninitialized `io` reports the
 * same conservative UINT32_MAX rather than a fabricated 0). CT_COMMISSIONING_
 * PLAN.md step 2's "every relay reported off for >= 5 s" precondition reads
 * this rather than re-deriving it from kiln_io_get_relay_shadow() alone,
 * since a point-in-time zero-mask read cannot answer "for how long".
 *
 * This tracks the CHOPPED relay shadow -- the post-PWM commanded state
 * actually written to the expander -- not any zone's intended pre-PWM duty
 * (project_pwm_chopping_disarms_guards: four other guards were fooled by
 * exactly this distinction before 78d233f taught them to read intended duty
 * instead of chopped relay state). A duty-cycled zone reads "all off" for
 * most of its chop window even while a profile or autotune run is actively
 * commanding it, so this alone is NOT sufficient to prove "nothing is
 * running" -- callers (e.g. the CT auto-zero precondition gate in
 * safety_cfg_http.c) must separately refuse when a profile or autotune is
 * active rather than inferring it from this value.
 * UINT32_MAX means ONLY "a relay is on / unknown"; a genuine off-time saturates
 * at UINT32_MAX - 1 (about 49.7 days) so it never reads as relay-on. */
uint32_t kiln_io_relays_off_ms(const kiln_io_t *io);

#ifdef __cplusplus
}
#endif

#endif // KILN_IO_H
