// test_iter_tune.c -- ITER_TUNE_REDESIGN_PLAN.md steps 1, 2 and 5:
// firing_score.c (per-segment tracking scoring), firing_compare.c
// (matched-pair comparator + accept rule) and the rewritten iter_tune.c
// decision core. Pure host tests, no ESP-IDF stubs needed.
//
// The old whole-firing-IAE tests are gone with the code they tested (plan
// step 5's gate: "the old whole-firing path fully removed, not left dual").
//
// NEGATIVE TESTS. Each block below names the PRODUCTION edit that makes it
// go red -- not a test-local copy, per this repo's standing rule that a
// negative test on a mirror is vacuous. Two were actually run and confirmed
// RED during this change (see the commit message):
//   - firing_compare.c's veto: change `ss->median_normalised >= 1.0f` to
//     `>= 100.0f` -> test_veto_rejects_a_trade goes red.
//   - iter_tune.c's cage: drop the anchor clamp in
//     iter_tune_clamp_to_cage() -> test_cage_clamps_to_anchor goes red.

#include "test_common.h"
#include "../drivers/control/iter_tune.h"
#include "../drivers/control/firing_score.h"
#include "../drivers/control/firing_compare.h"

#include <string.h>

// Mirrors ZONE_PID_GAIN_MAX (zones_http.h). If this literal and
// ITER_TUNE_GAIN_CEIL_C (iter_tune.h) ever drift apart, this line -- not a
// silent clamp mismatch -- is where it will be caught.
#define ZONE_PID_GAIN_MAX_MIRROR 1000.0f

static firing_score_cfg_t mk_cfg(void)
{
    firing_score_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.band_c = 5.0f;
    c.temp_bucket_c = 5.0f;
    c.rate_bucket_c_per_hr = 25.0f;
    c.min_scored_ticks = 10;
    return c;
}

// ------------------------------------------------------------------ score

static void test_gain_ceil_mirrors_zone_pid_gain_max(void)
{
    TEST_SECTION("iter_tune: ITER_TUNE_GAIN_CEIL_C mirrors ZONE_PID_GAIN_MAX");
    // Negative test: change ITER_TUNE_GAIN_CEIL_C in iter_tune.h without
    // changing ZONE_PID_GAIN_MAX -> red here.
    TEST_CHECK(ITER_TUNE_GAIN_CEIL_C == ZONE_PID_GAIN_MAX_MIRROR, "ceiling mirrors zone gain max");
}

static void test_classify_rate(void)
{
    TEST_SECTION("firing_score: a near-zero commanded rate is a DWELL, not a ramp");
    // This is what stops lag_s dividing by a near-zero rate. Negative test:
    // lower FIRING_SCORE_RAMP_MIN_RATE_C_PER_HR to 1.0f -> red.
    TEST_CHECK(firing_score_classify(0.0f) == FIRING_SEG_DWELL, "0 C/hr is a dwell");
    TEST_CHECK(firing_score_classify(5.0f) == FIRING_SEG_DWELL, "5 C/hr is a dwell");
    TEST_CHECK(firing_score_classify(60.0f) == FIRING_SEG_RAMP_UP, "60 C/hr is a ramp up");
    TEST_CHECK(firing_score_classify(-60.0f) == FIRING_SEG_RAMP_DOWN, "-60 C/hr is a ramp down");
}

// Two ramps, same TRACKING LAG in seconds but wildly different commanded
// rates and error magnitudes, must score the same lag_s. This is the whole
// reason lag is expressed in seconds.
static void test_lag_is_rate_normalised(void)
{
    TEST_SECTION("firing_score: 0.5C at 50C/hr and 5C at 500C/hr are the same lag");
    firing_score_cfg_t cfg = mk_cfg();
    firing_score_seg_t a, b;
    bool cap_a = true, cap_b = true;

    firing_score_seg_begin(&a, &cfg, 0, 50.0f, 100.0f, 30.0f, 100.0f);
    firing_score_seg_begin(&b, &cfg, 0, 500.0f, 100.0f, 30.0f, 100.0f);
    for (int t = 0; t < 200; t++) {
        firing_score_seg_tick(&a, &cap_a, 100.0f, 99.5f, false, 1.0f);
        firing_score_seg_tick(&b, &cap_b, 100.0f, 95.0f, false, 1.0f);
    }
    firing_segment_score_t sa, sb;
    TEST_CHECK(firing_score_seg_finish(&a, &sa), "slow ramp scored");
    TEST_CHECK(firing_score_seg_finish(&b, &sb), "fast ramp scored");
    // 0.5 / (50/3600) = 36 s; 5.0 / (500/3600) = 36 s. Histogram bin is 2 s.
    TEST_CHECK_NEAR(sa.value[FIRING_SUBSCORE_LAG_S], 36.0f, 2.0f, "slow ramp lag ~36 s");
    TEST_CHECK_NEAR(sb.value[FIRING_SUBSCORE_LAG_S], 36.0f, 2.0f, "fast ramp lag ~36 s");
}

// THE CENTRAL CLAIM OF THE REDESIGN: two firings differing only in start
// temperature must score identically. Negative test: delete the
// `if (zone_captured && !*zone_captured)` early-return block in
// firing_score.c's firing_score_seg_tick() -> red.
static void test_start_temperature_does_not_change_the_score(void)
{
    TEST_SECTION("firing_score: capture-transient exclusion makes start temperature irrelevant");
    firing_score_cfg_t cfg = mk_cfg();

    float scores[2];
    for (int cold = 0; cold < 2; cold++) {
        bool captured = false;
        firing_score_seg_t seg;
        firing_score_seg_begin(&seg, &cfg, 0, 0.0f, 100.0f, 30.0f, 100.0f);
        // A cold start spends 300 ticks climbing from 20 C; a warm start
        // only 20. Both then hold at exactly the same 1.0 C offset.
        int prefix = cold ? 300 : 20;
        for (int t = 0; t < prefix; t++) {
            float actual = cold ? 20.0f + (float)t * 0.2f : 90.0f + (float)t * 0.2f;
            if (actual > 94.0f) actual = 94.0f; // stays outside the 5 C band
            firing_score_seg_tick(&seg, &captured, 100.0f, actual, false, 1.0f);
        }
        for (int t = 0; t < 600; t++) {
            firing_score_seg_tick(&seg, &captured, 100.0f, 101.0f, false, 1.0f);
        }
        firing_segment_score_t s;
        TEST_CHECK(firing_score_seg_finish(&seg, &s), "segment scored");
        scores[cold] = s.value[FIRING_SUBSCORE_STEADY_RMS_C];
    }
    TEST_CHECK_NEAR(scores[0], scores[1], 0.001f, "warm and cold starts score identically");
}

static void test_saturated_ticks_are_excluded(void)
{
    TEST_SECTION("firing_score: saturated-and-still-cold ticks are not scored");
    // Negative test: delete the `if (saturated_high && err < 0.0f) return;`
    // line in firing_score.c -> red.
    firing_score_cfg_t cfg = mk_cfg();
    firing_score_seg_t seg;
    bool cap = true;
    firing_score_seg_begin(&seg, &cfg, 0, 0.0f, 100.0f, 30.0f, 100.0f);
    for (int t = 0; t < 400; t++) {
        firing_score_seg_tick(&seg, &cap, 100.0f, 96.0f, true, 1.0f); // saturated, below target
    }
    firing_segment_score_t s;
    TEST_CHECK(!firing_score_seg_finish(&seg, &s), "all-saturated segment is dropped, not scored");
}

static void test_short_segment_dropped(void)
{
    TEST_SECTION("firing_score: a segment shorter than min_scored_ticks is dropped");
    firing_score_cfg_t cfg = mk_cfg();
    firing_score_seg_t seg;
    bool cap = true;
    firing_score_seg_begin(&seg, &cfg, 0, 0.0f, 100.0f, 30.0f, 100.0f);
    for (int t = 0; t < 5; t++) firing_score_seg_tick(&seg, &cap, 100.0f, 100.0f, false, 1.0f);
    firing_segment_score_t s;
    TEST_CHECK(!firing_score_seg_finish(&seg, &s), "5 ticks < 10 min ticks");
}

// ---------------------------------------------- 2026-09-13 scorecard fixes
// (docs/audits/firing_score_four_objective_scorecard_2026-09-13.md).

