/* Host-native test for kilnlink_reboot_result.{c,h} -- the Pico->ESP
 * SAFETY_CMD_REBOOT_RESULT (0x2A) codec, docs/LINK_PROTOCOL.md sec 4.
 * Mirrors test_rollback_result.c's structure, with one deliberate
 * difference: accepted=1 is a REAL, routinely-sent value here (see the
 * header's "SYMMETRIC, unlike kilnlink_rollback_result.h" comment), so the
 * accepted path is tested as a first-class case rather than a
 * wire-completeness curiosity.
 */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_reboot_result.h"
#include "kilnlink/kilnlink_rollback_result.h"

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

static void test_round_trip_accepted(void)
{
    kilnlink_reboot_result_t msg = {0};
    msg.accepted = 1;
    msg.reason = KILNLINK_REBOOT_RESULT_REASON_NONE;

    uint8_t buf[KILNLINK_REBOOT_RESULT_LEN];
    kilnlink_reboot_result_status_t status;
    size_t n = kilnlink_reboot_result_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_REBOOT_RESULT_OK, "encode() reports OK");
    CHECK(n == KILNLINK_REBOOT_RESULT_LEN, "encode() always writes exactly 3 bytes");
    CHECK(buf[0] == KILNLINK_REBOOT_RESULT_CMD, "encoded byte 0 is the command id");

    kilnlink_reboot_result_t decoded;
    memset(&decoded, 0xAA, sizeof(decoded));
    CHECK(kilnlink_reboot_result_decode(buf, n, &decoded) == KILNLINK_REBOOT_RESULT_OK,
          "decode() reports OK for a just-encoded payload");
    CHECK(decoded.accepted == 1, "decoded accepted round-trips (1)");
    CHECK(decoded.reason == KILNLINK_REBOOT_RESULT_REASON_NONE, "decoded reason round-trips (NONE)");
}

static void test_round_trip_refused_armed(void)
{
    kilnlink_reboot_result_t msg = {0};
    msg.accepted = 0;
    msg.reason = KILNLINK_REBOOT_RESULT_REASON_ARMED;

    uint8_t buf[KILNLINK_REBOOT_RESULT_LEN];
    kilnlink_reboot_result_status_t status;
    size_t n = kilnlink_reboot_result_encode(&msg, buf, sizeof(buf), &status);
    CHECK(n == KILNLINK_REBOOT_RESULT_LEN, "encode() of a refusal writes 3 bytes");

    kilnlink_reboot_result_t decoded;
    CHECK(kilnlink_reboot_result_decode(buf, n, &decoded) == KILNLINK_REBOOT_RESULT_OK, "decode() of a refusal OK");
    CHECK(decoded.accepted == 0, "decoded accepted round-trips (0)");
    CHECK(decoded.reason == KILNLINK_REBOOT_RESULT_REASON_ARMED, "decoded reason round-trips (ARMED)");
}

static void test_vector(void)
{
    static const uint8_t expected[] = {0x2A, 0x00, 0x01}; /* refused, ARMED */
    kilnlink_reboot_result_t msg = {0};
    msg.accepted = 0;
    msg.reason = KILNLINK_REBOOT_RESULT_REASON_ARMED;

    uint8_t buf[KILNLINK_REBOOT_RESULT_LEN];
    kilnlink_reboot_result_status_t status;
    size_t n = kilnlink_reboot_result_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_REBOOT_RESULT_OK, "vector: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector: bytes match {0x2A,0x00,0x01}");
    } else {
        CHECK(1, "vector: bytes match {0x2A,0x00,0x01}");
    }
}

/* REBOOT_RESULT and ROLLBACK_RESULT have the same 3-byte shape and nearby
 * ids; only the command byte separates "the Pico rebooted in place" from
 * "the Pico changed which firmware slot it boots". Neither decoder may ever
 * accept the other's frame. */
