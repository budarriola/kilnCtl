// max31856_codec.h -- pure fixed-point encode/decode for the MAX31856's
// cold-junction, linearized-thermocouple and threshold registers, split out
// of MAX31856.c the same way clear_trip_diag_codec.h is split out of
// clear_trip_diag.c on SaftyFW: free of ESP-IDF/FreeRTOS includes so it is
// directly host-testable (test/test_max31856_codec.c links this .c file with
// no stub layer needed), unlike MAX31856.c itself, which needs the real SPI
// bus/GPIO/mutex machinery.
//
// MAX31856.h includes this header (rather than duplicating the scale
// constants) so both files always agree on the LSB weights; MAX31856.c calls
// straight through to these functions where it used to have its own static
// copies.
//
// Datasheet references throughout name registers/bits as the datasheet does
// -- see datasheets/ThermocoupleBoard_Sensor_Temperature/MAX31856.pdf,
// "Internal Registers" (Table 6) onward, and the per-register bit-weight
// tables on the pages MAX31856.c's own header comment cites.
#ifndef MAX31856_CODEC_H
#define MAX31856_CODEC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Fixed-point scales (datasheet register bit-weight tables) --- */
/* LTHFTH/LTHFTL and LTLFTH/LTLFTL: sign + 2^10..2^-4 => 1/16 degC per LSB. */
#define MAX31856_TC_THRESHOLD_C_PER_LSB 0.0625f
/* CJTH/CJTL: sign + 2^6..2^-6, low two bits hard 0 => 1/256 degC per raw LSB
 * of the combined 16-bit word. */
#define MAX31856_CJ_TEMP_C_PER_LSB      (1.0f / 256.0f)
/* LTCBH/M/L: sign + 2^10..2^-7 in the top 19 bits of a 24-bit word, low 5 bits
 * unused => 1/4096 degC per raw LSB of the combined 24-bit word (equivalently
 * 0.0078125 degC per 19-bit code). */
#define MAX31856_TC_TEMP_C_PER_LSB      (1.0f / 4096.0f)

/* CJTH:CJTL -- sign + 2^6..2^-6 with the low two bits of CJTL hard-wired to 0.
 * Treating the pair as a plain int16 and dividing by 256 lands every bit on
 * its documented weight, and the sign bit is already in the right place. */
float max31856_decode_cj(uint8_t cjth, uint8_t cjtl);

/* LTCBH:LTCBM:LTCBL -- 19 significant bits at the top of a 24-bit word: sign +
 * 2^10..2^-7, with LTCBL[4:0] documented as don't-care. Mask those five bits
 * off (they are not guaranteed zero), sign-extend the 24-bit value into an
 * int32, then divide by 4096 so bit 12 is the 1 degC place. That is the same
 * number as (code19 * 0.0078125) but without an implementation-defined
 * arithmetic shift of a negative value. */
float max31856_decode_tc(uint8_t ltcbh, uint8_t ltcbm, uint8_t ltcbl);

/* degC -> the LTHFTH/LTHFTL (and LTLFTH/LTLFTL) pair: sign + 2^10..2^-4, i.e.
 * a two's-complement int16 at 1/16 degC per LSB. Rounds to nearest and clamps;
 * the clamped ends (+2047.9375 / -2048 degC) are already well outside every
 * thermocouple type's range, so clamping can only ever disable a threshold,
 * never move one somewhere surprising. */
int16_t max31856_encode_tc_threshold(float temperature_c);

#ifdef __cplusplus
}
#endif

#endif // MAX31856_CODEC_H
