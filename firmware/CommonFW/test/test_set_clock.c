/* Host-native test for kilnlink_set_clock.{c,h} -- the ESP->Pico
 * SAFETY_CMD_SET_CLOCK (0x0C, optional) codec, docs/LINK_PROTOCOL.md sec 4.
 * Mirrors test_trip.c's structure: round-trip encode/decode, byte-exact
 * vectors from test/vectors/set_clock_vectors.json, and the hostile input
 * set: too-short, too-long (fixed-size frame), wrong command byte.
 */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_set_clock.h"

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
    kilnlink_set_clock_t msg = {0};
    msg.epoch_ms = 1700000000000ULL;

    uint8_t buf[KILNLINK_SET_CLOCK_LEN];
    kilnlink_set_clock_status_t status;
    size_t n = kilnlink_set_clock_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_SET_CLOCK_OK, "encode() reports OK");
    CHECK(n == KILNLINK_SET_CLOCK_LEN, "encode() always writes exactly 9 bytes");

    kilnlink_set_clock_t decoded;
    kilnlink_set_clock_status_t dstatus = kilnlink_set_clock_decode(buf, n, &decoded);
    CHECK(dstatus == KILNLINK_SET_CLOCK_OK, "decode() reports OK for a just-encoded payload");
    CHECK(decoded.epoch_ms == msg.epoch_ms, "decoded epoch_ms matches");
}

static void test_round_trip_max_u64(void)
{
    kilnlink_set_clock_t msg = {0};
    msg.epoch_ms = 0xFFFFFFFFFFFFFFFFULL;

    uint8_t buf[KILNLINK_SET_CLOCK_LEN];
    kilnlink_set_clock_status_t status;
    size_t n = kilnlink_set_clock_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_SET_CLOCK_OK, "encode() at u64 max reports OK");

    kilnlink_set_clock_t decoded;
    CHECK(kilnlink_set_clock_decode(buf, n, &decoded) == KILNLINK_SET_CLOCK_OK, "decode() OK");
    CHECK(decoded.epoch_ms == 0xFFFFFFFFFFFFFFFFULL, "epoch_ms round-trips at the u64 max");
}

/* -- byte-exact vectors (test/vectors/set_clock_vectors.json) ----------- */

static void test_vector_real_epoch(void)
{
    static const uint8_t expected[] = {0x0c, 0x00, 0x68, 0xe5, 0xcf, 0x8b, 0x01, 0x00, 0x00};
    kilnlink_set_clock_t msg = {0};
    msg.epoch_ms = 1700000000000ULL;

    uint8_t buf[KILNLINK_SET_CLOCK_LEN];
    kilnlink_set_clock_status_t status;
    size_t n = kilnlink_set_clock_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_SET_CLOCK_OK, "vector real_epoch: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector real_epoch: bytes match set_clock_vectors.json");
    } else {
        CHECK(1, "vector real_epoch: bytes match set_clock_vectors.json");
    }
}

static void test_vector_zero(void)
{
    static const uint8_t expected[] = {0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    kilnlink_set_clock_t msg = {0};
    msg.epoch_ms = 0;

    uint8_t buf[KILNLINK_SET_CLOCK_LEN];
    kilnlink_set_clock_status_t status;
    size_t n = kilnlink_set_clock_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_SET_CLOCK_OK, "vector zero: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector zero: bytes match set_clock_vectors.json");
    } else {
        CHECK(1, "vector zero: bytes match set_clock_vectors.json");
    }
}

/* -- decode hostile inputs ------------------------------------------------ */

static void test_decode_too_short(void)
{
    uint8_t buf[KILNLINK_SET_CLOCK_LEN - 1] = {0};
    buf[0] = KILNLINK_SET_CLOCK_CMD;
    kilnlink_set_clock_t out;
    CHECK(kilnlink_set_clock_decode(buf, sizeof(buf), &out) == KILNLINK_SET_CLOCK_ERR_LENGTH_MISMATCH,
          "decode() of an 8-byte (one short) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_too_long(void)
{
    uint8_t buf[KILNLINK_SET_CLOCK_LEN + 1] = {0};
    buf[0] = KILNLINK_SET_CLOCK_CMD;
    kilnlink_set_clock_t out;
    CHECK(kilnlink_set_clock_decode(buf, sizeof(buf), &out) == KILNLINK_SET_CLOCK_ERR_LENGTH_MISMATCH,
          "decode() of a 10-byte (one too many) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_SET_CLOCK_LEN] = {0};
    buf[0] = 0x0B; /* GET_FW_VERSION's id, not SET_CLOCK's */
    kilnlink_set_clock_t out;
    CHECK(kilnlink_set_clock_decode(buf, sizeof(buf), &out) == KILNLINK_SET_CLOCK_ERR_WRONG_CMD,
          "decode() with wrong command byte -> ERR_WRONG_CMD");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_set_clock_t msg = {0};
    uint8_t buf[4]; /* needs 9 */
    kilnlink_set_clock_status_t status;
    size_t n = kilnlink_set_clock_encode(&msg, buf, sizeof(buf), &status);
    CHECK(n == 0, "encode() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_SET_CLOCK_ERR_BUFFER_TOO_SMALL,
          "encode() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
}

int main(void)
{
    test_round_trip();
    test_round_trip_max_u64();
    test_vector_real_epoch();
    test_vector_zero();
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
