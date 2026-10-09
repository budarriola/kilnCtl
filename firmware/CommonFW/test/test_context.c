/* Host-native test for kilnlink_context.{c,h} -- the ESP->Pico
 * SAFETY_CMD_PUSH_CONTEXT (0x07) codec, docs/LINK_PROTOCOL.md sec 4. Mirrors
 * test_frame.c's structure: round-trip encode/decode, byte-exact vectors
 * from test/vectors/context_vectors.json (hand-verified against Python
 * struct.pack('<f'/'<I')), and the hostile-input set CommonFW/README.md asks
 * for: truncated payload, wrong command byte, out-of-range zone_count, a
 * zone_count that promises more bytes than actually arrived.
 *
 * Build (MSVC host compiler, no CMake needed for this one-shot check):
 *   cl /nologo /W4 /I ..\include test_context.c ..\src\kilnlink_context.c
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_context.h"

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

static void test_round_trip_two_zones(void)
{
    kilnlink_context_t ctx = {0};
    ctx.flags = KILNLINK_CONTEXT_FLAG_PROFILE_RUNNING | KILNLINK_CONTEXT_FLAG_HEAT_REQUESTED |
                KILNLINK_CONTEXT_FLAG_CONTEXT_VALID;
    ctx.boot_id = 3;
    ctx.seq = 1000;
    ctx.uptime_ms = 500000;
    ctx.relay_now_mask = 0x05;
    ctx.relay_recent_mask = 0x0F;
    ctx.recent_window_s = 180;
    ctx.zone_count = 2;
    ctx.zones[0].zone_index = 0;
    ctx.zones[0].flags = KILNLINK_ZONE_FLAG_MEASURED_VALID | KILNLINK_ZONE_FLAG_ACTIVE;
    ctx.zones[0].setpoint_c = 1200.5f;
    ctx.zones[0].measured_c = 1198.25f;
    ctx.zones[0].sample_counter = 42;
    ctx.zones[0].tc_type = 1;
    ctx.zones[0].tc_fault = 0;
    ctx.zones[1].zone_index = 1;
    ctx.zones[1].flags =
        KILNLINK_ZONE_FLAG_MEASURED_VALID | KILNLINK_ZONE_FLAG_ACTIVE | KILNLINK_ZONE_FLAG_GUARD_TRIPPED;
    ctx.zones[1].setpoint_c = 900.0f;
    ctx.zones[1].measured_c = (float)NAN;
    ctx.zones[1].sample_counter = 7;
    ctx.zones[1].tc_type = 2;
    ctx.zones[1].tc_fault = 0x20;

    uint8_t buf[KILNLINK_CONTEXT_MAX_LEN];
    kilnlink_context_status_t status;
    size_t n = kilnlink_context_encode(&ctx, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_CONTEXT_OK, "encode() reports OK");
    CHECK(n == KILNLINK_CONTEXT_FIXED_LEN + 2 * KILNLINK_CONTEXT_ZONE_BLOCK_LEN,
          "encode() writes 15 + 2*14 bytes for two zones");

    kilnlink_context_t decoded;
    kilnlink_context_status_t dstatus = kilnlink_context_decode(buf, n, &decoded);
    CHECK(dstatus == KILNLINK_CONTEXT_OK, "decode() reports OK for a just-encoded payload");
    CHECK(decoded.flags == ctx.flags, "decoded flags match");
    CHECK(decoded.boot_id == ctx.boot_id, "decoded boot_id matches");
    CHECK(decoded.seq == ctx.seq, "decoded seq matches");
    CHECK(decoded.uptime_ms == ctx.uptime_ms, "decoded uptime_ms matches");
    CHECK(decoded.relay_now_mask == ctx.relay_now_mask, "decoded relay_now_mask matches");
    CHECK(decoded.relay_recent_mask == ctx.relay_recent_mask, "decoded relay_recent_mask matches");
    CHECK(decoded.recent_window_s == ctx.recent_window_s, "decoded recent_window_s matches");
    CHECK(decoded.zone_count == 2, "decoded zone_count matches");
    CHECK(decoded.zones[0].setpoint_c == ctx.zones[0].setpoint_c, "zone0 setpoint_c matches");
    CHECK(decoded.zones[0].measured_c == ctx.zones[0].measured_c, "zone0 measured_c matches");
    CHECK(decoded.zones[0].sample_counter == ctx.zones[0].sample_counter,
          "zone0 sample_counter matches");
    CHECK(isnan(decoded.zones[1].measured_c), "zone1 measured_c round-trips as NaN, not fixed up to 0");
    CHECK(decoded.zones[1].tc_fault == ctx.zones[1].tc_fault, "zone1 tc_fault matches");
}

static void test_round_trip_zero_zones(void)
{
    kilnlink_context_t ctx = {0};
    ctx.boot_id = 1;
    ctx.recent_window_s = 150;
    ctx.zone_count = 0;

    uint8_t buf[KILNLINK_CONTEXT_MAX_LEN];
    kilnlink_context_status_t status;
    size_t n = kilnlink_context_encode(&ctx, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_CONTEXT_OK, "encode() with zero zones reports OK");
    CHECK(n == KILNLINK_CONTEXT_FIXED_LEN, "encode() with zero zones writes exactly the fixed header");

    kilnlink_context_t decoded;
    CHECK(kilnlink_context_decode(buf, n, &decoded) == KILNLINK_CONTEXT_OK,
          "decode() of a zero-zone payload is OK");
    CHECK(decoded.zone_count == 0, "decoded zone_count is 0");
}

/* -- byte-exact vectors (test/vectors/context_vectors.json) -------------- */

