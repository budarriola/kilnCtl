/* Host-native test for kilnlink_ct_cal.{c,h} -- the Pico->ESP
 * SAFETY_CMD_CT_CAL (0x1A) reply codec, docs/LINK_PROTOCOL.md sec 6. Mirrors
 * test_power.c's structure: round-trip encode/decode (including per-channel
 * independence -- each channel's calibrated/gain/offset must decode back
 * exactly as encoded, without bleeding into a neighbouring channel), a
 * byte-exact vector, and the hostile input set: too-short, too-long
 * (fixed-size frame), wrong command byte.
 */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_ct_cal.h"

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

/* -- round trip --------------------------------------------------------- */

static void test_round_trip_mixed(void)
{
    /* Channel 0 calibrated, channel 1 uncalibrated, channel 2 calibrated
     * with different constants -- proves per-channel independence: setting
     * one channel's fields must never disturb another's on the wire. */
    kilnlink_ct_cal_t cal = {0};
    cal.channels[0].calibrated = 1;
    cal.channels[0].gain = 1.021f;
    cal.channels[0].offset = -0.03f;
    cal.channels[1].calibrated = 0;
    cal.channels[1].gain = 0.0f;
    cal.channels[1].offset = 0.0f;
    cal.channels[2].calibrated = 1;
    cal.channels[2].gain = 0.987f;
    cal.channels[2].offset = 0.11f;

    uint8_t buf[KILNLINK_CT_CAL_LEN];
    kilnlink_ct_cal_status_t status;
    size_t n = kilnlink_ct_cal_encode(&cal, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_CT_CAL_OK, "encode() reports OK");
    CHECK(n == KILNLINK_CT_CAL_LEN, "encode() always writes exactly 28 bytes");

    kilnlink_ct_cal_t decoded;
    CHECK(kilnlink_ct_cal_decode(buf, n, &decoded) == KILNLINK_CT_CAL_OK,
          "decode() reports OK for a just-encoded payload");

    CHECK(decoded.channels[0].calibrated == 1, "channel 0 calibrated round-trips");
    CHECK(decoded.channels[0].gain == cal.channels[0].gain, "channel 0 gain round-trips");
    CHECK(decoded.channels[0].offset == cal.channels[0].offset, "channel 0 offset round-trips");

    CHECK(decoded.channels[1].calibrated == 0,
          "channel 1 calibrated round-trips as 0, unaffected by channel 0/2's values");
    CHECK(decoded.channels[1].gain == 0.0f, "channel 1 gain untouched by neighbours");

    CHECK(decoded.channels[2].calibrated == 1, "channel 2 calibrated round-trips");
    CHECK(decoded.channels[2].gain == cal.channels[2].gain, "channel 2 gain round-trips");
    CHECK(decoded.channels[2].offset == cal.channels[2].offset, "channel 2 offset round-trips");
}

static void test_round_trip_all_uncalibrated(void)
{
    /* The all-zero/all-uncalibrated table -- config_store_default()'s shape,
     * what a blank/never-commissioned Pico reports. */
    kilnlink_ct_cal_t cal = {0};

    uint8_t buf[KILNLINK_CT_CAL_LEN];
    kilnlink_ct_cal_status_t status;
    size_t n = kilnlink_ct_cal_encode(&cal, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_CT_CAL_OK, "all-uncalibrated: encode OK");

    kilnlink_ct_cal_t decoded;
    CHECK(kilnlink_ct_cal_decode(buf, n, &decoded) == KILNLINK_CT_CAL_OK, "all-uncalibrated: decode OK");
    for (unsigned ch = 0; ch < KILNLINK_CT_CAL_NUM_CHANNELS; ch++) {
        CHECK(decoded.channels[ch].calibrated == 0, "all-uncalibrated: every channel decodes as uncalibrated");
    }
}

/* -- byte-exact vector ---------------------------------------------------- */

static void test_vector_all_zero(void)
{
    static const uint8_t expected[] = {
        0x1a,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    kilnlink_ct_cal_t cal = {0};

    uint8_t buf[KILNLINK_CT_CAL_LEN];
    kilnlink_ct_cal_status_t status;
    size_t n = kilnlink_ct_cal_encode(&cal, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_CT_CAL_OK, "vector all_zero: encode OK");
    CHECK(sizeof(expected) == KILNLINK_CT_CAL_LEN, "vector all_zero: expected[] sized correctly");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector all_zero: bytes match");
    } else {
        CHECK(1, "vector all_zero: bytes match");
    }
}

/* -- decode hostile inputs ------------------------------------------------ */

static void test_decode_too_short(void)
{
    uint8_t buf[KILNLINK_CT_CAL_LEN - 1] = {0};
    buf[0] = KILNLINK_CT_CAL_CMD;
    kilnlink_ct_cal_t out;
    CHECK(kilnlink_ct_cal_decode(buf, sizeof(buf), &out) == KILNLINK_CT_CAL_ERR_LENGTH_MISMATCH,
          "decode() of a 27-byte (one short) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_too_long(void)
{
    uint8_t buf[KILNLINK_CT_CAL_LEN + 1] = {0};
    buf[0] = KILNLINK_CT_CAL_CMD;
    kilnlink_ct_cal_t out;
    CHECK(kilnlink_ct_cal_decode(buf, sizeof(buf), &out) == KILNLINK_CT_CAL_ERR_LENGTH_MISMATCH,
          "decode() of a 29-byte (one too many) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_CT_CAL_LEN] = {0};
    buf[0] = 0x0E; /* POWER's id, not CT_CAL's */
    kilnlink_ct_cal_t out;
    CHECK(kilnlink_ct_cal_decode(buf, sizeof(buf), &out) == KILNLINK_CT_CAL_ERR_WRONG_CMD,
          "decode() with wrong command byte -> ERR_WRONG_CMD");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_ct_cal_t cal = {0};
    uint8_t buf[10]; /* needs 28 */
    kilnlink_ct_cal_status_t status;
    size_t n = kilnlink_ct_cal_encode(&cal, buf, sizeof(buf), &status);
    CHECK(n == 0, "encode() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_CT_CAL_ERR_BUFFER_TOO_SMALL,
          "encode() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
}

int main(void)
{
    test_round_trip_mixed();
    test_round_trip_all_uncalibrated();
    test_vector_all_zero();
    test_decode_too_short();
    test_decode_too_long();
    test_decode_wrong_cmd();
    test_encode_buffer_too_small();

    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
