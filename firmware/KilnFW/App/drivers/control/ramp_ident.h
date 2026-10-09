// ramp_ident.h -- PID_EXPANSION_PLAN.md 3.3, "Dynamics from ramps": re-fit a
// zone's FOPDT tau/dead-time from an ORDINARY firing ramp segment, instead
// of only from a dedicated step/autotune run.
//
// ============================================================================
// PARKED EXPERIMENT -- DO NOT WIRE INTO ANY CONTROL PATH IN ITS CURRENT FORM.
//
// An opus review (commit 13dcf49, and the review that followed it) established
// that the two-point reaction-curve fit this file implements measures NO
// plant information when run on this kiln's real closed-loop data -- see
// "-- What the fit actually measures on real data --" below for the full
// analysis. This is not a caveat on an otherwise-useful result; the fitted
// tau/L are an artifact of the model gain, the apparent duty step, and the
// profile's ramp rate, and would come out identical on a plant with any tau
// whatsoever. Do not call ramp_ident_fit() from adaptive_tune.c or any other
// control-path code, do not loosen its thresholds to accept more segments,
// and do not retune it to make the two accepted fixture fits agree with the
// bench-identified dynamics -- that would be curve-fitting the record, not
// fixing the estimator. See "-- What would actually work --" below for the
// direction a real fix would need to take.
// ============================================================================
//
// Pure math, no ESP-IDF/FreeRTOS dependency -- same host-testability
// contract as pid_autotune.c/max31856_codec.c/panel_codec.c
// (test_ramp_ident.c exercises this file directly). NOT wired into
// adaptive_tune.c or anywhere else: as of this writing the only references
// to this module anywhere in the tree are firmware/KilnFW/App/drivers/
// CMakeLists.txt:249 (build it), test_main.c:49/101 (run its host tests),
// and App/test/build_host_tests.ps1:74/96 (host-test build script). Keep it
// that way until a rewritten estimator (see below) replaces the fit method.
//
// -- Why most ramps carry no usable information -----------------------------
//
// A ramp segment only carries identifiable dynamics if something in it
// actually EXCITES the plant: a duty change big enough, and fast enough,
// to expose how the temperature lags behind it. A slow, well-tracked ramp
// -- the kind PID_EXPANSION_PLAN.md section 1/2's terminal-ease-off and
// coupled feedforward work were explicitly built to produce -- holds
// tracking error near zero throughout, which means duty is already
// following almost exactly the smooth trajectory the CURRENT model
// predicts. Fitting tau/L from a segment like that does not measure the
// plant; it mostly re-derives the model that generated the duty command
// in the first place, with sensor quantization (0.1 C) as the only "new"
// signal -- confident-looking garbage. This file's excitation gate
// (ramp_ident_fit(), the checks before any fit math runs) exists
// specifically to refuse that case rather than silently return a number.
// Refusing is the safe default: an un-refit model is exactly today's
// behaviour, so a refusal costs nothing a dedicated autotune run cannot
// still supply.
//
// -- What the excitation gate actually checks --------------------------------
//
//   1. Enough samples/duration to see a real transient at all
//      (RAMP_IDENT_MIN_SAMPLES / RAMP_IDENT_MIN_DURATION_S).
//   2. Duty actually moves over the whole segment
//      (RAMP_IDENT_MIN_DUTY_SPAN) -- a duty trace pinned near one value
//      cannot excite anything.
//   3. A genuine STEP-LIKE event somewhere in the segment: duty's smoothed
//      level over a trailing window differs from its smoothed level over a
//      leading window by at least RAMP_IDENT_MIN_STEP_DUTY, with enough
//      samples on both sides to fit a baseline and observe a response
//      (RAMP_IDENT_PRE_STEP_MIN_SAMPLES / RAMP_IDENT_MIN_RESPONSE_SAMPLES).
//      This is the "sufficient duty variation" and "actual transient at
//      segment entry" tests the plan calls for, generalized to fire
//      wherever in the segment the step happens to sit, not only at t=0.
//   4. The temperature actually DEVIATES from the trend it was already on
//      before the step (RAMP_IDENT_MIN_TRANSIENT_C, checked against the
//      DETRENDED response -- see below). This is the discriminator that
//      catches the well-tracked-ramp case directly: if duty steps but the
//      plant's trajectory does not measurably bend away from its pre-step
//      trend, the step carried no information the model didn't already
//      have, no matter how big the duty change was.
//
//   5. Duty must roughly PLATEAU after the step for the rest of the
//      segment (RAMP_IDENT_STEP_HOLD_TOL), not keep moving. Found
//      necessary by validating against real captured ramp segments (see
//      the "Validated against real captures" note below): the two-point
//      method assumes a held step, and real mid-ramp duty commonly keeps
//      climbing after the initial jump because feedforward is still
//      tracking a moving target, not settling -- fitting through that
//      folds the ongoing duty increase into the apparent temperature
//      rise and biases the fitted tau low.
//
// All five must pass. Any refusal is reported through *out with a specific
// reason string, same convention as adaptive_tune.c's set_refusal()/
// set_reason() (fixed-size buffer, refusal is not a special "zero" model --
// see ramp_ident_result_t below).
//
// -- Fit method ---------------------------------------------------------
//
// Once a step event is located, this reuses the SAME two-point (28.3%/
// 63.2%) reaction-curve method pid_autotune_fit_fopdt() uses on a clean
// step test (tau = 1.5*(t63-t28), dead_time = t63-tau -- pid_autotune.c),
// applied to the DETRENDED response: actual_c minus a linear
// extrapolation of the plant's own pre-step trend (fitted by least squares
// over the samples immediately before the step). Detrending is what makes
// the method valid mid-ramp instead of only from a flat baseline: it
// removes the ramp's ordinary climb (this zone's own steady rise, plus any
// steady neighbour contribution already present before the step) so what
// is left is, to first order, the plant's response to JUST the duty
// change. The expected asymptotic rise used to place the 28.3%/63.2%
// targets is K_dc * duty_delta, using the CALLER-SUPPLIED model gain (the
// same diagonal K_dc adaptive_tune.c already refines) -- this fit never
// tries to re-derive K, only tau/L, consistent with the plan item's scope.
//
// -- What the fit actually measures on real data -----------------------------
//
// Ported to Python and reproduced exactly against the two fixture segments
// this module accepts (final_z1_seg0: tau 35.56s, L 16.88s; final_z2_seg0:
// tau 23.48s, L 3.31s), the fitted tau/L reduce ANALYTICALLY to a function
// of three quantities that have nothing to do with plant dynamics: the
// model gain K, the apparent duty step delta_d, and the profile's ramp rate
// R. For a linear rise at rate R, the two-point method's crossing times are
// t28 = 0.283*K*delta_d/R and t63 = 0.632*K*delta_d/R, so the fitted
// tau = 1.5*(t63-t28) = 0.524*K*delta_d/R -- a closed-form expression that
// contains no tau or L term from the actual plant at all. Predicted vs.
// fitted: z1 43s predicted vs. 35.6s fitted; z2 22s predicted vs. 23.5s
// fitted -- the fitted numbers track the closed-form artifact, not the
// bench-identified dynamics (tau 264-271s, dead time 34-53s,
// PID_EXPANSION_PLAN.md section 2). This is not "measuring the wrong loop"
// (closed-loop apparent dynamics instead of open-loop plant dynamics, as an
// earlier version of this comment claimed) -- it measures NEITHER loop. The
// fitted tau would come out identical on a plant with any tau whatsoever,
// because the quantity being fitted is arithmetic on the duty/ramp trace,
// not a response to it.
//
// Two joint causes, both confirmed against the fixture data, not merely
// theorized:
//
//   1. RAMP_IDENT_MAX_RESPONSE_SAMPLES (ramp_ident.c) is 15, which is 153s
//      at this fixture data's ~10.2s/sample tick -- against a true
//      t63 = L+tau ~= 300-320s at the bench-identified dynamics. The bounded
//      response window closes roughly HALF way to the true 63.2% crossing,
//      so the true response is never observed at all; nothing in the module
//      relates the window length to the prior tau estimate it could be
//      checked against.
//   2. The detrend baseline (fit_trend() over the samples strictly before
//      the step) sits INSIDE the plant's own dead time in both accepted
//      segments -- the pre-step window is dead flat (fitted baseline slope
//      6.5e-05 C/s, i.e. nothing), so detrending removes approximately
//      nothing. What is left as "the response" is just the ordinary ramp
//      climb the plant was already on, not a step response.
//
// A third, independent problem found in the same two segments: both
// accepted fits had zone 0's duty moving (0.044 -> 0.205) concurrently with
// the "step" under test, directly violating the single-zone/neighbour-
// quiescence assumption this fit relies on -- 2 for 2, not a rare edge case.
// And step_index landed on RAMP_IDENT_PRE_STEP_MIN_SAMPLES, the MINIMUM
// allowed index, in both segments, because the candidate search at
// ramp_ident.c:199 (`fabsf(delta) > fabsf(best_delta)`) maximizes
// |duty delta| ALONE with no preference for a longer pre-step baseline --
// it systematically prefers the earliest, least-baselined candidate. The
// "step" accepted in both cases is a slice of a smooth run-start duty climb
// (0.135, 0.163, 0.202, 0.234, 0.267 duty across consecutive samples), not
// a step at all.
//
// Measured consequence of integrating this as-is: dropping tau from the
// bench value (264s) to a fitted value like 30s would cut the climb
// feedforward term roughly 9x, since profile_executor_feedforward.c:27
// computes u_ff = (T_sp-T_amb)/K_dc + (dT_sp/dt)*tau/K_dc -- tau enters the
// climb term linearly. And dropping dead time from 34-53s to 3-17s would
// shrink the terminal ease-off window (sized at 2x dead time) by roughly
// 5x. Both are the kind of "confidently wrong" adjustment a refusal-based
// gate is supposed to prevent; the gate did not prevent it here because the
// two segments it let through are exactly the pathological case above.
//
// -- Real-data refusal distribution (27 dwelling==false segments, all five
// tools/PcTools/tests/fixtures/plant_sim/*.jsonl captures, each zone's
// PID_EXPANSION_PLAN.md section 2 K_dc as the model gain) ------------------
//
//   RAMP_IDENT_STEP_NOT_HELD          21
//   RAMP_IDENT_TOO_FEW_SAMPLES         3
//   RAMP_IDENT_NO_STEP_EVENT           3
//   RAMP_IDENT_INSUFFICIENT_DUTY_EXCITATION  1
//   RAMP_IDENT_FIT_FAILED              1
//   RAMP_IDENT_TOO_SHORT_DURATION      0
//   RAMP_IDENT_INVALID_GAIN            0
//   RAMP_IDENT_INSUFFICIENT_TRANSIENT  0
//   (accepted: 2 -- final_z1_seg0, final_z2_seg0, both artifacts as above)
//
// An earlier version of this comment claimed refusals were "spread across
// every gate", implying INSUFFICIENT_TRANSIENT -- the discriminator the
// design rationale above rests its whole argument on -- was doing real
// work. It fires on ZERO of 27 real segments. STEP_NOT_HELD alone accounts
// for 21 of 25 refusals: this small kiln's feedforward is almost always
// actively adjusting duty to track the ramp (correct controller behaviour),
// so a genuinely held post-step plateau essentially never occurs in this
// data set, and the module's actual behaviour on real traces is "refuse via
// STEP_NOT_HELD, or fall through to the analytical artifact above" -- not
// the graduated, multi-gate discrimination the original text described.
//
// -- What would actually work -------------------------------------------
//
// The two-point reaction-curve method (this file, and
// pid_autotune_fit_fopdt()) needs a step that is applied and then HELD
// for at least ~2*(L+tau) ~= 600s at the bench-identified dynamics, so the
// 63.2% crossing is actually observed. This kiln's closed-loop feedforward
// never holds duty that long -- that is precisely what
// RAMP_IDENT_STEP_NOT_HELD is reporting 21 times out of 28. Nor does duty
// ever saturate here (observed range 0.04-0.45 across all 27 segments), so
// "only accept segments where duty pins against 0 or 1" is not an escape
// hatch either -- there is no data on this plant where it would fire.
//
// A method that does not need a held step: a whole-segment output-error /
// ARX estimator. Simulate a FOPDT model driven by the ACTUAL recorded duty
// trace over the WHOLE segment (not just a windowed step-and-response
// slice), and fit tau/L by least squares against the residual between the
// simulated and actual temperature trace. This needs no step and no hold at
// all -- it uses whatever duty motion the segment happens to contain -- so
// it can use all 27 fixture segments instead of 2, and it extends naturally
// to the coupled case (drive the simulation with every zone's duty, not
// just this one, and fit the cross terms too). This is a different
// estimator, not a parameter tweak to the one in this file, and is left for
// a future item -- do not attempt to retrofit it onto the two-point method
// above.
//
// -- Integration point (NOT wired up, and must stay that way -- see the
// PARKED EXPERIMENT banner at the top of this file) --------------------------
//
// adaptive_tune.c already harvests per-zone traces during a run
// (adaptive_tune_zone_tick()) and applies refinements only at a safe run
// boundary (adaptive_tune_run_end()). If a rewritten (whole-segment ARX,
// per above) estimator someday replaces the fit method in this file, the
// natural hook is a ramp-segment ring analogous to adaptive_tune's dwell
// ring: collect (t, target_c, actual_c, duty) while profile_executor.c
// reports the zone is ramping, hand the finished segment to the estimator
// at ramp-end or run-end, and require the OTHER zones' duty trace (or, for
// the ARX approach, feed it directly) so cross-coupling is modeled rather
// than silently folded into a single zone's diagonal estimate. None of that
// is implemented here, and the CURRENT (two-point) fit method in this file
// must not be wired to it regardless -- see the top-of-file banner.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One (time, target, actual, duty) sample of a ramp-segment trace. t_s is
// elapsed time from an arbitrary segment-local zero (samples must be sorted
// ascending by t_s; spacing need not be perfectly uniform). actual_c is
// calibration-corrected, same convention as adaptive_tune_zone_tick()'s
// actual_c. duty is the zone's commanded duty in [0,1].
typedef struct {
    float t_s;
    float target_c;
    float actual_c;
    float duty;
} ramp_ident_sample_t;

