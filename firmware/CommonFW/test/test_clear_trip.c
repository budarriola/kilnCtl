/* Host-native test for kilnlink_clear_trip.{c,h} -- the ESP->Pico
 * SAFETY_CMD_CLEAR_TRIP (0x0A) codec, docs/LINK_PROTOCOL.md sec 4. Mirrors
 * test_trip.c's structure: round-trip encode/decode, byte-exact vectors
 * from test/vectors/clear_trip_vectors.json, and the hostile input set:
 * too-short, too-long (fixed-size frame), wrong command byte.
 */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_clear_trip.h"

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
    kilnlink_clear_trip_t msg = {0};
    msg.trip_mask = 0x0203;

    uint8_t buf[KILNLINK_CLEAR_TRIP_LEN];
    kilnlink_clear_trip_status_t status;
    size_t n = kilnlink_clear_trip_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_CLEAR_TRIP_OK, "encode() reports OK");
    CHECK(n == KILNLINK_CLEAR_TRIP_LEN, "encode() always writes exactly 3 bytes");

    kilnlink_clear_trip_t decoded;
    kilnlink_clear_trip_status_t dstatus = kilnlink_clear_trip_decode(buf, n, &decoded);
    CHECK(dstatus == KILNLINK_CLEAR_TRIP_OK, "decode() reports OK for a just-encoded payload");
    CHECK(decoded.trip_mask == msg.trip_mask, "decoded trip_mask matches");
}

static void test_round_trip_zero_mask(void)
{
    kilnlink_clear_trip_t msg = {0};
    msg.trip_mask = 0;

    uint8_t buf[KILNLINK_CLEAR_TRIP_LEN];
    kilnlink_clear_trip_status_t status;
    size_t n = kilnlink_clear_trip_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_CLEAR_TRIP_OK, "encode() with trip_mask=0 reports OK");

    kilnlink_clear_trip_t decoded;
    CHECK(kilnlink_clear_trip_decode(buf, n, &decoded) == KILNLINK_CLEAR_TRIP_OK, "decode() OK");
    CHECK(decoded.trip_mask == 0, "trip_mask round-trips as 0");
}

/* -- byte-exact vectors (test/vectors/clear_trip_vectors.json) ---------- */

static void test_vector_mask5(void)
{
    static const uint8_t expected[] = {0x0a, 0x05, 0x00};
    kilnlink_clear_trip_t msg = {0};
    msg.trip_mask = 5;

    uint8_t buf[KILNLINK_CLEAR_TRIP_LEN];
    kilnlink_clear_trip_status_t status;
    size_t n = kilnlink_clear_trip_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_CLEAR_TRIP_OK, "vector mask5: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector mask5: bytes match clear_trip_vectors.json");
    } else {
        CHECK(1, "vector mask5: bytes match clear_trip_vectors.json");
    }
}

static void test_vector_mask_all(void)
{
    static const uint8_t expected[] = {0x0a, 0xff, 0xff};
    kilnlink_clear_trip_t msg = {0};
    msg.trip_mask = 0xFFFF;

    uint8_t buf[KILNLINK_CLEAR_TRIP_LEN];
    kilnlink_clear_trip_status_t status;
    size_t n = kilnlink_clear_trip_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_CLEAR_TRIP_OK, "vector mask_all: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector mask_all: bytes match clear_trip_vectors.json");
    } else {
        CHECK(1, "vector mask_all: bytes match clear_trip_vectors.json");
    }
}

/* -- decode hostile inputs ------------------------------------------------ */

