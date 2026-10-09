// max31856_decode.c -- see max31856_decode.h.
#include "max31856_decode.h"

#include "max31856.h" // MAX31856_CJ_TEMP_C_PER_LSB / MAX31856_TC_TEMP_C_PER_LSB -- pure header

float max31856_decode_cj(uint8_t cjth, uint8_t cjtl)
{
    int16_t raw = (int16_t)(((uint16_t)cjth << 8) | cjtl);
    return (float)raw * MAX31856_CJ_TEMP_C_PER_LSB;
}

float max31856_decode_tc(uint8_t ltcbh, uint8_t ltcbm, uint8_t ltcbl)
{
    uint32_t raw = ((uint32_t)ltcbh << 16) | ((uint32_t)ltcbm << 8) | (uint32_t)ltcbl;
    raw &= 0x00FFFFE0u;

    int32_t signed_raw = (int32_t)raw;
    if (raw & 0x00800000u) {
        signed_raw = (int32_t)(raw | 0xFF000000u); // sign-extend bit 23
    }
    return (float)signed_raw * MAX31856_TC_TEMP_C_PER_LSB;
}
