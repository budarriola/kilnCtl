/* Host-native test for kilnlink_power.{c,h} -- the Pico->ESP
 * SAFETY_CMD_POWER (0x0E, Frame E) codec, docs/LINK_PROTOCOL.md sec 6.
 * Mirrors test_status.c's structure: round-trip encode/decode, byte-exact
 * vectors from test/vectors/power_vectors.json, and the hostile input set:
 * too-short, too-long (this is a fixed-size frame, not a minimum size),
 * wrong command byte.
 *
 * Build (MSVC host compiler, no CMake needed for this one-shot check):
 *   cl /nologo /W4 /I ..\include test_power.c ..\src\kilnlink_power.c
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_power.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);          \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

static void print_hex(const char *label, const uint8_t *b, size_t n)
{
    printf("%s: ", label);
    for (size_t i = 0; i < n; ++i) printf("%02x", b[i]);
    printf("\n");
}

static int feq(float a, float b)
{
    if (isnan(a) && isnan(b)) {
        return 1;
    }
    return a == b;
}

static int deq(double a, double b)
{
    if (isnan(a) && isnan(b)) {
        return 1;
    }
    return a == b;
}

/* -- round trip --------------------------------------------------------- */

static void test_round_trip_healthy(void)
{
    kilnlink_power_t pw = {0};
    pw.power_window_s = 120;
    pw.flags = KILNLINK_POWER_FLAG_MAINS_VOLTAGE_CONFIGURED | KILNLINK_POWER_FLAG_CALIBRATED;
    pw.mains_voltage_v = 240.0f;
    pw.i_conducting_a[0] = 8.5f;
    pw.i_conducting_a[1] = 7.9f;
    pw.i_conducting_a[2] = 0.0f;
    pw.conduction_fraction[0] = 0.42f;
    pw.conduction_fraction[1] = 0.38f;
    pw.conduction_fraction[2] = 0.0f;
    pw.p_avg_w[0] = 1008.0f;
    pw.p_avg_w[1] = 856.2f;
    pw.p_avg_w[2] = 0.0f;
    pw.p_total_w = 1864.2f;
    pw.energy_wh = 12345.678;
    pw.counts_avg[0] = 10;
    pw.counts_avg[1] = 2000;
    pw.counts_avg[2] = 4095;

    uint8_t buf[KILNLINK_POWER_LEN];
    kilnlink_power_status_t status;
    size_t n = kilnlink_power_encode(&pw, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_POWER_OK, "encode() reports OK");
    CHECK(n == KILNLINK_POWER_LEN_V2, "encode() always writes exactly 61 (V2) bytes");

    kilnlink_power_t decoded;
    kilnlink_power_status_t dstatus = kilnlink_power_decode(buf, n, &decoded);
    CHECK(dstatus == KILNLINK_POWER_OK, "decode() reports OK for a just-encoded payload");
    CHECK(decoded.power_window_s == pw.power_window_s, "decoded power_window_s matches");
    CHECK((decoded.flags & (uint8_t)KILNLINK_POWER_FLAG_COUNTS_VALID) != 0,
          "encoder always sets COUNTS_VALID");
    CHECK(feq(decoded.mains_voltage_v, pw.mains_voltage_v), "decoded mains_voltage_v matches");
    for (unsigned ch = 0; ch < KILNLINK_POWER_CHANNELS; ch++) {
        CHECK(feq(decoded.i_conducting_a[ch], pw.i_conducting_a[ch]), "decoded i_conducting_a matches");
        CHECK(feq(decoded.conduction_fraction[ch], pw.conduction_fraction[ch]),
              "decoded conduction_fraction matches");
        CHECK(feq(decoded.p_avg_w[ch], pw.p_avg_w[ch]), "decoded p_avg_w matches");
        CHECK(decoded.counts_avg[ch] == pw.counts_avg[ch], "decoded counts_avg matches");
    }
    CHECK(feq(decoded.p_total_w, pw.p_total_w), "decoded p_total_w matches");
    CHECK(deq(decoded.energy_wh, pw.energy_wh), "decoded energy_wh matches");
}

/* -- counts_avg-specific coverage (2026-09-06, CURRENT_SENSE.md sec 4's
 * "Tooling gap" option 2) ------------------------------------------------ */

