// mcp23017.h -- register-level driver for the Microchip MCP23017 16-bit I2C
// I/O expander, used twice on this board (docs/DESIGN_NOTES.md section 3.7): #1 at
// 0x20 (relay sense, E-stop drive, DUT-power relay, J20 IO_3/IO_4, spares),
// #2 at 0x21 (spare I/O, generic). This file is a plain register/pin
// accessor with no RTOS dependency and no opinion about which pins mean
// what -- that mapping lives in the owning task (src/tasks/i2c_owner.c),
// per the single-owner doctrine documented there and in DESIGN_NOTES.md section 4's
// opening paragraph. Mirrors how ../../SaftyFW/src/max31856.c is a plain
// transport-level driver called by its owner task (thermo_task), not a task
// itself.
//
// IOCON.BANK is never touched here and stays at its POR default (0), which
// makes every A/B register pair sequential (IODIRA=0x00, IODIRB=0x01, ...)
// -- the datasheet's default addressing table, reproduced below.
//
// Direction/pull-up/output-latch state is cached in the mcp23017_t handle
// (IODIR/GPPU/OLAT shadows) rather than read back from the part before every
// modification: mcp23017_init() assumes the device is at its POR defaults
// (IODIR=0xFFFF all-input, GPPU=0x0000, OLAT=0x0000 -- true for a part that
// has just been powered and never touched) and every subsequent
// mcp23017_pin_set_dir()/mcp23017_pin_set_pullup()/mcp23017_pin_write() call
// updates the shadow, then writes the whole affected register back in one
// I2C transaction. This only stays correct if this driver is the only thing
// that ever writes to a given device's IODIR/GPPU/OLAT registers -- which
// holds precisely because i2c_owner is I2C0's sole owner (no other task
// calls into this driver, or touches I2C0 at all).
#ifndef SIMFW_DRIVERS_MCP23017_H
#define SIMFW_DRIVERS_MCP23017_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hardware/i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- Register addresses (datasheet Table 3-2/3-3, IOCON.BANK = 0) --------
#define MCP23017_REG_IODIRA   0x00u
#define MCP23017_REG_IODIRB   0x01u
#define MCP23017_REG_IPOLA    0x02u
#define MCP23017_REG_IPOLB    0x03u
#define MCP23017_REG_GPINTENA 0x04u
#define MCP23017_REG_GPINTENB 0x05u
#define MCP23017_REG_DEFVALA  0x06u
#define MCP23017_REG_DEFVALB  0x07u
#define MCP23017_REG_INTCONA  0x08u
#define MCP23017_REG_INTCONB  0x09u
#define MCP23017_REG_IOCON    0x0Au // 0x0B mirrors it; only 0x0A used here
#define MCP23017_REG_GPPUA    0x0Cu
#define MCP23017_REG_GPPUB    0x0Du
#define MCP23017_REG_INTFA    0x0Eu
#define MCP23017_REG_INTFB    0x0Fu
#define MCP23017_REG_INTCAPA  0x10u
#define MCP23017_REG_INTCAPB  0x11u
#define MCP23017_REG_GPIOA    0x12u
#define MCP23017_REG_GPIOB    0x13u
#define MCP23017_REG_OLATA    0x14u
#define MCP23017_REG_OLATB    0x15u

// One device instance: which I2C peripheral, which 7-bit address, and the
// direction/pull-up/output-latch shadows this driver keeps authoritative
// (see file header). Zero-initialize before mcp23017_init(); the caller
// owns storage (no heap allocation in this driver).
typedef struct {
    i2c_inst_t *i2c;
    uint8_t     addr7;
    uint16_t    iodir_shadow; // 1 = input (POR default 0xFFFF), bit n = pin n
    uint16_t    gppu_shadow;  // 1 = pull-up enabled (POR default 0x0000)
    uint16_t    olat_shadow;  // output latch for pins currently set as outputs
    bool        initialized;
} mcp23017_t;

// Binds dev to (i2c, addr7) and seeds the shadows to the part's POR defaults
// (IODIR=0xFFFF, GPPU=0x0000, OLAT=0x0000) without touching the bus -- valid
// only for a part that has just powered up and been touched by nobody else,
// which is guaranteed here by I2C0's single-owner doctrine. i2c_init() and
// the SDA/SCL GPIO function/pull-up setup are the caller's job (i2c_owner
// owns I2C0 itself, not any one device on it). Returns false only on a NULL
// argument.
bool mcp23017_init(mcp23017_t *dev, i2c_inst_t *i2c, uint8_t addr7);

