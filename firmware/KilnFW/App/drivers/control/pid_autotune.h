// pid_autotune -- system identification and tuning-rule computation.
// TODO.md section 6A.4.
//
// Two independent identification methods live here, because they answer
// different questions: the open-loop step test fits a full FOPDT *model*
// (K, tau, L -- which the feedforward term, the ramp ceiling and the
// interaction matrix all need), while the relay-feedback (Astrom-Hagglund)
// test measures the single frequency-response point (Ku, Tu) that the
// classic oscillation-based rules are defined in terms of. The step test is
// the default; see pid_autotune_fit_relay()'s comment for why the relay test
// is deliberately not.
//
// Still single-band either way (no gain scheduling yet). This file is the *pure*
// identification/tuning math -- no FreeRTOS, no ESP-IDF, no I/O, same
// host-testability contract as pid.c/thermal_guard.c/heater_output.c, and
// for the same reason: this is "the only place the identification math can
// be validated exactly" per TODO.md 6A.8, since a host-side sim_plant.c run
// gives ground-truth K/tau/L to fit against.
//
// The caller (autotune_engine.c, on-target) is responsible for actually
// driving a duty step, sampling the temperature at a fixed rate, and
// handing the resulting trace to pid_autotune_fit_fopdt() once the plant
// has reached (or approached) a new steady state.
#ifndef PID_AUTOTUNE_H
#define PID_AUTOTUNE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUTOTUNE_RULE_SIMC = 0,   /* default -- see TODO.md 6A.4 for why ZN is wrong for a kiln */
    AUTOTUNE_RULE_ZIEGLER_NICHOLS,
    AUTOTUNE_RULE_TYREUS_LUYBEN,
    AUTOTUNE_RULE_COHEN_COON, /* appended, never inserted -- this enum crosses the HTTP boundary
                               * and value 0 is the documented default; see PID_EXPANSION_PLAN.md
                               * Phase 1. FOPDT-derivable like SIMC, but the more aggressive of the
                               * two -- see pid_autotune_tune_from_fopdt()'s header comment below. */
} autotune_rule_t;

/* One (time, temperature) sample of a step-test trace. t_s is elapsed time
 * since the step was applied (t_s == 0 at the step, not at trace start --
 * the caller records a pre-step baseline separately, see
 * pid_autotune_fit_fopdt()'s baseline_c parameter). */
typedef struct {
    float t_s;
    float measurement_c;
} autotune_sample_t;

