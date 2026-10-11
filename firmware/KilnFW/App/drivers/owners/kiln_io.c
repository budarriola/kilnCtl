#include "kiln_io.h"

/* ROADMAP.md M15 A1: kiln_io.c is one of the SX1509 write/config owners --
 * see SX1509_internal.h's top comment. */
#define SX1509_OWNER_BUILD
#include "SX1509_internal.h"

#include <string.h>

#include "esp_log.h"
#include "hal_time.h" /* CT_COMMISSIONING_PLAN.md step 2 -- relays_all_off_since_us, kiln_io_relays_off_ms() */
#include "settings.h"

static const char *TAG = "kiln_io";

#define KILN_BIT(n) ((uint16_t)(1u << (n)))

/* --- The board, as bit masks. Everything below is derived from the pin
 * numbers in settings.h so that this file and menuconfig can never drift. --- */

/* Relay1..Relay4 are IO0..IO3 -- contiguous and in that order, which is the one
 * thing that lets the relay mask helpers be a plain shift instead of a table.
 * They are NOT K1..K4: see the mapping table in kiln_io.h. */
#define KILN_IO_RELAY_MASK                                                        \
    (KILN_BIT(SX1509_RELAY1_PIN) | KILN_BIT(SX1509_RELAY2_PIN) | KILN_BIT(SX1509_RELAY3_PIN) | \
     KILN_BIT(SX1509_RELAY4_PIN))

/* Pins this board drives. IO_2 (the opto-isolated output to J25) is the only
 * one of the general-purpose I/Os that is an output by default; IO_1 is an opto
 * *input* and IO_3..IO_7 are bare terminal-block pins that start as inputs
 * because that is the state that can't fight whatever is wired to them. */
#define KILN_IO_OUTPUT_MASK                                                    \
    (KILN_IO_RELAY_MASK | KILN_BIT(SX1509_IO2_PIN) | KILN_BIT(SX1509_LCD_DC_PIN) |   \
     KILN_BIT(SX1509_LCD_RESET_PIN))

/* RegDir polarity is the part's: 1 = input. */
#define KILN_IO_DIR_MASK ((uint16_t)~KILN_IO_OUTPUT_MASK)

/* Internal pull-ups. IO_1 already has a 2.2k pull-up to 3.3V on the board, so
 * it does not get one here. The ~DRDY pins do: an absent or unpowered
 * thermocouple daughterboard then reads high, which is "not ready" -- the safe
 * reading -- instead of floating and inventing conversions. */
#define KILN_IO_PULLUP_MASK                                                                 \
    (KILN_BIT(SX1509_IO3_PIN) | KILN_BIT(SX1509_IO4_PIN) | KILN_BIT(SX1509_IO5_PIN) |                \
     KILN_BIT(SX1509_IO6_PIN) | KILN_BIT(SX1509_IO7_PIN) | KILN_BIT(THERMO_DRDY0_EXP_PIN) |          \
     KILN_BIT(THERMO_DRDY1_EXP_PIN) | KILN_BIT(THERMO_DRDY2_EXP_PIN))

/* The output latch values loaded before the pins become outputs: every relay
 * off, IO_2's opto off, LCD_IORQ high (data), and LCD_Reset HIGH -- the panel's
 * reset is active low, so high means "not held in reset". Input pins' latch
 * bits are left at 0 so that an I/O later flipped to an output starts low
 * rather than immediately asserting whatever is on the terminal block. */
#define KILN_IO_SAFE_DATA ((uint16_t)(KILN_BIT(SX1509_LCD_DC_PIN) | KILN_BIT(SX1509_LCD_RESET_PIN)))

/* IO_1..IO_7 -> expander pin. Not contiguous: IO_4 is pin 7 and IO_5 jumps to
 * pin 11, because pins 8-10 are the thermocouple ~DRDY lines. */
static const uint8_t kiln_io_pins[KILN_IO_DIGITAL_COUNT] = {
    SX1509_IO1_PIN, SX1509_IO2_PIN, SX1509_IO3_PIN, SX1509_IO4_PIN,
    SX1509_IO5_PIN, SX1509_IO6_PIN, SX1509_IO7_PIN,
};

/* Rotated the same one position as MAX31856_start_all()'s cs_pins/fault_pins
 * (MAX31856.c, 2026-08-27 bench fix) and for the same reason: ~DRDY is a
 * property of a specific MAX31856 chip, and logical channel i now talks to
 * a different chip than before the CS rotation -- its DRDY line has to
 * follow. Leaving this array un-rotated would have logical channel i read
 * CS(i-1 mod 3)'s conversions but DRDY(i)'s ready line (a DIFFERENT chip's),
 * so a dead channel could read as fresh (neighbour's DRDY still toggling)
 * and a live one as permanently stale -- exactly what MAX31856_
 * set_drdy_provider()'s staleness detection exists to prevent. */
static const uint8_t kiln_io_drdy_pins[KILN_IO_DRDY_COUNT] = {
    THERMO_DRDY2_EXP_PIN,
    THERMO_DRDY0_EXP_PIN,
    THERMO_DRDY1_EXP_PIN,
};

/* Bank B is I/O[15:8], so a pin >= 8 lives at bit (pin - 8) of RegDataB. Both
 * display control lines are in that bank, which is what makes the D/C fast path
 * a single-register write. */
#define KILN_IO_LCD_DC_BANKB_BIT    ((uint8_t)(1u << (SX1509_LCD_DC_PIN - 8)))

static esp_err_t kiln_io_track(kiln_io_t *io, esp_err_t err)
{
    io->last_i2c_failed = (err != ESP_OK);
    return err;
}

/* Every public call checks this rather than just `io->exp`. kiln_io_init sets
 * `exp` before it starts configuring, so a bring-up that failed halfway leaves
 * a struct that looks attached but whose pin directions, pull-ups and output
 * latches are in an unknown mixture of reset and configured state. Driving a
 * relay through that would be commanding hardware whose direction register we
 * do not know -- so it is ESP_ERR_INVALID_STATE, not a best effort. */