// Objective 2 (settle quickly) previously had NO instrument: a quick,
// clean settle and a long ring that decays before the entry window ends
// scored identically. This proves SETTLE_S now distinguishes them.
// Negative test: comment out the `seg->settle_last_outside_s = seg->elapsed_s;`
// line in firing_score.c's dwell tick branch -> both segments below read
// settle_s == 0.0 and this test goes red.
//
// Excursion amplitudes here are sized against FIRING_SCORE_SETTLE_BAND_C,
// which was re-sized from the plant (0.5 -> 2.0 degC) on 2026-09-14; see
// docs/audits/firing_score_subscore_enrolment_2026-09-14.md.
static void test_settle_time_distinguishes_ringing_from_quick_settle(void)
{
    TEST_SECTION("firing_score: SETTLE_S sees a slow ring the old subscores could not");
    firing_score_cfg_t cfg = mk_cfg();
    firing_segment_score_t quick, ring;

    // Quick settle: enters the settle band immediately and never leaves.
    {
        firing_score_seg_t seg;
        bool cap = true;
        firing_score_seg_begin(&seg, &cfg, 0, 0.0f, 100.0f, 30.0f, 100.0f);
        for (int t = 0; t < 500; t++) firing_score_seg_tick(&seg, &cap, 100.0f, 100.1f, false, 1.0f);
        TEST_CHECK(firing_score_seg_finish(&seg, &quick), "quick-settle segment scored");
    }
    // Rings well outside the settle band for the first ~190s (same peak
    // magnitude as the quick case would never reach at all, and no worse
    // eventual steady RMS), then settles cleanly for the remaining ~300s --
    // exactly the case Finding A says the old three subscores cannot tell
    // apart from a clean settle, since a peak/RMS-only view sees the same
    // eventual quiet tail either way.
    {
        firing_score_seg_t seg;
        bool cap = true;
        firing_score_seg_begin(&seg, &cfg, 0, 0.0f, 100.0f, 30.0f, 100.0f);
        for (int t = 0; t < 190; t++) {
            float actual = 100.0f + ((t % 20 < 10) ? 3.0f : -3.0f); // +-3C, outside the settle band
            firing_score_seg_tick(&seg, &cap, 100.0f, actual, false, 1.0f);
        }
        for (int t = 0; t < 300; t++) firing_score_seg_tick(&seg, &cap, 100.0f, 100.1f, false, 1.0f);
        TEST_CHECK(firing_score_seg_finish(&seg, &ring), "ringing-then-settled segment scored");
    }

    TEST_CHECK(quick.has[FIRING_SUBSCORE_SETTLE_S], "quick settle has a settle time");
    TEST_CHECK(ring.has[FIRING_SUBSCORE_SETTLE_S], "ringing segment has a settle time");
    TEST_CHECK_NEAR(quick.value[FIRING_SUBSCORE_SETTLE_S], 0.0f, 2.0f, "quick settle is ~0 s");
    TEST_CHECK(ring.value[FIRING_SUBSCORE_SETTLE_S] > 150.0f,
               "ringing segment's settle time reflects the ~190s it spends outside band");
    TEST_CHECK(ring.value[FIRING_SUBSCORE_SETTLE_S] > quick.value[FIRING_SUBSCORE_SETTLE_S] + 100.0f,
               "SETTLE_S separates the two where peak/steady-RMS alone could not");
}

// A dwell that never comes to rest inside the settle band must not report a
// NUMBER at all. Until 2026-09-14 it reported the segment's own duration,
// which is the documented in-band-sentinel hazard: indistinguishable from a
// slow-but-settled dwell, and consumed as a measurement by firing_compare's
// plain difference. Measured consequence (audit R5): across six real
// matched-condition captures the saturated readings for one dwell class
// spread 131 s against a 60 s Bar-1 floor, i.e. the sentinel alone could
// ACCEPT or REJECT between two identical-gain firings.
//
// Negative test: restore the old encoding (make seg_finish() report
// settle_last_outside_s unconditionally, ignoring settle_outside_at_end)
// -> has[SETTLE_S] becomes true with value ~= the duration and this goes red.
static void test_settle_time_never_settling_reports_nothing_not_a_sentinel(void)
{
    TEST_SECTION("firing_score: a dwell that never settles reports NO settle time, not a sentinel");
    firing_score_cfg_t cfg = mk_cfg();
    firing_score_seg_t seg;
    bool cap = true;
    firing_score_seg_begin(&seg, &cfg, 0, 0.0f, 100.0f, 30.0f, 100.0f);
    // Stays 4C off target (outside the settle band) for the whole 400-tick
    // segment. captured=true from the start so none of this is discarded by
    // the capture-transient exclusion.
    for (int t = 0; t < 400; t++) firing_score_seg_tick(&seg, &cap, 100.0f, 104.0f, false, 1.0f);
    firing_segment_score_t s;
    TEST_CHECK(firing_score_seg_finish(&seg, &s), "segment scored");
    TEST_CHECK(!s.has[FIRING_SUBSCORE_SETTLE_S],
               "never-settled reports has == false, the comparator's first-class 'nothing to say'");
    TEST_CHECK(s.dwell_unsettled == 1,
               "the FACT of not settling is still reported, out of band, for humans");

    // A slow-but-settled dwell of the SAME length is now distinguishable: it
    // reports a real number. Under the old encoding both read ~400 s.
    firing_score_seg_t slow;
    bool cap2 = true;
    firing_score_seg_begin(&slow, &cfg, 0, 0.0f, 100.0f, 30.0f, 100.0f);
    for (int t = 0; t < 395; t++) firing_score_seg_tick(&slow, &cap2, 100.0f, 104.0f, false, 1.0f);
    for (int t = 0; t < 5; t++)   firing_score_seg_tick(&slow, &cap2, 100.0f, 100.1f, false, 1.0f);
    firing_segment_score_t ss;
    TEST_CHECK(firing_score_seg_finish(&slow, &ss), "slow-but-settled segment scored");
    TEST_CHECK(ss.has[FIRING_SUBSCORE_SETTLE_S], "slow-but-settled DOES report a settle time");
    TEST_CHECK(ss.dwell_unsettled == 0, "and is not counted as unsettled");
}

// A class merging a settled and an unsettled dwell must not let the settled
// one's number stand in for the class -- that would resurrect the sentinel
// one level up. Negative test: drop the `s == FIRING_SUBSCORE_SETTLE_S`
// branch in firing_score_set_add()'s merge loop -> has becomes true and this
// goes red.
static void test_settle_merge_requires_both_dwells_to_have_settled(void)
{
    TEST_SECTION("firing_score: SETTLE_S merges only when BOTH same-class dwells settled");
    firing_score_cfg_t cfg = mk_cfg();
    firing_score_set_t set;
    memset(&set, 0, sizeof(set));

    // Settled dwell of this class.
    {
        firing_score_seg_t seg;
        bool cap = true;
        firing_score_seg_begin(&seg, &cfg, 0, 0.0f, 100.0f, 30.0f, 100.0f);
        for (int t = 0; t < 300; t++) firing_score_seg_tick(&seg, &cap, 100.0f, 100.1f, false, 1.0f);
        TEST_CHECK(firing_score_set_finish_segment(&set, &seg), "settled dwell added");
    }
    TEST_CHECK(set.entry[0].has[FIRING_SUBSCORE_SETTLE_S], "settled dwell alone reports a settle time");
    // Same class, never settles.
    {
        firing_score_seg_t seg;
        bool cap = true;
        firing_score_seg_begin(&seg, &cfg, 0, 0.0f, 100.0f, 30.0f, 100.0f);
        for (int t = 0; t < 300; t++) firing_score_seg_tick(&seg, &cap, 100.0f, 104.0f, false, 1.0f);
        TEST_CHECK(firing_score_set_finish_segment(&set, &seg), "unsettled dwell merged into the class");
    }
    TEST_CHECK(set.count == 1, "both dwells are the same class");
    TEST_CHECK(!set.entry[0].has[FIRING_SUBSCORE_SETTLE_S],
               "the merged class reports no settle time once one of its dwells never settled");
    TEST_CHECK(set.entry[0].dwell_unsettled == 1, "the unsettled count survives the merge");
}

// Objective 4b (undershoot): a dwell entered from below must be measured,
// and that measurement must NOT be cancelled by a later recovery. Negative
// test: delete the `seg->entry_trough_c = err;` update (or force
// ENTRY_UNDERSHOOT_C's value to 0.0f in firing_score_seg_finish()) -> red.
static void test_undershoot_measured_and_not_cancelled_by_recovery(void)
{
    TEST_SECTION("firing_score: undershoot has its own axis and survives recovery");
    firing_score_cfg_t cfg = mk_cfg();
    firing_score_seg_t seg;
    bool cap = true;
    // dead_time=30, tau=20 -> entry_window_s = 70s.
    firing_score_seg_begin(&seg, &cfg, 0, 0.0f, 100.0f, 30.0f, 20.0f);
    // Enters 3C low, then recovers to a perfect track well inside the entry
    // window, and stays perfect through the steady phase.
    for (int t = 0; t < 20; t++) firing_score_seg_tick(&seg, &cap, 100.0f, 97.0f, false, 1.0f);
    for (int t = 0; t < 50; t++) firing_score_seg_tick(&seg, &cap, 100.0f, 100.0f, false, 1.0f);
    for (int t = 0; t < 200; t++) firing_score_seg_tick(&seg, &cap, 100.0f, 100.0f, false, 1.0f);
    firing_segment_score_t s;
    TEST_CHECK(firing_score_seg_finish(&seg, &s), "segment scored");
    TEST_CHECK(s.has[FIRING_SUBSCORE_ENTRY_UNDERSHOOT_C], "undershoot is reported");
    TEST_CHECK_NEAR(s.value[FIRING_SUBSCORE_ENTRY_UNDERSHOOT_C], 3.0f, 0.01f,
                    "the 3C undershoot is measured, not erased by the recovery");
    TEST_CHECK(s.has[FIRING_SUBSCORE_ENTRY_PEAK_C], "overshoot axis still reports (never entered from above)");
    TEST_CHECK_NEAR(s.value[FIRING_SUBSCORE_ENTRY_PEAK_C], 0.0f, 0.01f,
                    "overshoot axis stays 0 -- undershoot does not leak onto it");
}

