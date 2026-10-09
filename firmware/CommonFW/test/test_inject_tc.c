/* Host-native test for kilnlink_inject_tc.{c,h} -- the ESP->Pico
 * SAFETY_CMD_INJECT_TC (0x21) codec, docs/LINK_PROTOCOL.md sec 4. Mirrors
 * test_set_config.c's structure (fixed-length ESP->Pico command) and
 * test_ct_cal.c's float-field discipline (bit-exact round trip, NaN/Inf
 * survive unmolested): round-trip encode/decode, a byte-exact vector, and
 * the hostile input set: too-short, too-long (fixed-size frame), wrong
 * command byte, undersized encode buffer.
 *
 * This codec was the only payload codec in CommonFW with no host test at
 * all before this file -- see CommonFW/README.md and kilnlink_inject_tc.h's
 * own doc comment for why that mattered: SAFETY_CMD_INJECT_TC feeds a
 * synthetic thermocouple reading straight into SaftyFW's guard chain
 * (S1/S5/S11/S12) via thermo_task_inject_reading(). The *gating* (refused
 * unless safety_tc_installed == 0) lives entirely on the receiving side and
 * is out of scope here -- this file only proves the wire codec itself:
 * every field round-trips exactly, hostile lengths/command bytes are
 * refused, and -- the point of test_valid_zero_carries_raw_bytes below --
 * the codec does not editorialize about `tc_c`/`cj_c` when `valid == 0`.
 * That is the receiver's job (thermo_task_inject_reading() substitutes NaN
 * itself), not this codec's.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_inject_tc.h"

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

static uint32_t f32_bits(float v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return bits;
}

/* -- round trip --------------------------------------------------------- */

static void test_round_trip_valid_reading(void)
{
    kilnlink_inject_tc_t msg = {0};
    msg.valid = 1;
    msg.tc_c = 875.5f;
    msg.cj_c = 23.75f;
    msg.fault_bits = 0x05u;

    uint8_t buf[KILNLINK_INJECT_TC_LEN];
    kilnlink_inject_tc_status_t status;
    size_t n = kilnlink_inject_tc_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_INJECT_TC_OK, "encode() reports OK");
    CHECK(n == KILNLINK_INJECT_TC_LEN, "encode() always writes exactly 11 bytes");
    CHECK(buf[0] == KILNLINK_INJECT_TC_CMD, "byte 0 is the command id");

    kilnlink_inject_tc_t decoded;
    kilnlink_inject_tc_status_t dstatus = kilnlink_inject_tc_decode(buf, n, &decoded);
    CHECK(dstatus == KILNLINK_INJECT_TC_OK, "decode() reports OK for a just-encoded payload");
    CHECK(decoded.valid == msg.valid, "decoded valid matches");
    CHECK(decoded.tc_c == msg.tc_c, "decoded tc_c matches");
    CHECK(decoded.cj_c == msg.cj_c, "decoded cj_c matches");
    CHECK(decoded.fault_bits == msg.fault_bits, "decoded fault_bits matches");
}

static void test_valid_zero_carries_raw_bytes(void)
{
    /* valid == 0 means "inject a bad read"; tc_c/cj_c are garbage on the
     * wire (not omitted -- this is a fixed-size frame). The receiver
     * (thermo_task_inject_reading()) is the one that substitutes NaN --
     * this codec must not "helpfully" zero or fix up these fields itself.
     * Use a non-zero, non-NaN, easily-distinguished bit pattern so a
     * decoder that silently clamps to 0.0 (an editorializing bug) would be
     * caught by this test. */
    kilnlink_inject_tc_t msg = {0};
    msg.valid = 0;
    msg.tc_c = -4096.25f;   /* deliberately "garbage": no receiver should trust this */
    msg.cj_c = 123456.75f;
    msg.fault_bits = 0xFFu;

    uint8_t buf[KILNLINK_INJECT_TC_LEN];
    kilnlink_inject_tc_status_t status;
    size_t n = kilnlink_inject_tc_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_INJECT_TC_OK, "valid=0: encode() reports OK");

    kilnlink_inject_tc_t decoded;
    CHECK(kilnlink_inject_tc_decode(buf, n, &decoded) == KILNLINK_INJECT_TC_OK,
          "valid=0: decode() reports OK -- a bad reading is still a well-formed frame");
    CHECK(decoded.valid == 0, "valid=0: round-trips as 0");
    CHECK(decoded.tc_c == msg.tc_c,
          "valid=0: tc_c round-trips byte-for-byte -- the codec does not editorialize");
    CHECK(decoded.cj_c == msg.cj_c,
          "valid=0: cj_c round-trips byte-for-byte -- the codec does not editorialize");
    CHECK(decoded.fault_bits == msg.fault_bits, "valid=0: fault_bits round-trips");
}