// Raw register access, for anything the pin-level API below doesn't cover
// (e.g. reading INTCAP, or driver-internal use). len-byte read/write
// starting at reg; the part auto-increments the register pointer across A/B
// pairs, same as MAX31856's auto-increment (max31856.c's read_burst
// comment) -- convenient for e.g. reading GPIOA+GPIOB in one transaction.
bool mcp23017_read_reg(const mcp23017_t *dev, uint8_t reg, uint8_t *val);
bool mcp23017_write_reg(const mcp23017_t *dev, uint8_t reg, uint8_t val);
bool mcp23017_read_regs(const mcp23017_t *dev, uint8_t reg, uint8_t *buf, size_t len);

// --- Pin-level API ---------------------------------------------------------
// pin is 0..15: 0..7 = port A (GPA0..GPA7), 8..15 = port B (GPB0..GPB7).
// Each call does one register read-modify-of-shadow plus one I2C register
// write (IODIR or GPPU or OLAT, 8 bits) -- fine for configuration-time and
// output-command use, not intended for a tight polling loop (use
// mcp23017_read_gpio_word() for that; see below).
bool mcp23017_pin_set_dir(mcp23017_t *dev, uint8_t pin, bool input);
bool mcp23017_pin_set_pullup(mcp23017_t *dev, uint8_t pin, bool enable);
bool mcp23017_pin_write(mcp23017_t *dev, uint8_t pin, bool level);
// Reads the live GPIO register (not the shadow) for one pin -- valid for
// both input and output pins (GPIO reflects the output latch when a pin is
// configured as an output, per datasheet).
bool mcp23017_pin_read(const mcp23017_t *dev, uint8_t pin, bool *level);

// One-shot read of both GPIOA/GPIOB as a 16-bit word (bit n = pin n),
// exactly the "instantaneous raw reading" a debounce scan needs -- one I2C
// transaction (auto-increment across the A/B pair) instead of 16 individual
// mcp23017_pin_read() calls.
bool mcp23017_read_gpio_word(const mcp23017_t *dev, uint16_t *word);

// --- Debounce helper (docs/DESIGN_NOTES.md section 4.1: "relay-sense debounced
// scan (5-10 ms)") --------------------------------------------------------
// Simple N-consecutive-identical-reading debounce, applied to a 16-bit raw
// GPIO word and a mask selecting which bits are actually debounced (other
// bits are ignored -- callers with unrelated pins on the same device, e.g.
// exp1's relay-sense pins vs. its output pins, pass a mask covering only the
// input pins they care about).
//
// Chosen parameters: N = 3 consecutive matching samples, scanned every 8 ms
// (mid-point of PLAN.md's stated 5-10 ms band) -- so a transition is
// confirmed within roughly 2 scan periods after the raw signal has settled
// (worst case ~3 x scan period = 24 ms end-to-end, comfortably faster than
// any relay's own mechanical bounce settling time this fixture needs to
// filter, and far faster than the guard reaction times it feeds). This is a
// bench-tool debounce for a fixture with no hardware in this environment to
// tune against -- N and the scan period are a reasonable starting point
// documented here, not a bench-verified constant; revisit once real relay
// contacts are on the bench (docs/PLAN.md section 14, bring-up step 8).
#define MCP23017_DEBOUNCE_N       3u
#define MCP23017_DEBOUNCE_SCAN_MS 8u

typedef struct {
    uint16_t stable;      // last debounced (confirmed) value, masked bits only meaningful
    uint16_t candidate;   // most recent raw reading being tracked for confirmation
    uint8_t  match_count; // consecutive samples matching `candidate`
    bool     initialized; // false until the first update seeds stable/candidate
} mcp23017_debounce_t;

void mcp23017_debounce_init(mcp23017_debounce_t *db);

// Feeds one new raw sample. Returns true if `stable` changed on this call
// (i.e. a debounced edge was just confirmed), and if changed_mask is
// non-NULL, sets *changed_mask to the bits (within `mask`) that flipped.
// The very first call after mcp23017_debounce_init() always seeds `stable`
// from `raw` and returns false (no edge -- there is no prior state to have
// transitioned from).
bool mcp23017_debounce_update(mcp23017_debounce_t *db, uint16_t raw, uint16_t mask,
                               uint16_t *changed_mask);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_DRIVERS_MCP23017_H
