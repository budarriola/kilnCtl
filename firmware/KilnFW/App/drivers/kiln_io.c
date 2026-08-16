#include "kiln_io.h"

#include <string.h>

#include "esp_log.h"
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

static const uint8_t kiln_io_drdy_pins[KILN_IO_DRDY_COUNT] = {
    THERMO_DRDY0_EXP_PIN,
    THERMO_DRDY1_EXP_PIN,
    THERMO_DRDY2_EXP_PIN,
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

/* Re-derives the commanded relay state from what the expander last *accepted*.
 *
 * SX1509_write_masked updates the chip driver's data shadow as soon as the part
 * ACKs the write, even if the verifying read-back then disagreed or a later
 * step of the sequence failed -- so after a failure the coils may well have
 * moved. Continuing to report the previous commanded value would be the one lie
 * this layer must never tell: "relay off" while it is energized. Report the
 * mismatch and adopt the expander's view. */
static void kiln_io_resync_relay_shadow(kiln_io_t *io)
{
    uint8_t accepted = (uint8_t)(SX1509_get_shadow(io->exp) & (uint16_t)KILN_IO_RELAY_MASK);
    if (accepted != io->relay_shadow) {
        ESP_LOGE(TAG, "relay shadow mismatch: commanded 0x%X, expander last accepted 0x%X -- "
                      "reporting the expander's value",
                 io->relay_shadow, accepted);
        io->relay_shadow = accepted;
    }
}

esp_err_t kiln_io_init(kiln_io_t *io, SX1509Class *exp)
{
    if (!io || !exp) return ESP_ERR_INVALID_ARG;

    memset(io, 0, sizeof(*io));
    io->exp = exp;
    io->relay_shadow = 0;
    io->io_shadow = 0;
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

    /* Step 2: load the output latches while every pin is still an input. This
     * moves no pin -- it only decides what they will drive the instant they
     * become outputs in step 5. Getting this order wrong is what makes relays
     * click at boot: RegData's power-on value is all ones, so directions-first
     * would energize all four coils until the next transfer landed. */
    err = SX1509_write_port(exp, KILN_IO_SAFE_DATA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "loading safe output levels failed: %s", esp_err_to_name(err));
        return kiln_io_track(io, err);
    }

    /* Step 3: input conditioning. Pull-downs and open-drain are explicitly
     * written to zero rather than assumed: a reset does set them to zero, but
     * this function is also the recovery path after someone has been poking
     * registers over the debug subcommands. */
    err = SX1509_set_pullup(exp, KILN_IO_PULLUP_MASK);
    if (err == ESP_OK) err = SX1509_set_pulldown(exp, 0);
    if (err == ESP_OK) err = SX1509_set_open_drain(exp, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "input conditioning failed: %s", esp_err_to_name(err));
        return kiln_io_track(io, err);
    }

    /* Step 4: interrupts, still before the pins start driving. Every input gets
     * an interrupt; the mask register's polarity is inverted (1 = disabled), so
     * the value written is the complement of the input mask. Sense is both
     * edges on the opto input and the terminal blocks -- a contact opening
     * matters as much as one closing -- and falling only on ~DRDY, whose rising
     * edge is just the part rearming and carries no information. */
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
        return kiln_io_track(io, err);
    }

    /* Step 5: and only now do the outputs start driving -- with the values from
     * step 2. */
    err = SX1509_set_dir(exp, KILN_IO_DIR_MASK);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "direction configuration failed: %s", esp_err_to_name(err));
        return kiln_io_track(io, err);
    }

    /* Clear anything the reconfiguration itself latched (changing a pin's sense
     * clears its event bit, but the pins that were already settled may have
     * recorded an edge as the pull-ups came on) so ~INT starts released. */
    (void)SX1509_get_interrupt_source(exp, NULL, true);

    io->initialized = true;
    ESP_LOGI(TAG, "board I/O ready: dir=0x%04X data=0x%04X pullups=0x%04X, all relays off",
             KILN_IO_DIR_MASK, KILN_IO_SAFE_DATA, KILN_IO_PULLUP_MASK);
    return kiln_io_track(io, ESP_OK);
}

esp_err_t kiln_io_set_relay(kiln_io_t *io, uint8_t relay, bool on)
{
    if (!io || relay < 1u || relay > KILN_IO_RELAY_COUNT) return ESP_ERR_INVALID_ARG;
    if (!kiln_io_ready(io)) return ESP_ERR_INVALID_STATE;
    uint8_t bit = (uint8_t)(1u << (relay - 1u));
    return kiln_io_set_relay_mask(io, bit, on ? bit : 0u);
}

esp_err_t kiln_io_set_relay_mask(kiln_io_t *io, uint8_t mask, uint8_t value)
{
    if (!io) return ESP_ERR_INVALID_ARG;
    if (!kiln_io_ready(io)) return ESP_ERR_INVALID_STATE;

    /* Relay N (1-based) is expander pin N-1, so the relay bit order and the
     * expander bit order are literally the same four low bits -- no shuffling,
     * just a width change. */
    mask &= (uint8_t)KILN_IO_RELAY_MASK;
    value &= mask;
    if (mask == 0) return ESP_OK;

    esp_err_t err = SX1509_write_masked(io->exp, (uint16_t)mask, (uint16_t)value);
    if (err == ESP_OK) {
        io->relay_shadow = (uint8_t)((io->relay_shadow & (uint8_t)~mask) | value);
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
        kiln_io_resync_relay_shadow(io);
    }
    return kiln_io_track(io, err);
}

uint16_t kiln_io_relay_pin_mask(void)
{
    return (uint16_t)KILN_IO_RELAY_MASK;
}

esp_err_t kiln_io_all_relays_off(kiln_io_t *io)
{
    /* Deliberately the ONE call that does not require a fully initialized
     * board: this is the fail-safe path, and refusing to drop the relays
     * because bring-up did not finish would be exactly backwards. It needs an
     * attached expander and nothing else. */
    if (!io || !io->exp) return ESP_ERR_INVALID_ARG;

    esp_err_t err = SX1509_write_masked(io->exp, (uint16_t)KILN_IO_RELAY_MASK, 0);
    if (err == ESP_OK) {
        io->relay_shadow = 0;
        ESP_LOGI(TAG, "all relays off");
    } else {
        ESP_LOGE(TAG, "ALL RELAYS OFF FAILED: %s -- the expander is not answering",
                 esp_err_to_name(err));
        kiln_io_resync_relay_shadow(io);
    }
    return kiln_io_track(io, err);
}

esp_err_t kiln_io_set_io(kiln_io_t *io, uint8_t index, bool level)
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

esp_err_t kiln_io_set_io_dir(kiln_io_t *io, uint8_t index, bool input, bool pullup)
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

esp_err_t kiln_io_read(kiln_io_t *io, kiln_io_state_t *out)
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
                               KILN_IO_FLAG_I2C_FAILED);
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

    io->last_i2c_failed = false;
    return ESP_OK;
}

esp_err_t kiln_io_get_drdy(kiln_io_t *io, uint8_t channel, bool *out_asserted)
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

esp_err_t kiln_io_lcd_dc(kiln_io_t *io, bool data)
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

esp_err_t kiln_io_lcd_reset(kiln_io_t *io, bool asserted)
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
