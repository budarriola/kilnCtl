#pragma once
// firing_compare.h -- ITER_TUNE_REDESIGN_PLAN.md sec 2.3 + sec 3, step 2.
//
// Matched-pair comparator and the accept/reject rule. Takes two
// firing_score_set_t (a BASELINE firing and a TRIAL firing, which may be
// entirely different profiles started from entirely different temperatures)
// and answers one question: did the trial track better, worse, or
// indistinguishably?
//
// HOW TWO DISSIMILAR FIRINGS BECOME COMPARABLE (plan sec 2.3):
//   1. Intersect the two sets on class key. Only classes present in BOTH
//      contribute -- nothing is extrapolated across classes.
//   2. Per sub-score, take the paired differences d_i = trial_i - baseline_i
//      (negative == the trial is better).
//   3. Aggregate by the MEDIAN of those paired differences, with n (the
//      number of matched pairs) reported alongside. n == 0 is a FIRST-CLASS
//      OUTCOME, not an error: it means this firing pair simply has nothing
//      to say about that sub-score.
//
// NORMALISATION. lag_s is in seconds and its Bar-1 floor (0.5 degC of
// tracking error) is a different number of seconds for every commanded
// rate. Rather than compare a median-of-seconds against one floor that only
// suits one rate, every paired difference is divided by ITS OWN class's
// Bar-1 floor before aggregation, so a "normalised" difference of -1.0
// means "improved by exactly one owner floor" regardless of class. The raw
// medians are reported too, for humans.
//
// THE RULE (plan sec 3):
//   Bar 1 (the owner's 0.5 degC floor, available from day one): at least one
//     sub-score's median normalised improvement must be <= -1.0.
//   Bar 2 (the statistical floor, SKIPPED until a floor artifact exists):
//     the median must also clear that key's measured floor, AND the paired
//     differences must be consistently signed -- ceil(0.75*n) of n >= 5
//     differences must be improvements.
//   Bar 1 also requires n >= FIRING_COMPARE_BAR1_MIN_N matched pairs. See
//     that macro for why an accept may not run on thinner evidence than a
//     veto.
//   No-degradation veto: REJECT if any sub-score with n >= 3 degrades by
//     more than its own Bar-1 floor, or if time-in-band degrades outside
//     tolerance. A full-floor degradation at n < 3 is too thin to REJECT on
//     but is still enough to block an ACCEPT (verdict INSUFFICIENT). This makes the rule a non-dominance test: a trial that buys
//     1 degC of lag by adding 1 degC of overshoot is rejected, never
//     silently traded.
//
// PURE. No ESP-IDF, no NVS, no lock, no FreeRTOS, no allocation.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "firing_score.h"

