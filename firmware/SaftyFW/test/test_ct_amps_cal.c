// Host tests for firmware/SaftyFW/src/ct_amps_cal.c -- the pure per-channel
// CT amps correction current_sense.c applies on top of its own physics-
// based conversion. No pico-sdk/FreeRTOS dependency (unlike current_sense.c
// itself, which is NOT host-tested -- see current_sense.h's header comment),
// same discipline as test_safety_guards.c.
//
// Covers this task's explicit safety-relevant requirement: "that an
// uncalibrated channel applies the compiled-in default exactly" -- i.e. an
// uncommissioned channel must be byte-for-byte identity, never a plausible-
// looking but wrong number.
#include "test_common.h"

#include "ct_amps_cal.h"

static void test_uncalibrated_is_exact_identity(void)
{
    TEST_SECTION("ct_amps_cal_apply -- uncalibrated channel is exact identity");

    ct_amps_cal_table_t table = ct_amps_cal_uncalibrated_table();

    for (uint8_t ch = 0; ch < CT_AMPS_CAL_NUM_CHANNELS; ch++) {
        TEST_CHECK(!ct_amps_cal_is_calibrated(&table, ch), "uncalibrated_table: channel starts uncalibrated");

        float raw_values[] = {0.0f, 0.001f, 3.5f, 12.75f, 100.0f, -0.0f};
        for (size_t i = 0; i < sizeof(raw_values) / sizeof(raw_values[0]); i++) {
            float raw = raw_values[i];
            float got = ct_amps_cal_apply(&table, ch, raw);
            TEST_CHECK(got == raw, "uncalibrated channel: apply() returns raw amps unchanged, exactly");
        }
    }
}

static void test_uncalibrated_ignores_stale_gain_offset(void)
{
    TEST_SECTION("ct_amps_cal_apply -- calibrated=false ignores non-zero gain/offset");

    // A channel that carries plausible-looking (but not-applied) gain/offset
    // must still behave as pure identity -- calibrated=false means the
    // numbers are never read, not "gain/offset happen to be neutral."
    ct_amps_cal_table_t table = ct_amps_cal_uncalibrated_table();
    table.channels[1].calibrated = false;
    table.channels[1].gain = 5.0f;   // if this were mistakenly applied, 2.0 -> 10.0
    table.channels[1].offset = 3.0f; // and this would shift it further

    float got = ct_amps_cal_apply(&table, 1, 2.0f);
    TEST_CHECK(got == 2.0f, "calibrated=false: stale gain/offset never applied, even though non-zero");
}

static void test_calibrated_applies_linear_correction(void)
{
    TEST_SECTION("ct_amps_cal_apply -- calibrated channel applies gain*raw+offset");

    ct_amps_cal_table_t table = ct_amps_cal_uncalibrated_table();
    table.channels[0].calibrated = true;
    table.channels[0].gain = 2.0f;
    table.channels[0].offset = 0.5f;

    float got = ct_amps_cal_apply(&table, 0, 3.0f);
    TEST_CHECK(got == 6.5f, "calibrated channel: 2.0*3.0 + 0.5 == 6.5");

    got = ct_amps_cal_apply(&table, 0, 0.0f);
    TEST_CHECK(got == 0.5f, "calibrated channel: 2.0*0.0 + 0.5 == 0.5");
}

static void test_calibrated_clamps_negative_to_zero(void)
{
    TEST_SECTION("ct_amps_cal_apply -- clamps a negative correction to 0");

    ct_amps_cal_table_t table = ct_amps_cal_uncalibrated_table();
    table.channels[2].calibrated = true;
    table.channels[2].gain = 1.0f;
    table.channels[2].offset = -5.0f; // 1.0*raw - 5.0 goes negative for small raw

    float got = ct_amps_cal_apply(&table, 2, 1.0f);
    TEST_CHECK(got == 0.0f, "a negative corrected value clamps to 0.0f, same convention as "
                            "current_sense.c's cs_counts_to_amps()");

    got = ct_amps_cal_apply(&table, 2, 10.0f);
    TEST_CHECK(got == 5.0f, "a positive corrected value is unaffected by the clamp");
}

static void test_per_channel_independence(void)
{
    TEST_SECTION("ct_amps_cal_apply -- per-channel independence");

    // Setting channel 0's calibration must never leak into channel 1/2's
    // behaviour, and vice versa -- the whole point of carrying three
    // separate ct_amps_cal_channel_t entries rather than one shared pair.
    ct_amps_cal_table_t table = ct_amps_cal_uncalibrated_table();
    table.channels[0].calibrated = true;
    table.channels[0].gain = 10.0f;
    table.channels[0].offset = 0.0f;
    // channels[1], channels[2] stay uncalibrated.

    TEST_CHECK(ct_amps_cal_apply(&table, 0, 1.0f) == 10.0f, "channel 0: calibrated correction applies");
    TEST_CHECK(ct_amps_cal_apply(&table, 1, 1.0f) == 1.0f, "channel 1: untouched, still identity");
    TEST_CHECK(ct_amps_cal_apply(&table, 2, 1.0f) == 1.0f, "channel 2: untouched, still identity");

    TEST_CHECK(ct_amps_cal_is_calibrated(&table, 0), "channel 0 reports calibrated");
    TEST_CHECK(!ct_amps_cal_is_calibrated(&table, 1), "channel 1 still reports uncalibrated");
    TEST_CHECK(!ct_amps_cal_is_calibrated(&table, 2), "channel 2 still reports uncalibrated");

    // Now calibrate channel 2 differently -- channel 0 and 1 must not move.
    table.channels[2].calibrated = true;
    table.channels[2].gain = 0.5f;
    table.channels[2].offset = 1.0f;

    TEST_CHECK(ct_amps_cal_apply(&table, 0, 1.0f) == 10.0f, "channel 0: unaffected by channel 2's later change");
    TEST_CHECK(ct_amps_cal_apply(&table, 1, 1.0f) == 1.0f, "channel 1: still identity");
    TEST_CHECK(ct_amps_cal_apply(&table, 2, 4.0f) == 3.0f, "channel 2: its own new calibration applies (0.5*4+1=3)");
}

static void test_hostile_inputs(void)
{
    TEST_SECTION("ct_amps_cal_apply/_is_calibrated -- hostile inputs");

    ct_amps_cal_table_t table = ct_amps_cal_uncalibrated_table();
    table.channels[0].calibrated = true;
    table.channels[0].gain = 99.0f;
    table.channels[0].offset = 99.0f;

    TEST_CHECK(ct_amps_cal_apply(NULL, 0, 7.5f) == 7.5f, "NULL table: identity, not a crash");
    TEST_CHECK(!ct_amps_cal_is_calibrated(NULL, 0), "NULL table: reports not calibrated");

    TEST_CHECK(ct_amps_cal_apply(&table, CT_AMPS_CAL_NUM_CHANNELS, 7.5f) == 7.5f,
               "out-of-range channel: identity, not a crash, not channel 0's garbage constants");
    TEST_CHECK(ct_amps_cal_apply(&table, 255u, 7.5f) == 7.5f,
               "wildly out-of-range channel: still identity");
    TEST_CHECK(!ct_amps_cal_is_calibrated(&table, CT_AMPS_CAL_NUM_CHANNELS),
               "out-of-range channel: reports not calibrated");
}

void run_test_ct_amps_cal(void)
{
    test_uncalibrated_is_exact_identity();
    test_uncalibrated_ignores_stale_gain_offset();
    test_calibrated_applies_linear_correction();
    test_calibrated_clamps_negative_to_zero();
    test_per_channel_independence();
    test_hostile_inputs();
}
