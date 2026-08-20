// max31856_decode.h -- host-side register decode for virtual_dut.
//
// WHY THIS FILE EXISTS (read before touching it): firmware/SaftyFW/src/
// max31856.c is the real safety-thermocouple driver, but its two decode
// functions (max31856_decode_cj/max31856_decode_tc) are `static` and the
// file that owns them is NOT host-compilable -- max31856_read() calls
// spi_owner_transfer() (real SPI hardware, RP2040-only) and max31856_init()
// calls hardware/gpio.h. There is no real SPI bus in this harness (no DUT
// ever drives CS/SCLK against virtual_simfw's emulated MAX31856 register
// file -- see virtual_simfw/README.md's "no real SPI bytes ever flow"), so
// the only way to get from the 16 raw register bytes virtual_simfw's
// TC_GET_REGS reply carries to the tc_c/cj_c/fault_bits a real driver would
// have decoded is to do that decode ourselves.
//
// This is NOT a reimplementation of any SAFETY decision (threshold, guard,
// timing rule) -- SAFETY_GUARDS.C ITSELF IS COMPILED VERBATIM AND UNMODIFIED
// (see dut_core's build script and main.c). This file only reproduces the
// MAX31856's own fixed-point register encoding (a datasheet fact, not a
// safety policy), so that safety_guards_tick() can be fed a real tc_c/cj_c/
// fault_bits instead of something invented. The four fixed-point
// conversions and the OPEN/OVUV/TCRANGE/CJRANGE invalidation rule below are
// copied byte-for-byte from firmware/SaftyFW/src/max31856.c's
// max31856_decode_cj()/max31856_decode_tc()/max31856_read()'s own
// TC_INVALIDATING_FAULTS handling (which is itself a verbatim port of
// firmware/KilnFW/App/drivers/MAX31856.c, per that file's own header
// comment) -- register addresses/scales are also cross-checked against
// firmware/SimFW/src/sim/max31856_regs.h, which documents that it "must not
// need a single driver change" to match both real drivers byte-for-byte.
// If SaftyFW's max31856.c ever becomes host-compilable (e.g. spi_owner.h
// grows a host shim), replace this file's math with a real call into that
// driver instead of maintaining a second copy.
#ifndef VIRTUAL_DUT_MAX31856_DECODE_H
#define VIRTUAL_DUT_MAX31856_DECODE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float   tc_c;        /* NaN if invalidated by an SR fault bit */
    float   cj_c;        /* NaN if invalidated by CJRANGE */
    uint8_t fault_bits;   /* raw SR register byte */
} max31856_decoded_t;

/* regs[] is the 16-byte MAX31856 register image exactly as TC_GET_REGS
 * returns it (firmware/SimFW/docs/PROTOCOL.md sec 5.2), addresses 0x00..0x0F.
 * Decodes CJTH:CJTL (0x0A:0x0B), LTCBH:LTCBM:LTCBL (0x0C:0x0D:0x0E) and SR
 * (0x0F), applying the same TC_INVALIDATING_FAULTS (OPEN|OVUV|TCRANGE) and
 * CJRANGE rules max31856.c's max31856_read() applies. */
void max31856_decode_regs(const uint8_t regs[16], max31856_decoded_t *out);

#endif // VIRTUAL_DUT_MAX31856_DECODE_H
