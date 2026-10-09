// max31856_decode.h -- pure CJTH:CJTL / LTCBH:LTCBM:LTCBL fixed-point decode
// math, split out of max31856.c the same way max31856_tc_type_policy.h is
// (see that header's own comment for the general pattern): no pico-sdk/
// FreeRTOS/hardware includes, so this is directly host-tested
// (test/test_max31856_decode.c links this .c file with no stub layer),
// unlike max31856.c itself which needs real hardware/gpio.h and spi_owner.h.
//
// Both functions are ported verbatim from KilnFW's MAX31856_decode_cj()/
// MAX31856_decode_tc() (firmware/KilnFW/App/drivers/hw/MAX31856.c) -- see this
// module's max31856_read() for how they are used together with the
// SR-register fault bits to decide whether the temperature they produce
// means anything.
#ifndef SAFTYFW_MAX31856_DECODE_H
#define SAFTYFW_MAX31856_DECODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// CJTH:CJTL (datasheet "Register 0Ah/0Bh: Cold-Junction Temperature") --
// sign + 2^6..2^-6, low two bits of CJTL hard-wired to 0 by the part itself
// (not masked here; the hardware never sets them, unlike LTCBL's don't-care
// bits below which genuinely can hold garbage and must be masked).
float max31856_decode_cj(uint8_t cjth, uint8_t cjtl);

// LTCBH:LTCBM:LTCBL (datasheet "Register 0Ch/0Dh/0Eh: Linearized TC
// Temperature") -- 19 significant bits at the top of a 24-bit word: sign +
// 2^10..2^-7, LTCBL[4:0] documented don't-care and explicitly masked off
// before the sign-extend.
float max31856_decode_tc(uint8_t ltcbh, uint8_t ltcbm, uint8_t ltcbl);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_MAX31856_DECODE_H
