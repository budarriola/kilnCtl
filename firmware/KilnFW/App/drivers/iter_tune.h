#pragma once
// iter_tune.h -- PID_EXPANSION_PLAN.md 3.3, "Iterative tuning": the layer
// that scores each firing with its own already-recorded normalized IAE
// (profile_executor_firing_stats.c's firing_stats_snapshot(), read there,
// never recomputed here), perturbs a zone's gains slightly, and keeps the
// change only if the NEXT comparable firing scores better.
//
// WHY THIS LAYER MATTERS MORE THAN THE IDENTIFICATION LAYERS (see the
// plan's 3.3 entry on "Dynamics from ramps", SHELVED): every identification
// fit in this codebase needs an informative signal -- a held step, an
// excited ramp -- and every one of those has turned out to be fakeable by
// something that isn't plant dynamics (the ramp fit reduced to a function
// of the commanded rate alone). This layer needs none of that: it only
// asks "did the whole firing track better than last time," which is true
// or false regardless of what shape the profile commanded. It is much
// harder to fool BUT it is trivially fooled by noise if the accept
// threshold isn't respected -- see the noise-floor section below, which is
// the load-bearing part of this file.
//
// THIS MODULE IS PURE DECISION LOGIC. No ESP-IDF, no NVS, no lock, no
// FreeRTOS -- same posture as max31856_codec.c/panel_codec.c. The caller
// (integration point documented at the bottom of this file) owns:
//   - persisting iter_tune_zone_state_t (its own NVS namespace, own opt-in
//     flag storage -- deliberately NOT zones_http.c/adaptive_tune's own
//     "adap_tune" namespace; those are owned by other in-flight work this
//     session, see PID_EXPANSION_PLAN.md 3.3's other two open bullets)
//   - actually writing gains into the zone's live PID config before a run
//     and reading profile_exec_firing_stats_t back out after one
//   - calling iter_tune_process_firing() exactly once per completed run,
//     at the same run-boundary-only call site profile_executor_firing_
//     stats.c already finalizes a profile_firing_run_record_t (see
//     firing_stats_maybe_finalize() in that file) -- NEVER mid-run, or a
//     perturbation could be judged against telemetry it didn't produce.
//
// This file does NOT touch adaptive_tune.c/.h, adaptive_tune_ki.c,
// adaptive_tune_model.c, adaptive_tune_internal.h, zones_http.c, or any
// display file, and is not wired into profile_executor.c by this change --
// wiring it in is the caller's job, at the integration point documented at
// the end of this header.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------
// Bounds -- mirror the limits the other tuning layers already respect.
//
// ITER_TUNE_GAIN_CEIL_C mirrors ZONE_PID_GAIN_MAX (zones_http.h, 1000.0f)
// by VALUE, not by #include -- this file must stay free of zones_http.h's
// dependency chain (that header is owned by other in-flight work this
// session). test_iter_tune.c pins both values with an explicit comment so
// the day one changes without the other, the test goes red, not silent.
#define ITER_TUNE_GAIN_FLOOR_C 0.0f
#define ITER_TUNE_GAIN_CEIL_C 1000.0f

// Bounded, revertible nudge per trial -- same posture as adaptive_tune's
// ADAPTIVE_TUNE_MAX_FRACTIONAL_MOVE / ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE
// (both 20%, adaptive_tune_internal.h), but iterative tuning has no fitted
// model to bound the move against -- only the immediately prior accepted
// gains -- so a materially smaller step is used: 20% of a gain, applied
// blind (no plant model informing direction or size), would be a much
// larger single-firing swing than a blended, model-checked 20% move.
#define ITER_TUNE_PERTURB_FRACTION 0.05f

