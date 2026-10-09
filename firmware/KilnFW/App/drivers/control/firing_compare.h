#pragma once
// firing_compare.h -- ITER_TUNE_REDESIGN.md sec 2.3 + sec 3, step 2.
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

// ---------------------------------------------------------------------------
// WHICH SUB-SCORES VOTE (2026-09-14,
// docs/audits/firing_score_subscore_enrolment_2026-09-14.md)
//
// Until this change both loops in firing_compare.c ran
// `for (s = 0; s < FIRING_SUBSCORE_COUNT; s++)`, so the SIZE OF AN ENUM was
// the decision rule: d41da85f raised FIRING_SUBSCORE_COUNT from 3 to 6 and
// thereby enrolled three brand-new, unvalidated axes into Bar 1 (any one can
// authorise a permanent gain change alone), the no-degradation veto, the
// low-n degraded_untrusted gate and the human composite -- with no edit to
// this file and no line in the commit message saying so. Five of 660 A1 null
// comparisons flipped and A2's improvement count fell 6 -> 3 as a result.
//
// Enrolment is now an explicit, visible act: a sub-score votes if and only
// if its bit is in FIRING_COMPARE_VOTING_MASK. Everything else is MEASURED
// and REPORTED (n, medians, improved counts are all still computed for every
// axis) but cannot move a verdict.
//
// Adding a sub-score without classifying it is a COMPILE ERROR, not a silent
// enrolment: FIRING_COMPARE_CLASSIFIED_MASK must cover every enum value.
//
//   voting      -- validated axes with a magnitude definition this plant can
//                  support: the three the pinned A1 bar was set against, plus
//                  ENTRY_UNDERSHOOT_C (a correctly-signed magnitude closing a
//                  measured accept-permissive gap, and measured never to fire
//                  in the A1 null experiment).
//   report-only -- SETTLE_S and LAG_SIGNED_S. SETTLE_S's band has just been
//                  re-sized from the plant and its never-settles case
//                  re-encoded; it votes again only once a matched-pair
//                  measurement on real captures says it carries signal.
//                  LAG_SIGNED_S is SIGNED, which no "lower is better"
//                  comparator can adjudicate at all (see the static assert).
#define FIRING_COMPARE_VOTING_MASK                                           \
    ((1u << FIRING_SUBSCORE_LAG_S) | (1u << FIRING_SUBSCORE_ENTRY_PEAK_C) |  \
     (1u << FIRING_SUBSCORE_STEADY_RMS_C) |                                  \
     (1u << FIRING_SUBSCORE_ENTRY_UNDERSHOOT_C))

#define FIRING_COMPARE_REPORT_ONLY_MASK                                      \
    ((1u << FIRING_SUBSCORE_SETTLE_S) | (1u << FIRING_SUBSCORE_LAG_SIGNED_S))

#define FIRING_COMPARE_CLASSIFIED_MASK \
    (FIRING_COMPARE_VOTING_MASK | FIRING_COMPARE_REPORT_ONLY_MASK)

#define FIRING_COMPARE_ALL_SUBSCORES_MASK ((1u << FIRING_SUBSCORE_COUNT) - 1u)

// A new sub-score must be listed as voting OR report-only. If you just added
// one to firing_subscore_t and landed here: decide, deliberately, whether it
// may authorise a permanent gain change, and say so in the audit trail.
_Static_assert(FIRING_COMPARE_CLASSIFIED_MASK == FIRING_COMPARE_ALL_SUBSCORES_MASK,
               "every firing_subscore_t must be classified voting or report-only");
// No axis may be in both.
_Static_assert((FIRING_COMPARE_VOTING_MASK & FIRING_COMPARE_REPORT_ONLY_MASK) == 0u,
               "a sub-score cannot be both voting and report-only");
