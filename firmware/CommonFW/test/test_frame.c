/* Host-native test for kilnlink_frame.{c,h} -- no board, no toolchain beyond
 * a plain C11 host compiler. Exercises round-trip encode/decode, a
 * byte-exact vector cross-checked against pc_tools/src/kilnctrl/protocol.py
 * (the second, already-proven implementation of this exact envelope), and
 * the hostile-input set CommonFW/README.md's checklist asks for: truncated
 * frames, an over-long length byte, a length/buffer mismatch, a bad CRC, an
 * unknown type byte, and an unterminated trailing escape.
 *
 * Build (MSVC host compiler, no CMake needed for this one-shot check):
 *   cl /nologo /W4 /I ..\include test_frame.c ..\src\kilnlink_frame.c ..\src\kilnlink_crc.c
 *
 * Exits 0 and prints "ALL PASS" if every check passes; on the first failure
 * it prints which one and exits 1 -- deliberately fails fast rather than
 * collecting every failure, since a broken CRC or framing routine tends to
 * cascade into failures in everything downstream of it. */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_frame.h"

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
    /* CRC16/CCITT-FALSE of an empty message is the init value, unmodified. */
    CHECK(kilnlink_crc16_ccitt_false(NULL, 0) == 0xFFFF, "crc of empty input is 0xFFFF");

    /* Known-answer test for CRC16/CCITT-FALSE: CRC("123456789") = 0x29B1.
     * This is the standard test vector for this exact CRC variant (poly
     * 0x1021, init 0xFFFF, no reflection, no final xor) -- if this fails,
     * the algorithm itself is wrong, not just this codebase's usage of it. */
    uint16_t crc = kilnlink_crc16_ccitt_false((const uint8_t *)"123456789", 9);
    CHECK(crc == 0x29B1, "CRC16/CCITT-FALSE(\"123456789\") == 0x29B1");
}

/* -- stuff/unstuff ----------------------------------------------------------- */

static void test_stuffing_round_trip(void)
{
    /* A payload deliberately containing both bytes that must be escaped
     * (0x7E, 0x7D) plus ordinary bytes, so both the escape path and the
     * pass-through path are exercised in one buffer. */
    uint8_t raw[] = {0x01, 0x7E, 0x02, 0x7D, 0x03, 0x7E, 0x7D};
    uint8_t stuffed[64];
    size_t stuffed_len = kilnlink_stuff(raw, sizeof(raw), stuffed, sizeof(stuffed));
    CHECK(stuffed_len > 0, "stuff() succeeds with ample buffer");
    CHECK(stuffed[0] == KILNLINK_FRAME_DELIM, "stuffed frame opens with delimiter");
    CHECK(stuffed[stuffed_len - 1] == KILNLINK_FRAME_DELIM, "stuffed frame closes with delimiter");

    uint8_t unstuffed[64];
    kilnlink_frame_status_t status;
    size_t unstuffed_len =
        kilnlink_unstuff(stuffed, stuffed_len, unstuffed, sizeof(unstuffed), &status);
    CHECK(status == KILNLINK_FRAME_OK, "unstuff() reports OK");
    CHECK(unstuffed_len == sizeof(raw), "unstuff() recovers the original length");
    CHECK(memcmp(unstuffed, raw, sizeof(raw)) == 0, "unstuff(stuff(x)) == x");

    /* kilnlink_unstuff must also accept input with the delimiters already
     * stripped (a caller decoding a captured/hex-pasted frame body). */
    size_t bare_len = kilnlink_unstuff(stuffed + 1, stuffed_len - 2, unstuffed, sizeof(unstuffed),
                                       &status);
    CHECK(status == KILNLINK_FRAME_OK, "unstuff() without delimiters still OK");
    CHECK(bare_len == sizeof(raw) && memcmp(unstuffed, raw, sizeof(raw)) == 0,
          "unstuff() without delimiters matches with-delimiters result");
}

static void test_stuff_buffer_too_small(void)
{
    uint8_t raw[8] = {0};
    uint8_t out[4]; /* far too small: needs up to 2*8+2 = 18 */
    size_t n = kilnlink_stuff(raw, sizeof(raw), out, sizeof(out));
    CHECK(n == 0, "stuff() with an undersized buffer returns 0");
}

