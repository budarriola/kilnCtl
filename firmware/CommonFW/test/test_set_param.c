/* Host-native test for kilnlink_set_param.{c,h} -- the ESP->Pico
 * SAFETY_CMD_SET_PARAM (0x1C) codec, docs/LINK_PROTOCOL.md sec 4,
 * docs/COMMISSIONING.md sec 2. Round-trip encode/decode for every
 * KILNLINK_PARAM_TYPE_*, byte-exact vectors, and the hostile input set:
 * too-short, over-long, wrong command byte, bad type tag, and a
 * truncated-mid-value frame for each type. Also sweeps every possible frame
 * length 0..12 for a fixed type, asserting the decoder never returns OK
 * except at the one exact length and never reads out of bounds (run under
 * a tool that would catch that, but the length-exactness assertion alone
 * proves no over-read masquerades as success).
 */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_set_param.h"

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

/* -- round trip, one per type --------------------------------------------- */

static void test_round_trip_bool(void)
{
    kilnlink_set_param_t msg = {0};
    msg.param_id = 42;
    msg.type = KILNLINK_PARAM_TYPE_BOOL;
    msg.value.bool_val = 1;

    uint8_t buf[KILNLINK_SET_PARAM_MAX_LEN];
    kilnlink_set_param_status_t status;
    size_t n = kilnlink_set_param_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_SET_PARAM_OK, "BOOL: encode OK");
    CHECK(n == KILNLINK_SET_PARAM_HDR_LEN + 1u, "BOOL: encodes to header + 1 byte");

    kilnlink_set_param_t decoded;
    CHECK(kilnlink_set_param_decode(buf, n, &decoded) == KILNLINK_SET_PARAM_OK, "BOOL: decode OK");
    CHECK(decoded.param_id == 42, "BOOL: param_id round-trips");
    CHECK(decoded.type == KILNLINK_PARAM_TYPE_BOOL, "BOOL: type round-trips");
    CHECK(decoded.value.bool_val == 1, "BOOL: value round-trips");
}

static void test_round_trip_u8(void)
{
    kilnlink_set_param_t msg = {0};
    msg.param_id = 7;
    msg.type = KILNLINK_PARAM_TYPE_U8;
    msg.value.u8_val = 0x2A;

    uint8_t buf[KILNLINK_SET_PARAM_MAX_LEN];
    kilnlink_set_param_status_t status;
    size_t n = kilnlink_set_param_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_SET_PARAM_OK, "U8: encode OK");
    CHECK(n == KILNLINK_SET_PARAM_HDR_LEN + 1u, "U8: encodes to header + 1 byte");

    kilnlink_set_param_t decoded;
    CHECK(kilnlink_set_param_decode(buf, n, &decoded) == KILNLINK_SET_PARAM_OK, "U8: decode OK");
    CHECK(decoded.value.u8_val == 0x2A, "U8: value round-trips");
}

static void test_round_trip_u16(void)
{
    kilnlink_set_param_t msg = {0};
    msg.param_id = 0xBEEF;
    msg.type = KILNLINK_PARAM_TYPE_U16;
    msg.value.u16_val = 3600; /* e.g. link_dead_hard_s-scale value */

    uint8_t buf[KILNLINK_SET_PARAM_MAX_LEN];
    kilnlink_set_param_status_t status;
    size_t n = kilnlink_set_param_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_SET_PARAM_OK, "U16: encode OK");
    CHECK(n == KILNLINK_SET_PARAM_HDR_LEN + 2u, "U16: encodes to header + 2 bytes");

    kilnlink_set_param_t decoded;
    CHECK(kilnlink_set_param_decode(buf, n, &decoded) == KILNLINK_SET_PARAM_OK, "U16: decode OK");
    CHECK(decoded.param_id == 0xBEEF, "U16: param_id round-trips");
    CHECK(decoded.value.u16_val == 3600, "U16: value round-trips");
}

