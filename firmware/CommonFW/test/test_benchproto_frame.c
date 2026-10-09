/* Host-native test for benchproto_frame.{c,h} -- no board, no toolchain
 * beyond a plain C11 host compiler. Exercises round-trip encode/decode, the
 * byte-exact vectors in test/vectors/benchproto_frame_vectors.json (computed
 * independently in Python, see that file's header comment for how), and the
 * hostile-input set CommonFW/README.md's checklist asks for (kilnlink's own
 * test_frame.c follows the identical shape -- deliberately mirrored here for
 * the sibling protocol): truncated frames, an over-long length byte, a
 * length/buffer mismatch, a bad CRC, an unknown type byte, and an
 * unterminated trailing escape.
 *
 * Build (MSVC host compiler, no CMake needed for this one-shot check):
 *   cl /nologo /W4 /I ..\include test_benchproto_frame.c ..\src\benchproto_frame.c ..\src\benchproto_crc.c
 *
 * Exits 0 and prints "ALL PASS" if every check passes; on the first failure
 * it prints which one and exits 1 -- deliberately fails fast rather than
 * collecting every failure, since a broken CRC or framing routine tends to
 * cascade into failures in everything downstream of it. */

#include <stdio.h>
#include <string.h>

#include "benchproto/benchproto_frame.h"

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

/* -- CRC ------------------------------------------------------------------- */

static void test_crc(void)
{
    CHECK(benchproto_crc16_ccitt_false(NULL, 0) == 0xFFFF, "crc of empty input is 0xFFFF");

    /* Known-answer test for CRC16/CCITT-FALSE: CRC("123456789") = 0x29B1 --
     * see test/vectors/benchproto_frame_vectors.json's crc_known_answer.
     * If this fails, the algorithm itself is wrong, not just this
     * codebase's usage of it. */
    uint16_t crc = benchproto_crc16_ccitt_false((const uint8_t *)"123456789", 9);
    CHECK(crc == 0x29B1, "CRC16/CCITT-FALSE(\"123456789\") == 0x29B1");
}

/* -- stuff/unstuff ----------------------------------------------------------- */

static void test_stuffing_round_trip(void)
{
    uint8_t raw[] = {0x01, 0x7E, 0x02, 0x7D, 0x03, 0x7E, 0x7D};
    uint8_t stuffed[64];
    size_t stuffed_len = benchproto_stuff(raw, sizeof(raw), stuffed, sizeof(stuffed));
    CHECK(stuffed_len > 0, "stuff() succeeds with ample buffer");
    CHECK(stuffed[0] == BENCHPROTO_FRAME_DELIM, "stuffed frame opens with delimiter");
    CHECK(stuffed[stuffed_len - 1] == BENCHPROTO_FRAME_DELIM, "stuffed frame closes with delimiter");

    uint8_t unstuffed[64];
    benchproto_frame_status_t status;
    size_t unstuffed_len =
        benchproto_unstuff(stuffed, stuffed_len, unstuffed, sizeof(unstuffed), &status);
    CHECK(status == BENCHPROTO_FRAME_OK, "unstuff() reports OK");
    CHECK(unstuffed_len == sizeof(raw), "unstuff() recovers the original length");
    CHECK(memcmp(unstuffed, raw, sizeof(raw)) == 0, "unstuff(stuff(x)) == x");

    size_t bare_len = benchproto_unstuff(stuffed + 1, stuffed_len - 2, unstuffed, sizeof(unstuffed),
                                         &status);
    CHECK(status == BENCHPROTO_FRAME_OK, "unstuff() without delimiters still OK");
    CHECK(bare_len == sizeof(raw) && memcmp(unstuffed, raw, sizeof(raw)) == 0,
          "unstuff() without delimiters matches with-delimiters result");
}

static void test_stuff_buffer_too_small(void)
{
    uint8_t raw[8] = {0};
    uint8_t out[4]; /* far too small: needs up to 2*8+2 = 18 */
    size_t n = benchproto_stuff(raw, sizeof(raw), out, sizeof(out));
    CHECK(n == 0, "stuff() with an undersized buffer returns 0");
}