static inline bool kiln_io_ready(const kiln_io_t *io)
{
    return io && io->exp && io->initialized;
}

/* Logical relay N (1-based, what every caller outside this file means --
 * zone relay_mask, the dashboard, danger_mode, MCP tools, the PC protocol's
 * relay_shadow byte) vs the actual SX1509 pin it must drive. Confirmed on the
 * bench 2026-08-27: commanding logical relay 2 was energizing the physical
 * contactor at panel position 4, and commanding relay 4 was energizing the
 * one at position 2 -- a real PCB-level swap between those two nets, not a
 * numbering-convention mismatch (R1/R3 read correctly). settings.h's
 * SX1509_RELAYn_PIN defines stay the true, undisturbed hardware pins (see
 * that file's comment for why renaming them there would not have fixed
 * this); this table is the one and only place the compensating swap lives.
 * Index i = logical relay (i+1)'s bit position (0-3); value = the SX1509 pin
 * bit that logical relay actually needs. Self-inverse (swapping 1<->3 twice
 * is identity), which is why the same table also undoes the swap when
 * kiln_io_resync_relay_shadow() below reads real hardware bits back into
 * logical shadow bits. */
static const uint8_t kiln_relay_logical_to_pin_bit[KILN_IO_RELAY_COUNT] = { 0, 3, 2, 1 };

/* Remaps a 4-bit relay mask/value between logical-relay-bit-order and
 * physical-SX1509-pin-bit-order using kiln_relay_logical_to_pin_bit above --
 * same operation either direction since that table is self-inverse. */
static uint8_t kiln_io_remap_relay_bits(uint8_t bits)
{
    uint8_t out = 0;
    for (uint8_t i = 0; i < KILN_IO_RELAY_COUNT; i++) {
        if (bits & (uint8_t)(1u << i)) {
            out |= (uint8_t)(1u << kiln_relay_logical_to_pin_bit[i]);
        }
    }
    return out;
}

/* Re-derives the commanded relay state from what the expander last *accepted*.
 *
 * SX1509_write_masked updates the chip driver's data shadow as soon as the part
 * ACKs the write, even if the verifying read-back then disagreed or a later
 * step of the sequence failed -- so after a failure the coils may well have
 * moved. Continuing to report the previous commanded value would be the one lie
 * this layer must never tell: "relay off" while it is energized. Report the
 * mismatch and adopt the expander's view. */
static void kiln_io_note_relay_shadow_changed(kiln_io_t *io); /* forward decl -- defined below,
                                                                  used by both this function and
                                                                  the write paths further down */

#define KILN_IO_LOCK_WAIT_MS          6500u /* normal callers: a little over the SX1509 driver's own lock wait */
#define KILN_IO_FAILSAFE_LOCK_WAIT_MS 2000u /* fail-safe all-off: bounded, then an unlocked OFF write */

/* K7 MED-2. Lock order: this lock, then the SX1509 driver's lock, never the
 * reverse. A NULL io_lock (hand-built struct in a host test) means no locking. */
