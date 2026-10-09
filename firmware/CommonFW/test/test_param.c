/* Host-native test for kilnlink_param.{c,h} -- the Pico->ESP
 * SAFETY_CMD_PARAM (0x1E) reply codec, docs/LINK_PROTOCOL.md sec 4/6,
 * docs/COMMISSIONING.md sec 2. Round-trip for found/not-found and every
 * KILNLINK_PARAM_TYPE_*, a byte-exact vector, and the hostile input set:
 * too-short, over-long, wrong command byte, bad found byte, bad type tag,
 * and a truncated-mid-value frame.
 */

/* Also proves the KILNLINK_PROTOCOL_VERSION 7 split from the request
 * (kilnlink_get_param.h's SAFETY_CMD_GET_PARAM, now 0x23): this reply's id
 * (0x1E, unchanged) and the request's id must be DIFFERENT, and this
 * decoder must REJECT a frame carrying the request's id.
 */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_get_param.h"
#include "kilnlink/kilnlink_param.h"

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

static void test_round_trip_found_f32(void)
{
    kilnlink_param_t msg = {0};
    msg.param_id = 9;
    msg.found = 1;
    msg.type = KILNLINK_PARAM_TYPE_F32;
    msg.value.f32_val = 85.0f; /* cj_max_c-scale value */

    uint8_t buf[KILNLINK_PARAM_MAX_LEN];
    kilnlink_param_status_t status;
    size_t n = kilnlink_param_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_PARAM_OK, "found/F32: encode OK");
    CHECK(n == KILNLINK_PARAM_HDR_LEN + 4u, "found/F32: header + 4 bytes");

    kilnlink_param_t decoded;
    CHECK(kilnlink_param_decode(buf, n, &decoded) == KILNLINK_PARAM_OK, "found/F32: decode OK");
    CHECK(decoded.param_id == 9, "found/F32: param_id round-trips");
    CHECK(decoded.found == 1, "found/F32: found round-trips");
    CHECK(decoded.type == KILNLINK_PARAM_TYPE_F32, "found/F32: type round-trips");
    CHECK(decoded.value.f32_val == 85.0f, "found/F32: value round-trips");
}

static void test_round_trip_not_found(void)
{
    /* An id this build doesn't recognise -- COMMISSIONING.md sec 2:
     * "unknown ids are refused individually and named in the reply". */
    kilnlink_param_t msg = {0};
    msg.param_id = 0xFFFF;
    msg.found = 0;

    uint8_t buf[KILNLINK_PARAM_MAX_LEN];
    kilnlink_param_status_t status;
    size_t n = kilnlink_param_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_PARAM_OK, "not-found: encode OK");
    CHECK(n == KILNLINK_PARAM_HDR_LEN, "not-found: exactly the header, no value bytes");

    kilnlink_param_t decoded;
    CHECK(kilnlink_param_decode(buf, n, &decoded) == KILNLINK_PARAM_OK, "not-found: decode OK");
    CHECK(decoded.param_id == 0xFFFF, "not-found: param_id round-trips");
    CHECK(decoded.found == 0, "not-found: found round-trips as 0");
}

static void test_round_trip_found_bool(void)
{
    kilnlink_param_t msg = {0};
    msg.param_id = 2;
    msg.found = 1;
    msg.type = KILNLINK_PARAM_TYPE_BOOL;
    msg.value.bool_val = 0;

    uint8_t buf[KILNLINK_PARAM_MAX_LEN];
    kilnlink_param_status_t status;
    size_t n = kilnlink_param_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_PARAM_OK, "found/BOOL: encode OK");
    CHECK(n == KILNLINK_PARAM_HDR_LEN + 1u, "found/BOOL: header + 1 byte");

    kilnlink_param_t decoded;
    CHECK(kilnlink_param_decode(buf, n, &decoded) == KILNLINK_PARAM_OK, "found/BOOL: decode OK");
    CHECK(decoded.value.bool_val == 0, "found/BOOL: value round-trips");
}

/* -- byte-exact vector ---------------------------------------------------- */

static void test_vector_not_found(void)
{
    static const uint8_t expected[] = {0x1e, 0xff, 0xff, 0x00, 0x00};
    kilnlink_param_t msg = {0};
    msg.param_id = 0xFFFF;
    msg.found = 0;

    uint8_t buf[KILNLINK_PARAM_MAX_LEN];
    kilnlink_param_status_t status;
    size_t n = kilnlink_param_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_PARAM_OK, "vector not-found: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector not-found: bytes match");
    } else {
        CHECK(1, "vector not-found: bytes match");
    }
}

/* -- decode hostile inputs ------------------------------------------------ */

static void test_decode_too_short(void)
{
    uint8_t buf[KILNLINK_PARAM_HDR_LEN - 1] = {0};
    buf[0] = KILNLINK_PARAM_CMD;
    kilnlink_param_t out;
    CHECK(kilnlink_param_decode(buf, sizeof(buf), &out) == KILNLINK_PARAM_ERR_LENGTH_MISMATCH,
          "decode() shorter than the header -> ERR_LENGTH_MISMATCH");
}