static void test_unstuff_unterminated_escape(void)
{
    uint8_t in[] = {0x01, 0x02, BENCHPROTO_FRAME_ESC};
    uint8_t out[16];
    benchproto_frame_status_t status;
    size_t n = benchproto_unstuff(in, sizeof(in), out, sizeof(out), &status);
    CHECK(n == 0, "unstuff() with a dangling escape returns 0");
    CHECK(status == BENCHPROTO_FRAME_ERR_UNTERMINATED_ESC,
          "unstuff() with a dangling escape reports ERR_UNTERMINATED_ESC");
}

/* -- frame encode/decode round trip ----------------------------------------- */

static void test_frame_round_trip(void)
{
    uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    benchproto_frame_t frame = {
        .msg_type = BENCHPROTO_MSG_DATA,
        .msg_index = 5,
        .src_device = 0,
        .src_task = 3,
        .dst_device = 1,
        .dst_task = 3,
        .length = (uint8_t)sizeof(payload),
        .payload = payload,
    };

    uint8_t raw[BENCHPROTO_FRAME_RAW_MAX];
    benchproto_frame_status_t status;
    size_t raw_len = benchproto_frame_encode_raw(&frame, raw, sizeof(raw), &status);
    CHECK(status == BENCHPROTO_FRAME_OK, "encode_raw() reports OK");
    CHECK(raw_len == BENCHPROTO_FRAME_HEADER_LEN + sizeof(payload) + BENCHPROTO_FRAME_CRC_LEN,
          "encode_raw() writes header+payload+crc, nothing more");

    benchproto_frame_t decoded;
    benchproto_frame_status_t dstatus = benchproto_frame_decode(raw, raw_len, &decoded);
    CHECK(dstatus == BENCHPROTO_FRAME_OK, "decode() reports OK for a just-encoded frame");
    CHECK(decoded.msg_type == frame.msg_type, "decoded msg_type matches");
    CHECK(decoded.msg_index == frame.msg_index, "decoded msg_index matches");
    CHECK(decoded.src_device == frame.src_device, "decoded src_device matches");
    CHECK(decoded.src_task == frame.src_task, "decoded src_task matches");
    CHECK(decoded.dst_device == frame.dst_device, "decoded dst_device matches");
    CHECK(decoded.dst_task == frame.dst_task, "decoded dst_task matches");
    CHECK(decoded.length == frame.length, "decoded length matches");
    CHECK(memcmp(decoded.payload, payload, sizeof(payload)) == 0, "decoded payload matches");

    uint8_t wire[BENCHPROTO_FRAME_STUFFED_MAX];
    size_t wire_len = benchproto_stuff(raw, raw_len, wire, sizeof(wire));
    CHECK(wire_len > 0, "stuff() succeeds on an encoded frame");
    uint8_t unstuffed[BENCHPROTO_FRAME_RAW_MAX];
    size_t unstuffed_len =
        benchproto_unstuff(wire, wire_len, unstuffed, sizeof(unstuffed), &dstatus);
    CHECK(dstatus == BENCHPROTO_FRAME_OK, "unstuff() of a freshly-stuffed frame reports OK");
    CHECK(unstuffed_len == raw_len && memcmp(unstuffed, raw, raw_len) == 0,
          "unstuff(stuff(encode(frame))) == encode(frame)");
}

static void test_zero_length_payload(void)
{
    benchproto_frame_t frame = {
        .msg_type = BENCHPROTO_MSG_ACK,
        .msg_index = 0,
        .src_device = 1,
        .src_task = 7,
        .dst_device = 0,
        .dst_task = 7,
        .length = 0,
        .payload = NULL,
    };
    uint8_t raw[BENCHPROTO_FRAME_RAW_MAX];
    benchproto_frame_status_t status;
    size_t raw_len = benchproto_frame_encode_raw(&frame, raw, sizeof(raw), &status);
    CHECK(status == BENCHPROTO_FRAME_OK, "encode_raw() with a NULL zero-length payload is OK");
    CHECK(raw_len == BENCHPROTO_FRAME_HEADER_LEN + BENCHPROTO_FRAME_CRC_LEN,
          "zero-length payload frame is exactly header+crc");

    benchproto_frame_t decoded;
    CHECK(benchproto_frame_decode(raw, raw_len, &decoded) == BENCHPROTO_FRAME_OK,
          "decode() of a zero-length-payload frame is OK");
    CHECK(decoded.length == 0, "decoded zero-length payload has length 0");
}