static bool kiln_io_lock(kiln_io_t *io, uint32_t wait_ms)
{
    if (!io->io_lock) return true;
    return xSemaphoreTake(io->io_lock, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}

static void kiln_io_unlock(kiln_io_t *io)
{
    if (io->io_lock) xSemaphoreGive(io->io_lock);
}

/* Reads the chip's real relay state: a relay is energised only if its pin is an
 * OUTPUT (RegDir bit 0) AND reads high. Both registers are read from the part,
 * never from the driver's shadows (K7-02: the driver data shadow records what
 * was last ACKed, not what the pins hold). Returns false if the chip cannot be
 * read. *out_logical is in logical relay-bit order; *out_pins_input (optional)
 * is true if any relay pin is an input, i.e. the expander was reset behind the
 * driver's back. */
static bool kiln_io_read_chip_relays(kiln_io_t *io, uint8_t *out_logical, bool *out_pins_input)
{
    uint8_t dir_b[2] = { 0xFF, 0xFF };
    uint16_t port = 0;
    if (SX1509_read_regs(io->exp, SX1509_REG_DIR_B, dir_b, sizeof(dir_b)) != ESP_OK) return false;
    if (SX1509_read_port(io->exp, &port) != ESP_OK) return false;
    uint16_t dir = (uint16_t)(((uint16_t)dir_b[0] << 8) | dir_b[1]);
    uint16_t driven = (uint16_t)(port & ~dir & (uint16_t)KILN_IO_RELAY_MASK);
    *out_logical = kiln_io_remap_relay_bits((uint8_t)driven);
    if (out_pins_input) *out_pins_input = (dir & (uint16_t)KILN_IO_RELAY_MASK) != 0;
    return true;
}

/* Re-derives the relay shadow from the CHIP after a failed or uncertain write.
 * If the chip cannot be read and allow_safe_off, first try to drive every relay
 * OFF (the only safe direction). If the state still cannot be established the
 * shadow keeps its last verified value -- flipping it to ON would disarm the
 * Pico S3 and ESP H9 guards that catch a stuck coil -- and relay_state_unknown
 * is raised instead (K7 MED-1); the caller still gets its error. A verified chip
 * read clears the flag. Returns true when the chip was read.
 *
 * The fail-safe all-off passes allow_safe_off=false: its own write just failed,
 * repeating it would double the stuck-bus latency (K7 MED-3). */
static bool kiln_io_resync_relay_shadow(kiln_io_t *io, bool allow_safe_off, bool *out_pins_input)
{
    uint8_t actual = 0;
    bool pins_input = false;
    bool known = kiln_io_read_chip_relays(io, &actual, &pins_input);
    if (!known && allow_safe_off) {
        known = SX1509_write_masked(io->exp, (uint16_t)KILN_IO_RELAY_MASK, 0) == ESP_OK &&
                kiln_io_read_chip_relays(io, &actual, &pins_input);
    }
    if (!known) {
        actual = io->relay_shadow; /* keep the last verified state; the failure is reported to the caller */
        io->relay_state_unknown = true;
        ESP_LOGE(TAG, "RELAY STATE UNKNOWN (chip unreadable): shadow stays 0x%X, relay_state_unknown set",
                 actual);
    } else {
        io->relay_state_unknown = false;
    }
    if (actual != io->relay_shadow) {
        ESP_LOGE(TAG, "relay shadow mismatch: commanded 0x%X, chip holds 0x%X (logical) -- "
                      "reporting the chip's value",
                 io->relay_shadow, actual);
        io->relay_shadow = actual;
    }
    if (out_pins_input) *out_pins_input = known && pins_input;
    kiln_io_note_relay_shadow_changed(io);
    return known;
}

/* K7 HIGH-1: a bring-up / re-init that failed after the relay pins became
 * outputs must not leave them driving behind a zeroed shadow. Return the relay
 * pins to inputs (the POR state: gate not driven, coil de-energised). The
 * verifying set_dir either lands -- then shadow 0 is honest -- or fails, in
 * which case the shadow is re-derived from the chip (an energised pin reads ON)
 * or relay_state_unknown is raised. */
static void kiln_io_relays_to_inputs(kiln_io_t *io)
{
    uint16_t dir = (uint16_t)(SX1509_get_dir_shadow(io->exp) | (uint16_t)KILN_IO_RELAY_MASK);
    if (SX1509_set_dir(io->exp, dir) == ESP_OK) {
        io->relay_shadow = 0;
        io->relay_state_unknown = false;
        kiln_io_note_relay_shadow_changed(io);
        ESP_LOGE(TAG, "relay pins returned to inputs after a failed (re)configuration");
        return;
    }
    ESP_LOGE(TAG, "could not return relay pins to inputs -- re-reading the chip");
    (void)kiln_io_resync_relay_shadow(io, false, NULL);
}

/* Steps 2-5 of bring-up (after a reset), then a read-back of the relay pins:
 * they must be outputs, driven low. Only a verified result may mark the board
 * initialised. */
static esp_err_t kiln_io_configure_pins(kiln_io_t *io)
{
    SX1509Class *exp = io->exp;
    esp_err_t err = SX1509_write_port(exp, KILN_IO_SAFE_DATA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "loading safe output levels failed: %s", esp_err_to_name(err));
        return err;
    }
    err = SX1509_set_pullup(exp, KILN_IO_PULLUP_MASK);
    if (err == ESP_OK) err = SX1509_set_pulldown(exp, 0);
    if (err == ESP_OK) err = SX1509_set_open_drain(exp, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "input conditioning failed: %s", esp_err_to_name(err));
        return err;
    }
    uint32_t sense = 0;
    for (uint8_t i = 0; i < KILN_IO_DIGITAL_COUNT; ++i) {
        uint8_t pin = kiln_io_pins[i];
        if (KILN_IO_DIR_MASK & KILN_BIT(pin)) {
            sense |= SX1509_SENSE_FOR_PIN(pin, SX1509_SENSE_BOTH);
        }
    }
    for (uint8_t ch = 0; ch < KILN_IO_DRDY_COUNT; ++ch) {
        sense |= SX1509_SENSE_FOR_PIN(kiln_io_drdy_pins[ch], SX1509_SENSE_FALLING);
    }
    err = SX1509_set_interrupt(exp, (uint16_t)~KILN_IO_DIR_MASK, sense);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "interrupt configuration failed: %s", esp_err_to_name(err));
        return err;
    }
    err = SX1509_set_dir(exp, KILN_IO_DIR_MASK);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "direction configuration failed: %s", esp_err_to_name(err));
        return err;
    }
    (void)SX1509_get_interrupt_source(exp, NULL, true);

    /* Verify from the chip: relay pins are outputs and none reads high. */
    uint8_t dir_b[2] = { 0xFF, 0xFF };
    uint16_t port = 0xFFFFu;
    err = SX1509_read_regs(exp, SX1509_REG_DIR_B, dir_b, sizeof(dir_b));
    if (err == ESP_OK) err = SX1509_read_port(exp, &port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "relay pin read-back failed: %s", esp_err_to_name(err));
        return err;
    }
    uint16_t dir = (uint16_t)(((uint16_t)dir_b[0] << 8) | dir_b[1]);
    if ((dir & (uint16_t)KILN_IO_RELAY_MASK) != 0 || (port & (uint16_t)KILN_IO_RELAY_MASK) != 0) {
        ESP_LOGE(TAG, "relay pin read-back wrong: dir=0x%04X port=0x%04X", dir, port);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t kiln_io_lcd_dc_locked(kiln_io_t *io, bool data);
static esp_err_t kiln_io_lcd_reset_locked(kiln_io_t *io, bool asserted);

/* K7 LOW-1: a reset is a POR, so IO_1..IO_7 directions/pull-ups/levels and the
 * LCD lines go back to defaults. Re-apply what the user had configured (best
 * effort, logged). The relay and display pins keep the verified
 * kiln_io_configure_pins() result; the latch is loaded before the direction so
 * an output never glitches. */
static void kiln_io_restore_user_io(kiln_io_t *io, uint16_t saved_dir, uint16_t saved_pu,
                                    uint8_t saved_io_shadow)
{
    uint16_t user = 0;
    for (uint8_t i = 0; i < KILN_IO_DIGITAL_COUNT; ++i) user |= KILN_BIT(kiln_io_pins[i]);
    uint16_t dir = (uint16_t)((KILN_IO_DIR_MASK & ~user) | (saved_dir & user));
    uint16_t pu = (uint16_t)((KILN_IO_PULLUP_MASK & ~user) | (saved_pu & user));
    uint16_t out_mask = 0, levels = 0;
    for (uint8_t i = 0; i < KILN_IO_DIGITAL_COUNT; ++i) {
        uint16_t bit = KILN_BIT(kiln_io_pins[i]);
        if (!(dir & bit)) {
            out_mask |= bit;
            if (saved_io_shadow & (uint8_t)(1u << i)) levels |= bit;
        }
    }
    esp_err_t err = ESP_OK;
    if (out_mask) err = SX1509_write_masked(io->exp, out_mask, levels);
    if (err == ESP_OK && pu != KILN_IO_PULLUP_MASK) err = SX1509_set_pullup(io->exp, pu);
    if (err == ESP_OK && dir != KILN_IO_DIR_MASK) err = SX1509_set_dir(io->exp, dir);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "user IO configuration not fully restored after re-init: %s", esp_err_to_name(err));
    }
}