// The accept-permissive gap this closes, proven through the real comparator:
// before this change, an entry undershooting 3C and a perfect entry scored
// IDENTICALLY on every subscore (both entry_peak_c == 0). With
// ENTRY_UNDERSHOOT_C wired in, the undershooting trial must come out
// strictly worse -- and with enough matched pairs, the no-degradation veto
// fires on it, closing the "worse controller looks equal" gap Finding B
// describes.
static void test_undershoot_makes_a_worse_trial_score_worse(void)
{
    TEST_SECTION("firing_compare: an undershooting trial no longer scores equal to a clean one");
    firing_score_cfg_t cfg = mk_cfg();
    firing_score_set_t base_set, trial_set;
    memset(&base_set, 0, sizeof(base_set));
    memset(&trial_set, 0, sizeof(trial_set));

    // Three DIFFERENT classes (by zone index), not three reps of one class --
    // firing_score_set_add() merges same-key segments into a single averaged
    // observation (by design, plan sec 2.2: repeats of one class in a firing
    // are one observation, not several), so the veto's n >= 3 needs 3
    // distinct classes here, not 3 repeats of the same one.
    for (int zone = 0; zone < 3; zone++) {
        // Baseline: perfect dwell entry.
        {
            firing_score_seg_t seg;
            bool cap = true;
            firing_score_seg_begin(&seg, &cfg, (uint8_t)zone, 0.0f, 100.0f, 30.0f, 20.0f);
            for (int t = 0; t < 300; t++) firing_score_seg_tick(&seg, &cap, 100.0f, 100.0f, false, 1.0f);
            TEST_CHECK(firing_score_set_finish_segment(&base_set, &seg), "baseline zone scored");
        }
        // Trial: same class, but enters 2C low and recovers.
        {
            firing_score_seg_t seg;
            bool cap = true;
            firing_score_seg_begin(&seg, &cfg, (uint8_t)zone, 0.0f, 100.0f, 30.0f, 20.0f);
            for (int t = 0; t < 20; t++) firing_score_seg_tick(&seg, &cap, 100.0f, 98.0f, false, 1.0f);
            for (int t = 0; t < 280; t++) firing_score_seg_tick(&seg, &cap, 100.0f, 100.0f, false, 1.0f);
            TEST_CHECK(firing_score_set_finish_segment(&trial_set, &seg), "trial zone scored");
        }
    }

    firing_compare_result_t r;
    firing_compare_verdict_t v = firing_compare(&base_set, &trial_set, NULL, &r);
    TEST_CHECK(r.sub[FIRING_SUBSCORE_ENTRY_UNDERSHOOT_C].n >= FIRING_COMPARE_VETO_MIN_N,
               "undershoot axis has enough matched pairs to arm the veto");
    TEST_CHECK(r.sub[FIRING_SUBSCORE_ENTRY_UNDERSHOOT_C].degraded,
               "the undershooting trial is flagged as degraded on its OWN axis");
    TEST_CHECK(v == FIRING_COMPARE_REJECT_DEGRADED,
               "the comparator rejects a trial that only the old scorecard would have called equal");
}

// Finding C, part 1: LAG_S is unsigned, so converting a lag into an equal
// lead reads as "no change". LAG_SIGNED_S must tell them apart while LAG_S
// itself stays byte-identical (same |value| either way).
static void test_lag_signed_distinguishes_lead_from_lag(void)
{
    TEST_SECTION("firing_score: LAG_SIGNED_S sees direction that LAG_S (unchanged) cannot");
    firing_score_cfg_t cfg = mk_cfg();
    firing_segment_score_t behind, ahead;

    { // ramp-up, actual trails target the whole way -- BEHIND schedule.
        firing_score_seg_t seg;
        bool cap = true;
        firing_score_seg_begin(&seg, &cfg, 0, 50.0f, 100.0f, 30.0f, 100.0f);
        for (int t = 0; t < 200; t++) firing_score_seg_tick(&seg, &cap, 100.0f, 99.5f, false, 1.0f);
        TEST_CHECK(firing_score_seg_finish(&seg, &behind), "behind-schedule ramp scored");
    }
    { // ramp-up, actual leads target the whole way -- AHEAD of schedule.
        firing_score_seg_t seg;
        bool cap = true;
        firing_score_seg_begin(&seg, &cfg, 0, 50.0f, 100.0f, 30.0f, 100.0f);
        for (int t = 0; t < 200; t++) firing_score_seg_tick(&seg, &cap, 100.0f, 100.5f, false, 1.0f);
        TEST_CHECK(firing_score_seg_finish(&seg, &ahead), "ahead-of-schedule ramp scored");
    }

    // LAG_S (unsigned, UNCHANGED behaviour): both read the same magnitude --
    // this is the blind spot, demonstrated on the untouched production axis.
    TEST_CHECK_NEAR(behind.value[FIRING_SUBSCORE_LAG_S], ahead.value[FIRING_SUBSCORE_LAG_S], 2.0f,
                    "LAG_S alone cannot tell a lag from an equal lead (unchanged, by design)");

    // LAG_SIGNED_S: opposite signs, same magnitude.
    TEST_CHECK(behind.has[FIRING_SUBSCORE_LAG_SIGNED_S] && ahead.has[FIRING_SUBSCORE_LAG_SIGNED_S],
               "both ramps report a signed lag");
    TEST_CHECK(behind.value[FIRING_SUBSCORE_LAG_SIGNED_S] > 0.0f, "behind-schedule is positive");
    TEST_CHECK(ahead.value[FIRING_SUBSCORE_LAG_SIGNED_S] < 0.0f, "ahead-of-schedule is negative");
    TEST_CHECK_NEAR(behind.value[FIRING_SUBSCORE_LAG_SIGNED_S], -ahead.value[FIRING_SUBSCORE_LAG_SIGNED_S], 2.0f,
                    "same magnitude, opposite sign");
}

// Finding C, part 2: LAG_S drops exactly the ticks where a saturated zone is
// still short of target -- the ticks where objective 1 is failing hardest.
// LAG_SIGNED_S must see them. Negative test: gate the LAG_SIGNED_S feed in
// firing_score_seg_tick() on `!(saturated_high && err < 0.0f)` (i.e. move it
// after the infeasibility exclusion) -> lag_signed_samples drops to equal
// lag_samples and this test goes red.
static void test_lag_signed_includes_saturated_short_ticks(void)
{
    TEST_SECTION("firing_score: LAG_SIGNED_S is not blind to saturated-and-short ramp ticks");
    firing_score_cfg_t cfg = mk_cfg();
    firing_score_seg_t seg;
    bool cap = true;
    firing_score_seg_begin(&seg, &cfg, 0, 50.0f, 100.0f, 30.0f, 100.0f);
    // 300 ticks saturated and far short of target (LAG_S drops these), then
    // enough on-track ticks to clear min_scored_ticks so the segment is not
    // dropped outright.
    for (int t = 0; t < 300; t++) firing_score_seg_tick(&seg, &cap, 100.0f, 50.0f, true, 1.0f);
    for (int t = 0; t < 50; t++) firing_score_seg_tick(&seg, &cap, 100.0f, 99.5f, false, 1.0f);
    firing_segment_score_t s;
    TEST_CHECK(firing_score_seg_finish(&seg, &s), "segment scored (the 50 on-track ticks clear min_scored_ticks)");
    TEST_CHECK(s.has[FIRING_SUBSCORE_LAG_S] && s.has[FIRING_SUBSCORE_LAG_SIGNED_S], "both lag axes present");
    // LAG_S saw only the 50 unsaturated ticks (all ~36s lag). LAG_SIGNED_S
    // saw all 350 -- dominated by the 300 badly-behind saturated ticks, so
    // its median must read far worse than LAG_S's.
    TEST_CHECK(s.value[FIRING_SUBSCORE_LAG_SIGNED_S] > s.value[FIRING_SUBSCORE_LAG_S] + 100.0f,
               "LAG_SIGNED_S reflects the saturated-short ticks LAG_S excludes");
}

// ---------------------------------------------------------------- compare

static void add_seg(firing_score_set_t *set, uint8_t zone, firing_seg_kind_t kind, int16_t rate_bucket,
                    int16_t temp_bucket, float rate_c_per_s, const float *values, const bool *has,
                    float in_band)
{
    firing_segment_score_t s;
    memset(&s, 0, sizeof(s));
    s.key.zone_index = zone;
    s.key.kind = (uint8_t)kind;
    s.key.rate_bucket = rate_bucket;
    s.key.temp_bucket = temp_bucket;
    s.rate_c_per_s = rate_c_per_s;
    s.scored_ticks = 600;
    s.merged = 1;
    s.in_band_frac = in_band;
    for (int i = 0; i < FIRING_SUBSCORE_COUNT; i++) { s.has[i] = has[i]; s.value[i] = values[i]; }
    TEST_CHECK(firing_score_set_add(set, &s), "segment added to set");
}

static void add_dwell(firing_score_set_t *set, int16_t temp_bucket, float entry, float steady, float in_band)
{
    float v[FIRING_SUBSCORE_COUNT] = {0.0f, entry, steady};
    bool h[FIRING_SUBSCORE_COUNT] = {false, true, true};
    add_seg(set, 0, FIRING_SEG_DWELL, 0, temp_bucket, 0.0f, v, h, in_band);
}

// ------------------------------------- 2026-09-14 subscore-enrolment guards
// (docs/audits/firing_score_subscore_enrolment_2026-09-14.md).
//
// THE REGRESSION THIS GUARDS. d41da85f added three sub-scores and raised
// FIRING_SUBSCORE_COUNT 3 -> 6. Because firing_compare.c looped to
// FIRING_SUBSCORE_COUNT in BOTH its accumulation and verdict loops, all
// three were enrolled into Bar 1, the no-degradation veto, the low-n gate
// and the composite by that increment alone -- no edit to firing_compare.c,
// nothing in the commit message. Five of 660 A1 null comparisons flipped.
// The two tests below fail if that ever happens again.

