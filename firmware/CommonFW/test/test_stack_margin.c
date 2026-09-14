/* Host-native test for kilnlink_stack_margin.{c,h} -- the Pico->ESP
 * SAFETY_CMD_STACK_MARGIN (0x2C) codec, reply to SAFETY_CMD_GET_STACK_MARGIN
 * (0x2B). docs/LINK_PROTOCOL.md sec 4, KILNLINK_PROTOCOL_VERSION 13.
 *
 * Covers: a full round trip with all 9 tasks fully measured, a round trip
 * with the KILNLINK_STACK_MARGIN_UNMEASURED sentinel still present
 * (rounds_completed == 0, matching a board that has not finished its first
 * poller cycle yet), a fixed wire vector, and malformed-frame decode
 * failures (length mismatch, wrong cmd) -- the two negative cases this
 * task's instructions call out explicitly. */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_stack_margin.h"

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

static void fill_all_measured(kilnlink_stack_margin_t *msg)
{
    memset(msg, 0, sizeof(*msg));
    msg->rounds_completed = 3;
    /* Plausible-looking per-task words, distinct per entry so a transposed
     * field/entry bug would be caught rather than hidden behind repeated
     * values. */
    static const uint16_t hw[KILNLINK_STACK_MARGIN_NUM_TASKS] = {
        120, 300, 45, 512, 71, 900, 472, 210, 88};
    static const uint16_t total[KILNLINK_STACK_MARGIN_NUM_TASKS] = {
        256, 512, 256, 1024, 256, 6144, 512, 1024, 256};
    for (size_t i = 0; i < KILNLINK_STACK_MARGIN_NUM_TASKS; i++) {
        msg->entries[i].task_id = (uint8_t)i;
        msg->entries[i].high_water_words = hw[i];
        msg->entries[i].stack_total_words = total[i];
    }
}

static void test_round_trip_all_measured(void)
{
    kilnlink_stack_margin_t msg;
    fill_all_measured(&msg);

    uint8_t buf[KILNLINK_STACK_MARGIN_LEN];
    kilnlink_stack_margin_status_t status;
    size_t n = kilnlink_stack_margin_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_STACK_MARGIN_OK, "encode() reports OK");
    CHECK(n == KILNLINK_STACK_MARGIN_LEN, "encode() always writes exactly 47 bytes");
    CHECK(buf[0] == KILNLINK_STACK_MARGIN_CMD, "encoded byte 0 is the command id (0x2C)");

    kilnlink_stack_margin_t decoded;
    memset(&decoded, 0xAA, sizeof(decoded));
    CHECK(kilnlink_stack_margin_decode(buf, n, &decoded) == KILNLINK_STACK_MARGIN_OK,
          "decode() reports OK for a just-encoded payload");
    CHECK(decoded.rounds_completed == 3, "decoded rounds_completed round-trips");
    for (size_t i = 0; i < KILNLINK_STACK_MARGIN_NUM_TASKS; i++) {
        CHECK(decoded.entries[i].task_id == msg.entries[i].task_id, "task_id round-trips per entry");
        CHECK(decoded.entries[i].high_water_words == msg.entries[i].high_water_words,
              "high_water_words round-trips per entry");
        CHECK(decoded.entries[i].stack_total_words == msg.entries[i].stack_total_words,
              "stack_total_words round-trips per entry");
    }
}

static void test_round_trip_unmeasured_sentinel(void)
{
    /* A board still mid-first-round: rounds_completed == 0, and at least
     * one entry still carries the UNMEASURED sentinel. Proves the sentinel
     * value itself (0xFFFF) survives the wire round trip undisturbed --
     * it must never be silently clamped or reinterpreted. */
    kilnlink_stack_margin_t msg;
    fill_all_measured(&msg);
    msg.rounds_completed = 0;
    msg.entries[7].high_water_words = KILNLINK_STACK_MARGIN_UNMEASURED;
    msg.entries[8].high_water_words = KILNLINK_STACK_MARGIN_UNMEASURED;

    uint8_t buf[KILNLINK_STACK_MARGIN_LEN];
    kilnlink_stack_margin_status_t status;
    size_t n = kilnlink_stack_margin_encode(&msg, buf, sizeof(buf), &status);
    CHECK(n == KILNLINK_STACK_MARGIN_LEN, "encode() of a partial-round message still writes 47 bytes");

    kilnlink_stack_margin_t decoded;
    CHECK(kilnlink_stack_margin_decode(buf, n, &decoded) == KILNLINK_STACK_MARGIN_OK,
          "decode() of a partial-round message OK");
    CHECK(decoded.rounds_completed == 0, "decoded rounds_completed == 0 round-trips");
    CHECK(decoded.entries[7].high_water_words == KILNLINK_STACK_MARGIN_UNMEASURED,
          "UNMEASURED sentinel round-trips unchanged (entry 7)");
    CHECK(decoded.entries[8].high_water_words == KILNLINK_STACK_MARGIN_UNMEASURED,
          "UNMEASURED sentinel round-trips unchanged (entry 8)");
}

