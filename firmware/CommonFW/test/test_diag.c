/* Host-native test for kilnlink_diag.{c,h} -- the Pico->ESP SAFETY_CMD_DIAG
 * (0x08, Frame B) codec, docs/LINK_PROTOCOL.md sec 6. Mirrors test_power.c's
 * structure: round-trip encode/decode, byte-exact vectors from
 * test/vectors/diag_vectors.json, and the hostile input set: too-short,
 * too-long (fixed-size frame), wrong command byte.
 */

#include <stdio.h>
#include <string.h>

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

#include "kilnlink/kilnlink_diag.h"

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
    kilnlink_diag_t dg = {0};
    dg.trip_reason = 3;
    dg.warn_mask = 0x0001;
    dg.trip_mask = 0x0004;
    dg.uptime_ms = 999999;
    dg.boot_reason = KILNLINK_DIAG_BOOT_WATCHDOG;
    dg.context_age_100ms = 12;
    dg.context_frames_ok = 500;
    dg.context_frames_bad = 2;
    dg.tx_frames_dropped = 1;
    dg.state = KILNLINK_DIAG_STATE_TRIPPED;
    dg.flags = KILNLINK_DIAG_FLAG_SIM_CONTEXT_SEEN | KILNLINK_DIAG_FLAG_CALIBRATION_MISSING;
    dg.log_frames_dropped = 42;

    uint8_t buf[KILNLINK_DIAG_LEN];
    kilnlink_diag_status_t status;
    size_t n = kilnlink_diag_encode(&dg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_DIAG_OK, "encode() reports OK");
    CHECK(n == KILNLINK_DIAG_LEN, "encode() always writes exactly 30 bytes");

    kilnlink_diag_t decoded;
    kilnlink_diag_status_t dstatus = kilnlink_diag_decode(buf, n, &decoded);
    CHECK(dstatus == KILNLINK_DIAG_OK, "decode() reports OK for a just-encoded payload");
    CHECK(decoded.trip_reason == dg.trip_reason, "decoded trip_reason matches");
    CHECK(decoded.warn_mask == dg.warn_mask, "decoded warn_mask matches");
    CHECK(decoded.trip_mask == dg.trip_mask, "decoded trip_mask matches");
    CHECK(decoded.uptime_ms == dg.uptime_ms, "decoded uptime_ms matches");
    CHECK(decoded.boot_reason == dg.boot_reason, "decoded boot_reason matches");
    CHECK(decoded.context_age_100ms == dg.context_age_100ms, "decoded context_age_100ms matches");
    CHECK(decoded.context_frames_ok == dg.context_frames_ok, "decoded context_frames_ok matches");
    CHECK(decoded.context_frames_bad == dg.context_frames_bad, "decoded context_frames_bad matches");
    CHECK(decoded.tx_frames_dropped == dg.tx_frames_dropped, "decoded tx_frames_dropped matches");
    CHECK(decoded.state == dg.state, "decoded state matches");
    CHECK(decoded.flags == dg.flags, "decoded flags matches");
    CHECK(decoded.log_frames_dropped == dg.log_frames_dropped, "decoded log_frames_dropped matches");
}

static void test_round_trip_never_received_context(void)
{
    /* LINK_PROTOCOL.md sec 6: context_age_100ms == 255 means "never
     * received", the honest default before any PUSH_CONTEXT has been
     * parsed. */
    kilnlink_diag_t dg = {0};
    dg.context_age_100ms = KILNLINK_DIAG_CONTEXT_AGE_NEVER;
    dg.state = KILNLINK_DIAG_STATE_INIT;

    uint8_t buf[KILNLINK_DIAG_LEN];
    kilnlink_diag_status_t status;
    size_t n = kilnlink_diag_encode(&dg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_DIAG_OK, "encode() with never-received context reports OK");

    kilnlink_diag_t decoded;
    CHECK(kilnlink_diag_decode(buf, n, &decoded) == KILNLINK_DIAG_OK, "decode() OK");
    CHECK(decoded.context_age_100ms == 255, "context_age_100ms round-trips as 255 (never)");
}

/* -- byte-exact vectors (test/vectors/diag_vectors.json) ---------------- */