/* First-order-plus-dead-time model fitted from a step response. */
typedef struct {
    float k_gain_c_per_duty; /* static gain: steady-state deltaT per unit duty step */
    float tau_s;             /* time constant */
    float dead_time_s;       /* L: transport delay before the response starts moving */
    bool  valid;             /* false if the trace didn't support a fit (see .c for reasons) */
    /* True only when the caller's own settle detector declared genuine
     * steady state before the trace was handed here -- this file has no way
     * to know that on its own, so it always defaults false and the caller
     * (autotune_engine.c) sets it explicitly right after a successful fit.
     * false means the trace was cut off by a time budget or an abort
     * instead: the two-point fit can still succeed on such a trace (nothing
     * here requires the *final* sample to be at steady state), but K/tau/L
     * from it are lower-confidence than one that genuinely settled -- see
     * autotune_engine.c's finalize_fit() for how that distinction is
     * surfaced. */
    bool  settled;
    /* 2026-09-02 round-3 review, item 4: the asymptote-extrapolation loop
     * (pid_autotune_fit_fopdt(), see its own comment) corrects
     * k_gain_c_per_duty from the trace's raw last-sample rise toward its
     * true steady-state value, and normally re-fits tau_s/dead_time_s from
     * that same corrected rise so all three stay mutually consistent. Two
     * things can go wrong with that, independently, and both are recorded
     * here rather than silently absorbed:
     *
     *   tau_consistent_with_gain: false means the loop had to stop because
     *   re-fitting tau_s/dead_time_s at the LAST accepted k_gain_c_per_duty
     *   failed (the implied 28.3%/63.2% crossing levels are past what the
     *   trace actually reached) -- tau_s/dead_time_s then reflect an
     *   EARLIER, less-corrected rise estimate than k_gain_c_per_duty does.
     *   A caller that cares about tau/L specifically (Cohen-Coon's L-heavy
     *   formula, the dead-time floor check) should treat them as stale
     *   when this is false; k_gain_c_per_duty itself is unaffected.
     *
     *   extrapolation_converged: false means MAX_EXTRAPOLATION_ITERATIONS
     *   was reached (or the loop stopped for a reason other than eps-
     *   convergence, e.g. the MAX_EXTRAPOLATION_RATIO ceiling) without the
     *   rise estimate settling to within EXTRAPOLATION_CONVERGE_EPS_C --
     *   k_gain_c_per_duty is still the best available estimate (the
     *   iteration ran, it just didn't provably stabilize), not a fit
     *   failure, but a caller surfacing confidence to an operator should
     *   not present it as equivalent to a converged one.
     *
     * Both read true in the common case, for two DIFFERENT reasons that a
     * caller does not need to distinguish (either one means "trust this
     * fit"): (1) no extrapolation loop ran at all -- the trace was too
     * short to compute an end-of-trace slope -- so there was nothing to be
     * inconsistent or unconverged about; or (2) the loop ran but its very
     * first candidate already showed no further correction was warranted
     * (the trace's tail was flat or trending back toward baseline, exactly
     * what a genuinely settled plant's own sensor-quantization noise looks
     * like) -- see pid_autotune_fit_fopdt()'s sign-check break for why this
     * is counted the same as (1), not as a failure to converge.
     *
     * RESIDUAL ACCURACY GAP (round-3 review judgement call, answered
     * explicitly rather than left silent -- and now WIRED, round-3
     * follow-up): measured at a 50%-of-asymptote truncation (right at the
     * MAX_EXTRAPOLATION_RATIO cap boundary -- see that constant's
     * comment), the iterated fit still recovers K only ~17.6% low. That is
     * the SAME low-K/over-driven-feedforward direction as the original
     * overshoot defect this whole pass exists to fix, at roughly a quarter
     * the magnitude -- NOT considered acceptable as a fully resolved case,
     * only a large improvement on the ~33% (single-pass) or effectively
     * unbounded (pre-fix) bias that preceded it.
     *
     * These two flags are now folded into autotune_engine_accept()'s
     * ack_unsettled gate alongside fopdt_model_t::settled (autotune_
     * engine.c/.h, "2026-09-01 review fix, extended 2026-09-02"): a fit
     * whose STEPPING detector genuinely fired but whose extrapolation
     * capped out (extrapolation_converged==false and/or tau_consistent_
     * with_gain==false) can still reach DONE, but acceptance -- and
     * finalize_fit()'s cross-gain coupling persist, gated the same way --
     * is refused until the operator explicitly acknowledges it, the same
     * as a max-duration-backstop fit. The 17.6% residual itself is
     * unchanged by this -- it is now surfaced and gate-able rather than
     * silent, not eliminated. Longer term, a proper least-squares
     * two-parameter (K, tau) fit would degrade more gracefully on
     * truncated data than this two-point method does at any iteration
     * count. */
    bool  tau_consistent_with_gain;
    bool  extrapolation_converged;
    char  invalid_reason[64];
    /* Diagnostic fit inputs/intermediates, added 2026-08-31 to make a run's
     * fit externally falsifiable (see autotune_engine.c's finalize_fit() and
     * dashboard_http.c's /api/autotune) -- a K that looks wrong could not
     * previously be traced to a specific baseline_c/final_c/rise_inf without
     * re-deriving them from the raw trace by hand. Zero (the invalid_model()
     * default) whenever !valid. */
    float baseline_c;  /* the baseline_c parameter this fit was called with */
    float final_c;     /* samples[sample_count-1].measurement_c */
    float raw_rise_c;  /* final_c - baseline_c, pid_autotune.c's raw_rise */
    float rise_inf_c;  /* extrapolated asymptote rise actually used for k_gain_c_per_duty */
} fopdt_model_t;

/* Fits a FOPDT model from a monotonic step-response trace using the
 * two-point method (28.3%/63.2% of the total rise), which is what TODO.md
 * 6A.4 specifies as the baseline (a full least-squares refinement is not
 * implemented in this pass -- the two-point method is the documented
 * fallback and is what's actually wired up).
 *
 * baseline_c is the steady-state measurement *before* the step (t_s < 0,
 * conceptually); duty_step is the duty change applied (e.g. 0.5 if duty
 * went from 0.0 to 0.5). samples must be sorted by t_s ascending and start
 * at or after the step (t_s >= 0). Returns a model with valid=false and a
 * reason string if the trace doesn't reach far enough into its rise to
 * locate both the 28.3% and 63.2% points, or if duty_step is ~0.
 */
