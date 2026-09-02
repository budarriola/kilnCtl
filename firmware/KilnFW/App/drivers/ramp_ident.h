// ramp_ident.h -- PID_EXPANSION_PLAN.md 3.3, "Dynamics from ramps": re-fit a
// zone's FOPDT tau/dead-time from an ORDINARY firing ramp segment, instead
// of only from a dedicated step/autotune run.
//
// Pure math, no ESP-IDF/FreeRTOS dependency -- same host-testability
// contract as pid_autotune.c/max31856_codec.c/panel_codec.c
// (test_ramp_ident.c exercises this file directly). NOT wired into
// adaptive_tune.c yet: adaptive_tune.c/.h are under review by another agent
// as this is written, so this is a self-contained module with a documented
// integration point below, for a later commit to call.
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
// -- Coupling: what the fitted tau/L actually mean --------------------------
//
// This is a SINGLE-ZONE fit. It cannot separate "this zone's own element
// responding to its own duty step" from "a neighbour zone's duty also
// moved around the same time and its heat arrived here." Nothing in this
// file inspects other zones' duty at all. Concretely:
//
//   - If the OTHER zones' duty is steady across the step-and-response
//     window (the common case for a single-zone excitation inside an
//     otherwise-smooth multi-zone ramp), the detrending step above
//     absorbs their steady contribution into the removed baseline slope,
//     and the fitted tau/L are a reasonable estimate of THIS zone's own
//     diagonal dynamics -- the same quantity pid_autotune_fit_fopdt()
//     identifies from a dedicated step test, just noisier.
//   - If a NEIGHBOUR's duty also changes inside the response window, this
//     fit has no way to know that, and the fitted tau/L become a blend of
//     this zone's own (fast, ~264-271 s tau per PID_EXPANSION_PLAN.md
//     section 2) dynamics and the much slower cross-zone path (620-730 s
//     tau, 135-158 s dead time in the same section). A caller integrating
//     this later MUST NOT treat every accepted fit as pure diagonal
//     dynamics on that basis alone -- see the integration note below for
//     the mitigation left for that later pass. Silently attributing
//     neighbour-driven rise to this zone's own dynamics is exactly the
//     error that biased the existing diagonal K_dc high (section 2); this
//     file does not repeat it for tau/L, it just says plainly where the
//     same risk still lives.
//
// -- Validated against real captures -----------------------------------
//
// Run over every dwelling==false stretch of tools/PcTools/tests/fixtures/
// plant_sim/*.jsonl (27 zone-segments, all five captures), with each
// zone's PID_EXPANSION_PLAN.md section 2 identified K_dc as the model
// gain: 2 of 27 segments (final_z1_seg0, final_z2_seg0) were ACCEPTED;
// the other 25 were refused, spread across every gate in the list above
// (insufficient duty span, no step event, step not held, insufficient
// transient) rather than one gate doing all the work -- so the gate is a
// real discriminator, not a rubber stamp or a dead letter. The dominant
// refusal reason by far, though, is RAMP_IDENT_STEP_NOT_HELD: in this
// data set, duty is very rarely quiet for even the ~15-sample bounded
// response window (RAMP_IDENT_MAX_RESPONSE_SAMPLES in the .c file) --
// this small kiln's feedforward is almost always actively adjusting duty
// to track the ramp, which is CORRECT controller behaviour, not a data
// artifact, and it is exactly the well-tracked-ramp case this whole item
// exists to refuse rather than fit through. A firing with more genuinely
// held plateaus (a coarser feedforward update rate, or a longer ramp with
// real settling stretches) would very likely accept more; this data set
// mostly does not offer them, and that is reported honestly here rather
// than loosened away.
//
// The two ACCEPTED fits (tau ~= 22-37 s, dead time ~= 4-16 s) disagree
// substantially with the bench-identified diagonal figures in
// PID_EXPANSION_PLAN.md section 2 (tau 264-271 s, dead time 34-53 s) --
// reported honestly rather than retuned to agree, per this item's own
// validation requirement. The most likely reason, also found during this
// validation and not merely assumed: even a duty trace that passes the
// STEP_NOT_HELD gate here only "roughly plateaus" over a bounded ~15-
// sample window, a far looser bar than autotune's own dedicated, fully-
// settled, MUCH LONGER step trace -- so what these two fits actually
// measure is closer to this plant's fast CLOSED-LOOP apparent response
// (the control loop's own correction shortening the observed rise) than
// its open-loop FOPDT dynamics. Combined with the single-zone/coupling
// caveat above, an accepted ramp-segment fit should be treated as a
// noisy, lower-confidence, possibly closed-loop-biased sibling of a
// dedicated autotune fit, not a substitute for one -- exactly the
// explicit caveat PID_EXPANSION_PLAN.md 3.3 asks this item to state
// rather than silently average away.
//
// -- Integration point (NOT wired up by this commit) -------------------------
//
// adaptive_tune.c already harvests per-zone traces during a run
// (adaptive_tune_zone_tick()) and applies refinements only at a safe run
// boundary (adaptive_tune_run_end()). The natural hook for this module is
// a ramp-segment ring analogous to its dwell ring: collect (t, target_c,
// actual_c, duty) while profile_executor.c reports the zone is ramping
// (mirroring the `dwelling` flag adaptive_tune_zone_tick() already takes),
// hand the finished segment to ramp_ident_fit() at ramp-end or run-end, and
// -- to close the neighbour-contribution gap noted above before applying
// anything -- also require the OTHER zones' duty to have stayed within a
// small band across the step-and-response window before accepting the
// result as this zone's own diagonal tau/L. That neighbour-quiescence
// check needs multi-zone data this file's single-zone interface does not
// carry, so it is left for whoever wires this in, not implemented here.
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