// A signed quantity cannot be adjudicated by an all-"lower is better"
// comparator: "further ahead of schedule" would read as unbounded
// improvement. See FIRING_SUBSCORE_SIGNED_MASK in firing_score.h.
_Static_assert((FIRING_COMPARE_VOTING_MASK & FIRING_SUBSCORE_SIGNED_MASK) == 0u,
               "a signed sub-score must never vote in a lower-is-better comparator");

// True if this sub-score participates in Bar 1 / Bar 2 / the veto / the
// composite. Exposed so tests and reporting can pin the voting set by name.
bool firing_compare_subscore_votes(firing_subscore_t sub);
// ---------------------------------------------------------------------------

// Bar 2's minimum sample size and sign-consistency fraction (plan sec 3).
#define FIRING_COMPARE_BAR2_MIN_N 5
#define FIRING_COMPARE_BAR2_SIGN_FRACTION 0.75f

// Bar-1 floor for FIRING_SUBSCORE_SETTLE_S, in seconds. The owner has not set
// a materiality figure for a TIME sub-score (docs/audits/firing_score_four_
// objective_scorecard_2026-09-13.md finding A), so 0.5 degC's own floor
// cannot be reused directly -- 0.5 (as "0.5 seconds") would clear on any real
// difference at all and make Bar 1 trivial for this axis. One PWM window (the
// same 60 s already used elsewhere in this module family as the shortest
// interval a dwell-entry statistic can mean anything over,
// FIRING_SCORE_MIN_SCORED_TICKS at the 1 Hz executor tick) is used instead:
// a settle-time difference smaller than one window is window-phase noise,
// not a measured improvement.
#define FIRING_COMPARE_SETTLE_FLOOR_S 60.0f

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
    // A malloc() failure inside firing_compare() itself, before any pair was
    // even examined -- distinct from NO_MATCHED_PAIRS, which means "this pair
    // genuinely has nothing to say" and is a legitimate, expected outcome.
    // This one means "the comparison never actually ran". Every caller must
    // treat it AT LEAST as conservatively as NO_MATCHED_PAIRS: never scored,
    // never applied, never counted as evidence either way (2026-09-24 review
    // advisory -- a shadow-mode counter that folded this into
    // no_matched_pairs_count could not tell a genuine no-match streak from a
    // low-memory streak).
    FIRING_COMPARE_ALLOC_FAILED = 4,
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
    bool  votes;                 // did this axis participate in the verdict at all?
                                 // false == measured and reported only (see
                                 // FIRING_COMPARE_VOTING_MASK).
    bool  bar1_cleared;
    bool  bar2_cleared;
    bool  degraded;              // median_normalised >= +1.0 with n >= VETO_MIN_N
    bool  degraded_untrusted;    // median_normalised >= +1.0 but n < VETO_MIN_N: too
                                 // thin to VETO on, still enough to refuse an ACCEPT.
                                 // Consumed only inside firing_compare.c's own verdict
                                 // logic today -- no caller outside this file (iter_tune.c
                                 // included) reads this field yet; it is exposed on the
                                 // result struct for a future caller that wants the WHY,
                                 // not because one exists now.
} firing_compare_subscore_t;

typedef struct {
    firing_compare_verdict_t verdict;
    firing_compare_subscore_t sub[FIRING_SUBSCORE_COUNT];
    uint16_t matched_classes;
    uint16_t in_band_n;
    float in_band_median_delta;  // trial - baseline, fraction of ticks; negative == worse
    bool  in_band_veto;
    bool  in_band_degraded_untrusted; // in-band fell outside tolerance at n < VETO_MIN_N.
                                      // Same "no consumer yet" note as sub[]'s
                                      // degraded_untrusted above: read only inside this
                                      // file's own verdict computation today.
    bool  accept_blocked_untrusted;   // an ACCEPT was downgraded to INSUFFICIENT purely
                                      // by a low-n degradation somewhere. Also has no
                                      // consumer outside this file yet -- exposed for a
                                      // future caller, not read by one today.
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
