/* Host-native test for kilnlink_test_trip_result.{c,h} -- the Pico->ESP
 * SAFETY_CMD_TEST_TRIP_RESULT (0x2F) codec, docs/LINK_PROTOCOL.md sec 4,
 * docs/TEST_TRIP_PLAN.md sec 2.1. The outcome is a CLOSED enum (0..7): any
 * other value is refused on both encode and decode. */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_test_trip_result.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                           \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

static void test_constants(void)
{
    CHECK(KILNLINK_TEST_TRIP_RESULT_CMD == 0x2Fu, "cmd id is 0x2F");
    CHECK(KILNLINK_TEST_TRIP_RESULT_LEN == 4u, "frame is 4 bytes");
    CHECK(KILNLINK_TEST_TRIP_OUTCOME_ACCEPTED == 0, "ACCEPTED = 0");
    CHECK(KILNLINK_TEST_TRIP_OUTCOME_REFUSED_ALREADY_TRIPPED == 1, "ALREADY_TRIPPED = 1");
    CHECK(KILNLINK_TEST_TRIP_OUTCOME_REFUSED_BOOT_ID == 2, "BOOT_ID = 2");
    CHECK(KILNLINK_TEST_TRIP_OUTCOME_REFUSED_PEER_VERSION == 3, "PEER_VERSION = 3");
    CHECK(KILNLINK_TEST_TRIP_OUTCOME_REFUSED_RATE_LIMIT == 4, "RATE_LIMIT = 4");
    CHECK(KILNLINK_TEST_TRIP_OUTCOME_REFUSED_UPDATING == 5, "UPDATING = 5");
    CHECK(KILNLINK_TEST_TRIP_OUTCOME_REFUSED_BAD_FRAME == 6, "BAD_FRAME = 6");
    CHECK(KILNLINK_TEST_TRIP_OUTCOME_DUPLICATE == 7, "DUPLICATE = 7");
    CHECK(KILNLINK_TEST_TRIP_OUTCOME_MAX == 7u, "MAX = 7");
}

static void test_vectors(void)
{
    static const uint8_t accepted[] = {0x2f, 0x42, 0x00, 0x09};
    static const uint8_t dup[] = {0x2f, 0x01, 0x07, 0x03};
    kilnlink_test_trip_result_t a = {0x42, KILNLINK_TEST_TRIP_OUTCOME_ACCEPTED, 0x09};
    kilnlink_test_trip_result_t b = {0x01, KILNLINK_TEST_TRIP_OUTCOME_DUPLICATE, 0x03};
    uint8_t buf[KILNLINK_TEST_TRIP_RESULT_LEN];
    kilnlink_test_trip_result_status_t st;
    CHECK(kilnlink_test_trip_result_encode(&a, buf, sizeof(buf), &st) == 4 && st == KILNLINK_TEST_TRIP_RESULT_OK,
          "accepted encode OK");
    CHECK(memcmp(buf, accepted, 4) == 0, "frozen accepted vector");
    CHECK(kilnlink_test_trip_result_encode(&b, buf, sizeof(buf), &st) == 4, "duplicate encode OK");
    CHECK(memcmp(buf, dup, 4) == 0, "frozen duplicate vector");
}

static void test_round_trip_all_outcomes(void)
{
    for (uint8_t o = 0; o <= KILNLINK_TEST_TRIP_OUTCOME_MAX; ++o) {
        kilnlink_test_trip_result_t m = {0xA0, o, (uint8_t)(o + 1)};
        uint8_t buf[KILNLINK_TEST_TRIP_RESULT_LEN];
        kilnlink_test_trip_result_status_t st;
        size_t n = kilnlink_test_trip_result_encode(&m, buf, sizeof(buf), &st);
        kilnlink_test_trip_result_t d;
        memset(&d, 0, sizeof(d));
        CHECK(n == 4, "encode length");
        CHECK(kilnlink_test_trip_result_decode(buf, n, &d) == KILNLINK_TEST_TRIP_RESULT_OK, "decode OK");
        CHECK(d.request_id == 0xA0 && d.outcome == o && d.trip_seq == (uint8_t)(o + 1), "fields round-trip");
    }
}

static void test_unknown_outcome_refused(void)
{
    uint8_t buf[KILNLINK_TEST_TRIP_RESULT_LEN] = {0x2f, 0x01, 0x08, 0x00};
    kilnlink_test_trip_result_t d;
    CHECK(kilnlink_test_trip_result_decode(buf, 4, &d) == KILNLINK_TEST_TRIP_RESULT_ERR_BAD_OUTCOME,
          "outcome 8 -> ERR_BAD_OUTCOME");
    buf[2] = 0xFF;
    CHECK(kilnlink_test_trip_result_decode(buf, 4, &d) == KILNLINK_TEST_TRIP_RESULT_ERR_BAD_OUTCOME,
          "outcome 0xFF -> ERR_BAD_OUTCOME");
    kilnlink_test_trip_result_t m = {0, 8, 0};
    kilnlink_test_trip_result_status_t st;
    CHECK(kilnlink_test_trip_result_encode(&m, buf, sizeof(buf), &st) == 0, "encode refuses unknown outcome");
    CHECK(st == KILNLINK_TEST_TRIP_RESULT_ERR_BAD_OUTCOME, "encode status ERR_BAD_OUTCOME");
}

static void test_hostile(void)
{
    uint8_t buf[KILNLINK_TEST_TRIP_RESULT_LEN + 1] = {0};
    kilnlink_test_trip_result_t d;
    buf[0] = KILNLINK_TEST_TRIP_RESULT_CMD;
    for (size_t len = 0; len <= sizeof(buf); ++len) {
        if (len == KILNLINK_TEST_TRIP_RESULT_LEN) continue;
        CHECK(kilnlink_test_trip_result_decode(buf, len, &d) == KILNLINK_TEST_TRIP_RESULT_ERR_LENGTH_MISMATCH,
              "any length other than 4 -> ERR_LENGTH_MISMATCH");
    }
    buf[0] = 0x2E; /* the request id, never the reply's */
    CHECK(kilnlink_test_trip_result_decode(buf, KILNLINK_TEST_TRIP_RESULT_LEN, &d) ==
              KILNLINK_TEST_TRIP_RESULT_ERR_WRONG_CMD,
          "request opcode in reply slot -> ERR_WRONG_CMD");
}

static void test_encode_small(void)
{
    kilnlink_test_trip_result_t m = {0, 0, 0};
    uint8_t buf[3];
    kilnlink_test_trip_result_status_t st;
    CHECK(kilnlink_test_trip_result_encode(&m, buf, sizeof(buf), &st) == 0, "undersized buffer writes nothing");
    CHECK(st == KILNLINK_TEST_TRIP_RESULT_ERR_BUFFER_TOO_SMALL, "-> ERR_BUFFER_TOO_SMALL");
}

int main(void)
{
    test_constants();
    test_vectors();
    test_round_trip_all_outcomes();
    test_unknown_outcome_refused();
    test_hostile();
    test_encode_small();
    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
