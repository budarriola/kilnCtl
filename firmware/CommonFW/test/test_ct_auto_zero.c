/* Host-native test for the CT auto-zero wire trio (CT_COMMISSIONING_PLAN.md
 * step 2): kilnlink_ct_auto_zero_begin.h (0x26), kilnlink_get_ct_auto_zero.h
 * (0x27, request), kilnlink_ct_auto_zero_status.h (0x28, reply). Round-trip
 * encode/decode, byte-exact vectors, and the hostile input set (too-short,
 * too-long, wrong command byte) for each, plus id-separation checks: none
 * of the three ids may collide with each other or with GET_CT_CAL/CT_CAL. */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_ct_auto_zero_begin.h"
#include "kilnlink/kilnlink_ct_auto_zero_status.h"
#include "kilnlink/kilnlink_get_ct_auto_zero.h"
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

/* -- BEGIN (0x26) --------------------------------------------------------- */

static void test_begin_round_trip(void)
{
    kilnlink_ct_auto_zero_begin_t msg = { .channel = 2 };
    uint8_t buf[KILNLINK_CT_AUTO_ZERO_BEGIN_LEN];
    kilnlink_ct_auto_zero_begin_status_t status;
    size_t n = kilnlink_ct_auto_zero_begin_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_CT_AUTO_ZERO_BEGIN_OK, "begin encode OK");
    CHECK(n == KILNLINK_CT_AUTO_ZERO_BEGIN_LEN, "begin encode length");
    CHECK(buf[0] == KILNLINK_CT_AUTO_ZERO_BEGIN_CMD, "begin cmd byte");

    kilnlink_ct_auto_zero_begin_t decoded = {0};
    CHECK(kilnlink_ct_auto_zero_begin_decode(buf, n, &decoded) == KILNLINK_CT_AUTO_ZERO_BEGIN_OK,
          "begin decode OK");
    CHECK(decoded.channel == 2, "begin decode round-trips channel");
}

static void test_begin_vector(void)
{
    static const uint8_t expected[] = {0x26, 0x01};
    kilnlink_ct_auto_zero_begin_t msg = { .channel = 1 };
    uint8_t buf[KILNLINK_CT_AUTO_ZERO_BEGIN_LEN];
    kilnlink_ct_auto_zero_begin_status_t status;
    size_t n = kilnlink_ct_auto_zero_begin_encode(&msg, buf, sizeof(buf), &status);
    CHECK(n == sizeof(expected) && memcmp(buf, expected, sizeof(expected)) == 0,
          "begin vector matches {0x26, 0x01}");
}

static void test_begin_hostile(void)
{
    uint8_t buf[KILNLINK_CT_AUTO_ZERO_BEGIN_LEN + 1] = {0};
    kilnlink_ct_auto_zero_begin_t out;
    CHECK(kilnlink_ct_auto_zero_begin_decode(buf, 0, &out) ==
              KILNLINK_CT_AUTO_ZERO_BEGIN_ERR_LENGTH_MISMATCH,
          "begin decode too-short -> LENGTH_MISMATCH");
    buf[0] = KILNLINK_CT_AUTO_ZERO_BEGIN_CMD;
    CHECK(kilnlink_ct_auto_zero_begin_decode(buf, sizeof(buf), &out) ==
              KILNLINK_CT_AUTO_ZERO_BEGIN_ERR_LENGTH_MISMATCH,
          "begin decode too-long -> LENGTH_MISMATCH");
    uint8_t wrong[KILNLINK_CT_AUTO_ZERO_BEGIN_LEN] = {0x19, 0x00}; /* SET_CT_CAL's id */
    CHECK(kilnlink_ct_auto_zero_begin_decode(wrong, sizeof(wrong), &out) ==
              KILNLINK_CT_AUTO_ZERO_BEGIN_ERR_WRONG_CMD,
          "begin decode wrong cmd -> WRONG_CMD");
    kilnlink_ct_auto_zero_begin_status_t estatus;
    CHECK(kilnlink_ct_auto_zero_begin_encode(&out, buf, 0, &estatus) == 0 &&
              estatus == KILNLINK_CT_AUTO_ZERO_BEGIN_ERR_BUFFER_TOO_SMALL,
          "begin encode undersized -> BUFFER_TOO_SMALL");
}

/* -- GET (0x27) ------------------------------------------------------------ */

static void test_get_round_trip(void)
{
    uint8_t buf[KILNLINK_GET_CT_AUTO_ZERO_LEN];
    kilnlink_get_ct_auto_zero_status_t status;
    size_t n = kilnlink_get_ct_auto_zero_encode(NULL, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_GET_CT_AUTO_ZERO_OK, "get encode OK");
    CHECK(n == 1 && buf[0] == KILNLINK_GET_CT_AUTO_ZERO_CMD, "get vector is {0x27}");

    kilnlink_get_ct_auto_zero_t out;
    CHECK(kilnlink_get_ct_auto_zero_decode(buf, n, &out) == KILNLINK_GET_CT_AUTO_ZERO_OK,
          "get decode OK");
}