fopdt_model_t pid_autotune_fit_fopdt(const autotune_sample_t *samples, int sample_count, float baseline_c,
                                     float duty_step);

/* SIMC/ZN/Tyreus-Luyben tuning rule outputs, in the same {kp,ki,kd} form
 * pid_cfg_t expects (Ki/Kd already absorb the rule's Ti/Td -- pid.c's
 * integral/derivative terms are Ki*integral and Kd*d_filtered, i.e.
 * "parallel form", not "Ti/Td series form", so this function does that
 * conversion once here rather than making every caller redo it). */
/* Why a call to pid_autotune_tune_from_fopdt()/pid_autotune_tune_from_relay()
 * produced all-zero gains -- see PID_EXPANSION_PLAN.md Phase 1, "Surface the
 * refusal in the UI." Every refusal path used to collapse to the same
 * kp=ki=kd=0, indistinguishable from every other refusal path; this enum
 * plus the reason string below (same convention as fopdt_model_t's
 * valid/invalid_reason pair above) let the caller tell an operator *why*,
 * with the actual offending numbers where they help. AUTOTUNE_REFUSAL_OK is
 * the zero value so a zero-initialized autotune_gains_t reads as "ok" only
 * by convention of also having non-zero gains; callers should check the
 * enum, not infer success from gains alone. */
typedef enum {
    AUTOTUNE_REFUSAL_OK = 0,
    AUTOTUNE_REFUSAL_INVALID_MODEL,       /* input model/fit was not valid (fopdt or relay) */
    AUTOTUNE_REFUSAL_RULE_NOT_ON_THIS_PATH, /* e.g. ZN/Tyreus-Luyben on the FOPDT path, or SIMC on the relay path */
    AUTOTUNE_REFUSAL_DEAD_TIME_TOO_SMALL, /* Cohen-Coon only: dead_time_s below AUTOTUNE_COHEN_COON_MIN_DEAD_TIME_S */
    AUTOTUNE_REFUSAL_NONPOSITIVE_TAU,     /* tau_s <= 0 */
    AUTOTUNE_REFUSAL_NONPOSITIVE_GAIN,    /* k_gain_c_per_duty <= 0, or Ku/Tu <= 0 on the relay path */
} autotune_refusal_t;

typedef struct {
    float kp;
    float ki;
    float kd;
    autotune_rule_t rule;
    autotune_refusal_t refusal;    /* AUTOTUNE_REFUSAL_OK on success */
    char  refusal_reason[96];      /* human-readable, with the specific numbers where they help; empty on success */
} autotune_gains_t;

/* lambda_s is only used by AUTOTUNE_RULE_SIMC; pass 0 to get the "default
 * lambda = 3*L, robust" behavior TODO.md 6A.4 specifies, or a positive
 * value (e.g. model.dead_time_s for "tight") to override it. Ignored for
 * the other three rules -- ZN/Tyreus-Luyben are not lambda-tunable by
 * definition, and Cohen-Coon has no lambda parameter at all (its formula is
 * fixed once {K, tau, L} are known).
 *
 * Two rules are derivable from a FOPDT model alone: SIMC (the default) and,
 * as of PID_EXPANSION_PLAN.md Phase 1, Cohen-Coon -- both take {K, tau, L}
 * and need nothing else. ZN and Tyreus-Luyben are defined here in their
 * *relay-test* form (from Ku/Tu, an ultimate gain and period), not derivable
 * from a FOPDT model alone without also assuming a relationship between
 * (K,tau,L) and (Ku,Tu) -- so pid_autotune_tune_from_relay() is the one that
 * actually accepts those two rules; calling pid_autotune_tune_from_fopdt()
 * with AUTOTUNE_RULE_ZIEGLER_NICHOLS or AUTOTUNE_RULE_TYREUS_LUYBEN returns
 * kp=ki=kd=0 and is a caller error. */
autotune_gains_t pid_autotune_tune_from_fopdt(const fopdt_model_t *model, autotune_rule_t rule, float lambda_s);