/* mode 0 = auto: hard reset when the ~RESET GPIO is wired (a soft reset cannot
 * recover a wedged I2C state machine -- K7 NIT-1), else soft. 1 = soft. 2 = hard
 * (SX1509_reset refuses if the GPIO is not wired). Caller holds io_lock. */
static esp_err_t kiln_io_reinit_locked(kiln_io_t *io, int mode)
{
    /* Captured BEFORE the reset: SX1509_reset() re-initialises the driver shadows. */
    uint16_t saved_dir = SX1509_get_dir_shadow(io->exp);
    uint16_t saved_pu = io->exp->pu_shadow;
    uint8_t saved_io_shadow = io->io_shadow;
    bool saved_dc = io->lcd_dc_data;
    bool saved_reset = io->lcd_reset_asserted;

    io->initialized = false; /* relay ON is refused until the read-back below passes */
    esp_err_t err;
    if (mode == 1) {
        err = SX1509_reset(io->exp, false);
    } else if (mode == 2) {
        err = SX1509_reset(io->exp, true);
    } else {
        err = io->exp->reset_gpio >= 0 ? SX1509_reset(io->exp, true) : ESP_FAIL;
        if (err != ESP_OK) err = SX1509_reset(io->exp, false);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "expander reset failed: %s", esp_err_to_name(err));
        /* K7 LOW-5: the reset may or may not have landed; take the chip's word
         * for the relay state (or raise relay_state_unknown), assume nothing. */
        (void)kiln_io_resync_relay_shadow(io, false, NULL);
        return kiln_io_track(io, err);
    }
    /* All pins are inputs now, so no relay can be energised. */
    io->relay_shadow = 0;
    io->relay_state_unknown = false;
    kiln_io_note_relay_shadow_changed(io);
    err = kiln_io_configure_pins(io);
    if (err != ESP_OK) {
        kiln_io_relays_to_inputs(io); /* K7 HIGH-1 */
        return kiln_io_track(io, err);
    }
    io->initialized = true;
    /* configure_pins left the display lines at SAFE_DATA (D/C data, ~RESET high). */
    io->lcd_dc_data = (KILN_IO_SAFE_DATA & KILN_BIT(SX1509_LCD_DC_PIN)) != 0;
    io->lcd_reset_asserted = false;
    kiln_io_restore_user_io(io, saved_dir, saved_pu, saved_io_shadow);
    if (saved_dc != io->lcd_dc_data) (void)kiln_io_lcd_dc_locked(io, saved_dc);
    if (saved_reset) (void)kiln_io_lcd_reset_locked(io, true);
    ESP_LOGI(TAG, "board I/O re-initialised, all relays off");
    return kiln_io_track(io, ESP_OK);
}

esp_err_t kiln_io_reinit(kiln_io_t *io)
{
    if (!io || !io->exp) return ESP_ERR_INVALID_ARG;
    if (!kiln_io_lock(io, KILN_IO_LOCK_WAIT_MS)) return ESP_ERR_TIMEOUT;
    esp_err_t err = kiln_io_reinit_locked(io, 0);
    kiln_io_unlock(io);
    return err;
}

esp_err_t kiln_io_reset_and_reinit(kiln_io_t *io, bool hard)
{
    if (!io || !io->exp) return ESP_ERR_INVALID_ARG;
    if (!kiln_io_lock(io, KILN_IO_LOCK_WAIT_MS)) return ESP_ERR_TIMEOUT;
    esp_err_t err = kiln_io_reinit_locked(io, hard ? 2 : 1);
    kiln_io_unlock(io);
    return err;
}

bool kiln_io_relay_state_unknown(const kiln_io_t *io)
{
    return io && io->relay_state_unknown;
}

esp_err_t kiln_io_init(kiln_io_t *io, SX1509Class *exp)
{
    if (!io || !exp) return ESP_ERR_INVALID_ARG;

    memset(io, 0, sizeof(*io));
    io->io_lock = xSemaphoreCreateMutex();
    if (!io->io_lock) {
        ESP_LOGE(TAG, "kiln_io lock allocation failed");
        return ESP_ERR_NO_MEM;
    }
    io->exp = exp;
    io->relay_shadow = 0;
    io->io_shadow = 0;
    io->relays_all_off_since_us = -1; /* stamped below once init leaves every relay off */
    io->lcd_dc_data = (KILN_IO_SAFE_DATA & KILN_BIT(SX1509_LCD_DC_PIN)) != 0;
    io->lcd_reset_asserted = false; /* ~RESET is left high, i.e. not asserted */

    /* Step 1: a known starting point. A software reset puts all 16 pins back to
     * inputs -- which drops every relay gate if a warm restart left one
     * energized -- and re-establishes the RegMisc bit the interrupt handling
     * below depends on. Cheap, idempotent, and it means kiln_io_init doesn't
     * have to trust that whoever called it went through SX1509_start. */
    esp_err_t err = SX1509_reset(exp, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "expander reset failed: %s", esp_err_to_name(err));
        return kiln_io_track(io, err);
    }

    /* Steps 2-5: latches first, conditioning, interrupts, then directions; see
     * kiln_io_configure_pins(). Relay pins are read back before init succeeds. */
    err = kiln_io_configure_pins(io);
    if (err != ESP_OK) {
        kiln_io_relays_to_inputs(io); /* K7 HIGH-1: never leave relay pins driving */
        return kiln_io_track(io, err);
    }

    io->initialized = true;
    kiln_io_note_relay_shadow_changed(io); /* relay_shadow is 0 here -- stamps relays_all_off_since_us */
    ESP_LOGI(TAG, "board I/O ready: dir=0x%04X data=0x%04X pullups=0x%04X, all relays off",
             KILN_IO_DIR_MASK, KILN_IO_SAFE_DATA, KILN_IO_PULLUP_MASK);
    return kiln_io_track(io, ESP_OK);
}

