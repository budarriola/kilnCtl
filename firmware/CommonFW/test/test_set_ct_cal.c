/* Host-native test for kilnlink_set_ct_cal.{c,h} -- the ESP->Pico
 * SAFETY_CMD_SET_CT_CAL (0x19) codec, docs/LINK_PROTOCOL.md sec 4. Mirrors
 * test_set_config.c's structure: round-trip encode/decode, a byte-exact
 * vector (hand-computed, no vectors.json for this new codec yet), and the
 * hostile input set: too-short, too-long, wrong command byte.
 */

#include <string.h>
#include <stdio.h>

#include "kilnlink/kilnlink_set_ct_cal.h"

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

static void test_round_trip(void)
{
    kilnlink_set_ct_cal_t msg = {0};
    msg.channel = 1;
    msg.calibrated = 1;
    msg.gain = 1.0321f;
    msg.offset = -0.045f;

    uint8_t buf[KILNLINK_SET_CT_CAL_LEN];
    kilnlink_set_ct_cal_status_t status;
    size_t n = kilnlink_set_ct_cal_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_SET_CT_CAL_OK, "encode() reports OK");
    CHECK(n == KILNLINK_SET_CT_CAL_LEN, "encode() always writes exactly 11 bytes");

    kilnlink_set_ct_cal_t decoded;
    kilnlink_set_ct_cal_status_t dstatus = kilnlink_set_ct_cal_decode(buf, n, &decoded);
    CHECK(dstatus == KILNLINK_SET_CT_CAL_OK, "decode() reports OK for a just-encoded payload");
    CHECK(decoded.channel == msg.channel, "decoded channel matches");
    CHECK(decoded.calibrated == msg.calibrated, "decoded calibrated matches");
    CHECK(decoded.gain == msg.gain, "decoded gain matches");
    CHECK(decoded.offset == msg.offset, "decoded offset matches");
}

static void test_round_trip_per_channel(void)
{
    /* Each channel index must round-trip independently -- a wire-level
     * analogue of the "per-channel independence" property this whole
     * feature is required to prove. */
    for (uint8_t ch = 0; ch < KILNLINK_SET_CT_CAL_NUM_CHANNELS; ch++) {
        kilnlink_set_ct_cal_t msg = {0};
        msg.channel = ch;
        msg.calibrated = 1;
        msg.gain = 2.0f + (float)ch;
        msg.offset = 0.1f * (float)ch;

        uint8_t buf[KILNLINK_SET_CT_CAL_LEN];
        kilnlink_set_ct_cal_status_t status;
        size_t n = kilnlink_set_ct_cal_encode(&msg, buf, sizeof(buf), &status);
        CHECK(status == KILNLINK_SET_CT_CAL_OK, "per-channel: encode OK");

        kilnlink_set_ct_cal_t decoded;
        CHECK(kilnlink_set_ct_cal_decode(buf, n, &decoded) == KILNLINK_SET_CT_CAL_OK,
              "per-channel: decode OK");
        CHECK(decoded.channel == ch, "per-channel: channel index round-trips");
        CHECK(decoded.gain == msg.gain, "per-channel: gain round-trips");
        CHECK(decoded.offset == msg.offset, "per-channel: offset round-trips");
    }
}

static void test_round_trip_uncalibrated(void)
{
    /* calibrated=0 with a non-neutral gain/offset must still round-trip
     * exactly -- decode never infers "calibrated" from the numbers, only
     * from this byte (see config_store.h for why). */
    kilnlink_set_ct_cal_t msg = {0};
    msg.channel = 2;
    msg.calibrated = 0;
    msg.gain = 99.0f;
    msg.offset = -99.0f;

    uint8_t buf[KILNLINK_SET_CT_CAL_LEN];
    kilnlink_set_ct_cal_status_t status;
    size_t n = kilnlink_set_ct_cal_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_SET_CT_CAL_OK, "uncalibrated: encode OK");

    kilnlink_set_ct_cal_t decoded;
    CHECK(kilnlink_set_ct_cal_decode(buf, n, &decoded) == KILNLINK_SET_CT_CAL_OK,
          "uncalibrated: decode OK");
    CHECK(decoded.calibrated == 0, "uncalibrated: calibrated byte round-trips as 0");
    CHECK(decoded.gain == 99.0f, "uncalibrated: gain still round-trips even though ignored downstream");
}