static void test_vector_healthy_armed(void)
{
    static const uint8_t expected[] = {
        0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0xe2, 0x01, 0x00, 0x01, 0xff,
        0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x02, 0x00, 0x00, 0x00, 0x00,
    };
    kilnlink_diag_t dg = {0};
    dg.trip_reason = 0;
    dg.warn_mask = 0;
    dg.trip_mask = 0;
    dg.uptime_ms = 123456;
    dg.boot_reason = KILNLINK_DIAG_BOOT_POWERON;
    dg.context_age_100ms = 255;
    dg.context_frames_ok = 10;
    dg.context_frames_bad = 0;
    dg.tx_frames_dropped = 0;
    dg.state = KILNLINK_DIAG_STATE_ARMED;
    dg.flags = KILNLINK_DIAG_FLAG_CALIBRATION_MISSING;
    dg.log_frames_dropped = 0;

    uint8_t buf[KILNLINK_DIAG_LEN];
    kilnlink_diag_status_t status;
    size_t n = kilnlink_diag_encode(&dg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_DIAG_OK, "vector healthy_armed: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector healthy_armed: bytes match diag_vectors.json");
    } else {
        CHECK(1, "vector healthy_armed: bytes match diag_vectors.json");
    }
}

static void test_vector_tripped_with_history(void)
{
    static const uint8_t expected[] = {
        0x08, 0x03, 0x01, 0x00, 0x04, 0x00, 0x3f, 0x42, 0x0f, 0x00, 0x02, 0x0c,
        0xf4, 0x01, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x04, 0x03, 0x07, 0x00, 0x00, 0x00,
    };
    kilnlink_diag_t dg = {0};
    dg.trip_reason = 3;
    dg.warn_mask = 0x0001;
    dg.trip_mask = 0x0004;
    dg.uptime_ms = 999999;
    dg.boot_reason = KILNLINK_DIAG_BOOT_WATCHDOG;
    dg.context_age_100ms = 12;
    dg.context_frames_ok = 500;
    dg.context_frames_bad = 2;
    dg.tx_frames_dropped = 1;
    dg.state = KILNLINK_DIAG_STATE_TRIPPED;
    dg.flags = KILNLINK_DIAG_FLAG_SIM_CONTEXT_SEEN | KILNLINK_DIAG_FLAG_CALIBRATION_MISSING;
    dg.log_frames_dropped = 7;

    uint8_t buf[KILNLINK_DIAG_LEN];
    kilnlink_diag_status_t status;
    size_t n = kilnlink_diag_encode(&dg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_DIAG_OK, "vector tripped_with_history: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector tripped_with_history: bytes match diag_vectors.json");
    } else {
        CHECK(1, "vector tripped_with_history: bytes match diag_vectors.json");
    }
}

/* -- decode hostile inputs ------------------------------------------------ */

static void test_decode_too_short(void)
{
    uint8_t buf[KILNLINK_DIAG_LEN - 1] = {0};
    buf[0] = KILNLINK_DIAG_CMD;
    kilnlink_diag_t out;
    CHECK(kilnlink_diag_decode(buf, sizeof(buf), &out) == KILNLINK_DIAG_ERR_LENGTH_MISMATCH,
          "decode() of a 29-byte (one short) payload -> ERR_LENGTH_MISMATCH");
}

static void test_decode_too_long(void)
{
    uint8_t buf[KILNLINK_DIAG_LEN_V3 + 1] = {0};
    buf[0] = KILNLINK_DIAG_CMD;
    kilnlink_diag_t out;
    CHECK(kilnlink_diag_decode(buf, sizeof(buf), &out) == KILNLINK_DIAG_ERR_LENGTH_MISMATCH,
          "decode() of a 33-byte (one past the boot_id form) payload -> ERR_LENGTH_MISMATCH");
}

/* -- protocol 17 trip_seq form (kilnlink audit 2026-10-09 M4) -------------- */

static void test_round_trip_trip_seq(void)
{
    kilnlink_diag_t dg = {0};
    dg.trip_reason = 6;
    dg.trip_mask = 0x0020;
    dg.state = KILNLINK_DIAG_STATE_TRIPPED;
    dg.log_frames_dropped = 0x01020304u;
    dg.has_trip_seq = true;
    dg.trip_seq = 0xC3;

    uint8_t buf[KILNLINK_DIAG_LEN_V2];
    kilnlink_diag_status_t status;
    size_t n = kilnlink_diag_encode(&dg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_DIAG_OK, "trip_seq encode() reports OK");
    CHECK(n == KILNLINK_DIAG_LEN_V2, "trip_seq encode() writes exactly 31 bytes");
    CHECK(buf[30] == 0xC3, "trip_seq lands at offset 30");

    kilnlink_diag_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    CHECK(kilnlink_diag_decode(buf, n, &decoded) == KILNLINK_DIAG_OK,
          "decode() accepts the 31-byte form");
    CHECK(decoded.has_trip_seq, "31-byte frame decodes with has_trip_seq set");
    CHECK(decoded.trip_seq == 0xC3, "trip_seq round-trips");
    CHECK(decoded.log_frames_dropped == 0x01020304u, "log_frames_dropped unaffected by the new byte");
    CHECK(decoded.trip_mask == 0x0020, "trip_mask unaffected by the new byte");
}

