/* Host-native test for kilnlink_set_log_level.{c,h} -- the ESP->Pico
 * SAFETY_CMD_SET_LOG_LEVEL (0x1B) codec, docs/LINK_PROTOCOL.md sec 4,
 * docs/COMMISSIONING.md sec 2. Mirrors test_set_config.c's structure:
 * round-trip encode/decode, byte-exact vectors, and the hostile input set:
 * too-short, too-long (fixed-size frame), wrong command byte.
 */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_set_log_level.h"

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
    kilnlink_set_log_level_t msg = {0};
    msg.level = 2; /* LOG_LEVEL_INFO */

    uint8_t buf[KILNLINK_SET_LOG_LEVEL_LEN];
    kilnlink_set_log_level_status_t status;
    size_t n = kilnlink_set_log_level_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_SET_LOG_LEVEL_OK, "encode() reports OK");
    CHECK(n == KILNLINK_SET_LOG_LEVEL_LEN, "encode() always writes exactly 2 bytes");

    kilnlink_set_log_level_t decoded;
    CHECK(kilnlink_set_log_level_decode(buf, n, &decoded) == KILNLINK_SET_LOG_LEVEL_OK,
          "decode() reports OK for a just-encoded payload");
    CHECK(decoded.level == msg.level, "decoded level matches");
}

static void test_round_trip_every_level(void)
{
    for (uint8_t level = 0; level <= 4; level++) {
        kilnlink_set_log_level_t msg = {0};
        msg.level = level;

        uint8_t buf[KILNLINK_SET_LOG_LEVEL_LEN];
        kilnlink_set_log_level_status_t status;
        size_t n = kilnlink_set_log_level_encode(&msg, buf, sizeof(buf), &status);
        CHECK(status == KILNLINK_SET_LOG_LEVEL_OK, "encode() OK for every LOG_LEVEL_* value");

        kilnlink_set_log_level_t decoded;
        CHECK(kilnlink_set_log_level_decode(buf, n, &decoded) == KILNLINK_SET_LOG_LEVEL_OK,
              "decode() OK for every LOG_LEVEL_* value");
        CHECK(decoded.level == level, "level round-trips exactly");
    }
}

/* -- byte-exact vector ---------------------------------------------------- */

static void test_vector_warn(void)
{
    static const uint8_t expected[] = {0x1b, 0x01};
    kilnlink_set_log_level_t msg = {0};
    msg.level = 1; /* LOG_LEVEL_WARN */

    uint8_t buf[KILNLINK_SET_LOG_LEVEL_LEN];
    kilnlink_set_log_level_status_t status;
    size_t n = kilnlink_set_log_level_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_SET_LOG_LEVEL_OK, "vector warn: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector warn: bytes match {0x1b, 0x01}");
    } else {
        CHECK(1, "vector warn: bytes match {0x1b, 0x01}");
    }
}

/* -- decode hostile inputs ------------------------------------------------ */

static void test_decode_too_short(void)
{
    uint8_t buf[KILNLINK_SET_LOG_LEVEL_LEN - 1] = {0};
    buf[0] = KILNLINK_SET_LOG_LEVEL_CMD;
    kilnlink_set_log_level_t out;
    CHECK(kilnlink_set_log_level_decode(buf, sizeof(buf), &out) == KILNLINK_SET_LOG_LEVEL_ERR_LENGTH_MISMATCH,
          "decode() of a 1-byte (one short) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_too_long(void)
{
    uint8_t buf[KILNLINK_SET_LOG_LEVEL_LEN + 1] = {0};
    buf[0] = KILNLINK_SET_LOG_LEVEL_CMD;
    kilnlink_set_log_level_t out;
    CHECK(kilnlink_set_log_level_decode(buf, sizeof(buf), &out) == KILNLINK_SET_LOG_LEVEL_ERR_LENGTH_MISMATCH,
          "decode() of a 3-byte (one too many) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_SET_LOG_LEVEL_LEN] = {0};
    buf[0] = 0x16; /* SET_CONFIG's id, not SET_LOG_LEVEL's */
    kilnlink_set_log_level_t out;
    CHECK(kilnlink_set_log_level_decode(buf, sizeof(buf), &out) == KILNLINK_SET_LOG_LEVEL_ERR_WRONG_CMD,
          "decode() with wrong command byte -> ERR_WRONG_CMD");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_set_log_level_t msg = {0};
    uint8_t buf[1]; /* needs 2 */
    kilnlink_set_log_level_status_t status;
    size_t n = kilnlink_set_log_level_encode(&msg, buf, 0, &status);
    CHECK(n == 0, "encode() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_SET_LOG_LEVEL_ERR_BUFFER_TOO_SMALL,
          "encode() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
    (void)buf;
}

int main(void)
{
    test_round_trip();
    test_round_trip_every_level();
    test_vector_warn();
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
