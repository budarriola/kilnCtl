// max31856_codec.c -- see max31856_codec.h.
#include "max31856_codec.h"

#include <math.h>

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
        signed_raw = (int32_t)(raw | 0xFF000000u); /* sign-extend bit 23 */
    }
    return (float)signed_raw * MAX31856_TC_TEMP_C_PER_LSB;
}

int16_t max31856_encode_tc_threshold(float temperature_c)
{
    if (isnan(temperature_c)) {
        return 0;
    }
    float lsbs = roundf(temperature_c / MAX31856_TC_THRESHOLD_C_PER_LSB);
    if (lsbs > 32767.0f) {
        return INT16_MAX;
    }
    if (lsbs < -32768.0f) {
        return INT16_MIN;
    }
    return (int16_t)lsbs;
}