/* ------------------------------------------------------------------------
 * Relay-feedback (Astrom-Hagglund) identification -- TODO.md 6A.4.
 *
 * The other half of 6A.4's identification story. Where the step test fits a
 * *model* (K, tau, L) from one monotonic climb, the relay test measures the
 * plant's behaviour at exactly one frequency: the one where the loop phase
 * is -180 degrees. That single point (Ku, Tu) is all the classic
 * oscillation-based rules (ZN, Tyreus-Luyben) need, and it is what Marlin's
 * M303 and Klipper's PID_CALIBRATE report.
 *
 * The price is that getting it requires deliberately driving the chamber
 * into a sustained oscillation for several cycles, and on a kiln one cycle
 * can be ten minutes or more. TODO.md is blunt about this being slow,
 * thermally abusive, and the last thing anyone wants at cone temperature --
 * which is why the step test stays the default path and this exists as the
 * opt-in alternative, not a replacement.
 *
 * This file implements only the *math*: given a trace recorded while
 * something else was bang-banging the relay, recover Ku and Tu. Actually
 * driving the relay is the on-target engine's job.
 * ------------------------------------------------------------------------ */

/* Result of a relay-feedback identification.
 *
 * amplitude_c is the oscillation *amplitude* -- half of peak-to-peak, not
 * peak-to-peak. This distinction is the single easiest way to get Ku wrong
 * by a factor of two, so it is worth being explicit: the describing-function
 * derivation of Ku = 4d / (pi * sqrt(a^2 - h^2)) models the plant output as
 * a sinusoid a*sin(wt), whose *amplitude* is a. TODO.md 6A.4 writes "the
 * sustained oscillation's peak-to-peak amplitude a", which reads the other
 * way; the formula it then quotes is the standard one and only balances with
 * a = peak-to-peak/2, so that is what is used and reported here. The same
 * "half" convention applies to hysteresis_c below, and it must, because the
 * two are subtracted under the same square root. */
typedef struct {
    float ku;            /* ultimate gain, duty per degC (pairs with pid.c's error-in-degC input) */
    float tu_s;          /* ultimate (oscillation) period */
    float amplitude_c;   /* a: HALF of peak-to-peak, averaged over the cycles actually used */
    int   cycles_used;   /* how many trailing complete cycles the averages came from */
    bool  valid;         /* false if the trace never settled into a usable limit cycle */
    char  invalid_reason[64];
} relay_model_t;

/* Minimum number of complete cycles the trace must contain before a fit is
 * even attempted, and the maximum number of trailing cycles averaged. The
 * first cycles after the relay starts are a transient converging *towards*
 * the limit cycle, not the limit cycle itself, so they are discarded: their
 * amplitude and period are both biased by wherever the plant happened to
 * start. Averaging the last few instead of just the very last one buys noise
 * rejection without reintroducing that bias. */
#define AUTOTUNE_RELAY_MIN_CYCLES 3
#define AUTOTUNE_RELAY_FIT_CYCLES 3

/* Identifies (Ku, Tu) from a trace recorded under relay control.
 *
 * samples/sample_count follow exactly the same contract as
 * pid_autotune_fit_fopdt(): sorted ascending by t_s, with t_s the elapsed
 * time since the relay test began. The per-sample timestamp is what is used
 * for the period, so a separate "sample period" argument would be redundant
 * (and a second, disagreeing source of truth for the same quantity); an
 * exactly-uniform sample rate is not required.
 *
 * relay_amplitude_duty is d, the relay's HALF-amplitude in duty units: a
 * relay swinging between duty 0.2 and 0.8 has d = 0.3, not 0.6. Same "half"
 * convention as `a` above, for the same reason -- the describing function is
 * of a symmetric square wave of amplitude d.
 *
 * hysteresis_c is h, the HALF-width of the switching band in degC: a
 * controller that turns the heat on below setpoint-2 and off above
 * setpoint+2 has h = 2. The sqrt(a^2 - h^2) correction is not optional
 * decoration -- with a mechanical relay, hysteresis is not optional either
 * (see docs/HARDWARE.md: EE2-12NUH electromechanical relays, commanded over
 * I2C), and ignoring it inflates Ku, which is precisely the direction that
 * produces a too-aggressive tuning.
 *
 * Returns valid=false with a reason string, and never a NaN, if:
 *   - the trace contains fewer than AUTOTUNE_RELAY_MIN_CYCLES complete
 *     cycles (it never oscillated, or was cut short);
 *   - the trailing cycles disagree too much in period or amplitude, i.e. the
 *     plant was still converging (or drifting) rather than sitting in a
 *     limit cycle -- an un-settled trace is rejected, never fitted;
 *   - a <= h, which makes the square root imaginary. This is a real,
 *     reachable condition rather than a theoretical one: an oscillation no
 *     bigger than the switching band means the relay is merely chattering
 *     inside its own hysteresis and the measurement carries no information
 *     about the plant at all.
 */