/* CT_COMMISSIONING_PLAN.md step 2 -- called after every write that may have
 * changed relay_shadow (both success and the failure/resync path), so
 * relays_all_off_since_us always reflects the CURRENT shadow, never a stale
 * value from before a failed write's resync. */
static void kiln_io_note_relay_shadow_changed(kiln_io_t *io)
{
    if (io->relay_shadow == 0u) {
        if (io->relays_all_off_since_us < 0) {
            io->relays_all_off_since_us = (int64_t)hal_time_now_us();
        }
    } else {
        io->relays_all_off_since_us = -1;
    }
}

esp_err_t kiln_io_set_relay(kiln_io_t *io, uint8_t relay, bool on)
{
    if (!io || relay < 1u || relay > KILN_IO_RELAY_COUNT) return ESP_ERR_INVALID_ARG;
    if (!kiln_io_ready(io)) return ESP_ERR_INVALID_STATE;
    uint8_t bit = (uint8_t)(1u << (relay - 1u));
    return kiln_io_set_relay_mask(io, bit, on ? bit : 0u);
}

static esp_err_t kiln_io_set_relay_mask_locked(kiln_io_t *io, uint8_t mask, uint8_t value)
{
    if (!io) return ESP_ERR_INVALID_ARG;
    if (!kiln_io_ready(io)) return ESP_ERR_INVALID_STATE;

    /* `mask`/`value` are in LOGICAL relay-bit order (bit N-1 = relay N) --
     * every caller (kiln_io_set_relay() above, kiln_io_owner.c) works in
     * that space, and relay_shadow below stays in it too. The actual SX1509
     * pins differ (kiln_relay_logical_to_pin_bit's own comment, above in
     * this file) -- translated to physical bit order only for the wire
     * write, right here, the one place logical meets hardware. */
    mask &= (uint8_t)KILN_IO_RELAY_MASK;
    value &= mask;
    if (mask == 0) return ESP_OK;

    uint8_t phys_mask = kiln_io_remap_relay_bits(mask);
    uint8_t phys_value = kiln_io_remap_relay_bits(value);
    /* K7 LOW-3: the pre-check below only sees the DRIVER's direction shadow. An
     * expander POR the driver never learned about is caught by the write itself:
     * SX1509_write_masked() verifies by reading the pins back, and a relay pin
     * that is still an input does not read what was written, so the write fails
     * (INVALID_RESPONSE) and the failure path below repairs the board. The
     * verify is the real backstop for an unnoticed POR; the shadow check is the
     * cheap early refusal once the driver does know.
     * K7-03: never energise on uncertainty. If the driver's own direction
     * shadow says a relay pin to be driven ON is not an output (an expander
     * reset/POR left it an input), the latch write would "succeed" with nothing
     * driven. Refuse; kiln_io_reinit() is the repair. OFF is always allowed. */
    if ((SX1509_get_dir_shadow(io->exp) & (uint16_t)phys_value) != 0) {
        ESP_LOGE(TAG, "relay ON refused: relay pin(s) 0x%X are not outputs (expander reset?)", phys_value);
        return kiln_io_track(io, ESP_ERR_INVALID_STATE);
    }
    /* K7 MED-1: never energise while the relay state is unknown; OFF stays
     * allowed. Cleared by a verified chip read or a verified all-off. */
    if (phys_value != 0 && io->relay_state_unknown) {
        ESP_LOGE(TAG, "relay ON refused: relay state unknown (a coil may be energised)");
        return kiln_io_track(io, ESP_ERR_INVALID_STATE);
    }
    esp_err_t err = SX1509_write_masked(io->exp, (uint16_t)phys_mask, (uint16_t)phys_value);
    if (err == ESP_OK) {
        io->relay_shadow = (uint8_t)((io->relay_shadow & (uint8_t)~mask) | value);
        kiln_io_note_relay_shadow_changed(io);
        ESP_LOGD(TAG, "relays now 0x%X (mask 0x%X value 0x%X)", io->relay_shadow, mask, value);
    } else {
        /* The commanded state is deliberately NOT updated from `value` on
         * failure. For relays the honest report is "I could not put them where
         * you asked", not "they are where you asked"; a caller comparing
         * relay_shadow against the read-back would otherwise see agreement that
         * isn't real. It is still re-synced against what the part accepted, so
         * a write that landed but failed verification cannot leave the shadow
         * claiming a coil is off while it is energized. */
        ESP_LOGE(TAG, "relay write (mask 0x%X value 0x%X) failed: %s", mask, value,
                 esp_err_to_name(err));
        bool pins_input = false;
        (void)kiln_io_resync_relay_shadow(io, true, &pins_input);
        if (pins_input) {
            /* K7 LOW-4: the relay pins are inputs -- the expander lost its
             * configuration (POR) behind the driver. Nothing is driven, so the
             * relays are off; repair now so the NEXT command works instead of
             * the board sitting half-configured until someone notices. The
             * original error is still what the caller gets. */
            ESP_LOGE(TAG, "relay pins are inputs after a failed write -- re-initialising the expander");
            (void)kiln_io_reinit_locked(io, 0);
        }
    }
    return kiln_io_track(io, err);
}

uint16_t kiln_io_relay_pin_mask(void)
{
    return (uint16_t)KILN_IO_RELAY_MASK;
}