static void test_legacy_v1_length_decodes_with_zeroed_counts(void)
{
    /* A pre-KILNLINK_PROTOCOL_VERSION-11 Pico still sends the 55-byte V1
     * layout -- must decode cleanly, with counts_avg zeroed and
     * COUNTS_VALID clear, not garbage or a length-mismatch error. */
    kilnlink_power_t pw = {0};
    pw.power_window_s = 120;
    pw.flags = KILNLINK_POWER_FLAG_CALIBRATED;
    pw.mains_voltage_v = 240.0f;
    pw.p_total_w = 100.0f;
    pw.energy_wh = 1.0;

    /* Hand-build a V1 (55-byte) frame the way the OLD encoder used to --
     * cannot use kilnlink_power_encode() itself, since this build's encoder
     * always writes V2 now (that is the whole point of this test). */
    uint8_t v1[KILNLINK_POWER_LEN_V1] = {0};
    v1[0] = KILNLINK_POWER_CMD;
    v1[1] = pw.power_window_s;
    v1[2] = pw.flags; /* COUNTS_VALID (bit3) clear -- a real legacy peer never sets it */
    memcpy(&v1[3], &pw.mains_voltage_v, 4);
    memcpy(&v1[43], &pw.p_total_w, 4);
    memcpy(&v1[47], &pw.energy_wh, 8);

    kilnlink_power_t decoded;
    kilnlink_power_status_t status = kilnlink_power_decode(v1, sizeof(v1), &decoded);
    CHECK(status == KILNLINK_POWER_OK, "decode() of a legacy 55-byte V1 frame is OK, not a mismatch");
    CHECK((decoded.flags & (uint8_t)KILNLINK_POWER_FLAG_COUNTS_VALID) == 0,
          "V1 frame decodes with COUNTS_VALID clear");
    for (unsigned ch = 0; ch < KILNLINK_POWER_CHANNELS; ch++) {
        CHECK(decoded.counts_avg[ch] == 0, "V1 frame decodes with counts_avg zeroed, not garbage");
    }
    CHECK(feq(decoded.mains_voltage_v, pw.mains_voltage_v), "V1 frame's other fields still decode correctly");
}

static void test_v2_length_but_flag_not_set_is_treated_as_invalid(void)
{
    /* Untrusted wire input: a 61-byte frame whose sender (a buggy peer, or
     * noise) never set COUNTS_VALID must still be treated as "counts are not
     * real" -- length alone must never be trusted as proof, matching this
     * decoder's own documented "never trust memory/bytes you didn't
     * validate" discipline. NEGATIVE TEST for that discipline: break it by
     * asserting the FLAG governs, not the length, on a length-61 frame with
     * nonzero payload bytes at the counts offset. */
    uint8_t buf[KILNLINK_POWER_LEN_V2] = {0};
    buf[0] = KILNLINK_POWER_CMD;
    buf[2] = 0x00; /* COUNTS_VALID clear */
    buf[55] = 0xAA; buf[56] = 0xAA; /* nonzero counts bytes an honest V2 sender would never leave here unflagged */

    kilnlink_power_t decoded;
    CHECK(kilnlink_power_decode(buf, sizeof(buf), &decoded) == KILNLINK_POWER_OK,
          "decode() of a 61-byte frame with the flag clear still succeeds");
    CHECK(decoded.counts_avg[0] == 0,
          "counts_avg is zeroed, not the raw (0xAAAA) bytes, when COUNTS_VALID is clear -- "
          "this assertion is the one that would catch a decoder that trusted length over the flag");
}