// Guard 1: the voting set is pinned BY NAME. Adding a sub-score cannot
// enrol it silently -- this test names every axis and its classification,
// and firing_compare.h's _Static_assert refuses to compile an unclassified
// one. Negative test: move FIRING_SUBSCORE_SETTLE_S from
// FIRING_COMPARE_REPORT_ONLY_MASK to FIRING_COMPARE_VOTING_MASK -> red.
static void test_subscore_vote_enrolment_is_explicit(void)
{
    TEST_SECTION("firing_compare: which sub-scores vote is an explicit, pinned list");

    TEST_CHECK(firing_compare_subscore_votes(FIRING_SUBSCORE_LAG_S), "LAG_S votes");
    TEST_CHECK(firing_compare_subscore_votes(FIRING_SUBSCORE_ENTRY_PEAK_C), "ENTRY_PEAK_C votes");
    TEST_CHECK(firing_compare_subscore_votes(FIRING_SUBSCORE_STEADY_RMS_C), "STEADY_RMS_C votes");
    TEST_CHECK(firing_compare_subscore_votes(FIRING_SUBSCORE_ENTRY_UNDERSHOOT_C), "ENTRY_UNDERSHOOT_C votes");

    TEST_CHECK(!firing_compare_subscore_votes(FIRING_SUBSCORE_SETTLE_S),
               "SETTLE_S is REPORT-ONLY (band just re-sized; not yet validated as a decision axis)");
    TEST_CHECK(!firing_compare_subscore_votes(FIRING_SUBSCORE_LAG_SIGNED_S),
               "LAG_SIGNED_S is REPORT-ONLY (signed: 'lower is better' cannot adjudicate it)");

    // Exactly four voters, and every enum value classified exactly once.
    int voters = 0;
    for (int i = 0; i < FIRING_SUBSCORE_COUNT; i++) {
        if (firing_compare_subscore_votes((firing_subscore_t)i)) voters++;
    }
    TEST_CHECK(voters == 4, "exactly four sub-scores vote -- a fifth is a deliberate act, not a side effect");
    TEST_CHECK(FIRING_COMPARE_CLASSIFIED_MASK == FIRING_COMPARE_ALL_SUBSCORES_MASK,
               "every sub-score is classified voting or report-only");
    TEST_CHECK((FIRING_COMPARE_VOTING_MASK & FIRING_SUBSCORE_SIGNED_MASK) == 0u,
               "no SIGNED sub-score is in the voting set");
}

// Guard 2: the behavioural half. A report-only axis is MEASURED (n, medians
// all present) but cannot move a verdict in EITHER direction -- it can
// neither manufacture an ACCEPT nor fire the veto, and it stays out of the
// human composite. Negative test: delete the `if (!ss->votes) continue;`
// line in firing_compare.c -> both halves go red.
static void test_report_only_subscores_cannot_change_a_verdict(void)
{
    TEST_SECTION("firing_compare: a report-only sub-score is measured but never decides");

    const float LAG_RATE = 100.0f / 3600.0f;   // 100 degC/hr ramp class
    // Voting axes IDENTICAL in both sets; only the report-only axes differ.
    for (int direction = 0; direction < 2; direction++) {
        const float sign = (direction == 0) ? -1.0f : +1.0f; // "improvement" then "degradation"
        firing_score_set_t base_set, trial_set;
        memset(&base_set, 0, sizeof(base_set));
        memset(&trial_set, 0, sizeof(trial_set));

        for (uint8_t zone = 0; zone < 3; zone++) {
            float vb[FIRING_SUBSCORE_COUNT] = {0};
            bool  hb[FIRING_SUBSCORE_COUNT] = {0};
            vb[FIRING_SUBSCORE_LAG_S] = 120.0f;             hb[FIRING_SUBSCORE_LAG_S] = true;
            vb[FIRING_SUBSCORE_SETTLE_S] = 600.0f;          hb[FIRING_SUBSCORE_SETTLE_S] = true;
            vb[FIRING_SUBSCORE_LAG_SIGNED_S] = 0.0f;        hb[FIRING_SUBSCORE_LAG_SIGNED_S] = true;
            float vt[FIRING_SUBSCORE_COUNT];
            bool  ht[FIRING_SUBSCORE_COUNT];
            memcpy(vt, vb, sizeof(vt));
            memcpy(ht, hb, sizeof(ht));
            // Report-only axes moved by ~10 Bar-1 floors each (600 s against
            // a 60 s settle floor; 180 s against the lag floor of
            // 0.5/rate == 18 s).
            vt[FIRING_SUBSCORE_SETTLE_S] = 600.0f + sign * 600.0f;
            vt[FIRING_SUBSCORE_LAG_SIGNED_S] = sign * 180.0f;

            add_seg(&base_set, zone, FIRING_SEG_RAMP_UP, 4, 4, LAG_RATE, vb, hb, 0.95f);
            add_seg(&trial_set, zone, FIRING_SEG_RAMP_UP, 4, 4, LAG_RATE, vt, ht, 0.95f);
        }

        firing_compare_result_t r;
        firing_compare_verdict_t v = firing_compare(&base_set, &trial_set, NULL, &r);

        // Measured: both report-only axes have their full matched-pair
        // statistics. This is not "excluded", it is "does not vote".
        TEST_CHECK(r.sub[FIRING_SUBSCORE_SETTLE_S].n == 3, "SETTLE_S is still measured (n == 3)");
        TEST_CHECK(r.sub[FIRING_SUBSCORE_LAG_SIGNED_S].n == 3, "LAG_SIGNED_S is still measured (n == 3)");
        TEST_CHECK(!r.sub[FIRING_SUBSCORE_SETTLE_S].votes, "SETTLE_S is marked non-voting on the result");
        TEST_CHECK(!r.sub[FIRING_SUBSCORE_LAG_SIGNED_S].votes, "LAG_SIGNED_S is marked non-voting");
        TEST_CHECK(r.sub[FIRING_SUBSCORE_LAG_S].votes, "LAG_S is marked voting");

        // Never decides: neither half of the verdict moves.
        TEST_CHECK(!r.sub[FIRING_SUBSCORE_SETTLE_S].bar1_cleared, "a report-only axis never clears Bar 1");
        TEST_CHECK(!r.sub[FIRING_SUBSCORE_LAG_SIGNED_S].bar1_cleared, "nor does the signed lag axis");
        TEST_CHECK(!r.sub[FIRING_SUBSCORE_SETTLE_S].degraded, "a report-only axis never fires the veto");
        TEST_CHECK(!r.sub[FIRING_SUBSCORE_LAG_SIGNED_S].degraded, "nor does the signed lag axis");
        TEST_CHECK(!r.sub[FIRING_SUBSCORE_SETTLE_S].degraded_untrusted, "nor blocks an accept at low n");
        TEST_CHECK(!r.sub[FIRING_SUBSCORE_LAG_SIGNED_S].degraded_untrusted, "nor does the signed lag axis");
        TEST_CHECK(v == FIRING_COMPARE_INSUFFICIENT,
                   "verdict is unchanged by a 10-floor swing on report-only axes alone");
        // And the human composite stays comparable with pre-2026-09-13 ones.
        TEST_CHECK_NEAR(r.composite_normalised, 0.0f, 0.001f,
                        "the composite averages voting axes only");
    }
}

static void test_no_matched_pairs_is_first_class(void)
{
    TEST_SECTION("firing_compare: disjoint classes yield NO_MATCHED_PAIRS, not a verdict");
    firing_score_set_t a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    add_dwell(&a, 4, 2.0f, 1.0f, 0.9f);
    add_dwell(&b, 9, 0.1f, 0.1f, 1.0f); // hugely "better", but a different class
    firing_compare_result_t r;
    TEST_CHECK(firing_compare(&a, &b, NULL, &r) == FIRING_COMPARE_NO_MATCHED_PAIRS,
               "different temperature buckets never compare");
    TEST_CHECK(r.sub[FIRING_SUBSCORE_STEADY_RMS_C].n == 0, "n == 0 reported explicitly");
}

// Two firings of DIFFERENT profiles, different lengths, sharing three
// classes, still compare -- the old module refused this outright.
static void test_different_profiles_still_compare(void)
{
    TEST_SECTION("firing_compare: dissimilar firings compare on their shared classes");
    firing_score_set_t a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    add_dwell(&a, 4, 3.0f, 2.0f, 0.9f);
    add_dwell(&a, 5, 3.0f, 2.0f, 0.9f);
    add_dwell(&a, 6, 3.0f, 2.0f, 0.9f);
    add_dwell(&a, 7, 3.0f, 2.0f, 0.9f); // class only firing A has
    add_dwell(&b, 4, 1.0f, 1.0f, 0.95f);
    add_dwell(&b, 5, 1.0f, 1.0f, 0.95f);
    add_dwell(&b, 6, 1.0f, 1.0f, 0.95f);
    firing_compare_result_t r;
    TEST_CHECK(firing_compare(&a, &b, NULL, &r) == FIRING_COMPARE_ACCEPT, "clear 2C/1C win accepted");
    TEST_CHECK(r.matched_classes == 3, "only the three shared classes contributed");
    TEST_CHECK(r.sub[FIRING_SUBSCORE_STEADY_RMS_C].n == 3, "n == 3");
}

static void test_owner_floor_refuses_small_wins(void)
{
    TEST_SECTION("firing_compare: a sub-0.5C improvement is refused, per the owner's floor");
    // Negative test: lower FIRING_COMPARE_OWNER_FLOOR_C to 0.05f in
    // firing_compare.h -> red.
    firing_score_set_t a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    for (int16_t k = 4; k <= 6; k++) {
        add_dwell(&a, k, 2.0f, 2.0f, 0.9f);
        add_dwell(&b, k, 1.8f, 1.8f, 0.9f); // 0.2 C better -- real, but below the floor
    }
    firing_compare_result_t r;
    TEST_CHECK(firing_compare(&a, &b, NULL, &r) == FIRING_COMPARE_INSUFFICIENT,
               "0.2C improvement is not worth kiln time");
}

// The non-dominance test: buying lag with overshoot must be REJECTED, never
// silently traded. This is the check the composite would have got wrong.
static void test_veto_rejects_a_trade(void)
{
    TEST_SECTION("firing_compare: 1C of lag bought with 1C of overshoot is rejected");
    firing_score_set_t a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    for (int16_t k = 4; k <= 6; k++) {
        add_dwell(&a, k, 1.0f, 3.0f, 0.9f);
        add_dwell(&b, k, 3.0f, 1.0f, 0.9f); // steady 2C better, entry peak 2C worse
    }
    firing_compare_result_t r;
    TEST_CHECK(firing_compare(&a, &b, NULL, &r) == FIRING_COMPARE_REJECT_DEGRADED,
               "a trade is a rejection, not an acceptance");
    TEST_CHECK(r.sub[FIRING_SUBSCORE_ENTRY_PEAK_C].degraded, "the degraded sub-score is named");
    TEST_CHECK(r.sub[FIRING_SUBSCORE_STEADY_RMS_C].bar1_cleared, "the improving one still cleared Bar 1");
}