/* Caller holds io_lock. */
static esp_err_t kiln_io_all_relays_off_locked(kiln_io_t *io)
{
    esp_err_t err = SX1509_write_masked(io->exp, (uint16_t)KILN_IO_RELAY_MASK, 0);
    if (err == ESP_OK) {
        io->relay_shadow = 0;
        io->relay_state_unknown = false;
        kiln_io_note_relay_shadow_changed(io);
        ESP_LOGI(TAG, "all relays off");
        return kiln_io_track(io, ESP_OK);
    }
    ESP_LOGE(TAG, "ALL RELAYS OFF FAILED: %s -- checking the chip", esp_err_to_name(err));
    /* K7 MED-3: no safe-off retry inside the resync (it would repeat the write
     * that just failed). */
    bool known = kiln_io_resync_relay_shadow(io, false, NULL);
    if (!known && err != ESP_ERR_INVALID_RESPONSE) {
        /* Transport failure AND an unreadable chip: the bus is dead. A reset +
         * re-init would only spend another ~30 s retrying it (K7 MED-3: the
         * fail-safe path went from ~25 s to ~55 s). Report the failure;
         * relay_state_unknown is already raised, and the watchdog retries this
         * call every period, so the repair happens as soon as the chip answers. */
        ESP_LOGE(TAG, "bus unresponsive -- skipping the reset/re-init, relay state UNKNOWN");
        return kiln_io_track(io, err);
    }
    /* K7-04: a chip POR behind the driver's back leaves relay pins as inputs
     * (safe) but the data write cannot verify. Repair direction via a reset +
     * verified re-configure; if that fails the original error stands (and the
     * board stays not-initialised, so ON is refused). */
    if (kiln_io_reinit_locked(io, 0) == ESP_OK) {
        ESP_LOGI(TAG, "all relays off after re-init");
        return kiln_io_track(io, ESP_OK);
    }
    return kiln_io_track(io, err);
}

static volatile uint32_t s_relay_off_epoch;

uint32_t kiln_io_relay_off_epoch(void)
{
    return s_relay_off_epoch;
}

esp_err_t kiln_io_all_relays_off(kiln_io_t *io)
{
    s_relay_off_epoch++; /* before anything can fail: a queued ON stamped earlier is now stale */
    /* Deliberately the ONE call that does not require a fully initialized
     * board: this is the fail-safe path, and refusing to drop the relays
     * because bring-up did not finish would be exactly backwards. It needs an
     * attached expander and nothing else. */
    if (!io || !io->exp) return ESP_ERR_INVALID_ARG;

    if (kiln_io_lock(io, KILN_IO_FAILSAFE_LOCK_WAIT_MS)) {
        esp_err_t err = kiln_io_all_relays_off_locked(io);
        kiln_io_unlock(io);
        return err;
    }
    /* K7 MED-2: the lock holder is stuck (a dead-bus transfer). The fail-safe
     * must not wait forever: do the safest possible action without the lock --
     * one bare OFF write (the SX1509 driver's own lock still serialises the
     * wire). The unlocked fields are only ever moved toward OFF here, and
     * relay_state_unknown is left RAISED either way: we could not serialise
     * against whatever the holder is doing, so only a later locked, verified
     * all-off may clear it (the watchdog retries every period). */
    ESP_LOGE(TAG, "ALL RELAYS OFF: kiln_io lock busy for %u ms -- unlocked OFF write",
             (unsigned)KILN_IO_FAILSAFE_LOCK_WAIT_MS);
    esp_err_t err = SX1509_write_masked(io->exp, (uint16_t)KILN_IO_RELAY_MASK, 0);
    io->relay_state_unknown = true;
    /* K7 review F3: this write was not serialised against the lock holder, whose own ON write may
     * land AFTER it. So never report ESP_OK and never touch relay_shadow here: the shadow belongs
     * to the lock holder, and a caller that records a verified OFF on ESP_OK (kiln_io_owner's
     * off-tracker) would be wrong. A successful unserialised write returns
     * KILN_IO_ERR_UNSERIALISED_OFF; callers treat any non-OK as "not verified, retry" and the
     * watchdog's next locked all-off clears relay_state_unknown. */
    if (err == ESP_OK) {
        err = KILN_IO_ERR_UNSERIALISED_OFF;
    }
    return kiln_io_track(io, err);
}

static esp_err_t kiln_io_set_io_locked(kiln_io_t *io, uint8_t index, bool level)
{
    if (!io || index < 1u || index > KILN_IO_DIGITAL_COUNT) return ESP_ERR_INVALID_ARG;
    if (!kiln_io_ready(io)) return ESP_ERR_INVALID_STATE;

    uint8_t pin = kiln_io_pins[index - 1u];
    esp_err_t err = SX1509_write_pin(io->exp, pin, level);
    if (err == ESP_OK) {
        uint8_t bit = (uint8_t)(1u << (index - 1u));
        io->io_shadow = (uint8_t)(level ? (io->io_shadow | bit) : (io->io_shadow & ~bit));
    }
    return kiln_io_track(io, err);
}

static esp_err_t kiln_io_set_io_dir_locked(kiln_io_t *io, uint8_t index, bool input, bool pullup)
{
    if (!io || index < 1u || index > KILN_IO_DIGITAL_COUNT) return ESP_ERR_INVALID_ARG;
    if (!kiln_io_ready(io)) return ESP_ERR_INVALID_STATE;

    uint8_t pin = kiln_io_pins[index - 1u];
    uint16_t bit = KILN_BIT(pin);

    /* Pull-up first when becoming an input, direction first when becoming an
     * output. Either way the pin never spends a moment as an output with a
     * pull-up fighting it, and an input never spends a moment floating with the
     * pull-up already removed. */
    uint16_t pu = (uint16_t)((io->exp->pu_shadow & ~bit) | ((input && pullup) ? bit : 0u));
    uint16_t dir = (uint16_t)((SX1509_get_dir_shadow(io->exp) & ~bit) | (input ? bit : 0u));

    esp_err_t err;
    if (input) {
        err = SX1509_set_pullup(io->exp, pu);
        if (err == ESP_OK) err = SX1509_set_dir(io->exp, dir);
    } else {
        err = SX1509_set_dir(io->exp, dir);
        if (err == ESP_OK) err = SX1509_set_pullup(io->exp, pu);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IO_%u dir=%s pullup=%d failed: %s", (unsigned)index, input ? "in" : "out",
                 (int)pullup, esp_err_to_name(err));
    }
    return kiln_io_track(io, err);
}

