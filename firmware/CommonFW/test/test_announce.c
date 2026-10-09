/* Host-native test for kilnlink_announce.{c,h} -- the ESP->Pico
 * SAFETY_CMD_ANNOUNCE_VERSION (0x0F) codec, docs/LINK_PROTOCOL.md sec 4.
 * Mirrors test_context.c/test_status.c's structure: round-trip
 * encode/decode, byte-exact vectors from
 * test/vectors/announce_vectors.json, and the hostile input set: too-short,
 * wrong command byte, a variable-length field that claims more bytes than
 * the buffer actually holds.
 *
 * Build (MSVC host compiler, no CMake needed for this one-shot check):
 *   cl /nologo /W4 /I ..\include test_announce.c ..\src\kilnlink_announce.c
 */

#include <string.h>
#include <stdio.h>

#include "kilnlink/kilnlink_announce.h"

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

static void test_round_trip_full(void)
{
    kilnlink_announce_t msg = {0};
    msg.protocol_version = 5;
    msg.min_compatible = 5;
    msg.dirty = 0;
    const char *commit = "abc1234";
    memcpy(msg.commit, commit, strlen(commit));
    msg.commit_len = (uint8_t)strlen(commit);
    const char *dt = "2026-08-18 12:00:00Z";
    memcpy(msg.datetime, dt, strlen(dt));
    msg.datetime_len = (uint8_t)strlen(dt);
    msg.boot_id = 7;

    uint8_t buf[KILNLINK_ANNOUNCE_MAX_LEN];
    kilnlink_announce_status_t status;
    size_t n = kilnlink_announce_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_ANNOUNCE_OK, "encode() reports OK");
    CHECK(n == KILNLINK_ANNOUNCE_FIXED_LEN + msg.commit_len + msg.datetime_len,
          "encode() writes 9 + commit_len + datetime_len bytes");

    kilnlink_announce_t decoded;
    kilnlink_announce_status_t dstatus = kilnlink_announce_decode(buf, n, &decoded);
    CHECK(dstatus == KILNLINK_ANNOUNCE_OK, "decode() reports OK for a just-encoded payload");
    CHECK(decoded.protocol_version == msg.protocol_version, "decoded protocol_version matches");
    CHECK(decoded.min_compatible == msg.min_compatible, "decoded min_compatible matches");
    CHECK(decoded.dirty == msg.dirty, "decoded dirty matches");
    CHECK(decoded.commit_len == msg.commit_len, "decoded commit_len matches");
    CHECK(memcmp(decoded.commit, msg.commit, msg.commit_len) == 0, "decoded commit bytes match");
    CHECK(decoded.datetime_len == msg.datetime_len, "decoded datetime_len matches");
    CHECK(memcmp(decoded.datetime, msg.datetime, msg.datetime_len) == 0,
          "decoded datetime bytes match");
    CHECK(decoded.boot_id == msg.boot_id, "decoded boot_id matches");
}

static void test_round_trip_empty_strings(void)
{
    kilnlink_announce_t msg = {0};
    msg.protocol_version = 5;
    msg.min_compatible = 3;
    msg.dirty = 1;
    msg.commit_len = 0;
    msg.datetime_len = 0;
    msg.boot_id = 0;

    uint8_t buf[KILNLINK_ANNOUNCE_MAX_LEN];
    kilnlink_announce_status_t status;
    size_t n = kilnlink_announce_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_ANNOUNCE_OK, "encode() of empty strings reports OK");
    CHECK(n == KILNLINK_ANNOUNCE_FIXED_LEN, "encode() of empty strings writes exactly 9 bytes");

    kilnlink_announce_t decoded;
    CHECK(kilnlink_announce_decode(buf, n, &decoded) == KILNLINK_ANNOUNCE_OK,
          "decode() of an empty-strings payload is OK");
    CHECK(decoded.commit_len == 0, "decoded commit_len is 0");
    CHECK(decoded.datetime_len == 0, "decoded datetime_len is 0");
}

/* -- byte-exact vectors (test/vectors/announce_vectors.json) ------------- */

static void test_vector_clean_full(void)
{
    static const uint8_t expected[] = {
        0x0f, 0x05, 0x00, 0x05, 0x00, 0x00, 0x07, 0x61, 0x62, 0x63, 0x31,
        0x32, 0x33, 0x34, 0x14, 0x32, 0x30, 0x32, 0x36, 0x2d, 0x30, 0x38,
        0x2d, 0x31, 0x38, 0x20, 0x31, 0x32, 0x3a, 0x30, 0x30, 0x3a, 0x30,
        0x30, 0x5a, 0x07,
    };
    kilnlink_announce_t msg = {0};
    msg.protocol_version = 5;
    msg.min_compatible = 5;
    msg.dirty = 0;
    const char *commit = "abc1234";
    memcpy(msg.commit, commit, strlen(commit));
    msg.commit_len = (uint8_t)strlen(commit);
    const char *dt = "2026-08-18 12:00:00Z";
    memcpy(msg.datetime, dt, strlen(dt));
    msg.datetime_len = (uint8_t)strlen(dt);
    msg.boot_id = 7;

    uint8_t buf[KILNLINK_ANNOUNCE_MAX_LEN];
    kilnlink_announce_status_t status;
    size_t n = kilnlink_announce_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_ANNOUNCE_OK, "vector clean_full: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector clean_full: bytes match announce_vectors.json");
    } else {
        CHECK(1, "vector clean_full: bytes match announce_vectors.json");
    }
}