/* -- byte-exact vectors -------------------------------------------------------
 *
 * From test/vectors/benchproto_frame_vectors.json, computed by an
 * independent Python re-implementation of this exact spec (see that file's
 * header comment). Unlike kilnlink's cross-implementation vector, there is
 * no second *production* implementation to check against yet -- these guard
 * against benchproto_frame.c regressing from its own frozen spec, and are
 * the baseline SimFW's future PC-side `kilnsim` link layer should be
 * checked against once it exists (see DESIGN_NOTES.md sec 6's "prove-it-twice"
 * note). */

static void test_vector_data_with_payload(void)
{
    static const uint8_t expected_wire[] = {
        0x7e, 0x01, 0x00, 0x05, 0x00, 0x03, 0x01, 0x03, 0x04, 0xde, 0xad, 0xbe, 0xef, 0xb6, 0x59,
        0x7e,
    };
    uint8_t payload[] = {0xde, 0xad, 0xbe, 0xef};
    benchproto_frame_t frame = {
        .msg_type = BENCHPROTO_MSG_DATA,
        .msg_index = 5,
        .src_device = 0,
        .src_task = 3,
        .dst_device = 1,
        .dst_task = 3,
        .length = (uint8_t)sizeof(payload),
        .payload = payload,
    };

    uint8_t raw[BENCHPROTO_FRAME_RAW_MAX];
    benchproto_frame_status_t status;
    size_t raw_len = benchproto_frame_encode_raw(&frame, raw, sizeof(raw), &status);
    CHECK(status == BENCHPROTO_FRAME_OK, "vector data_with_payload: encode OK");

    uint8_t wire[BENCHPROTO_FRAME_STUFFED_MAX];
    size_t wire_len = benchproto_stuff(raw, raw_len, wire, sizeof(wire));
    if (wire_len != sizeof(expected_wire) || memcmp(wire, expected_wire, sizeof(expected_wire)) != 0) {
        print_hex("  got     ", wire, wire_len);
        print_hex("  expected", expected_wire, sizeof(expected_wire));
        CHECK(0, "vector data_with_payload: wire bytes match the manifest");
    } else {
        CHECK(1, "vector data_with_payload: wire bytes match the manifest");
    }
}

static void test_vector_ack_zero_length(void)
{
    static const uint8_t expected_raw[] = {0x02, 0x00, 0x00, 0x01, 0x07, 0x00, 0x07, 0x00,
                                           0xdc, 0x73};
    benchproto_frame_t frame = {
        .msg_type = BENCHPROTO_MSG_ACK,
        .msg_index = 0,
        .src_device = 1,
        .src_task = 7,
        .dst_device = 0,
        .dst_task = 7,
        .length = 0,
        .payload = NULL,
    };
    uint8_t raw[BENCHPROTO_FRAME_RAW_MAX];
    benchproto_frame_status_t status;
    size_t raw_len = benchproto_frame_encode_raw(&frame, raw, sizeof(raw), &status);
    CHECK(status == BENCHPROTO_FRAME_OK, "vector ack_zero_length: encode OK");
    CHECK(raw_len == sizeof(expected_raw) && memcmp(raw, expected_raw, raw_len) == 0,
          "vector ack_zero_length: raw bytes match the manifest");
}

static void test_vector_broadcast_needs_escaping(void)
{
    static const uint8_t expected_wire[] = {
        0x7e, 0x04, 0x01, 0x2c, 0x01, 0x05, 0x00, 0x05, 0x05, 0x01, 0x7d, 0x5e, 0x02, 0x7d,
        0x5d, 0x03, 0x3f, 0x30, 0x7e,
    };
    uint8_t payload[] = {0x01, 0x7e, 0x02, 0x7d, 0x03};
    benchproto_frame_t frame = {
        .msg_type = BENCHPROTO_MSG_BROADCAST,
        .msg_index = 300,
        .src_device = 1,
        .src_task = 5,
        .dst_device = 0,
        .dst_task = 5,
        .length = (uint8_t)sizeof(payload),
        .payload = payload,
    };
    uint8_t raw[BENCHPROTO_FRAME_RAW_MAX];
    benchproto_frame_status_t status;
    size_t raw_len = benchproto_frame_encode_raw(&frame, raw, sizeof(raw), &status);
    CHECK(status == BENCHPROTO_FRAME_OK, "vector broadcast_needs_escaping: encode OK");

    uint8_t wire[BENCHPROTO_FRAME_STUFFED_MAX];
    size_t wire_len = benchproto_stuff(raw, raw_len, wire, sizeof(wire));
    if (wire_len != sizeof(expected_wire) || memcmp(wire, expected_wire, sizeof(expected_wire)) != 0) {
        print_hex("  got     ", wire, wire_len);
        print_hex("  expected", expected_wire, sizeof(expected_wire));
        CHECK(0, "vector broadcast_needs_escaping: wire bytes match the manifest");
    } else {
        CHECK(1, "vector broadcast_needs_escaping: wire bytes match the manifest");
    }
}

