/* Host-native test for kilnlink_status.{c,h} -- the Pico->ESP
 * SAFETY_CMD_GET_STATUS (0x01, Frame A) codec, docs/LINK_PROTOCOL.md sec 6.
 * Mirrors test_frame.c/test_context.c's structure: round-trip encode/decode,
 * byte-exact vectors from test/vectors/status_vectors.json, and the hostile
 * input set: too-short, too-long (this is a fixed-size frame, not a minimum
 * size), wrong command byte.
 *
 * Build (MSVC host compiler, no CMake needed for this one-shot check):
 *   cl /nologo /W4 /I ..\include test_status.c ..\src\kilnlink_status.c
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_status.h"

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

/* -- round trip --------------------------------------------------------- */

static void test_round_trip_healthy(void)
{
    kilnlink_status_t st = {0};
    st.flags = KILNLINK_STATUS_FLAG_ESTOP | KILNLINK_STATUS_FLAG_RELAY |
               KILNLINK_STATUS_FLAG_ENABLED | KILNLINK_STATUS_FLAG_TEMP_VALID;
    st.safety_tc_c = 875.5f;
    st.cold_junction_c = 23.75f;
    st.tc_fault = 0;
    st.current1_a = 1.5f;
    st.current2_a = 2.25f;
    st.current3_a = 0.0f;

    uint8_t buf[KILNLINK_STATUS_LEN];
    kilnlink_status_status_t status;
    size_t n = kilnlink_status_encode(&st, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_STATUS_OK, "encode() reports OK");
    CHECK(n == KILNLINK_STATUS_LEN, "encode() always writes exactly 23 bytes");

    kilnlink_status_t decoded;
    kilnlink_status_status_t dstatus = kilnlink_status_decode(buf, n, &decoded);
    CHECK(dstatus == KILNLINK_STATUS_OK, "decode() reports OK for a just-encoded payload");
    CHECK(decoded.flags == st.flags, "decoded flags match");
    CHECK(decoded.safety_tc_c == st.safety_tc_c, "decoded safety_tc_c matches");
    CHECK(decoded.cold_junction_c == st.cold_junction_c, "decoded cold_junction_c matches");
    CHECK(decoded.tc_fault == st.tc_fault, "decoded tc_fault matches");
    CHECK(decoded.current1_a == st.current1_a, "decoded current1_a matches");
    CHECK(decoded.current2_a == st.current2_a, "decoded current2_a matches");
    CHECK(decoded.current3_a == st.current3_a, "decoded current3_a matches");
}

static void test_round_trip_temp_invalid_is_nan(void)
{
    /* LINK_PROTOCOL.md sec 6, Frame A: "Send NaN, never 0, when TEMP_VALID
     * is clear." */
    kilnlink_status_t st = {0};
    st.flags = 0; /* TEMP_VALID clear */
    st.safety_tc_c = (float)NAN;
    st.cold_junction_c = (float)NAN;
    st.tc_fault = 3;
    st.current1_a = 0.0f;
    st.current2_a = 0.0f;
    st.current3_a = 0.0f;

    uint8_t buf[KILNLINK_STATUS_LEN];
    kilnlink_status_status_t status;
    size_t n = kilnlink_status_encode(&st, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_STATUS_OK, "encode() with NaN temps reports OK");

    kilnlink_status_t decoded;
    CHECK(kilnlink_status_decode(buf, n, &decoded) == KILNLINK_STATUS_OK,
          "decode() of a NaN-temp payload is OK");
    CHECK(isnan(decoded.safety_tc_c), "safety_tc_c round-trips as NaN, not fixed up to 0");
    CHECK(isnan(decoded.cold_junction_c), "cold_junction_c round-trips as NaN, not fixed up to 0");
    CHECK(decoded.current1_a == 0.0f, "current1_a is a legitimate 0, not NaN -- the NaN rule is temps-only");
}

/* -- byte-exact vectors (test/vectors/status_vectors.json) --------------- */

static void test_vector_healthy_reading(void)
{
    static const uint8_t expected[] = {
        0x01, 0x3c, 0x00, 0xe0, 0x5a, 0x44, 0x00, 0x00, 0xbe, 0x41, 0x00, 0x00,
        0x00, 0xc0, 0x3f, 0x00, 0x00, 0x10, 0x40, 0x00, 0x00, 0x00, 0x00,
    };
    kilnlink_status_t st = {0};
    st.flags = 0x3C;
    st.safety_tc_c = 875.5f;
    st.cold_junction_c = 23.75f;
    st.tc_fault = 0;
    st.current1_a = 1.5f;
    st.current2_a = 2.25f;
    st.current3_a = 0.0f;

    uint8_t buf[KILNLINK_STATUS_LEN];
    kilnlink_status_status_t status;
    size_t n = kilnlink_status_encode(&st, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_STATUS_OK, "vector healthy_reading: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector healthy_reading: bytes match status_vectors.json");
    } else {
        CHECK(1, "vector healthy_reading: bytes match status_vectors.json");
    }
}