relay_model_t pid_autotune_fit_relay(const autotune_sample_t *samples, int sample_count,
                                     float relay_amplitude_duty, float hysteresis_c);

/* Tuning rules that need an ultimate gain/period, i.e. the ones the FOPDT
 * path rejects. AUTOTUNE_RULE_SIMC is a model-based rule and has no
 * definition in terms of (Ku, Tu) alone, so asking for it here returns
 * kp=ki=kd=0 -- the mirror image of asking for ZN from the FOPDT path.
 *
 * WARNING, and it is the whole reason AUTOTUNE_RULE_SIMC is the enum's zero
 * value: Ziegler-Nichols targets roughly quarter-amplitude decay, i.e. it is
 * *designed* to leave the loop oscillating. On a 3D-printer hotend that
 * costs a few degrees of ripple. On a kiln at 1200 degC it costs the firing
 * and thermally cycles the elements. Neither rule here may ever become a
 * default anywhere -- they are offered because a user who knows what they
 * are asking for may want to reproduce a Marlin-style number, and
 * Tyreus-Luyben (much more conservative, roughly half ZN's gain with a far
 * longer integral time) is the one to reach for if either is used at all. */
autotune_gains_t pid_autotune_tune_from_relay(const relay_model_t *model, autotune_rule_t rule);

/* Ramp-ceiling estimate TODO.md 6A.4 says autotune should replace the
 * user-entered guess with, in degC/hour (matching max_ramp_c_per_hr's
 * existing unit in zone_cfg_t): approx (K*u_max - (T_now - T_ambient)) / tau,
 * converted from °C/s to °C/hr. Returns 0 if the model can't support the
 * estimate (tau <= 0). */
float pid_autotune_estimate_max_ramp_c_per_hr(const fopdt_model_t *model, float u_max, float t_now_c,
                                              float t_ambient_c);

/* ------------------------------------------------------------------------
 * Relative Gain Array (Bristol, 1966) -- TODO.md 6A.5(c).
 *
 * The RGA answers one question and answers it as a number: *is it legitimate
 * to run one independent PID loop per zone at all?* Given the steady-state
 * cross-gain matrix K (K[i][j] = zone j's degC-per-unit-duty response to a
 * duty step on zone i -- exactly what 6A.5(b)'s autotune_coupling_matrix_t
 * accumulates), the RGA is
 *
 *     Lambda = K .* (K^-1)^T          (elementwise/Hadamard, not matmul)
 *
 * and each element Lambda[i][j] is the ratio of "gain from zone i's heat to
 * zone j's temperature with every OTHER loop open" to "the same gain with
 * every other loop closed and holding its own zone". Lambda[i][i] == 1
 * therefore means the other loops make no difference to zone i -- the loops
 * do not interact and per-zone PID is exactly right. The further from 1,
 * the more each loop's corrections are being undone or amplified by its
 * neighbours' loops; negative means closing the other loops *reverses* the
 * sign of this loop's own gain, which is the pathological case where
 * decentralized control actively destabilizes itself.
 *
 * Rows and columns of Lambda each sum to 1 by construction, for any K --
 * which is also the cheapest available self-check on the arithmetic.
 *
 * Everything here is pure: no allocation, caller-owned structs, no logging,
 * no ESP-IDF, same host-testability contract as the rest of this file. It
 * is the *only* part of the interaction story that can be validated exactly
 * off-target, and at time of writing it has never seen real hardware data
 * (see autotune_engine.h: no cross-gain cell has ever been filled on the
 * bench unit because no thermocouples are attached, so every on-target run
 * aborts on guard 6 long before finalize_fit()).
 * ------------------------------------------------------------------------ */

/* Upper bound on the square matrix this module will invert. Sized for
 * MAX31856_CHANNEL_COUNT (3) with one slot of headroom; the closed-form
 * cofactor inverse below only covers n=2 and n=3, so anything larger is
 * refused rather than silently mis-handled. This header stays free of
 * MAX31856.h on purpose -- the math must not depend on the sensor driver. */
