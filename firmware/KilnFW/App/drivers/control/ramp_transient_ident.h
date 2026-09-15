// ramp_transient_ident.h -- whole-segment output-error re-identification of
// a zone's FOPDT tau/dead-time from an ORDINARY firing ramp segment.
//
// This is the estimator ramp_ident.h's "What would actually work" section
// calls for, replacing that file's two-point reaction-curve method (parked
// there -- see its top-of-file banner -- because it was shown to reduce
// analytically to a closed-form function of known quantities with no plant
// content at all). Full rationale, the acceptance-gate design, and
// simulation validation (including a closed-loop mismatched-model case the
// old method could never resolve): docs/RAMP_TRANSIENT_IDENT_DESIGN.md.
//
// Pure math, no ESP-IDF/FreeRTOS dependency -- same host-testability
// contract as ramp_ident.c/pid_autotune.c. NOT wired into adaptive_tune.c,
// profile_executor.c, or any other control path: this module only takes a
// finished sample array and returns a result. See the design doc's "Guard 1
// / profiles_stop interaction" section for why wiring a live harvester is
// deliberately left as future work.
//
// -- Method summary (full detail in the design doc) -------------------------
//
// Given one segment's (t, target, actual, duty) trace, simulate a candidate
// FOPDT model (dT/dt = (K*duty_delayed(t-L) - (T-T0))/tau, forward Euler at
// the trace's own spacing) driven by the segment's OWN recorded duty over
// the WHOLE segment -- not a windowed step-and-response slice -- and fit
// (tau, L) by least squares against the residual. K is caller-supplied and
// never re-fitted (same scope as ramp_ident.c). T0 is corrected for a
// linear pre-segment trend (see RTI_TREND_SAMPLES) to reduce, though not
// eliminate, the same non-rested bias autotune has.
//
// Four gates must ALL pass before a fit is reported valid: duty excitation
// floor, minimum duration, cost-curvature (identifiability) around the
// fitted tau, and improvement-over-null versus the caller's CURRENT
// (tau, L). The curvature and improvement-over-null gates are what
// distinguish this from ramp_ident.c's fit-always-returns-a-number
// behaviour -- they can and do refuse a segment that carries no
// information, rather than returning a confident wrong tau.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One (time, target, actual, duty) sample. Same convention as
// ramp_ident_sample_t: t_s ascending, actual_c calibration-corrected,
// duty in [0,1].
typedef struct {
    float t_s;
    float target_c;
    float actual_c;
    float duty;
} rti_sample_t;

typedef enum {
    RTI_OK = 0,
    RTI_TOO_FEW_SAMPLES,          // n below RTI_MIN_SAMPLES
    RTI_TOO_SHORT_DURATION,       // t_s span below RTI_MIN_DURATION_S
    RTI_INVALID_GAIN,             // caller's k_gain_c_per_duty <= 0
    RTI_INSUFFICIENT_DUTY_EXCITATION, // whole-segment duty std/range below floor -- a flat/held
                                       // duty (pure dwell) carries no tau information at all
    RTI_TREND_RESIDUAL_TOO_LARGE, // pre-segment window is not close to linear -- plant was still
                                   // decaying from an earlier transient, not rested; see the design
                                   // doc's "non-rested bias" section
    RTI_FLAT_COST,                // best-fit tau found, but perturbing it by +-RTI_PERTURB_FRAC does
                                   // not raise the SSE by RTI_MIN_CURVATURE_FRAC -- the segment does
                                   // not actually distinguish candidate tau values (closed-loop
                                   // identifiability failure); THE central negative-case gate
    RTI_NO_IMPROVEMENT,           // best fit's SSE is not enough better than simulating with the
                                   // caller's current (tau, L) -- this segment tells us nothing the
                                   // existing model doesn't already predict
} rti_result_t;

typedef struct {
    rti_result_t result;
    bool  valid;              // true only when result == RTI_OK
    float tau_s;              // valid only if `valid`
    float dead_time_s;        // valid only if `valid`

    // Diagnostics, always populated once the excitation/duration/trend
    // gates are passed (result >= RTI_FLAT_COST), zero otherwise -- lets a
    // test or future caller assert on WHY a fit was accepted or refused.
    float best_sse;           // SSE at the fitted (tau_s, dead_time_s)
    float null_sse;           // SSE simulating with the caller's current (tau, L)
    float perturbed_sse;      // SSE at tau_s*(1+RTI_PERTURB_FRAC), the curvature probe
    float duty_std;
    float trend_residual_c;

    char refusal_reason[96];  // empty when valid; specific reason otherwise, same fixed-buffer
                               // convention as ramp_ident.c / adaptive_tune.c.
} rti_fit_t;

// Attempts to re-identify tau/dead-time from one ramp segment via
// whole-segment output-error simulation. samples must be sorted ascending
// by t_s. k_gain_c_per_duty is the zone's current model gain (never
// re-fitted). current_tau_s/current_dead_time_s are the model's EXISTING
// parameters, used only for the improvement-over-null gate (RTI_NO_
// IMPROVEMENT) -- pass the same values adaptive_tune/zones_config would
// otherwise use. out must not be NULL; *out is always fully populated
// (result, refusal_reason, and whichever diagnostics were reached) even on
// refusal.
//
// Returns the same value as out->result.
rti_result_t rti_fit(const rti_sample_t *samples, uint32_t n, float k_gain_c_per_duty, float current_tau_s,
                      float current_dead_time_s, rti_fit_t *out);

#ifdef __cplusplus
}
#endif
