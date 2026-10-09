/* Host-native test for kilnlink_commit_config_rejected.{c,h} -- the
 * Pico->ESP SAFETY_CMD_COMMIT_CONFIG_REJECTED (0x20) codec,
 * docs/LINK_PROTOCOL.md sec 4, docs/COMMISSIONING.md sec 2/3.1. Mirrors
 * test_set_param.c's structure (u16 field + u8 field): round-trip
 * encode/decode, a byte-exact vector, and the hostile input set:
 * too-short, too-long (fixed-size frame), wrong command byte.
 */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_commit_config_rejected.h"

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

static void test_round_trip(void)
{
    kilnlink_commit_config_rejected_t msg = {0};
    msg.param_id = 0x0104u; /* abs_max_temp_c */
    msg.reason = KILNLINK_COMMIT_CONFIG_REJECT_RANGE;

    uint8_t buf[KILNLINK_COMMIT_CONFIG_REJECTED_LEN];
    kilnlink_commit_config_rejected_status_t status;
    size_t n = kilnlink_commit_config_rejected_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_COMMIT_CONFIG_REJECTED_OK, "encode() reports OK");
    CHECK(n == KILNLINK_COMMIT_CONFIG_REJECTED_LEN, "encode() always writes exactly 4 bytes");
    CHECK(buf[0] == KILNLINK_COMMIT_CONFIG_REJECTED_CMD, "encoded byte 0 is the command id");

    kilnlink_commit_config_rejected_t decoded;
    memset(&decoded, 0xAA, sizeof(decoded));
    CHECK(kilnlink_commit_config_rejected_decode(buf, n, &decoded) == KILNLINK_COMMIT_CONFIG_REJECTED_OK,
          "decode() reports OK for a just-encoded payload");
    CHECK(decoded.param_id == msg.param_id, "decoded param_id round-trips");
    CHECK(decoded.reason == msg.reason, "decoded reason round-trips");
}

static void test_no_param_id_sentinel_round_trips(void)
{
    /* ARMED/STORAGE refusals are not about one field -- the sentinel must
     * survive the wire unmangled, same as any other id. */
    kilnlink_commit_config_rejected_t msg = {0};
    msg.param_id = KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID;
    msg.reason = KILNLINK_COMMIT_CONFIG_REJECT_ARMED;

    uint8_t buf[KILNLINK_COMMIT_CONFIG_REJECTED_LEN];
    kilnlink_commit_config_rejected_status_t status;
    size_t n = kilnlink_commit_config_rejected_encode(&msg, buf, sizeof(buf), &status);
    CHECK(n == KILNLINK_COMMIT_CONFIG_REJECTED_LEN, "encode() with the sentinel still writes 4 bytes");

    kilnlink_commit_config_rejected_t decoded;
    CHECK(kilnlink_commit_config_rejected_decode(buf, n, &decoded) == KILNLINK_COMMIT_CONFIG_REJECTED_OK,
          "decode() of the sentinel case reports OK");
    CHECK(decoded.param_id == KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID,
          "the 0xFFFF sentinel survives the wire, not clamped/reinterpreted");
    CHECK(decoded.reason == KILNLINK_COMMIT_CONFIG_REJECT_ARMED, "reason round-trips alongside the sentinel");
}

static void test_vector(void)
{
    /* param_id=0x0201 (firing_margin_c) LE, reason=1 (CONTRADICTION) --
     * byte-exact so a future accidental byte-order swap on either field
     * fails this test instead of shipping silently. */
    static const uint8_t expected[] = {0x20, 0x01, 0x02, 0x01};
    kilnlink_commit_config_rejected_t msg = {0};
    msg.param_id = 0x0201u;
    msg.reason = KILNLINK_COMMIT_CONFIG_REJECT_CONTRADICTION;

    uint8_t buf[KILNLINK_COMMIT_CONFIG_REJECTED_LEN];
    kilnlink_commit_config_rejected_status_t status;
    size_t n = kilnlink_commit_config_rejected_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_COMMIT_CONFIG_REJECTED_OK, "vector: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector: bytes match {0x20, 0x01, 0x02, 0x01}");
    } else {
        CHECK(1, "vector: bytes match {0x20, 0x01, 0x02, 0x01}");
    }
}

static void test_decode_too_short(void)
{
    uint8_t buf[3] = {KILNLINK_COMMIT_CONFIG_REJECTED_CMD, 0, 0};
    kilnlink_commit_config_rejected_t out;
    CHECK(kilnlink_commit_config_rejected_decode(buf, sizeof(buf), &out) ==
              KILNLINK_COMMIT_CONFIG_REJECTED_ERR_LENGTH_MISMATCH,
          "decode() of a 3-byte (one short) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_too_long(void)
{
    uint8_t buf[KILNLINK_COMMIT_CONFIG_REJECTED_LEN + 1] = {0};
    buf[0] = KILNLINK_COMMIT_CONFIG_REJECTED_CMD;
    kilnlink_commit_config_rejected_t out;
    CHECK(kilnlink_commit_config_rejected_decode(buf, sizeof(buf), &out) ==
              KILNLINK_COMMIT_CONFIG_REJECTED_ERR_LENGTH_MISMATCH,
          "decode() of a 5-byte (one too many) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_COMMIT_CONFIG_REJECTED_LEN] = {0};
    buf[0] = 0x1Du; /* COMMIT_CONFIG's id, not COMMIT_CONFIG_REJECTED's */
    kilnlink_commit_config_rejected_t out;
    CHECK(kilnlink_commit_config_rejected_decode(buf, sizeof(buf), &out) ==
              KILNLINK_COMMIT_CONFIG_REJECTED_ERR_WRONG_CMD,
          "decode() with wrong command byte -> ERR_WRONG_CMD");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_commit_config_rejected_t msg = {0};
    uint8_t buf[1];
    kilnlink_commit_config_rejected_status_t status;
    size_t n = kilnlink_commit_config_rejected_encode(&msg, buf, 0, &status);
    CHECK(n == 0, "encode() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_COMMIT_CONFIG_REJECTED_ERR_BUFFER_TOO_SMALL,
          "encode() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
}

int main(void)
{
    test_round_trip();
    test_no_param_id_sentinel_round_trips();
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