#define AUTOTUNE_RGA_MAX_ZONES 4

/* Relative size a determinant must exceed, measured against the only
 * scale-free yardstick available for an n x n matrix: max|K[i][j]|^n, since
 * det scales as the n-th power of the entries. A bare `det == 0` test is
 * useless here -- K's entries come from a noisy curve fit and land on exact
 * zero essentially never, so a matrix that is singular *to within the
 * measurement* would sail through and produce enormous, entirely fictional
 * Lambda values.
 *
 * 1e-4 corresponds roughly to |Lambda| reaching 1e4, i.e. far past any
 * reading a potter (or a decoupler) could act on. Between "meaningful" and
 * that ceiling the values are reported as-is and left for the caller to
 * flag as severe -- refusal is reserved for the range where the answer is
 * numerically untrustworthy, not merely alarming. */
#define AUTOTUNE_RGA_SINGULAR_REL_EPS 1e-4f

typedef enum {
    AUTOTUNE_RGA_OK = 0,
    AUTOTUNE_RGA_ERR_TOO_FEW_ZONES,   /* n < 2: interaction between one zone and itself is not a thing */
    AUTOTUNE_RGA_ERR_INCOMPLETE,      /* no 2x2-or-larger block of K is fully measured yet */
    AUTOTUNE_RGA_ERR_SINGULAR,        /* |det| below the scale-aware floor above, or a non-finite entry */
    AUTOTUNE_RGA_ERR_UNSUPPORTED,     /* n > AUTOTUNE_RGA_MAX_ZONES, or the sub-block exceeded 3x3 */
} autotune_rga_status_t;

/* On success, `lambda` is the n x n RGA of the *sub-block* of K that was
 * actually measured -- NOT of the full zone_count x zone_count matrix.
 * zone_index[] maps each row/column of `lambda` back to the caller's own
 * zone numbering, and must be used when labelling the result: for a 3-zone
 * kiln with only zones 0 and 2 tuned, n is 2 and lambda[0][0] is zone 0's.
 *
 * Padding the missing rows with zeros instead would be far easier and
 * completely wrong: a zero row makes K singular (det 0), and if it were
 * nudged to something invertible the resulting Lambda would describe an
 * imaginary kiln in which an untested zone is perfectly decoupled from
 * everything -- the exact false reassurance this whole feature exists to
 * avoid giving. */
typedef struct {
    int     n;                                                       /* size of the sub-block used, >= 2 on success */
    uint8_t zone_index[AUTOTUNE_RGA_MAX_ZONES];                      /* lambda row/col -> caller's zone index */
    float   lambda[AUTOTUNE_RGA_MAX_ZONES][AUTOTUNE_RGA_MAX_ZONES];  /* the RGA itself */
    float   determinant;                                             /* det of the sub-block, for diagnosis */
    bool    valid;
    autotune_rga_status_t status;
    char    invalid_reason[80];
} autotune_rga_t;

/* Computes the RGA of the largest fully-measured principal sub-block of K.
 *
 * k and k_valid are row-major n x n arrays (k[i * n + j] is K[i][j]);
 * k_valid[i * n + j] == false means that cell has never been measured.
 * n is the caller's full zone count, <= AUTOTUNE_RGA_MAX_ZONES.
 *
 * "Principal sub-block" is the load-bearing phrase: a subset S of zones is
 * usable only if *every* cell K[a][b] with a and b both in S is measured --
 * a zone with a complete row but a hole in its column contributes an
 * unknown to the inverse and so cannot be included. The largest such S is
 * chosen (lowest indices winning ties), because a 2x2 answer over the zones
 * that have actually been tuned is real, while a 3x3 answer over a matrix
 * with a hole in it is fiction.
 *
 * Never returns NaN or Inf in `lambda`. Refuses -- valid=false, a status
 * code, and a human-readable reason -- rather than approximating, because a
 * wrong RGA reads exactly like a right one: it would tell an operator their
 * zones are independent when they are not, and that mistake surfaces later
 * as a firing that will not hold a cone. */
autotune_rga_t pid_autotune_rga(const float *k, const bool *k_valid, int n);

#ifdef __cplusplus
}
#endif

#endif // PID_AUTOTUNE_H