static void test_in_band_veto(void)
{
    TEST_SECTION("firing_compare: a fall in time-in-band vetoes an otherwise-good trial");
    firing_score_set_t a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    for (int16_t k = 4; k <= 6; k++) {
        add_dwell(&a, k, 2.0f, 2.0f, 0.98f);
        add_dwell(&b, k, 1.0f, 1.0f, 0.70f); // both sub-scores better, but far more time out of band
    }
    firing_compare_result_t r;
    TEST_CHECK(firing_compare(&a, &b, NULL, &r) == FIRING_COMPARE_REJECT_DEGRADED, "in-band veto fires");
    TEST_CHECK(r.in_band_veto, "in_band_veto flagged");
}

static void test_bar2_requires_sign_consistency(void)
{
    TEST_SECTION("firing_compare: with a floor present, Bar 2 needs n>=5 consistently-signed pairs");
    firing_score_set_t a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    // Four pairs only: below Bar 2's n >= 5, so Bar 2 cannot be cleared.
    for (int16_t k = 4; k <= 7; k++) {
        add_dwell(&a, k, 2.0f, 3.0f, 0.9f);
        add_dwell(&b, k, 1.0f, 1.0f, 0.9f);
    }
    firing_compare_floors_t floors;
    memset(&floors, 0, sizeof(floors));
    floors.available = true;
    floors.floor_value[FIRING_SUBSCORE_ENTRY_PEAK_C] = 0.2f;
    floors.floor_value[FIRING_SUBSCORE_STEADY_RMS_C] = 0.2f;
    floors.floor_value[FIRING_SUBSCORE_LAG_S] = 5.0f;

    firing_compare_result_t r;
    TEST_CHECK(firing_compare(&a, &b, &floors, &r) == FIRING_COMPARE_INSUFFICIENT,
               "Bar 1 cleared but Bar 2 short of n>=5 -> refuse");
    TEST_CHECK(r.bar2_applied, "Bar 2 was applied");

    // Same data with a fifth matched class clears it.
    add_dwell(&a, 8, 2.0f, 3.0f, 0.9f);
    add_dwell(&b, 8, 1.0f, 1.0f, 0.9f);
    TEST_CHECK(firing_compare(&a, &b, &floors, &r) == FIRING_COMPARE_ACCEPT, "n==5 and all-signed -> accept");
    TEST_CHECK(!r.bar2_applied || r.sub[FIRING_SUBSCORE_STEADY_RMS_C].bar2_cleared, "Bar 2 cleared");
}


// The exact scenario an opus review constructed on 2026-09-09, and the
// reason FIRING_COMPARE_BAR1_MIN_N exists. ONE matched segment class:
// lag_s improves by 1.2 owner floors at n = 1, overshoot degrades by 5
// owner floors at n = 1, no noise-floor artifact so Bar 2 is skipped.
// Before the fix this returned ACCEPT -- the improvement cleared an
// unguarded Bar 1 while the degradation sat below the veto's own n >= 3 and
// so could not object -- and iter_tune then moved `baseline` permanently
// onto those gains. A single-segment firing pair ratcheted the gains on
// evidence the module's own veto explicitly refuses to trust.
//
// NEGATIVE TEST (production edit, confirmed RED): in firing_compare.c
// change
//     ss->bar1_cleared = (cnt[s] >= FIRING_COMPARE_BAR1_MIN_N) && (...)
// back to
//     ss->bar1_cleared = (ss->median_normalised <= -1.0f);
// AND drop `&& !any_degraded_untrusted` from the ACCEPT arm -> red here.
// Each half alone also goes red, which is the point: the two gates are
// independent.
static void test_no_accept_on_a_single_matched_segment(void)
{
    TEST_SECTION("firing_compare: n==1 lag win + n==1 overshoot loss is NOT an accept");
    const float rate = 60.0f / 3600.0f; // 60 C/hr
    const float lag_floor = FIRING_COMPARE_OWNER_FLOOR_C / rate; // 30 s
    firing_score_set_t a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));

    float va[FIRING_SUBSCORE_COUNT] = {120.0f, 1.0f, 1.0f};
    float vb[FIRING_SUBSCORE_COUNT];
    bool  h[FIRING_SUBSCORE_COUNT] = {true, true, true};
    vb[FIRING_SUBSCORE_LAG_S] = 120.0f - 1.2f * lag_floor;  // 1.2 floors BETTER
    vb[FIRING_SUBSCORE_ENTRY_PEAK_C] = 1.0f + 5.0f * FIRING_COMPARE_OWNER_FLOOR_C; // 5 floors WORSE
    vb[FIRING_SUBSCORE_STEADY_RMS_C] = 1.0f;                 // unchanged
    add_seg(&a, 0, FIRING_SEG_RAMP_UP, 2, 5, rate, va, h, 0.9f);
    add_seg(&b, 0, FIRING_SEG_RAMP_UP, 2, 5, rate, vb, h, 0.9f);

    firing_compare_result_t r;
    firing_compare_verdict_t v = firing_compare(&a, &b, NULL, &r);
    TEST_CHECK(r.matched_classes == 1, "exactly one matched segment class");
    TEST_CHECK(r.sub[FIRING_SUBSCORE_LAG_S].n == 1, "lag has a single pair");
    TEST_CHECK(r.sub[FIRING_SUBSCORE_ENTRY_PEAK_C].n == 1, "overshoot has a single pair");
    TEST_CHECK(!r.bar2_applied, "no floor artifact, so Bar 2 is skipped");
    TEST_CHECK(v != FIRING_COMPARE_ACCEPT, "a single-segment pair must not move the gains");
    TEST_CHECK(v == FIRING_COMPARE_INSUFFICIENT, "and it is INSUFFICIENT, not a REJECT on n==1");
    TEST_CHECK(!r.sub[FIRING_SUBSCORE_LAG_S].bar1_cleared, "Bar 1 needs VETO_MIN_N pairs too");
    TEST_CHECK(r.sub[FIRING_SUBSCORE_ENTRY_PEAK_C].degraded_untrusted,
               "the thin degradation is recorded, not discarded");
    TEST_CHECK(!r.sub[FIRING_SUBSCORE_ENTRY_PEAK_C].degraded, "but it does not arm the veto");
}

// The second, independent half of the same fix: even with Bar 1's n
// satisfied, a full-floor degradation seen on THINNER evidence blocks the
// accept. It cannot REJECT (one odd segment must not veto a good trial) --
// it downgrades to INSUFFICIENT, which the step schedule treats as
// "unmeasured" rather than reversing direction on n < 3.
static void test_low_n_degradation_blocks_but_does_not_reject(void)
{
    TEST_SECTION("firing_compare: a full-floor loss at n<3 blocks an accept without vetoing");
    firing_score_set_t a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    // Three matched dwell classes: steady_rms improves by 2 C on all three
    // (n == 3, Bar 1 satisfied), entry_peak present on only ONE of them and
    // 2 C worse there (n == 1, below the veto's minimum).
    for (int16_t k = 4; k <= 6; k++) {
        float v[FIRING_SUBSCORE_COUNT] = {0.0f, 1.0f, 3.0f};
        bool  h[FIRING_SUBSCORE_COUNT] = {false, (k == 4), true};
        add_seg(&a, 0, FIRING_SEG_DWELL, 0, k, 0.0f, v, h, 0.9f);
        float w[FIRING_SUBSCORE_COUNT] = {0.0f, 3.0f, 1.0f};
        add_seg(&b, 0, FIRING_SEG_DWELL, 0, k, 0.0f, w, h, 0.9f);
    }
    firing_compare_result_t r;
    firing_compare_verdict_t v = firing_compare(&a, &b, NULL, &r);
    TEST_CHECK(r.sub[FIRING_SUBSCORE_STEADY_RMS_C].n == 3, "the winning sub-score has n == 3");
    TEST_CHECK(r.sub[FIRING_SUBSCORE_STEADY_RMS_C].bar1_cleared, "and it clears Bar 1");
    TEST_CHECK(r.sub[FIRING_SUBSCORE_ENTRY_PEAK_C].n == 1, "the losing sub-score has n == 1");
    TEST_CHECK(v == FIRING_COMPARE_INSUFFICIENT, "blocked, but not rejected");
    TEST_CHECK(r.accept_blocked_untrusted, "and the reason is reported");
}

// --------------------------------------------------------------- iter_tune

static iter_tune_gains_t g3(float kp, float ki, float kd)
{
    iter_tune_gains_t g = {kp, ki, kd};
    return g;
}

static firing_compare_result_t verdict(firing_compare_verdict_t v)
{
    firing_compare_result_t r;
    memset(&r, 0, sizeof(r));
    r.verdict = v;
    r.sub[FIRING_SUBSCORE_STEADY_RMS_C].n = 3;
    return r;
}

static void test_default_off_and_anchor_on_first_enable(void)
{
    TEST_SECTION("iter_tune: zeroed state is OFF; the first enable snapshots the anchor");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_CHECK(!st.enabled, "default OFF");
    TEST_CHECK(!iter_tune_propose_perturbation(&st, NULL), "a disabled zone proposes nothing");

    TEST_CHECK(iter_tune_enable(&st, g3(0.04f, 0.0003f, 0.65f)), "enable succeeds");
    TEST_CHECK(st.has_anchor && st.anchor.kp == 0.04f, "anchor snapshotted from the live gains");
    TEST_CHECK(st.baseline.kp == 0.04f, "baseline seeded from the same gains");

    // A second enable must not move the anchor.
    st.baseline.kp = 0.08f;
    st.enabled = false;
    st.status = (uint8_t)ITER_TUNE_STATUS_TUNING;
    TEST_CHECK(iter_tune_enable(&st, g3(0.08f, 0.0003f, 0.65f)), "re-enable succeeds");
    TEST_CHECK(st.anchor.kp == 0.04f, "anchor did NOT move on re-enable");
}

