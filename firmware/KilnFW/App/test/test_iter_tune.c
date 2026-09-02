// test_iter_tune.c -- PID_EXPANSION_PLAN.md 3.3 "Iterative tuning".
// Pure host tests, no ESP-IDF stubs needed (see iter_tune.h's top comment).
//
// Every check below has a documented negative case: mutate the constant or
// input named in the comment, watch the named test go red, then revert.
// Literals used as expected values are computed independently of the
// module's own constants where the point is to pin BEHAVIOR, not restate
// the constant (e.g. the noise-floor tests use 15%/25% deltas chosen to
// straddle ITER_TUNE_MIN_RELATIVE_IMPROVEMENT's 20% from literals, not by
// reading the macro back).

#include "test_common.h"
#include "../drivers/iter_tune.h"

#include <string.h>

// Mirrors ZONE_PID_GAIN_MAX (zones_http.h). If this literal and
// ITER_TUNE_GAIN_CEIL_C (iter_tune.h) ever drift apart, this line -- not a
// silent clamp mismatch -- is where it will be caught.
#define ZONE_PID_GAIN_MAX_MIRROR 1000.0f

static iter_tune_firing_t mk_firing(uint8_t profile_id, uint8_t zone_mask, float start_temp_c,
                                     float iae, float kp, float ki, float kd)
{
    iter_tune_firing_t f;
    memset(&f, 0, sizeof(f));
    f.profile_id = profile_id;
    f.zone_mask = zone_mask;
    f.start_temp_c = start_temp_c;
    f.iae_normalized = iae;
    f.gains.kp = kp;
    f.gains.ki = ki;
    f.gains.kd = kd;
    return f;
}

static void test_gain_ceil_mirrors_zone_pid_gain_max(void)
{
    TEST_SECTION("iter_tune: ITER_TUNE_GAIN_CEIL_C mirrors ZONE_PID_GAIN_MAX");
    // Negative test: change ITER_TUNE_GAIN_CEIL_C in iter_tune.h without
    // updating ZONE_PID_GAIN_MAX_MIRROR above (or vice versa) -- this goes
    // red immediately, which is the point: the two headers cannot see each
    // other, so this is the only thing that would catch a drift.
    TEST_CHECK(ITER_TUNE_GAIN_CEIL_C == ZONE_PID_GAIN_MAX_MIRROR, "gain ceiling must mirror zones_http.h's bound");
}

static void test_disabled_zone_refuses(void)
{
    TEST_SECTION("iter_tune: disabled zone refuses everything");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st)); // enabled == false by zero-init -- the default-OFF requirement
    iter_tune_firing_t f = mk_firing(7, 0x07, 25.0f, 0.05f, 10.0f, 0.1f, 1.0f);
    char reason[96];
    iter_tune_result_t r = iter_tune_process_firing(&st, &f, reason, sizeof(reason));
    // Negative test: flip st.enabled = true here -- this assertion goes red
    // (result becomes SEEDED_BASELINE), proving the check is load-bearing.
    TEST_CHECK(r == ITER_TUNE_RESULT_DISABLED, "disabled zone must refuse, not seed a baseline");
    TEST_CHECK(!st.has_baseline, "disabled zone must not acquire a baseline");
}

static void test_first_firing_seeds_baseline_no_perturbation_yet(void)
{
    TEST_SECTION("iter_tune: first firing seeds baseline");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    st.enabled = true;
    iter_tune_firing_t f = mk_firing(7, 0x07, 25.0f, 0.0500f, 10.0f, 0.10f, 1.0f);
    char reason[96];
    iter_tune_result_t r = iter_tune_process_firing(&st, &f, reason, sizeof(reason));
    TEST_CHECK(r == ITER_TUNE_RESULT_SEEDED_BASELINE, "first-ever firing must seed, not compare");
    TEST_CHECK(st.has_baseline, "baseline must now be set");
    TEST_CHECK_NEAR(st.baseline.iae_normalized, 0.0500, 1e-6, "seeded score must match input exactly");
    iter_tune_gains_t active = iter_tune_active_gains(&st);
    TEST_CHECK_NEAR(active.kp, 10.0, 1e-6, "active gains before any trial must equal seeded gains");
}