static void test_round_trip_mains_not_configured_is_nan(void)
{
    /* LINK_PROTOCOL.md sec 6, Frame E: mains_voltage_v is NaN if not set, and
     * every power figure downstream of it (p_avg_w per channel, p_total_w) is
     * NaN too -- currents/fractions themselves stay real readings. */
    kilnlink_power_t pw = {0};
    pw.power_window_s = 120;
    pw.flags = 0; /* MAINS_VOLTAGE_CONFIGURED clear */
    pw.mains_voltage_v = (float)NAN;
    pw.i_conducting_a[0] = 8.5f;
    pw.i_conducting_a[1] = 7.9f;
    pw.i_conducting_a[2] = 0.0f;
    pw.conduction_fraction[0] = 0.42f;
    pw.conduction_fraction[1] = 0.38f;
    pw.conduction_fraction[2] = 0.0f;
    pw.p_avg_w[0] = (float)NAN;
    pw.p_avg_w[1] = (float)NAN;
    pw.p_avg_w[2] = (float)NAN;
    pw.p_total_w = (float)NAN;
    pw.energy_wh = 500.0;

    uint8_t buf[KILNLINK_POWER_LEN];
    kilnlink_power_status_t status;
    size_t n = kilnlink_power_encode(&pw, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_POWER_OK, "encode() with NaN power fields reports OK");

    kilnlink_power_t decoded;
    CHECK(kilnlink_power_decode(buf, n, &decoded) == KILNLINK_POWER_OK,
          "decode() of a NaN-power payload is OK");
    CHECK(isnan(decoded.mains_voltage_v), "mains_voltage_v round-trips as NaN, not fixed up to 0");
    CHECK(isnan(decoded.p_total_w), "p_total_w round-trips as NaN");
    CHECK(decoded.i_conducting_a[0] == 8.5f, "i_conducting_a is a legitimate reading, not NaN -- the NaN rule is power-only");
}

/* -- byte-exact vectors (test/vectors/power_vectors.json) ---------------- */

static void test_vector_healthy_reading(void)
{
    /* 55-byte V1 body unchanged, +6 bytes counts_avg (2026-09-06); flags
     * byte gains bit3 (COUNTS_VALID), always set by this build's encoder. */
    static const uint8_t expected[] = {
        0x0e, 0x78, 0x0d, 0x00, 0x00, 0x70, 0x43, 0x00, 0x00, 0x08, 0x41, 0x3d,
        0x0a, 0xd7, 0x3e, 0xcd, 0xcc, 0xfc, 0x40, 0x5c, 0x8f, 0xc2, 0x3e, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0x44, 0xcd,
        0x0c, 0x56, 0x44, 0x00, 0x00, 0x00, 0x00, 0x66, 0x06, 0xe9, 0x44, 0x58,
        0x39, 0xb4, 0xc8, 0xd6, 0x1c, 0xc8, 0x40, 0x0a, 0x00, 0xd0, 0x07, 0xff, 0x0f,
    };
    kilnlink_power_t pw = {0};
    pw.power_window_s = 120;
    pw.flags = 0x05;
    pw.mains_voltage_v = 240.0f;
    pw.i_conducting_a[0] = 8.5f;
    pw.conduction_fraction[0] = 0.42f;
    pw.i_conducting_a[1] = 7.9f;
    pw.conduction_fraction[1] = 0.38f;
    pw.i_conducting_a[2] = 0.0f;
    pw.conduction_fraction[2] = 0.0f;
    pw.p_avg_w[0] = 1008.0f;
    pw.p_avg_w[1] = 856.2f;
    pw.p_avg_w[2] = 0.0f;
    pw.p_total_w = 1864.2f;
    pw.energy_wh = 12345.678;
    pw.counts_avg[0] = 10;
    pw.counts_avg[1] = 2000;
    pw.counts_avg[2] = 4095;

    uint8_t buf[KILNLINK_POWER_LEN];
    kilnlink_power_status_t status;
    size_t n = kilnlink_power_encode(&pw, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_POWER_OK, "vector healthy_reading: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector healthy_reading: bytes match power_vectors.json");
    } else {
        CHECK(1, "vector healthy_reading: bytes match power_vectors.json");
    }
}