static void test_legacy_decode_has_no_trip_seq(void)
{
    kilnlink_diag_t dg = {0};
    dg.trip_reason = 6;
    uint8_t buf[KILNLINK_DIAG_LEN];
    size_t n = kilnlink_diag_encode(&dg, buf, sizeof(buf), NULL);
    CHECK(n == KILNLINK_DIAG_LEN, "has_trip_seq=false still encodes the 30-byte form");

    kilnlink_diag_t decoded;
    memset(&decoded, 0xFF, sizeof(decoded)); /* poison: decode must clear has_trip_seq */
    CHECK(kilnlink_diag_decode(buf, n, &decoded) == KILNLINK_DIAG_OK,
          "decode() still accepts the 30-byte form");
    CHECK(!decoded.has_trip_seq, "30-byte frame decodes with has_trip_seq false");
    CHECK(decoded.trip_seq == 0, "30-byte frame decodes trip_seq as 0");
}

static void test_vector_tripped_with_trip_seq(void)
{
    static const uint8_t expected[] = {
        0x08, 0x03, 0x01, 0x00, 0x04, 0x00, 0x3f, 0x42, 0x0f, 0x00, 0x02, 0x0c,
        0xf4, 0x01, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x04, 0x03, 0x07, 0x00, 0x00, 0x00, 0x09,
    };
    kilnlink_diag_t dg = {0};
    dg.trip_reason = 3;
    dg.warn_mask = 0x0001;
    dg.trip_mask = 0x0004;
    dg.uptime_ms = 999999;
    dg.boot_reason = KILNLINK_DIAG_BOOT_WATCHDOG;
    dg.context_age_100ms = 12;
    dg.context_frames_ok = 500;
    dg.context_frames_bad = 2;
    dg.tx_frames_dropped = 1;
    dg.state = KILNLINK_DIAG_STATE_TRIPPED;
    dg.flags = KILNLINK_DIAG_FLAG_SIM_CONTEXT_SEEN | KILNLINK_DIAG_FLAG_CALIBRATION_MISSING;
    dg.log_frames_dropped = 7;
    dg.has_trip_seq = true;
    dg.trip_seq = 9;

    uint8_t buf[KILNLINK_DIAG_LEN_V2];
    kilnlink_diag_status_t status;
    size_t n = kilnlink_diag_encode(&dg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_DIAG_OK, "vector tripped_with_trip_seq: encode OK");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector tripped_with_trip_seq: bytes match diag_vectors.json");
    } else {
        CHECK(1, "vector tripped_with_trip_seq: bytes match diag_vectors.json");
    }
}

static void test_encode_trip_seq_buffer_too_small(void)
{
    kilnlink_diag_t dg = {0};
    dg.has_trip_seq = true;
    uint8_t buf[KILNLINK_DIAG_LEN]; /* 30: one short for the trip_seq form */
    kilnlink_diag_status_t status;
    size_t n = kilnlink_diag_encode(&dg, buf, sizeof(buf), &status);
    CHECK(n == 0, "trip_seq encode() into a 30-byte buffer writes nothing");
    CHECK(status == KILNLINK_DIAG_ERR_BUFFER_TOO_SMALL,
          "trip_seq encode() into a 30-byte buffer -> ERR_BUFFER_TOO_SMALL");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_DIAG_LEN] = {0};
    buf[0] = 0x01; /* GET_STATUS's id, not DIAG's */
    kilnlink_diag_t out;
    CHECK(kilnlink_diag_decode(buf, sizeof(buf), &out) == KILNLINK_DIAG_ERR_WRONG_CMD,
          "decode() with wrong command byte -> ERR_WRONG_CMD");
}

static void test_encode_buffer_too_small(void)
{
    kilnlink_diag_t dg = {0};
    uint8_t buf[10]; /* needs 30 */
    kilnlink_diag_status_t status;
    size_t n = kilnlink_diag_encode(&dg, buf, sizeof(buf), &status);
    CHECK(n == 0, "encode() with an undersized output buffer writes nothing");
    CHECK(status == KILNLINK_DIAG_ERR_BUFFER_TOO_SMALL,
          "encode() with an undersized output buffer -> ERR_BUFFER_TOO_SMALL");
}

