/* Host-native test for kilnlink_get_fw_version.{c,h} -- the ESP->Pico
 * SAFETY_CMD_GET_FW_VERSION (0x0B) codec, docs/LINK_PROTOCOL.md sec 4.
 * One byte, no arguments -- mirrors test_trip.c's structure as far as a
 * no-field frame allows: round-trip encode/decode, the byte-exact vector
 * from test/vectors/get_fw_version_vectors.json, and the hostile input set:
 * too-short, too-long (fixed-size frame), wrong command byte.
 */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_get_fw_version.h"

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

static void test_round_trip(void)
{
    kilnlink_get_fw_version_t msg = {0};

    uint8_t buf[KILNLINK_GET_FW_VERSION_LEN];
    kilnlink_get_fw_version_status_t status;
    size_t n = kilnlink_get_fw_version_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_GET_FW_VERSION_OK, "encode() reports OK");
    CHECK(n == KILNLINK_GET_FW_VERSION_LEN, "encode() always writes exactly 1 byte");
    CHECK(buf[0] == KILNLINK_GET_FW_VERSION_CMD, "encoded byte is the command id");

    kilnlink_get_fw_version_t decoded;
    kilnlink_get_fw_version_status_t dstatus = kilnlink_get_fw_version_decode(buf, n, &decoded);
    CHECK(dstatus == KILNLINK_GET_FW_VERSION_OK, "decode() reports OK for a just-encoded payload");
}

static void test_encode_null_msg(void)
{
    /* No fields exist to read, so a NULL msg pointer is a legitimate way to
     * say "just give me the command byte." */
    uint8_t buf[KILNLINK_GET_FW_VERSION_LEN];
    kilnlink_get_fw_version_status_t status;
    size_t n = kilnlink_get_fw_version_encode(NULL, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_GET_FW_VERSION_OK, "encode(NULL) reports OK");
    CHECK(n == KILNLINK_GET_FW_VERSION_LEN, "encode(NULL) still writes 1 byte");
}

static void test_decode_null_out(void)
{
    uint8_t buf[KILNLINK_GET_FW_VERSION_LEN] = {KILNLINK_GET_FW_VERSION_CMD};
    CHECK(kilnlink_get_fw_version_decode(buf, sizeof(buf), NULL) == KILNLINK_GET_FW_VERSION_OK,
          "decode(..., NULL) still reports OK -- nothing to fill in");
}

/* -- byte-exact vector (test/vectors/get_fw_version_vectors.json) ------- */

static void test_vector_request(void)
{
    static const uint8_t expected[] = {0x0b};
    kilnlink_get_fw_version_t msg = {0};

    uint8_t buf[KILNLINK_GET_FW_VERSION_LEN];
    kilnlink_get_fw_version_status_t status;
    size_t n = kilnlink_get_fw_version_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_GET_FW_VERSION_OK, "vector request: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector request: bytes match get_fw_version_vectors.json");
    } else {
        CHECK(1, "vector request: bytes match get_fw_version_vectors.json");
    }
}

/* -- decode hostile inputs ------------------------------------------------ */

static void test_decode_too_short(void)
{
    uint8_t buf[1];
    kilnlink_get_fw_version_t out;
    CHECK(kilnlink_get_fw_version_decode(buf, 0, &out) == KILNLINK_GET_FW_VERSION_ERR_LENGTH_MISMATCH,
          "decode() of a 0-byte payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_too_long(void)
{
    uint8_t buf[KILNLINK_GET_FW_VERSION_LEN + 1] = {0};
    buf[0] = KILNLINK_GET_FW_VERSION_CMD;
    kilnlink_get_fw_version_t out;
    CHECK(kilnlink_get_fw_version_decode(buf, sizeof(buf), &out) == KILNLINK_GET_FW_VERSION_ERR_LENGTH_MISMATCH,
          "decode() of a 2-byte (one too many) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_GET_FW_VERSION_LEN] = {0};
    buf[0] = 0x0C; /* SET_CLOCK's id, not GET_FW_VERSION's */
    kilnlink_get_fw_version_t out;
    CHECK(kilnlink_get_fw_version_decode(buf, sizeof(buf), &out) == KILNLINK_GET_FW_VERSION_ERR_WRONG_CMD,
          "decode() with wrong command byte -> ERR_WRONG_CMD");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_get_fw_version_t msg = {0};
    uint8_t buf[1];
    kilnlink_get_fw_version_status_t status;
    size_t n = kilnlink_get_fw_version_encode(&msg, buf, 0, &status);
    CHECK(n == 0, "encode() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_GET_FW_VERSION_ERR_BUFFER_TOO_SMALL,
          "encode() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
}

int main(void)
{
    test_round_trip();
    test_encode_null_msg();
    test_decode_null_out();
    test_vector_request();
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