static void test_vector_mains_not_configured(void)
{
    static const uint8_t expected[] = {
        0x0e, 0x78, 0x08, 0x00, 0x00, 0xc0, 0x7f, 0x00, 0x00, 0x08, 0x41, 0x3d,
        0x0a, 0xd7, 0x3e, 0xcd, 0xcc, 0xfc, 0x40, 0x5c, 0x8f, 0xc2, 0x3e, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x7f, 0x00,
        0x00, 0xc0, 0x7f, 0x00, 0x00, 0xc0, 0x7f, 0x00, 0x00, 0xc0, 0x7f, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x40, 0x7f, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    kilnlink_power_t pw = {0};
    pw.power_window_s = 120;
    pw.flags = 0x00;
    pw.mains_voltage_v = (float)NAN;
    pw.i_conducting_a[0] = 8.5f;
    pw.conduction_fraction[0] = 0.42f;
    pw.i_conducting_a[1] = 7.9f;
    pw.conduction_fraction[1] = 0.38f;
    pw.i_conducting_a[2] = 0.0f;
    pw.conduction_fraction[2] = 0.0f;
    pw.p_avg_w[0] = (float)NAN;
    pw.p_avg_w[1] = (float)NAN;
    pw.p_avg_w[2] = (float)NAN;
    pw.p_total_w = (float)NAN;
    pw.energy_wh = 500.0;

    uint8_t buf[KILNLINK_POWER_LEN];
    kilnlink_power_status_t status;
    size_t n = kilnlink_power_encode(&pw, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_POWER_OK, "vector mains_not_configured: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector mains_not_configured: bytes match power_vectors.json");
    } else {
        CHECK(1, "vector mains_not_configured: bytes match power_vectors.json");
    }
}

/* -- decode hostile inputs ------------------------------------------------ */

static void test_decode_too_short(void)
{
    /* One byte short of V1 (54) -- neither of the two accepted lengths. */
    uint8_t buf[KILNLINK_POWER_LEN_V1 - 1] = {0};
    buf[0] = KILNLINK_POWER_CMD;
    kilnlink_power_t out;
    CHECK(kilnlink_power_decode(buf, sizeof(buf), &out) == KILNLINK_POWER_ERR_LENGTH_MISMATCH,
          "decode() of a 54-byte (one short of V1) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_too_long(void)
{
    /* One byte past V2 (62) -- this frame only ever has TWO valid lengths
     * (V1=55, V2=61); anything else, including one byte too many past the
     * current max, is exactly as invalid as one byte too few. */
    uint8_t buf[KILNLINK_POWER_LEN_V2 + 1] = {0};
    buf[0] = KILNLINK_POWER_CMD;
    kilnlink_power_t out;
    CHECK(kilnlink_power_decode(buf, sizeof(buf), &out) == KILNLINK_POWER_ERR_LENGTH_MISMATCH,
          "decode() of a 62-byte (one past V2) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_between_v1_and_v2_is_rejected(void)
{
    /* 55 < len < 61 is not a valid frame of either version -- must not be
     * silently accepted as a truncated V2 or a padded V1. */
    uint8_t buf[KILNLINK_POWER_LEN_V1 + 3] = {0};
    buf[0] = KILNLINK_POWER_CMD;
    kilnlink_power_t out;
    CHECK(kilnlink_power_decode(buf, sizeof(buf), &out) == KILNLINK_POWER_ERR_LENGTH_MISMATCH,
          "decode() of a length strictly between V1 and V2 -> ERR_LENGTH_MISMATCH");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_POWER_LEN] = {0};
    buf[0] = 0x01; /* GET_STATUS's id, not POWER's */
    kilnlink_power_t out;
    CHECK(kilnlink_power_decode(buf, sizeof(buf), &out) == KILNLINK_POWER_ERR_WRONG_CMD,
          "decode() with wrong command byte -> ERR_WRONG_CMD");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_power_t pw = {0};
    uint8_t buf[10]; /* needs 55 */
    kilnlink_power_status_t status;
    size_t n = kilnlink_power_encode(&pw, buf, sizeof(buf), &status);
    CHECK(n == 0, "encode() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_POWER_ERR_BUFFER_TOO_SMALL,
          "encode() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
}

int main(void)
{
    test_round_trip_healthy();
    test_round_trip_mains_not_configured_is_nan();
    test_vector_healthy_reading();
    test_vector_mains_not_configured();
    test_legacy_v1_length_decodes_with_zeroed_counts();
    test_v2_length_but_flag_not_set_is_treated_as_invalid();
    test_decode_too_short();
    test_decode_too_long();
    test_decode_between_v1_and_v2_is_rejected();
    test_decode_wrong_cmd();
    test_encode_buffer_too_small();

    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