static void test_perturbation_bounded_alternates_and_reverts_exactly(void)
{
    TEST_SECTION("iter_tune: perturbation bounded, alternates, exact revert");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    st.enabled = true;
    // Non-round baseline gains, deliberately -- an exact-revert bug that
    // only shows up on values a naive rounding/re-derivation would hide is
    // exactly the kind of thing an idealized 10.0/0.1/1.0 fixture would
    // mask (this repo's own "idealized test input" bug class).
    iter_tune_firing_t seed = mk_firing(7, 0x07, 25.13f, 0.05123f, 12.34567f, 0.089123f, 3.0215f);
    char reason[96];
    iter_tune_process_firing(&st, &seed, reason, sizeof(reason));

    iter_tune_gains_t g1;
    TEST_CHECK(iter_tune_propose_perturbation(&st, &g1), "first perturbation must be proposable");
    // First call: next_perturb_negative starts false -> direction is +5%.
    TEST_CHECK_NEAR(g1.kp, 12.34567 * 1.05, 1e-3, "kp perturbation must be +5% on first call");
    TEST_CHECK_NEAR(g1.ki, 0.089123 * 1.05, 1e-5, "ki perturbation must be +5% on first call");
    TEST_CHECK_NEAR(g1.kd, 3.0215, 1e-6, "kd must be left untouched by a perturbation");
    // Negative test: a second propose while one is pending must be refused
    // (mutate the has_pending guard away in iter_tune_propose_perturbation
    // and this goes red -- it would silently overwrite the outstanding
    // trial instead of returning false).
    iter_tune_gains_t g_ignored;
    TEST_CHECK(!iter_tune_propose_perturbation(&st, &g_ignored), "cannot propose a second trial while one is pending");

    // Score the trial WORSE than baseline (0.05123 -> 0.06, a real
    // regression) -- must revert to the EXACT original bits, not a
    // recomputation from the perturbation fraction.
    iter_tune_firing_t worse = mk_firing(7, 0x07, 25.13f, 0.06000f, g1.kp, g1.ki, g1.kd);
    iter_tune_result_t r = iter_tune_process_firing(&st, &worse, reason, sizeof(reason));
    TEST_CHECK(r == ITER_TUNE_RESULT_REVERTED, "a worse score must revert");
    iter_tune_gains_t active = iter_tune_active_gains(&st);
    // Negative test: if process_firing recomputed the revert (e.g.
    // g / 1.05 instead of leaving baseline.gains untouched), floating-point
    // division would not land back on this literal -- the tolerance here
    // (1e-4) is set by float32's own decimal precision at this magnitude,
    // not by how exact the revert needs to be; a recomputed g.kp/1.05 would
    // miss it by far more than that (division is not the exact inverse of
    // multiplication in float32, and the two paths round differently).
    TEST_CHECK_NEAR(active.kp, 12.34567, 1e-4, "revert must restore the EXACT original kp, not a recomputation");
    TEST_CHECK_NEAR(active.ki, 0.089123, 1e-6, "revert must restore the EXACT original ki");
    TEST_CHECK(!st.has_pending, "revert must clear the pending trial");

    // Now propose again -- direction must have alternated to negative.
    iter_tune_gains_t g2;
    TEST_CHECK(iter_tune_propose_perturbation(&st, &g2), "second perturbation must be proposable after revert");
    TEST_CHECK_NEAR(g2.kp, 12.34567 * 0.95, 1e-3, "perturbation direction must alternate to -5% on second call");
}

