/* Host-native test for kilnlink_fw_version.{c,h} -- the Pico->ESP Frame C,
 * SAFETY_CMD_FW_VERSION (0x0B) codec, docs/LINK_PROTOCOL.md sec 6. Mirrors
 * test_announce.c's structure (the two frames share a layout): round-trip
 * encode/decode, byte-exact vectors from
 * test/vectors/fw_version_vectors.json, and the hostile input set:
 * too-short, wrong command byte, variable-length fields that claim more
 * bytes than the buffer actually holds, and a string length past this
 * codec's cap.
 *
 * Build (MSVC host compiler, no CMake needed for this one-shot check):
 *   cl /nologo /W4 /I ..\include test_fw_version.c ..\src\kilnlink_fw_version.c
 */

#include <string.h>
#include <stdio.h>

#include "kilnlink/kilnlink_fw_version.h"

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
    kilnlink_fw_version_t msg = {0};
    msg.protocol_version = 7;
    msg.min_compatible = 5;
    msg.dirty = 0;
    const char *commit = "abc1234";
    memcpy(msg.commit, commit, strlen(commit));
    msg.commit_len = (uint8_t)strlen(commit);
    const char *dt = "2026-08-18 12:00:00Z";
    memcpy(msg.datetime, dt, strlen(dt));
    msg.datetime_len = (uint8_t)strlen(dt);
    msg.boot_id = 7;
    msg.config_version = 3;
    msg.config_crc = 0xBEEF;

    uint8_t buf[KILNLINK_FW_VERSION_MAX_LEN];
    kilnlink_fw_version_status_t status;
    size_t n = kilnlink_fw_version_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_FW_VERSION_OK, "encode() reports OK");
    CHECK(n == KILNLINK_FW_VERSION_FIXED_LEN + msg.commit_len + msg.datetime_len,
          "encode() writes 12 + commit_len + datetime_len bytes");

    kilnlink_fw_version_t decoded;
    kilnlink_fw_version_status_t dstatus = kilnlink_fw_version_decode(buf, n, &decoded);
    CHECK(dstatus == KILNLINK_FW_VERSION_OK, "decode() reports OK for a just-encoded payload");
    CHECK(decoded.protocol_version == msg.protocol_version, "decoded protocol_version matches");
    CHECK(decoded.min_compatible == msg.min_compatible, "decoded min_compatible matches");
    CHECK(decoded.dirty == msg.dirty, "decoded dirty matches");
    CHECK(decoded.commit_len == msg.commit_len, "decoded commit_len matches");
    CHECK(memcmp(decoded.commit, msg.commit, msg.commit_len) == 0, "decoded commit bytes match");
    CHECK(decoded.datetime_len == msg.datetime_len, "decoded datetime_len matches");
    CHECK(memcmp(decoded.datetime, msg.datetime, msg.datetime_len) == 0,
          "decoded datetime bytes match");
    CHECK(decoded.boot_id == msg.boot_id, "decoded boot_id matches");
    CHECK(decoded.config_version == msg.config_version, "decoded config_version matches");
    CHECK(decoded.config_crc == msg.config_crc, "decoded config_crc matches");
}

static void test_round_trip_empty_strings(void)
{
    kilnlink_fw_version_t msg = {0};
    msg.protocol_version = 7;
    msg.min_compatible = 5;
    msg.dirty = 1;
    msg.commit_len = 0;
    msg.datetime_len = 0;
    msg.boot_id = 0;
    msg.config_version = 0;
    msg.config_crc = 0;

    uint8_t buf[KILNLINK_FW_VERSION_MAX_LEN];
    kilnlink_fw_version_status_t status;
    size_t n = kilnlink_fw_version_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_FW_VERSION_OK, "encode() of empty strings reports OK");
    CHECK(n == KILNLINK_FW_VERSION_FIXED_LEN, "encode() of empty strings writes exactly 12 bytes");

    kilnlink_fw_version_t decoded;
    CHECK(kilnlink_fw_version_decode(buf, n, &decoded) == KILNLINK_FW_VERSION_OK,
          "decode() of an empty-strings payload is OK");
    CHECK(decoded.commit_len == 0, "decoded commit_len is 0");
    CHECK(decoded.datetime_len == 0, "decoded datetime_len is 0");
    CHECK(decoded.config_crc == 0, "decoded config_crc is 0 (never commissioned)");
}