static void test_vector_nack_zero_length(void)
{
    static const uint8_t expected_raw[] = {0x03, 0x00, 0x2a, 0x01, 0x09, 0x00, 0x02, 0x00,
                                           0x75, 0x05};
    benchproto_frame_t frame = {
        .msg_type = BENCHPROTO_MSG_NACK,
        .msg_index = 42,
        .src_device = 1,
        .src_task = 9,
        .dst_device = 0,
        .dst_task = 2,
        .length = 0,
        .payload = NULL,
    };
    uint8_t raw[BENCHPROTO_FRAME_RAW_MAX];
    benchproto_frame_status_t status;
    size_t raw_len = benchproto_frame_encode_raw(&frame, raw, sizeof(raw), &status);
    CHECK(status == BENCHPROTO_FRAME_OK, "vector nack_zero_length: encode OK");
    CHECK(raw_len == sizeof(expected_raw) && memcmp(raw, expected_raw, raw_len) == 0,
          "vector nack_zero_length: raw bytes match the manifest");
}

/* -- decode hostile inputs ---------------------------------------------------
 *
 * A payload is untrusted input arriving over a link to a PC. Every one of
 * these must be rejected with a specific status, never a crash, never a
 * silently-accepted garbage frame. */

static void test_decode_too_short(void)
{
    uint8_t raw[BENCHPROTO_FRAME_HEADER_LEN + BENCHPROTO_FRAME_CRC_LEN - 1] = {0};
    benchproto_frame_t out;
    CHECK(benchproto_frame_decode(raw, sizeof(raw), &out) == BENCHPROTO_FRAME_ERR_TOO_SHORT,
          "decode() of a too-short buffer -> ERR_TOO_SHORT");
    CHECK(benchproto_frame_decode(raw, 0, &out) == BENCHPROTO_FRAME_ERR_TOO_SHORT,
          "decode() of an empty buffer -> ERR_TOO_SHORT");
}

static void test_decode_length_too_long(void)
{
    uint8_t raw[BENCHPROTO_FRAME_HEADER_LEN + BENCHPROTO_FRAME_CRC_LEN] = {0};
    raw[7] = (uint8_t)(BENCHPROTO_FRAME_MAX_PAYLOAD + 1); /* > MAX_PAYLOAD (128) */
    benchproto_frame_t out;
    CHECK(benchproto_frame_decode(raw, sizeof(raw), &out) == BENCHPROTO_FRAME_ERR_LENGTH_TOO_LONG,
          "decode() with LENGTH > MAX_PAYLOAD -> ERR_LENGTH_TOO_LONG, regardless of buffer size");
}

static void test_decode_length_mismatch(void)
{
    uint8_t raw[BENCHPROTO_FRAME_HEADER_LEN + BENCHPROTO_FRAME_CRC_LEN] = {0};
    raw[7] = 5;
    benchproto_frame_t out;
    CHECK(benchproto_frame_decode(raw, sizeof(raw), &out) == BENCHPROTO_FRAME_ERR_LENGTH_MISMATCH,
          "decode() with LENGTH not matching buffer size -> ERR_LENGTH_MISMATCH");
}

