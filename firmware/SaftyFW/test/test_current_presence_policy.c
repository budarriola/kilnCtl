// Host tests for current_presence_policy.h -- current_presence_is_flowing(),
// the decoupled-from-k_ct_v_per_a presence decision that fixes the fail-open
// on S3/S9/S11/S6b documented in that header's own comment (k_ct_v_per_a was
// never commissioned anywhere in src/, so cs_counts_to_amps() always
// returned 0.0f, and current_any_present() -- which used to compare
// amps[n] > i_present_a -- could never be true).
#include "test_common.h"
#include "../src/current_presence_policy.h"

// --- commissioned (k_ct_v_per_a > 0) --------------------------------------
// This branch must reproduce the OLD "amps > i_present_a" decision exactly,
// bit for bit -- no behavior change on a calibrated channel.

static void test_commissioned_below_threshold_not_present(void)
{
    TEST_SECTION("commissioned, delta well below i_present_a's equivalent counts -> not present");

    // zero_counts=100, i_present_a=2.0A, k_ct=0.010 V/A, gain=0.715 (default).
    // v_present = 2.0 * 0.715 * sqrt2 * 0.010 =~ 0.02022 V
    // counts_threshold = 0.02022 * 4096 / 3.3 =~ 25.09 counts
    // counts_avg = 120 -> delta = 20, well below ~25.
    TEST_CHECK(!current_presence_is_flowing(120u, 100u, 2.0f, 0.010f, 0.715f),
               "delta=20 counts < ~25-count threshold for these cal values -> not flowing");
}

static void test_commissioned_above_threshold_present(void)
{
    TEST_SECTION("commissioned, delta above i_present_a's equivalent counts -> present");

    // Same cal as above; counts_avg = 200 -> delta = 100, well above ~25.
    TEST_CHECK(current_presence_is_flowing(200u, 100u, 2.0f, 0.010f, 0.715f),
               "delta=100 counts > ~25-count threshold for these cal values -> flowing");
}

static void test_commissioned_zero_counts_offset_applied(void)
{
    TEST_SECTION("commissioned -- zero_counts offset is subtracted before comparison, never ignored");

    // counts_avg == zero_counts -> delta 0 -> never present regardless of
    // how permissive i_present_a/k_ct/gain are.
    TEST_CHECK(!current_presence_is_flowing(500u, 500u, 0.001f, 0.001f, 0.1f),
               "counts_avg == zero_counts -> delta 0 -> not flowing even with a tiny threshold");
}

static void test_commissioned_delta_below_zero_counts_clamped(void)
{
    TEST_SECTION("commissioned -- counts_avg below zero_counts clamps delta to 0, no underflow");

    TEST_CHECK(!current_presence_is_flowing(50u, 500u, 2.0f, 0.010f, 0.715f),
               "counts_avg (50) < zero_counts (500) -> delta clamped to 0 -> not flowing "
               "(and must not crash/wrap via unsigned underflow)");
}

// --- NOT commissioned (k_ct_v_per_a <= 0) ---------------------------------
// This is the whole point of the fix: presence must still be detectable
// with NO scale factor at all, via the fixed fallback margin.

static void test_uncommissioned_small_delta_not_present(void)
{
    TEST_SECTION("uncommissioned (k_ct<=0) -- delta at/below the fallback margin -> not present "
                  "(this is the noise floor the margin exists to reject)");

    // zero_counts=1000, counts_avg=1010 -> delta=10, below the 25-count
    // fallback margin.
    TEST_CHECK(!current_presence_is_flowing(1010u, 1000u, 2.0f, 0.0f, 0.715f),
               "k_ct_v_per_a==0.0 (uncommissioned) -- delta=10 <= 25-count fallback margin -> "
               "not flowing");
}

static void test_uncommissioned_large_delta_present(void)
{
    TEST_SECTION("uncommissioned (k_ct<=0) -- delta above the fallback margin -> present -- THIS IS "
                  "THE FIX: before this pass, an uncommissioned k_ct_v_per_a made presence detection "
                  "impossible (cs_counts_to_amps() hard-returned 0.0f, so amps[n] > i_present_a could "
                  "never be true no matter how much real current was flowing)");

    // zero_counts=1000, counts_avg=1200 -> delta=200, well above the
    // 25-count fallback margin -- a large, obviously-real signal.
    TEST_CHECK(current_presence_is_flowing(1200u, 1000u, 2.0f, 0.0f, 0.715f),
               "k_ct_v_per_a==0.0 (uncommissioned) -- delta=200 > 25-count fallback margin -> "
               "flowing, DECOUPLED from k_ct_v_per_a entirely");
}