static void test_get_hostile(void)
{
    kilnlink_get_ct_auto_zero_t out;
    uint8_t buf[1] = {0};
    CHECK(kilnlink_get_ct_auto_zero_decode(buf, 0, &out) ==
              KILNLINK_GET_CT_AUTO_ZERO_ERR_LENGTH_MISMATCH,
          "get decode empty -> LENGTH_MISMATCH");
    buf[0] = 0x22; /* GET_CT_CAL's id */
    CHECK(kilnlink_get_ct_auto_zero_decode(buf, sizeof(buf), &out) ==
              KILNLINK_GET_CT_AUTO_ZERO_ERR_WRONG_CMD,
          "get decode wrong cmd -> WRONG_CMD");
}

/* -- STATUS (0x28) reply ---------------------------------------------------- */

static void test_status_round_trip(void)
{
    kilnlink_ct_auto_zero_status_t msg = {
        .state = KILNLINK_CT_AUTO_ZERO_STATE_DONE,
        .channel = 2,
        .samples_taken = 200,
        .samples_target = 200,
        .zero_counts = 61,
    };
    uint8_t buf[KILNLINK_CT_AUTO_ZERO_STATUS_LEN];
    kilnlink_ct_auto_zero_status_codec_t status;
    size_t n = kilnlink_ct_auto_zero_status_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_CT_AUTO_ZERO_STATUS_OK, "status encode OK");
    CHECK(n == KILNLINK_CT_AUTO_ZERO_STATUS_LEN, "status encode length");

    kilnlink_ct_auto_zero_status_t decoded = {0};
    CHECK(kilnlink_ct_auto_zero_status_decode(buf, n, &decoded) == KILNLINK_CT_AUTO_ZERO_STATUS_OK,
          "status decode OK");
    CHECK(decoded.state == msg.state, "status decode state");
    CHECK(decoded.channel == msg.channel, "status decode channel");
    CHECK(decoded.samples_taken == msg.samples_taken, "status decode samples_taken");
    CHECK(decoded.samples_target == msg.samples_target, "status decode samples_target");
    CHECK(decoded.zero_counts == msg.zero_counts, "status decode zero_counts");
}

static void test_status_hostile(void)
{
    kilnlink_ct_auto_zero_status_t out;
    uint8_t buf[KILNLINK_CT_AUTO_ZERO_STATUS_LEN + 1] = {0};
    CHECK(kilnlink_ct_auto_zero_status_decode(buf, 0, &out) ==
              KILNLINK_CT_AUTO_ZERO_STATUS_ERR_LENGTH_MISMATCH,
          "status decode empty -> LENGTH_MISMATCH");
    buf[0] = KILNLINK_CT_AUTO_ZERO_STATUS_CMD;
    CHECK(kilnlink_ct_auto_zero_status_decode(buf, sizeof(buf), &out) ==
              KILNLINK_CT_AUTO_ZERO_STATUS_ERR_LENGTH_MISMATCH,
          "status decode too-long -> LENGTH_MISMATCH");
    uint8_t wrong[KILNLINK_CT_AUTO_ZERO_STATUS_LEN] = {0x1A}; /* CT_CAL's id */
    CHECK(kilnlink_ct_auto_zero_status_decode(wrong, sizeof(wrong), &out) ==
              KILNLINK_CT_AUTO_ZERO_STATUS_ERR_WRONG_CMD,
          "status decode wrong cmd -> WRONG_CMD");
}

/* -- id separation ---------------------------------------------------------- */

static void test_ids_distinct(void)
{
    CHECK(KILNLINK_CT_AUTO_ZERO_BEGIN_CMD != KILNLINK_GET_CT_AUTO_ZERO_CMD, "begin != get");
    CHECK(KILNLINK_CT_AUTO_ZERO_BEGIN_CMD != KILNLINK_CT_AUTO_ZERO_STATUS_CMD, "begin != status");
    CHECK(KILNLINK_GET_CT_AUTO_ZERO_CMD != KILNLINK_CT_AUTO_ZERO_STATUS_CMD, "get != status");
    CHECK(KILNLINK_CT_AUTO_ZERO_BEGIN_CMD != KILNLINK_GET_CT_CAL_CMD, "begin != GET_CT_CAL");
    CHECK(KILNLINK_CT_AUTO_ZERO_STATUS_CMD != KILNLINK_CT_CAL_CMD, "status != CT_CAL reply");
}

int main(void)
{
    test_begin_round_trip();
    test_begin_vector();
    test_begin_hostile();
    test_get_round_trip();
    test_get_hostile();
    test_status_round_trip();
    test_status_hostile();
    test_ids_distinct();

    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
