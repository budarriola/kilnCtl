// Host tests for kilnlink_power.c (CommonFW) as consumed by link_task.c's
// link_task_send_power() -- TODO.md Phase 11 ("wire current-sensing output
// into a real SAFETY_CMD_POWER send"). link_task.c itself is not
// host-testable (FreeRTOS + uart_owner + real hardware), and current_sense.c
// is not host-testable either (calls hardware/adc.h directly, per that
// file's own header comment) -- so the pure, hardware-free piece of this
// pass that CAN run on the host is the actual wire codec link_task_send_power()
// calls: kilnlink_power_encode()/kilnlink_power_decode() round-tripping the
// exact kilnlink_power_t shape that function builds from current_sense_power_t
// (mains_voltage_v/i_conducting_a/conduction_fraction/p_avg_w/p_total_w/
// energy_wh/flags), plus the flag-derivation logic link_task_send_power()
// itself performs (mirrored here as synthetic-input assertions, same "known
// input, known expected output" style test_safety_guards.c already uses for
// its threshold-comparison checks).
#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "test_common.h"
#include "kilnlink/kilnlink_power.h"

static void expect_ok(kilnlink_power_status_t s, const char *msg)
{
    TEST_CHECK(s == KILNLINK_POWER_OK, msg);
}

static void test_roundtrip_configured(void)
{
    TEST_SECTION("kilnlink_power: round-trip, mains configured, nothing clipped");

    // Synthetic ADC-derived values -- the shape link_task_send_power() would
    // build from a current_sense_power_t once the front end is commissioned
    // (docs/CURRENT_SENSE.md sec 5): three channels, one conducting near its
    // i_present_a threshold, mains voltage set, so p_avg_w/p_total_w are
    // real numbers rather than NAN.
    kilnlink_power_t pw = {
        .power_window_s = 120u,
        .flags = KILNLINK_POWER_FLAG_MAINS_VOLTAGE_CONFIGURED | KILNLINK_POWER_FLAG_CALIBRATED,
        .mains_voltage_v = 240.0f,
        .i_conducting_a = {12.5f, 0.0f, 0.0f},
        .conduction_fraction = {0.20f, 0.0f, 0.0f},
        .p_avg_w = {600.0f, 0.0f, 0.0f},
        .p_total_w = 600.0f,
        .energy_wh = 1234.5,
    };

    uint8_t wire[KILNLINK_POWER_LEN];
    kilnlink_power_status_t enc_status;
    size_t len = kilnlink_power_encode(&pw, wire, sizeof(wire), &enc_status);
    TEST_CHECK(len == KILNLINK_POWER_LEN, "encode returns KILNLINK_POWER_LEN");
    expect_ok(enc_status, "encode status OK");
    TEST_CHECK(wire[0] == KILNLINK_POWER_CMD, "byte 0 is the SAFETY_CMD_POWER command id");

    kilnlink_power_t out;
    memset(&out, 0xAA, sizeof(out)); // poison, so a field the decoder forgets to touch is caught
    kilnlink_power_status_t dec_status = kilnlink_power_decode(wire, len, &out);
    expect_ok(dec_status, "decode status OK");

    TEST_CHECK(out.power_window_s == pw.power_window_s, "power_window_s round-trips");
    // 2026-09-06: encode() always sets COUNTS_VALID (bit3) itself, since this
    // build's encoder always writes the V2 layout -- see kilnlink_power.h's
    // own doc comment. Mask it out of the comparison rather than baking it
    // into `pw.flags` above, so this test still documents the flags the
    // CALLER actually asked for.
    TEST_CHECK((out.flags & (uint8_t)~KILNLINK_POWER_FLAG_COUNTS_VALID) == pw.flags,
               "caller-supplied flags round-trip (encoder-added COUNTS_VALID excluded)");
    TEST_CHECK((out.flags & (uint8_t)KILNLINK_POWER_FLAG_COUNTS_VALID) != 0,
               "encoder always sets COUNTS_VALID");
    TEST_CHECK_NEAR(out.mains_voltage_v, pw.mains_voltage_v, 1e-4, "mains_voltage_v round-trips");
    for (unsigned ch = 0; ch < KILNLINK_POWER_CHANNELS; ch++) {
        TEST_CHECK_NEAR(out.i_conducting_a[ch], pw.i_conducting_a[ch], 1e-4, "i_conducting_a[ch] round-trips");
        TEST_CHECK_NEAR(out.conduction_fraction[ch], pw.conduction_fraction[ch], 1e-4,
                         "conduction_fraction[ch] round-trips");
        TEST_CHECK_NEAR(out.p_avg_w[ch], pw.p_avg_w[ch], 1e-4, "p_avg_w[ch] round-trips");
    }
    TEST_CHECK_NEAR(out.p_total_w, pw.p_total_w, 1e-4, "p_total_w round-trips");
    TEST_CHECK_NEAR(out.energy_wh, pw.energy_wh, 1e-9, "energy_wh (f64) round-trips");
}