static void test_uncommissioned_negative_k_ct_also_falls_back(void)
{
    TEST_SECTION("uncommissioned -- a negative k_ct_v_per_a (never a real value, but defensive) "
                  "also takes the fallback path, not a garbage physics computation");

    TEST_CHECK(current_presence_is_flowing(1200u, 1000u, 2.0f, -1.0f, 0.715f),
               "k_ct_v_per_a < 0 -> fallback path -> delta=200 > margin -> flowing");
}

static void test_uncommissioned_zero_i_present_a_also_falls_back(void)
{
    TEST_SECTION("i_present_a<=0 (also uncommissioned, even with a real k_ct) -- falls back too, "
                  "since the physics formula cannot honor a zero/negative amps threshold either");

    TEST_CHECK(!current_presence_is_flowing(1010u, 1000u, 0.0f, 0.010f, 0.715f),
               "i_present_a==0.0 -- fallback path -- delta=10 <= 25-count margin -> not flowing");
    TEST_CHECK(current_presence_is_flowing(1200u, 1000u, 0.0f, 0.010f, 0.715f),
               "i_present_a==0.0 -- fallback path -- delta=200 > 25-count margin -> flowing");
}

static void test_uncommissioned_real_measured_noise_does_not_false_trigger(void)
{
    TEST_SECTION("uncommissioned -- fed the ACTUAL measured board noise (docs/CURRENT_SENSE.md "
                  "'Measured noise floor', 2026-09-06 capture on the one fitted 1A:1V CT: mean 66.89 "
                  "counts, std 4.678 counts, observed range 60-76 counts over a 60s idle window), "
                  "not an idealized round delta -- proves the 25-count fallback margin actually "
                  "survives real ADC noise rather than a synthetic value chosen to be safely small");

    // zero_counts calibrated at the measured idle mean (rounded, as
    // current_task_ct_auto_zero_begin()/_poll() would compute it).
    const uint16_t zero_counts = 67u;

    // Worst observed excursion in the 60s capture: max=76 -> delta=9,
    // nowhere near the 25-count margin. If this ever trips, either the
    // margin has been narrowed unsafely or the board's real noise has
    // gotten worse than the 2026-09-06 measurement -- either way it is a
    // real regression, not a test artifact.
    TEST_CHECK(!current_presence_is_flowing(76u, zero_counts, 2.0f, 0.0f, 0.715f),
               "measured worst-case idle noise (delta=9 counts, from the 60-76 observed range) "
               "stays well clear of the 25-count fallback margin -- no false 'current present'");

    // Same check three std above the measured mean (3 * 4.678 =~ 14 counts)
    // -- a more conservative "how much noise could plausibly occur" bound
    // than the one 60s window happened to show.
    TEST_CHECK(!current_presence_is_flowing(zero_counts + 14u, zero_counts, 2.0f, 0.0f, 0.715f),
               "3-sigma of measured noise (delta=14 counts) still under the 25-count margin");
}

static void test_uncommissioned_exactly_at_margin_not_present(void)
{
    TEST_SECTION("uncommissioned -- delta exactly at the fallback margin -> not present "
                  "(strictly greater-than, matching the commissioned branch's own > convention)");

    // zero_counts=0, counts_avg == CURRENT_PRESENCE_POLICY_FALLBACK_MARGIN_COUNTS exactly.
    TEST_CHECK(!current_presence_is_flowing(CURRENT_PRESENCE_POLICY_FALLBACK_MARGIN_COUNTS, 0u, 2.0f,
                                              0.0f, 0.715f),
               "delta exactly == fallback margin -> not flowing (> not >=)");
    TEST_CHECK(current_presence_is_flowing(CURRENT_PRESENCE_POLICY_FALLBACK_MARGIN_COUNTS + 1u, 0u,
                                             2.0f, 0.0f, 0.715f),
               "delta == fallback margin + 1 -> flowing");
}

void run_test_current_presence_policy(void)
{
    test_commissioned_below_threshold_not_present();
    test_commissioned_above_threshold_present();
    test_commissioned_zero_counts_offset_applied();
    test_commissioned_delta_below_zero_counts_clamped();

    test_uncommissioned_small_delta_not_present();
    test_uncommissioned_large_delta_present();
    test_uncommissioned_negative_k_ct_also_falls_back();
    test_uncommissioned_zero_i_present_a_also_falls_back();
    test_uncommissioned_real_measured_noise_does_not_false_trigger();
    test_uncommissioned_exactly_at_margin_not_present();
}
