/* Host-native test for kilnlink_rollback_result.{c,h} -- the Pico->ESP
 * SAFETY_CMD_ROLLBACK_RESULT (0x25) codec, docs/LINK_PROTOCOL.md sec 4.
 * Mirrors test_commit_config_rejected.c's structure (the closest existing
 * precedent: a Pico->ESP refusal reply with a closed reason enum): round-
 * trip encode/decode, a byte-exact vector, and the hostile input set
 * (too-short, too-long, wrong command byte, undersized output buffer).
 */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_rollback.h"
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

static void test_round_trip_refused(void)
{
    kilnlink_rollback_result_t msg = {0};
    msg.accepted = 0;
    msg.reason = KILNLINK_ROLLBACK_RESULT_REASON_SLOT_INVALID;

    uint8_t buf[KILNLINK_ROLLBACK_RESULT_LEN];
    kilnlink_rollback_result_status_t status;
    size_t n = kilnlink_rollback_result_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_ROLLBACK_RESULT_OK, "encode() reports OK");
    CHECK(n == KILNLINK_ROLLBACK_RESULT_LEN, "encode() always writes exactly 3 bytes");
    CHECK(buf[0] == KILNLINK_ROLLBACK_RESULT_CMD, "encoded byte 0 is the command id");

    kilnlink_rollback_result_t decoded;
    memset(&decoded, 0xAA, sizeof(decoded));
    CHECK(kilnlink_rollback_result_decode(buf, n, &decoded) == KILNLINK_ROLLBACK_RESULT_OK,
          "decode() reports OK for a just-encoded payload");
    CHECK(decoded.accepted == 0, "decoded accepted round-trips (0)");
    CHECK(decoded.reason == KILNLINK_ROLLBACK_RESULT_REASON_SLOT_INVALID, "decoded reason round-trips");
}

/* accepted=1 has no real sender today (see the header's "ASYMMETRIC BY
 * DESIGN" comment), but the codec itself must not special-case it -- the
 * wire format is complete even though nothing in this codebase emits this
 * value yet. */
static void test_round_trip_accepted_bit(void)
{
    kilnlink_rollback_result_t msg = {0};
    msg.accepted = 1;
    msg.reason = KILNLINK_ROLLBACK_RESULT_REASON_ARMED; /* meaningless when accepted, but must still round-trip */

    uint8_t buf[KILNLINK_ROLLBACK_RESULT_LEN];
    kilnlink_rollback_result_status_t status;
    size_t n = kilnlink_rollback_result_encode(&msg, buf, sizeof(buf), &status);
    CHECK(n == KILNLINK_ROLLBACK_RESULT_LEN, "encode() with accepted=1 still writes 3 bytes");

    kilnlink_rollback_result_t decoded;
    CHECK(kilnlink_rollback_result_decode(buf, n, &decoded) == KILNLINK_ROLLBACK_RESULT_OK,
          "decode() of accepted=1 reports OK");
    CHECK(decoded.accepted == 1, "accepted=1 survives the wire unmangled");
}

static void test_all_reasons_round_trip(void)
{
    const uint8_t reasons[] = {
        KILNLINK_ROLLBACK_RESULT_REASON_ARMED,
        KILNLINK_ROLLBACK_RESULT_REASON_NO_METADATA,
        KILNLINK_ROLLBACK_RESULT_REASON_SLOT_INVALID,
        KILNLINK_ROLLBACK_RESULT_REASON_STORAGE,
        KILNLINK_ROLLBACK_RESULT_REASON_UNKNOWN,
    };
    for (size_t i = 0; i < sizeof(reasons) / sizeof(reasons[0]); i++) {
        kilnlink_rollback_result_t msg = {0};
        msg.reason = reasons[i];
        uint8_t buf[KILNLINK_ROLLBACK_RESULT_LEN];
        kilnlink_rollback_result_status_t status;
        size_t n = kilnlink_rollback_result_encode(&msg, buf, sizeof(buf), &status);
        kilnlink_rollback_result_t decoded;
        CHECK(n == KILNLINK_ROLLBACK_RESULT_LEN && kilnlink_rollback_result_decode(buf, n, &decoded) ==
                                                        KILNLINK_ROLLBACK_RESULT_OK &&
                  decoded.reason == reasons[i],
              "each closed-set reason value round-trips");
    }
}

static void test_vector(void)
{
    /* accepted=0, reason=2 (SLOT_INVALID) -- byte-exact so a future
     * accidental field-order swap fails this test instead of shipping
     * silently. */
    static const uint8_t expected[] = {0x25, 0x00, 0x02};
    kilnlink_rollback_result_t msg = {0};
    msg.accepted = 0;
    msg.reason = KILNLINK_ROLLBACK_RESULT_REASON_SLOT_INVALID;

    uint8_t buf[KILNLINK_ROLLBACK_RESULT_LEN];
    kilnlink_rollback_result_status_t status;
    size_t n = kilnlink_rollback_result_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_ROLLBACK_RESULT_OK, "vector: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector: bytes match {0x25, 0x00, 0x02}");
    } else {
        CHECK(1, "vector: bytes match {0x25, 0x00, 0x02}");
    }
}

static void test_decode_too_short(void)
{
    /* A truncated/old-format frame -- e.g. a 1-byte cmd-only frame the way
     * SAFETY_CMD_ROLLBACK itself is shaped, in case something upstream ever
     * confuses the two ids' framing. */
    uint8_t buf[2] = {KILNLINK_ROLLBACK_RESULT_CMD, 0};
    kilnlink_rollback_result_t out;
    CHECK(kilnlink_rollback_result_decode(buf, sizeof(buf), &out) == KILNLINK_ROLLBACK_RESULT_ERR_LENGTH_MISMATCH,
          "decode() of a 2-byte (one short) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_too_long(void)
{
    uint8_t buf[KILNLINK_ROLLBACK_RESULT_LEN + 1] = {0};
    buf[0] = KILNLINK_ROLLBACK_RESULT_CMD;
    kilnlink_rollback_result_t out;
    CHECK(kilnlink_rollback_result_decode(buf, sizeof(buf), &out) == KILNLINK_ROLLBACK_RESULT_ERR_LENGTH_MISMATCH,
          "decode() of a 4-byte (one too many) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_ROLLBACK_RESULT_LEN] = {0};
    buf[0] = KILNLINK_ROLLBACK_CMD; /* 0x17, SAFETY_CMD_ROLLBACK's id, not the result's */
    kilnlink_rollback_result_t out;
    CHECK(kilnlink_rollback_result_decode(buf, sizeof(buf), &out) == KILNLINK_ROLLBACK_RESULT_ERR_WRONG_CMD,
          "decode() with wrong command byte -> ERR_WRONG_CMD");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_rollback_result_t msg = {0};
    uint8_t buf[1];
    kilnlink_rollback_result_status_t status;
    size_t n = kilnlink_rollback_result_encode(&msg, buf, 0, &status);
    CHECK(n == 0, "encode() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_ROLLBACK_RESULT_ERR_BUFFER_TOO_SMALL,
          "encode() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
}

int main(void)
{
    test_round_trip_refused();
    test_round_trip_accepted_bit();
    test_all_reasons_round_trip();
    test_vector();
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
