/* Host-native test for kilnlink_ceiling.{c,h} -- the ESP->Pico
 * SAFETY_CMD_SET_FIRING_CEILING (0x09) codec, docs/LINK_PROTOCOL.md sec 4.
 * Mirrors test_trip.c's structure: round-trip encode/decode, byte-exact
 * vectors from test/vectors/ceiling_vectors.json, and the hostile input
 * set: too-short, too-long (fixed-size frame), wrong command byte.
 */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_ceiling.h"

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

static int f32_eq(float a, float b)
{
    return a == b;
}

/* -- round trip --------------------------------------------------------- */

static void test_round_trip(void)
{
    kilnlink_ceiling_t msg = {0};
    msg.firing_max_c = 1250.5f;

    uint8_t buf[KILNLINK_CEILING_LEN];
    kilnlink_ceiling_status_t status;
    size_t n = kilnlink_ceiling_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_CEILING_OK, "encode() reports OK");
    CHECK(n == KILNLINK_CEILING_LEN, "encode() always writes exactly 5 bytes");

    kilnlink_ceiling_t decoded;
    kilnlink_ceiling_status_t dstatus = kilnlink_ceiling_decode(buf, n, &decoded);
    CHECK(dstatus == KILNLINK_CEILING_OK, "decode() reports OK for a just-encoded payload");
    CHECK(f32_eq(decoded.firing_max_c, msg.firing_max_c), "decoded firing_max_c matches");
}

static void test_round_trip_no_ceiling(void)
{
    /* LINK_PROTOCOL.md sec 4: "0 or NaN = no firing / no ceiling known" --
     * this codec passes either through unmodified. */
    kilnlink_ceiling_t msg = {0};
    msg.firing_max_c = 0.0f;

    uint8_t buf[KILNLINK_CEILING_LEN];
    kilnlink_ceiling_status_t status;
    size_t n = kilnlink_ceiling_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_CEILING_OK, "encode() with firing_max_c=0 reports OK");

    kilnlink_ceiling_t decoded;
    CHECK(kilnlink_ceiling_decode(buf, n, &decoded) == KILNLINK_CEILING_OK, "decode() OK");
    CHECK(f32_eq(decoded.firing_max_c, 0.0f), "firing_max_c round-trips as 0.0 (no ceiling)");
}

/* -- byte-exact vectors (test/vectors/ceiling_vectors.json) ------------- */

static void test_vector_bisque900(void)
{
    static const uint8_t expected[] = {0x09, 0x00, 0x00, 0x61, 0x44};
    kilnlink_ceiling_t msg = {0};
    msg.firing_max_c = 900.0f;

    uint8_t buf[KILNLINK_CEILING_LEN];
    kilnlink_ceiling_status_t status;
    size_t n = kilnlink_ceiling_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_CEILING_OK, "vector bisque900: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector bisque900: bytes match ceiling_vectors.json");
    } else {
        CHECK(1, "vector bisque900: bytes match ceiling_vectors.json");
    }
}

static void test_vector_none0(void)
{
    static const uint8_t expected[] = {0x09, 0x00, 0x00, 0x00, 0x00};
    kilnlink_ceiling_t msg = {0};
    msg.firing_max_c = 0.0f;

    uint8_t buf[KILNLINK_CEILING_LEN];
    kilnlink_ceiling_status_t status;
    size_t n = kilnlink_ceiling_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_CEILING_OK, "vector none0: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector none0: bytes match ceiling_vectors.json");
    } else {
        CHECK(1, "vector none0: bytes match ceiling_vectors.json");
    }
}

/* -- decode hostile inputs ------------------------------------------------ */

static void test_decode_too_short(void)
{
    uint8_t buf[KILNLINK_CEILING_LEN - 1] = {0};
    buf[0] = KILNLINK_CEILING_CMD;
    kilnlink_ceiling_t out;
    CHECK(kilnlink_ceiling_decode(buf, sizeof(buf), &out) == KILNLINK_CEILING_ERR_LENGTH_MISMATCH,
          "decode() of a 4-byte (one short) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_too_long(void)
{
    uint8_t buf[KILNLINK_CEILING_LEN + 1] = {0};
    buf[0] = KILNLINK_CEILING_CMD;
    kilnlink_ceiling_t out;
    CHECK(kilnlink_ceiling_decode(buf, sizeof(buf), &out) == KILNLINK_CEILING_ERR_LENGTH_MISMATCH,
          "decode() of a 6-byte (one too many) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_CEILING_LEN] = {0};
    buf[0] = 0x0A; /* CLEAR_TRIP's id, not SET_FIRING_CEILING's */
    kilnlink_ceiling_t out;
    CHECK(kilnlink_ceiling_decode(buf, sizeof(buf), &out) == KILNLINK_CEILING_ERR_WRONG_CMD,
          "decode() with wrong command byte -> ERR_WRONG_CMD");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_ceiling_t msg = {0};
    uint8_t buf[2]; /* needs 5 */
    kilnlink_ceiling_status_t status;
    size_t n = kilnlink_ceiling_encode(&msg, buf, sizeof(buf), &status);
    CHECK(n == 0, "encode() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_CEILING_ERR_BUFFER_TOO_SMALL,
          "encode() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
}

int main(void)
{
    test_round_trip();
    test_round_trip_no_ceiling();
    test_vector_bisque900();
    test_vector_none0();
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
