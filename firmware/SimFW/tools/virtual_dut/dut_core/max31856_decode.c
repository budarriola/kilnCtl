// max31856_decode.c -- see max31856_decode.h for why this exists and what
// it deliberately does NOT contain (no safety policy, only the datasheet's
// fixed-point register encoding, ported byte-for-byte from
// firmware/SaftyFW/src/max31856.c's static decode helpers).
#include "max31856_decode.h"

#include <math.h>

#define REG_CJTH   0x0Au
#define REG_CJTL   0x0Bu
#define REG_LTCBH  0x0Cu
#define REG_LTCBM  0x0Du
#define REG_LTCBL  0x0Eu
#define REG_SR     0x0Fu

#define FAULT_OPEN    0x01u
#define FAULT_OVUV    0x02u
#define FAULT_CJRANGE 0x80u
#define FAULT_TCRANGE 0x40u

#define TC_INVALIDATING_FAULTS ((uint8_t)(FAULT_OPEN | FAULT_OVUV | FAULT_TCRANGE))

#define TC_TEMP_C_PER_LSB (1.0f / 4096.0f)
#define CJ_TEMP_C_PER_LSB (1.0f / 256.0f)

/* Ported verbatim from max31856.c's max31856_decode_cj(). */
static float decode_cj(uint8_t cjth, uint8_t cjtl)
{
    int16_t raw = (int16_t)(((uint16_t)cjth << 8) | cjtl);
    return (float)raw * CJ_TEMP_C_PER_LSB;
}

/* Ported verbatim from max31856.c's max31856_decode_tc(). */
static float decode_tc(uint8_t ltcbh, uint8_t ltcbm, uint8_t ltcbl)
{
    uint32_t raw = ((uint32_t)ltcbh << 16) | ((uint32_t)ltcbm << 8) | (uint32_t)ltcbl;
    raw &= 0x00FFFFE0u;

    int32_t signed_raw = (int32_t)raw;
    if (raw & 0x00800000u) {
        signed_raw = (int32_t)(raw | 0xFF000000u);
    }
    return (float)signed_raw * TC_TEMP_C_PER_LSB;
}

void max31856_decode_regs(const uint8_t regs[16], max31856_decoded_t *out)
{
    out->cj_c = decode_cj(regs[REG_CJTH], regs[REG_CJTL]);
    out->tc_c = decode_tc(regs[REG_LTCBH], regs[REG_LTCBM], regs[REG_LTCBL]);
    out->fault_bits = regs[REG_SR];

    /* Ported verbatim from max31856_read()'s post-transfer invalidation. */
    if (out->fault_bits & TC_INVALIDATING_FAULTS) {
        out->tc_c = NAN;
    }
    if (out->fault_bits & FAULT_CJRANGE) {
        out->cj_c = NAN;
    }
}