/* -- byte-exact vectors (test/vectors/fw_version_vectors.json) ---------- */

static void test_vector_clean_full(void)
{
    static const uint8_t expected[] = {
        0x0b, 0x07, 0x00, 0x05, 0x00, 0x00, 0x07, 0x61, 0x62, 0x63, 0x31,
        0x32, 0x33, 0x34, 0x14, 0x32, 0x30, 0x32, 0x36, 0x2d, 0x30, 0x38,
        0x2d, 0x31, 0x38, 0x20, 0x31, 0x32, 0x3a, 0x30, 0x30, 0x3a, 0x30,
        0x30, 0x5a, 0x07, 0x03, 0xef, 0xbe,
    };
    kilnlink_fw_version_t msg = {0};
    msg.protocol_version = 7;
    msg.min_compatible = 5;
    msg.dirty = 0;
    const char *commit = "abc1234";
    memcpy(msg.commit, commit, strlen(commit));
    msg.commit_len = (uint8_t)strlen(commit);
    const char *dt = "2026-08-18 12:00:00Z";
    memcpy(msg.datetime, dt, strlen(dt));
    msg.datetime_len = (uint8_t)strlen(dt);
    msg.boot_id = 7;
    msg.config_version = 3;
    msg.config_crc = 0xBEEF;

    uint8_t buf[KILNLINK_FW_VERSION_MAX_LEN];
    kilnlink_fw_version_status_t status;
    size_t n = kilnlink_fw_version_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_FW_VERSION_OK, "vector clean_full: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector clean_full: bytes match fw_version_vectors.json");
    } else {
        CHECK(1, "vector clean_full: bytes match fw_version_vectors.json");
    }
}

static void test_vector_dirty_no_strings(void)
{
    static const uint8_t expected[] = {
        0x0b, 0x07, 0x00, 0x05, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    kilnlink_fw_version_t msg = {0};
    msg.protocol_version = 7;
    msg.min_compatible = 5;
    msg.dirty = 1;
    msg.commit_len = 0;
    msg.datetime_len = 0;
    msg.boot_id = 0;
    msg.config_version = 0;
    msg.config_crc = 0;

    uint8_t buf[KILNLINK_FW_VERSION_MAX_LEN];
    kilnlink_fw_version_status_t status;
    size_t n = kilnlink_fw_version_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_FW_VERSION_OK, "vector dirty_no_strings: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector dirty_no_strings: bytes match fw_version_vectors.json");
    } else {
        CHECK(1, "vector dirty_no_strings: bytes match fw_version_vectors.json");
    }
}

/* -- decode hostile inputs ------------------------------------------------ */

static void test_decode_too_short(void)
{
    uint8_t buf[KILNLINK_FW_VERSION_FIXED_LEN - 2] = {0};
    buf[0] = KILNLINK_FW_VERSION_CMD;
    kilnlink_fw_version_t out;
    CHECK(kilnlink_fw_version_decode(buf, sizeof(buf), &out) == KILNLINK_FW_VERSION_ERR_TOO_SHORT,
          "decode() of a 10-byte (fewer than the 12-byte fixed prefix) payload -> ERR_TOO_SHORT");
}

