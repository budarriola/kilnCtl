/* Host-native test for kilnlink_trip.{c,h} -- the Pico->ESP SAFETY_CMD_TRIP_EVENT
 * (0x0D, Frame D) codec, docs/LINK_PROTOCOL.md sec 6. Mirrors test_diag.c's
 * structure: round-trip encode/decode, byte-exact vectors from
 * test/vectors/trip_vectors.json, and the hostile input set: too-short,
 * too-long (fixed-size frame), wrong command byte.
 */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_trip.h"

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

static int f32_eq(float a, float b)
{
    /* exact equality is fine here: values round-trip through the same
     * IEEE-754 bit pattern, no arithmetic happens in between. */
    return a == b;
}

/* -- round trip --------------------------------------------------------- */

static void test_round_trip(void)
{
    kilnlink_trip_t tr = {0};
    tr.trip_seq = 1;
    tr.trip_reason = 9;
    tr.uptime_ms = 500000;
    tr.safety_tc_c = 245.5f;
    tr.deciding_threshold = 250.0f;
    tr.current_a[0] = 1.2f;
    tr.current_a[1] = 0.0f;
    tr.current_a[2] = 0.3f;
    tr.relay_recent_mask = 0x05;
    tr.context_age_100ms = 3;

    uint8_t buf[KILNLINK_TRIP_LEN];
    kilnlink_trip_status_t status;
    size_t n = kilnlink_trip_encode(&tr, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_TRIP_OK, "encode() reports OK");
    CHECK(n == KILNLINK_TRIP_LEN, "encode() always writes exactly 29 bytes");

    kilnlink_trip_t decoded;
    kilnlink_trip_status_t dstatus = kilnlink_trip_decode(buf, n, &decoded);
    CHECK(dstatus == KILNLINK_TRIP_OK, "decode() reports OK for a just-encoded payload");
    CHECK(decoded.trip_seq == tr.trip_seq, "decoded trip_seq matches");
    CHECK(decoded.trip_reason == tr.trip_reason, "decoded trip_reason matches");
    CHECK(decoded.uptime_ms == tr.uptime_ms, "decoded uptime_ms matches");
    CHECK(f32_eq(decoded.safety_tc_c, tr.safety_tc_c), "decoded safety_tc_c matches");
    CHECK(f32_eq(decoded.deciding_threshold, tr.deciding_threshold), "decoded deciding_threshold matches");
    for (unsigned ch = 0; ch < KILNLINK_TRIP_CHANNELS; ch++) {
        CHECK(f32_eq(decoded.current_a[ch], tr.current_a[ch]), "decoded current_a[ch] matches");
    }
    CHECK(decoded.relay_recent_mask == tr.relay_recent_mask, "decoded relay_recent_mask matches");
    CHECK(decoded.context_age_100ms == tr.context_age_100ms, "decoded context_age_100ms matches");
}

static void test_round_trip_never_received_context(void)
{
    /* LINK_PROTOCOL.md sec 6: context_age_100ms == 255 means "never
     * received" -- a trip can legitimately happen before the first
     * PUSH_CONTEXT ever arrives (e.g. a link-dead trip). */
    kilnlink_trip_t tr = {0};
    tr.trip_seq = 255;
    tr.trip_reason = 2;
    tr.context_age_100ms = 255;

    uint8_t buf[KILNLINK_TRIP_LEN];
    kilnlink_trip_status_t status;
    size_t n = kilnlink_trip_encode(&tr, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_TRIP_OK, "encode() with never-received context reports OK");

    kilnlink_trip_t decoded;
    CHECK(kilnlink_trip_decode(buf, n, &decoded) == KILNLINK_TRIP_OK, "decode() OK");
    CHECK(decoded.context_age_100ms == 255, "context_age_100ms round-trips as 255 (never)");
    CHECK(decoded.trip_seq == 255, "trip_seq round-trips at the u8 max (wraps, doesn't clamp)");
}

/* -- byte-exact vectors (test/vectors/trip_vectors.json) ---------------- */