typedef enum {
    RAMP_IDENT_OK = 0,
    RAMP_IDENT_TOO_FEW_SAMPLES,       // n below RAMP_IDENT_MIN_SAMPLES
    RAMP_IDENT_TOO_SHORT_DURATION,    // t_s span below RAMP_IDENT_MIN_DURATION_S
    RAMP_IDENT_INVALID_GAIN,          // caller's k_gain_c_per_duty <= 0 -- can't place 28.3%/63.2% targets
    RAMP_IDENT_INSUFFICIENT_DUTY_EXCITATION, // whole-segment duty range below RAMP_IDENT_MIN_DUTY_SPAN
    RAMP_IDENT_NO_STEP_EVENT,         // no window pair anywhere in the segment cleared RAMP_IDENT_MIN_STEP_DUTY
                                       // with enough samples on both sides
    RAMP_IDENT_STEP_NOT_HELD,         // a step was found, but duty kept moving through the rest of the segment
                                       // instead of holding -- see ramp_ident.c's "step must hold" comment;
                                       // found necessary validating against real captured ramp segments, where
                                       // feedforward keeps raising duty through an ongoing ramp rather than
                                       // settling to a new level, which biases a two-point tau fit low
    RAMP_IDENT_INSUFFICIENT_TRANSIENT,// a step was found but the DETRENDED response never cleared
                                       // RAMP_IDENT_MIN_TRANSIENT_C -- the well-tracked-ramp refusal case
    RAMP_IDENT_FIT_FAILED,            // response found the transient but never reached the 28.3%/63.2%
                                       // crossings, or they came out in the wrong order (tau <= 0)
} ramp_ident_result_t;

