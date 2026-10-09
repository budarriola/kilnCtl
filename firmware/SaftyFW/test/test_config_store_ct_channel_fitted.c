// test_config_store_ct_channel_fitted.c -- 2026-09-18 owner-directed fix,
// summed-CT-topology support. config_store_ct_channel_fitted()
// (config_store.h) is THE single place "does hardware channel `ch` have a
// CT physically wired behind it" is decided -- s_current_sensing_
// commissioned's relaxation and the any_current_present masking (both in
// safety_core.c and current_task.c) both route through it, so this pure
// predicate is where the real behavioural coverage for the whole fix lives.
// It is a plain static inline in a host-compilable header, so it can be
// exercised directly rather than through a source-text scan.
#include <stdbool.h>
#include <string.h>

#include "test_common.h"

#include "config_store.h"

static void test_summed_channel2_only(void)
{
    TEST_SECTION("config_store_ct_channel_fitted: SUMMED topology -- only "
                 "channel 2 (the one shared CT) is fitted");

    TEST_CHECK(!config_store_ct_channel_fitted(0u, true, CONFIG_STORE_CT_TOPOLOGY_SUMMED),
               "channel 0 must NOT be fitted in SUMMED topology");
    TEST_CHECK(!config_store_ct_channel_fitted(1u, true, CONFIG_STORE_CT_TOPOLOGY_SUMMED),
               "channel 1 must NOT be fitted in SUMMED topology");
    TEST_CHECK(config_store_ct_channel_fitted(2u, true, CONFIG_STORE_CT_TOPOLOGY_SUMMED),
               "channel 2 (the shared CT, HARDWARE.md's channel 3/GPIO28) must be fitted in "
               "SUMMED topology");
}

static void test_per_zone_all_fitted(void)
{
    TEST_SECTION("config_store_ct_channel_fitted: PER_ZONE topology -- all three channels "
                 "are fitted (unchanged behaviour)");

    for (uint8_t ch = 0; ch < 3u; ch++) {
        TEST_CHECK(config_store_ct_channel_fitted(ch, true, CONFIG_STORE_CT_TOPOLOGY_PER_ZONE),
                   "every channel must be fitted in PER_ZONE topology");
    }
}

static void test_ct_installed_false_masks_every_channel(void)
{
    TEST_SECTION("config_store_ct_channel_fitted: ct_installed_effective == false -- no "
                 "channel is fitted regardless of topology");

    for (uint8_t ch = 0; ch < 3u; ch++) {
        TEST_CHECK(!config_store_ct_channel_fitted(ch, false, CONFIG_STORE_CT_TOPOLOGY_PER_ZONE),
                   "ct_installed_effective==false must mask every channel in PER_ZONE");
        TEST_CHECK(!config_store_ct_channel_fitted(ch, false, CONFIG_STORE_CT_TOPOLOGY_SUMMED),
                   "ct_installed_effective==false must mask every channel in SUMMED too");
    }
}

static void test_legacy_topology_byte_decodes_per_zone(void)
{
    TEST_SECTION("config_store_ct_channel_fitted: an unrecognized/legacy ct_topology byte "
                 "decodes to PER_ZONE (this file's own documented default), not SUMMED");

    // Anything other than CONFIG_STORE_CT_TOPOLOGY_SUMMED (1) must behave
    // exactly like CONFIG_STORE_CT_TOPOLOGY_PER_ZONE (0) -- e.g. a legacy
    // record's zeroed/unwritten tail byte.
    TEST_CHECK(config_store_ct_channel_fitted(0u, true, 0xFFu),
               "an unrecognized topology byte must fall through to PER_ZONE (all fitted)");
    TEST_CHECK(config_store_ct_channel_fitted(1u, true, 0xFFu),
               "an unrecognized topology byte must fall through to PER_ZONE (all fitted)");
}

// -- config_store_current_sensing_commissioned() -----------------------
// The other mandatory half of the fix: relaxing this gate to per-fitted-
// channel is only safe once any_current_present is also masked (below) --
// but the gate itself is what makes a SUMMED board commissionable at all.

static config_store_record_t make_rec(uint8_t ct_installed, uint8_t ct_topology, float k0,
                                       float k1, float k2)
{
    config_store_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.fields_set |= CONFIG_STORE_SET_CT_INSTALLED;
    rec.ct_installed = ct_installed;
    rec.ct_topology = ct_topology;
    rec.k_ct_v_per_a[0] = k0;
    rec.k_ct_v_per_a[1] = k1;
    rec.k_ct_v_per_a[2] = k2;
    return rec;
}

static void test_commissioned_summed_ch2_calibrated(void)
{
    TEST_SECTION("config_store_current_sensing_commissioned: SUMMED, only channel 2 "
                 "calibrated -- IS commissioned (the whole point of this fix)");
    config_store_record_t rec = make_rec(1u, CONFIG_STORE_CT_TOPOLOGY_SUMMED, 0.0f, 0.0f, 1.0f);
    TEST_CHECK(config_store_current_sensing_commissioned(&rec),
               "SUMMED board with only its one fitted channel (2) calibrated must report "
               "commissioned");
}