// Negative test: delete the `if (state->has_anchor) { ... }` cage-narrowing
// block in iter_tune.c's iter_tune_clamp_to_cage() -> red here. Confirmed.
static void test_cage_clamps_to_anchor(void)
{
    TEST_SECTION("iter_tune: gains are caged to [0.5x, 2x] of the commissioned anchor");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    iter_tune_enable(&st, g3(1.0f, 0.01f, 0.5f));

    bool hit = false;
    iter_tune_gains_t hi = iter_tune_clamp_to_cage(&st, g3(50.0f, 5.0f, 0.5f), &hit);
    TEST_CHECK(hit, "clamp reported hitting an edge");
    TEST_CHECK(hi.kp == 2.0f, "kp clamped to 2x anchor");
    TEST_CHECK(hi.ki == 0.02f, "ki clamped to 2x anchor");
    iter_tune_gains_t lo = iter_tune_clamp_to_cage(&st, g3(0.0f, 0.0f, 0.5f), &hit);
    TEST_CHECK(lo.kp == 0.5f, "kp clamped to 0.5x anchor");
    TEST_CHECK(lo.ki == 0.005f, "ki clamped to 0.5x anchor");
    TEST_CHECK(lo.kd == 0.5f, "kd passed through untouched");
}

static void test_absolute_ceiling_still_binds(void)
{
    TEST_SECTION("iter_tune: the absolute ceiling binds even inside a permissive cage");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    iter_tune_enable(&st, g3(900.0f, 1.0f, 0.5f)); // 2x anchor would be 1800 > 1000
    bool hit = false;
    iter_tune_gains_t g = iter_tune_clamp_to_cage(&st, g3(1800.0f, 1.0f, 0.5f), &hit);
    TEST_CHECK(g.kp == ITER_TUNE_GAIN_CEIL_C, "clamped to the absolute ceiling, not 2x anchor");
}

static void test_one_parameter_per_trial_and_kd_untouched(void)
{
    TEST_SECTION("iter_tune: one parameter moves per trial; kd never moves");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    iter_tune_enable(&st, g3(1.0f, 0.01f, 0.5f));
    iter_tune_gains_t p;
    TEST_CHECK(iter_tune_propose_perturbation(&st, &p), "first proposal made");
    TEST_CHECK_NEAR(p.kp, 1.0f * (1.0f + ITER_TUNE_STEP_START), 1e-5f, "kp moved by the starting step");
    TEST_CHECK(p.ki == 0.01f, "ki did NOT move in the same trial");
    TEST_CHECK(p.kd == 0.5f, "kd untouched");
    TEST_CHECK(!iter_tune_propose_perturbation(&st, &p), "no second proposal while one is pending");
}

// The exact-revert posture, kept verbatim from the old module.
static void test_revert_is_bit_exact(void)
{
    TEST_SECTION("iter_tune: revert restores the exact float bits, never a recomputation");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    iter_tune_enable(&st, g3(0.0318f, 0.0002f, 0.6526f));
    iter_tune_gains_t before = iter_tune_active_gains(&st);
    iter_tune_propose_perturbation(&st, NULL);
    TEST_CHECK(iter_tune_active_gains(&st).kp != before.kp, "trial gains are live while pending");
    firing_compare_result_t r = verdict(FIRING_COMPARE_INSUFFICIENT);
    TEST_CHECK(iter_tune_process_comparison(&st, &r, NULL, 0) == ITER_TUNE_RESULT_REVERTED, "reverted");
    iter_tune_gains_t after = iter_tune_active_gains(&st);
    TEST_CHECK(memcmp(&before, &after, sizeof(before)) == 0, "byte-identical revert");
}

static void test_accept_moves_baseline(void)
{
    TEST_SECTION("iter_tune: an accepted trial becomes the new baseline");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    iter_tune_enable(&st, g3(1.0f, 0.01f, 0.5f));
    iter_tune_gains_t p;
    iter_tune_propose_perturbation(&st, &p);
    firing_compare_result_t r = verdict(FIRING_COMPARE_ACCEPT);
    TEST_CHECK(iter_tune_process_comparison(&st, &r, NULL, 0) == ITER_TUNE_RESULT_ACCEPTED, "accepted");
    TEST_CHECK(iter_tune_active_gains(&st).kp == p.kp, "baseline moved to the trial gains");
    TEST_CHECK(!st.has_pending, "trial cleared");
}

// Coordinate descent cycles the parameter after EVERY scored trial (plan
// sec 4), and each parameter carries its own step. Negative test: change
// iter_tune.c's post-verdict advance_param(state) call to only run on a
// reject -> red, because the second proposal then moves kp again.
static void test_parameter_cycles_every_trial(void)
{
    TEST_SECTION("iter_tune: the parameter cycles kp -> ki after every scored trial");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    iter_tune_enable(&st, g3(1.0f, 0.01f, 0.5f));
    firing_compare_result_t r = verdict(FIRING_COMPARE_INSUFFICIENT);

    iter_tune_gains_t p1, p2;
    iter_tune_propose_perturbation(&st, &p1);
    TEST_CHECK(p1.kp != 1.0f && p1.ki == 0.01f, "first trial moves kp");
    iter_tune_process_comparison(&st, &r, NULL, 0);
    iter_tune_propose_perturbation(&st, &p2);
    TEST_CHECK(p2.kp == 1.0f && p2.ki != 0.01f, "second trial moves ki, not kp again");
}

// An unmeasurable result means the step was TOO SMALL, so it must GROW.
// Halving it here (the plan's literal schedule) is what made the mechanism
// structurally inert in simulation. Negative test: change iter_tune.c's
// INSUFFICIENT branch back to `state->step_frac[pi] * 0.5f` -> red.
static void test_insufficient_grows_the_step(void)
{
    TEST_SECTION("iter_tune: an unmeasurable trial GROWS the step; a degraded one halves it");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    iter_tune_enable(&st, g3(1.0f, 0.01f, 0.5f));

    firing_compare_result_t insuff = verdict(FIRING_COMPARE_INSUFFICIENT);
    iter_tune_propose_perturbation(&st, NULL);
    iter_tune_process_comparison(&st, &insuff, NULL, 0);
    TEST_CHECK(st.step_frac[ITER_TUNE_PARAM_KP] > ITER_TUNE_STEP_START, "kp step grew after an unmeasurable trial");
    TEST_CHECK(st.step_frac[ITER_TUNE_PARAM_KP] <= ITER_TUNE_STEP_PROBE_MAX, "and stayed under the probe cap");

    firing_compare_result_t bad = verdict(FIRING_COMPARE_REJECT_DEGRADED);
    memset(&st, 0, sizeof(st));
    iter_tune_enable(&st, g3(1.0f, 0.01f, 0.5f));
    for (int i = 0; i < 2; i++) {
        // Two DEGRADED verdicts on kp: one to reverse direction, one to halve.
        st.param = ITER_TUNE_PARAM_KP;
        iter_tune_propose_perturbation(&st, NULL);
        st.param = ITER_TUNE_PARAM_KP;
        iter_tune_process_comparison(&st, &bad, NULL, 0);
    }
    TEST_CHECK_NEAR(st.step_frac[ITER_TUNE_PARAM_KP], ITER_TUNE_STEP_START * 0.5f, 1e-6f,
                    "two degraded trials halve the step");
}

static void test_trial_budget_stops_the_zone(void)
{
    TEST_SECTION("iter_tune: six scored trials converge the zone (owner decision 9.2)");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    iter_tune_enable(&st, g3(1.0f, 0.01f, 0.5f));
    // Alternating accept/reject keeps the step at its 10% start (neither two
    // consecutive accepts nor two consecutive rejects), so this test measures
    // the TRIAL BUDGET and nothing else -- a run of accepts would instead hit
    // the cage edge first, which is a different stopping rule with its own
    // test below.
    firing_compare_result_t acc = verdict(FIRING_COMPARE_ACCEPT);
    firing_compare_result_t rej = verdict(FIRING_COMPARE_INSUFFICIENT);
    int proposals = 0;
    for (int i = 0; i < 20; i++) {
        if (!iter_tune_propose_perturbation(&st, NULL)) break;
        proposals++;
        iter_tune_process_comparison(&st, (i % 2) ? &rej : &acc, NULL, 0);
    }
    TEST_CHECK(proposals == ITER_TUNE_MAX_TRIALS, "exactly the trial budget was spent");
    TEST_CHECK(st.status == ITER_TUNE_STATUS_CONVERGED, "zone converged");
    TEST_CHECK(!st.enabled, "and stopped proposing");
}

// The cage is a stopping rule as well as a clamp: a search that keeps
// pushing at a boundary it may not cross is done. Negative test: raise
// ITER_TUNE_MAX_CAGE_EDGE_HITS to 99 -> red (the loop then runs to the trial
// budget instead).
static void test_cage_edge_stops_the_zone(void)
{
    TEST_SECTION("iter_tune: two cage-edge hits stop the zone before the trial budget");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    iter_tune_enable(&st, g3(1.0f, 0.01f, 0.5f));
    // Retire ki so every trial lands on kp: with the parameter cycling every
    // trial (plan sec 4), a six-trial budget splits three each and neither
    // reaches its cage edge, which would test nothing.
    st.param_done[ITER_TUNE_PARAM_KI] = 1;
    firing_compare_result_t acc = verdict(FIRING_COMPARE_ACCEPT);
    int proposals = 0;
    for (int i = 0; i < 20; i++) {
        if (!iter_tune_propose_perturbation(&st, NULL)) break;
        proposals++;
        iter_tune_process_comparison(&st, &acc, NULL, 0);
    }
    TEST_CHECK(proposals < ITER_TUNE_MAX_TRIALS, "an unbroken run of accepts hits the cage first");
    TEST_CHECK(st.status == ITER_TUNE_STATUS_CONVERGED, "zone converged at the cage edge");
    TEST_CHECK(st.baseline.kp <= 2.0f * st.anchor.kp, "and never left the cage");
}