static void test_roundtrip_unconfigured_mains_is_nan_not_zero(void)
{
    TEST_SECTION("kilnlink_power: NAN survives the wire -- unconfigured mains must not decode as 0");

    // docs/CURRENT_SENSE.md sec 3b: "p_avg_w ... only if mains_voltage_v is
    // configured; otherwise absent" -- "absent" is encoded as NAN, and NAN
    // must not collapse into a plausible-looking 0.0 across the wire, which
    // would silently look like "confirmed zero power" instead of "unknown".
    kilnlink_power_t pw = {
        .power_window_s = 120u,
        .flags = 0u, // MAINS_VOLTAGE_CONFIGURED bit clear
        .mains_voltage_v = NAN,
        .i_conducting_a = {3.0f, 0.0f, 0.0f},
        .conduction_fraction = {0.5f, 0.0f, 0.0f},
        .p_avg_w = {NAN, NAN, NAN},
        .p_total_w = NAN,
        .energy_wh = 0.0,
    };

    uint8_t wire[KILNLINK_POWER_LEN];
    kilnlink_power_status_t enc_status;
    size_t len = kilnlink_power_encode(&pw, wire, sizeof(wire), &enc_status);
    expect_ok(enc_status, "encode status OK (NAN payload)");
    TEST_CHECK(len == KILNLINK_POWER_LEN, "encode returns KILNLINK_POWER_LEN (NAN payload)");

    kilnlink_power_t out;
    kilnlink_power_status_t dec_status = kilnlink_power_decode(wire, len, &out);
    expect_ok(dec_status, "decode status OK (NAN payload)");

    TEST_CHECK(isnan(out.mains_voltage_v), "mains_voltage_v decodes as NAN, not 0");
    TEST_CHECK(isnan(out.p_total_w), "p_total_w decodes as NAN, not 0");
    for (unsigned ch = 0; ch < KILNLINK_POWER_CHANNELS; ch++) {
        TEST_CHECK(isnan(out.p_avg_w[ch]), "p_avg_w[ch] decodes as NAN, not 0");
    }
    TEST_CHECK((out.flags & KILNLINK_POWER_FLAG_MAINS_VOLTAGE_CONFIGURED) == 0,
               "MAINS_VOLTAGE_CONFIGURED bit stays clear");
}

static void test_decode_rejects_bad_length_and_wrong_cmd(void)
{
    TEST_SECTION("kilnlink_power: decode rejects untrusted-wire hostile input");

    uint8_t short_buf[KILNLINK_POWER_LEN - 1] = {KILNLINK_POWER_CMD};
    kilnlink_power_t out;
    kilnlink_power_status_t s = kilnlink_power_decode(short_buf, sizeof(short_buf), &out);
    TEST_CHECK(s == KILNLINK_POWER_ERR_LENGTH_MISMATCH, "short payload -> ERR_LENGTH_MISMATCH");

    uint8_t wrong_cmd[KILNLINK_POWER_LEN] = {0};
    wrong_cmd[0] = 0x00u; // anything other than KILNLINK_POWER_CMD (0x0E)
    s = kilnlink_power_decode(wrong_cmd, sizeof(wrong_cmd), &out);
    TEST_CHECK(s == KILNLINK_POWER_ERR_WRONG_CMD, "wrong cmd byte -> ERR_WRONG_CMD");
}

static void test_encode_rejects_undersized_buffer(void)
{
    TEST_SECTION("kilnlink_power: encode refuses to write past an undersized output buffer");

    kilnlink_power_t pw = {0};
    uint8_t tiny[4];
    kilnlink_power_status_t s;
    size_t len = kilnlink_power_encode(&pw, tiny, sizeof(tiny), &s);
    TEST_CHECK(len == 0, "encode returns 0 for a too-small buffer");
    TEST_CHECK(s == KILNLINK_POWER_ERR_BUFFER_TOO_SMALL, "status is ERR_BUFFER_TOO_SMALL");
}

