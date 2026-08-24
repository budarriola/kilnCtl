// Host tests for ct_calibration.c -- the per-channel gain/offset mapping
// wave_owner.c's ct_wave_amps_to_pwm_scale() now delegates to (DESIGN_NOTES.md
// section 3.3 / M-D, and firmware/SimFW/tools/ct_calibration/README.md's
// "Remaining firmware work").
//
// The three things worth pinning down, per that README and the honest state
// of the bench:
//   1. the clamp holds at BOTH ends (a calibration can and will push a
//      commanded value out of range near the extremes of its fitted span);
//   2. channels are independent -- channel 1's constants must never leak
//      into channel 0, which is the exact failure mode CURRENT_SENSE.md
//      section 5's crosstalk gate exists to prevent on the PC side;
//   3. an UNCALIBRATED channel -- the only state that exists today, since
//      no CT hardware and no calibration run exist -- behaves as exact
//      identity, byte-for-byte the old placeholder's behavior.
#include <string.h>

#include "test_common.h"
#include "../src/sim/ct_calibration.h"

// A deliberately asymmetric table: every channel has different constants, so
// any cross-channel leak shows up as a wrong number rather than a coincidence.
static ct_cal_table_t make_table(void)
{
    ct_cal_table_t t;
    t.ct_id_known = false;
    t.ct_id[0] = '\0';
    t.channels[0].calibrated = true;
    t.channels[0].gain = 0.05f;    // 20 A -> full scale
    t.channels[0].offset = 0.0f;

    t.channels[1].calibrated = true;
    t.channels[1].gain = 0.10f;    // 10 A -> full scale, plus a real offset
    t.channels[1].offset = 0.20f;

    t.channels[2].calibrated = false; // never calibrated -> identity
    t.channels[2].gain = 99.0f;       // poison: must be ignored entirely
    t.channels[2].offset = -99.0f;
    return t;
}

static void test_linear_mapping(void)
{
    TEST_SECTION("ct_calibration -- clamp(gain*amps + offset, 0, 1)");

    ct_cal_table_t t = make_table();

    TEST_CHECK_NEAR(ct_cal_apply(&t, 0, 0.0f), 0.0f, 1e-6, "ch0: 0 A -> 0.0 (offset 0)");
    TEST_CHECK_NEAR(ct_cal_apply(&t, 0, 10.0f), 0.5f, 1e-6, "ch0: 10 A -> 0.5 (gain 0.05)");
    TEST_CHECK_NEAR(ct_cal_apply(&t, 0, 20.0f), 1.0f, 1e-6, "ch0: 20 A -> 1.0 (full scale)");
    TEST_CHECK_NEAR(ct_cal_apply(&t, 0, 3.0f), 0.15f, 1e-6, "ch0: 3 A -> 0.15");

    TEST_CHECK_NEAR(ct_cal_apply(&t, 1, 0.0f), 0.20f, 1e-6, "ch1: 0 A -> the offset itself, 0.20");
    TEST_CHECK_NEAR(ct_cal_apply(&t, 1, 4.0f), 0.60f, 1e-6, "ch1: 4 A -> 0.6 (0.1*4 + 0.2)");
}

static void test_clamp_both_ends(void)
{
    TEST_SECTION("ct_calibration -- clamp holds at BOTH ends");

    ct_cal_table_t t = make_table();

    // Upper end: a calibrated channel driven past its fitted span.
    TEST_CHECK_NEAR(ct_cal_apply(&t, 0, 21.0f), 1.0f, 1e-6, "ch0: 21 A clamps to 1.0, not 1.05");
    TEST_CHECK_NEAR(ct_cal_apply(&t, 0, 1.0e6f), 1.0f, 1e-6, "ch0: absurd amps still clamps to 1.0");
    TEST_CHECK_NEAR(ct_cal_apply(&t, 1, 9.0f), 1.0f, 1e-6, "ch1: 9 A (0.9+0.2=1.1) clamps to 1.0");

    // Lower end: a negative amps request, and a negative-offset channel
    // whose fit legitimately predicts a sub-zero command near 0 A.
    TEST_CHECK_NEAR(ct_cal_apply(&t, 0, -5.0f), 0.0f, 1e-6, "ch0: negative amps clamps to 0.0");

    ct_cal_table_t neg = t;
    neg.channels[0].offset = -0.30f;
    TEST_CHECK_NEAR(ct_cal_apply(&neg, 0, 1.0f), 0.0f, 1e-6,
                    "ch0: 0.05*1 - 0.30 = -0.25 clamps to 0.0, never a negative duty");
    TEST_CHECK_NEAR(ct_cal_apply(&neg, 0, 10.0f), 0.20f, 1e-6,
                    "ch0: same channel above its x-intercept is unclamped (0.5 - 0.3)");

    // The clamp is on the OUTPUT, so identity channels clamp too.
    TEST_CHECK_NEAR(ct_cal_apply(&t, 2, 2.5f), 1.0f, 1e-6, "uncalibrated ch2: 2.5 clamps to 1.0");
    TEST_CHECK_NEAR(ct_cal_apply(&t, 2, -2.5f), 0.0f, 1e-6, "uncalibrated ch2: -2.5 clamps to 0.0");
}

