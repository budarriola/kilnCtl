/* Host-native test for kilnlink_commit_config.{c,h} -- the ESP->Pico
 * SAFETY_CMD_COMMIT_CONFIG (0x1D) codec, docs/LINK_PROTOCOL.md sec 4,
 * docs/COMMISSIONING.md sec 2. One byte, no arguments -- mirrors
 * test_get_ct_cal.c's structure: round-trip encode/decode, a byte-exact
 * vector, and the hostile input set: too-short, too-long (fixed-size
 * frame), wrong command byte.
 */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_commit_config.h"

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

static void test_round_trip(void)
{
    kilnlink_commit_config_t msg = {0};

    uint8_t buf[KILNLINK_COMMIT_CONFIG_LEN];
    kilnlink_commit_config_status_t status;
    size_t n = kilnlink_commit_config_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_COMMIT_CONFIG_OK, "encode() reports OK");
    CHECK(n == KILNLINK_COMMIT_CONFIG_LEN, "encode() always writes exactly 1 byte");
    CHECK(buf[0] == KILNLINK_COMMIT_CONFIG_CMD, "encoded byte is the command id");

    kilnlink_commit_config_t decoded;
    CHECK(kilnlink_commit_config_decode(buf, n, &decoded) == KILNLINK_COMMIT_CONFIG_OK,
          "decode() reports OK for a just-encoded payload");
}

static void test_encode_null_msg(void)
{
    uint8_t buf[KILNLINK_COMMIT_CONFIG_LEN];
    kilnlink_commit_config_status_t status;
    size_t n = kilnlink_commit_config_encode(NULL, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_COMMIT_CONFIG_OK, "encode(NULL) reports OK");
    CHECK(n == KILNLINK_COMMIT_CONFIG_LEN, "encode(NULL) still writes 1 byte");
}

static void test_decode_null_out(void)
{
    uint8_t buf[KILNLINK_COMMIT_CONFIG_LEN] = {KILNLINK_COMMIT_CONFIG_CMD};
    CHECK(kilnlink_commit_config_decode(buf, sizeof(buf), NULL) == KILNLINK_COMMIT_CONFIG_OK,
          "decode(..., NULL) still reports OK -- nothing to fill in");
}

static void test_vector_request(void)
{
    static const uint8_t expected[] = {0x1d};
    kilnlink_commit_config_t msg = {0};

    uint8_t buf[KILNLINK_COMMIT_CONFIG_LEN];
    kilnlink_commit_config_status_t status;
    size_t n = kilnlink_commit_config_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_COMMIT_CONFIG_OK, "vector request: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector request: bytes match {0x1d}");
    } else {
        CHECK(1, "vector request: bytes match {0x1d}");
    }
}

static void test_decode_too_short(void)
{
    uint8_t buf[1];
    kilnlink_commit_config_t out;
    CHECK(kilnlink_commit_config_decode(buf, 0, &out) == KILNLINK_COMMIT_CONFIG_ERR_LENGTH_MISMATCH,
          "decode() of a 0-byte payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_too_long(void)
{
    uint8_t buf[KILNLINK_COMMIT_CONFIG_LEN + 1] = {0};
    buf[0] = KILNLINK_COMMIT_CONFIG_CMD;
    kilnlink_commit_config_t out;
    CHECK(kilnlink_commit_config_decode(buf, sizeof(buf), &out) == KILNLINK_COMMIT_CONFIG_ERR_LENGTH_MISMATCH,
          "decode() of a 2-byte (one too many) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_COMMIT_CONFIG_LEN] = {0};
    buf[0] = 0x1Cu; /* SET_PARAM's id, not COMMIT_CONFIG's */
    kilnlink_commit_config_t out;
    CHECK(kilnlink_commit_config_decode(buf, sizeof(buf), &out) == KILNLINK_COMMIT_CONFIG_ERR_WRONG_CMD,
          "decode() with wrong command byte -> ERR_WRONG_CMD");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_commit_config_t msg = {0};
    uint8_t buf[1];
    kilnlink_commit_config_status_t status;
    size_t n = kilnlink_commit_config_encode(&msg, buf, 0, &status);
    CHECK(n == 0, "encode() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_COMMIT_CONFIG_ERR_BUFFER_TOO_SMALL,
          "encode() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
}

int main(void)
{
    test_round_trip();
    test_encode_null_msg();
    test_decode_null_out();
    test_vector_request();
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