// Mirrors link_task_send_power()'s own flag-derivation (link_task.c): the
// three independent flag bits are each set purely from one input condition,
// never from each other -- a clipped-but-uncalibrated channel and a
// calibrated-but-unconfigured-mains channel must produce distinct, correct
// flag bytes.
static uint8_t derive_flags(bool mains_configured, bool any_clipped, bool calibrated)
{
    uint8_t flags = 0;
    if (mains_configured) {
        flags |= KILNLINK_POWER_FLAG_MAINS_VOLTAGE_CONFIGURED;
    }
    if (any_clipped) {
        flags |= KILNLINK_POWER_FLAG_ANY_CHANNEL_CLIPPED;
    }
    if (calibrated) {
        flags |= KILNLINK_POWER_FLAG_CALIBRATED;
    }
    return flags;
}

static void test_flag_derivation_is_independent_per_bit(void)
{
    TEST_SECTION("kilnlink_power: flags -- each bit reflects exactly one independent condition");

    TEST_CHECK(derive_flags(false, false, false) == 0, "no conditions -> flags == 0");
    TEST_CHECK(derive_flags(true, false, false) == KILNLINK_POWER_FLAG_MAINS_VOLTAGE_CONFIGURED,
               "mains only");
    TEST_CHECK(derive_flags(false, true, false) == KILNLINK_POWER_FLAG_ANY_CHANNEL_CLIPPED,
               "clipped only");
    TEST_CHECK(derive_flags(false, false, true) == KILNLINK_POWER_FLAG_CALIBRATED,
               "calibrated only");
    TEST_CHECK(derive_flags(true, true, true) == (KILNLINK_POWER_FLAG_MAINS_VOLTAGE_CONFIGURED |
                                                    KILNLINK_POWER_FLAG_ANY_CHANNEL_CLIPPED |
                                                    KILNLINK_POWER_FLAG_CALIBRATED),
               "all three conditions -> all three bits");
}

// 2026-09-06: CURRENT_SENSE.md sec 4's "Tooling gap" -- counts_avg must
// round-trip even when the channel is UNCALIBRATED (mains unconfigured,
// amps/power all NaN), since the whole point of this field is to be visible
// independent of commissioning state.
static void test_counts_avg_survives_uncalibrated_channel(void)
{
    TEST_SECTION("kilnlink_power: counts_avg round-trips even when uncalibrated");

    kilnlink_power_t pw = {
        .power_window_s = 120u,
        .flags = 0u, // nothing configured/calibrated
        .mains_voltage_v = NAN,
        .i_conducting_a = {0.0f, 0.0f, 0.0f},
        .conduction_fraction = {0.0f, 0.0f, 0.0f},
        .p_avg_w = {NAN, NAN, NAN},
        .p_total_w = NAN,
        .energy_wh = 0.0,
        .counts_avg = {1u, 2048u, 4095u}, // quantized, real ADC-domain values -- not idealized floats
    };

    uint8_t wire[KILNLINK_POWER_LEN];
    kilnlink_power_status_t enc_status;
    size_t len = kilnlink_power_encode(&pw, wire, sizeof(wire), &enc_status);
    expect_ok(enc_status, "encode status OK (uncalibrated + counts_avg)");

    kilnlink_power_t out;
    kilnlink_power_status_t dec_status = kilnlink_power_decode(wire, len, &out);
    expect_ok(dec_status, "decode status OK (uncalibrated + counts_avg)");
    TEST_CHECK((out.flags & (uint8_t)KILNLINK_POWER_FLAG_COUNTS_VALID) != 0,
               "COUNTS_VALID set even though nothing else is calibrated -- counts_avg is "
               "documented to be independent of calibration state");
    for (unsigned ch = 0; ch < KILNLINK_POWER_CHANNELS; ch++) {
        TEST_CHECK(out.counts_avg[ch] == pw.counts_avg[ch], "counts_avg[ch] round-trips");
    }
    TEST_CHECK(isnan(out.p_total_w), "power fields stay NaN -- counts_avg does not leak into them");
}

void run_test_kilnlink_power(void)
{
    test_roundtrip_configured();
    test_roundtrip_unconfigured_mains_is_nan_not_zero();
    test_decode_rejects_bad_length_and_wrong_cmd();
    test_encode_rejects_undersized_buffer();
    test_flag_derivation_is_independent_per_bit();
    test_counts_avg_survives_uncalibrated_channel();
}