static void test_decode_not_found_over_long(void)
{
    /* found=0 must be exactly the header -- one trailing byte is rejected. */
    uint8_t buf[KILNLINK_PARAM_HDR_LEN + 1] = {
        KILNLINK_PARAM_CMD, 0x00, 0x00, 0x00, 0x00, 0xAA,
    };
    kilnlink_param_t out;
    CHECK(kilnlink_param_decode(buf, sizeof(buf), &out) == KILNLINK_PARAM_ERR_LENGTH_MISMATCH,
          "decode() found=0 with a trailing byte -> ERR_LENGTH_MISMATCH");
}

static void test_decode_truncated_mid_value(void)
{
    /* found=1, type=U16 (needs 2 bytes), only 1 supplied. */
    uint8_t buf[KILNLINK_PARAM_HDR_LEN + 1] = {
        KILNLINK_PARAM_CMD, 0x00, 0x00, 0x01, KILNLINK_PARAM_TYPE_U16, 0x10,
    };
    kilnlink_param_t out;
    CHECK(kilnlink_param_decode(buf, sizeof(buf), &out) == KILNLINK_PARAM_ERR_LENGTH_MISMATCH,
          "decode() found=1 truncated mid-U16-value -> ERR_LENGTH_MISMATCH");
}

static void test_decode_bad_type(void)
{
    uint8_t buf[KILNLINK_PARAM_HDR_LEN + 1] = {
        KILNLINK_PARAM_CMD, 0x00, 0x00, 0x01, 0x7Fu, 0x00,
    };
    kilnlink_param_t out;
    CHECK(kilnlink_param_decode(buf, sizeof(buf), &out) == KILNLINK_PARAM_ERR_BAD_TYPE,
          "decode() found=1 with an unrecognised type tag -> ERR_BAD_TYPE");
}

static void test_decode_bad_found(void)
{
    uint8_t buf[KILNLINK_PARAM_HDR_LEN] = {
        KILNLINK_PARAM_CMD, 0x00, 0x00, 0x02 /* neither 0 nor 1 */, 0x00,
    };
    kilnlink_param_t out;
    CHECK(kilnlink_param_decode(buf, sizeof(buf), &out) == KILNLINK_PARAM_ERR_BAD_FOUND,
          "decode() with found neither 0 nor 1 -> ERR_BAD_FOUND");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_PARAM_HDR_LEN] = {0x1Bu, 0x00, 0x00, 0x00, 0x00};
    kilnlink_param_t out;
    CHECK(kilnlink_param_decode(buf, sizeof(buf), &out) == KILNLINK_PARAM_ERR_WRONG_CMD,
          "decode() with wrong command byte -> ERR_WRONG_CMD");
}

static void test_encode_bad_type(void)
{
    kilnlink_param_t msg = {0};
    msg.found = 1;
    msg.type = 0x7Fu;
    uint8_t buf[KILNLINK_PARAM_MAX_LEN];
    kilnlink_param_status_t status;
    size_t n = kilnlink_param_encode(&msg, buf, sizeof(buf), &status);
    CHECK(n == 0, "encode() found=1 with a bad type writes nothing");
    CHECK(status == KILNLINK_PARAM_ERR_BAD_TYPE, "encode() found=1 with a bad type -> ERR_BAD_TYPE");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_param_t msg = {0};
    msg.found = 1;
    msg.type = KILNLINK_PARAM_TYPE_F32; /* needs 5 + 4 = 9 */
    uint8_t buf[5];
    kilnlink_param_status_t status;
    size_t n = kilnlink_param_encode(&msg, buf, sizeof(buf), &status);
    CHECK(n == 0, "encode() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_PARAM_ERR_BUFFER_TOO_SMALL,
          "encode() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
}

/* -- request/reply id separation (KILNLINK_PROTOCOL_VERSION 7) ----------- */

static void test_request_and_reply_ids_differ(void)
{
    CHECK(KILNLINK_PARAM_CMD != KILNLINK_GET_PARAM_CMD,
          "PARAM reply id and GET_PARAM request id must be different");
}

static void test_decode_rejects_request_id(void)
{
    uint8_t buf[KILNLINK_PARAM_HDR_LEN] = {
        KILNLINK_GET_PARAM_CMD, 0x00, 0x00, 0x00, 0x00,
    };
    kilnlink_param_t out;
    CHECK(kilnlink_param_decode(buf, sizeof(buf), &out) == KILNLINK_PARAM_ERR_WRONG_CMD,
          "PARAM decode() rejects a frame carrying GET_PARAM's (request) id");
}

int main(void)
{
    test_round_trip_found_f32();
    test_round_trip_not_found();
    test_round_trip_found_bool();
    test_vector_not_found();
    test_decode_too_short();
    test_decode_not_found_over_long();
    test_decode_truncated_mid_value();
    test_decode_bad_type();
    test_decode_bad_found();
    test_decode_wrong_cmd();
    test_encode_bad_type();
    test_encode_buffer_too_small();
    test_request_and_reply_ids_differ();
    test_decode_rejects_request_id();

    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