// Opus review of 249ce287, finding 4: a proposal that clamps to no
// movement used to retire the parameter after trying only ONE direction --
// step_negative[pi] was never flipped first. A baseline sitting at the TOP
// of the cage (a common resting place after a run of accepts) retired kp
// forever the first time an upward step clamped to no movement, even
// though a downward step was still entirely legal and unexplored.
// NEGATIVE TEST: revert the cage_edge_dir_tried flip-before-retire block in
// iter_tune.c's iter_tune_propose_perturbation() back to unconditional
// retirement -> red here (both checks).
static void test_cage_edge_no_movement_tries_other_direction_first(void)
{
    TEST_SECTION("iter_tune: a clamp-to-no-movement at one cage edge tries the OTHER direction "
                 "before retiring the parameter");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    iter_tune_enable(&st, g3(1.0f, 0.01f, 0.5f)); // cage: kp in [0.5, 2.0]
    st.param_done[ITER_TUNE_PARAM_KI] = 1;        // every trial lands on kp
    st.param = ITER_TUNE_PARAM_KP;

    // Baseline sitting exactly at the cage's TOP edge, proposing upward.
    st.baseline.kp = 2.0f;
    st.step_negative[ITER_TUNE_PARAM_KP] = false; // positive direction
    st.step_frac[ITER_TUNE_PARAM_KP] = ITER_TUNE_STEP_START;

    // First proposal: upward from the top edge clamps to no movement.
    // Must NOT retire kp -- must flip direction and report no trial yet.
    iter_tune_gains_t out;
    bool proposed = iter_tune_propose_perturbation(&st, &out);
    TEST_CHECK(!proposed, "the immovable upward step produces no trial this call");
    TEST_CHECK(st.param_done[ITER_TUNE_PARAM_KP] == 0,
               "kp is NOT retired after only the upward direction proved immovable");
    TEST_CHECK(st.step_negative[ITER_TUNE_PARAM_KP] == true,
               "the direction flipped to negative so the next proposal tries downward");
    TEST_CHECK(st.status == ITER_TUNE_STATUS_TUNING, "the zone is still tuning, not converged");

    // Second proposal: downward from the top edge is well inside the cage
    // and must produce a real, moving trial.
    proposed = iter_tune_propose_perturbation(&st, &out);
    TEST_CHECK(proposed, "the downward step, now tried, produces a real trial");
    TEST_CHECK(out.kp < 2.0f, "the trial actually moved kp down from the cage edge");
    TEST_CHECK(st.has_pending, "a trial is now pending");
}

// Both directions genuinely immovable (a degenerate, collapsed cage) must
// still retire the parameter -- this is the "genuinely exhausted" half of
// the same fix, and without it the fix above would spin forever instead of
// converging.
static void test_cage_edge_both_directions_immovable_retires(void)
{
    TEST_SECTION("iter_tune: BOTH directions proven immovable at the cage edge DOES retire the "
                 "parameter");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    iter_tune_enable(&st, g3(1.0f, 0.01f, 0.5f));
    st.param_done[ITER_TUNE_PARAM_KI] = 1;
    st.param = ITER_TUNE_PARAM_KP;

    // A single-point cage: anchor 1.0 with the low/high factors collapsed
    // onto each other is not reachable via enable(), so drive it directly
    // with a baseline at the top edge and a step so large that BOTH
    // directions clamp back to the same edge value (up clamps to the cage
    // ceiling it is already at; down is then forced through the same
    // no-movement check by pre-marking the down direction as already tried,
    // isolating exactly the "both bits set" branch under test).
    st.baseline.kp = 2.0f;
    st.step_negative[ITER_TUNE_PARAM_KP] = false;
    st.step_frac[ITER_TUNE_PARAM_KP] = ITER_TUNE_STEP_START;
    st.cage_edge_dir_tried[ITER_TUNE_PARAM_KP] = 0x2u; // negative direction already proven immovable

    iter_tune_gains_t out;
    bool proposed = iter_tune_propose_perturbation(&st, &out);
    TEST_CHECK(!proposed, "no trial produced");
    TEST_CHECK(st.param_done[ITER_TUNE_PARAM_KP] == 1,
               "kp IS retired once both directions are proven immovable");
}

// An ACCEPT moves the baseline, which invalidates any earlier
// "immovable in this direction" finding for the accepted parameter (it was
// relative to the OLD baseline position) -- confirmed by re-testing the
// direction right after an accept.
static void test_accept_clears_cage_edge_direction_memory(void)
{
    TEST_SECTION("iter_tune: an ACCEPT clears cage_edge_dir_tried for the accepted parameter");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    iter_tune_enable(&st, g3(1.0f, 0.01f, 0.5f));
    st.param = ITER_TUNE_PARAM_KP;
    st.cage_edge_dir_tried[ITER_TUNE_PARAM_KP] = 0x1u; // stale: positive direction "immovable"

    st.has_pending = true;
    st.pending_gains = st.baseline;
    st.pending_gains.kp = 1.1f;
    firing_compare_result_t acc = verdict(FIRING_COMPARE_ACCEPT);
    iter_tune_process_comparison(&st, &acc, NULL, 0);

    TEST_CHECK(st.cage_edge_dir_tried[ITER_TUNE_PARAM_KP] == 0,
               "the stale immovability finding is cleared once the baseline moves");
}

static void test_carry_limit(void)
{
    TEST_SECTION("iter_tune: an unscorable trial carries at most 3 times, then reverts unscored");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    iter_tune_enable(&st, g3(1.0f, 0.01f, 0.5f));
    iter_tune_gains_t before = iter_tune_active_gains(&st);
    iter_tune_propose_perturbation(&st, NULL);
    firing_compare_result_t r = verdict(FIRING_COMPARE_NO_MATCHED_PAIRS);
    for (int i = 0; i < ITER_TUNE_MAX_CARRIES; i++) {
        TEST_CHECK(iter_tune_process_comparison(&st, &r, NULL, 0) == ITER_TUNE_RESULT_CARRIED, "carried");
        TEST_CHECK(st.has_pending, "trial stays armed while carrying");
    }
    TEST_CHECK(iter_tune_process_comparison(&st, &r, NULL, 0) == ITER_TUNE_RESULT_CARRY_EXHAUSTED,
               "the fourth unscorable firing discards it");
    TEST_CHECK(st.trials_scored == 0, "an unscored carry never counted against the budget");
    iter_tune_gains_t after = iter_tune_active_gains(&st);
    TEST_CHECK(memcmp(&before, &after, sizeof(before)) == 0, "gains reverted exactly");
}

static void test_fault_disables_stickily(void)
{
    TEST_SECTION("iter_tune: a fault discards the trial and disables the zone, sticky");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    iter_tune_enable(&st, g3(1.0f, 0.01f, 0.5f));
    iter_tune_gains_t before = iter_tune_active_gains(&st);
    iter_tune_propose_perturbation(&st, NULL);
    iter_tune_fault(&st);
    TEST_CHECK(!st.enabled && st.status == ITER_TUNE_STATUS_FAULTED, "faulted and disabled");
    iter_tune_gains_t after_fault = iter_tune_active_gains(&st);
    TEST_CHECK(memcmp(&before, &after_fault, sizeof(before)) == 0, "gains reverted");
    TEST_CHECK(!iter_tune_enable(&st, g3(1.0f, 0.01f, 0.5f)), "does not retry on a plain re-enable");
}

static void test_restore_commissioned(void)
{
    TEST_SECTION("iter_tune: restore-commissioned puts the anchor back and disables the zone");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    iter_tune_enable(&st, g3(1.0f, 0.01f, 0.5f));
    firing_compare_result_t r = verdict(FIRING_COMPARE_ACCEPT);
    iter_tune_propose_perturbation(&st, NULL);
    iter_tune_process_comparison(&st, &r, NULL, 0);
    TEST_CHECK(st.baseline.kp != 1.0f, "gains have moved");
    iter_tune_gains_t back = iter_tune_restore_commissioned(&st);
    TEST_CHECK(back.kp == 1.0f, "commissioned kp returned");
    TEST_CHECK(!st.enabled, "and the zone is off");
}

static void test_reanchor_moves_the_cage(void)
{
    TEST_SECTION("iter_tune: re-anchor deliberately moves the cage centre and restarts the budget");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    iter_tune_enable(&st, g3(1.0f, 0.01f, 0.5f));
    st.trials_scored = 4;
    iter_tune_reanchor(&st, g3(2.0f, 0.02f, 0.5f));
    TEST_CHECK(st.anchor.kp == 2.0f, "anchor moved");
    TEST_CHECK(st.trials_scored == 0, "budget restarted");
    bool hit = false;
    TEST_CHECK(iter_tune_clamp_to_cage(&st, g3(10.0f, 1.0f, 0.5f), &hit).kp == 4.0f, "new cage in force");
}