#ifdef __cplusplus
extern "C" {
#endif

// The owner's floor, promoted from advice into a hard accept-rule term:
// differences below 0.5 degC are explicitly not worth kiln time.
#define FIRING_COMPARE_OWNER_FLOOR_C 0.5f

// Bar 2's minimum sample size and sign-consistency fraction (plan sec 3).
#define FIRING_COMPARE_BAR2_MIN_N 5
#define FIRING_COMPARE_BAR2_SIGN_FRACTION 0.75f

// Veto arms only with this many matched pairs -- below it a single odd
// segment could veto an otherwise good trial.
#define FIRING_COMPARE_VETO_MIN_N 3

// Bar 1 arms only with this many matched pairs. IT MUST NEVER BE SMALLER
// THAN FIRING_COMPARE_VETO_MIN_N, and the same-n choice is deliberate.
//
// The asymmetry this fixes (found in review, 2026-09-09): bar1_cleared was
// unguarded by n while the no-degradation veto required n >= 3. A single
// matched segment class could therefore produce an ACCEPT -- lag improving
// by one floor at n = 1 clears Bar 1, overshoot degrading by five floors at
// n = 1 is BELOW the veto's own minimum and so cannot object, floors
// unavailable so Bar 2 is skipped -- and iter_tune then moves `baseline`
// permanently onto those gains. The module was accepting on exactly the
// evidence its own veto refuses to trust.
//
// WHY 3 AND NOT MORE. An accept is permanent and a veto is not, so the
// accept side should if anything carry the heavier burden. It does, but
// through two additional terms rather than through a bigger n:
//   - an accept must ALSO survive every other sub-score, at ANY n, being
//     free of a full-floor degradation (the `untrusted degradation` gate
//     below) -- a strictly stronger condition than the veto's own n >= 3;
//   - an accept must clear a FULL owner floor of median improvement, while
//     nothing is required to merely refuse.
// Raising this to Bar 2's 5 instead would collide with reality: a firing
// commonly yields only 3-6 matched classes and the whole per-zone budget is
// six scored trials, so a 5-pair Bar 1 with no noise-floor artifact present
// would make the mechanism structurally inert -- the "two mutually
// exclusive conditions" failure this repo has already recorded twice
// (dwell credit; the plan's own step schedule). 3 is the smallest value
// that restores the accept-vs-veto ordering without reintroducing that.
#define FIRING_COMPARE_BAR1_MIN_N FIRING_COMPARE_VETO_MIN_N

// How much time-in-band may fall before the diagnostic veto fires. Fraction
// of ticks, i.e. 0.05 == five percentage points.
#define FIRING_COMPARE_IN_BAND_TOLERANCE 0.05f

#define FIRING_COMPARE_MAX_PAIRS FIRING_SCORE_MAX_ENTRIES

typedef enum {
    FIRING_COMPARE_ACCEPT = 0,
    FIRING_COMPARE_REJECT_DEGRADED = 1,     // the no-degradation veto fired
    FIRING_COMPARE_INSUFFICIENT = 2,        // nothing cleared Bar 1 (or Bar 2) -- refuse to act
    FIRING_COMPARE_NO_MATCHED_PAIRS = 3,    // n == 0 everywhere; this pair says nothing at all
} firing_compare_verdict_t;

// Optional measured noise floor (plan sec 3.1). `available == false` (the
// zero value) means Bar 2 is skipped entirely and Bar 1 plus the veto are
// the whole rule -- which is the designed default, and the conservative
// direction.
typedef struct {
    bool  available;
    float floor_value[FIRING_SUBSCORE_COUNT]; // same units as the sub-score
} firing_compare_floors_t;

typedef struct {
    uint16_t n;                  // matched pairs contributing to this sub-score
    uint16_t improved;           // how many of those n were improvements (d < 0)
    float median_raw;            // median paired difference, sub-score's own units
    float median_normalised;     // median of d_i / bar1_floor_i; -1.0 == one owner floor better
    bool  bar1_cleared;
    bool  bar2_cleared;
    bool  degraded;              // median_normalised >= +1.0 with n >= VETO_MIN_N
    bool  degraded_untrusted;    // median_normalised >= +1.0 but n < VETO_MIN_N: too
                                 // thin to VETO on, still enough to refuse an ACCEPT
} firing_compare_subscore_t;

typedef struct {
    firing_compare_verdict_t verdict;
    firing_compare_subscore_t sub[FIRING_SUBSCORE_COUNT];
    uint16_t matched_classes;
    uint16_t in_band_n;
    float in_band_median_delta;  // trial - baseline, fraction of ticks; negative == worse
    bool  in_band_veto;
    bool  in_band_degraded_untrusted; // in-band fell outside tolerance at n < VETO_MIN_N
    bool  accept_blocked_untrusted;   // an ACCEPT was downgraded to INSUFFICIENT purely
                                      // by a low-n degradation somewhere
    bool  bar2_applied;
    // Human-facing composite ONLY (plan sec 2.2: "a composite IS still
    // computed and displayed for humans; it never decides anything").
    float composite_normalised;
} firing_compare_result_t;

// Compares `trial` against `baseline`. `floors` may be NULL (== Bar 2
// skipped). Returns the verdict, also written into *out.
firing_compare_verdict_t firing_compare(const firing_score_set_t *baseline, const firing_score_set_t *trial,
                                        const firing_compare_floors_t *floors, firing_compare_result_t *out);

// Bar-1 floor for one sub-score of one class, in that sub-score's units:
// 0.5 degC for the two degC sub-scores, and 0.5/rate seconds for lag.
// Exposed for host tests and for reporting.
float firing_compare_bar1_floor(firing_subscore_t sub, float rate_c_per_s);

#ifdef __cplusplus
}
#endif
