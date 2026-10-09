// max31856_tc_type_policy.h -- pure CR1.TC TYPE[3:0] range check, split out of
// max31856.c the same way uart_owner_tx_policy.h is split out of uart_owner.c
// (see that header's own comment): max31856.h itself has no pico-sdk/
// FreeRTOS/hardware includes (it is register addresses, bit masks and a
// plain-data reading struct), so this predicate can #include it directly and
// still be host-tested with no stub layer (test/test_max31856_tc_type_policy.c
// links this .c file alone), unlike max31856.c itself which needs real
// hardware/gpio.h and spi_owner.h.
//
// 2026-08-24: datasheet Table 2 / the CR1 register field table (page 20)
// defines CR1.TC TYPE[3:0] as TWO different things depending on the top bits:
//   0000-0111 (0x00-0x07): a real thermocouple type (B/E/J/K/N/R/S/T) --
//     conversions are linearized against that type's NIST tables and land in
//     LTCBH/M/L as a temperature, MAX31856_TC_TEMP_C_PER_LSB scaled.
//   10xx (0x08-0x0B): Voltage Mode, Gain = 8.
//   11xx (0x0C-0x0F): Voltage Mode, Gain = 32.
//   In both voltage-mode cases the datasheet gives the LTCB code as
//   `gain * 1.6 * 2^17 * VIN` -- a SCALED INPUT VOLTAGE, not a linearized
//   temperature. max31856.c's max31856_decode_tc() unconditionally applies
//   MAX31856_TC_TEMP_C_PER_LSB (1/4096) to whatever is in LTCB, so a part
//   configured into voltage mode would still produce a plausible-looking
//   float that safety_guards.c would trust as degrees C. Worked example: a
//   Type-K junction at 1000 degC produces ~41 mV; read out in voltage mode
//   gain 8, the code is 8*1.6*131072*0.041 ~= 68800, which /4096 decodes as
//   ~17 degC -- the over-temperature guard would never trip on a genuinely
//   1000 degC kiln. This board (THERMOCOUPLE.md section 2) has tc_type as
//   its only runtime commissioning axis and no documented raw/voltage-mode
//   debug path (unlike KilnFW, which deliberately allows 0x08-0x0F behind a
//   separate raw debug subcommand and bounds its own operator-facing paths
//   to 0-7 via ZONE_TC_TYPE_MAX_REAL) -- so there is no justification here
//   for ever accepting anything past MAX31856_TC_TYPE_T.
#ifndef SAFTYFW_MAX31856_TC_TYPE_POLICY_H
#define SAFTYFW_MAX31856_TC_TYPE_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// True iff tc_type selects one of the eight real, linearized thermocouple
// types (MAX31856_TC_TYPE_B..MAX31856_TC_TYPE_T, 0x00-0x07) -- false for
// 0x08-0x0F (Voltage Mode, both gains) and for any value above 0x0F (which
// cannot occur from a real 4-bit field but is checked anyway since this
// function's caller may hand it an arbitrary byte from flash/the wire).
bool max31856_tc_type_is_valid(uint8_t tc_type);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_MAX31856_TC_TYPE_POLICY_H