/* 2026-09-09, RP2040 fatal-fault diagnosability pass: boot_reason bits 3-5
 * (STACK_OVERFLOW/MALLOC_FAILED/ASSERT_FAILED), added purely by reusing
 * spare bits of an already-transmitted byte -- see kilnlink_diag.h's own
 * comment on why this needed no KILNLINK_DIAG_LEN/protocol-version change.
 * Proves they round-trip byte-exact through the SAME fixed-length codec
 * every other boot_reason bit already goes through, and that they compose
 * correctly alongside KILNLINK_DIAG_BOOT_WATCHDOG (a fatal hook firing IS
 * what caused that watchdog reset, so both bits are legitimately set
 * together). */
static void test_round_trip_fatal_boot_reason_bits(void)
{
    kilnlink_diag_t dg = {0};
    dg.boot_reason = (uint8_t)(KILNLINK_DIAG_BOOT_WATCHDOG | KILNLINK_DIAG_BOOT_ASSERT_FAILED);

    uint8_t buf[KILNLINK_DIAG_LEN];
    kilnlink_diag_status_t status;
    size_t n = kilnlink_diag_encode(&dg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_DIAG_OK, "encode() reports OK with fatal boot_reason bits set");
    CHECK(n == KILNLINK_DIAG_LEN, "encode() still writes exactly 30 bytes -- fatal bits reuse "
                                   "spare bits in the EXISTING boot_reason byte, they do not "
                                   "grow the frame");

    kilnlink_diag_t decoded;
    kilnlink_diag_status_t dstatus = kilnlink_diag_decode(buf, n, &decoded);
    CHECK(dstatus == KILNLINK_DIAG_OK, "decode() reports OK");
    CHECK(decoded.boot_reason == dg.boot_reason,
          "WATCHDOG and ASSERT_FAILED bits must both round-trip exactly, set together");
    CHECK((decoded.boot_reason & KILNLINK_DIAG_BOOT_STACK_OVERFLOW) == 0,
          "STACK_OVERFLOW must stay clear when it was never set -- these three fatal bits "
          "are mutually exclusive on the SaftyFW side and the codec must not conflate them");
    CHECK((decoded.boot_reason & KILNLINK_DIAG_BOOT_MALLOC_FAILED) == 0,
          "MALLOC_FAILED must stay clear when it was never set");
}

/* FAIL FAST, DO NOT HANG (2026-09-20). The defect this file exists to catch
 * -- a KILNLINK_DIAG_LEN that disagrees with the offsets
 * kilnlink_diag_encode() actually writes -- overruns the `uint8_t
 * buf[KILNLINK_DIAG_LEN]` locals above. Under MSVC's Debug runtime checks
 * that raises Run-Time Check Failure #2 ("stack around the variable ... was
 * corrupted"), which the debug CRT reports with its default
 * _CRTDBG_MODE_WNDW: a MODAL DIALOG. Confirmed by hand against a sabotaged
 * KILNLINK_DIAG_LEN (30 -> 28): the executable produced no output at all and
 * was still alive after 20s -- a hang, not a failure, which every runner in
 * this repo grades worse than a red test because it stalls rather than
 * reporting. Routing _CRT_ERROR/_CRT_ASSERT/_CRT_WARN to stderr turns that
 * same case into an immediate, named, non-zero exit. No effect on a healthy
 * run, and none at all outside MSVC.
 * check_commonfw_diag_vectors.ps1 keeps a wall-clock timeout as the backstop
 * for anything this does not cover. */
static void fail_fast_instead_of_dialog(void)
{
#ifdef _MSC_VER
    int modes[] = { _CRT_WARN, _CRT_ERROR, _CRT_ASSERT };
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        _CrtSetReportMode(modes[i], _CRTDBG_MODE_FILE);
        _CrtSetReportFile(modes[i], _CRTDBG_FILE_STDERR);
    }
#endif
}

/* -- protocol 18 boot_id form (docs/TEST_TRIP_PLAN.md, F6) ---------------- */