/* -- byte-exact vector ---------------------------------------------------- */

static void test_vector_zero(void)
{
    /* channel=0, calibrated=0, gain=0.0f, offset=0.0f -- every multi-byte
     * field is all-zero, so the expected bytes are trivial to hand-verify. */
    static const uint8_t expected[] = {0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x00, 0x00};
    kilnlink_set_ct_cal_t msg = {0};

    uint8_t buf[KILNLINK_SET_CT_CAL_LEN];
    kilnlink_set_ct_cal_status_t status;
    size_t n = kilnlink_set_ct_cal_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_SET_CT_CAL_OK, "vector zero: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector zero: bytes match");
    } else {
        CHECK(1, "vector zero: bytes match");
    }
}

static void test_vector_channel2_gain1(void)
{
    /* channel=2, calibrated=1, gain=1.0f (0x3F800000 LE), offset=0.0f. */
    static const uint8_t expected[] = {0x19, 0x02, 0x01, 0x00, 0x00, 0x80, 0x3f,
                                        0x00, 0x00, 0x00, 0x00};
    kilnlink_set_ct_cal_t msg = {0};
    msg.channel = 2;
    msg.calibrated = 1;
    msg.gain = 1.0f;
    msg.offset = 0.0f;

    uint8_t buf[KILNLINK_SET_CT_CAL_LEN];
    kilnlink_set_ct_cal_status_t status;
    size_t n = kilnlink_set_ct_cal_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_SET_CT_CAL_OK, "vector channel2_gain1: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector channel2_gain1: bytes match");
    } else {
        CHECK(1, "vector channel2_gain1: bytes match");
    }
}

/* -- decode hostile inputs ------------------------------------------------ */

static void test_decode_too_short(void)
{
    uint8_t buf[KILNLINK_SET_CT_CAL_LEN - 1] = {0};
    buf[0] = KILNLINK_SET_CT_CAL_CMD;
    kilnlink_set_ct_cal_t out;
    CHECK(kilnlink_set_ct_cal_decode(buf, sizeof(buf), &out) == KILNLINK_SET_CT_CAL_ERR_LENGTH_MISMATCH,
          "decode() of a 10-byte (one short) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_too_long(void)
{
    uint8_t buf[KILNLINK_SET_CT_CAL_LEN + 1] = {0};
    buf[0] = KILNLINK_SET_CT_CAL_CMD;
    kilnlink_set_ct_cal_t out;
    CHECK(kilnlink_set_ct_cal_decode(buf, sizeof(buf), &out) == KILNLINK_SET_CT_CAL_ERR_LENGTH_MISMATCH,
          "decode() of a 12-byte (one too many) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_SET_CT_CAL_LEN] = {0};
    buf[0] = 0x16; /* SET_CONFIG's id, not SET_CT_CAL's */
    kilnlink_set_ct_cal_t out;
    CHECK(kilnlink_set_ct_cal_decode(buf, sizeof(buf), &out) == KILNLINK_SET_CT_CAL_ERR_WRONG_CMD,
          "decode() with wrong command byte -> ERR_WRONG_CMD");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_set_ct_cal_t msg = {0};
    uint8_t buf[1]; /* needs 11 */
    kilnlink_set_ct_cal_status_t status;
    size_t n = kilnlink_set_ct_cal_encode(&msg, buf, 0, &status);
    CHECK(n == 0, "encode() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_SET_CT_CAL_ERR_BUFFER_TOO_SMALL,
          "encode() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
}

int main(void)
{
    test_round_trip();
    test_round_trip_per_channel();
    test_round_trip_uncalibrated();
    test_vector_zero();
    test_vector_channel2_gain1();
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