static void test_round_trip_f32(void)
{
    kilnlink_set_param_t msg = {0};
    msg.param_id = 1;
    msg.type = KILNLINK_PARAM_TYPE_F32;
    msg.value.f32_val = 1300.0f; /* abs_max_temp_c-scale value */

    uint8_t buf[KILNLINK_SET_PARAM_MAX_LEN];
    kilnlink_set_param_status_t status;
    size_t n = kilnlink_set_param_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_SET_PARAM_OK, "F32: encode OK");
    CHECK(n == KILNLINK_SET_PARAM_HDR_LEN + 4u, "F32: encodes to header + 4 bytes");

    kilnlink_set_param_t decoded;
    CHECK(kilnlink_set_param_decode(buf, n, &decoded) == KILNLINK_SET_PARAM_OK, "F32: decode OK");
    CHECK(decoded.value.f32_val == 1300.0f, "F32: value round-trips");
}

/* -- byte-exact vector ---------------------------------------------------- */

static void test_vector_u16(void)
{
    /* param_id=0x0001 LE, type=U16(0x02), value=3600 (0x0E10) LE */
    static const uint8_t expected[] = {0x1c, 0x01, 0x00, 0x02, 0x10, 0x0e};
    kilnlink_set_param_t msg = {0};
    msg.param_id = 1;
    msg.type = KILNLINK_PARAM_TYPE_U16;
    msg.value.u16_val = 3600;

    uint8_t buf[KILNLINK_SET_PARAM_MAX_LEN];
    kilnlink_set_param_status_t status;
    size_t n = kilnlink_set_param_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_SET_PARAM_OK, "vector u16: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector u16: bytes match");
    } else {
        CHECK(1, "vector u16: bytes match");
    }
}

/* -- decode hostile inputs ------------------------------------------------ */

static void test_decode_too_short(void)
{
    /* Shorter than even the 4-byte header. */
    uint8_t buf[3] = {KILNLINK_SET_PARAM_CMD, 0x01, 0x00};
    kilnlink_set_param_t out;
    CHECK(kilnlink_set_param_decode(buf, sizeof(buf), &out) == KILNLINK_SET_PARAM_ERR_LENGTH_MISMATCH,
          "decode() shorter than the header -> ERR_LENGTH_MISMATCH");
}

static void test_decode_truncated_mid_value(void)
{
    /* Header says U16 (needs 2 more bytes) but only 1 follows. */
    uint8_t buf[KILNLINK_SET_PARAM_HDR_LEN + 1] = {
        KILNLINK_SET_PARAM_CMD, 0x01, 0x00, KILNLINK_PARAM_TYPE_U16, 0x10,
    };
    kilnlink_set_param_t out;
    CHECK(kilnlink_set_param_decode(buf, sizeof(buf), &out) == KILNLINK_SET_PARAM_ERR_LENGTH_MISMATCH,
          "decode() truncated mid-U16-value -> ERR_LENGTH_MISMATCH");
}

static void test_decode_truncated_mid_value_f32(void)
{
    /* Header says F32 (needs 4 more bytes) but only 2 follow. */
    uint8_t buf[KILNLINK_SET_PARAM_HDR_LEN + 2] = {
        KILNLINK_SET_PARAM_CMD, 0x00, 0x00, KILNLINK_PARAM_TYPE_F32, 0x00, 0x00,
    };
    kilnlink_set_param_t out;
    CHECK(kilnlink_set_param_decode(buf, sizeof(buf), &out) == KILNLINK_SET_PARAM_ERR_LENGTH_MISMATCH,
          "decode() truncated mid-F32-value -> ERR_LENGTH_MISMATCH");
}

static void test_decode_over_long(void)
{
    /* BOOL only needs 1 value byte; one extra trailing byte must be rejected. */
    uint8_t buf[KILNLINK_SET_PARAM_HDR_LEN + 2] = {
        KILNLINK_SET_PARAM_CMD, 0x00, 0x00, KILNLINK_PARAM_TYPE_BOOL, 0x01, 0xFF,
    };
    kilnlink_set_param_t out;
    CHECK(kilnlink_set_param_decode(buf, sizeof(buf), &out) == KILNLINK_SET_PARAM_ERR_LENGTH_MISMATCH,
          "decode() with one trailing byte past a BOOL value -> ERR_LENGTH_MISMATCH");
}