// ---------------------------------------------------------------------
// The noise floor. THIS IS THE PART THAT MAKES THE MECHANISM HONEST.
//
// Two firings of the same profile, same zones, same gains do not produce
// identical normalized IAE -- ordinary sensor quantization (0.1 degC),
// ambient drift, and the executor's own tick jitter all move it. Accepting
// ANY improvement, however small, ratchets on that noise and calls it
// progress; §3.3's own adaptive-tune-Ki work independently hit exactly
// this failure mode (a vacuous decreasing-direction test) before it was
// closed.
//
// THIS REPO HAS NO UNCONTAMINATED SAME-GAIN REPEAT-FIRING DATASET, and an
// earlier version of this comment claimed one anyway. It compared
// holdfix_clean.jsonl and final.jsonl (both climb_mode=coupled/
// integral_floor=ff_hold, i.e. the same shipped build) and read their
// whole-run normalized-IAE spread (+22.5% / +47.5% / +28.6% across the
// three zones) as if it were a measurement of run-to-run noise. It is not:
// the two captures' own first poll rows show a **4.8 degC warmer start**
// in final.jsonl (z0/z1/z2: 24.57/24.62/24.71 degC at 10:16:38 vs
// 29.32/29.50/29.57 degC at 14:14:29, same afternoon). That is exactly
// this repo's own documented failure mode --
// project_autotune_needs_rested_baseline, "residual heat biases results" --
// applied to THIS module's own evidence. An unknown, likely large share of
// that spread is segment 0 needing less heating from a warmer start, not
// noise. Presenting a confounded number as a measured noise floor was the
// mistake; it is corrected here rather than quietly reused.
//
// So: the true noise floor -- the spread between two firings of the same
// profile, same gains, both from a genuinely rested start -- is UNKNOWN.
// It has not been measured in this repo. ITER_TUNE_MIN_RELATIVE_IMPROVEMENT
// below is a DELIBERATELY CONSERVATIVE CHOICE pending real data, not a
// number derived from these two runs -- 20%, matching the fractional-move
// convention every other bounded layer in this file's neighborhood already
// uses (ADAPTIVE_TUNE_MAX_FRACTIONAL_MOVE / ADAPTIVE_TUNE_KI_MAX_
// FRACTIONAL_MOVE, adaptive_tune_internal.h), chosen because it is the
// established convention here, not because two contaminated data points
// support it. It could be too loose or too tight; nobody has the
// experiment that would tell.
//
// WHAT WOULD ACTUALLY ESTABLISH THE NOISE FLOOR: N repeat firings (N >= 5
// suggested) of the SAME profile, with GAINS HELD FIXED across all of
// them, each one starting from a genuinely rested baseline (every zone at
// ambient, not just the one nominally under test -- see this repo's own
// "Autotune needs a rested baseline" lesson) and separated by enough time
// to fully cool between firings. The spread of iae_normalized across that
// set, per zone, is the real noise floor this file should be comparing
// against. That is a hardware experiment for someone to run later; this
// file cannot manufacture it from captures that were never designed to
// hold gains and starting temperature fixed.
//
// HONEST CONSEQUENCE of shipping a conservative guess instead of a
// measured floor: this mechanism may accept real noise as an improvement,
// or revert a real improvement that measured smaller than actual noise on
// one firing -- in either direction, unquantified until the experiment
// above is run. Neither failure compounds, because every subsequent firing
// is scored against whatever is currently accepted, not against history --
// a lucky accept gets re-tested the very next firing under the same bar.
//
// ---------------------------------------------------------------------
// UPDATE 2026-09: the experiment above has now been run --
// tools/PcTools/config_presets/noise_floor.json, schema 2, six repeat
// firings of the same profile/gains (generated_from lists all six; the
// file's own start_conditions block flags them as NOT strictly like-for-
// like -- start temps span 1.29 degC against a 1.0 degC threshold -- so
// these numbers are a slight overestimate of true noise, i.e. still on the
// conservative side, not an underestimate). The metric this file scores on
// is iae_normalized_whole_c; per zone (mean / std_c / range-of-6 = "noise_
// floor_c" in that artifact):
//   z0: mean 1.6043, std_c 0.05267, range 0.11565
//   z1: mean 1.1904, std_c 0.03181, range 0.07709
//   z2: mean 0.8765, std_c 0.05408, range 0.14730
//
// THE ARITHMETIC (do not reuse the "range" column directly as a threshold
// -- a max-min range across n=6 repeats is 2.53*sigma, not 1*sigma, and a
// single future accept/reject decision needs a two-sample PREDICTION
// interval, not a description of the sample already in hand). This
// mechanism compares exactly one trial firing against exactly one baseline
// firing, so the right figure is the two-sided prediction interval for the
// difference of two independent same-distribution draws:
//   PI = t(.975, n-1=5) * std_c * sqrt(2) = 2.571 * std_c * 1.41421
//   z0: 2.571 * 0.05267 * 1.41421 = 0.1915 degC
//   z1: 2.571 * 0.03181 * 1.41421 = 0.1156 degC
//   z2: 2.571 * 0.05408 * 1.41421 = 0.1966 degC
// A trial/baseline pair separated by less than this, for that zone, is not
// distinguishable from noise at 97.5% one-sided confidence -- accepting it
// as "improvement" ratchets on nothing.
//
// ITER_TUNE_MIN_RELATIVE_IMPROVEMENT is a RELATIVE threshold; the floor
// above is ABSOLUTE (degrees C of normalized IAE). Converting requires the
// magnitude the relative fraction is taken OF. At this bench's own
// measured baseline magnitudes (the means above -- consistent with the
// 0.5-1.7 normalized-IAE range the A/B campaign report,
// logs/coupling/ab_campaign_report.md, shows for real runs), 20% relative
// works out to:
//   z0: 0.20 * 1.6043 = 0.3209 degC  (>= 0.1915 PI --  1.68x margin, SAFE)
//   z1: 0.20 * 1.1904 = 0.2381 degC  (>= 0.1156 PI --  2.06x margin, SAFE)
//   z2: 0.20 * 0.8765 = 0.1753 degC  (<  0.1966 PI -- 0.89x margin, UNSAFE)
//
// VERDICT: 20% relative is NOT uniformly conservative. For z0 and z1 it
// clears the noise floor with 1.7-2x margin -- correctly conservative,
// matching the header's original intent. For z2 it is already, TODAY, at
// today's measured typical magnitude, BELOW the noise floor: a trial that
// scored 17.5% better than baseline would be ACCEPTED even though a
// 19.7 degC... (0.1966 degC) swing on this zone is not statistically
// distinguishable from ordinary run-to-run noise (0.1966 degC). This is not a distant
// risk -- it is the current, live threshold on the current, live baseline
// magnitude. It gets WORSE as tuning succeeds: this mechanism's whole
// point is to shrink iae_normalized over time, and a purely relative
// threshold's absolute requirement shrinks in lockstep, while the noise
// floor (a property of the sensor/tick-jitter/ambient-drift measurement
// process, not of how well-tuned the zone currently is) does not shrink
// with it. Eventually any fixed relative fraction, applied to a
// sufficiently well-tuned baseline, demands less absolute separation than
// the noise floor -- a purely relative bound is the wrong shape for a
// floor that is fundamentally absolute.
//
// FIX: keep the relative fraction (still the right primary signal --
// it scales the required improvement to how far from good a zone
// currently is) but require the ABSOLUTE improvement to also clear a
// floor sized to the worst-case (largest) measured prediction interval
// across the three zones -- max(0.1915, 0.1156, 0.1966) = 0.1966, rounded
// up to 0.20 degC for headroom given the artifact's own like-for-like
// caveat could still be underestimating drift contamination in the other
// direction on a different bench day. A single global absolute constant
// (not per-zone) is deliberately used here even though the floor differs
// by nearly 2x across zones (0.077-0.147 in the range column): sizing to
// the WORST zone makes the other two zones somewhat more conservative than
// their own individual floor strictly requires, but that is the safe
// direction to err in, and three near-identical per-zone constants (0.19
// / 0.12 / 0.20, all within 2x of each other) are not worth the added
// state, host-test surface, and per-zone config plumbing (zone_mask
// already exists for grouping, not per-zone THRESHOLDS) for a difference
// this small. iter_tune_process_firing() now requires:
//   (baseline_score - firing_score) >= max(
//       ITER_TUNE_MIN_RELATIVE_IMPROVEMENT * baseline_score,
//       ITER_TUNE_MIN_ABSOLUTE_IMPROVEMENT_C)
// which behaves exactly as before (relative-driven) whenever the baseline
// score is large enough that its 20% already clears 0.20 degC (baseline
// score >= 1.0, true for today's z0/z1 and marginal for z2), and falls
// back to the absolute floor once tuning has driven the baseline score
// low enough that 20% of it no longer would.
#define ITER_TUNE_MIN_RELATIVE_IMPROVEMENT 0.20f

