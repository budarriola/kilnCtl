/* Host-native test for kilnlink_get_config_page.{c,h} -- the ESP->Pico
 * SAFETY_CMD_GET_CONFIG_PAGE (0x24) request codec, docs/LINK_PROTOCOL.md
 * sec 4, docs/COMMISSIONING.md sec 2. Fixed 2-byte request -- mirrors
 * test_get_ct_cal.c's structure: round-trip encode/decode, a byte-exact
 * vector, and the hostile input set: too-short, too-long (fixed-size
 * frame), wrong command byte.
 *
 * Also proves the KILNLINK_PROTOCOL_VERSION 7 split from the reply
 * (kilnlink_config_page.h's SAFETY_CMD_CONFIG_PAGE, still 0x1F): this
 * request's id and the reply's id must be DIFFERENT, and this decoder must
 * REJECT a frame carrying the reply's id.
 */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_config_page.h"
#include "kilnlink/kilnlink_get_config_page.h"

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
    kilnlink_get_config_page_t msg = {0};
    msg.page_index = 3;

    uint8_t buf[KILNLINK_GET_CONFIG_PAGE_LEN];
    kilnlink_get_config_page_status_t status;
    size_t n = kilnlink_get_config_page_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_GET_CONFIG_PAGE_OK, "encode() reports OK");
    CHECK(n == KILNLINK_GET_CONFIG_PAGE_LEN, "encode() always writes exactly 2 bytes");

    kilnlink_get_config_page_t decoded;
    CHECK(kilnlink_get_config_page_decode(buf, n, &decoded) == KILNLINK_GET_CONFIG_PAGE_OK,
          "decode() reports OK for a just-encoded payload");
    CHECK(decoded.page_index == 3, "page_index round-trips");
}

static void test_vector_page0(void)
{
    static const uint8_t expected[] = {0x24, 0x00};
    kilnlink_get_config_page_t msg = {0};
    msg.page_index = 0;

    uint8_t buf[KILNLINK_GET_CONFIG_PAGE_LEN];
    kilnlink_get_config_page_status_t status;
    size_t n = kilnlink_get_config_page_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_GET_CONFIG_PAGE_OK, "vector page0: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector page0: bytes match");
    } else {
        CHECK(1, "vector page0: bytes match");
    }
}

static void test_decode_too_short(void)
{
    uint8_t buf[KILNLINK_GET_CONFIG_PAGE_LEN - 1] = {0};
    buf[0] = KILNLINK_GET_CONFIG_PAGE_CMD;
    kilnlink_get_config_page_t out;
    CHECK(kilnlink_get_config_page_decode(buf, sizeof(buf), &out) ==
              KILNLINK_GET_CONFIG_PAGE_ERR_LENGTH_MISMATCH,
          "decode() of a 1-byte (one short) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_too_long(void)
{
    uint8_t buf[KILNLINK_GET_CONFIG_PAGE_LEN + 1] = {0};
    buf[0] = KILNLINK_GET_CONFIG_PAGE_CMD;
    kilnlink_get_config_page_t out;
    CHECK(kilnlink_get_config_page_decode(buf, sizeof(buf), &out) ==
              KILNLINK_GET_CONFIG_PAGE_ERR_LENGTH_MISMATCH,
          "decode() of a 3-byte (one too many) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_GET_CONFIG_PAGE_LEN] = {0};
    buf[0] = 0x1Eu; /* GET_PARAM's id, not GET_CONFIG_PAGE's */
    kilnlink_get_config_page_t out;
    CHECK(kilnlink_get_config_page_decode(buf, sizeof(buf), &out) ==
              KILNLINK_GET_CONFIG_PAGE_ERR_WRONG_CMD,
          "decode() with wrong command byte -> ERR_WRONG_CMD");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_get_config_page_t msg = {0};
    uint8_t buf[1]; /* needs 2 */
    kilnlink_get_config_page_status_t status;
    size_t n = kilnlink_get_config_page_encode(&msg, buf, sizeof(buf), &status);
    CHECK(n == 0, "encode() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_GET_CONFIG_PAGE_ERR_BUFFER_TOO_SMALL,
          "encode() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
}

/* -- request/reply id separation (KILNLINK_PROTOCOL_VERSION 7) ----------- */

static void test_request_and_reply_ids_differ(void)
{
    CHECK(KILNLINK_GET_CONFIG_PAGE_CMD != KILNLINK_CONFIG_PAGE_CMD,
          "GET_CONFIG_PAGE request id and CONFIG_PAGE reply id must be different");
}

static void test_decode_rejects_reply_id(void)
{
    uint8_t buf[KILNLINK_GET_CONFIG_PAGE_LEN] = {0};
    buf[0] = KILNLINK_CONFIG_PAGE_CMD;
    kilnlink_get_config_page_t out;
    CHECK(kilnlink_get_config_page_decode(buf, sizeof(buf), &out) ==
              KILNLINK_GET_CONFIG_PAGE_ERR_WRONG_CMD,
          "GET_CONFIG_PAGE decode() rejects a frame carrying CONFIG_PAGE's (reply) id");
}

int main(void)
{
    test_round_trip();
    test_vector_page0();
    test_decode_too_short();
    test_decode_too_long();
    test_decode_wrong_cmd();
    test_encode_buffer_too_small();
    test_request_and_reply_ids_differ();
    test_decode_rejects_reply_id();

    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