static esp_err_t kiln_io_read_locked(kiln_io_t *io, kiln_io_state_t *out)
{
    if (!io || !out) return ESP_ERR_INVALID_ARG;
    if (!kiln_io_ready(io)) return ESP_ERR_INVALID_STATE;

    memset(out, 0, sizeof(*out));

    uint16_t data = 0;
    esp_err_t err = SX1509_read_port(io->exp, &data);
    if (err != ESP_OK) {
        /* Still fill in what is known -- the commanded relay state and the ~INT
         * line, neither of which needs the bus -- and flag the failure. A
         * response that says "the expander is dead, and the relays were last
         * commanded to X" is far more useful than no response at all. */
        out->relay_shadow = io->relay_shadow;
        out->dir = SX1509_get_dir_shadow(io->exp);
        out->flags = (uint8_t)((SX1509_irq_asserted(io->exp) ? KILN_IO_FLAG_INT_ASSERTED : 0u) |
                               KILN_IO_FLAG_I2C_FAILED |
                               (io->relay_state_unknown ? KILN_IO_FLAG_RELAY_UNKNOWN : 0u));
        io->last_i2c_failed = true;
        return err;
    }

    /* Reading RegData does NOT release ~INT on this driver's configuration (the
     * part's auto-clear-on-RegData-read is deliberately turned off -- see
     * sx1509_update_misc_locked), so clear the source explicitly. Doing it
     * after the data read means an edge that lands in between keeps its bit and
     * simply re-asserts ~INT: the next read picks it up rather than it being
     * silently swallowed. */
    (void)SX1509_get_interrupt_source(io->exp, NULL, true);

    out->data = data;
    out->dir = SX1509_get_dir_shadow(io->exp);
    out->relay_shadow = io->relay_shadow;

    for (uint8_t i = 0; i < KILN_IO_DIGITAL_COUNT; ++i) {
        if (data & KILN_BIT(kiln_io_pins[i])) out->io_levels |= (uint8_t)(1u << i);
    }
    for (uint8_t ch = 0; ch < KILN_IO_DRDY_COUNT; ++ch) {
        /* ~DRDY is active low: asserted means the pin reads 0. */
        if (!(data & KILN_BIT(kiln_io_drdy_pins[ch]))) out->drdy_bits |= (uint8_t)(1u << ch);
    }
    if (SX1509_irq_asserted(io->exp)) out->flags |= KILN_IO_FLAG_INT_ASSERTED;
    if (io->relay_state_unknown) out->flags |= KILN_IO_FLAG_RELAY_UNKNOWN;

    io->last_i2c_failed = false;
    return ESP_OK;
}

static esp_err_t kiln_io_get_drdy_locked(kiln_io_t *io, uint8_t channel, bool *out_asserted)
{
    if (!io || channel >= KILN_IO_DRDY_COUNT || !out_asserted) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!kiln_io_ready(io)) return ESP_ERR_INVALID_STATE;
    bool level = false;
    esp_err_t err = SX1509_read_pin(io->exp, kiln_io_drdy_pins[channel], &level);
    if (err == ESP_OK) *out_asserted = !level; /* active low */
    return kiln_io_track(io, err);
}

static esp_err_t kiln_io_lcd_dc_locked(kiln_io_t *io, bool data)
{
    if (!io) return ESP_ERR_INVALID_ARG;
    if (!kiln_io_ready(io)) return ESP_ERR_INVALID_STATE;

    /* The cheapest possible version of this call is the one that doesn't
     * happen. The display driver batches a command's whole payload into one SPI
     * transaction precisely so that this runs once per command rather than once
     * per byte, and a run of data-only writes doesn't need to touch the bus at
     * all. */
    if (io->lcd_dc_data == data) return ESP_OK;

    /* One two-byte write of RegDataB alone. Both display lines and all the
     * ~DRDY inputs live in bank B, so writing that single register touches
     * nothing in bank A -- the relays are physically incapable of being
     * disturbed by this path. The byte is built from the SX1509 driver's data
     * shadow, so no read round trip is needed, and SX1509_write_reg keeps that
     * shadow in step afterwards.
     *
     * Writing the latch bits of the bank's input pins (~DRDY, IO_5..IO_7) is
     * harmless: an input pin's output latch drives nothing. */
    uint8_t bank_b = (uint8_t)(SX1509_get_shadow(io->exp) >> 8);
    bank_b = (uint8_t)(data ? (bank_b | KILN_IO_LCD_DC_BANKB_BIT)
                            : (bank_b & (uint8_t)~KILN_IO_LCD_DC_BANKB_BIT));

    esp_err_t err = SX1509_write_reg(io->exp, SX1509_REG_DATA_B, bank_b);
    if (err == ESP_OK) {
        io->lcd_dc_data = data;
    } else {
        ESP_LOGW(TAG, "LCD D/C -> %s failed: %s", data ? "data" : "command", esp_err_to_name(err));
    }
    return kiln_io_track(io, err);
}

static esp_err_t kiln_io_lcd_reset_locked(kiln_io_t *io, bool asserted)
{
    if (!io) return ESP_ERR_INVALID_ARG;
    if (!kiln_io_ready(io)) return ESP_ERR_INVALID_STATE;

    /* The panel's ~RESET is active low, so "asserted" is a LOW pin. This runs
     * twice per display reset, not per command, so it takes the verified
     * masked-write path rather than the D/C fast path -- a reset that silently
     * didn't happen is a panel that never initializes. */
    uint16_t bit = KILN_BIT(SX1509_LCD_RESET_PIN);
    esp_err_t err = SX1509_write_masked(io->exp, bit, asserted ? 0u : bit);
    if (err == ESP_OK) {
        io->lcd_reset_asserted = asserted;
    } else {
        ESP_LOGE(TAG, "LCD ~RESET %s failed: %s", asserted ? "assert" : "release",
                 esp_err_to_name(err));
    }
    return kiln_io_track(io, err);
}