// See the UPDATE block above this constant for the derivation: the largest
// of the three zones' measured two-sample prediction intervals (z2,
// 0.1966 degC), rounded up to 0.20 degC. Applied as a floor UNDER the
// relative requirement (max() of the two, see iter_tune_process_firing()),
// never in place of it -- a large baseline score should still require
// a large absolute improvement, not just this floor.
#define ITER_TUNE_MIN_ABSOLUTE_IMPROVEMENT_C 0.20f

// Two firings are only comparable at a "comparable starting temperature" --
// project_autotune_needs_rested_baseline (this repo's own lesson):
// residual heat from a prior firing biases the fitted/measured behavior.
// Originally set to 5.0 degC on the mistaken belief that the holdfix_
// clean.jsonl/final.jsonl pair above was a clean noise-floor measurement;
// that pair is 4.8 degC apart and would have been ACCEPTED as comparable
// under that window -- i.e. the window was loose enough to admit the exact
// confound it exists to exclude. Tightened to 2.0 degC: the plant's own
// dwell-settle criterion and the residual-heat lesson both point at "a
// couple of degrees at most" for "this zone is at a rested baseline,"
// and 2.0 degC excludes the 4.8 degC contaminated pair with margin while
// still tolerating ordinary sensor/ambient jitter at a genuinely rested
// start.
#define ITER_TUNE_START_TEMP_TOLERANCE_C 2.0f

