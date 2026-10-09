/* Host-native test for kilnlink_reboot.{c,h} -- the ESP->Pico
 * SAFETY_CMD_REBOOT (0x29) codec, docs/LINK_PROTOCOL.md sec 4. Mirrors
 * test_announce_reboot.c's structure exactly (this frame also carries no
 * payload at all): round-trip encode/decode, a byte-exact vector, and the
 * hostile input set: too-short, too-long, wrong command byte, undersized
 * output buffer.
 */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_announce_reboot.h"
#include "kilnlink/kilnlink_reboot.h"
#include "kilnlink/kilnlink_rollback.h"

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

static void test_round_trip(void)
{
    kilnlink_reboot_t msg = {0};

    uint8_t buf[KILNLINK_REBOOT_LEN];
    kilnlink_reboot_status_t status;
    size_t n = kilnlink_reboot_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_REBOOT_OK, "encode() reports OK");
    CHECK(n == KILNLINK_REBOOT_LEN, "encode() always writes exactly 1 byte");

    kilnlink_reboot_t decoded;
    CHECK(kilnlink_reboot_decode(buf, n, &decoded) == KILNLINK_REBOOT_OK,
          "decode() reports OK for a just-encoded payload");
}

static void test_round_trip_null_msg_and_out(void)
{
    uint8_t buf[KILNLINK_REBOOT_LEN];
    kilnlink_reboot_status_t status;
    size_t n = kilnlink_reboot_encode(NULL, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_REBOOT_OK, "encode(NULL msg) reports OK");
    CHECK(n == KILNLINK_REBOOT_LEN, "encode(NULL msg) still writes 1 byte");
    CHECK(kilnlink_reboot_decode(buf, n, NULL) == KILNLINK_REBOOT_OK, "decode(NULL out) still reports OK");
}

static void test_vector(void)
{
    static const uint8_t expected[] = {0x29};
    kilnlink_reboot_t msg = {0};

    uint8_t buf[KILNLINK_REBOOT_LEN];
    kilnlink_reboot_status_t status;
    size_t n = kilnlink_reboot_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_REBOOT_OK, "vector: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector: bytes match {0x29}");
    } else {
        CHECK(1, "vector: bytes match {0x29}");
    }
}

/* The whole point of this frame is that it is NOT a rollback and NOT the
 * ESP's own announce-reboot courtesy notice -- three distinct ids, and a
 * decoder for one must reject the others outright. A future renumbering
 * that collided any two of them would be a silent, extremely consequential
 * bug (a "reboot in place" arriving as a slot-changing rollback). */
static void test_id_is_distinct_from_rollback_and_announce(void)
{
    CHECK(KILNLINK_REBOOT_CMD != KILNLINK_ROLLBACK_CMD, "REBOOT's id differs from ROLLBACK's (0x17)");
    CHECK(KILNLINK_REBOOT_CMD != KILNLINK_ANNOUNCE_REBOOT_CMD,
          "REBOOT's id differs from ANNOUNCE_REBOOT's (0x18)");

    uint8_t rollback_buf[KILNLINK_ROLLBACK_LEN];
    kilnlink_rollback_status_t rstatus;
    size_t rn = kilnlink_rollback_encode(NULL, rollback_buf, sizeof(rollback_buf), &rstatus);
    kilnlink_reboot_t out;
    CHECK(kilnlink_reboot_decode(rollback_buf, rn, &out) == KILNLINK_REBOOT_ERR_WRONG_CMD,
          "a ROLLBACK frame is never decoded as a REBOOT");

    uint8_t reboot_buf[KILNLINK_REBOOT_LEN];
    kilnlink_reboot_status_t status;
    size_t n = kilnlink_reboot_encode(NULL, reboot_buf, sizeof(reboot_buf), &status);
    kilnlink_rollback_t rout;
    CHECK(kilnlink_rollback_decode(reboot_buf, n, &rout) == KILNLINK_ROLLBACK_ERR_WRONG_CMD,
          "a REBOOT frame is never decoded as a ROLLBACK");
}

static void test_decode_too_short(void)
{
    uint8_t buf[1] = {0};
    kilnlink_reboot_t out;
    CHECK(kilnlink_reboot_decode(buf, 0, &out) == KILNLINK_REBOOT_ERR_LENGTH_MISMATCH,
          "decode() of a 0-byte (one short) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_too_long(void)
{
    uint8_t buf[KILNLINK_REBOOT_LEN + 1] = {0};
    buf[0] = KILNLINK_REBOOT_CMD;
    kilnlink_reboot_t out;
    CHECK(kilnlink_reboot_decode(buf, sizeof(buf), &out) == KILNLINK_REBOOT_ERR_LENGTH_MISMATCH,
          "decode() of a 2-byte (one too many) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_REBOOT_LEN] = {0};
    buf[0] = 0x18; /* ANNOUNCE_REBOOT's id, not REBOOT's */
    kilnlink_reboot_t out;
    CHECK(kilnlink_reboot_decode(buf, sizeof(buf), &out) == KILNLINK_REBOOT_ERR_WRONG_CMD,
          "decode() with wrong command byte -> ERR_WRONG_CMD");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_reboot_t msg = {0};
    uint8_t buf[1];
    kilnlink_reboot_status_t status;
    size_t n = kilnlink_reboot_encode(&msg, buf, 0, &status);
    CHECK(n == 0, "encode() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_REBOOT_ERR_BUFFER_TOO_SMALL,
          "encode() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
}

int main(void)
{
    test_round_trip();
    test_round_trip_null_msg_and_out();
    test_vector();
    test_id_is_distinct_from_rollback_and_announce();
    test_decode_too_short();
    test_decode_too_long();
    test_decode_wrong_cmd();
    test_encode_buffer_too_small();

    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