static void test_commissioned_summed_ch2_not_calibrated(void)
{
    TEST_SECTION("config_store_current_sensing_commissioned: SUMMED, channel 2 NOT "
                 "calibrated -- NOT commissioned");
    config_store_record_t rec = make_rec(1u, CONFIG_STORE_CT_TOPOLOGY_SUMMED, 0.0f, 0.0f, 0.0f);
    TEST_CHECK(!config_store_current_sensing_commissioned(&rec),
               "SUMMED board whose one fitted channel is NOT calibrated must NOT report "
               "commissioned");
}

static void test_commissioned_per_zone_all_calibrated(void)
{
    TEST_SECTION("config_store_current_sensing_commissioned: PER_ZONE, all three "
                 "calibrated -- still commissioned (unchanged behaviour)");
    config_store_record_t rec =
        make_rec(1u, CONFIG_STORE_CT_TOPOLOGY_PER_ZONE, 1.0f, 1.0f, 1.0f);
    TEST_CHECK(config_store_current_sensing_commissioned(&rec),
               "PER_ZONE board with all three channels calibrated must report commissioned");
}

static void test_commissioned_per_zone_one_uncalibrated(void)
{
    TEST_SECTION("config_store_current_sensing_commissioned: PER_ZONE, one channel "
                 "uncalibrated -- NOT commissioned (unchanged behaviour)");
    config_store_record_t rec =
        make_rec(1u, CONFIG_STORE_CT_TOPOLOGY_PER_ZONE, 1.0f, 0.0f, 1.0f);
    TEST_CHECK(!config_store_current_sensing_commissioned(&rec),
               "PER_ZONE board missing calibration on any one of its three fitted channels "
               "must NOT report commissioned");
}

static void test_commissioned_ct_installed_zero_never_commissioned(void)
{
    TEST_SECTION("config_store_current_sensing_commissioned: ct_installed == 0 -- never "
                 "commissioned, even with stale/leftover k_ct_v_per_a values");
    config_store_record_t rec =
        make_rec(0u, CONFIG_STORE_CT_TOPOLOGY_SUMMED, 1.0f, 1.0f, 1.0f);
    TEST_CHECK(!config_store_current_sensing_commissioned(&rec),
               "a board with no CT installed at all must never report commissioned, "
               "regardless of leftover k_ct_v_per_a values");
}

// -- config_store_mask_current_present_to_fitted() ----------------------
// The safety-critical other half: an unfitted channel's raw ADC noise
// (channels 0/1 idle at 16-17 counts against only a 25-count fallback
// margin -- see current_presence_policy.h) must never contribute to
// any_current_present, or S9 (unclearable once latched) could arm and
// latch purely off that noise on a SUMMED board.

static void test_mask_unfitted_noise_does_not_count(void)
{
    TEST_SECTION("config_store_mask_current_present_to_fitted: SUMMED board, unfitted "
                 "channels 0/1 reading 'present' from noise, fitted channel 2 genuinely "
                 "NOT present -- must NOT report any_current_present");
    bool present[3] = { true, true, false }; // ch0/ch1 noise-triggered, ch2 (fitted) quiet
    TEST_CHECK(!config_store_mask_current_present_to_fitted(present, true,
                                                             CONFIG_STORE_CT_TOPOLOGY_SUMMED),
               "unfitted channels' noise-driven 'present' must be masked out on a SUMMED "
               "board");
}

static void test_mask_fitted_channel_present_still_counts(void)
{
    TEST_SECTION("config_store_mask_current_present_to_fitted: SUMMED board, fitted "
                 "channel 2 genuinely present -- must report any_current_present true");
    bool present[3] = { false, false, true }; // only the fitted channel is present
    TEST_CHECK(config_store_mask_current_present_to_fitted(present, true,
                                                            CONFIG_STORE_CT_TOPOLOGY_SUMMED),
               "the fitted channel's genuine presence must still be observable after "
               "masking -- masking must narrow, never eliminate, real detection");
}

static void test_mask_ct_installed_false_masks_everything(void)
{
    TEST_SECTION("config_store_mask_current_present_to_fitted: ct_installed_effective == "
                 "false -- masks every channel even if present[] is all true");
    bool present[3] = { true, true, true };
    TEST_CHECK(!config_store_mask_current_present_to_fitted(present, false,
                                                             CONFIG_STORE_CT_TOPOLOGY_PER_ZONE),
               "no CT installed at all must mask every channel's present[] regardless of "
               "topology");
}

void run_test_config_store_ct_channel_fitted(void)
{
    test_summed_channel2_only();
    test_per_zone_all_fitted();
    test_ct_installed_false_masks_every_channel();
    test_legacy_topology_byte_decodes_per_zone();

    test_commissioned_summed_ch2_calibrated();
    test_commissioned_summed_ch2_not_calibrated();
    test_commissioned_per_zone_all_calibrated();
    test_commissioned_per_zone_one_uncalibrated();
    test_commissioned_ct_installed_zero_never_commissioned();

    test_mask_unfitted_noise_does_not_count();
    test_mask_fitted_channel_present_still_counts();
    test_mask_ct_installed_false_masks_everything();
}
