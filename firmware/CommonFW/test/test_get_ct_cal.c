/* Host-native test for kilnlink_get_ct_cal.{c,h} -- the ESP->Pico
 * SAFETY_CMD_GET_CT_CAL (0x22) codec, docs/LINK_PROTOCOL.md sec 4. One byte,
 * no arguments -- mirrors test_get_fw_version.c's structure: round-trip
 * encode/decode, a byte-exact vector, and the hostile input set: too-short,
 * too-long (fixed-size frame), wrong command byte.
 *
 * Also proves the KILNLINK_PROTOCOL_VERSION 7 split from the reply
 * (kilnlink_ct_cal.h's SAFETY_CMD_CT_CAL): this request's id (0x22) and the
 * reply's id (0x1A) must be DIFFERENT, and this decoder must REJECT a frame
 * carrying the reply's old id -- see test_decode_rejects_reply_id().
 */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_ct_cal.h"
#include "kilnlink/kilnlink_get_ct_cal.h"

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
    kilnlink_get_ct_cal_t msg = {0};

    uint8_t buf[KILNLINK_GET_CT_CAL_LEN];
    kilnlink_get_ct_cal_status_t status;
    size_t n = kilnlink_get_ct_cal_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_GET_CT_CAL_OK, "encode() reports OK");
    CHECK(n == KILNLINK_GET_CT_CAL_LEN, "encode() always writes exactly 1 byte");
    CHECK(buf[0] == KILNLINK_GET_CT_CAL_CMD, "encoded byte is the command id");

    kilnlink_get_ct_cal_t decoded;
    kilnlink_get_ct_cal_status_t dstatus = kilnlink_get_ct_cal_decode(buf, n, &decoded);
    CHECK(dstatus == KILNLINK_GET_CT_CAL_OK, "decode() reports OK for a just-encoded payload");
}

static void test_encode_null_msg(void)
{
    uint8_t buf[KILNLINK_GET_CT_CAL_LEN];
    kilnlink_get_ct_cal_status_t status;
    size_t n = kilnlink_get_ct_cal_encode(NULL, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_GET_CT_CAL_OK, "encode(NULL) reports OK");
    CHECK(n == KILNLINK_GET_CT_CAL_LEN, "encode(NULL) still writes 1 byte");
}

static void test_decode_null_out(void)
{
    uint8_t buf[KILNLINK_GET_CT_CAL_LEN] = {KILNLINK_GET_CT_CAL_CMD};
    CHECK(kilnlink_get_ct_cal_decode(buf, sizeof(buf), NULL) == KILNLINK_GET_CT_CAL_OK,
          "decode(..., NULL) still reports OK -- nothing to fill in");
}

/* -- byte-exact vector ---------------------------------------------------- */

static void test_vector_request(void)
{
    static const uint8_t expected[] = {0x22};
    kilnlink_get_ct_cal_t msg = {0};

    uint8_t buf[KILNLINK_GET_CT_CAL_LEN];
    kilnlink_get_ct_cal_status_t status;
    size_t n = kilnlink_get_ct_cal_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_GET_CT_CAL_OK, "vector request: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector request: bytes match {0x22}");
    } else {
        CHECK(1, "vector request: bytes match {0x22}");
    }
}

/* -- decode hostile inputs ------------------------------------------------ */

static void test_decode_too_short(void)
{
    uint8_t buf[1];
    kilnlink_get_ct_cal_t out;
    CHECK(kilnlink_get_ct_cal_decode(buf, 0, &out) == KILNLINK_GET_CT_CAL_ERR_LENGTH_MISMATCH,
          "decode() of a 0-byte payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_too_long(void)
{
    uint8_t buf[KILNLINK_GET_CT_CAL_LEN + 1] = {0};
    buf[0] = KILNLINK_GET_CT_CAL_CMD;
    kilnlink_get_ct_cal_t out;
    CHECK(kilnlink_get_ct_cal_decode(buf, sizeof(buf), &out) == KILNLINK_GET_CT_CAL_ERR_LENGTH_MISMATCH,
          "decode() of a 2-byte (one too many) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_GET_CT_CAL_LEN] = {0};
    buf[0] = 0x0B; /* GET_FW_VERSION's id, not GET_CT_CAL's */
    kilnlink_get_ct_cal_t out;
    CHECK(kilnlink_get_ct_cal_decode(buf, sizeof(buf), &out) == KILNLINK_GET_CT_CAL_ERR_WRONG_CMD,
          "decode() with wrong command byte -> ERR_WRONG_CMD");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_get_ct_cal_t msg = {0};
    uint8_t buf[1];
    kilnlink_get_ct_cal_status_t status;
    size_t n = kilnlink_get_ct_cal_encode(&msg, buf, 0, &status);
    CHECK(n == 0, "encode() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_GET_CT_CAL_ERR_BUFFER_TOO_SMALL,
          "encode() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
}

/* -- request/reply id separation (KILNLINK_PROTOCOL_VERSION 7) ----------- */

static void test_request_and_reply_ids_differ(void)
{
    CHECK(KILNLINK_GET_CT_CAL_CMD != KILNLINK_CT_CAL_CMD,
          "GET_CT_CAL request id and CT_CAL reply id must be different");
}

static void test_decode_rejects_reply_id(void)
{
    /* A frame carrying the reply's id (0x1A) must be rejected by the
     * REQUEST decoder as a wrong command, not silently accepted -- the
     * whole point of separating the ids is that length is no longer the
     * only thing telling these two apart. */
    uint8_t buf[KILNLINK_GET_CT_CAL_LEN] = {0};
    buf[0] = KILNLINK_CT_CAL_CMD;
    kilnlink_get_ct_cal_t out;
    CHECK(kilnlink_get_ct_cal_decode(buf, sizeof(buf), &out) == KILNLINK_GET_CT_CAL_ERR_WRONG_CMD,
          "GET_CT_CAL decode() rejects a frame carrying CT_CAL's (reply) id");
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
    test_request_and_reply_ids_differ();
    test_decode_rejects_reply_id();

    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