static void test_vector_current_spike(void)
{
    static const uint8_t expected[] = {
        0x0d, 0x01, 0x09, 0x20, 0xa1, 0x07, 0x00, 0x00, 0x80, 0x75, 0x43, 0x00,
        0x00, 0x7a, 0x43, 0x9a, 0x99, 0x99, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x9a,
        0x99, 0x99, 0x3e, 0x05, 0x03,
    };
    kilnlink_trip_t tr = {0};
    tr.trip_seq = 1;
    tr.trip_reason = 9;
    tr.uptime_ms = 500000;
    tr.safety_tc_c = 245.5f;
    tr.deciding_threshold = 250.0f;
    tr.current_a[0] = 1.2f;
    tr.current_a[1] = 0.0f;
    tr.current_a[2] = 0.3f;
    tr.relay_recent_mask = 0x05;
    tr.context_age_100ms = 3;

    uint8_t buf[KILNLINK_TRIP_LEN];
    kilnlink_trip_status_t status;
    size_t n = kilnlink_trip_encode(&tr, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_TRIP_OK, "vector current_spike: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector current_spike: bytes match trip_vectors.json");
    } else {
        CHECK(1, "vector current_spike: bytes match trip_vectors.json");
    }
}

static void test_vector_wraparound_no_context(void)
{
    static const uint8_t expected[] = {
        0x0d, 0xff, 0x02, 0x87, 0xd6, 0x12, 0x00, 0x9a, 0xf9, 0x79, 0x44, 0x00,
        0x00, 0x61, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xff,
    };
    kilnlink_trip_t tr = {0};
    tr.trip_seq = 255;
    tr.trip_reason = 2;
    tr.uptime_ms = 1234567;
    tr.safety_tc_c = 999.9f;
    tr.deciding_threshold = 900.0f;
    tr.current_a[0] = 0.0f;
    tr.current_a[1] = 0.0f;
    tr.current_a[2] = 0.0f;
    tr.relay_recent_mask = 0;
    tr.context_age_100ms = 255;

    uint8_t buf[KILNLINK_TRIP_LEN];
    kilnlink_trip_status_t status;
    size_t n = kilnlink_trip_encode(&tr, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_TRIP_OK, "vector wraparound_no_context: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector wraparound_no_context: bytes match trip_vectors.json");
    } else {
        CHECK(1, "vector wraparound_no_context: bytes match trip_vectors.json");
    }
}

/* -- decode hostile inputs ------------------------------------------------ */

static void test_decode_too_short(void)
{
    uint8_t buf[KILNLINK_TRIP_LEN - 1] = {0};
    buf[0] = KILNLINK_TRIP_CMD;
    kilnlink_trip_t out;
    CHECK(kilnlink_trip_decode(buf, sizeof(buf), &out) == KILNLINK_TRIP_ERR_LENGTH_MISMATCH,
          "decode() of a 28-byte (one short) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_too_long(void)
{
    uint8_t buf[KILNLINK_TRIP_LEN + 1] = {0};
    buf[0] = KILNLINK_TRIP_CMD;
    kilnlink_trip_t out;
    CHECK(kilnlink_trip_decode(buf, sizeof(buf), &out) == KILNLINK_TRIP_ERR_LENGTH_MISMATCH,
          "decode() of a 30-byte (one too many) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_TRIP_LEN] = {0};
    buf[0] = 0x01; /* GET_STATUS's id, not TRIP_EVENT's */
    kilnlink_trip_t out;
    CHECK(kilnlink_trip_decode(buf, sizeof(buf), &out) == KILNLINK_TRIP_ERR_WRONG_CMD,
          "decode() with wrong command byte -> ERR_WRONG_CMD");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_trip_t tr = {0};
    uint8_t buf[10]; /* needs 29 */
    kilnlink_trip_status_t status;
    size_t n = kilnlink_trip_encode(&tr, buf, sizeof(buf), &status);
    CHECK(n == 0, "encode() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_TRIP_ERR_BUFFER_TOO_SMALL,
          "encode() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
}

int main(void)
{
    test_round_trip();
    test_round_trip_never_received_context();
    test_vector_current_spike();
    test_vector_wraparound_no_context();
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