static void test_perturbation_clamps_at_ceiling_and_floor(void)
{
    TEST_SECTION("iter_tune: perturbation clamps to gain bounds");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    st.enabled = true;
    // Baseline kp already at the ceiling -- a +5% nudge must clamp, not
    // exceed ITER_TUNE_GAIN_CEIL_C (== ZONE_PID_GAIN_MAX).
    iter_tune_firing_t seed = mk_firing(3, 0x01, 20.0f, 0.10f, ITER_TUNE_GAIN_CEIL_C, 0.0f, 0.0f);
    char reason[96];
    iter_tune_process_firing(&st, &seed, reason, sizeof(reason));
    iter_tune_gains_t g;
    iter_tune_propose_perturbation(&st, &g);
    // Negative test: remove the clampf() call on kp in iter_tune.c and this
    // goes red (g.kp would be 1050.0, exceeding the board's accepted bound).
    TEST_CHECK(g.kp <= ITER_TUNE_GAIN_CEIL_C, "kp perturbation must clamp at the gain ceiling");
    TEST_CHECK_NEAR(g.kp, ITER_TUNE_GAIN_CEIL_C, 1e-6, "clamped kp must equal the ceiling exactly");

    // Floor case: ki starts at 0, a -5% nudge (second call, direction now
    // flipped) of 0 stays 0 -- must not go negative.
    iter_tune_zone_state_t st2;
    memset(&st2, 0, sizeof(st2));
    st2.enabled = true;
    iter_tune_firing_t seed2 = mk_firing(3, 0x01, 20.0f, 0.10f, 5.0f, 0.0f, 0.0f);
    iter_tune_process_firing(&st2, &seed2, reason, sizeof(reason));
    st2.next_perturb_negative = true; // force the -5% direction directly
    iter_tune_gains_t g2;
    iter_tune_propose_perturbation(&st2, &g2);
    TEST_CHECK(g2.ki >= ITER_TUNE_GAIN_FLOOR_C, "ki perturbation must clamp at the gain floor, never go negative");
}

static void test_noise_floor_refuses_small_improvement(void)
{
    TEST_SECTION("iter_tune: sub-floor improvement is refused (reverted)");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    st.enabled = true;
    iter_tune_firing_t seed = mk_firing(7, 0x07, 30.0f, 0.1000f, 10.0f, 0.1f, 1.0f);
    char reason[96];
    iter_tune_process_firing(&st, &seed, reason, sizeof(reason));
    iter_tune_gains_t g;
    iter_tune_propose_perturbation(&st, &g);

    // 15% better -- chosen to sit strictly below ITER_TUNE_MIN_RELATIVE_
    // IMPROVEMENT's 20% floor without being derived from that macro.
    iter_tune_firing_t trial = mk_firing(7, 0x07, 30.0f, 0.0850f, g.kp, g.ki, g.kd);
    iter_tune_result_t r = iter_tune_process_firing(&st, &trial, reason, sizeof(reason));
    // Negative test: this is THE test that proves the noise floor is
    // load-bearing. Weaken ITER_TUNE_MIN_RELATIVE_IMPROVEMENT (e.g. to
    // 0.10f) and this specific assertion flips from REVERTED to ACCEPTED --
    // confirmed by hand during this task (see the task report) and
    // reverted immediately after.
    TEST_CHECK(r == ITER_TUNE_RESULT_REVERTED, "a 15% improvement must be refused -- below the 20% noise floor");
    TEST_CHECK_NEAR(st.baseline.iae_normalized, 0.1000, 1e-6, "baseline score must NOT move on a refused trial");
    TEST_CHECK_NEAR(st.baseline.gains.kp, 10.0, 1e-6, "baseline gains must NOT move on a refused trial");
}