static void test_vector(void)
{
    /* Fixed wire vector for two tasks, hand-computed little-endian, to
     * catch an offset/endianness regression that a self-consistent
     * round-trip test alone could miss. rounds_completed=1;
     * task0: id=0, hw=0x0010(16), total=0x0100(256);
     * task1: id=1, hw=0x0002(2),  total=0x0080(128);
     * remaining 7 entries zeroed. */
    kilnlink_stack_margin_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.rounds_completed = 1;
    msg.entries[0].task_id = 0;
    msg.entries[0].high_water_words = 16;
    msg.entries[0].stack_total_words = 256;
    msg.entries[1].task_id = 1;
    msg.entries[1].high_water_words = 2;
    msg.entries[1].stack_total_words = 128;

    uint8_t buf[KILNLINK_STACK_MARGIN_LEN];
    kilnlink_stack_margin_status_t status;
    size_t n = kilnlink_stack_margin_encode(&msg, buf, sizeof(buf), &status);
    CHECK(n == KILNLINK_STACK_MARGIN_LEN, "vector: encode() writes 47 bytes");

    static const uint8_t expected_prefix[] = {
        0x2C,       /* cmd */
        0x01,       /* rounds_completed */
        0x00, 0x10, 0x00, 0x00, 0x01, /* entry 0: id, hw lo/hi, total lo/hi */
        0x01, 0x02, 0x00, 0x80, 0x00, /* entry 1: id, hw lo/hi, total lo/hi */
    };
    print_hex("vector actual  ", buf, sizeof(expected_prefix));
    print_hex("vector expected", expected_prefix, sizeof(expected_prefix));
    CHECK(memcmp(buf, expected_prefix, sizeof(expected_prefix)) == 0,
          "vector: first 12 bytes match the hand-computed little-endian layout");
}

/* -- malformed input: negative-test coverage ------------------------------ */

static void test_decode_length_mismatch(void)
{
    uint8_t buf[KILNLINK_STACK_MARGIN_LEN + 1];
    memset(buf, 0, sizeof(buf));
    buf[0] = KILNLINK_STACK_MARGIN_CMD;
    kilnlink_stack_margin_t out;
    CHECK(kilnlink_stack_margin_decode(buf, sizeof(buf), &out) ==
              KILNLINK_STACK_MARGIN_ERR_LENGTH_MISMATCH,
          "decode() of a too-long buffer reports LENGTH_MISMATCH");

    CHECK(kilnlink_stack_margin_decode(buf, KILNLINK_STACK_MARGIN_LEN - 1, &out) ==
              KILNLINK_STACK_MARGIN_ERR_LENGTH_MISMATCH,
          "decode() of a too-short buffer reports LENGTH_MISMATCH");

    CHECK(kilnlink_stack_margin_decode(buf, 0, &out) == KILNLINK_STACK_MARGIN_ERR_LENGTH_MISMATCH,
          "decode() of a 0-byte buffer reports LENGTH_MISMATCH");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_STACK_MARGIN_LEN];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x00; /* not 0x2C */
    kilnlink_stack_margin_t out;
    CHECK(kilnlink_stack_margin_decode(buf, sizeof(buf), &out) == KILNLINK_STACK_MARGIN_ERR_WRONG_CMD,
          "decode() of a frame with the wrong cmd byte reports WRONG_CMD");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_stack_margin_t msg;
    fill_all_measured(&msg);
    uint8_t buf[KILNLINK_STACK_MARGIN_LEN - 1];
    kilnlink_stack_margin_status_t status;
    size_t n = kilnlink_stack_margin_encode(&msg, buf, sizeof(buf), &status);
    CHECK(n == 0, "encode() into an undersized buffer returns 0");
    CHECK(status == KILNLINK_STACK_MARGIN_ERR_BUFFER_TOO_SMALL,
          "encode() into an undersized buffer reports BUFFER_TOO_SMALL");
}

static void test_encode_null_msg(void)
{
    uint8_t buf[KILNLINK_STACK_MARGIN_LEN];
    kilnlink_stack_margin_status_t status;
    size_t n = kilnlink_stack_margin_encode(NULL, buf, sizeof(buf), &status);
    CHECK(n == 0, "encode(NULL msg) returns 0 -- unlike the request codec, this frame has real "
                  "per-task fields with no meaningful default to invent");
}

int main(void)
{
    test_round_trip_all_measured();
    test_round_trip_unmeasured_sentinel();
    test_vector();
    test_decode_length_mismatch();
    test_decode_wrong_cmd();
    test_encode_buffer_too_small();
    test_encode_null_msg();

    if (g_failures == 0) {
        printf("PASS: all test_stack_margin checks passed\n");
        return 0;
    }
    printf("FAIL: %d check(s) failed\n", g_failures);
    return 1;
}
