// mcp23017.c -- see mcp23017.h for the shadow-register discipline and
// debounce parameters. Transport is pico-sdk's hardware/i2c.h blocking API
// -- fine here: i2c_owner's scan period is 8 ms (mcp23017.h), nowhere near a
// hard-real-time budget the way spi_emu_a/b or wave_owner's core-1 paths are
// (PLAN.md section 4.1), so a blocking I2C transaction costing a handful of
// microseconds per byte at 400 kHz is not a concern.
#include "mcp23017.h"

#include <string.h>

// Every transaction here is at most: 1 register-address byte + 2 data bytes
// (a 16-bit register write) or a handful more for mcp23017_read_regs() on a
// caller-bounded buffer -- no fixed cap needed the way MAX31856's SPI driver
// caps MAX_XFER_LEN, since i2c_write_blocking/i2c_read_blocking take a
// caller-supplied length directly rather than a fixed on-stack buffer here.

bool mcp23017_init(mcp23017_t *dev, i2c_inst_t *i2c, uint8_t addr7)
{
    if (!dev || !i2c) {
        return false;
    }

    memset(dev, 0, sizeof(*dev));
    dev->i2c = i2c;
    dev->addr7 = addr7;
    dev->iodir_shadow = 0xFFFFu; // POR default: all 16 pins input
    dev->gppu_shadow = 0x0000u;  // POR default: no pull-ups enabled
    dev->olat_shadow = 0x0000u;  // POR default: output latch low
    dev->initialized = true;
    return true;
}

bool mcp23017_write_reg(const mcp23017_t *dev, uint8_t reg, uint8_t val)
{
    if (!dev || !dev->initialized) {
        return false;
    }
    uint8_t tx[2] = { reg, val };
    int n = i2c_write_blocking(dev->i2c, dev->addr7, tx, sizeof(tx), false);
    return n == (int)sizeof(tx);
}

bool mcp23017_read_reg(const mcp23017_t *dev, uint8_t reg, uint8_t *val)
{
    return mcp23017_read_regs(dev, reg, val, 1u);
}

bool mcp23017_read_regs(const mcp23017_t *dev, uint8_t reg, uint8_t *buf, size_t len)
{
    if (!dev || !dev->initialized || !buf || len == 0) {
        return false;
    }
    // Write the target register address (no stop), then a repeated-start
    // read of len bytes -- the part auto-increments across it.
    int wn = i2c_write_blocking(dev->i2c, dev->addr7, &reg, 1u, true);
    if (wn != 1) {
        return false;
    }
    int rn = i2c_read_blocking(dev->i2c, dev->addr7, buf, len, false);
    return rn == (int)len;
}

// Writes a 16-bit shadow (IODIR/GPPU/OLAT) back to the device as its A/B
// register pair, one 2-byte auto-incrementing transaction starting at
// reg_a (must be the *A register of the pair, e.g. MCP23017_REG_IODIRA).
static bool mcp23017_write_shadow16(const mcp23017_t *dev, uint8_t reg_a, uint16_t shadow)
{
    uint8_t tx[3] = { reg_a, (uint8_t)(shadow & 0xFFu), (uint8_t)((shadow >> 8) & 0xFFu) };
    int n = i2c_write_blocking(dev->i2c, dev->addr7, tx, sizeof(tx), false);
    return n == (int)sizeof(tx);
}

static bool pin_valid(uint8_t pin)
{
    return pin < 16u;
}

bool mcp23017_pin_set_dir(mcp23017_t *dev, uint8_t pin, bool input)
{
    if (!dev || !dev->initialized || !pin_valid(pin)) {
        return false;
    }
    uint16_t mask = (uint16_t)(1u << pin);
    uint16_t next = input ? (uint16_t)(dev->iodir_shadow | mask) : (uint16_t)(dev->iodir_shadow & ~mask);
    if (!mcp23017_write_shadow16(dev, MCP23017_REG_IODIRA, next)) {
        return false;
    }
    dev->iodir_shadow = next;
    return true;
}

bool mcp23017_pin_set_pullup(mcp23017_t *dev, uint8_t pin, bool enable)
{
    if (!dev || !dev->initialized || !pin_valid(pin)) {
        return false;
    }
    uint16_t mask = (uint16_t)(1u << pin);
    uint16_t next = enable ? (uint16_t)(dev->gppu_shadow | mask) : (uint16_t)(dev->gppu_shadow & ~mask);
    if (!mcp23017_write_shadow16(dev, MCP23017_REG_GPPUA, next)) {
        return false;
    }
    dev->gppu_shadow = next;
    return true;
}

bool mcp23017_pin_write(mcp23017_t *dev, uint8_t pin, bool level)
{
    if (!dev || !dev->initialized || !pin_valid(pin)) {
        return false;
    }
    uint16_t mask = (uint16_t)(1u << pin);
    uint16_t next = level ? (uint16_t)(dev->olat_shadow | mask) : (uint16_t)(dev->olat_shadow & ~mask);
    if (!mcp23017_write_shadow16(dev, MCP23017_REG_OLATA, next)) {
        return false;
    }
    dev->olat_shadow = next;
    return true;
}

bool mcp23017_pin_read(const mcp23017_t *dev, uint8_t pin, bool *level)
{
    if (!dev || !dev->initialized || !pin_valid(pin) || !level) {
        return false;
    }
    uint16_t word;
    if (!mcp23017_read_gpio_word(dev, &word)) {
        return false;
    }
    *level = (word & (uint16_t)(1u << pin)) != 0;
    return true;
}

bool mcp23017_read_gpio_word(const mcp23017_t *dev, uint16_t *word)
{
    if (!dev || !dev->initialized || !word) {
        return false;
    }
    uint8_t buf[2];
    if (!mcp23017_read_regs(dev, MCP23017_REG_GPIOA, buf, sizeof(buf))) {
        return false;
    }
    *word = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    return true;
}

void mcp23017_debounce_init(mcp23017_debounce_t *db)
{
    if (!db) {
        return;
    }
    memset(db, 0, sizeof(*db));
}

bool mcp23017_debounce_update(mcp23017_debounce_t *db, uint16_t raw, uint16_t mask,
                               uint16_t *changed_mask)
{
    if (changed_mask) {
        *changed_mask = 0;
    }
    if (!db) {
        return false;
    }

    if (!db->initialized) {
        db->stable = (uint16_t)(raw & mask);
        db->candidate = raw;
        db->match_count = MCP23017_DEBOUNCE_N;
        db->initialized = true;
        return false; // seeding, not an observed edge
    }

    uint16_t raw_masked = (uint16_t)(raw & mask);
    uint16_t candidate_masked = (uint16_t)(db->candidate & mask);

    if (raw_masked == candidate_masked) {
        if (db->match_count < 0xFFu) {
            db->match_count++;
        }
    } else {
        db->candidate = raw;
        db->match_count = 1u;
    }

    if (db->match_count >= MCP23017_DEBOUNCE_N) {
        uint16_t diff = (uint16_t)((db->stable ^ raw_masked) & mask);
        if (diff != 0) {
            db->stable = (uint16_t)((db->stable & (uint16_t)~mask) | raw_masked);
            if (changed_mask) {
                *changed_mask = diff;
            }
            return true;
        }
    }
    return false;
}