static void test_decode_wrong_cmd(void)
{
    static const uint8_t buf[] = {
        0x0f, 0x07, 0x00, 0x05, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    kilnlink_fw_version_t out;
    CHECK(kilnlink_fw_version_decode(buf, sizeof(buf), &out) == KILNLINK_FW_VERSION_ERR_WRONG_CMD,
          "decode() with wrong command byte (0x0F, ANNOUNCE_VERSION's id) -> ERR_WRONG_CMD");
}

static void test_decode_length_mismatch_commit_truncated(void)
{
    /* commit_len (offset 6) = 7, but the 12-byte buffer only has 5 bytes
     * left after the fixed prefix -- datetime_len's own offset (7+7=14)
     * falls past the end. */
    static const uint8_t buf[] = {
        0x0b, 0x05, 0x00, 0x05, 0x00, 0x00, 0x07, 0x61, 0x62, 0x63, 0x64, 0x65,
    };
    kilnlink_fw_version_t out;
    CHECK(kilnlink_fw_version_decode(buf, sizeof(buf), &out) ==
              KILNLINK_FW_VERSION_ERR_LENGTH_MISMATCH,
          "decode() with commit_len=7 but datetime_len offset past the buffer -> ERR_LENGTH_MISMATCH");
}

static void test_decode_length_mismatch_datetime_truncated(void)
{
    /* commit_len = 0 (fine), but datetime_len (byte right after) claims 20
     * bytes follow when the 12-byte buffer has none left. */
    static const uint8_t buf[] = {
        0x0b, 0x05, 0x00, 0x05, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00,
    };
    kilnlink_fw_version_t out;
    CHECK(kilnlink_fw_version_decode(buf, sizeof(buf), &out) ==
              KILNLINK_FW_VERSION_ERR_LENGTH_MISMATCH,
          "decode() with datetime_len=20 but only 0 datetime bytes present -> ERR_LENGTH_MISMATCH");
}

static void test_decode_string_too_long(void)
{
    /* commit_len (offset 6) = 0xFF, past KILNLINK_FW_VERSION_MAX_COMMIT_LEN
     * (64) -- must be rejected before treating it as a real length, even
     * though it fits in a u8. */
    static const uint8_t buf[] = {
        0x0b, 0x05, 0x00, 0x05, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    kilnlink_fw_version_t out;
    CHECK(kilnlink_fw_version_decode(buf, sizeof(buf), &out) ==
              KILNLINK_FW_VERSION_ERR_STRING_TOO_LONG,
          "decode() with commit_len=255 (past the 64-byte cap) -> ERR_STRING_TOO_LONG");
}

static void test_decode_oversized_declared_length_not_read_oob(void)
{
    /* Regression guard for the failure mode this task exists to avoid
     * (commit ca472fb): a decoder that trusts an untrusted length field and
     * reads min(len, cap) out of a buffer that isn't actually that long.
     * commit_len claims the maximum legal value (64) but the buffer is far
     * shorter than 12 + 64 -- must be rejected as LENGTH_MISMATCH, never
     * read past `len`. */
    uint8_t buf[20] = {0};
    buf[0] = KILNLINK_FW_VERSION_CMD;
    buf[6] = KILNLINK_FW_VERSION_MAX_COMMIT_LEN; /* 64, legal per-field cap, but buf is only 20 bytes */
    kilnlink_fw_version_t out;
    CHECK(kilnlink_fw_version_decode(buf, sizeof(buf), &out) ==
              KILNLINK_FW_VERSION_ERR_LENGTH_MISMATCH,
          "decode() with a maximal-but-legal commit_len that the actual buffer can't back -> "
          "ERR_LENGTH_MISMATCH, not a partial/OOB read");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_fw_version_t msg = {0};
    msg.commit_len = 10;
    msg.datetime_len = 10;
    uint8_t buf[5]; /* needs 12 + 10 + 10 = 32 */
    kilnlink_fw_version_status_t status;
    size_t n = kilnlink_fw_version_encode(&msg, buf, sizeof(buf), &status);
    CHECK(n == 0, "encode() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_FW_VERSION_ERR_BUFFER_TOO_SMALL,
          "encode() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
}

static void test_encode_string_too_long(void)
{
    kilnlink_fw_version_t msg = {0};
    msg.commit_len = KILNLINK_FW_VERSION_MAX_COMMIT_LEN + 1; /* one past the cap */
    uint8_t buf[KILNLINK_FW_VERSION_MAX_LEN];
    kilnlink_fw_version_status_t status;
    size_t n = kilnlink_fw_version_encode(&msg, buf, sizeof(buf), &status);
    CHECK(n == 0, "encode() with commit_len past the cap writes nothing");
    CHECK(status == KILNLINK_FW_VERSION_ERR_STRING_TOO_LONG,
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
    test_decode_oversized_declared_length_not_read_oob();
    test_encode_buffer_too_small();
    test_encode_string_too_long();

    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