static void test_vector_two_zones(void)
{
    static const uint8_t expected[] = {
        0x07, 0x0d, 0x03, 0xe8, 0x03, 0x00, 0x00, 0x20, 0xa1, 0x07, 0x00, 0x05, 0x0f, 0xb4, 0x02,
        0x00, 0x03, 0x00, 0x10, 0x96, 0x44, 0x00, 0xc8, 0x95, 0x44, 0x2a, 0x01, 0x00, 0x00, 0x01,
        0x0b, 0x00, 0x00, 0x61, 0x44, 0x00, 0x00, 0xc0, 0x7f, 0x07, 0x02, 0x20, 0x00,
    };

    kilnlink_context_t ctx = {0};
    ctx.flags = 0x0D;
    ctx.boot_id = 3;
    ctx.seq = 1000;
    ctx.uptime_ms = 500000;
    ctx.relay_now_mask = 5;
    ctx.relay_recent_mask = 15;
    ctx.recent_window_s = 180;
    ctx.zone_count = 2;
    ctx.zones[0] = (kilnlink_zone_context_t){0, 3, 1200.5f, 1198.25f, 42, 1, 0};
    ctx.zones[1] = (kilnlink_zone_context_t){1, 11, 900.0f, (float)NAN, 7, 2, 0x20};

    uint8_t buf[KILNLINK_CONTEXT_MAX_LEN];
    kilnlink_context_status_t status;
    size_t n = kilnlink_context_encode(&ctx, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_CONTEXT_OK, "vector two_zones: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector two_zones: bytes match context_vectors.json");
    } else {
        CHECK(1, "vector two_zones: bytes match context_vectors.json");
    }
}

static void test_vector_zero_zones(void)
{
    static const uint8_t expected[] = {0x07, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x00, 0x96, 0x00};
    kilnlink_context_t ctx = {0};
    ctx.boot_id = 1;
    ctx.recent_window_s = 150;
    ctx.zone_count = 0;

    uint8_t buf[KILNLINK_CONTEXT_MAX_LEN];
    kilnlink_context_status_t status;
    size_t n = kilnlink_context_encode(&ctx, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_CONTEXT_OK, "vector zero_zones: encode OK");
    CHECK(n == sizeof(expected) && memcmp(buf, expected, n) == 0,
          "vector zero_zones: bytes match context_vectors.json");
}