typedef struct {
    ramp_ident_result_t result;
    bool  valid;              // true only when result == RAMP_IDENT_OK
    float tau_s;               // valid only if `valid`
    float dead_time_s;         // valid only if `valid`

    // Diagnostics, populated whenever a step candidate was at least found
    // (result >= RAMP_IDENT_INSUFFICIENT_TRANSIENT), zero otherwise -- lets
    // a test (or a future caller) assert on the REASON a fit was accepted
    // or refused, not just the verdict, same convention as
    // adaptive_tune_ki_diag_t::zero_crossings.
    uint32_t step_index;             // sample index the step event was centered at
    float    step_duty_delta;        // signed, smoothed after-window duty minus before-window duty
    float    pre_step_slope_c_per_s; // fitted baseline (pre-step) trend, removed before fitting
    float    transient_peak_c;       // max |detrended response| observed in the post-step window

    char refusal_reason[96]; // empty when valid; a specific reason otherwise (see .c for the exact
                              // wording per ramp_ident_result_t case) -- same fixed-buffer convention
                              // as adaptive_tune.c's set_refusal()/set_reason().
} ramp_ident_fit_t;

// Attempts to re-fit tau/dead-time from one ramp segment. samples must be
// sorted ascending by t_s and cover exactly one segment (a ramp, or any
// other stretch of firing where target_c is changing -- nothing here
// requires target_c's motion to be linear). k_gain_c_per_duty is the
// zone's CURRENT model gain (zones_config_get_model()'s K_dc, or
// adaptive_tune's refined value if available) -- used only to place the
// expected-asymptote targets for the two-point crossing search, never
// re-fitted itself. out must not be NULL; *out is always fully populated
// (result, refusal_reason, and whichever diagnostics were reached) even on
// refusal, so a caller/test can inspect why without a second call.
//
// Returns the same value as out->result, for a caller that just wants the
// verdict inline.
ramp_ident_result_t ramp_ident_fit(const ramp_ident_sample_t *samples, uint32_t n, float k_gain_c_per_duty,
                                    ramp_ident_fit_t *out);

#ifdef __cplusplus
}
#endif
