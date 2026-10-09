// Host tests for kilnlink_inject_tc.c (CommonFW) -- the wire codec for
// SAFETY_CMD_INJECT_TC (0x21), item 5 of the safety-TC-not-installed pass.
// link_task.c's handler and thermo_task.c's gate are NOT host-testable
// (FreeRTOS + pico-sdk), same reason link_task.c itself has no host test
// file -- this covers the one pure, hardware-free piece: encode/decode
// round-tripping and malformed-input rejection, the same split test_
// kilnlink_power.c documents for its own frame.
#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "test_common.h"
#include "kilnlink/kilnlink_inject_tc.h"

static void test_roundtrip_valid_reading(void)
{
    TEST_SECTION("kilnlink_inject_tc: round-trip, a good synthetic reading");

    kilnlink_inject_tc_t msg = {
        .valid = 1u,
        .tc_c = 851.25f,
        .cj_c = 23.5f,
        .fault_bits = 0u,
    };
    uint8_t buf[KILNLINK_INJECT_TC_LEN];
    kilnlink_inject_tc_status_t st;
    size_t n = kilnlink_inject_tc_encode(&msg, buf, sizeof(buf), &st);
    TEST_CHECK(n == KILNLINK_INJECT_TC_LEN, "encode returns the full fixed length");
    TEST_CHECK(st == KILNLINK_INJECT_TC_OK, "encode status OK");
    TEST_CHECK(buf[0] == KILNLINK_INJECT_TC_CMD, "byte 0 is the command id (0x21)");

    kilnlink_inject_tc_t out;
    memset(&out, 0xAA, sizeof(out));
    kilnlink_inject_tc_status_t dst = kilnlink_inject_tc_decode(buf, KILNLINK_INJECT_TC_LEN, &out);
    TEST_CHECK(dst == KILNLINK_INJECT_TC_OK, "decode status OK");
    TEST_CHECK(out.valid == 1u, "valid roundtrips");
    TEST_CHECK_NEAR(out.tc_c, 851.25f, 0.0001, "tc_c roundtrips");
    TEST_CHECK_NEAR(out.cj_c, 23.5f, 0.0001, "cj_c roundtrips");
    TEST_CHECK(out.fault_bits == 0u, "fault_bits roundtrips");
}

static void test_roundtrip_bad_read_with_fault_bits(void)
{
    TEST_SECTION("kilnlink_inject_tc: round-trip, an injected BAD read (S5 exercise path)");

    // valid=0 with fault_bits set -- exercises S5's bad-read recognition on
    // the receiving side (safety_guards.c's s5_bad_read_now()) the same way
    // a real MAX31856 OPEN fault would. tc_c/cj_c still carry real numbers
    // on the wire (this codec's own header comment: "still carried ... as
    // real IEEE754 floats ... rather than omitted") -- the RECEIVER is what
    // substitutes NaN, not this codec, so this test proves the wire bytes
    // themselves are not silently coerced by the codec.
    kilnlink_inject_tc_t msg = {
        .valid = 0u,
        .tc_c = 999.0f,
        .cj_c = 999.0f,
        .fault_bits = 0x01u, // SAFETY_THERMO_FAULT_OPEN
    };
    uint8_t buf[KILNLINK_INJECT_TC_LEN];
    size_t n = kilnlink_inject_tc_encode(&msg, buf, sizeof(buf), NULL);
    TEST_CHECK(n == KILNLINK_INJECT_TC_LEN, "encode succeeds with status==NULL (local fallback)");

    kilnlink_inject_tc_t out;
    kilnlink_inject_tc_status_t dst = kilnlink_inject_tc_decode(buf, KILNLINK_INJECT_TC_LEN, &out);
    TEST_CHECK(dst == KILNLINK_INJECT_TC_OK, "decode status OK");
    TEST_CHECK(out.valid == 0u, "valid=0 roundtrips (the receiver, not this codec, coerces NaN)");
    TEST_CHECK(out.fault_bits == 0x01u, "fault_bits (OPEN) roundtrips");
    TEST_CHECK_NEAR(out.tc_c, 999.0f, 0.0001,
                     "tc_c is passed through UNCHANGED even though valid=0 -- "
                     "this codec does not itself decide validity, same rule "
                     "link_frame_pack_status() documents for its own temp_valid");
}

static void test_decode_rejects_bad_length_and_wrong_cmd(void)
{
    TEST_SECTION("kilnlink_inject_tc: decode rejects malformed input");

    kilnlink_inject_tc_t msg = { .valid = 1u, .tc_c = 1.0f, .cj_c = 2.0f, .fault_bits = 0u };
    uint8_t buf[KILNLINK_INJECT_TC_LEN];
    kilnlink_inject_tc_encode(&msg, buf, sizeof(buf), NULL);

    kilnlink_inject_tc_t out;
    TEST_CHECK(kilnlink_inject_tc_decode(buf, KILNLINK_INJECT_TC_LEN - 1u, &out) ==
                   KILNLINK_INJECT_TC_ERR_LENGTH_MISMATCH,
               "one byte short is rejected");
    TEST_CHECK(kilnlink_inject_tc_decode(buf, KILNLINK_INJECT_TC_LEN + 1u, &out) ==
                   KILNLINK_INJECT_TC_ERR_LENGTH_MISMATCH,
               "one byte long is rejected -- this is a fixed-size frame, not a minimum length");

    uint8_t wrong_cmd[KILNLINK_INJECT_TC_LEN];
    memcpy(wrong_cmd, buf, sizeof(wrong_cmd));
    wrong_cmd[0] = 0x22u; // any other command id
    TEST_CHECK(kilnlink_inject_tc_decode(wrong_cmd, KILNLINK_INJECT_TC_LEN, &out) ==
                   KILNLINK_INJECT_TC_ERR_WRONG_CMD,
               "wrong command byte is rejected");
}

static void test_encode_rejects_undersized_buffer(void)
{
    TEST_SECTION("kilnlink_inject_tc: encode refuses a too-small output buffer");

    kilnlink_inject_tc_t msg = { .valid = 1u, .tc_c = 1.0f, .cj_c = 2.0f, .fault_bits = 0u };
    uint8_t small[KILNLINK_INJECT_TC_LEN - 1u];
    kilnlink_inject_tc_status_t st;
    size_t n = kilnlink_inject_tc_encode(&msg, small, sizeof(small), &st);
    TEST_CHECK(n == 0, "encode returns 0 when out_cap is too small");
    TEST_CHECK(st == KILNLINK_INJECT_TC_ERR_BUFFER_TOO_SMALL, "status reports BUFFER_TOO_SMALL");
}

void run_test_kilnlink_inject_tc(void)
{
    test_roundtrip_valid_reading();
    test_roundtrip_bad_read_with_fault_bits();
    test_decode_rejects_bad_length_and_wrong_cmd();
    test_encode_rejects_undersized_buffer();
}