// ---------------------------------------------------------------------

typedef struct {
    float kp;
    float ki;
    float kd;
} iter_tune_gains_t;

// One firing's identity + score, as the caller reads it back from
// profile_firing_run_record_t / profile_exec_firing_stats_t after a run
// completes. iae_normalized is copied VERBATIM from
// profile_exec_firing_stats_t.iae_normalized (profile_executor_firing_
// stats.c's firing_stats_snapshot()) -- never recomputed here.
typedef struct {
    uint8_t profile_id;
    uint8_t zone_mask;     // profile.zone_mask for this run -- must match exactly to compare
    float   start_temp_c;  // this zone's actual_c at the first accumulated tick
    float   iae_normalized;// score -- lower is better
    iter_tune_gains_t gains; // the gains that PRODUCED this score
} iter_tune_firing_t;

// Persisted per-zone state. Zero-initialized is a valid, fully-disabled
// starting state (enabled == false, has_baseline == false) -- matches the
// "opt-in, default OFF, per zone" requirement without a separate init call.
typedef struct {
    bool enabled;                    // per-zone opt-in, default OFF

    bool               has_baseline; // false until the first firing under this mechanism completes
    iter_tune_firing_t baseline;     // last ACCEPTED (or seeded) firing: its gains + the score they earned

    bool               has_pending;    // a perturbation is currently on trial
    iter_tune_gains_t  pending_gains;  // gains under trial (only meaningful if has_pending)
    bool               next_perturb_negative; // alternates trial direction call to call -- see
                                               // iter_tune_propose_perturbation()'s doc comment
} iter_tune_zone_state_t;

typedef enum {
    ITER_TUNE_RESULT_DISABLED,          // state->enabled was false; nothing touched
    ITER_TUNE_RESULT_SEEDED_BASELINE,   // no prior baseline -- this firing became one, no comparison made
    ITER_TUNE_RESULT_BASELINE_REFRESHED,// firing used baseline gains (no trial pending) -- baseline updated
    ITER_TUNE_RESULT_REFUSED_NOT_COMPARABLE, // different profile/zone_mask/start temp -- trial left pending, unscored
    ITER_TUNE_RESULT_ACCEPTED,          // trial scored >= MIN_RELATIVE_IMPROVEMENT better -- new baseline
    ITER_TUNE_RESULT_REVERTED,          // trial scored worse, or not enough better -- baseline gains restored
} iter_tune_result_t;

