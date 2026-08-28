// Host tests for MAX31856's pure fixed-point codec (../drivers/
// max31856_codec.c). Bit weights checked against the MAX31856 datasheet
// (datasheets/ThermocoupleBoard_Sensor_Temperature/MAX31856.pdf), "Internal
// Registers" (Table 6) and the CJTH/CJTL, LTCBH/M/L and LTHFTH/L bit-weight
// tables (pages 20, 23, 24).
//
// Every vector below is exactly representable in IEEE-754 float (the divisor
// in every scale constant is a power of two), so TEST_CHECK_NEAR is used with
// a tight epsilon purely as float-equality hygiene -- not because any of
// these values are genuinely inexact. The one exception is the round-trip
// section at the bottom, which is deliberately checked against a half-LSB
// tolerance because it composes encode+decode of temperatures that do not
// themselves land on an exact LSB multiple.
#include "test_common.h"
#include "../drivers/max31856_codec.h"

#include <stdint.h>

/* Vectors that are exact in float get zero tolerance; this exists only so a
 * stray typo doesn't get read as "well it's probably fine". */
#define EXACT 0.0

void run_test_max31856_codec(void)
{
    TEST_SECTION("max31856_codec");

    /* --- CJ decode: CJTH:CJTL, sign + 2^6..2^-6, 1/256 degC per raw LSB --- */
    {
        TEST_CHECK_NEAR(max31856_decode_cj(0x0C, 0x00), 12.0, EXACT,
                         "CJ decode: 0x0C,0x00 -> +12.0");
        TEST_CHECK_NEAR(max31856_decode_cj(0xFF, 0x00), -1.0, EXACT,
                         "CJ decode: 0xFF,0x00 -> -1.0");
        TEST_CHECK_NEAR(max31856_decode_cj(0x00, 0x04), 0.015625, EXACT,
                         "CJ decode: 0x00,0x04 -> +0.015625 (the 2^-6 LSB)");
        TEST_CHECK_NEAR(max31856_decode_cj(0x00, 0x00), 0.0, EXACT,
                         "CJ decode: 0x00,0x00 -> 0.0");

        /* Hardware clamp endpoints. The datasheet's stated clamp is +128 degC
         * max / -64 degC min on the cold-junction sensor's own output, but
         * the CJTH:CJTL register pair is a 16-bit two's-complement value at
         * 1/256 degC per LSB, whose actual representable range is
         * -128.0 .. +127.99609375 -- +128.0 itself overflows the format (the
         * byte pattern that would encode it, 0x80,0x00, decodes to -128.0
         * instead, since bit 15 is the sign bit). So the datasheet's +128
         * figure is the sensor's physical ceiling, never actually written
         * into these registers; the nearest reachable code to it is
         * 0x7F,0xFF (+127.99609375). The -64.0 endpoint, by contrast, is
         * exactly reachable at 0xC0,0x00 and is checked below. This is the
         * one place this test suite disagrees with a vector as stated in the
         * task and substitutes the value the format can actually hold. */
        TEST_CHECK_NEAR(max31856_decode_cj(0x7F, 0xFF), 127.99609375, EXACT,
                         "CJ decode: 0x7F,0xFF -> +127.99609375 (closest reachable code to the "
                         "sensor's +128 physical ceiling; +128.0 itself is not representable)");
        TEST_CHECK_NEAR(max31856_decode_cj(0xC0, 0x00), -64.0, EXACT,
                         "CJ decode: 0xC0,0x00 -> -64.0 (hardware clamp low endpoint)");

        /* The two low CJTL bits are documented hard-wired to 0 on the part,
         * but the decode function has no way to know that -- it just treats
         * the full 16 bits as a plain int16/256. If a caller (or a future
         * fault) ever handed it a CJTL with those bits set, they would be
         * decoded as plain fractional weights: CJTL bit1 = 1/128 degC, bit0
         * = 1/256 degC (each CJTL bit b contributes 2^b/256 to the combined
         * value, same as every other bit in the pair), exactly as if they
         * were legitimate data. That is acceptable: the part itself never
         * sets them, so in normal operation they are always 0 and contribute
         * nothing; this just documents that the codec does not mask them off
         * (unlike the TC decode below, which DOES mask its five don't-care
         * bits, because those are documented as *undefined*, not *hard-wired
         * zero*). */
        TEST_CHECK_NEAR(max31856_decode_cj(0x00, 0x03), 1.0 / 128.0 + 1.0 / 256.0, EXACT,
                         "CJ decode: 0x00,0x03 -> the two hard-wired-zero bits, if ever set, "
                         "decode as plain 1/128 + 1/256 degC weights (not masked off)");
    }

    /* --- TC decode: LTCBH:LTCBM:LTCBL, sign + 2^10..2^-7, LTCBL[4:0] --- */
    /* don't-care, 1/4096 degC per raw LSB of the masked 24-bit word.       */
    {
        TEST_CHECK_NEAR(max31856_decode_tc(0x3E, 0x80, 0x00), 1000.0, EXACT,
                         "TC decode: 0x3E,0x80,0x00 -> +1000.0");
        TEST_CHECK_NEAR(max31856_decode_tc(0xFF, 0xF0, 0x00), -1.0, EXACT,
                         "TC decode: 0xFF,0xF0,0x00 -> -1.0");
        TEST_CHECK_NEAR(max31856_decode_tc(0x00, 0x00, 0x20), 0.0078125, EXACT,
                         "TC decode: 0x00,0x00,0x20 -> +0.0078125 (the 2^-7 LSB)");
        TEST_CHECK_NEAR(max31856_decode_tc(0x00, 0x00, 0x00), 0.0, EXACT,
                         "TC decode: 0x00,0x00,0x00 -> 0.0");

        /* Full-scale endpoints of the masked 24-bit word (19 significant
         * bits, don't-care low 5 bits masked to 0). Positive full scale is
         * raw 0x7FFFE0 = 8388576 / 4096 = 2047.9921875 (8388608 - 32, i.e.
         * one LSB step short of 2048); negative full scale is raw 0x800000
         * sign-extended to -8388608 / 4096 = -2048.0 exactly. */
        TEST_CHECK_NEAR(max31856_decode_tc(0x7F, 0xFF, 0xE0), 2047.9921875, EXACT,
                         "TC decode: 0x7F,0xFF,0xE0 -> +2047.9921875 (positive full scale)");
        TEST_CHECK_NEAR(max31856_decode_tc(0x80, 0x00, 0x00), -2048.0, EXACT,
                         "TC decode: 0x80,0x00,0x00 -> -2048.0 (negative full scale)");

        /* THE regression guard: LTCBL's low 5 bits are documented don't-care
         * (not guaranteed zero, unlike CJTL's two hard-wired bits above), so
         * the decode function masks them off with 0x00FFFFE0 before doing
         * anything else. 0x1F set in the LSB must vanish and this must still
         * decode to exactly the same +1000.0 as the all-zero-LSB vector
         * above. */
        TEST_CHECK_NEAR(max31856_decode_tc(0x3E, 0x80, 0x1F), 1000.0, EXACT,
                         "TC decode: 0x3E,0x80,0x1F -> +1000.0 (don't-care mask regression guard)");
    }

    /* --- Threshold encode: degC -> int16 at 1/16 degC per LSB --- */
    {
        TEST_CHECK(max31856_encode_tc_threshold(1000.0f) == 16000,
                    "threshold encode: +1000.0 -> 16000 (0x3E80)");
        TEST_CHECK(max31856_encode_tc_threshold(-1.0f) == -16,
                    "threshold encode: -1.0 -> -16");
        TEST_CHECK(max31856_encode_tc_threshold(0.0f) == 0,
                    "threshold encode: 0.0 -> 0");
        TEST_CHECK(max31856_encode_tc_threshold(NAN) == 0,
                    "threshold encode: NaN -> 0 (explicit isnan guard, not an accidental 0)");

        /* Round-to-nearest at a half-LSB boundary (0.5 * 0.0625 = 0.03125
         * degC). roundf() rounds halfway cases away from zero, so +0.03125
         * -> +1 LSB and -0.03125 -> -1 LSB, not toward-zero truncation. */
        TEST_CHECK(max31856_encode_tc_threshold(0.03125f) == 1,
                    "threshold encode: +0.03125 (exact half-LSB) rounds away from zero -> 1");
        TEST_CHECK(max31856_encode_tc_threshold(-0.03125f) == -1,
                    "threshold encode: -0.03125 (exact half-LSB) rounds away from zero -> -1");

        /* Clamp ends: +2047.9375 degC is exactly 32767 LSBs, still in range
         * (not clamped); anything beyond it clamps to INT16_MAX. -2048.0 is
         * exactly -32768 LSBs, still in range; anything below it clamps to
         * INT16_MIN. Clamping, not wraparound, is what must happen. */
        TEST_CHECK(max31856_encode_tc_threshold(2047.9375f) == 32767,
                    "threshold encode: +2047.9375 (exactly INT16_MAX LSBs) -> 32767, not clamped");
        TEST_CHECK(max31856_encode_tc_threshold(-2048.0f) == -32768,
                    "threshold encode: -2048.0 (exactly INT16_MIN LSBs) -> -32768, not clamped");
        TEST_CHECK(max31856_encode_tc_threshold(3000.0f) == INT16_MAX,
                    "threshold encode: +3000.0 (beyond +2047.9375) clamps to INT16_MAX, not wraps");
        TEST_CHECK(max31856_encode_tc_threshold(-3000.0f) == INT16_MIN,
                    "threshold encode: -3000.0 (beyond -2048.0) clamps to INT16_MIN, not wraps");
    }

    /* --- Round-trip: encode then decode-equivalent (raw * 1/16), for a --- */
    /* spread of realistic kiln temperatures, within half an LSB (0.03125). */
    {
        const double kiln_temps_c[] = {0.0, 100.0, 500.0, 1000.0, 1170.0, 1300.0};
        const double half_lsb = 0.03125;
        for (size_t i = 0; i < sizeof(kiln_temps_c) / sizeof(kiln_temps_c[0]); ++i) {
            double t = kiln_temps_c[i];
            int16_t raw = max31856_encode_tc_threshold((float)t);
            double back = (double)raw * (double)MAX31856_TC_THRESHOLD_C_PER_LSB;
            char msg[96];
            snprintf(msg, sizeof(msg), "round-trip: %.1f degC survives encode+decode within half an LSB", t);
            TEST_CHECK_NEAR(back, t, half_lsb, msg);
        }
    }

    /* --- Fault invalidation: which SR bits NaN which temperature --- */
    /* 2026-08-27: CJRANGE must invalidate BOTH temperatures, not just its  */
    /* own -- LTCB is cold-junction-compensated in hardware, so an          */
    /* out-of-range cold junction taints the hot-junction number too. See   */
    /* max31856_codec.h's max31856_fault_invalidates_tc() doc comment.      */
    {
        TEST_CHECK(!max31856_fault_invalidates_tc(0x00u),
                    "no fault bits -> TC not invalidated");
        TEST_CHECK(!max31856_fault_invalidates_cj(0x00u),
                    "no fault bits -> CJ not invalidated");

        TEST_CHECK(max31856_fault_invalidates_tc(MAX31856_CODEC_FAULT_OPEN),
                    "OPEN alone -> TC invalidated");
        TEST_CHECK(!max31856_fault_invalidates_cj(MAX31856_CODEC_FAULT_OPEN),
                    "OPEN alone -> CJ NOT invalidated (cold junction is unaffected)");

        TEST_CHECK(max31856_fault_invalidates_tc(MAX31856_CODEC_FAULT_OVUV),
                    "OVUV alone -> TC invalidated");
        TEST_CHECK(max31856_fault_invalidates_tc(MAX31856_CODEC_FAULT_TCRANGE),
                    "TCRANGE alone -> TC invalidated");

        /* THE regression guard for this fix: CJRANGE set BY ITSELF (no OPEN/
         * OVUV/TCRANGE) must still invalidate tc_temperature_c, because the
         * part's own hot-junction linearization is CJ-compensated. Before
         * this fix, only cj_temperature_c was NaN'd here and tc_temperature_c
         * was reported as a plausible-looking but wrong number. */
        TEST_CHECK(max31856_fault_invalidates_tc(MAX31856_CODEC_FAULT_CJRANGE),
                    "CJRANGE alone -> TC invalidated too (hot junction is CJ-compensated in hardware)");
        TEST_CHECK(max31856_fault_invalidates_cj(MAX31856_CODEC_FAULT_CJRANGE),
                    "CJRANGE alone -> CJ invalidated (the fault is directly about the CJ reading)");

        /* A threshold fault (TCLOW/TCHIGH/CJLOW/CJHIGH) invalidates neither
         * temperature -- those are operator-configured limits on an
         * otherwise-good reading, not a "this number is meaningless" signal. */
        TEST_CHECK(!max31856_fault_invalidates_tc(0x08u /* TCHIGH */),
                    "TCHIGH threshold fault alone -> TC not invalidated");
        TEST_CHECK(!max31856_fault_invalidates_cj(0x20u /* CJHIGH */),
                    "CJHIGH threshold fault alone -> CJ not invalidated");

        /* Combined faults still work as a bitwise OR of the individual
         * decisions -- OPEN plus CJRANGE together invalidate both. */
        TEST_CHECK(max31856_fault_invalidates_tc((uint8_t)(MAX31856_CODEC_FAULT_OPEN | MAX31856_CODEC_FAULT_CJRANGE)),
                    "OPEN | CJRANGE -> TC invalidated");
        TEST_CHECK(max31856_fault_invalidates_cj((uint8_t)(MAX31856_CODEC_FAULT_OPEN | MAX31856_CODEC_FAULT_CJRANGE)),
                    "OPEN | CJRANGE -> CJ invalidated");
    }
}