static void test_decode_too_short(void)
{
    uint8_t buf[KILNLINK_CLEAR_TRIP_LEN - 1] = {0};
    buf[0] = KILNLINK_CLEAR_TRIP_CMD;
    kilnlink_clear_trip_t out;
    CHECK(kilnlink_clear_trip_decode(buf, sizeof(buf), &out) == KILNLINK_CLEAR_TRIP_ERR_LENGTH_MISMATCH,
          "decode() of a 2-byte (one short) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_too_long(void)
{
    uint8_t buf[KILNLINK_CLEAR_TRIP_LEN_V3 + 1] = {0};
    buf[0] = KILNLINK_CLEAR_TRIP_CMD;
    kilnlink_clear_trip_t out;
    CHECK(kilnlink_clear_trip_decode(buf, sizeof(buf), &out) == KILNLINK_CLEAR_TRIP_ERR_LENGTH_MISMATCH,
          "decode() of a 6-byte (one past the boot_id form) payload -> ERR_LENGTH_MISMATCH");
}

/* -- protocol 17 occurrence-bound form (kilnlink audit 2026-10-09 M4) ------ */

static void test_round_trip_bound(void)
{
    kilnlink_clear_trip_t msg = {0};
    msg.trip_mask = 0x0020;
    msg.has_trip_seq = true;
    msg.trip_seq = 0xA7;

    uint8_t buf[KILNLINK_CLEAR_TRIP_LEN_V2];
    kilnlink_clear_trip_status_t status;
    size_t n = kilnlink_clear_trip_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_CLEAR_TRIP_OK, "bound encode() reports OK");
    CHECK(n == KILNLINK_CLEAR_TRIP_LEN_V2, "bound encode() writes exactly 4 bytes");

    kilnlink_clear_trip_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    CHECK(kilnlink_clear_trip_decode(buf, n, &decoded) == KILNLINK_CLEAR_TRIP_OK,
          "decode() accepts the 4-byte bound form");
    CHECK(decoded.trip_mask == 0x0020, "bound trip_mask round-trips");
    CHECK(decoded.has_trip_seq, "4-byte frame decodes with has_trip_seq set");
    CHECK(decoded.trip_seq == 0xA7, "trip_seq round-trips");
}

static void test_legacy_decode_has_no_seq(void)
{
    static const uint8_t legacy[] = {0x0a, 0x20, 0x00};
    kilnlink_clear_trip_t decoded;
    memset(&decoded, 0xFF, sizeof(decoded)); /* poison: decode must clear has_trip_seq */
    CHECK(kilnlink_clear_trip_decode(legacy, sizeof(legacy), &decoded) == KILNLINK_CLEAR_TRIP_OK,
          "decode() still accepts the 3-byte legacy form");
    CHECK(!decoded.has_trip_seq, "3-byte frame decodes with has_trip_seq false");
    CHECK(decoded.trip_seq == 0, "3-byte frame decodes trip_seq as 0");
}

static void test_vector_bound_s6a(void)
{
    static const uint8_t expected[] = {0x0a, 0x20, 0x00, 0x05};
    kilnlink_clear_trip_t msg = {0};
    msg.trip_mask = 0x0020;
    msg.has_trip_seq = true;
    msg.trip_seq = 5;

    uint8_t buf[KILNLINK_CLEAR_TRIP_LEN_V2];
    kilnlink_clear_trip_status_t status;
    size_t n = kilnlink_clear_trip_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_CLEAR_TRIP_OK, "vector bound_s6a: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector bound_s6a: bytes match clear_trip_vectors.json");
    } else {
        CHECK(1, "vector bound_s6a: bytes match clear_trip_vectors.json");
    }
}

static void test_encode_bound_buffer_too_small(void)
{
    kilnlink_clear_trip_t msg = {0};
    msg.has_trip_seq = true;
    uint8_t buf[KILNLINK_CLEAR_TRIP_LEN]; /* 3: enough for legacy, one short for bound */
    kilnlink_clear_trip_status_t status;
    size_t n = kilnlink_clear_trip_encode(&msg, buf, sizeof(buf), &status);
    CHECK(n == 0, "bound encode() into a 3-byte buffer writes nothing");
    CHECK(status == KILNLINK_CLEAR_TRIP_ERR_BUFFER_TOO_SMALL,
          "bound encode() into a 3-byte buffer -> ERR_BUFFER_TOO_SMALL");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_CLEAR_TRIP_LEN] = {0};
    buf[0] = 0x09; /* SET_FIRING_CEILING's id, not CLEAR_TRIP's */
    kilnlink_clear_trip_t out;
    CHECK(kilnlink_clear_trip_decode(buf, sizeof(buf), &out) == KILNLINK_CLEAR_TRIP_ERR_WRONG_CMD,
          "decode() with wrong command byte -> ERR_WRONG_CMD");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_clear_trip_t msg = {0};
    uint8_t buf[1]; /* needs 3 */
    kilnlink_clear_trip_status_t status;
    size_t n = kilnlink_clear_trip_encode(&msg, buf, sizeof(buf), &status);
    CHECK(n == 0, "encode() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_CLEAR_TRIP_ERR_BUFFER_TOO_SMALL,
          "encode() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
}

