/* Host-native test for kilnlink_test_trip.{c,h} -- the ESP->Pico
 * SAFETY_CMD_TEST_TRIP (0x2E) codec, docs/LINK_PROTOCOL.md sec 4,
 * docs/TEST_TRIP_PLAN.md sec 2.1. Frozen byte vectors, round trip, and the
 * hostile input set (too short, too long, wrong cmd). The magic byte is
 * carried verbatim: a wrong magic must still decode (Pico-side
 * REFUSED_BAD_FRAME, not a codec failure). */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_test_trip.h"

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
    CHECK(KILNLINK_TEST_TRIP_CMD == 0x2Eu, "cmd id is 0x2E");
    CHECK(KILNLINK_TEST_TRIP_LEN == 4u, "frame is 4 bytes");
    CHECK(KILNLINK_TEST_TRIP_MAGIC == 0xA5u, "magic is 0xA5");
}

static void test_vector(void)
{
    static const uint8_t expected[] = {0x2e, 0x07, 0x42, 0xa5};
    kilnlink_test_trip_t msg = {0x07, 0x42, KILNLINK_TEST_TRIP_MAGIC};
    uint8_t buf[KILNLINK_TEST_TRIP_LEN];
    kilnlink_test_trip_status_t st;
    size_t n = kilnlink_test_trip_encode(&msg, buf, sizeof(buf), &st);
    CHECK(st == KILNLINK_TEST_TRIP_OK, "encode OK");
    CHECK(n == sizeof(expected) && memcmp(buf, expected, sizeof(expected)) == 0, "frozen vector bytes");
}

static void test_round_trip(void)
{
    kilnlink_test_trip_t msg = {0xFF, 0x00, KILNLINK_TEST_TRIP_MAGIC};
    uint8_t buf[KILNLINK_TEST_TRIP_LEN];
    kilnlink_test_trip_status_t st;
    size_t n = kilnlink_test_trip_encode(&msg, buf, sizeof(buf), &st);
    kilnlink_test_trip_t d;
    memset(&d, 0, sizeof(d));
    CHECK(kilnlink_test_trip_decode(buf, n, &d) == KILNLINK_TEST_TRIP_OK, "decode OK");
    CHECK(d.pico_boot_id == 0xFF && d.request_id == 0x00 && d.magic == KILNLINK_TEST_TRIP_MAGIC,
          "fields round-trip");
}

static void test_wrong_magic_still_decodes(void)
{
    static const uint8_t wire[] = {0x2e, 0x01, 0x02, 0x5a};
    kilnlink_test_trip_t d;
    CHECK(kilnlink_test_trip_decode(wire, sizeof(wire), &d) == KILNLINK_TEST_TRIP_OK,
          "wrong magic is handed up, not a codec error");
    CHECK(d.magic == 0x5a, "wrong magic carried verbatim");
}

static void test_hostile(void)
{
    uint8_t buf[KILNLINK_TEST_TRIP_LEN + 1] = {0};
    kilnlink_test_trip_t d;
    buf[0] = KILNLINK_TEST_TRIP_CMD;
    for (size_t len = 0; len <= sizeof(buf); ++len) {
        if (len == KILNLINK_TEST_TRIP_LEN) continue;
        CHECK(kilnlink_test_trip_decode(buf, len, &d) == KILNLINK_TEST_TRIP_ERR_LENGTH_MISMATCH,
              "any length other than 4 -> ERR_LENGTH_MISMATCH");
    }
    buf[0] = 0x2D;
    CHECK(kilnlink_test_trip_decode(buf, KILNLINK_TEST_TRIP_LEN, &d) == KILNLINK_TEST_TRIP_ERR_WRONG_CMD,
          "wrong cmd -> ERR_WRONG_CMD");
}

static void test_encode_small(void)
{
    kilnlink_test_trip_t msg = {0, 0, 0};
    uint8_t buf[3];
    kilnlink_test_trip_status_t st;
    CHECK(kilnlink_test_trip_encode(&msg, buf, sizeof(buf), &st) == 0, "undersized buffer writes nothing");
    CHECK(st == KILNLINK_TEST_TRIP_ERR_BUFFER_TOO_SMALL, "-> ERR_BUFFER_TOO_SMALL");
}

int main(void)
{
    test_constants();
    test_vector();
    test_round_trip();
    test_wrong_magic_still_decodes();
    test_hostile();
    test_encode_small();
    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
