/* Host-native test for kilnlink_get_stack_margin.{c,h} -- the ESP->Pico
 * SAFETY_CMD_GET_STACK_MARGIN (0x2B) request codec, docs/LINK_PROTOCOL.md
 * sec 4. Mirrors test_get_ct_cal.c's structure: a 1-byte, no-argument
 * request whose only content is its own command byte. */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_get_stack_margin.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);          \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

static void test_round_trip(void)
{
    kilnlink_get_stack_margin_t req = {0};
    uint8_t buf[KILNLINK_GET_STACK_MARGIN_LEN];
    kilnlink_get_stack_margin_status_t status;

    size_t n = kilnlink_get_stack_margin_encode(&req, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_GET_STACK_MARGIN_OK, "encode() reports OK");
    CHECK(n == KILNLINK_GET_STACK_MARGIN_LEN, "encode() always writes exactly 1 byte");
    CHECK(buf[0] == KILNLINK_GET_STACK_MARGIN_CMD, "encoded byte 0 is the command id (0x2B)");

    kilnlink_get_stack_margin_t decoded;
    memset(&decoded, 0xAA, sizeof(decoded));
    CHECK(kilnlink_get_stack_margin_decode(buf, n, &decoded) == KILNLINK_GET_STACK_MARGIN_OK,
          "decode() reports OK for a just-encoded payload");
}

static void test_encode_null_msg_ok(void)
{
    /* msg may be NULL -- there is nothing in it to read. */
    uint8_t buf[KILNLINK_GET_STACK_MARGIN_LEN];
    kilnlink_get_stack_margin_status_t status;
    size_t n = kilnlink_get_stack_margin_encode(NULL, buf, sizeof(buf), &status);
    CHECK(n == KILNLINK_GET_STACK_MARGIN_LEN, "encode(NULL msg) still writes 1 byte");
    CHECK(buf[0] == KILNLINK_GET_STACK_MARGIN_CMD, "encode(NULL msg) still writes the cmd byte");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_get_stack_margin_t req = {0};
    uint8_t buf[1];
    kilnlink_get_stack_margin_status_t status;
    size_t n = kilnlink_get_stack_margin_encode(&req, buf, 0, &status);
    CHECK(n == 0, "encode() into a zero-capacity buffer returns 0");
    CHECK(status == KILNLINK_GET_STACK_MARGIN_ERR_BUFFER_TOO_SMALL,
          "encode() into a zero-capacity buffer reports BUFFER_TOO_SMALL");
}

/* -- malformed input: length mismatch / wrong cmd -------------------------- */

static void test_decode_length_mismatch(void)
{
    uint8_t buf[2] = {KILNLINK_GET_STACK_MARGIN_CMD, 0x00};
    kilnlink_get_stack_margin_t out;
    CHECK(kilnlink_get_stack_margin_decode(buf, sizeof(buf), &out) ==
              KILNLINK_GET_STACK_MARGIN_ERR_LENGTH_MISMATCH,
          "decode() of a 2-byte buffer (expected 1) reports LENGTH_MISMATCH");

    CHECK(kilnlink_get_stack_margin_decode(buf, 0, &out) ==
              KILNLINK_GET_STACK_MARGIN_ERR_LENGTH_MISMATCH,
          "decode() of a 0-byte buffer reports LENGTH_MISMATCH");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_GET_STACK_MARGIN_LEN] = {0x00}; /* not 0x2B */
    kilnlink_get_stack_margin_t out;
    CHECK(kilnlink_get_stack_margin_decode(buf, sizeof(buf), &out) ==
              KILNLINK_GET_STACK_MARGIN_ERR_WRONG_CMD,
          "decode() of a frame with the wrong cmd byte reports WRONG_CMD");
}

int main(void)
{
    test_round_trip();
    test_encode_null_msg_ok();
    test_encode_buffer_too_small();
    test_decode_length_mismatch();
    test_decode_wrong_cmd();

    if (g_failures == 0) {
        printf("PASS: all test_get_stack_margin checks passed\n");
        return 0;
    }
    printf("FAIL: %d check(s) failed\n", g_failures);
    return 1;
}
