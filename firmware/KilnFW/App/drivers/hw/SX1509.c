/* ROADMAP.md M15 A1: SX1509.c is the driver itself -- it defines every
 * write/config function, so it needs their declarations too. See
 * SX1509_internal.h's top comment. */
#define SX1509_OWNER_BUILD
#include "SX1509_internal.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/task.h"
#include "settings.h"
#include "stack_margin.h"

static const char *TAG = "SX1509";

/* One transfer never moves more than a handful of bytes; this timeout only has
 * to cover a bus that's momentarily busy with another device's queued
 * transaction. Same value and same reasoning as the PCF8575 driver used. */
#define SX1509_TIMEOUT_MS 1000

/* Long enough for one START/address/STOP, short enough that a sluggish device
 * can't stall the whole four-address sweep -- same as i2c_scan.c. */
#define SX1509_PROBE_TIMEOUT_MS 50

/* Every device on this shared bus runs at the one project-wide clock, so a
 * device's own driver can't quietly re-rate the wires for everyone else. */
#define SX1509_I2C_CLK_HZ I2C_MASTER_FREQ_HZ

/* ~RESET pulse. The datasheet's minimum is 200 ns; 10 us costs nothing and
 * survives any amount of RC on the net. The recovery wait afterwards is the
 * part's internal reset procedure (t_RESET), which the datasheet in this repo
 * DOES specify: 2.5 ms max, hardware/datasheets/mainBoard_SX1509/SX1509.pdf
 * page 8's electrical characteristics table (verified 2026-08-24 -- this
 * comment previously claimed the number was absent and called 5 ms a guess;
 * it is not a guess, it clears the 2.5 ms max with 2x margin). Same table
 * gives the pulse minimum as 200 ns t_PULSE, which 10 us clears easily. */
#define SX1509_RESET_PULSE_US 10
#define SX1509_RESET_RECOVERY_MS 5

/* How long a caller will wait for another caller's read-modify-write to
 * finish. Generous: the longest thing anyone holds this for is a write plus a
 * verify read, each bounded by SX1509_TIMEOUT_MS, times the retry count. */
#define SX1509_LOCK_TIMEOUT_MS (SX1509_TIMEOUT_MS * (I2C_WRITE_RETRY_ATTEMPTS + 1))

/* The four addresses ADDR[1:0] select. Not a range -- see SX1509.h. */
static const uint8_t sx1509_addresses[SX1509_ADDR_COUNT] = {
    SX1509_ADDR_00, SX1509_ADDR_01, SX1509_ADDR_10, SX1509_ADDR_11,
};

/* RegIOnX, the per-pin ON-intensity register. The LED driver register block is
 * NOT a uniform stride: pins 4-7 and 12-15 are fade-capable and carry two extra
 * registers (RegTRise/RegTFall) each, which shifts everything after them. Hence
 * a table rather than arithmetic. Taken from the SX1509B register overview. */
static const uint8_t sx1509_reg_ion[SX1509_PIN_COUNT] = {
    0x2A, 0x2D, 0x30, 0x33, /* I/O[0..3]   -- PWM + blink */
    0x36, 0x3B, 0x40, 0x45, /* I/O[4..7]   -- PWM + blink + fade */
    0x4A, 0x4D, 0x50, 0x53, /* I/O[8..11]  -- PWM + blink */
    0x56, 0x5B, 0x60, 0x65, /* I/O[12..15] -- PWM + blink + fade */
};

/* ---------------------------------------------------------------------------
 * Locking and transport
 * ------------------------------------------------------------------------ */

static bool sx1509_lock(SX1509Class *e)
{
    if (!e->lock) return true; /* not yet created: single-threaded init path */
    return xSemaphoreTake(e->lock, pdMS_TO_TICKS(SX1509_LOCK_TIMEOUT_MS)) == pdTRUE;
}

static void sx1509_unlock(SX1509Class *e)
{
    if (e->lock) xSemaphoreGive(e->lock);
}

/* The part's register file is 0x00..0x7F. A raw read or write past it (which
 * the debug SX_READ_REG/SX_WRITE_REG subcommands can ask for, since they carry
 * a whole byte) is not a register at all -- and a multi-register read that
 * starts in range and runs off the end would fill the caller's buffer with
 * whatever the part clocks out there. */
#define SX1509_REG_MAX 0x7Fu

static bool sx1509_reg_range_valid(uint8_t reg, size_t len)
{
    return len > 0 && reg <= SX1509_REG_MAX && ((size_t)reg + len) <= (SX1509_REG_MAX + 1u);
}

static bool sx1509_addr_valid(uint8_t addr)
{
    for (size_t i = 0; i < SX1509_ADDR_COUNT; ++i) {
        if (sx1509_addresses[i] == addr) return true;
    }
    return false;
}

static esp_err_t sx1509_add_device(SX1509Class *e, uint8_t addr)
{
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = SX1509_I2C_CLK_HZ,
    };
    return i2c_master_bus_add_device(e->bus, &dev_config, &e->dev);
}

static esp_err_t sx1509_transfer(SX1509Class *e, const uint8_t *tx, size_t tx_len, uint8_t *rx,
                                 size_t rx_len)
{
    if (e->owner_initialized) {
        return i2c_owner_transfer(&e->owner, e->dev, tx, tx_len, rx, rx_len, SX1509_TIMEOUT_MS);
    }
    /* Fallback for an instance whose owner task failed to start: the raw
     * i2c_master calls still work, they just aren't serialized behind a queue. */
    if (tx && tx_len > 0 && rx && rx_len > 0) {
        return i2c_master_transmit_receive(e->dev, tx, tx_len, rx, rx_len, SX1509_TIMEOUT_MS);
    }
    if (tx && tx_len > 0) {
        return i2c_master_transmit(e->dev, tx, tx_len, SX1509_TIMEOUT_MS);
    }
    return i2c_master_receive(e->dev, rx, rx_len, SX1509_TIMEOUT_MS);
}

/* Read `len` consecutive registers starting at `reg`. Depends on the part's
 * address auto-increment (RegMisc bit 1 = 0, the default, which this driver
 * never changes) for len > 1. */