static void test_round_trip_boot_id(void)
{
    kilnlink_diag_t dg = {0};
    dg.trip_reason = 4;
    dg.trip_mask = 0x0008; /* SAFETY_TRIP_TEST */
    dg.state = KILNLINK_DIAG_STATE_TRIPPED;
    dg.log_frames_dropped = 0x01020304u;
    dg.has_boot_id = true;
    dg.trip_seq = 0xC3;
    dg.pico_boot_id = 0x5E;

    uint8_t buf[KILNLINK_DIAG_LEN_V3];
    kilnlink_diag_status_t status;
    size_t n = kilnlink_diag_encode(&dg, buf, sizeof(buf), &status);
    CHECK(status == KILNLINK_DIAG_OK && n == KILNLINK_DIAG_LEN_V3, "V3 encode writes exactly 32 bytes");
    CHECK(buf[30] == 0xC3, "trip_seq at offset 30 in the 32-byte form");
    CHECK(buf[31] == 0x5E, "pico_boot_id lands at offset 31");

    kilnlink_diag_t d;
    memset(&d, 0, sizeof(d));
    CHECK(kilnlink_diag_decode(buf, n, &d) == KILNLINK_DIAG_OK, "decode accepts the 32-byte form");
    CHECK(d.has_trip_seq && d.has_boot_id, "32-byte frame sets has_trip_seq and has_boot_id");
    CHECK(d.trip_seq == 0xC3 && d.pico_boot_id == 0x5E, "trip_seq and pico_boot_id round-trip");
    CHECK(d.log_frames_dropped == 0x01020304u && d.trip_mask == 0x0008, "earlier fields unaffected");
}

static void test_older_diag_forms_have_no_boot_id(void)
{
    uint8_t buf[KILNLINK_DIAG_LEN_V2] = {0};
    buf[0] = KILNLINK_DIAG_CMD;
    kilnlink_diag_t d;
    memset(&d, 0xFF, sizeof(d));
    CHECK(kilnlink_diag_decode(buf, KILNLINK_DIAG_LEN_V2, &d) == KILNLINK_DIAG_OK, "31-byte form still accepted");
    CHECK(d.has_trip_seq && !d.has_boot_id && d.pico_boot_id == 0, "31-byte form has no boot_id");
    memset(&d, 0xFF, sizeof(d));
    CHECK(kilnlink_diag_decode(buf, KILNLINK_DIAG_LEN, &d) == KILNLINK_DIAG_OK, "30-byte form still accepted");
    CHECK(!d.has_trip_seq && !d.has_boot_id, "30-byte form has neither");
}

static void test_vector_boot_id(void)
{
    /* All-zero fields except trip_reason/trip_mask/seq/boot_id: pins the tail offsets. */
    kilnlink_diag_t dg = {0};
    dg.trip_reason = 4;
    dg.trip_mask = 0x0008;
    dg.has_boot_id = true;
    dg.trip_seq = 0x01;
    dg.pico_boot_id = 0x02;
    uint8_t buf[KILNLINK_DIAG_LEN_V3];
    kilnlink_diag_status_t status;
    size_t n = kilnlink_diag_encode(&dg, buf, sizeof(buf), &status);
    CHECK(n == 32, "V3 vector length");
    CHECK(buf[0] == 0x08 && buf[30] == 0x01 && buf[31] == 0x02, "V3 vector cmd and tail bytes");
}

static void test_encode_boot_id_buffer_too_small(void)
{
    kilnlink_diag_t dg = {0};
    dg.has_boot_id = true;
    uint8_t buf[KILNLINK_DIAG_LEN_V2];
    kilnlink_diag_status_t status;
    CHECK(kilnlink_diag_encode(&dg, buf, sizeof(buf), &status) == 0 &&
              status == KILNLINK_DIAG_ERR_BUFFER_TOO_SMALL,
          "V3 encode into a 31-byte buffer -> ERR_BUFFER_TOO_SMALL");
}

int main(void)
{
    fail_fast_instead_of_dialog();
    test_round_trip();
    test_round_trip_never_received_context();
    test_round_trip_fatal_boot_reason_bits();
    test_vector_healthy_armed();
    test_vector_tripped_with_history();
    test_decode_too_short();
    test_decode_too_long();
    test_decode_wrong_cmd();
    test_encode_buffer_too_small();
    test_round_trip_trip_seq();
    test_legacy_decode_has_no_trip_seq();
    test_vector_tripped_with_trip_seq();
    test_encode_trip_seq_buffer_too_small();

    test_round_trip_boot_id();
    test_older_diag_forms_have_no_boot_id();
    test_vector_boot_id();
    test_encode_boot_id_buffer_too_small();

    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