int kiln_io_irq_gpio(const kiln_io_t *io)
{
    return (io && io->exp) ? SX1509_get_irq_gpio(io->exp) : -1;
}

bool kiln_io_irq_asserted(const kiln_io_t *io)
{
    return (io && io->exp) ? SX1509_irq_asserted(io->exp) : false;
}

uint8_t kiln_io_get_relay_shadow(const kiln_io_t *io)
{
    return io ? io->relay_shadow : 0u;
}

uint32_t kiln_io_relays_off_ms(const kiln_io_t *io)
{
    if (!io || io->relays_all_off_since_us < 0) {
        return UINT32_MAX;
    }
    int64_t elapsed_us = (int64_t)hal_time_now_us() - io->relays_all_off_since_us;
    if (elapsed_us < 0) {
        elapsed_us = 0; /* clock anomaly -- never report a negative duration */
    }
    int64_t elapsed_ms = elapsed_us / 1000;
    /* UINT32_MAX is reserved for "a relay is ON" (callers compare against it).
     * A relays-off stretch longer than 49.7 days must saturate one below it,
     * never collide with that sentinel: the H9 CT alarm stops evaluating on
     * UINT32_MAX, so a wrap to the sentinel would silently disarm it. */
    if (elapsed_ms >= (int64_t)UINT32_MAX) {
        return UINT32_MAX - 1u;
    }
    return (uint32_t)elapsed_ms;
}

/* ---- locked public entry points (K7 MED-2) ------------------------------
 * Every multi-step access to the expander is serialised here so a fail-safe
 * re-init from outside the owner task cannot interleave with a relay
 * read-modify-write or an LCD line write. Normal callers wait a bounded time
 * and get ESP_ERR_TIMEOUT; kiln_io_all_relays_off() has its own shorter wait
 * and an unlocked OFF fallback. */
esp_err_t kiln_io_set_relay_mask(kiln_io_t *io, uint8_t mask, uint8_t value)
{
    if (!io) return ESP_ERR_INVALID_ARG;
    if (!kiln_io_lock(io, KILN_IO_LOCK_WAIT_MS)) return ESP_ERR_TIMEOUT;
    esp_err_t err = kiln_io_set_relay_mask_locked(io, mask, value);
    kiln_io_unlock(io);
    return err;
}

esp_err_t kiln_io_set_relay_mask_if_epoch(kiln_io_t *io, uint8_t mask, uint8_t value,
                                          uint32_t since_epoch, bool *out_stale)
{
    if (out_stale) *out_stale = false;
    if (!io) return ESP_ERR_INVALID_ARG;
    if (!kiln_io_lock(io, KILN_IO_LOCK_WAIT_MS)) return ESP_ERR_TIMEOUT;
    if (since_epoch != s_relay_off_epoch) {
        value = 0; /* a fail-safe all-off ran since the caller decided: never close a relay for it */
        if (out_stale) *out_stale = true;
    }
    esp_err_t err = kiln_io_set_relay_mask_locked(io, mask, value);
    kiln_io_unlock(io);
    return err;
}

esp_err_t kiln_io_set_io(kiln_io_t *io, uint8_t index, bool level)
{
    if (!io) return ESP_ERR_INVALID_ARG;
    if (!kiln_io_lock(io, KILN_IO_LOCK_WAIT_MS)) return ESP_ERR_TIMEOUT;
    esp_err_t err = kiln_io_set_io_locked(io, index, level);
    kiln_io_unlock(io);
    return err;
}

esp_err_t kiln_io_set_io_dir(kiln_io_t *io, uint8_t index, bool input, bool pullup)
{
    if (!io) return ESP_ERR_INVALID_ARG;
    if (!kiln_io_lock(io, KILN_IO_LOCK_WAIT_MS)) return ESP_ERR_TIMEOUT;
    esp_err_t err = kiln_io_set_io_dir_locked(io, index, input, pullup);
    kiln_io_unlock(io);
    return err;
}

esp_err_t kiln_io_read(kiln_io_t *io, kiln_io_state_t *out)
{
    if (!io || !out) return ESP_ERR_INVALID_ARG;
    if (!kiln_io_lock(io, KILN_IO_LOCK_WAIT_MS)) return ESP_ERR_TIMEOUT;
    esp_err_t err = kiln_io_read_locked(io, out);
    kiln_io_unlock(io);
    return err;
}

esp_err_t kiln_io_get_drdy(kiln_io_t *io, uint8_t channel, bool *out_asserted)
{
    if (!io) return ESP_ERR_INVALID_ARG;
    if (!kiln_io_lock(io, KILN_IO_LOCK_WAIT_MS)) return ESP_ERR_TIMEOUT;
    esp_err_t err = kiln_io_get_drdy_locked(io, channel, out_asserted);
    kiln_io_unlock(io);
    return err;
}

esp_err_t kiln_io_lcd_dc(kiln_io_t *io, bool data)
{
    if (!io) return ESP_ERR_INVALID_ARG;
    if (!kiln_io_lock(io, KILN_IO_LOCK_WAIT_MS)) return ESP_ERR_TIMEOUT;
    esp_err_t err = kiln_io_lcd_dc_locked(io, data);
    kiln_io_unlock(io);
    return err;
}

esp_err_t kiln_io_lcd_reset(kiln_io_t *io, bool asserted)
{
    if (!io) return ESP_ERR_INVALID_ARG;
    if (!kiln_io_lock(io, KILN_IO_LOCK_WAIT_MS)) return ESP_ERR_TIMEOUT;
    esp_err_t err = kiln_io_lcd_reset_locked(io, asserted);
    kiln_io_unlock(io);
    return err;
}