// A CONVERGED zone must have a real way back that does not throw the
// accepted gains away. Before 2026-09-09 it did not: iter_tune_enable()
// refuses while status is CONVERGED, and iter_tune_reanchor() only wrote
// TUNING when the zone was still `enabled` -- which a CONVERGED zone never
// is, because stop() clears the flag. The documented escape hatch ("moving
// it takes a deliberate re-anchor") was a permanent no-op, leaving
// iter_tune_restore_commissioned() -- which discards every accepted gain --
// as the only exit.
//
// NEGATIVE TEST (production edit, confirmed RED): in iter_tune.c's
// iter_tune_reanchor(), change the status line back to
//     if (state->enabled) state->status = (uint8_t)ITER_TUNE_STATUS_TUNING;
// -> "re-enable now succeeds" goes red. Separately, change the baseline
// branch back to `state->baseline = new_anchor;` -> "accepted gains
// survive" goes red.
static void test_reanchor_reopens_a_converged_zone(void)
{
    TEST_SECTION("iter_tune: re-anchor is a REAL escape from CONVERGED and keeps accepted gains");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_CHECK(iter_tune_enable(&st, g3(1.0f, 0.01f, 0.5f)), "enabled");

    // Spend the budget on accepts, so the zone converges holding gains that
    // are NOT the anchor.
    firing_compare_result_t acc = verdict(FIRING_COMPARE_ACCEPT);
    for (int i = 0; i < ITER_TUNE_MAX_TRIALS; i++) {
        if (!iter_tune_propose_perturbation(&st, NULL)) break;
        iter_tune_process_comparison(&st, &acc, NULL, 0);
    }
    TEST_CHECK(st.status == ITER_TUNE_STATUS_CONVERGED, "zone converged");
    TEST_CHECK(!st.enabled, "and is disabled, which is what made re-enable impossible");
    iter_tune_gains_t accepted = st.baseline;
    TEST_CHECK(accepted.kp != 1.0f || accepted.ki != 0.01f, "gains actually moved");
    TEST_CHECK(!iter_tune_enable(&st, g3(1.0f, 0.01f, 0.5f)), "a plain re-enable is still refused");

    // The deliberate re-anchor, onto the gains the search actually reached.
    TEST_CHECK(iter_tune_reanchor(&st, accepted), "re-anchor accepted");
    TEST_CHECK(st.status == ITER_TUNE_STATUS_OFF, "the sticky CONVERGED is cleared");
    TEST_CHECK(st.stop_reason == ITER_TUNE_STOP_NONE, "and so is the stop reason");
    TEST_CHECK(iter_tune_enable(&st, g3(9.0f, 9.0f, 9.0f)), "re-enable now succeeds");
    TEST_CHECK(st.status == ITER_TUNE_STATUS_TUNING, "and the zone is tuning again");
    TEST_CHECK(st.trials_scored == 0, "with a fresh budget");
    TEST_CHECK(memcmp(&st.baseline, &accepted, sizeof(accepted)) == 0,
               "accepted gains survive -- re-anchor is not a revert");
    TEST_CHECK(memcmp(&st.anchor, &accepted, sizeof(accepted)) == 0,
               "and the enable did NOT re-snapshot the anchor from current_gains");
}

// A re-anchor that does not move the gains must not silently discard them
// either, and one that shrinks the cage around them clamps rather than
// resets.
static void test_reanchor_keeps_gains_and_clamps_them(void)
{
    TEST_SECTION("iter_tune: re-anchor keeps the accepted baseline, clamped into the new cage");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    iter_tune_enable(&st, g3(1.0f, 0.01f, 0.5f));
    st.baseline = g3(1.2f, 0.012f, 0.5f);
    // New anchor at 4.0 puts the cage at [2.0, 8.0]; the 1.2 baseline is
    // below it and must be clamped up, not thrown away for the anchor value.
    TEST_CHECK(iter_tune_reanchor(&st, g3(4.0f, 0.012f, 0.5f)), "re-anchor accepted");
    TEST_CHECK(st.baseline.kp == 2.0f, "baseline clamped to the new cage floor, not reset to 4.0");
    TEST_CHECK(st.baseline.ki == 0.012f, "an in-cage gain is untouched");
    TEST_CHECK(st.baseline.kd == 0.5f, "kd is never touched");
}

// FAULTED stays sticky: plan sec 5.5's "it does not retry". Re-anchor is
// the escape from CONVERGED, not from a guard trip.
static void test_reanchor_refuses_a_faulted_zone(void)
{
    TEST_SECTION("iter_tune: re-anchor refuses a FAULTED zone and changes nothing");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    iter_tune_enable(&st, g3(1.0f, 0.01f, 0.5f));
    iter_tune_fault(&st);
    iter_tune_zone_state_t before = st;
    TEST_CHECK(!iter_tune_reanchor(&st, g3(2.0f, 0.02f, 0.5f)), "refused");
    TEST_CHECK(memcmp(&before, &st, sizeof(st)) == 0, "state untouched");
    TEST_CHECK(st.status == ITER_TUNE_STATUS_FAULTED, "still faulted");
    TEST_CHECK(st.stop_reason == ITER_TUNE_STOP_FAULT, "with the fault reason recorded");
}

// A zero-valued gain has no multiplicative step and its cage collapses to
// {0}. Before 2026-09-09 this stalled the whole search silently and
// permanently: the proposal collapsed onto the baseline, the "no movement"
// guard returned false without advancing the parameter or setting
// param_done, and status stayed TUNING forever. A zero kp additionally
// blocked ki, since `param` starts at KP and only advances from
// process_comparison().
//
// NEGATIVE TEST (production edit, confirmed RED): in iter_tune.c, delete
// the `param_is_perturbable()` retirement loop at the top of
// iter_tune_propose_perturbation() -> both checks below go red (status
// stays TUNING, and the ki probe never happens).
static void test_zero_gain_does_not_stall_the_search(void)
{
    TEST_SECTION("iter_tune: a zero gain is retired, never left spinning with status TUNING");

    // (a) kp == 0 must not block ki, which is perfectly tunable.
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    iter_tune_enable(&st, g3(0.0f, 0.01f, 0.5f));
    iter_tune_gains_t out;
    TEST_CHECK(iter_tune_propose_perturbation(&st, &out), "a proposal is still made");
    TEST_CHECK(st.param_done[ITER_TUNE_PARAM_KP], "kp retired as un-perturbable");
    TEST_CHECK(st.param == ITER_TUNE_PARAM_KI, "and ki is the parameter under test");
    TEST_CHECK(out.ki != 0.01f && out.kp == 0.0f, "ki moved, kp did not");
    TEST_CHECK(st.status == ITER_TUNE_STATUS_TUNING, "zone still tuning");

    // (b) both zero: terminate with a definite status AND a reason, rather
    // than returning false forever.
    iter_tune_zone_state_t z;
    memset(&z, 0, sizeof(z));
    iter_tune_enable(&z, g3(0.0f, 0.0f, 0.5f));
    TEST_CHECK(!iter_tune_propose_perturbation(&z, NULL), "nothing can be proposed");
    TEST_CHECK(z.status == ITER_TUNE_STATUS_CONVERGED, "and the zone STOPS -- not TUNING forever");
    TEST_CHECK(z.stop_reason == ITER_TUNE_STOP_UNPERTURBABLE, "with the un-perturbable reason");
    TEST_CHECK(iter_tune_stop_reason_str((iter_tune_stop_reason_t)z.stop_reason)[0] != '\0',
               "and a non-empty reason string");
    TEST_CHECK(!iter_tune_propose_perturbation(&z, NULL), "and it stays stopped");
}

// Every stopping path must name itself. A CONVERGED zone with
// stop_reason == NONE would be exactly the silent termination this pass
// exists to remove.
static void test_every_stop_names_a_reason(void)
{
    TEST_SECTION("iter_tune: every stopping path records a stop reason");
    iter_tune_zone_state_t st;
    memset(&st, 0, sizeof(st));
    iter_tune_enable(&st, g3(1.0f, 0.01f, 0.5f));
    firing_compare_result_t acc = verdict(FIRING_COMPARE_ACCEPT);
    for (int i = 0; i < ITER_TUNE_MAX_TRIALS; i++) {
        if (!iter_tune_propose_perturbation(&st, NULL)) break;
        iter_tune_process_comparison(&st, &acc, NULL, 0);
    }
    TEST_CHECK(st.status == ITER_TUNE_STATUS_CONVERGED, "converged");
    TEST_CHECK(st.stop_reason != ITER_TUNE_STOP_NONE, "a reason was recorded");
}

void run_test_iter_tune(void)
{
    test_gain_ceil_mirrors_zone_pid_gain_max();
    test_classify_rate();
    test_lag_is_rate_normalised();
    test_start_temperature_does_not_change_the_score();
    test_saturated_ticks_are_excluded();
    test_short_segment_dropped();
    test_settle_time_distinguishes_ringing_from_quick_settle();
    test_settle_time_never_settling_reports_nothing_not_a_sentinel();
    test_settle_merge_requires_both_dwells_to_have_settled();
    test_subscore_vote_enrolment_is_explicit();
    test_report_only_subscores_cannot_change_a_verdict();
    test_undershoot_measured_and_not_cancelled_by_recovery();
    test_undershoot_makes_a_worse_trial_score_worse();
    test_lag_signed_distinguishes_lead_from_lag();
    test_lag_signed_includes_saturated_short_ticks();
    test_no_matched_pairs_is_first_class();
    test_different_profiles_still_compare();
    test_owner_floor_refuses_small_wins();
    test_veto_rejects_a_trade();
    test_in_band_veto();
    test_bar2_requires_sign_consistency();
    test_no_accept_on_a_single_matched_segment();
    test_low_n_degradation_blocks_but_does_not_reject();
    test_default_off_and_anchor_on_first_enable();
    test_cage_clamps_to_anchor();
    test_absolute_ceiling_still_binds();
    test_one_parameter_per_trial_and_kd_untouched();
    test_revert_is_bit_exact();
    test_accept_moves_baseline();
    test_parameter_cycles_every_trial();
    test_insufficient_grows_the_step();
    test_trial_budget_stops_the_zone();
    test_cage_edge_stops_the_zone();
    test_cage_edge_no_movement_tries_other_direction_first();
    test_cage_edge_both_directions_immovable_retires();
    test_accept_clears_cage_edge_direction_memory();
    test_carry_limit();
    test_fault_disables_stickily();
    test_restore_commissioned();
    test_reanchor_moves_the_cage();
    test_reanchor_reopens_a_converged_zone();
    test_reanchor_keeps_gains_and_clamps_them();
    test_reanchor_refuses_a_faulted_zone();
    test_zero_gain_does_not_stall_the_search();
    test_every_stop_names_a_reason();
}