// True (and reason left empty) only if `b` may be legitimately compared
// against `a` -- same profile_id, same zone_mask, and a starting
// temperature within ITER_TUNE_START_TEMP_TOLERANCE_C. Pure, no state
// mutation; exposed separately from iter_tune_process_firing() so it is
// independently host-testable per REQUIREMENT.
bool iter_tune_comparable(const iter_tune_firing_t *a, const iter_tune_firing_t *b, char *reason,
                           size_t reason_len);

// Returns the gains that should be applied to this zone's live PID config
// RIGHT NOW -- the pending trial's gains if one is in flight, otherwise the
// current accepted baseline. A caller that always asks this function for
// "what gains do I run with" gets an exact revert for free: REVERTED never
// recomputes anything, it just clears has_pending, so this function starts
// returning the untouched baseline.gains again -- the same float bits that
// were accepted last time, not a recomputation of them.
iter_tune_gains_t iter_tune_active_gains(const iter_tune_zone_state_t *state);

// Proposes a new bounded, revertible perturbation of the current baseline
// gains (kp and ki move by +/-ITER_TUNE_PERTURB_FRACTION, alternating sign
// call to call so both directions get explored over time rather than
// walking off in whichever direction the first nudge happened to try; kd
// is left untouched -- derivative gain is the noise-sensitive term and is
// out of scope for a blind perturbation with no plant model backing it).
// Clamped to [ITER_TUNE_GAIN_FLOOR_C, ITER_TUNE_GAIN_CEIL_C]. Requires
// state->enabled && state->has_baseline && !state->has_pending; returns
// false (state untouched) otherwise -- callers should not propose a second
// trial while one is already outstanding. On success, writes
// state->pending_gains, sets has_pending, flips next_perturb_negative, and
// returns true.
bool iter_tune_propose_perturbation(iter_tune_zone_state_t *state, iter_tune_gains_t *out_gains);

// The one call site this whole module funnels through, at run-completion
// time only (see this header's top comment). `firing` is the just-
// completed run's identity/score, scored under iter_tune_active_gains()'s
// gains as of when that run STARTED (the caller is responsible for that
// invariant -- this function has no way to check it).
iter_tune_result_t iter_tune_process_firing(iter_tune_zone_state_t *state, const iter_tune_firing_t *firing,
                                             char *reason, size_t reason_len);

#ifdef __cplusplus
}
#endif

// ---------------------------------------------------------------------
// INTEGRATION POINT (documented, not wired in by this change -- see this
// file's top comment for why: avoiding profile_executor.c during a session
// where other agents are mid-flight on adjacent work in that area).
//
// At run start (profile_executor_run(), before the executor task begins
// driving a PID-mode zone): for each zone with iter_tune enabled, call
// iter_tune_active_gains() and write the result into that zone's live
// pid_cfg (zones_config_set_pid() or equivalent) before the run begins.
//
// At run completion (profile_executor_firing_stats.c's firing_stats_
// maybe_finalize(), the same call site that already builds a
// profile_firing_run_record_t): for each zone with iter_tune enabled,
// build an iter_tune_firing_t from that record's zr->stats.iae_normalized,
// zr->kp/ki/kd, rec->profile_id, rec->zone_mask, and the zone's actual_c
// at the run's first accumulated tick (not currently captured anywhere --
// a caller wiring this in needs to add that one field, e.g. to zone_
// runtime_t, alongside fs_target_min_c/fs_target_max_c's existing
// first-tick capture pattern), then call iter_tune_process_firing(). If
// the result is ITER_TUNE_RESULT_ACCEPTED or ITER_TUNE_RESULT_REVERTED,
// persist the (possibly reverted) state and call
// iter_tune_propose_perturbation() to arm the next run's trial.