static void test_vector_temp_invalid(void)
{
    static const uint8_t expected[] = {
        0x01, 0x00, 0x00, 0x00, 0xc0, 0x7f, 0x00, 0x00, 0xc0, 0x7f, 0x03, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    kilnlink_status_t st = {0};
    st.flags = 0;
    st.safety_tc_c = (float)NAN;
    st.cold_junction_c = (float)NAN;
    st.tc_fault = 3;
    st.current1_a = 0.0f;
    st.current2_a = 0.0f;
    st.current3_a = 0.0f;

    uint8_t buf[KILNLINK_STATUS_LEN];
    kilnlink_status_status_t status;
    size_t n = kilnlink_status_encode(&st, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_STATUS_OK, "vector temp_invalid_sends_nan_not_zero: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector temp_invalid_sends_nan_not_zero: bytes match status_vectors.json");
    } else {
        CHECK(1, "vector temp_invalid_sends_nan_not_zero: bytes match status_vectors.json");
    }
}

/* -- decode hostile inputs ------------------------------------------------ */

static void test_decode_too_short(void)
{
    uint8_t buf[KILNLINK_STATUS_LEN - 1] = {0};
    buf[0] = KILNLINK_STATUS_CMD;
    kilnlink_status_t out;
    CHECK(kilnlink_status_decode(buf, sizeof(buf), &out) == KILNLINK_STATUS_ERR_LENGTH_MISMATCH,
          "decode() of a 22-byte (one short) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_too_long(void)
{
    /* 24 bytes is now a legal V2 frame (see the V2 tests below) -- "too
     * long" for this two-length codec means one byte past the LONGER of
     * the two legal lengths, not past the original 23. */
    uint8_t buf[KILNLINK_STATUS_LEN_V2 + 1] = {0};
    buf[0] = KILNLINK_STATUS_CMD;
    kilnlink_status_t out;
    CHECK(kilnlink_status_decode(buf, sizeof(buf), &out) == KILNLINK_STATUS_ERR_LENGTH_MISMATCH,
          "decode() of a 25-byte (one past the longer V2 length) payload -> ERR_LENGTH_MISMATCH");
}

/* -- V2 (protocol 6, optional tx_dropped_sat byte) ------------------------ */

static void test_round_trip_v2_tx_dropped(void)
{
    /* kilnlink_version.h's 5->6 addition: byte 23, tx_dropped_sat, present
     * only when the sender has negotiated it. Mirrors
     * link_frame_pack_status()'s own negotiation -- see kilnlink_status.h's
     * file comment. */
    kilnlink_status_t st = {0};
    st.flags = KILNLINK_STATUS_FLAG_ESTOP;
    st.safety_tc_c = 100.0f;
    st.cold_junction_c = 25.0f;
    st.tc_fault = 0;
    st.current1_a = 0.5f;
    st.current2_a = 0.0f;
    st.current3_a = 0.0f;
    st.has_tx_dropped = 1;
    st.tx_dropped_sat = 42;

    uint8_t buf[KILNLINK_STATUS_LEN_V2];
    kilnlink_status_status_t status;
    size_t n = kilnlink_status_encode(&st, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_STATUS_OK, "V2: encode() reports OK");
    CHECK(n == KILNLINK_STATUS_LEN_V2, "V2: encode() writes exactly 24 bytes when has_tx_dropped is set");
    CHECK(buf[23] == 42, "V2: byte 23 on the wire is tx_dropped_sat");

    kilnlink_status_t decoded;
    CHECK(kilnlink_status_decode(buf, n, &decoded) == KILNLINK_STATUS_OK, "V2: decode() of a 24-byte frame is OK");
    CHECK(decoded.has_tx_dropped == 1, "V2: decode() sets has_tx_dropped for a 24-byte frame");
    CHECK(decoded.tx_dropped_sat == 42, "V2: tx_dropped_sat round-trips");
    CHECK(decoded.flags == st.flags, "V2: flags round-trips alongside the new byte");
}

static void test_encode_v1_when_has_tx_dropped_clear(void)
{
    /* has_tx_dropped == 0 is the "peer never negotiated V2" case -- encode()
     * must fall back to the original 23-byte layout, not silently emit a
     * 24th byte of stale/garbage tx_dropped_sat. */
    kilnlink_status_t st = {0};
    st.has_tx_dropped = 0;
    st.tx_dropped_sat = 0xAAu; /* garbage the encoder must ignore -- not on the wire at all */

    uint8_t buf[KILNLINK_STATUS_LEN_V2] = {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
                                            0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
                                            0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu};
    kilnlink_status_status_t status;
    size_t n = kilnlink_status_encode(&st, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_STATUS_OK, "V1 fallback: encode() reports OK");
    CHECK(n == KILNLINK_STATUS_LEN_V1, "V1 fallback: encode() writes exactly 23 bytes when has_tx_dropped is clear");
    CHECK(buf[23] == 0xFFu, "V1 fallback: byte 23 is untouched (not part of the returned length)");
}

static void test_decode_absent_tx_dropped_distinguishable_from_real_zero(void)
{
    /* The whole point of has_tx_dropped: a 23-byte (V1) frame must decode
     * to has_tx_dropped == 0 -- "not sent" -- and must NOT be
     * indistinguishable from a 24-byte (V2) frame that legitimately carries
     * tx_dropped_sat == 0 ("sent, and the real count is zero"). Collapsing
     * those two into the same observable state is exactly the bug this
     * struct field exists to prevent. */
    kilnlink_status_t st = {0};
    st.tx_dropped_sat = 7; /* must be ignored by encode() while has_tx_dropped == 0 */

    uint8_t buf_v1[KILNLINK_STATUS_LEN_V1];
    kilnlink_status_status_t status;
    size_t n1 = kilnlink_status_encode(&st, buf_v1, sizeof(buf_v1), &status);
    CHECK(n1 == KILNLINK_STATUS_LEN_V1, "distinguishability: V1 frame is 23 bytes");

    kilnlink_status_t absent;
    CHECK(kilnlink_status_decode(buf_v1, n1, &absent) == KILNLINK_STATUS_OK, "distinguishability: V1 decode OK");
    CHECK(absent.has_tx_dropped == 0, "distinguishability: a V1 (23-byte) frame decodes has_tx_dropped == 0");

    kilnlink_status_t st_zero = {0};
    st_zero.has_tx_dropped = 1;
    st_zero.tx_dropped_sat = 0; /* a REAL zero -- peer is on V2 and has genuinely dropped nothing */

    uint8_t buf_v2[KILNLINK_STATUS_LEN_V2];
    size_t n2 = kilnlink_status_encode(&st_zero, buf_v2, sizeof(buf_v2), &status);
    CHECK(n2 == KILNLINK_STATUS_LEN_V2, "distinguishability: V2 frame is 24 bytes");

    kilnlink_status_t real_zero;
    CHECK(kilnlink_status_decode(buf_v2, n2, &real_zero) == KILNLINK_STATUS_OK, "distinguishability: V2 decode OK");
    CHECK(real_zero.has_tx_dropped == 1,
          "distinguishability: a V2 (24-byte) frame decodes has_tx_dropped == 1, even when the count is 0");
    CHECK(real_zero.tx_dropped_sat == 0, "distinguishability: the real count (0) round-trips");

    /* The actual proof: these two decoded results must differ in
     * has_tx_dropped even though tx_dropped_sat reads 0 in both. */
    CHECK(absent.has_tx_dropped != real_zero.has_tx_dropped,
          "distinguishability: absent (V1) and real-zero (V2) are NOT the same observable state");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_STATUS_LEN] = {0};
    buf[0] = 0x08; /* DIAG's id, not GET_STATUS's */
    kilnlink_status_t out;
    CHECK(kilnlink_status_decode(buf, sizeof(buf), &out) == KILNLINK_STATUS_ERR_WRONG_CMD,
          "decode() with wrong command byte -> ERR_WRONG_CMD");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_status_t st = {0};
    uint8_t buf[10]; /* needs 23 */
    kilnlink_status_status_t status;
    size_t n = kilnlink_status_encode(&st, buf, sizeof(buf), &status);
    CHECK(n == 0, "encode() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_STATUS_ERR_BUFFER_TOO_SMALL,
          "encode() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
}

int main(void)
{
    test_round_trip_healthy();
    test_round_trip_temp_invalid_is_nan();
    test_vector_healthy_reading();
    test_vector_temp_invalid();
    test_decode_too_short();
    test_decode_too_long();
    test_decode_wrong_cmd();
    test_encode_buffer_too_small();
    test_round_trip_v2_tx_dropped();
    test_encode_v1_when_has_tx_dropped_clear();
    test_decode_absent_tx_dropped_distinguishable_from_real_zero();

    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