static void test_unstuff_unterminated_escape(void)
{
    /* 0x7E 0x01 0x7D 0x7E -- the trailing 0x7D promises an escaped byte that
     * never arrives (the following 0x7E is consumed as the closing
     * delimiter, not as the escaped byte, by the same rule a live receiver
     * uses: delimiters always resync regardless of escape state). Construct
     * the malformed case directly instead, with no closing delimiter at all,
     * so the escape is unambiguously dangling. */
    uint8_t in[] = {0x01, 0x02, KILNLINK_FRAME_ESC};
    uint8_t out[16];
    kilnlink_frame_status_t status;
    size_t n = kilnlink_unstuff(in, sizeof(in), out, sizeof(out), &status);
    CHECK(n == 0, "unstuff() with a dangling escape returns 0");
    CHECK(status == KILNLINK_FRAME_ERR_UNTERMINATED_ESC,
          "unstuff() with a dangling escape reports ERR_UNTERMINATED_ESC");
}

/* -- frame encode/decode round trip ----------------------------------------- */

static void test_frame_round_trip(void)
{
    uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    kilnlink_frame_t frame = {
        .msg_type = KILNLINK_MSG_DATA,
        .msg_index = 5,
        .src_device = 1, /* HOST */
        .src_task = 3,
        .dst_device = 0, /* ESP */
        .dst_task = 3,
        .length = (uint8_t)sizeof(payload),
        .payload = payload,
    };

    uint8_t raw[KILNLINK_FRAME_RAW_MAX];
    kilnlink_frame_status_t status;
    size_t raw_len = kilnlink_frame_encode_raw(&frame, raw, sizeof(raw), &status);
    CHECK(status == KILNLINK_FRAME_OK, "encode_raw() reports OK");
    CHECK(raw_len == KILNLINK_FRAME_HEADER_LEN + sizeof(payload) + KILNLINK_FRAME_CRC_LEN,
          "encode_raw() writes header+payload+crc, nothing more");

    kilnlink_frame_t decoded;
    kilnlink_frame_status_t dstatus = kilnlink_frame_decode(raw, raw_len, &decoded);
    CHECK(dstatus == KILNLINK_FRAME_OK, "decode() reports OK for a just-encoded frame");
    CHECK(decoded.msg_type == frame.msg_type, "decoded msg_type matches");
    CHECK(decoded.msg_index == frame.msg_index, "decoded msg_index matches");
    CHECK(decoded.src_device == frame.src_device, "decoded src_device matches");
    CHECK(decoded.src_task == frame.src_task, "decoded src_task matches");
    CHECK(decoded.dst_device == frame.dst_device, "decoded dst_device matches");
    CHECK(decoded.dst_task == frame.dst_task, "decoded dst_task matches");
    CHECK(decoded.length == frame.length, "decoded length matches");
    CHECK(memcmp(decoded.payload, payload, sizeof(payload)) == 0, "decoded payload matches");

    /* Full round trip through stuffing too. */
    uint8_t wire[KILNLINK_FRAME_STUFFED_MAX];
    size_t wire_len = kilnlink_stuff(raw, raw_len, wire, sizeof(wire));
    CHECK(wire_len > 0, "stuff() succeeds on an encoded frame");
    uint8_t unstuffed[KILNLINK_FRAME_RAW_MAX];
    size_t unstuffed_len =
        kilnlink_unstuff(wire, wire_len, unstuffed, sizeof(unstuffed), &dstatus);
    CHECK(dstatus == KILNLINK_FRAME_OK, "unstuff() of a freshly-stuffed frame reports OK");
    CHECK(unstuffed_len == raw_len && memcmp(unstuffed, raw, raw_len) == 0,
          "unstuff(stuff(encode(frame))) == encode(frame)");
}