/* -- decode hostile inputs ------------------------------------------------ */

static void test_decode_too_short(void)
{
    uint8_t buf[5] = {0x07, 0, 1, 2, 3};
    kilnlink_context_t out;
    CHECK(kilnlink_context_decode(buf, sizeof(buf), &out) == KILNLINK_CONTEXT_ERR_TOO_SHORT,
          "decode() of a too-short payload -> ERR_TOO_SHORT");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_CONTEXT_FIXED_LEN] = {0};
    buf[0] = 0x09; /* SET_FIRING_CEILING's id, not PUSH_CONTEXT's */
    kilnlink_context_t out;
    CHECK(kilnlink_context_decode(buf, sizeof(buf), &out) == KILNLINK_CONTEXT_ERR_WRONG_CMD,
          "decode() with wrong command byte -> ERR_WRONG_CMD");
}

static void test_decode_zone_count_out_of_range(void)
{
    uint8_t buf[KILNLINK_CONTEXT_FIXED_LEN] = {0};
    buf[0] = KILNLINK_CONTEXT_CMD;
    buf[14] = KILNLINK_CONTEXT_MAX_ZONES + 1; /* 4, past the cap of 3 */
    kilnlink_context_t out;
    CHECK(kilnlink_context_decode(buf, sizeof(buf), &out) == KILNLINK_CONTEXT_ERR_ZONE_COUNT,
          "decode() with zone_count > MAX_ZONES -> ERR_ZONE_COUNT, regardless of buffer length");
}

static void test_decode_length_mismatch(void)
{
    /* zone_count claims 1 zone (needs 15+14=29 bytes) but only the 15-byte
     * fixed header is actually present. */
    uint8_t buf[KILNLINK_CONTEXT_FIXED_LEN] = {0};
    buf[0] = KILNLINK_CONTEXT_CMD;
    buf[14] = 1;
    kilnlink_context_t out;
    CHECK(kilnlink_context_decode(buf, sizeof(buf), &out) == KILNLINK_CONTEXT_ERR_LENGTH_MISMATCH,
          "decode() with zone_count promising more bytes than present -> ERR_LENGTH_MISMATCH");
}

static void test_encode_zone_count_out_of_range(void)
{
    kilnlink_context_t ctx = {0};
    ctx.zone_count = KILNLINK_CONTEXT_MAX_ZONES + 1;
    uint8_t buf[KILNLINK_CONTEXT_MAX_LEN + 32];
    kilnlink_context_status_t status;
    size_t n = kilnlink_context_encode(&ctx, buf, sizeof(buf), &status);
    CHECK(n == 0, "encode() with zone_count > MAX_ZONES writes nothing");
    CHECK(status == KILNLINK_CONTEXT_ERR_ZONE_COUNT,
          "encode() with zone_count > MAX_ZONES -> ERR_ZONE_COUNT");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_context_t ctx = {0};
    ctx.zone_count = 1;
    uint8_t buf[4]; /* needs 15+14=29 */
    kilnlink_context_status_t status;
    size_t n = kilnlink_context_encode(&ctx, buf, sizeof(buf), &status);
    CHECK(n == 0, "encode() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_CONTEXT_ERR_BUFFER_TOO_SMALL,
          "encode() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
}

int main(void)
{
    test_round_trip_two_zones();
    test_round_trip_zero_zones();
    test_vector_two_zones();
    test_vector_zero_zones();
    test_decode_too_short();
    test_decode_wrong_cmd();
    test_decode_zone_count_out_of_range();
    test_decode_length_mismatch();
    test_encode_zone_count_out_of_range();
    test_encode_buffer_too_small();

    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