static void test_noise_floor_accepts_clear_improvement(void)
{
    TEST_SECTION("iter_tune: above-floor improvement is accepted");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    st.enabled = true;
    iter_tune_firing_t seed = mk_firing(7, 0x07, 30.0f, 0.1000f, 10.0f, 0.1f, 1.0f);
    char reason[96];
    iter_tune_process_firing(&st, &seed, reason, sizeof(reason));
    iter_tune_gains_t g;
    iter_tune_propose_perturbation(&st, &g);

    // 25% better -- chosen to sit strictly above the 20% floor.
    iter_tune_firing_t trial = mk_firing(7, 0x07, 30.0f, 0.0750f, g.kp, g.ki, g.kd);
    iter_tune_result_t r = iter_tune_process_firing(&st, &trial, reason, sizeof(reason));
    // Negative test: tighten ITER_TUNE_MIN_RELATIVE_IMPROVEMENT above 0.25
    // (e.g. to 0.30f) and this assertion flips from ACCEPTED to REVERTED.
    TEST_CHECK(r == ITER_TUNE_RESULT_ACCEPTED, "a 25% improvement must be accepted -- above the 20% floor");
    TEST_CHECK_NEAR(st.baseline.iae_normalized, 0.0750, 1e-6, "accepted trial's score must become the new baseline");
    TEST_CHECK_NEAR(st.baseline.gains.kp, g.kp, 1e-9, "accepted trial's EXACT gains must become the new baseline");
    TEST_CHECK(!st.has_pending, "accept must clear the pending trial");
}

static void test_refuses_comparison_across_different_profiles(void)
{
    TEST_SECTION("iter_tune: refuses cross-profile comparison");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    st.enabled = true;
    iter_tune_firing_t seed = mk_firing(7, 0x07, 30.0f, 0.10f, 10.0f, 0.1f, 1.0f);
    char reason[96];
    iter_tune_process_firing(&st, &seed, reason, sizeof(reason));
    iter_tune_gains_t g;
    iter_tune_propose_perturbation(&st, &g);

    // Different profile_id, dramatically better score -- must still be
    // refused; a huge score improvement on the WRONG profile is not
    // evidence about this profile's gains.
    iter_tune_firing_t other_profile = mk_firing(9, 0x07, 30.0f, 0.01f, g.kp, g.ki, g.kd);
    iter_tune_result_t r = iter_tune_process_firing(&st, &other_profile, reason, sizeof(reason));
    // Negative test: delete the profile_id check in iter_tune_comparable()
    // and this goes red (result becomes ACCEPTED on cross-profile data).
    TEST_CHECK(r == ITER_TUNE_RESULT_REFUSED_NOT_COMPARABLE, "must refuse comparison across different profiles");
    TEST_CHECK(st.has_pending, "trial must stay pending -- an incomparable firing decides nothing");
    TEST_CHECK_NEAR(st.baseline.gains.kp, 10.0, 1e-6, "baseline must be untouched by a refused comparison");
}

static void test_refuses_comparison_across_different_zone_masks(void)
{
    TEST_SECTION("iter_tune: refuses cross-zone-set comparison");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    st.enabled = true;
    iter_tune_firing_t seed = mk_firing(7, 0x07, 30.0f, 0.10f, 10.0f, 0.1f, 1.0f);
    char reason[96];
    iter_tune_process_firing(&st, &seed, reason, sizeof(reason));
    iter_tune_gains_t g;
    iter_tune_propose_perturbation(&st, &g);

    iter_tune_firing_t other_mask = mk_firing(7, 0x03, 30.0f, 0.01f, g.kp, g.ki, g.kd);
    iter_tune_result_t r = iter_tune_process_firing(&st, &other_mask, reason, sizeof(reason));
    // Negative test: delete the zone_mask check and this goes red.
    TEST_CHECK(r == ITER_TUNE_RESULT_REFUSED_NOT_COMPARABLE, "must refuse comparison across different zone sets");
}