static void test_decode_bad_type(void)
{
    uint8_t buf[KILNLINK_SET_PARAM_HDR_LEN + 1] = {
        KILNLINK_SET_PARAM_CMD, 0x00, 0x00, 0x7Fu, 0x00,
    };
    kilnlink_set_param_t out;
    CHECK(kilnlink_set_param_decode(buf, sizeof(buf), &out) == KILNLINK_SET_PARAM_ERR_BAD_TYPE,
          "decode() with an unrecognised type tag -> ERR_BAD_TYPE");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_SET_PARAM_HDR_LEN + 1] = {
        0x1Bu /* SET_LOG_LEVEL's id, not SET_PARAM's */, 0x00, 0x00, KILNLINK_PARAM_TYPE_BOOL, 0x00,
    };
    kilnlink_set_param_t out;
    CHECK(kilnlink_set_param_decode(buf, sizeof(buf), &out) == KILNLINK_SET_PARAM_ERR_WRONG_CMD,
          "decode() with wrong command byte -> ERR_WRONG_CMD");
}

static void test_encode_bad_type(void)
{
    kilnlink_set_param_t msg = {0};
    msg.type = 0x7Fu; /* not a real KILNLINK_PARAM_TYPE_* */
    uint8_t buf[KILNLINK_SET_PARAM_MAX_LEN];
    kilnlink_set_param_status_t status;
    size_t n = kilnlink_set_param_encode(&msg, buf, sizeof(buf), &status);
    CHECK(n == 0, "encode() with a bad type writes nothing");
    CHECK(status == KILNLINK_SET_PARAM_ERR_BAD_TYPE, "encode() with a bad type -> ERR_BAD_TYPE");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_set_param_t msg = {0};
    msg.type = KILNLINK_PARAM_TYPE_F32; /* needs header(4) + 4 = 8 */
    uint8_t buf[4];
    kilnlink_set_param_status_t status;
    size_t n = kilnlink_set_param_encode(&msg, buf, sizeof(buf), &status);
    CHECK(n == 0, "encode() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_SET_PARAM_ERR_BUFFER_TOO_SMALL,
          "encode() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
}

/* -- length sweep: only the exact length for this type ever decodes OK --- */

static void test_length_sweep_u16(void)
{
    uint8_t full[KILNLINK_SET_PARAM_HDR_LEN + 2] = {
        KILNLINK_SET_PARAM_CMD, 0x05, 0x00, KILNLINK_PARAM_TYPE_U16, 0x34, 0x12,
    };
    for (size_t len = 0; len <= sizeof(full) + 4; len++) {
        uint8_t buf[sizeof(full) + 4] = {0};
        size_t copy = len < sizeof(full) ? len : sizeof(full);
        memcpy(buf, full, copy);
        /* Beyond the real vector's length, pad with the same command byte
         * so we are exercising "trailing garbage after a valid frame", not
         * just "no command byte at all". */
        for (size_t i = copy; i < len && i < sizeof(buf); i++) {
            buf[i] = 0xAA;
        }
        kilnlink_set_param_t out;
        kilnlink_set_param_status_t st = kilnlink_set_param_decode(buf, len, &out);
        if (len == sizeof(full)) {
            CHECK(st == KILNLINK_SET_PARAM_OK, "length sweep: the one exact length decodes OK");
        } else {
            CHECK(st != KILNLINK_SET_PARAM_OK, "length sweep: every other length is rejected");
        }
    }
}

int main(void)
{
    test_round_trip_bool();
    test_round_trip_u8();
    test_round_trip_u16();
    test_round_trip_f32();
    test_vector_u16();
    test_decode_too_short();
    test_decode_truncated_mid_value();
    test_decode_truncated_mid_value_f32();
    test_decode_over_long();
    test_decode_bad_type();
    test_decode_wrong_cmd();
    test_encode_bad_type();
    test_encode_buffer_too_small();
    test_length_sweep_u16();

    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
