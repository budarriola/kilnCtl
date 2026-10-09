/* Host-native test for kilnlink_param_value.{c,h} -- the shared type-tag +
 * value codec used by SET_PARAM/GET_PARAM/PARAM/CONFIG_PAGE
 * (docs/COMMISSIONING.md sec 2). Exercises every KILNLINK_PARAM_TYPE_* tag's
 * round trip and, critically, every unrecognised tag byte: a fuzz-ish sweep
 * of all 256 possible type bytes asserting exactly the 4 known tags report a
 * length and every other byte reports 0 (the bad-type sentinel every caller
 * relies on).
 */

#include <stdio.h>

#include "kilnlink/kilnlink_param_value.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);          \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

static void test_lengths(void)
{
    CHECK(kilnlink_param_value_len(KILNLINK_PARAM_TYPE_BOOL) == 1u, "BOOL is 1 byte");
    CHECK(kilnlink_param_value_len(KILNLINK_PARAM_TYPE_U8) == 1u, "U8 is 1 byte");
    CHECK(kilnlink_param_value_len(KILNLINK_PARAM_TYPE_U16) == 2u, "U16 is 2 bytes");
    CHECK(kilnlink_param_value_len(KILNLINK_PARAM_TYPE_F32) == 4u, "F32 is 4 bytes");
}

/* Every byte value NOT one of the 4 known tags must report length 0 --
 * swept exhaustively rather than spot-checked, since this sentinel is what
 * every codec built on top of this file uses to detect a bad type tag. */
static void test_bad_type_sweep(void)
{
    unsigned known = 0;
    for (unsigned t = 0; t <= 0xFF; t++) {
        size_t len = kilnlink_param_value_len((uint8_t)t);
        if (t == KILNLINK_PARAM_TYPE_BOOL || t == KILNLINK_PARAM_TYPE_U8 ||
            t == KILNLINK_PARAM_TYPE_U16 || t == KILNLINK_PARAM_TYPE_F32) {
            CHECK(len != 0, "a known type tag reports a nonzero length");
            known++;
        } else {
            CHECK(len == 0, "an unknown type tag reports length 0");
        }
    }
    CHECK(known == 4, "exactly 4 type tags are recognised");
}

static void test_round_trip_bool(void)
{
    kilnlink_param_value_t v;
    v.bool_val = 1;
    uint8_t buf[8] = {0};
    CHECK(kilnlink_param_value_encode(KILNLINK_PARAM_TYPE_BOOL, &v, buf, 2), "encode BOOL OK");
    CHECK(buf[2] == 1, "BOOL byte matches");

    kilnlink_param_value_t decoded;
    CHECK(kilnlink_param_value_decode(KILNLINK_PARAM_TYPE_BOOL, buf, 2, &decoded), "decode BOOL OK");
    CHECK(decoded.bool_val == 1, "BOOL round-trips");
}

static void test_round_trip_u8(void)
{
    kilnlink_param_value_t v;
    v.u8_val = 0xAB;
    uint8_t buf[8] = {0};
    CHECK(kilnlink_param_value_encode(KILNLINK_PARAM_TYPE_U8, &v, buf, 0), "encode U8 OK");

    kilnlink_param_value_t decoded;
    CHECK(kilnlink_param_value_decode(KILNLINK_PARAM_TYPE_U8, buf, 0, &decoded), "decode U8 OK");
    CHECK(decoded.u8_val == 0xAB, "U8 round-trips");
}

static void test_round_trip_u16(void)
{
    kilnlink_param_value_t v;
    v.u16_val = 12345;
    uint8_t buf[8] = {0};
    CHECK(kilnlink_param_value_encode(KILNLINK_PARAM_TYPE_U16, &v, buf, 1), "encode U16 OK");

    kilnlink_param_value_t decoded;
    CHECK(kilnlink_param_value_decode(KILNLINK_PARAM_TYPE_U16, buf, 1, &decoded), "decode U16 OK");
    CHECK(decoded.u16_val == 12345, "U16 round-trips");
}

static void test_round_trip_f32(void)
{
    kilnlink_param_value_t v;
    v.f32_val = -1234.5f;
    uint8_t buf[8] = {0};
    CHECK(kilnlink_param_value_encode(KILNLINK_PARAM_TYPE_F32, &v, buf, 3), "encode F32 OK");

    kilnlink_param_value_t decoded;
    CHECK(kilnlink_param_value_decode(KILNLINK_PARAM_TYPE_F32, buf, 3, &decoded), "decode F32 OK");
    CHECK(decoded.f32_val == v.f32_val, "F32 round-trips");
}

static void test_encode_decode_bad_type_rejected(void)
{
    kilnlink_param_value_t v = {0};
    uint8_t buf[8] = {0};
    CHECK(!kilnlink_param_value_encode(0x7Fu, &v, buf, 0), "encode() refuses an unknown type tag");

    kilnlink_param_value_t decoded;
    CHECK(!kilnlink_param_value_decode(0x7Fu, buf, 0, &decoded), "decode() refuses an unknown type tag");
}

int main(void)
{
    test_lengths();
    test_bad_type_sweep();
    test_round_trip_bool();
    test_round_trip_u8();
    test_round_trip_u16();
    test_round_trip_f32();
    test_encode_decode_bad_type_rejected();

    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