static void test_refuses_comparison_on_residual_heat(void)
{
    TEST_SECTION("iter_tune: refuses comparison on residual-heat start temp (project_autotune_needs_rested_baseline)");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    st.enabled = true;
    iter_tune_firing_t seed = mk_firing(7, 0x07, 25.0f, 0.10f, 10.0f, 0.1f, 1.0f);
    char reason[96];
    iter_tune_process_firing(&st, &seed, reason, sizeof(reason));
    iter_tune_gains_t g;
    iter_tune_propose_perturbation(&st, &g);

    // 4.9 degC apart -- literal below the 5.0 degC tolerance, must compare.
    iter_tune_firing_t close = mk_firing(7, 0x07, 29.9f, 0.05f, g.kp, g.ki, g.kd);
    iter_tune_result_t r_close = iter_tune_process_firing(&st, &close, reason, sizeof(reason));
    TEST_CHECK(r_close != ITER_TUNE_RESULT_REFUSED_NOT_COMPARABLE, "4.9 degC start-temp gap must still be comparable");

    // Reset and try the far case.
    memset(&st, 0, sizeof(st));
    st.enabled = true;
    iter_tune_process_firing(&st, &seed, reason, sizeof(reason));
    iter_tune_propose_perturbation(&st, &g);
    // 5.1 degC apart -- literal above the 5.0 degC tolerance, must refuse.
    // This pair of tests (4.9 vs 5.1) pins the actual boundary behavior
    // rather than restating ITER_TUNE_START_TEMP_TOLERANCE_C's value.
    iter_tune_firing_t far = mk_firing(7, 0x07, 30.1f, 0.05f, g.kp, g.ki, g.kd);
    iter_tune_result_t r_far = iter_tune_process_firing(&st, &far, reason, sizeof(reason));
    // Negative test: change ITER_TUNE_START_TEMP_TOLERANCE_C to 10.0f and
    // this specific assertion goes red (5.1 degC gap becomes comparable).
    TEST_CHECK(r_far == ITER_TUNE_RESULT_REFUSED_NOT_COMPARABLE, "5.1 degC start-temp gap must be refused -- residual heat");
}

static void test_baseline_refreshes_without_pending_trial(void)
{
    TEST_SECTION("iter_tune: re-firing baseline gains with no trial refreshes, does not judge");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    st.enabled = true;
    iter_tune_firing_t seed = mk_firing(7, 0x07, 30.0f, 0.10f, 10.0f, 0.1f, 1.0f);
    char reason[96];
    iter_tune_process_firing(&st, &seed, reason, sizeof(reason));
    TEST_CHECK(!st.has_pending, "no trial should be pending right after seeding");

    // Fire again on baseline gains, wildly different score -- must NOT be
    // interpreted as an accept/reject decision (no trial was outstanding),
    // just a refresh of the recorded baseline identity/score.
    iter_tune_firing_t again = mk_firing(7, 0x07, 30.0f, 0.02f, 10.0f, 0.1f, 1.0f);
    iter_tune_result_t r = iter_tune_process_firing(&st, &again, reason, sizeof(reason));
    TEST_CHECK(r == ITER_TUNE_RESULT_BASELINE_REFRESHED, "firing with no pending trial must refresh, not accept/revert");
    TEST_CHECK_NEAR(st.baseline.iae_normalized, 0.02, 1e-6, "refresh must update the recorded score");
}