static void test_per_channel_independence(void)
{
    TEST_SECTION("ct_calibration -- per-channel independence (no constant leaks)");

    ct_cal_table_t t = make_table();

    // Same input, three channels, three different answers -- the whole point.
    float a0 = ct_cal_apply(&t, 0, 4.0f);
    float a1 = ct_cal_apply(&t, 1, 4.0f);
    float a2 = ct_cal_apply(&t, 2, 0.4f);
    TEST_CHECK_NEAR(a0, 0.20f, 1e-6, "ch0 at 4 A uses ch0's gain only");
    TEST_CHECK_NEAR(a1, 0.60f, 1e-6, "ch1 at 4 A uses ch1's gain+offset only");
    TEST_CHECK(a0 != a1, "ch0 and ch1 do not return the same value for the same amps");
    TEST_CHECK_NEAR(a2, 0.40f, 1e-6, "ch2 is uncalibrated: identity, unaffected by ch0/ch1");

    // Mutating channel 1 must not move channel 0 at all.
    ct_cal_table_t mutated = t;
    mutated.channels[1].gain = 0.5f;
    mutated.channels[1].offset = -0.4f;
    TEST_CHECK_NEAR(ct_cal_apply(&mutated, 0, 4.0f), a0, 1e-6,
                    "changing ch1's constants leaves ch0's answer identical");
    TEST_CHECK_NEAR(ct_cal_apply(&mutated, 2, 0.4f), a2, 1e-6,
                    "changing ch1's constants leaves ch2's identity answer identical");
    TEST_CHECK_NEAR(ct_cal_apply(&mutated, 1, 2.0f), 0.60f, 1e-6,
                    "ch1 itself does follow its own new constants (0.5*2 - 0.4)");

    // Calibrating exactly one channel must not calibrate its neighbours --
    // the "partial table" case the PC-side runner refuses to write, defended
    // in the firmware too.
    ct_cal_table_t only_one = t;
    for (uint8_t ch = 0; ch < CT_CAL_NUM_CHANNELS; ch++) {
        only_one.channels[ch].calibrated = false;
        only_one.channels[ch].gain = 0.0f;
        only_one.channels[ch].offset = 0.0f;
    }
    only_one.channels[1].calibrated = true;
    only_one.channels[1].gain = 0.10f;
    only_one.channels[1].offset = 0.20f;
    TEST_CHECK_NEAR(ct_cal_apply(&only_one, 1, 4.0f), 0.60f, 1e-6, "the one calibrated channel applies its fit");
    TEST_CHECK_NEAR(ct_cal_apply(&only_one, 0, 0.4f), 0.40f, 1e-6, "ch0 stays identity, not ch1's fit");
    TEST_CHECK_NEAR(ct_cal_apply(&only_one, 2, 0.4f), 0.40f, 1e-6, "ch2 stays identity, not ch1's fit");
}

static void test_uncalibrated_is_identity(void)
{
    TEST_SECTION("ct_calibration -- uncalibrated/no-table default is exact identity");

    ct_cal_table_t t = make_table();

    // A NULL table is the same fallback as an uncalibrated channel.
    static const float inputs[] = { 0.0f, 0.001f, 0.25f, 0.5f, 0.999f, 1.0f };
    for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        TEST_CHECK_NEAR(ct_cal_apply(NULL, 0, inputs[i]), inputs[i], 1e-6,
                        "NULL table: identity inside [0,1]");
        TEST_CHECK_NEAR(ct_cal_apply(&t, 2, inputs[i]), inputs[i], 1e-6,
                        "uncalibrated channel: identity inside [0,1]");
    }

    // Poison constants on an uncalibrated channel must be ignored entirely:
    // if `calibrated` were ever ignored, 99*0.5 - 99 would clamp to 0.0 and
    // this would fail loudly.
    TEST_CHECK_NEAR(ct_cal_apply(&t, 2, 0.5f), 0.5f, 1e-6,
                    "uncalibrated channel ignores its stored gain/offset entirely");

    TEST_CHECK(!ct_cal_is_calibrated(&t, 2), "ct_cal_is_calibrated() reports ch2 uncalibrated");
    TEST_CHECK(ct_cal_is_calibrated(&t, 0), "ct_cal_is_calibrated() reports ch0 calibrated");
    TEST_CHECK(!ct_cal_is_calibrated(NULL, 0), "ct_cal_is_calibrated() is false for a NULL table");

    // THE SHIPPED DEFAULT. This is what the firmware actually compiles in
    // today, and it must be identity on every channel: no CT hardware exists
    // and no calibration run has ever been taken, so an uncalibrated fixture
    // must behave exactly as it did before the calibration path existed.
    // If a real bench calibration is ever generated into
    // ct_calibration_defaults.h, this block is the test that will fail --
    // and it SHOULD, as the signal that the shipped default stopped being
    // identity.
    const ct_cal_table_t *def = ct_cal_default_table();
    TEST_CHECK(def != NULL, "ct_cal_default_table() is never NULL");
    for (uint8_t ch = 0; ch < CT_CAL_NUM_CHANNELS; ch++) {
        TEST_CHECK(!ct_cal_is_calibrated(def, ch),
                   "shipped default table: every channel is UNCALIBRATED (M-D is hardware-gated)");
        for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
            TEST_CHECK_NEAR(ct_cal_apply(def, ch, inputs[i]), inputs[i], 1e-6,
                            "shipped default table: identity on every channel");
        }
        TEST_CHECK_NEAR(ct_cal_apply(def, ch, 5.0f), 1.0f, 1e-6, "shipped default: clamps high at 1.0");
        TEST_CHECK_NEAR(ct_cal_apply(def, ch, -5.0f), 0.0f, 1e-6, "shipped default: clamps low at 0.0");
    }
}

