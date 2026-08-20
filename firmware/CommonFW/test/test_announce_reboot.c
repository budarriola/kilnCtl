/* Host-native test for kilnlink_announce_reboot.{c,h} -- the ESP->Pico
 * SAFETY_CMD_ANNOUNCE_REBOOT (0x18) codec, docs/LINK_PROTOCOL.md sec 4.
 * Mirrors test_rollback.c's structure exactly (this frame also carries no
 * payload at all): round-trip encode/decode, a byte-exact vector, and the
 * hostile input set: too-short, too-long (fixed-size frame), wrong command
 * byte.
 */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_announce_reboot.h"

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
    kilnlink_announce_reboot_t msg = {0};

    uint8_t buf[KILNLINK_ANNOUNCE_REBOOT_LEN];
    kilnlink_announce_reboot_status_t status;
    size_t n = kilnlink_announce_reboot_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_ANNOUNCE_REBOOT_OK, "encode() reports OK");
    CHECK(n == KILNLINK_ANNOUNCE_REBOOT_LEN, "encode() always writes exactly 1 byte");

    kilnlink_announce_reboot_t decoded;
    kilnlink_announce_reboot_status_t dstatus = kilnlink_announce_reboot_decode(buf, n, &decoded);
    CHECK(dstatus == KILNLINK_ANNOUNCE_REBOOT_OK, "decode() reports OK for a just-encoded payload");
}

static void test_round_trip_null_msg_and_out(void)
{
    /* Neither encode's msg nor decode's out carries meaningful fields --
     * both must accept NULL there without crashing. */
    uint8_t buf[KILNLINK_ANNOUNCE_REBOOT_LEN];
    kilnlink_announce_reboot_status_t status;
    size_t n = kilnlink_announce_reboot_encode(NULL, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_ANNOUNCE_REBOOT_OK, "encode(NULL msg) reports OK");
    CHECK(n == KILNLINK_ANNOUNCE_REBOOT_LEN, "encode(NULL msg) still writes 1 byte");

    kilnlink_announce_reboot_status_t dstatus = kilnlink_announce_reboot_decode(buf, n, NULL);
    CHECK(dstatus == KILNLINK_ANNOUNCE_REBOOT_OK, "decode(NULL out) still reports OK");
}

/* -- byte-exact vector ---------------------------------------------------- */

static void test_vector(void)
{
    static const uint8_t expected[] = {0x18};
    kilnlink_announce_reboot_t msg = {0};

    uint8_t buf[KILNLINK_ANNOUNCE_REBOOT_LEN];
    kilnlink_announce_reboot_status_t status;
    size_t n = kilnlink_announce_reboot_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_ANNOUNCE_REBOOT_OK, "vector: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector: bytes match {0x18}");
    } else {
        CHECK(1, "vector: bytes match {0x18}");
    }
}

/* -- decode hostile inputs ------------------------------------------------ */

static void test_decode_too_short(void)
{
    uint8_t buf[1] = {0}; /* len 0 passed below, not sizeof(buf) */
    kilnlink_announce_reboot_t out;
    CHECK(kilnlink_announce_reboot_decode(buf, 0, &out) == KILNLINK_ANNOUNCE_REBOOT_ERR_LENGTH_MISMATCH,
          "decode() of a 0-byte (one short) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_too_long(void)
{
    uint8_t buf[KILNLINK_ANNOUNCE_REBOOT_LEN + 1] = {0};
    buf[0] = KILNLINK_ANNOUNCE_REBOOT_CMD;
    kilnlink_announce_reboot_t out;
    CHECK(kilnlink_announce_reboot_decode(buf, sizeof(buf), &out) == KILNLINK_ANNOUNCE_REBOOT_ERR_LENGTH_MISMATCH,
          "decode() of a 2-byte (one too many) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_ANNOUNCE_REBOOT_LEN] = {0};
    buf[0] = 0x17; /* ROLLBACK's id, not ANNOUNCE_REBOOT's */
    kilnlink_announce_reboot_t out;
    CHECK(kilnlink_announce_reboot_decode(buf, sizeof(buf), &out) == KILNLINK_ANNOUNCE_REBOOT_ERR_WRONG_CMD,
          "decode() with wrong command byte -> ERR_WRONG_CMD");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_announce_reboot_t msg = {0};
    uint8_t buf[1]; /* needs 1 -- pass out_cap=0 explicitly rather than a
                        zero-sized array (not standard C) */
    kilnlink_announce_reboot_status_t status;
    size_t n = kilnlink_announce_reboot_encode(&msg, buf, 0, &status);
    CHECK(n == 0, "encode() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_ANNOUNCE_REBOOT_ERR_BUFFER_TOO_SMALL,
          "encode() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
}

int main(void)
{
    test_round_trip();
    test_round_trip_null_msg_and_out();
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