static void test_round_trip_nan_inf(void)
{
    /* NaN/Inf must survive bit-for-bit -- compare raw bits, not == (NaN !=
     * NaN by IEEE754, so a naive == check here would be a vacuous pass no
     * matter what the codec does). */
    kilnlink_inject_tc_t msg = {0};
    msg.valid = 1;
    msg.tc_c = (float)NAN;
    msg.cj_c = (float)INFINITY;
    msg.fault_bits = 0x00u;

    uint8_t buf[KILNLINK_INJECT_TC_LEN];
    kilnlink_inject_tc_status_t status;
    size_t n = kilnlink_inject_tc_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_INJECT_TC_OK, "nan/inf: encode() reports OK");

    kilnlink_inject_tc_t decoded;
    CHECK(kilnlink_inject_tc_decode(buf, n, &decoded) == KILNLINK_INJECT_TC_OK, "nan/inf: decode() reports OK");
    CHECK(f32_bits(decoded.tc_c) == f32_bits(msg.tc_c), "nan/inf: tc_c (NaN) round-trips bit-for-bit");
    CHECK(isnan(decoded.tc_c), "nan/inf: tc_c still reads as NaN after round trip");
    CHECK(f32_bits(decoded.cj_c) == f32_bits(msg.cj_c), "nan/inf: cj_c (+Inf) round-trips bit-for-bit");
    CHECK(isinf(decoded.cj_c) && decoded.cj_c > 0.0f, "nan/inf: cj_c still reads as +Inf after round trip");

    /* Negative infinity too, in a second pass -- proves the sign bit isn't
     * lost anywhere in the LE packing. */
    msg.tc_c = -(float)INFINITY;
    n = kilnlink_inject_tc_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_INJECT_TC_OK, "-inf: encode() reports OK");
    CHECK(kilnlink_inject_tc_decode(buf, n, &decoded) == KILNLINK_INJECT_TC_OK, "-inf: decode() reports OK");
    CHECK(f32_bits(decoded.tc_c) == f32_bits(msg.tc_c), "-inf: tc_c round-trips bit-for-bit");
    CHECK(isinf(decoded.tc_c) && decoded.tc_c < 0.0f, "-inf: tc_c still reads as -Inf after round trip");
}

/* -- byte-exact vector ---------------------------------------------------- */

static void test_vector_valid_reading(void)
{
    /* cmd(0x21) valid(1) tc_c=875.5f LE cj_c=23.75f LE fault_bits(0x05) */
    static const uint8_t expected[] = {
        0x21, 0x01, 0x00, 0xe0, 0x5a, 0x44, 0x00, 0x00, 0xbe, 0x41, 0x05,
    };
    kilnlink_inject_tc_t msg = {0};
    msg.valid = 1;
    msg.tc_c = 875.5f;
    msg.cj_c = 23.75f;
    msg.fault_bits = 0x05u;

    uint8_t buf[KILNLINK_INJECT_TC_LEN];
    kilnlink_inject_tc_status_t status;
    size_t n = kilnlink_inject_tc_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_INJECT_TC_OK, "vector valid_reading: encode OK");
    CHECK(sizeof(expected) == KILNLINK_INJECT_TC_LEN, "vector valid_reading: expected[] sized correctly");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector valid_reading: bytes match");
    } else {
        CHECK(1, "vector valid_reading: bytes match");
    }
}

/* -- decode hostile inputs ------------------------------------------------ */

static void test_decode_too_short(void)
{
    uint8_t buf[KILNLINK_INJECT_TC_LEN - 1] = {0};
    buf[0] = KILNLINK_INJECT_TC_CMD;
    kilnlink_inject_tc_t out;
    CHECK(kilnlink_inject_tc_decode(buf, sizeof(buf), &out) == KILNLINK_INJECT_TC_ERR_LENGTH_MISMATCH,
          "decode() of a 10-byte (one short) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_too_long(void)
{
    /* Fixed-size frame, not a minimum size -- one byte too many is exactly
     * as invalid as one byte too few. */
    uint8_t buf[KILNLINK_INJECT_TC_LEN + 1] = {0};
    buf[0] = KILNLINK_INJECT_TC_CMD;
    kilnlink_inject_tc_t out;
    CHECK(kilnlink_inject_tc_decode(buf, sizeof(buf), &out) == KILNLINK_INJECT_TC_ERR_LENGTH_MISMATCH,
          "decode() of a 12-byte (one too many) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_INJECT_TC_LEN] = {0};
    buf[0] = 0x20; /* COMMIT_CONFIG_REJECTED's id, not INJECT_TC's */
    kilnlink_inject_tc_t out;
    CHECK(kilnlink_inject_tc_decode(buf, sizeof(buf), &out) == KILNLINK_INJECT_TC_ERR_WRONG_CMD,
          "decode() with wrong command byte -> ERR_WRONG_CMD");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_inject_tc_t msg = {0};
    uint8_t buf[4]; /* needs 11 */
    kilnlink_inject_tc_status_t status;
    size_t n = kilnlink_inject_tc_encode(&msg, buf, sizeof(buf), &status);
    CHECK(n == 0, "encode() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_INJECT_TC_ERR_BUFFER_TOO_SMALL,
          "encode() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
}

static void test_encode_null_buffer(void)
{
    kilnlink_inject_tc_t msg = {0};
    kilnlink_inject_tc_status_t status;
    /* out may be NULL as long as out_cap (0) is also too small -- encode()
     * must check the capacity before ever touching `out`. */
    size_t n = kilnlink_inject_tc_encode(&msg, NULL, 0, &status);
    CHECK(n == 0, "encode() with a NULL output buffer and 0 capacity writes nothing");
    CHECK(status == KILNLINK_INJECT_TC_ERR_BUFFER_TOO_SMALL,
          "encode() with a NULL output buffer and 0 capacity -> ERR_BUFFER_TOO_SMALL, no crash");
}

int main(void)
{
    test_round_trip_valid_reading();
    test_valid_zero_carries_raw_bytes();
    test_round_trip_nan_inf();
    test_vector_valid_reading();
    test_decode_too_short();
    test_decode_too_long();
    test_decode_wrong_cmd();
    test_encode_buffer_too_small();
    test_encode_null_buffer();

    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
