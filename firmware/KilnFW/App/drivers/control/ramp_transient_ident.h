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
// FOPDT model (dT/dt = (K*duty_delayed(t-L) - (T-T_amb))/tau, forward Euler
// at the trace's own spacing) driven by the segment's OWN recorded duty over
// the WHOLE segment -- not a windowed step-and-response slice -- and fit
// (tau, L) by least squares against the residual. K is caller-supplied and
// never re-fitted (same scope as ramp_ident.c). The dynamics relax toward
// the CALLER-SUPPLIED true ambient (`ambient_c`), not the segment's own
// starting temperature -- a segment that starts above ambient (i.e. every
// segment but the first of a firing) needs only the EXCESS duty over the
// steady-state hold duty to explain its motion; feeding the model the full
// duty while relaxing it toward its own start temperature leaves a phantom
// heating term that only `tau` can absorb, inflating it (see
// docs/RAMP_TRANSIENT_IDENT_DESIGN.md's "Ambient reference" section and
// docs/audits/ramp_transient_ident_review_2026-09-14.md). The pre-segment
// trend fit (see RTI_TREND_SAMPLES) is used only for the rested-baseline
// GATE below (RTI_TREND_RESIDUAL_TOO_LARGE) -- it no longer supplies the
// dynamics' reference temperature.
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
    RTI_INVALID_GAIN,             // caller's k_gain_c_per_duty <= 0 or non-finite
    RTI_INVALID_AMBIENT,          // caller's ambient_c is NaN/infinite. Without this check a non-finite
                                   // ambient poisons every SSE to NaN, the tau search never updates its
                                   // running best (every NaN comparison is false), and the module fell out
                                   // of the bottom as RTI_NO_IMPROVEMENT with a refusal reason that blamed
                                   // the SEGMENT ("tells us nothing the existing model doesn't already
                                   // predict") rather than the caller -- refused for the right reason now.
    RTI_INSUFFICIENT_DUTY_EXCITATION, // whole-segment duty std/range below floor -- a flat/held
                                       // duty (pure dwell) carries no tau information at all
    RTI_TREND_RESIDUAL_TOO_LARGE, // pre-segment window is not close to linear -- plant was still
                                   // decaying from an earlier transient, not rested; see the design
                                   // doc's "non-rested bias" section
    RTI_FLAT_COST,                // best-fit tau found, but perturbing it by +-RTI_PERTURB_FRAC does
                                   // not raise the SSE by RTI_MIN_CURVATURE_FRAC -- the segment does
                                   // not actually distinguish candidate tau values. Reachable in
                                   // practice when the CALLER'S supplied k_gain_c_per_duty is badly
                                   // wrong (understated): the model then fits equally (badly) at every
                                   // tau because the dominant residual is the gain mismatch, not a
                                   // tau mismatch -- see test_ramp_transient_ident.c's
                                   // "gain badly mismatched" case and the design doc.
    RTI_NO_IMPROVEMENT,           // best fit's SSE is not enough better than simulating with the
                                   // caller's current (tau, L) -- this segment tells us nothing the
                                   // existing model doesn't already predict
    RTI_TAU_AT_SEARCH_BOUND,      // the optimiser settled on RTI_TAU_MIN_S or RTI_TAU_MAX_S itself. An
                                   // optimum sitting exactly on a search bound is not a measurement, it is
                                   // the bound: the true minimum is outside the searched interval (or the
                                   // objective is monotone across it), so the returned number carries no
                                   // information about how far outside. Recommended by the 2026-09-14 review
                                   // and deliberately withheld there because it would have masked the loud
                                   // saturated rows of the ambient defect while leaving the quiet wrong ones;
                                   // with that defect fixed the objection is void and this is strictly a gain.
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
// re-fitted). ambient_c is the plant's TRUE ambient reference (not the
// segment's own starting temperature) -- the model's relaxation target;
// getting this right is what distinguishes a segment starting well above
// ambient (the common case) from the misdiagnosed-as-rare case of segment
// 1 of a firing.
//
// *** ambient_c ACCURACY IS THE CALLER'S OBLIGATION AND IS NOT CHECKABLE HERE. ***
// Only ambient_c's FINITENESS is gated (RTI_INVALID_AMBIENT). Its VALUE cannot be
// policed from inside this function, and the sensitivity is severe: the fitted-tau
// error is a function of the ambient error alone, independent of segment start
// (measured 2026-09-15, true tau 280 s, K 200, true ambient 25 C, three different
// segment starts giving identical rows, all four gates passing with RTI_OK at
// every accepted row):
//
//    ambient_c error   -5.0   -2.5    0.0   +2.5   +5.0  +10.0  +15.0  (degC)
//    fitted tau          (*)  190.1  279.8  368.8  456.6  635.5  811.4  (s)
//    error vs true       --   -32%   -0.1%   +32%   +63%  +127%  +190%
//    (*) -5.0 C happened to be refused NO_IMPROVEMENT on those traces -- luck, not a gate.
//
// No gate can catch this because tau and ambient are very nearly DEGENERATE over
// a ramp segment: both enter only through the standing heat balance
// K*duty - (T - ambient), so an ambient error is absorbed by tau at roughly 12%
// of tau per degC while the model still explains the data. Measured
// whole-segment RMS residual rises only 0.029 C -> 0.060 C across that entire
// 0 to +15 C ambient sweep, i.e. it stays within about 2x the 0.1 C telemetry
// quantization floor -- there is no residual threshold that separates "right
// ambient" from "+15 C ambient" without also rejecting good fits.
//
// A caller must therefore supply a genuinely MEASURED ambient. KilnFW's producer
// is profile_executor's s_exec.ambient_c (a MAX31856 cold-junction reading
// captured at run start), which carries s_exec.ambient_from_cj -- false meaning
// FALLBACK_AMBIENT_C was substituted. A harvester must refuse to run at all when
// that flag is false: a fallback constant is exactly the several-degC-class error
// this table prices.
//
// k_gain_c_per_duty carries the same degeneracy from the other side (+5% K gave
// +43%..+61% tau on the same traces, also RTI_OK), but unlike ambient it IS
// visible in the absolute residual (RMS 0.029 C -> 0.86 C at +5% K, a 30x
// signal). See docs/RAMP_TRANSIENT_IDENT_DESIGN.md's "Sensitivity to the two
// caller-supplied constants" for the proposed absolute-residual gate that would
// close the overstated-K hole, deliberately left unimplemented because its
// threshold must be calibrated against real telemetry noise, not guessed.
//
// current_tau_s/current_dead_time_s are the model's
// EXISTING parameters, used only for the improvement-over-null gate
// (RTI_NO_IMPROVEMENT) -- pass the same values adaptive_tune/zones_config
// would otherwise use. out must not be NULL; *out is always fully
// populated (result, refusal_reason, and whichever diagnostics were
// reached) even on refusal.
//
// Returns the same value as out->result.
rti_result_t rti_fit(const rti_sample_t *samples, uint32_t n, float k_gain_c_per_duty, float ambient_c,
                      float current_tau_s, float current_dead_time_s, rti_fit_t *out);

#ifdef __cplusplus
}
#endif