// ---------------------------------------------------------------------
// Real-data sanity check: normalized IAE read directly from the two
// nominally-comparable plant_sim fixture captures cited in iter_tune.h's
// noise-floor comment (tools/PcTools/tests/fixtures/plant_sim/
// holdfix_clean.jsonl and final.jsonl, both climb_mode=coupled/
// integral_floor=ff_hold), whole-run figures read from each capture's
// final poll row's zones[].firing_stats.iae_normalized:
//   z0: 0.0275 -> 0.0337 (+22.5%, worse)
//   z1: 0.0160 -> 0.0236 (+47.5%, worse)
// These are QUANTIZED, real, non-round numbers -- not synthetic idealized
// input (this repo's documented "idealized test input" bug class). Neither
// pair should accept a change against the 20% floor since both regress;
// this pins the mechanism's behavior against actual bench-noise magnitude,
// not just synthetic percentages.
static void test_real_capture_data_regression_is_reverted(void)
{
    TEST_SECTION("iter_tune: real plant_sim capture spread (holdfix_clean -> final) reverts on both zones");
    iter_tune_zone_state_t st_z0;
    memset(&st_z0, 0, sizeof(st_z0));
    st_z0.enabled = true;
    iter_tune_firing_t seed_z0 = mk_firing(7, 0x07, 25.0f, 0.0275f, 8.0f, 0.05f, 0.5f);
    char reason[96];
    iter_tune_process_firing(&st_z0, &seed_z0, reason, sizeof(reason));
    iter_tune_gains_t g0;
    iter_tune_propose_perturbation(&st_z0, &g0);
    iter_tune_firing_t trial_z0 = mk_firing(7, 0x07, 25.0f, 0.0337f, g0.kp, g0.ki, g0.kd);
    iter_tune_result_t r0 = iter_tune_process_firing(&st_z0, &trial_z0, reason, sizeof(reason));
    TEST_CHECK(r0 == ITER_TUNE_RESULT_REVERTED, "z0's real-capture regression (0.0275->0.0337) must revert");

    iter_tune_zone_state_t st_z1;
    memset(&st_z1, 0, sizeof(st_z1));
    st_z1.enabled = true;
    iter_tune_firing_t seed_z1 = mk_firing(7, 0x07, 25.0f, 0.0160f, 7.0f, 0.04f, 0.4f);
    iter_tune_process_firing(&st_z1, &seed_z1, reason, sizeof(reason));
    iter_tune_gains_t g1;
    iter_tune_propose_perturbation(&st_z1, &g1);
    iter_tune_firing_t trial_z1 = mk_firing(7, 0x07, 25.0f, 0.0236f, g1.kp, g1.ki, g1.kd);
    iter_tune_result_t r1 = iter_tune_process_firing(&st_z1, &trial_z1, reason, sizeof(reason));
    TEST_CHECK(r1 == ITER_TUNE_RESULT_REVERTED, "z1's real-capture regression (0.0160->0.0236) must revert");

    // And the mirror direction (final -> holdfix_clean, i.e. treating the
    // SAME pair as an improvement) is large enough on z1 (47.5%) to clear
    // the 20% floor and accept -- proving the floor doesn't just always
    // say no.
    iter_tune_zone_state_t st_z1b;
    memset(&st_z1b, 0, sizeof(st_z1b));
    st_z1b.enabled = true;
    iter_tune_firing_t seed_z1b = mk_firing(7, 0x07, 25.0f, 0.0236f, 7.0f, 0.04f, 0.4f);
    iter_tune_process_firing(&st_z1b, &seed_z1b, reason, sizeof(reason));
    iter_tune_gains_t g1b;
    iter_tune_propose_perturbation(&st_z1b, &g1b);
    iter_tune_firing_t trial_z1b = mk_firing(7, 0x07, 25.0f, 0.0160f, g1b.kp, g1b.ki, g1b.kd);
    iter_tune_result_t r1b = iter_tune_process_firing(&st_z1b, &trial_z1b, reason, sizeof(reason));
    TEST_CHECK(r1b == ITER_TUNE_RESULT_ACCEPTED, "z1's real-capture 47.5% improvement direction must accept");
}

void run_test_iter_tune(void)
{
    test_gain_ceil_mirrors_zone_pid_gain_max();
    test_disabled_zone_refuses();
    test_first_firing_seeds_baseline_no_perturbation_yet();
    test_perturbation_bounded_alternates_and_reverts_exactly();
    test_perturbation_clamps_at_ceiling_and_floor();
    test_noise_floor_refuses_small_improvement();
    test_noise_floor_accepts_clear_improvement();
    test_refuses_comparison_across_different_profiles();
    test_refuses_comparison_across_different_zone_masks();
    test_refuses_comparison_on_residual_heat();
    test_baseline_refreshes_without_pending_trial();
    test_real_capture_data_regression_is_reverted();
}