static void test_out_of_range_channel(void)
{
    TEST_SECTION("ct_calibration -- out-of-range channel drives nothing");

    ct_cal_table_t t = make_table();
    TEST_CHECK_NEAR(ct_cal_apply(&t, CT_CAL_NUM_CHANNELS, 0.5f), 0.0f, 1e-6,
                    "channel == CT_CAL_NUM_CHANNELS returns 0.0, not a guessed drive level");
    TEST_CHECK_NEAR(ct_cal_apply(&t, 255, 0.5f), 0.0f, 1e-6, "channel 255 returns 0.0");
    TEST_CHECK_NEAR(ct_cal_apply(NULL, 200, 0.5f), 0.0f, 1e-6, "range check precedes the NULL-table fallback");
    TEST_CHECK(!ct_cal_is_calibrated(&t, CT_CAL_NUM_CHANNELS), "out-of-range channel is not 'calibrated'");
}

static void test_ct_id(void)
{
    TEST_SECTION("ct_calibration -- CT identifier (docs/PLAN.md section 11 item 12)");

    // A table with no identifier recorded: ct_id_known == false, poison
    // string content ignored entirely -- same "flag, not a neutral-value
    // convention" idiom test_uncalibrated_is_identity() exercises for
    // `calibrated` above, applied to the id field.
    ct_cal_table_t t = make_table();
    TEST_CHECK(!ct_cal_id_known(&t), "make_table(): ct_id_known is false by construction");
    TEST_CHECK(strcmp(ct_cal_id(&t), "") == 0, "unknown id: ct_cal_id() returns an empty string");

    // Poison the string buffer itself (as if left uninitialised/stale) --
    // ct_cal_id() must still report "" because ct_id_known is false. If
    // ct_cal_id() ever started reading the buffer regardless of the flag,
    // this would fail loudly instead of returning a wrong string.
    memset(t.ct_id, 'X', sizeof(t.ct_id) - 1);
    t.ct_id[sizeof(t.ct_id) - 1] = '\0';
    TEST_CHECK(strcmp(ct_cal_id(&t), "") == 0,
               "unknown id: poisoned buffer contents are still ignored");

    // A table WITH an identifier recorded.
    ct_cal_table_t known = make_table();
    known.ct_id_known = true;
    {
        static const char id[] = "Triad TY-300P#2";
        memcpy(known.ct_id, id, sizeof(id)); /* includes the NUL */
    }
    TEST_CHECK(ct_cal_id_known(&known), "known id: ct_cal_id_known() is true");
    TEST_CHECK(strcmp(ct_cal_id(&known), "Triad TY-300P#2") == 0,
               "known id: ct_cal_id() returns the recorded string");

    // NULL table: both accessors fail safe, never crash, never claim a
    // known id.
    TEST_CHECK(!ct_cal_id_known(NULL), "NULL table: ct_cal_id_known() is false");
    TEST_CHECK(strcmp(ct_cal_id(NULL), "") == 0, "NULL table: ct_cal_id() returns an empty string");

    // THE SHIPPED DEFAULT. No bench calibration run has ever been taken
    // (see test_uncalibrated_is_identity() above), so the compiled-in table
    // must also carry no CT identifier -- if a real bench run is ever
    // generated into ct_calibration_defaults.h without an id, or generated
    // WITH one, this is the check that will move, on purpose.
    const ct_cal_table_t *def = ct_cal_default_table();
    TEST_CHECK(!ct_cal_id_known(def), "shipped default table: no CT identifier recorded");
    TEST_CHECK(strcmp(ct_cal_id(def), "") == 0, "shipped default table: ct_cal_id() returns \"\"");
}

void run_test_ct_calibration(void)
{
    test_linear_mapping();
    test_clamp_both_ends();
    test_per_channel_independence();
    test_uncalibrated_is_identity();
    test_out_of_range_channel();
    test_ct_id();
}