static esp_err_t sx1509_read_raw(SX1509Class *e, uint8_t reg, uint8_t *out, size_t len)
{
    return sx1509_transfer(e, &reg, 1, out, len);
}

/* Write `len` consecutive registers starting at `reg`, retrying the transfer --
 * and only the transfer -- on a transport error. Verification, where it means
 * anything, is the caller's job. */
static esp_err_t sx1509_write_raw(SX1509Class *e, uint8_t reg, const uint8_t *vals, size_t len)
{
    uint8_t buf[5];
    /* `vals` is checked here rather than only at the entry points: every 8-,
     * 16- and 32-bit write in this file funnels through it, and a NULL would
     * reach the memcpy below. */
    if (!vals || len == 0) return ESP_ERR_INVALID_ARG;
    if (len + 1 > sizeof(buf)) return ESP_ERR_INVALID_SIZE;
    buf[0] = reg;
    memcpy(&buf[1], vals, len);

    esp_err_t err = ESP_FAIL;
    for (unsigned attempt = 1; attempt <= I2C_WRITE_RETRY_ATTEMPTS; ++attempt) {
        err = sx1509_transfer(e, buf, len + 1, NULL, 0);
        if (err == ESP_OK) return ESP_OK;
        ESP_LOGW(TAG, "write reg 0x%02X (%u bytes) to 0x%02X failed (attempt %u/%u): %s", reg,
                 (unsigned)len, e->addr, attempt, (unsigned)I2C_WRITE_RETRY_ATTEMPTS,
                 esp_err_to_name(err));
    }
    return err;
}

/* ---------------------------------------------------------------------------
 * Shadow bookkeeping
 *
 * A raw single-register write from the PC (SX_WRITE_REG) must not leave the
 * driver believing something else about the register it just changed, or the
 * next read-modify-write would undo it. Only the registers that back a shadow
 * are listed; everything else is fire-and-forget as far as this driver's state
 * is concerned.
 * ------------------------------------------------------------------------ */
static void sx1509_note_reg_write(SX1509Class *e, uint8_t reg, uint8_t value)
{
    struct {
        uint8_t reg_b;
        uint16_t *shadow;
    } const map[] = {
        { SX1509_REG_DATA_B, &e->data_shadow },   { SX1509_REG_DIR_B, &e->dir_shadow },
        { SX1509_REG_PULLUP_B, &e->pu_shadow },   { SX1509_REG_PULLDOWN_B, &e->pd_shadow },
        { SX1509_REG_OPEN_DRAIN_B, &e->od_shadow }, { SX1509_REG_LED_ENABLE_B, &e->led_shadow },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        if (reg == map[i].reg_b) { /* bank B holds I/O[15:8] -- the high byte */
            *map[i].shadow = (uint16_t)((*map[i].shadow & 0x00FFu) | ((uint16_t)value << 8));
            return;
        }
        if (reg == (uint8_t)(map[i].reg_b + 1u)) { /* bank A holds I/O[7:0] */
            *map[i].shadow = (uint16_t)((*map[i].shadow & 0xFF00u) | value);
            return;
        }
    }
}

/* ---------------------------------------------------------------------------
 * 16-bit register pair helpers
 *
 * Bank B (I/O[15:8]) sits at the LOWER address of every pair, so a two-byte
 * auto-incremented burst carries the HIGH byte first: these pairs are
 * big-endian on the wire. Everything above this layer speaks uint16_t with
 * bit N = pin N.
 * ------------------------------------------------------------------------ */
static esp_err_t sx1509_write16_raw(SX1509Class *e, uint8_t reg_b, uint16_t value)
{
    const uint8_t bytes[2] = {
        (uint8_t)(value >> 8),   /* bank B, I/O[15:8] */
        (uint8_t)(value & 0xFF), /* bank A, I/O[7:0]  */
    };
    esp_err_t err = sx1509_write_raw(e, reg_b, bytes, sizeof(bytes));
    if (err == ESP_OK) {
        sx1509_note_reg_write(e, reg_b, bytes[0]);
        sx1509_note_reg_write(e, (uint8_t)(reg_b + 1u), bytes[1]);
    }
    return err;
}