static void test_decode_bad_crc(void)
{
    benchproto_frame_t frame = {
        .msg_type = BENCHPROTO_MSG_BROADCAST,
        .msg_index = 42,
        .src_device = 1,
        .src_task = 7,
        .dst_device = 0,
        .dst_task = 7,
        .length = 0,
        .payload = NULL,
    };
    uint8_t raw[BENCHPROTO_FRAME_RAW_MAX];
    benchproto_frame_status_t status;
    size_t raw_len = benchproto_frame_encode_raw(&frame, raw, sizeof(raw), &status);
    CHECK(status == BENCHPROTO_FRAME_OK, "bad-crc setup: encode_raw() OK");

    raw[raw_len - 1] ^= 0xFF; /* flip the low CRC byte */

    benchproto_frame_t out;
    CHECK(benchproto_frame_decode(raw, raw_len, &out) == BENCHPROTO_FRAME_ERR_CRC,
          "decode() with a corrupted CRC byte -> ERR_CRC");
}

static void test_decode_unknown_type(void)
{
    uint8_t raw[BENCHPROTO_FRAME_HEADER_LEN + BENCHPROTO_FRAME_CRC_LEN];
    memset(raw, 0, sizeof(raw));
    raw[0] = 0x99; /* not DATA/ACK/NACK/BROADCAST */
    raw[7] = 0;    /* length 0 */
    uint16_t crc = benchproto_crc16_ccitt_false(raw, BENCHPROTO_FRAME_HEADER_LEN);
    raw[BENCHPROTO_FRAME_HEADER_LEN] = (uint8_t)(crc >> 8);
    raw[BENCHPROTO_FRAME_HEADER_LEN + 1] = (uint8_t)(crc & 0xFF);

    benchproto_frame_t out;
    CHECK(benchproto_frame_decode(raw, sizeof(raw), &out) == BENCHPROTO_FRAME_ERR_UNKNOWN_TYPE,
          "decode() with an unrecognized type byte -> ERR_UNKNOWN_TYPE (CRC still valid, "
          "so this is genuinely testing the type check, not a CRC false-positive)");
}

static void test_encode_length_too_long(void)
{
    uint8_t oversized_payload[BENCHPROTO_FRAME_MAX_PAYLOAD + 1] = {0};
    benchproto_frame_t frame = {
        .msg_type = BENCHPROTO_MSG_DATA,
        .msg_index = 0,
        .src_device = 0,
        .src_task = 0,
        .dst_device = 0,
        .dst_task = 0,
        .length = BENCHPROTO_FRAME_MAX_PAYLOAD + 1, /* 129: still fits a uint8_t, exceeds MAX_PAYLOAD */
        .payload = oversized_payload,
    };
    uint8_t raw[BENCHPROTO_FRAME_RAW_MAX + 16];
    benchproto_frame_status_t status;
    size_t n = benchproto_frame_encode_raw(&frame, raw, sizeof(raw), &status);
    CHECK(n == 0, "encode_raw() with length > MAX_PAYLOAD writes nothing");
    CHECK(status == BENCHPROTO_FRAME_ERR_LENGTH_TOO_LONG,
          "encode_raw() with length > MAX_PAYLOAD -> ERR_LENGTH_TOO_LONG");
}

static void test_encode_buffer_too_small(void)
{
    uint8_t payload[4] = {1, 2, 3, 4};
    benchproto_frame_t frame = {
        .msg_type = BENCHPROTO_MSG_DATA,
        .msg_index = 0,
        .src_device = 0,
        .src_task = 0,
        .dst_device = 0,
        .dst_task = 0,
        .length = 4,
        .payload = payload,
    };
    uint8_t raw[4]; /* needs 8 + 4 + 2 = 14 */
    benchproto_frame_status_t status;
    size_t n = benchproto_frame_encode_raw(&frame, raw, sizeof(raw), &status);
    CHECK(n == 0, "encode_raw() with an undersized output buffer writes nothing");
    CHECK(status == BENCHPROTO_FRAME_ERR_BUFFER_TOO_SMALL,
          "encode_raw() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
}

int main(void)
{
    test_crc();
    test_stuffing_round_trip();
    test_stuff_buffer_too_small();
    test_unstuff_unterminated_escape();
    test_frame_round_trip();
    test_zero_length_payload();
    test_vector_data_with_payload();
    test_vector_ack_zero_length();
    test_vector_broadcast_needs_escaping();
    test_vector_nack_zero_length();
    test_decode_too_short();
    test_decode_length_too_long();
    test_decode_length_mismatch();
    test_decode_bad_crc();
    test_decode_unknown_type();
    test_encode_length_too_long();
    test_encode_buffer_too_small();

    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