static void test_zero_length_payload(void)
{
    kilnlink_frame_t frame = {
        .msg_type = KILNLINK_MSG_ACK,
        .msg_index = 0,
        .src_device = 0,
        .src_task = 7,
        .dst_device = 1,
        .dst_task = 7,
        .length = 0,
        .payload = NULL,
    };
    uint8_t raw[KILNLINK_FRAME_RAW_MAX];
    kilnlink_frame_status_t status;
    size_t raw_len = kilnlink_frame_encode_raw(&frame, raw, sizeof(raw), &status);
    CHECK(status == KILNLINK_FRAME_OK, "encode_raw() with a NULL zero-length payload is OK");
    CHECK(raw_len == KILNLINK_FRAME_HEADER_LEN + KILNLINK_FRAME_CRC_LEN,
          "zero-length payload frame is exactly header+crc");

    kilnlink_frame_t decoded;
    CHECK(kilnlink_frame_decode(raw, raw_len, &decoded) == KILNLINK_FRAME_OK,
          "decode() of a zero-length-payload frame is OK");
    CHECK(decoded.length == 0, "decoded zero-length payload has length 0");
}

/* -- cross-implementation byte-exact vector ---------------------------------
 *
 * Generated and verified this session via pc_tools' own codec:
 *   codec_encode_frame('data', 5, 'host', 3, 'esp', 3, 'deadbeef')
 *   -> 7e0100050103000304deadbeefe51b7e
 * That Python encoder (protocol.py's Frame.to_wire()) is the same envelope
 * this file re-implements independently in C. If kilnlink produces anything
 * else for the identical logical frame, the two implementations have
 * silently diverged -- exactly the failure mode CommonFW/README.md's test
 * vectors exist to catch before it reaches hardware. */