static esp_err_t sx1509_read16_raw(SX1509Class *e, uint8_t reg_b, uint16_t *out_value)
{
    uint8_t bytes[2] = { 0, 0 };
    esp_err_t err = sx1509_read_raw(e, reg_b, bytes, sizeof(bytes));
    if (err != ESP_OK) return err;
    *out_value = (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
    return ESP_OK;
}

/* Write a 16-bit register pair and confirm the part holds it, over the bits in
 * verify_mask only. Used for the plain read/write configuration registers,
 * where an exact compare is meaningful because nothing outside the chip can
 * influence what they read back. A mismatch is retried along with the write --
 * it is indistinguishable from a write that never landed, and the fix for both
 * is the same. */
static esp_err_t sx1509_write16_verified(SX1509Class *e, uint8_t reg_b, uint16_t value,
                                         uint16_t verify_mask, const char *what)
{
    esp_err_t err = ESP_FAIL;
    for (unsigned attempt = 1; attempt <= I2C_WRITE_RETRY_ATTEMPTS; ++attempt) {
        err = sx1509_write16_raw(e, reg_b, value);
        if (err != ESP_OK) continue; /* sx1509_write_raw already logged and retried */

        if (verify_mask == 0) return ESP_OK;

        uint16_t readback = 0;
        esp_err_t read_err = sx1509_read16_raw(e, reg_b, &readback);
        if (read_err != ESP_OK) {
            ESP_LOGW(TAG, "%s = 0x%04X: read-back failed (attempt %u/%u): %s", what, value,
                     attempt, (unsigned)I2C_WRITE_RETRY_ATTEMPTS, esp_err_to_name(read_err));
            err = read_err;
            continue;
        }

        uint16_t wrong = (uint16_t)((readback ^ value) & verify_mask);
        if (wrong == 0) return ESP_OK;

        ESP_LOGW(TAG, "%s = 0x%04X: read back 0x%04X, bits 0x%04X disagree (attempt %u/%u)", what,
                 value, readback, wrong, attempt, (unsigned)I2C_WRITE_RETRY_ATTEMPTS);
        err = ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGE(TAG, "%s = 0x%04X failed after %u attempts: %s", what, value,
             (unsigned)I2C_WRITE_RETRY_ATTEMPTS, esp_err_to_name(err));
    return err;
}

/* Which RegData bits it is honest to check on a read-back. A read of RegData
 * returns *pin* states, so only a pin this device is actively pushing both ways
 * is guaranteed to agree with what was written:
 *   - an input pin reads the outside world,
 *   - an open-drain output "driven high" reads whatever is pulling the net,
 *   - an LED-driver pin reads a sample of a PWM waveform.
 * None of those disagreeing is a fault, so none of them is verified. */
static uint16_t sx1509_data_verify_mask(const SX1509Class *e)
{
    return (uint16_t)(~e->dir_shadow & ~e->od_shadow & ~e->led_shadow);
}

static esp_err_t sx1509_write_port_locked(SX1509Class *e, uint16_t value)
{
    /* The shadow tracks what this driver drove even when verification fails --
     * that is still the last value it put on the pins, and the next
     * read-modify-write has to build on it rather than on nothing. It is
     * updated inside sx1509_write16_raw, i.e. as soon as the part ACKs, which
     * is exactly that semantic. */
    uint16_t verify = sx1509_data_verify_mask(e);
    return sx1509_write16_verified(e, SX1509_REG_DATA_B, value, verify, "RegData");
}

/* Reset every shadow to the part's power-on values. Called after either flavour
 * of reset and after re-addressing, where continuing to believe the old values
 * would be worse than believing the datasheet's. */
static void sx1509_reset_shadows(SX1509Class *e)
{
    e->dir_shadow = SX1509_DIR_POWER_ON_STATE;
    e->data_shadow = SX1509_DATA_POWER_ON_STATE;
    e->pu_shadow = 0;
    e->pd_shadow = 0;
    e->od_shadow = 0;
    e->led_shadow = 0;
    e->osc_enabled = false;
    e->led_clock_div = 0;
}

/* RegMisc read-modify-write. Two bits in here are load-bearing and must survive
 * every other change:
 *   bit 1 (auto-increment) must stay 0 -- every 16-bit pair access and every
 *         multi-register read in this driver assumes it;
 *   bit 0 must stay 1 -- with it 0 the part clears RegInterruptSource (and
 *         releases ~INT) on any RegData read, so ordinary polling of the port
 *         would silently eat the very edges the bridge is polling to find out
 *         about. Clearing is explicit here, via SX1509_get_interrupt_source. */
static esp_err_t sx1509_update_misc_locked(SX1509Class *e, uint8_t clear_bits, uint8_t set_bits)
{
    uint8_t misc = 0;
    esp_err_t err = sx1509_read_raw(e, SX1509_REG_MISC, &misc, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RegMisc read failed: %s", esp_err_to_name(err));
        return err;
    }
    uint8_t next = (uint8_t)(((misc & (uint8_t)~clear_bits) | set_bits) & (uint8_t)~SX1509_MISC_NO_AUTOINC);
    next |= SX1509_MISC_NO_AUTOCLEAR;
    if (next == misc) return ESP_OK;

    err = sx1509_write_raw(e, SX1509_REG_MISC, &next, 1);
    if (err != ESP_OK) return err;

    uint8_t readback = 0;
    if (sx1509_read_raw(e, SX1509_REG_MISC, &readback, 1) == ESP_OK && readback != next) {
        ESP_LOGW(TAG, "RegMisc = 0x%02X read back 0x%02X", next, readback);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

/* Switch the internal 2 MHz oscillator on if it isn't already, and optionally
 * give the LED drivers a clock.
 *
 * RegClock[6:5] = OFF is the reset default, and with it off the debouncer, the
 * keypad engine and every LED driver are simply inert -- which is why both
 * SX1509_set_debounce and SX1509_led_driver come through here first instead of
 * this living in init. This board's expander does relays, terminal-block I/O
 * and the display's D/C line; none of that needs a clock, so the oscillator
 * stays off (and its quiescent current unspent) until something asks for a
 * feature that genuinely requires it. */
static esp_err_t sx1509_ensure_oscillator_locked(SX1509Class *e, bool need_led_clock)
{
    if (!e->osc_enabled) {
        uint8_t clock = SX1509_CLOCK_OSC_INTERNAL; /* OSCIO left as an input, OSCOUT off */
        esp_err_t err = sx1509_write_raw(e, SX1509_REG_CLOCK, &clock, 1);
        if (err != ESP_OK) return err;

        uint8_t readback = 0;
        if (sx1509_read_raw(e, SX1509_REG_CLOCK, &readback, 1) == ESP_OK && readback != clock) {
            ESP_LOGW(TAG, "RegClock = 0x%02X read back 0x%02X", clock, readback);
            return ESP_ERR_INVALID_RESPONSE;
        }
        e->osc_enabled = true;
        ESP_LOGI(TAG, "internal 2MHz oscillator enabled");
    }

    if (need_led_clock && e->led_clock_div == 0) {
        /* ClkX = fOSC / 2^(n-1). n = 1 gives the full 2 MHz, which at the
         * part's 255-step PWM is ~7.8 kHz -- well above anything an eye or a
         * slow opto cares about, and the simplest choice to explain. */
        const uint8_t div = 1;
        esp_err_t err = sx1509_update_misc_locked(e, SX1509_MISC_LED_CLK_MASK,
                                                  (uint8_t)(div << SX1509_MISC_LED_CLK_SHIFT));
        if (err != ESP_OK) return err;
        e->led_clock_div = div;
        ESP_LOGI(TAG, "LED driver clock enabled (ClkX = fOSC)");
    }
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------ */

esp_err_t SX1509_init(SX1509Class *e, i2c_master_bus_handle_t bus, uint8_t addr, int irq_gpio,
                      int reset_gpio)
{
    if (!e || !bus || !sx1509_addr_valid(addr)) {
        return ESP_ERR_INVALID_ARG;
    }
    /* A GPIO number that isn't one would be shifted into pin_bit_mask below --
     * undefined behaviour for a shift past the width of the type, and a wrong
     * pin configured if it happens to land. -1 means "not wired" and is fine. */
    if ((irq_gpio >= 0 && !GPIO_IS_VALID_GPIO(irq_gpio)) ||
        (reset_gpio >= 0 && !GPIO_IS_VALID_OUTPUT_GPIO(reset_gpio))) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Re-initializing a live instance would memset away the device handle, the
     * owner task and the mutex without freeing any of them. Deinit first. */
    if (e->dev || e->owner_initialized || e->lock) {
        ESP_LOGE(TAG, "init called on an instance that is already up (0x%02X)", e->addr);
        return ESP_ERR_INVALID_STATE;
    }

    memset(e, 0, sizeof(*e));
    e->bus = bus;
    e->addr = addr;
    e->irq_gpio = irq_gpio;
    e->reset_gpio = reset_gpio;
    sx1509_reset_shadows(e);

    e->lock = xSemaphoreCreateMutex();
    if (!e->lock) {
        ESP_LOGE(TAG, "mutex allocation failed");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = sx1509_add_device(e, addr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device(0x%02X) failed: %s", addr, esp_err_to_name(err));
        vSemaphoreDelete(e->lock);
        e->lock = NULL;
        return err;
    }

    /* Independent i2c_owner on the same (already-existing) bus handle -- this
     * driver never creates or destroys the bus itself.
     * TODO (HAL Phase 1b): still calls i2c_owner_* directly instead of
     * hal_i2c.h -- see docs/HW_ABSTRACTION_PLAN.md's FT6336U.c writeup for
     * why that file went first (this one is the live relay-safety-critical
     * expander driver, not a zero-blast-radius one) and the device-detach
     * interface gap this migration would have to solve first
     * (SX1509_deinit()/SX1509_set_address() both call
     * i2c_master_bus_rm_device(), which hal_i2c.h has no equivalent for). */
    err = i2c_owner_init(&e->owner, bus, 8, 5, 4096, tskNO_AFFINITY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_owner_init failed: %s", esp_err_to_name(err));
        i2c_master_bus_rm_device(e->dev);
        e->dev = NULL;
        vSemaphoreDelete(e->lock);
        e->lock = NULL;
        return err;
    }
    e->owner_initialized = true;

    /* i2c_owner_init() is shared with NS2009.c's touch-controller owner
     * (different stack_depth, 3072) and deliberately does NOT register
     * itself for stack-margin reporting -- see i2c_owner.c's comment at
     * its call site. Registering here, under a name distinct from
     * NS2009's, keeps the two independently identifiable in a
     * GET_STACK_MARGIN report instead of colliding under one shared
     * "i2c_owner" name. */
    stack_margin_register("i2c_owner_sx1509", &e->owner.task_handle, 4096);

    if (reset_gpio >= 0) {
        /* Drive the level register before switching the pin to an output, so
         * ~RESET never sees a low glitch on the way to being deasserted. */
        gpio_set_level((gpio_num_t)reset_gpio, 1);
        gpio_config_t rst_cfg = {
            .pin_bit_mask = 1ULL << reset_gpio,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&rst_cfg);
        if (err != ESP_OK) {
            /* Forget the pin rather than remember one that is not an output:
             * SX1509_reset(hard) would otherwise toggle a level register that
             * drives nothing and report success for a reset that never
             * happened. With it at -1 a hard reset is refused explicitly. */
            ESP_LOGW(TAG, "~RESET GPIO%d config failed: %s -- hard reset disabled", reset_gpio,
                     esp_err_to_name(err));
            e->reset_gpio = -1;
        } else {
            gpio_set_level((gpio_num_t)reset_gpio, 1);
        }
    }

    if (irq_gpio >= 0) {
        /* ~INT is open-drain, so it needs a pull-up; there is none on the main
         * board. No ISR is installed and no interrupt type is enabled -- see
         * SX1509_get_irq_gpio(): whoever owns the expander decides how it wants
         * to hear about an edge, and this driver refuses to assume. */
        gpio_config_t irq_cfg = {
            .pin_bit_mask = 1ULL << irq_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&irq_cfg);
        if (err != ESP_OK) {
            /* Same reasoning as ~RESET: an unconfigured pin read by
             * SX1509_irq_asserted() would report a level that means nothing,
             * and "~INT is asserted" is not something to invent. */
            ESP_LOGW(TAG, "~INT GPIO%d config failed: %s -- IRQ reporting disabled", irq_gpio,
                     esp_err_to_name(err));
            e->irq_gpio = -1;
        }
    }

    /* Deliberately no register traffic here, same as PCF8575_init: the part
     * comes out of reset with every pin an input, which is the safe state, and
     * nothing about a firmware restart justifies moving a relay gate or the
     * display's ~RESET. kiln_io_init does the configuring, in the safe order. */
    ESP_LOGI(TAG, "SX1509 initialized addr=0x%02X irq=GPIO%d reset=GPIO%d", addr, irq_gpio,
             reset_gpio);
    return ESP_OK;
}

esp_err_t SX1509_deinit(SX1509Class *e)
{
    if (!e) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ESP_OK;

    /* Take the lock first, and drop the device handle under it, so a caller
     * already inside a read-modify-write finishes before anything it is using
     * is torn down. Every public entry point checks e->dev after taking the
     * lock's place in the queue, so clearing it here is what turns a later call
     * into ESP_ERR_INVALID_ARG instead of a transfer on a freed handle. */
    bool locked = sx1509_lock(e);
    i2c_master_dev_handle_t dev = e->dev;
    e->dev = NULL;

    if (e->owner_initialized) {
        esp_err_t sub = i2c_owner_deinit(&e->owner);
        if (sub != ESP_OK) err = sub;
        e->owner_initialized = false;
    }
    if (dev) {
        esp_err_t sub = i2c_master_bus_rm_device(dev);
        if (sub != ESP_OK) err = sub;
    }
    if (e->lock) {
        SemaphoreHandle_t lock = e->lock;
        e->lock = NULL;
        if (locked) xSemaphoreGive(lock);
        vSemaphoreDelete(lock);
    }
    /* The bus belongs to whoever created it; never delete it here. */
    return err;
}

esp_err_t SX1509_start(SX1509Class *e, i2c_master_bus_handle_t bus)
{
    if (!e || !bus) return ESP_ERR_INVALID_ARG;

    /* Probe before committing to the address: an absent part turns every
     * subsequent transfer into a NACK that i2c_owner answers with a bus reset
     * and a retry, so the boot path would stall for seconds per register
     * instead of failing in one 50 ms probe. */
    esp_err_t probe_err = i2c_master_probe(bus, SX1509_I2C_ADDR, SX1509_PROBE_TIMEOUT_MS);
    if (probe_err != ESP_OK) {
        ESP_LOGE(TAG, "no SX1509 at 0x%02X: %s", SX1509_I2C_ADDR, esp_err_to_name(probe_err));
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = SX1509_init(e, bus, SX1509_I2C_ADDR, SX1509_IRQ_IO, SX1509_RESET_IO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SX1509_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Reset before anything else touches it. A warm restart of the firmware
     * (an OTA, a watchdog, a debugger) leaves the expander still holding
     * whatever the previous run configured -- possibly with relays energized.
     * A reset puts all 16 pins back to inputs, which drops every relay gate,
     * and gives kiln_io_init a known starting point. */
    err = SX1509_reset(e, e->reset_gpio >= 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SX1509 at 0x%02X did not reset: %s", e->addr, esp_err_to_name(err));
        SX1509_deinit(e);
        return err;
    }

    /* One read-back so a part that's configured but not actually answering
     * shows up as an error line at boot rather than on the first GUI command.
     * RegDir is the right register to look at: it reads 0xFFFF after a reset
     * and nothing outside the chip can influence it. */
    uint16_t dir = 0;
    err = SX1509_read_reg16(e, SX1509_REG_DIR_B, &dir);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SX1509 at 0x%02X did not answer a read: %s", e->addr, esp_err_to_name(err));
        SX1509_deinit(e);
        return err;
    }
    if (dir != SX1509_DIR_POWER_ON_STATE) {
        ESP_LOGW(TAG, "RegDir reads 0x%04X after reset, expected 0x%04X -- is this really an "
                      "SX1509?",
                 dir, SX1509_DIR_POWER_ON_STATE);
    }

    ESP_LOGI(TAG, "SX1509 ready at 0x%02X, RegDir reads 0x%04X", e->addr, dir);
    return ESP_OK;
}

esp_err_t SX1509_set_address(SX1509Class *e, uint8_t addr)
{
    if (!e || !e->bus || !sx1509_addr_valid(addr)) return ESP_ERR_INVALID_ARG;
    if (addr == e->addr && e->dev) return ESP_OK;

    /* Refuse to point at an address nothing lives on -- i2c_master_bus_add_device
     * registers any address happily, and the failure would then arrive one
     * NACKed transfer at a time, each costing a bus reset and a retry. */
    esp_err_t probe_err = i2c_master_probe(e->bus, addr, SX1509_PROBE_TIMEOUT_MS);
    if (probe_err != ESP_OK) {
        ESP_LOGW(TAG, "re-address to 0x%02X refused: nothing answered there (%s); still on 0x%02X",
                 addr, esp_err_to_name(probe_err), e->addr);
        return ESP_ERR_NOT_FOUND;
    }

    if (!sx1509_lock(e)) return ESP_ERR_TIMEOUT;

    i2c_master_dev_handle_t old_dev = e->dev;
    e->dev = NULL;
    esp_err_t err = sx1509_add_device(e, addr);
    if (err != ESP_OK) {
        /* Keep the old handle so the instance stays usable at its previous
         * address rather than being left with no device at all. */
        e->dev = old_dev;
        sx1509_unlock(e);
        ESP_LOGE(TAG, "re-address to 0x%02X failed: %s (still on 0x%02X)", addr,
                 esp_err_to_name(err), e->addr);
        return err;
    }
    if (old_dev) i2c_master_bus_rm_device(old_dev);

    e->addr = addr;
    /* A different chip's registers have nothing to do with the old one's. */
    sx1509_reset_shadows(e);
    sx1509_unlock(e);

    ESP_LOGI(TAG, "SX1509 now addressed at 0x%02X", addr);
    return ESP_OK;
}

esp_err_t SX1509_scan(i2c_master_bus_handle_t bus, uint8_t *out_addrs, size_t max_addrs,
                      size_t *out_count)
{
    if (!bus) return ESP_ERR_INVALID_ARG;

    size_t found = 0;
    for (size_t i = 0; i < SX1509_ADDR_COUNT; ++i) {
        uint8_t addr = sx1509_addresses[i];
        esp_err_t err = i2c_master_probe(bus, addr, SX1509_PROBE_TIMEOUT_MS);
        if (err == ESP_OK) {
            if (out_addrs && found < max_addrs) out_addrs[found] = addr;
            found++;
        } else if (err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "probe 0x%02X: %s", addr, esp_err_to_name(err));
        }
    }

    if (out_count) {
        *out_count = (out_addrs && found > max_addrs) ? max_addrs : found;
    }
    ESP_LOGI(TAG, "address scan (0x3E/0x3F/0x70/0x71): %u responded", (unsigned)found);
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * Raw register access
 * ------------------------------------------------------------------------ */

esp_err_t SX1509_write_reg(SX1509Class *e, uint8_t reg, uint8_t value)
{
    if (!e) return ESP_ERR_INVALID_ARG;
    if (!sx1509_reg_range_valid(reg, 1)) return ESP_ERR_INVALID_ARG;
    if (!e->dev) return ESP_ERR_INVALID_STATE;
    if (!sx1509_lock(e)) return ESP_ERR_TIMEOUT;
    esp_err_t err = sx1509_write_raw(e, reg, &value, 1);
    if (err == ESP_OK) sx1509_note_reg_write(e, reg, value);
    sx1509_unlock(e);
    return err;
}

esp_err_t SX1509_read_reg(SX1509Class *e, uint8_t reg, uint8_t *out_value)
{
    return SX1509_read_regs(e, reg, out_value, 1);
}

esp_err_t SX1509_read_regs(SX1509Class *e, uint8_t reg, uint8_t *out, size_t len)
{
    if (!e || !out || !sx1509_reg_range_valid(reg, len)) return ESP_ERR_INVALID_ARG;
    if (!e->dev) return ESP_ERR_INVALID_STATE;
    if (!sx1509_lock(e)) return ESP_ERR_TIMEOUT;
    esp_err_t err = sx1509_read_raw(e, reg, out, len);
    sx1509_unlock(e);
    return err;
}

esp_err_t SX1509_write_reg16(SX1509Class *e, uint8_t reg, uint16_t value)
{
    /* `reg` is the bank-B (lower) address of a pair, so reg+1 must exist too. */
    if (!e || !sx1509_reg_range_valid(reg, 2)) return ESP_ERR_INVALID_ARG;
    if (!e->dev) return ESP_ERR_INVALID_STATE;
    if (!sx1509_lock(e)) return ESP_ERR_TIMEOUT;
    esp_err_t err = sx1509_write16_raw(e, reg, value);
    sx1509_unlock(e);
    return err;
}

esp_err_t SX1509_read_reg16(SX1509Class *e, uint8_t reg, uint16_t *out_value)
{
    if (!e || !out_value || !sx1509_reg_range_valid(reg, 2)) return ESP_ERR_INVALID_ARG;
    if (!e->dev) return ESP_ERR_INVALID_STATE;
    if (!sx1509_lock(e)) return ESP_ERR_TIMEOUT;
    esp_err_t err = sx1509_read16_raw(e, reg, out_value);
    sx1509_unlock(e);
    return err;
}

/* ---------------------------------------------------------------------------
 * Pin configuration
 * ------------------------------------------------------------------------ */

esp_err_t SX1509_set_dir(SX1509Class *e, uint16_t dir_mask)
{
    if (!e) return ESP_ERR_INVALID_ARG;
    if (!e->dev) return ESP_ERR_INVALID_STATE; /* never attached, or already deinit'd */
    if (!sx1509_lock(e)) return ESP_ERR_TIMEOUT;
    esp_err_t err = sx1509_write16_verified(e, SX1509_REG_DIR_B, dir_mask, 0xFFFFu, "RegDir");
    sx1509_unlock(e);
    return err;
}

esp_err_t SX1509_set_pullup(SX1509Class *e, uint16_t mask)
{
    if (!e) return ESP_ERR_INVALID_ARG;
    if (!e->dev) return ESP_ERR_INVALID_STATE; /* never attached, or already deinit'd */
    if (!sx1509_lock(e)) return ESP_ERR_TIMEOUT;
    esp_err_t err = sx1509_write16_verified(e, SX1509_REG_PULLUP_B, mask, 0xFFFFu, "RegPullUp");
    sx1509_unlock(e);
    return err;
}

esp_err_t SX1509_set_pulldown(SX1509Class *e, uint16_t mask)
{
    if (!e) return ESP_ERR_INVALID_ARG;
    if (!e->dev) return ESP_ERR_INVALID_STATE; /* never attached, or already deinit'd */
    if (!sx1509_lock(e)) return ESP_ERR_TIMEOUT;
    esp_err_t err = sx1509_write16_verified(e, SX1509_REG_PULLDOWN_B, mask, 0xFFFFu, "RegPullDown");
    sx1509_unlock(e);
    return err;
}

esp_err_t SX1509_set_open_drain(SX1509Class *e, uint16_t mask)
{
    if (!e) return ESP_ERR_INVALID_ARG;
    if (!e->dev) return ESP_ERR_INVALID_STATE; /* never attached, or already deinit'd */
    if (!sx1509_lock(e)) return ESP_ERR_TIMEOUT;
    esp_err_t err = sx1509_write16_verified(e, SX1509_REG_OPEN_DRAIN_B, mask, 0xFFFFu,
                                            "RegOpenDrain");
    sx1509_unlock(e);
    return err;
}

esp_err_t SX1509_set_debounce(SX1509Class *e, uint16_t enable_mask, uint8_t config)
{
    if (!e || config > 7u) return ESP_ERR_INVALID_ARG;
    if (!e->dev) return ESP_ERR_INVALID_STATE;
    if (!sx1509_lock(e)) return ESP_ERR_TIMEOUT;

    esp_err_t err = ESP_OK;
    /* The debouncer is clocked from fOSC; with RegClock[6:5] = OFF it does
     * nothing at all, so anything but "disable everything" needs the
     * oscillator running first. */
    if (enable_mask != 0) {
        err = sx1509_ensure_oscillator_locked(e, false);
        if (err != ESP_OK) {
            sx1509_unlock(e);
            return err;
        }
    }

    uint8_t cfg = (uint8_t)(config & 0x07u);
    err = sx1509_write_raw(e, SX1509_REG_DEBOUNCE_CONFIG, &cfg, 1);
    if (err == ESP_OK) {
        uint8_t readback = 0;
        if (sx1509_read_raw(e, SX1509_REG_DEBOUNCE_CONFIG, &readback, 1) == ESP_OK &&
            (readback & 0x07u) != cfg) {
            ESP_LOGW(TAG, "RegDebounceConfig = 0x%02X read back 0x%02X", cfg, readback);
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (err == ESP_OK) {
        err = sx1509_write16_verified(e, SX1509_REG_DEBOUNCE_EN_B, enable_mask, 0xFFFFu,
                                      "RegDebounceEnable");
    }

    sx1509_unlock(e);
    return err;
}

esp_err_t SX1509_set_interrupt(SX1509Class *e, uint16_t mask, uint32_t sense)
{
    if (!e) return ESP_ERR_INVALID_ARG;
    if (!e->dev) return ESP_ERR_INVALID_STATE; /* never attached, or already deinit'd */
    if (!sx1509_lock(e)) return ESP_ERR_TIMEOUT;

    /* The four sense registers are consecutive from 0x14 and run from the
     * highest pins down: 0x14 = I/O[15:12], 0x15 = I/O[11:8], 0x16 = I/O[7:4],
     * 0x17 = I/O[3:0]. Flattening them into a uint32 with pin N at bits
     * [2N+1:2N] therefore means writing that uint32 big-endian -- the same
     * "high pins at the low address" rule as the 16-bit pairs, just twice as
     * wide. Written before the mask so no pin can be unmasked while its edge
     * sensitivity is still whatever it used to be. */
    const uint8_t sense_bytes[4] = {
        (uint8_t)((sense >> 24) & 0xFF), /* 0x14 RegSenseHighB, I/O[15:12] */
        (uint8_t)((sense >> 16) & 0xFF), /* 0x15 RegSenseLowB,  I/O[11:8]  */
        (uint8_t)((sense >> 8) & 0xFF),  /* 0x16 RegSenseHighA, I/O[7:4]   */
        (uint8_t)(sense & 0xFF),         /* 0x17 RegSenseLowA,  I/O[3:0]   */
    };
    esp_err_t err = sx1509_write_raw(e, SX1509_REG_SENSE_HIGH_B, sense_bytes, sizeof(sense_bytes));
    if (err == ESP_OK) {
        uint8_t readback[4] = { 0 };
        esp_err_t read_err = sx1509_read_raw(e, SX1509_REG_SENSE_HIGH_B, readback, sizeof(readback));
        if (read_err == ESP_OK && memcmp(readback, sense_bytes, sizeof(readback)) != 0) {
            ESP_LOGW(TAG, "RegSense = %08lX did not verify", (unsigned long)sense);
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (err == ESP_OK) {
        err = sx1509_write16_verified(e, SX1509_REG_INT_MASK_B, mask, 0xFFFFu, "RegInterruptMask");
    }

    sx1509_unlock(e);
    return err;
}

esp_err_t SX1509_get_interrupt_source(SX1509Class *e, uint16_t *out_mask, bool clear)
{
    if (!e) return ESP_ERR_INVALID_ARG;
    if (!e->dev) return ESP_ERR_INVALID_STATE; /* never attached, or already deinit'd */
    if (!sx1509_lock(e)) return ESP_ERR_TIMEOUT;

    uint16_t source = 0;
    esp_err_t err = sx1509_read16_raw(e, SX1509_REG_INT_SOURCE_B, &source);
    if (err == ESP_OK && clear && source != 0) {
        /* Write-1-to-clear, and only for the bits actually read. An edge that
         * arrives between the read and this write sets its bit again (or was
         * never in `source`), so it survives rather than being erased -- which
         * is why the mask written is `source` and not 0xFFFF. ~INT stays low
         * until every bit is clear.
         *
         * Not verified by read-back: this register cannot be, since re-reading
         * it after a clear legitimately shows any edge that has happened since.
         * A transport failure here is reported; a "did it really clear" check
         * is not possible and is not faked. */
        esp_err_t clear_err = sx1509_write16_raw(e, SX1509_REG_INT_SOURCE_B, source);
        if (clear_err != ESP_OK) err = clear_err;
    }
    sx1509_unlock(e);

    if (err == ESP_OK && out_mask) *out_mask = source;
    return err;
}

/* ---------------------------------------------------------------------------
 * Port I/O
 * ------------------------------------------------------------------------ */

esp_err_t SX1509_write_port(SX1509Class *e, uint16_t value)
{
    if (!e) return ESP_ERR_INVALID_ARG;
    if (!e->dev) return ESP_ERR_INVALID_STATE; /* never attached, or already deinit'd */
    if (!sx1509_lock(e)) return ESP_ERR_TIMEOUT;
    esp_err_t err = sx1509_write_port_locked(e, value);
    sx1509_unlock(e);
    return err;
}

esp_err_t SX1509_write_masked(SX1509Class *e, uint16_t mask, uint16_t value)
{
    if (!e) return ESP_ERR_INVALID_ARG;
    if (!e->dev) return ESP_ERR_INVALID_STATE; /* never attached, or already deinit'd */
    if (!sx1509_lock(e)) return ESP_ERR_TIMEOUT;
    uint16_t next = (uint16_t)((e->data_shadow & (uint16_t)~mask) | (value & mask));
    esp_err_t err = sx1509_write_port_locked(e, next);
    sx1509_unlock(e);
    return err;
}

esp_err_t SX1509_read_port(SX1509Class *e, uint16_t *out_value)
{
    if (!e || !out_value) return ESP_ERR_INVALID_ARG;
    if (!e->dev) return ESP_ERR_INVALID_STATE;
    if (!sx1509_lock(e)) return ESP_ERR_TIMEOUT;
    esp_err_t err = sx1509_read16_raw(e, SX1509_REG_DATA_B, out_value);
    sx1509_unlock(e);
    return err;
}

esp_err_t SX1509_write_pin(SX1509Class *e, uint8_t pin, bool level)
{
    if (!e || pin >= SX1509_PIN_COUNT) return ESP_ERR_INVALID_ARG;
    uint16_t mask = (uint16_t)(1u << pin);
    return SX1509_write_masked(e, mask, level ? mask : 0);
}

esp_err_t SX1509_read_pin(SX1509Class *e, uint8_t pin, bool *out_level)
{
    if (!e || pin >= SX1509_PIN_COUNT || !out_level) return ESP_ERR_INVALID_ARG;
    uint16_t port = 0;
    esp_err_t err = SX1509_read_port(e, &port);
    if (err != ESP_OK) return err;
    *out_level = (port & (uint16_t)(1u << pin)) != 0;
    return ESP_OK;
}

esp_err_t SX1509_led_driver(SX1509Class *e, uint8_t pin, bool enable, uint8_t intensity)
{
    if (!e || pin >= SX1509_PIN_COUNT) return ESP_ERR_INVALID_ARG;
    if (!e->dev) return ESP_ERR_INVALID_STATE;
    if (!sx1509_lock(e)) return ESP_ERR_TIMEOUT;

    const uint16_t bit = (uint16_t)(1u << pin);
    esp_err_t err = ESP_OK;

    if (enable) {
        /* Both clocks first: RegLEDDriverEnable does nothing while fOSC or ClkX
         * is off, and enabling in the other order would look like a silent
         * no-op until something later happened to start the oscillator. */
        err = sx1509_ensure_oscillator_locked(e, true);

        /* The datasheet's LED-driver sequence, in its order: input buffer off,
         * pull-up off, direction = output, intensity, then enable. Open-drain
         * is left alone -- the part sinks the LED either way, and forcing it
         * here would change the electrical behaviour of a pin the board layer
         * may have configured deliberately. */
        if (err == ESP_OK) {
            uint16_t input_disable = 0;
            err = sx1509_read16_raw(e, SX1509_REG_INPUT_DISABLE_B, &input_disable);
            if (err == ESP_OK) {
                err = sx1509_write16_verified(e, SX1509_REG_INPUT_DISABLE_B,
                                              (uint16_t)(input_disable | bit), 0xFFFFu,
                                              "RegInputDisable");
            }
        }
        if (err == ESP_OK && (e->pu_shadow & bit)) {
            err = sx1509_write16_verified(e, SX1509_REG_PULLUP_B,
                                          (uint16_t)(e->pu_shadow & ~bit), 0xFFFFu, "RegPullUp");
        }
        if (err == ESP_OK && (e->dir_shadow & bit)) {
            err = sx1509_write16_verified(e, SX1509_REG_DIR_B, (uint16_t)(e->dir_shadow & ~bit),
                                          0xFFFFu, "RegDir");
        }
        if (err == ESP_OK) {
            uint8_t ion = intensity;
            err = sx1509_write_raw(e, sx1509_reg_ion[pin], &ion, 1);
            if (err == ESP_OK) {
                uint8_t readback = 0;
                if (sx1509_read_raw(e, sx1509_reg_ion[pin], &readback, 1) == ESP_OK &&
                    readback != ion) {
                    ESP_LOGW(TAG, "RegIOn%u = 0x%02X read back 0x%02X", (unsigned)pin, ion,
                             readback);
                    err = ESP_ERR_INVALID_RESPONSE;
                }
            }
        }
        if (err == ESP_OK) {
            err = sx1509_write16_verified(e, SX1509_REG_LED_ENABLE_B,
                                          (uint16_t)(e->led_shadow | bit), 0xFFFFu,
                                          "RegLEDDriverEnable");
        }
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "LED driver on IO%u, intensity %u (pin drives LOW to light it)",
                     (unsigned)pin, (unsigned)intensity);
        }
    } else {
        err = sx1509_write16_verified(e, SX1509_REG_LED_ENABLE_B,
                                      (uint16_t)(e->led_shadow & ~bit), 0xFFFFu,
                                      "RegLEDDriverEnable");
        if (err == ESP_OK) {
            uint16_t input_disable = 0;
            if (sx1509_read16_raw(e, SX1509_REG_INPUT_DISABLE_B, &input_disable) == ESP_OK) {
                err = sx1509_write16_verified(e, SX1509_REG_INPUT_DISABLE_B,
                                              (uint16_t)(input_disable & ~bit), 0xFFFFu,
                                              "RegInputDisable");
            }
        }
        /* Direction is deliberately left as an output: the caller made this pin
         * an output when it enabled the driver, and flipping it back to an
         * input here would release whatever it is driving as a side effect of
         * "stop dimming". */
    }

    sx1509_unlock(e);
    return err;
}

esp_err_t SX1509_reset(SX1509Class *e, bool hard)
{
    if (!e || !e->dev) return ESP_ERR_INVALID_ARG;
    if (hard && e->reset_gpio < 0) {
        ESP_LOGE(TAG, "hard reset requested but no ~RESET GPIO is configured");
        return ESP_ERR_INVALID_STATE;
    }
    if (!sx1509_lock(e)) return ESP_ERR_TIMEOUT;

    esp_err_t err = ESP_OK;
    if (hard) {
        /* The only reset that works on a part whose I2C state machine has
         * wedged, which is exactly when you most want a reset. Note this
         * behaves as a POR only because RegMisc bit 2 is left at 0; with it set
         * ~RESET would reset just the PWM/blink counters. This driver never
         * sets that bit. */
        gpio_set_level((gpio_num_t)e->reset_gpio, 0);
        esp_rom_delay_us(SX1509_RESET_PULSE_US);
        gpio_set_level((gpio_num_t)e->reset_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(SX1509_RESET_RECOVERY_MS));
    } else {
        /* 0x12 then 0x34, consecutively, to RegReset. Two separate transfers is
         * correct -- these are two writes to the same address, not a two-byte
         * burst, which with auto-increment on would land 0x34 in RegTest1. */
        uint8_t magic = SX1509_RESET_MAGIC_1;
        err = sx1509_write_raw(e, SX1509_REG_RESET, &magic, 1);
        if (err == ESP_OK) {
            magic = SX1509_RESET_MAGIC_2;
            err = sx1509_write_raw(e, SX1509_REG_RESET, &magic, 1);
        }
        if (err == ESP_OK) vTaskDelay(pdMS_TO_TICKS(SX1509_RESET_RECOVERY_MS));
    }

    if (err == ESP_OK) {
        sx1509_reset_shadows(e);
        /* A reset also wipes RegMisc, which means the part is back to clearing
         * ~INT on every RegData read. Put that back the way this driver's
         * interrupt contract assumes -- see sx1509_update_misc_locked. */
        esp_err_t misc_err = sx1509_update_misc_locked(e, 0, SX1509_MISC_NO_AUTOCLEAR);
        if (misc_err != ESP_OK) {
            ESP_LOGW(TAG, "RegMisc could not be restored after reset: %s",
                     esp_err_to_name(misc_err));
            err = misc_err;
        }
    }

    sx1509_unlock(e);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SX1509 %s reset; all pins are inputs again", hard ? "hard" : "software");
    }
    return err;
}

uint16_t SX1509_get_shadow(const SX1509Class *e)
{
    return e ? e->data_shadow : SX1509_DATA_POWER_ON_STATE;
}

uint16_t SX1509_get_dir_shadow(const SX1509Class *e)
{
    return e ? e->dir_shadow : SX1509_DIR_POWER_ON_STATE;
}

int SX1509_get_irq_gpio(const SX1509Class *e)
{
    return e ? e->irq_gpio : -1;
}

bool SX1509_irq_asserted(const SX1509Class *e)
{
    if (!e || e->irq_gpio < 0) return false;
    return gpio_get_level((gpio_num_t)e->irq_gpio) == 0; /* ~INT is active low */
}
