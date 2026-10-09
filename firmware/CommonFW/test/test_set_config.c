/* Host-native test for kilnlink_set_config.{c,h} -- the ESP->Pico
 * SAFETY_CMD_SET_CONFIG (0x16) codec, docs/LINK_PROTOCOL.md sec 4. Mirrors
 * test_clear_trip.c's structure: round-trip encode/decode, byte-exact
 * vectors from test/vectors/set_config_vectors.json, and the hostile input
 * set: too-short, too-long (fixed-size frame), wrong command byte.
 */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_set_config.h"

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
    kilnlink_set_config_t msg = {0};
    msg.tc_type = 0x03u;

    uint8_t buf[KILNLINK_SET_CONFIG_LEN];
    kilnlink_set_config_status_t status;
    size_t n = kilnlink_set_config_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_SET_CONFIG_OK, "encode() reports OK");
    CHECK(n == KILNLINK_SET_CONFIG_LEN, "encode() always writes exactly 2 bytes");

    kilnlink_set_config_t decoded;
    kilnlink_set_config_status_t dstatus = kilnlink_set_config_decode(buf, n, &decoded);
    CHECK(dstatus == KILNLINK_SET_CONFIG_OK, "decode() reports OK for a just-encoded payload");
    CHECK(decoded.tc_type == msg.tc_type, "decoded tc_type matches");
}

static void test_round_trip_zero(void)
{
    kilnlink_set_config_t msg = {0};
    msg.tc_type = 0;

    uint8_t buf[KILNLINK_SET_CONFIG_LEN];
    kilnlink_set_config_status_t status;
    size_t n = kilnlink_set_config_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_SET_CONFIG_OK, "encode() with tc_type=0 reports OK");

    kilnlink_set_config_t decoded;
    CHECK(kilnlink_set_config_decode(buf, n, &decoded) == KILNLINK_SET_CONFIG_OK, "decode() OK");
    CHECK(decoded.tc_type == 0, "tc_type round-trips as 0");
}

/* -- byte-exact vectors (test/vectors/set_config_vectors.json) ---------- */

static void test_vector_tc_type_k(void)
{
    static const uint8_t expected[] = {0x16, 0x03};
    kilnlink_set_config_t msg = {0};
    msg.tc_type = 3;

    uint8_t buf[KILNLINK_SET_CONFIG_LEN];
    kilnlink_set_config_status_t status;
    size_t n = kilnlink_set_config_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_SET_CONFIG_OK, "vector tc_type_k: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector tc_type_k: bytes match set_config_vectors.json");
    } else {
        CHECK(1, "vector tc_type_k: bytes match set_config_vectors.json");
    }
}

static void test_vector_tc_type_max(void)
{
    static const uint8_t expected[] = {0x16, 0xff};
    kilnlink_set_config_t msg = {0};
    msg.tc_type = 0xFFu;

    uint8_t buf[KILNLINK_SET_CONFIG_LEN];
    kilnlink_set_config_status_t status;
    size_t n = kilnlink_set_config_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_SET_CONFIG_OK, "vector tc_type_max: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector tc_type_max: bytes match set_config_vectors.json");
    } else {
        CHECK(1, "vector tc_type_max: bytes match set_config_vectors.json");
    }
}

/* -- decode hostile inputs ------------------------------------------------ */

static void test_decode_too_short(void)
{
    uint8_t buf[KILNLINK_SET_CONFIG_LEN - 1] = {0};
    buf[0] = KILNLINK_SET_CONFIG_CMD;
    kilnlink_set_config_t out;
    CHECK(kilnlink_set_config_decode(buf, sizeof(buf), &out) == KILNLINK_SET_CONFIG_ERR_LENGTH_MISMATCH,
          "decode() of a 1-byte (one short) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_too_long(void)
{
    uint8_t buf[KILNLINK_SET_CONFIG_LEN + 1] = {0};
    buf[0] = KILNLINK_SET_CONFIG_CMD;
    kilnlink_set_config_t out;
    CHECK(kilnlink_set_config_decode(buf, sizeof(buf), &out) == KILNLINK_SET_CONFIG_ERR_LENGTH_MISMATCH,
          "decode() of a 3-byte (one too many) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_SET_CONFIG_LEN] = {0};
    buf[0] = 0x0A; /* CLEAR_TRIP's id, not SET_CONFIG's */
    kilnlink_set_config_t out;
    CHECK(kilnlink_set_config_decode(buf, sizeof(buf), &out) == KILNLINK_SET_CONFIG_ERR_WRONG_CMD,
          "decode() with wrong command byte -> ERR_WRONG_CMD");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_set_config_t msg = {0};
    uint8_t buf[1]; /* needs 2 */
    kilnlink_set_config_status_t status;
    size_t n = kilnlink_set_config_encode(&msg, buf, 0, &status);
    CHECK(n == 0, "encode() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_SET_CONFIG_ERR_BUFFER_TOO_SMALL,
          "encode() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
    (void)buf;
}

int main(void)
{
    test_round_trip();
    test_round_trip_zero();
    test_vector_tc_type_k();
    test_vector_tc_type_max();
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