/* -- protocol 18 boot-bound form (docs/TEST_TRIP_PLAN.md, F6) ------------- */

static void test_round_trip_boot_bound(void)
{
    kilnlink_clear_trip_t msg = {0};
    msg.trip_mask = 0x0020;
    msg.has_boot_id = true;
    msg.trip_seq = 0xA7;
    msg.pico_boot_id = 0x3C;

    uint8_t buf[KILNLINK_CLEAR_TRIP_LEN_V3];
    kilnlink_clear_trip_status_t status;
    size_t n = kilnlink_clear_trip_encode(&msg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_CLEAR_TRIP_OK && n == 5, "V3 encode writes exactly 5 bytes");

    kilnlink_clear_trip_t d;
    memset(&d, 0, sizeof(d));
    CHECK(kilnlink_clear_trip_decode(buf, n, &d) == KILNLINK_CLEAR_TRIP_OK, "decode accepts 5 bytes");
    CHECK(d.has_trip_seq && d.has_boot_id, "5-byte frame sets has_trip_seq and has_boot_id");
    CHECK(d.trip_mask == 0x0020 && d.trip_seq == 0xA7 && d.pico_boot_id == 0x3C, "V3 fields round-trip");
}

static void test_vector_boot_bound(void)
{
    static const uint8_t expected[] = {0x0a, 0x08, 0x00, 0x05, 0x11};
    kilnlink_clear_trip_t msg = {0};
    msg.trip_mask = 0x0008; /* SAFETY_TRIP_TEST */
    msg.has_boot_id = true;
    msg.trip_seq = 5;
    msg.pico_boot_id = 0x11;
    uint8_t buf[KILNLINK_CLEAR_TRIP_LEN_V3];
    kilnlink_clear_trip_status_t status;
    size_t n = kilnlink_clear_trip_encode(&msg, buf, sizeof(buf), &status);
    CHECK(n == sizeof(expected) && memcmp(buf, expected, sizeof(expected)) == 0, "V3 frozen vector bytes");
}

static void test_older_forms_have_no_boot_id(void)
{
    static const uint8_t v2[] = {0x0a, 0x20, 0x00, 0x05};
    kilnlink_clear_trip_t d;
    memset(&d, 0xFF, sizeof(d));
    CHECK(kilnlink_clear_trip_decode(v2, sizeof(v2), &d) == KILNLINK_CLEAR_TRIP_OK, "4-byte form still accepted");
    CHECK(d.has_trip_seq && !d.has_boot_id && d.pico_boot_id == 0, "4-byte form has no boot_id");
}

static void test_encode_boot_bound_buffer_too_small(void)
{
    kilnlink_clear_trip_t msg = {0};
    msg.has_boot_id = true;
    uint8_t buf[KILNLINK_CLEAR_TRIP_LEN_V2];
    kilnlink_clear_trip_status_t status;
    CHECK(kilnlink_clear_trip_encode(&msg, buf, sizeof(buf), &status) == 0 &&
              status == KILNLINK_CLEAR_TRIP_ERR_BUFFER_TOO_SMALL,
          "V3 encode into a 4-byte buffer -> ERR_BUFFER_TOO_SMALL");
}

int main(void)
{
    test_round_trip();
    test_round_trip_zero_mask();
    test_vector_mask5();
    test_vector_mask_all();
    test_decode_too_short();
    test_decode_too_long();
    test_decode_wrong_cmd();
    test_encode_buffer_too_small();
    test_round_trip_bound();
    test_legacy_decode_has_no_seq();
    test_vector_bound_s6a();
    test_encode_bound_buffer_too_small();

    test_round_trip_boot_bound();
    test_vector_boot_bound();
    test_older_forms_have_no_boot_id();
    test_encode_boot_bound_buffer_too_small();

    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