static void test_not_confusable_with_rollback_result(void)
{
    CHECK(KILNLINK_REBOOT_RESULT_CMD != KILNLINK_ROLLBACK_RESULT_CMD,
          "REBOOT_RESULT's id differs from ROLLBACK_RESULT's (0x25)");

    kilnlink_rollback_result_t rmsg = {0};
    rmsg.accepted = 0;
    rmsg.reason = KILNLINK_ROLLBACK_RESULT_REASON_ARMED;
    uint8_t rbuf[KILNLINK_ROLLBACK_RESULT_LEN];
    size_t rn = kilnlink_rollback_result_encode(&rmsg, rbuf, sizeof(rbuf), NULL);
    kilnlink_reboot_result_t out;
    CHECK(kilnlink_reboot_result_decode(rbuf, rn, &out) == KILNLINK_REBOOT_RESULT_ERR_WRONG_CMD,
          "a ROLLBACK_RESULT frame is never decoded as a REBOOT_RESULT");

    kilnlink_reboot_result_t msg = {0};
    msg.accepted = 1;
    uint8_t buf[KILNLINK_REBOOT_RESULT_LEN];
    size_t n = kilnlink_reboot_result_encode(&msg, buf, sizeof(buf), NULL);
    kilnlink_rollback_result_t rout;
    CHECK(kilnlink_rollback_result_decode(buf, n, &rout) == KILNLINK_ROLLBACK_RESULT_ERR_WRONG_CMD,
          "a REBOOT_RESULT frame is never decoded as a ROLLBACK_RESULT");
}

static void test_decode_too_short(void)
{
    uint8_t buf[KILNLINK_REBOOT_RESULT_LEN] = {KILNLINK_REBOOT_RESULT_CMD, 0, 0};
    kilnlink_reboot_result_t out;
    CHECK(kilnlink_reboot_result_decode(buf, KILNLINK_REBOOT_RESULT_LEN - 1u, &out) ==
              KILNLINK_REBOOT_RESULT_ERR_LENGTH_MISMATCH,
          "decode() of a 2-byte (one short) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_too_long(void)
{
    uint8_t buf[KILNLINK_REBOOT_RESULT_LEN + 1] = {0};
    buf[0] = KILNLINK_REBOOT_RESULT_CMD;
    kilnlink_reboot_result_t out;
    CHECK(kilnlink_reboot_result_decode(buf, sizeof(buf), &out) == KILNLINK_REBOOT_RESULT_ERR_LENGTH_MISMATCH,
          "decode() of a 4-byte (one too many) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_REBOOT_RESULT_LEN] = {0};
    buf[0] = KILNLINK_REBOOT_RESULT_CMD + 1u;
    kilnlink_reboot_result_t out;
    CHECK(kilnlink_reboot_result_decode(buf, sizeof(buf), &out) == KILNLINK_REBOOT_RESULT_ERR_WRONG_CMD,
          "decode() with wrong command byte -> ERR_WRONG_CMD");
}

/* An unknown reason byte must survive decode untouched (the codec
 * serializes, the receiver interprets) so the receiver can map it to
 * _REASON_UNKNOWN itself rather than have the codec alias it onto a real
 * reason -- CommonFW/README.md rule 6. */
static void test_decode_unknown_reason_byte_preserved(void)
{
    uint8_t buf[KILNLINK_REBOOT_RESULT_LEN] = {KILNLINK_REBOOT_RESULT_CMD, 0u, 0x7Fu};
    kilnlink_reboot_result_t out;
    CHECK(kilnlink_reboot_result_decode(buf, sizeof(buf), &out) == KILNLINK_REBOOT_RESULT_OK,
          "decode() of an out-of-enum reason byte still reports OK");
    CHECK(out.reason == 0x7Fu, "an out-of-enum reason byte is preserved verbatim, not aliased");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_reboot_result_t msg = {0};
    uint8_t buf[KILNLINK_REBOOT_RESULT_LEN];
    kilnlink_reboot_result_status_t status;
    size_t n = kilnlink_reboot_result_encode(&msg, buf, KILNLINK_REBOOT_RESULT_LEN - 1u, &status);
    CHECK(n == 0, "encode() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_REBOOT_RESULT_ERR_BUFFER_TOO_SMALL,
          "encode() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
}

int main(void)
{
    test_round_trip_accepted();
    test_round_trip_refused_armed();
    test_vector();
    test_not_confusable_with_rollback_result();
    test_decode_too_short();
    test_decode_too_long();
    test_decode_wrong_cmd();
    test_decode_unknown_reason_byte_preserved();
    test_encode_buffer_too_small();

    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
