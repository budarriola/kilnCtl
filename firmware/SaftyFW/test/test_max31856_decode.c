// Host tests for firmware/SaftyFW/src/max31856_decode.c -- the pure CJ/TC
// fixed-point decode math. No pico-sdk/FreeRTOS/hardware dependency, same
// discipline as test_max31856_tc_type_policy.c.
//
// Vectors are datasheet-verified (CJTH = Sign,2^6..2^0; CJTL = 2^-1..2^-6
// with the low 2 bits hardwired 0; LTCBH = Sign,2^10..2^4, sign at bit 23 of
// the 24-bit word, LTCBL low 5 bits documented don't-care) and are all
// exactly representable in float, so these use exact equality rather than an
// epsilon compare.
#include "test_common.h"

#include "max31856_decode.h"

static void test_decode_cj(void)
{
    TEST_SECTION("max31856_decode_cj -- CJTH:CJTL fixed-point decode");

    TEST_CHECK(max31856_decode_cj(0x0Cu, 0x00u) == 12.0f, "CJ 0x0C,0x00 -> +12.0 degC");
    TEST_CHECK(max31856_decode_cj(0xFFu, 0x00u) == -1.0f, "CJ 0xFF,0x00 -> -1.0 degC (negative, sign bit)");
    TEST_CHECK(max31856_decode_cj(0x00u, 0x04u) == 0.015625f,
               "CJ 0x00,0x04 -> +0.015625 degC (2^-6, the LSB)");
}

static void test_decode_tc(void)
{
    TEST_SECTION("max31856_decode_tc -- LTCBH:LTCBM:LTCBL fixed-point decode");

    TEST_CHECK(max31856_decode_tc(0x3Eu, 0x80u, 0x00u) == 1000.0f, "TC 0x3E,0x80,0x00 -> +1000.0 degC");
    TEST_CHECK(max31856_decode_tc(0xFFu, 0xF0u, 0x00u) == -1.0f,
               "TC 0xFF,0xF0,0x00 -> -1.0 degC (negative, sign bit at bit 23)");
    TEST_CHECK(max31856_decode_tc(0x00u, 0x00u, 0x20u) == 0.0078125f,
               "TC 0x00,0x00,0x20 -> +0.0078125 degC (2^-7, the LSB)");

    // The real regression guard: LTCBL's low 5 bits are documented
    // don't-care and must be masked off before the sign-extend/scale --
    // setting them must NOT change the result.
    TEST_CHECK(max31856_decode_tc(0x3Eu, 0x80u, 0x1Fu) == 1000.0f,
               "TC 0x3E,0x80,0x1F -> still exactly +1000.0 degC -- proves the 0x1F don't-care "
               "mask is actually applied, not just present in a comment");
}

void run_test_max31856_decode(void)
{
    test_decode_cj();
    test_decode_tc();
}