static void test_vector_dirty_no_strings(void)
{
    static const uint8_t expected[] = {
        0x0f, 0x05, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00,
    };
    kilnlink_announce_t msg = {0};
    msg.protocol_version = 5;
    msg.min_compatible = 3;
    msg.dirty = 1;
    msg.commit_len = 0;
    msg.datetime_len = 0;
    msg.boot_id = 0;

    uint8_t buf[KILNLINK_ANNOUNCE_MAX_LEN];
    kilnlink_announce_status_t status;
    size_t n = kilnlink_announce_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_ANNOUNCE_OK, "vector dirty_no_strings: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector dirty_no_strings: bytes match announce_vectors.json");
    } else {
        CHECK(1, "vector dirty_no_strings: bytes match announce_vectors.json");
    }
}

/* -- decode hostile inputs ------------------------------------------------ */

static void test_decode_too_short(void)
{
    uint8_t buf[KILNLINK_ANNOUNCE_FIXED_LEN - 2] = {0};
    buf[0] = KILNLINK_ANNOUNCE_CMD;
    kilnlink_announce_t out;
    CHECK(kilnlink_announce_decode(buf, sizeof(buf), &out) == KILNLINK_ANNOUNCE_ERR_TOO_SHORT,
          "decode() of a 7-byte (fewer than the 9-byte fixed prefix) payload -> ERR_TOO_SHORT");
}

static void test_decode_wrong_cmd(void)
{
    static const uint8_t buf[] = {
        0x0b, 0x05, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00,
    };
    kilnlink_announce_t out;
    CHECK(kilnlink_announce_decode(buf, sizeof(buf), &out) == KILNLINK_ANNOUNCE_ERR_WRONG_CMD,
          "decode() with wrong command byte (0x0B, FW_VERSION's id) -> ERR_WRONG_CMD");
}

static void test_decode_length_mismatch_commit_truncated(void)
{
    static const uint8_t buf[] = {
        0x0f, 0x05, 0x00, 0x05, 0x00, 0x00, 0x07, 0x61, 0x62,
    };
    kilnlink_announce_t out;
    CHECK(kilnlink_announce_decode(buf, sizeof(buf), &out) == KILNLINK_ANNOUNCE_ERR_LENGTH_MISMATCH,
          "decode() with commit_len=7 but only 2 commit bytes present -> ERR_LENGTH_MISMATCH");
}

static void test_decode_length_mismatch_datetime_truncated(void)
{
    /* commit_len = 0 (fine), but datetime_len (byte right after) claims 20
     * bytes follow when only 3 actually do. */
    static const uint8_t buf[] = {
        0x0f, 0x05, 0x00, 0x05, 0x00, 0x00, 0x00, 0x14, 0x61, 0x62, 0x63,
    };
    kilnlink_announce_t out;
    CHECK(kilnlink_announce_decode(buf, sizeof(buf), &out) == KILNLINK_ANNOUNCE_ERR_LENGTH_MISMATCH,
          "decode() with datetime_len=20 but only 3 datetime bytes present -> ERR_LENGTH_MISMATCH");
}

static void test_decode_string_too_long(void)
{
    /* commit_len (offset 6) = 0xFF, past KILNLINK_ANNOUNCE_MAX_COMMIT_LEN
     * (64) -- must be rejected before treating it as a real length, even
     * though it fits in a u8. */
    static const uint8_t buf[] = {
        0x0f, 0x05, 0x00, 0x05, 0x00, 0x00, 0xff, 0x00, 0x00,
    };
    kilnlink_announce_t out;
    CHECK(kilnlink_announce_decode(buf, sizeof(buf), &out) ==
              KILNLINK_ANNOUNCE_ERR_STRING_TOO_LONG,
          "decode() with commit_len=255 (past the 64-byte cap) -> ERR_STRING_TOO_LONG");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_announce_t msg = {0};
    msg.commit_len = 10;
    msg.datetime_len = 10;
    uint8_t buf[5]; /* needs 9 + 10 + 10 = 29 */
    kilnlink_announce_status_t status;
    size_t n = kilnlink_announce_encode(&msg, buf, sizeof(buf), &status);
    CHECK(n == 0, "encode() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_ANNOUNCE_ERR_BUFFER_TOO_SMALL,
          "encode() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
}

static void test_encode_string_too_long(void)
{
    kilnlink_announce_t msg = {0};
    msg.commit_len = KILNLINK_ANNOUNCE_MAX_COMMIT_LEN + 1; /* one past the cap */
    uint8_t buf[KILNLINK_ANNOUNCE_MAX_LEN];
    kilnlink_announce_status_t status;
    size_t n = kilnlink_announce_encode(&msg, buf, sizeof(buf), &status);
    CHECK(n == 0, "encode() with commit_len past the cap writes nothing");
    CHECK(status == KILNLINK_ANNOUNCE_ERR_STRING_TOO_LONG,
          "encode() with commit_len past the cap -> ERR_STRING_TOO_LONG");
}

int main(void)
{
    test_round_trip_full();
    test_round_trip_empty_strings();
    test_vector_clean_full();
    test_vector_dirty_no_strings();
    test_decode_too_short();
    test_decode_wrong_cmd();
    test_decode_length_mismatch_commit_truncated();
    test_decode_length_mismatch_datetime_truncated();
    test_decode_string_too_long();
    test_encode_buffer_too_small();
    test_encode_string_too_long();

    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