static void test_cross_implementation_vector(void)
{
    static const uint8_t expected[] = {
        0x7e, 0x01, 0x00, 0x05, 0x01, 0x03, 0x00, 0x03, 0x04, 0xde, 0xad, 0xbe, 0xef, 0xe5, 0x1b,
        0x7e,
    };

    uint8_t payload[] = {0xde, 0xad, 0xbe, 0xef};
    kilnlink_frame_t frame = {
        .msg_type = KILNLINK_MSG_DATA,
        .msg_index = 5,
        .src_device = 1, /* HOST */
        .src_task = 3,
        .dst_device = 0, /* ESP */
        .dst_task = 3,
        .length = (uint8_t)sizeof(payload),
        .payload = payload,
    };

    uint8_t raw[KILNLINK_FRAME_RAW_MAX];
    kilnlink_frame_status_t status;
    size_t raw_len = kilnlink_frame_encode_raw(&frame, raw, sizeof(raw), &status);
    CHECK(status == KILNLINK_FRAME_OK, "cross-vector: encode_raw() OK");

    uint8_t wire[KILNLINK_FRAME_STUFFED_MAX];
    size_t wire_len = kilnlink_stuff(raw, raw_len, wire, sizeof(wire));

    if (wire_len != sizeof(expected) || memcmp(wire, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", wire, wire_len);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "kilnlink's wire bytes match pc_tools' protocol.py byte-for-byte");
    } else {
        CHECK(1, "kilnlink's wire bytes match pc_tools' protocol.py byte-for-byte");
    }
}

/* Two more vectors from test/vectors/frame_vectors.json, generated by
 * pc_tools' protocol.py the same way as test_cross_implementation_vector's
 * -- covering zero-length payload and payload-needs-escaping, which that
 * one doesn't. */
static void test_vector_ack_zero_length(void)
{
    static const uint8_t expected_raw[] = {0x02, 0x00, 0x00, 0x00, 0x07, 0x01, 0x07, 0x00,
                                           0x41, 0x12};
    kilnlink_frame_t frame = {
        .msg_type = KILNLINK_MSG_ACK,
        .msg_index = 0,
        .src_device = 0,
        .src_task = 7,
        .dst_device = 1,
        .dst_task = 7,
        .length = 0,
        .payload = NULL,
    };
    uint8_t raw[KILNLINK_FRAME_RAW_MAX];
    kilnlink_frame_status_t status;
    size_t raw_len = kilnlink_frame_encode_raw(&frame, raw, sizeof(raw), &status);
    CHECK(status == KILNLINK_FRAME_OK, "vector ack_zero_length_payload: encode OK");
    CHECK(raw_len == sizeof(expected_raw) && memcmp(raw, expected_raw, raw_len) == 0,
          "vector ack_zero_length_payload: raw bytes match pc_tools' protocol.py");
}

static void test_vector_payload_needs_escaping(void)
{
    static const uint8_t expected_wire[] = {
        0x7e, 0x04, 0x01, 0x2c, 0x02, 0x05, 0x00, 0x05, 0x05, 0x01, 0x7d, 0x5e, 0x02, 0x7d,
        0x5d, 0x03, 0x8e, 0xff, 0x7e,
    };
    uint8_t payload[] = {0x01, 0x7e, 0x02, 0x7d, 0x03};
    kilnlink_frame_t frame = {
        .msg_type = KILNLINK_MSG_BROADCAST,
        .msg_index = 300,
        .src_device = 2,
        .src_task = 5,
        .dst_device = 0,
        .dst_task = 5,
        .length = (uint8_t)sizeof(payload),
        .payload = payload,
    };
    uint8_t raw[KILNLINK_FRAME_RAW_MAX];
    kilnlink_frame_status_t status;
    size_t raw_len = kilnlink_frame_encode_raw(&frame, raw, sizeof(raw), &status);
    CHECK(status == KILNLINK_FRAME_OK, "vector payload_needs_escaping: encode OK");

    uint8_t wire[KILNLINK_FRAME_STUFFED_MAX];
    size_t wire_len = kilnlink_stuff(raw, raw_len, wire, sizeof(wire));
    if (wire_len != sizeof(expected_wire) || memcmp(wire, expected_wire, sizeof(expected_wire)) != 0) {
        print_hex("  got     ", wire, wire_len);
        print_hex("  expected", expected_wire, sizeof(expected_wire));
        CHECK(0, "vector payload_needs_escaping: wire bytes match pc_tools' protocol.py");
    } else {
        CHECK(1, "vector payload_needs_escaping: wire bytes match pc_tools' protocol.py");
    }
}

/* -- decode hostile inputs ---------------------------------------------------
 *
 * A payload is untrusted input from another processor across an isolated
 * link -- CommonFW/README.md rule 6. Every one of these must be rejected
 * with a specific status, never a crash, never a silently-accepted garbage
 * frame. */

static void test_decode_too_short(void)
{
    uint8_t raw[KILNLINK_FRAME_HEADER_LEN + KILNLINK_FRAME_CRC_LEN - 1] = {0};
    kilnlink_frame_t out;
    CHECK(kilnlink_frame_decode(raw, sizeof(raw), &out) == KILNLINK_FRAME_ERR_TOO_SHORT,
          "decode() of a too-short buffer -> ERR_TOO_SHORT");
    CHECK(kilnlink_frame_decode(raw, 0, &out) == KILNLINK_FRAME_ERR_TOO_SHORT,
          "decode() of an empty buffer -> ERR_TOO_SHORT");
}

static void test_decode_length_too_long(void)
{
    /* A LENGTH byte past KILNLINK_FRAME_MAX_PAYLOAD must be rejected before
     * anything about the buffer's actual size is even considered -- an
     * attacker-controlled length byte must never drive an out-of-bounds
     * read of the caller's buffer. */
    uint8_t raw[KILNLINK_FRAME_HEADER_LEN + KILNLINK_FRAME_CRC_LEN] = {0};
    raw[7] = 254; /* > KILNLINK_FRAME_MAX_PAYLOAD (253) */
    kilnlink_frame_t out;
    CHECK(kilnlink_frame_decode(raw, sizeof(raw), &out) == KILNLINK_FRAME_ERR_LENGTH_TOO_LONG,
          "decode() with LENGTH > MAX_PAYLOAD -> ERR_LENGTH_TOO_LONG, regardless of buffer size");
}

static void test_decode_length_mismatch(void)
{
    /* A legal LENGTH byte (5) that doesn't match how many bytes actually
     * followed it (buffer only has room for header+crc, no payload at all). */
    uint8_t raw[KILNLINK_FRAME_HEADER_LEN + KILNLINK_FRAME_CRC_LEN] = {0};
    raw[7] = 5;
    kilnlink_frame_t out;
    CHECK(kilnlink_frame_decode(raw, sizeof(raw), &out) == KILNLINK_FRAME_ERR_LENGTH_MISMATCH,
          "decode() with LENGTH not matching buffer size -> ERR_LENGTH_MISMATCH");
}

static void test_decode_bad_crc(void)
{
    kilnlink_frame_t frame = {
        .msg_type = KILNLINK_MSG_BROADCAST,
        .msg_index = 42,
        .src_device = 2,
        .src_task = 7,
        .dst_device = 0,
        .dst_task = 7,
        .length = 0,
        .payload = NULL,
    };
    uint8_t raw[KILNLINK_FRAME_RAW_MAX];
    kilnlink_frame_status_t status;
    size_t raw_len = kilnlink_frame_encode_raw(&frame, raw, sizeof(raw), &status);
    CHECK(status == KILNLINK_FRAME_OK, "bad-crc setup: encode_raw() OK");

    raw[raw_len - 1] ^= 0xFF; /* flip the low CRC byte */

    kilnlink_frame_t out;
    CHECK(kilnlink_frame_decode(raw, raw_len, &out) == KILNLINK_FRAME_ERR_CRC,
          "decode() with a corrupted CRC byte -> ERR_CRC");
}

static void test_decode_unknown_type(void)
{
    uint8_t raw[KILNLINK_FRAME_HEADER_LEN + KILNLINK_FRAME_CRC_LEN];
    memset(raw, 0, sizeof(raw));
    raw[0] = 0x99; /* not DATA/ACK/NACK/BROADCAST */
    raw[7] = 0;    /* length 0 */
    uint16_t crc = kilnlink_crc16_ccitt_false(raw, KILNLINK_FRAME_HEADER_LEN);
    raw[KILNLINK_FRAME_HEADER_LEN] = (uint8_t)(crc >> 8);
    raw[KILNLINK_FRAME_HEADER_LEN + 1] = (uint8_t)(crc & 0xFF);

    kilnlink_frame_t out;
    CHECK(kilnlink_frame_decode(raw, sizeof(raw), &out) == KILNLINK_FRAME_ERR_UNKNOWN_TYPE,
          "decode() with an unrecognized type byte -> ERR_UNKNOWN_TYPE (CRC still valid, "
          "so this is genuinely testing the type check, not a CRC false-positive)");
}

static void test_encode_length_too_long(void)
{
    uint8_t oversized_payload[KILNLINK_FRAME_MAX_PAYLOAD + 1] = {0};
    kilnlink_frame_t frame = {
        .msg_type = KILNLINK_MSG_DATA,
        .msg_index = 0,
        .src_device = 0,
        .src_task = 0,
        .dst_device = 0,
        .dst_task = 0,
        .length = KILNLINK_FRAME_MAX_PAYLOAD + 1, /* 254: still fits a uint8_t, exceeds MAX_PAYLOAD */
        .payload = oversized_payload,
    };
    uint8_t raw[KILNLINK_FRAME_RAW_MAX + 16];
    kilnlink_frame_status_t status;
    size_t n = kilnlink_frame_encode_raw(&frame, raw, sizeof(raw), &status);
    CHECK(n == 0, "encode_raw() with length > MAX_PAYLOAD writes nothing");
    CHECK(status == KILNLINK_FRAME_ERR_LENGTH_TOO_LONG,
          "encode_raw() with length > MAX_PAYLOAD -> ERR_LENGTH_TOO_LONG");
}

static void test_encode_buffer_too_small(void)
{
    uint8_t payload[4] = {1, 2, 3, 4};
    kilnlink_frame_t frame = {
        .msg_type = KILNLINK_MSG_DATA,
        .msg_index = 0,
        .src_device = 0,
        .src_task = 0,
        .dst_device = 0,
        .dst_task = 0,
        .length = 4,
        .payload = payload,
    };
    uint8_t raw[4]; /* needs 8 + 4 + 2 = 14 */
    kilnlink_frame_status_t status;
    size_t n = kilnlink_frame_encode_raw(&frame, raw, sizeof(raw), &status);
    CHECK(n == 0, "encode_raw() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_FRAME_ERR_BUFFER_TOO_SMALL,
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
    test_cross_implementation_vector();
    test_vector_ack_zero_length();
    test_vector_payload_needs_escaping();
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
