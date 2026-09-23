// autotune_engine -- on-target driver for pid_autotune.h's identification
// math. TODO.md section 6A.4.
//
// Two methods, sharing one state machine, one guard suite, one relay
// authority claim and one trace buffer:
//   - AUTOTUNE_METHOD_STEP: the open-loop step test (settle at duty 0, apply
//     a fixed duty, fit a FOPDT model). The default, and the one TODO.md
//     6A.4 recommends running first.
//   - AUTOTUNE_METHOD_RELAY: relay-feedback (Astrom-Hagglund) -- bang-bang
//     the zone around an operator-supplied setpoint and recover (Ku, Tu)
//     from the resulting limit cycle. Added 2026-08-12. Opt-in, never a
//     default, because it works by *deliberately oscillating the chamber at
//     temperature*: see the AUTOTUNE_RELAY_* constants below and
//     pid_autotune.h's own warning about what ZN in particular does to a
//     kiln.
//
// Neither method has ever produced a fit on real hardware: the bench unit
// has no thermocouples attached, so every on-target run to date aborts on
// guard 6 (sensor invalid) within seconds. The relay path in particular is
// wholly untested against a real kiln -- the math beneath it is host-tested
// against sim_plant.c, the state machine driving it is not.
//
// NOT built in this pass:
//   - Gain-scheduling bands (multiple {Kp,Ki,Kd,K,tau,L} per zone).
//   - Cross-zone coupling logging during the run (6A.5's item (b)).
//   - Persisting the trace to flash -- it lives in RAM only, for the
//     duration of one run plus its DONE/ABORTED result, and is lost on
//     reboot. CSV download while it's live is what's built.
//
// Owns one zone's relay(s) while running, through the same relay_authority
// chokepoint profile_executor.c uses, and refuses to start (or is refused
// by profile_executor_run()) while the other is active -- see .c for the
// mutual-exclusion check.
//
// TODO.md 6A.5(b), 2026-08-11: while STEPPING, every configured zone's
// temperature is sampled each period (not just the zone under test), and on
// a successful fit, every OTHER zone's trace is also fit against the SAME
// duty step to yield that pair's {K,tau,L} -- one row of the cross-zone
// coupling matrix per completed run, kept in RAM only (same lifetime as the
// engine's own trace/model; lost on reboot, not persisted to NVS).
#ifndef AUTOTUNE_ENGINE_H
#define AUTOTUNE_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "MAX31856.h"
#include "kiln_io.h"
#include "pid_autotune.h"
#include "safety_link.h"
#include "thermal_guard.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUTOTUNE_ENGINE_IDLE = 0,
    AUTOTUNE_ENGINE_SETTLING,  /* step method: holding baseline_duty, waiting for the pre-step baseline to be trustworthy */
    AUTOTUNE_ENGINE_STEPPING,  /* step method: step applied, recording the trace */
    /* Relay method. The two phases are separated because only the second one
     * produces data: the approach is an ordinary heat-up under the same relay
     * law, and folding it into the recorded trace would put a long monotonic
     * ramp in front of the oscillation -- see the .c for why that breaks the
     * fit rather than merely padding it. */
    AUTOTUNE_ENGINE_RELAY_APPROACH, /* relay law running, driving to the setpoint, not yet recording */
    AUTOTUNE_ENGINE_RELAY_CYCLING,  /* limit-cycling around the setpoint, recording the trace */
    AUTOTUNE_ENGINE_DONE,      /* fit succeeded -- gains proposed, awaiting autotune_accept()/autotune_abort() */
    AUTOTUNE_ENGINE_ABORTED,   /* guard trip, time budget exceeded without a usable fit, or manual abort */
} autotune_engine_state_t;

/* Which identification method a run is using. STEP is 0 so a zeroed status
 * struct (and a caller that never asks) reads as the safe default. */
typedef enum {
    AUTOTUNE_METHOD_STEP = 0,
    AUTOTUNE_METHOD_RELAY,
} autotune_method_t;

typedef struct {
    autotune_engine_state_t state;
    autotune_method_t method;
    uint8_t  zone_index;
    uint32_t elapsed_s;         /* time in the current phase */
    uint16_t sample_count;
    float    actual_c;
    bool     actual_valid;
    float    duty;
    char     abort_reason[96];  /* only meaningful when state == ABORTED */

    /* Relay method only: the run's parameters, echoed back so the UI never
     * has to remember what it asked for (and so a page loaded mid-run can
     * still say what is being done to the kiln), plus live progress. */
    float    relay_setpoint_c;
    float    relay_amplitude_duty;  /* d, half-amplitude in duty units */
    float    relay_hysteresis_c;    /* h, HALF-width of the switching band */
    uint16_t relay_cycles_seen;     /* complete cycles recorded so far */
    uint16_t relay_cycles_target;   /* how many are needed before a fit is attempted */

    /* Target-temperature step mode (autotune_engine_run_to_target()), STEP
     * method only. Meaningful in every state, not just DONE -- see
     * autotune_engine_get_status()'s own comment -- so a caller can show
     * which of the two phases is running and, once probe_k_rough is
     * nonzero, why a particular identification duty was chosen. */
    bool     target_mode;    /* true: this run was started via _run_to_target(), not _run() */
    bool     probe_phase;    /* true while PHASE 1 (the low-duty probe) is in progress */
    float    target_c;       /* the requested (or defaulted) target temperature */
    float    probe_k_rough;  /* K estimated from the probe fit; 0 until the probe completes */
    /* What the identification step's OWN fitted model says it actually
     * asymptotes to (baseline_c + k_gain_c_per_duty * step_duty), NOT
     * target_c -- 0 until finalize_fit() has run on a valid model. Can
     * legitimately differ from target_c: duty is computed from the PROBE's
     * baseline, but the identification step re-baselines after only a
     * short re-settle from a zone still cooling from the probe, biasing
     * toward overshoot. See autotune_engine.c's AUTOTUNE_TARGET_ACHIEVED_
     * WARN_C for the full reasoning; this field is what lets a caller show
     * the operator the real landing point instead of the requested one. */
    float    target_achieved_c;

    /* Cold-junction ambient reference captured at the SETTLING->STEPPING
     * transition (autotune_engine.c ~line 2102), used by finalize_fit()'s
     * physical-plausibility check. Exposed here, next to model.baseline_c,
     * because the two have been conflated in past analysis despite being
     * different quantities: step_ambient_c is a cold-junction reading taken
     * once at step-start, baseline_c (fopdt_model_t) is the zone's own
     * settled temperature reading fed into the fit. Meaningful whenever a
     * step (not relay) run has reached STEPPING at least once; 0 before
     * that, same convention as target_achieved_c above. */
    float    step_ambient_c;

    /* Only meaningful when state == DONE.
     *
     * Exactly one of model.valid / relay.valid is ever true, and which one
     * depends on `method`: a step test yields a FOPDT model, a relay test
     * yields a single frequency-response point and no model at all. See
     * autotune_engine_accept() for why that difference reaches all the way
     * out to what gets written to NVS. */
    fopdt_model_t    model;            /* step method only */
    relay_model_t    relay;            /* relay method only */
    autotune_gains_t proposed_gains;   /* .rule says which rule produced these */
    float            predicted_max_ramp_c_per_hr; /* step method only -- needs tau, which a relay test never measures.
                                                    * Evaluated at t_now_c = end-of-step temperature (the hottest
                                                    * point this run actually reached), so it is the SMALLEST
                                                    * climb rate available from that point on, not a
                                                    * temperature-independent ceiling. Kept for UI/status display
                                                    * only -- see predicted_max_ramp_ambient_c_per_hr below for the
                                                    * value that is actually safe to adopt as a hard ceiling. */
    float            predicted_max_ramp_ambient_c_per_hr; /* step method only. Same estimator
                                                    * (pid_autotune_estimate_max_ramp_c_per_hr()) evaluated at
                                                    * t_now_c == t_ambient_c, i.e. the (T_now - T_ambient) term
                                                    * drops out and this reduces to K*u_max/tau*3600 -- the rate
                                                    * the plant can sustain from a cold start, which is what a
                                                    * caller checking "can this profile ever start" (profile_
                                                    * executor_run.c, profile_feasibility.c, profiles_edit_http.c)
                                                    * actually needs. This is the field autotune_engine_accept()
                                                    * (with opts->adopt_ceiling true) adopts into
                                                    * zones_config_set_max_ramp(), never the end-of-step field
                                                    * above. */
} autotune_engine_status_t;

#define AUTOTUNE_ENGINE_TICK_MS 1000u        /* same 1Hz as profile_executor */
/* Trace resolution. Raised 5s -> 10s on 2026-08-12 as half of the DRAM fix
 * described below: a kiln's time constant is on the order of 10,000 s, so
 * 10 s sampling loses nothing a FOPDT fit can see, and it halves the trace. */
#define AUTOTUNE_ENGINE_SAMPLE_PERIOD_S 10u
#define AUTOTUNE_ENGINE_SETTLE_S 180u         /* baseline hold before stepping */
#define AUTOTUNE_ENGINE_DEFAULT_MAX_DURATION_S (4u * 3600u) /* TODO.md 6A.4's 4h budget */
#define AUTOTUNE_ENGINE_MAX_SAMPLES 1440u     /* 4h at 10s/sample */

/* ---------------------------------------------------------------------------
 * Relay-feedback defaults. Every one of these is picked for a KILN -- a plant
 * whose limit-cycle period is ten minutes or more, not the seconds a hotend
 * takes -- and each is justified where it stands. None has been checked
 * against a real firing; they are reasoned starting points, not measurements.
 * ------------------------------------------------------------------------- */

/* The relay swings duty between CENTER - d and CENTER + d. Centring on 0.5 is
 * not arbitrary: the describing function Ku = 4d/(pi*sqrt(a^2-h^2)) assumes a
 * *symmetric* square wave, so both branches must land inside [0, 1] without
 * being clamped -- a clamped branch silently makes the real d smaller than the
 * d handed to the fit, which is the same factor-of-two class of error
 * pid_autotune.h warns about. 0.5 is the only centre that allows the largest
 * possible d, and it needs no prior knowledge of the duty required to hold the
 * setpoint, which is precisely the thing we have not identified yet. */
#define AUTOTUNE_RELAY_CENTER_DUTY 0.5f

/* d = 0.35 -> the relay alternates between duty 0.15 and 0.85.
 *
 * A limit cycle exists only if the duty that would hold the setpoint lies
 * strictly *between* the two branches; otherwise the low branch still heats
 * (temperature never comes back down) or the high branch never catches up.
 * 0.35 therefore covers any zone whose hold-duty is between 15% and 85%,
 * which is a wide net for a kiln below its ceiling. Going further (d -> 0.5,
 * i.e. full-off/full-on, what Marlin does) buys nothing but a larger
 * oscillation amplitude: more overshoot, more thermal abuse of the ware and
 * the elements, and more headroom eaten under the guard-5 ceiling. Going much
 * smaller shrinks the amplitude `a` toward the hysteresis band h, and
 * pid_autotune_fit_relay() correctly refuses a <= h. */
#define AUTOTUNE_RELAY_DEFAULT_D 0.35f
#define AUTOTUNE_RELAY_MAX_D 0.5f       /* above this a branch would clamp -- see CENTER_DUTY */

/* h = 2.0 degC HALF-width, i.e. a 4 degC switching band.
 *
 * Two floors set this from below: type-K noise through the MAX31856 is a few
 * tenths of a degree, and the trace is stored quantised to 0.1 degC, so a band
 * much tighter than a degree would have the relay switching on noise instead
 * of on the plant. From above, h subtracts under a square root against the
 * oscillation amplitude, so a large h both demands a large (more abusive)
 * oscillation and makes Ku ill-conditioned. 2.0 sits above the noise while
 * staying small compared with the overshoot a kiln's dead time produces
 * anyway -- on a plant with this much lag, `a` is set mostly by the dead time,
 * not by h. */
#define AUTOTUNE_RELAY_DEFAULT_H_C 2.0f
#define AUTOTUNE_RELAY_MIN_H_C 0.5f     /* below this the relay would chatter on sensor noise */
#define AUTOTUNE_RELAY_MAX_H_C 20.0f    /* above this the test is abusing the kiln, not measuring it */

/* Cycles recorded before a fit is attempted.
 *
 * pid_autotune_fit_relay() fits the trailing AUTOTUNE_RELAY_FIT_CYCLES (3) and
 * requires at least AUTOTUNE_RELAY_MIN_CYCLES (3), discarding nothing on its
 * own -- but the first cycles after the relay starts are the transient
 * *converging toward* the limit cycle, biased by wherever the plant happened
 * to be. 5 = 3 fitted + 2 thrown away as transient. Each extra cycle costs
 * ten-plus minutes of a kiln oscillating at temperature, so this is a
 * deliberate floor rather than "collect as much as the budget allows". */
#define AUTOTUNE_RELAY_TARGET_CYCLES 5u

/* Guard 5 trips on an absolute ceiling, and a relay test is *designed* to
 * overshoot its band -- by however much the dead time allows, which is exactly
 * the quantity being measured and therefore unknown before the test. Refusing
 * a setpoint within 50 degC of the zone's max_temp_c keeps the test from being
 * built to trip its own guard. The same margin is required above min_temp_c:
 * an oscillation that undershoots into the floor limit aborts just as hard. */
#define AUTOTUNE_RELAY_SETPOINT_HEADROOM_C 50.0f

/* Two separate budgets, because the two phases fail differently. If the zone
 * cannot reach the setpoint within the approach budget, the setpoint is out of
 * reach (or an element has failed) and continuing just holds a kiln at high
 * duty. If it reached the setpoint but has not produced
 * AUTOTUNE_RELAY_TARGET_CYCLES cycles within the cycling budget, the period is
 * longer than this test can afford. 4h each mirrors the step test's own
 * TODO.md 6A.4 budget; the worst case is therefore ~8h of unattended-if-you-
 * let-it heat, which is why the UI says to attend the run. */
#define AUTOTUNE_RELAY_APPROACH_MAX_S (4u * 3600u)
#define AUTOTUNE_RELAY_CYCLE_MAX_S (4u * 3600u)

/* Packed per-sample trace storage, one row per channel. The unpacked
 * {float t_s; float measurement_c} form (autotune_sample_t, still what
 * autotune_engine_get_trace() hands back) cost 8 bytes a sample, i.e.
 * 3 x 2880 x 8 = 69 KB of .bss for a board with ~196 KB of DRAM total. That,
 * plus the executor's history buffer, is why the board was found on
 * 2026-08-12 booting with 7 KB of free heap, failing to create tasks and
 * unable to serve HTTP. Packing to 4 bytes and halving the sample rate takes
 * this to ~17 KB.
 *
 *   t        implicit: sample i is at i * AUTOTUNE_ENGINE_SAMPLE_PERIOD_S
 *   temp     0.1 degC in an i16, AUTOTUNE_TRACE_TEMP_INVALID (INT16_MIN)
 *            for "no valid reading at this sample" */
#define AUTOTUNE_TRACE_TEMP_INVALID INT16_MIN
typedef int16_t autotune_trace_sample_t;

/* Non-fatal bring-up, same convention as every other driver's *_start(). */
esp_err_t autotune_engine_start(kiln_io_t *io_or_null, MAX31856BusClass *thermo_bus_or_null,
                                SafetyLinkClass *safety_or_null);

/* Starts a step test on zone_index at the given step_duty (applied on top of
 * a 0.0 baseline -- TODO.md 6A.4's "from steady state, apply a fixed duty
 * step"; baseline_duty other than 0 is not exposed in this pass, matching
 * "one monotonic climb" being the interesting case for a kiln that starts
 * cold). Refuses (false, err_msg filled) if: zone_index has no relay mask
 * configured, step_duty is out of (0, 1], profile_executor has a run active
 * (RUNNING/PAUSED, any zone -- TODO.md 6A.4: "cannot run while a profile
 * runs"), or this engine is already SETTLING/STEPPING.
 *
 * rule selects the tuning rule finalize_fit() applies to the fitted FOPDT
 * model once the step completes: AUTOTUNE_RULE_SIMC (the default -- pass it
 * explicitly, there is no "0 means default" magic here) or
 * AUTOTUNE_RULE_COHEN_COON, opt-in only, see PID_EXPANSION_PLAN.md Phase 1 /
 * pid_autotune.h's header comment on why it must never become the default on
 * a kiln. AUTOTUNE_RULE_ZIEGLER_NICHOLS and AUTOTUNE_RULE_TYREUS_LUYBEN are
 * relay-only (they need Ku/Tu, not a FOPDT model) and are refused here with
 * err_msg filled, the mirror image of autotune_engine_run_relay() refusing
 * AUTOTUNE_RULE_SIMC. */
bool autotune_engine_run(uint8_t zone_index, float step_duty, autotune_rule_t rule, char *err_msg, size_t err_cap);

/* Starts a TARGET-TEMPERATURE step test on zone_index: instead of the
 * operator guessing a duty and discovering where the response asymptotes
 * (autotune_engine_run() above), this picks the duty itself so the response
 * asymptotes at (approximately) target_c.
 *
 * Two phases, both visible through autotune_engine_get_status()'s
 * target_mode/probe_phase/probe_k_rough fields:
 *   PHASE 1 (probe): a short, conservative-duty step (AUTOTUNE_ENGINE_
 *     PROBE_DUTY for AUTOTUNE_ENGINE_PROBE_DURATION_S -- see their comments
 *     in the .c for the tradeoff) to get a rough gain estimate, fit with the
 *     SAME pid_autotune_fit_fopdt() finalize_fit() uses -- no second
 *     estimator.
 *   PHASE 2 (identify): duty = (target_c - baseline_c) / K_rough, clamped to
 *     (0, 1]; refused, not silently clamped, if that ratio exceeds 1.0 (the
 *     zone cannot reach target_c at full duty) -- the abort names the
 *     target and the highest temperature the rough fit says the zone CAN
 *     reach. The real identification step then runs exactly as
 *     autotune_engine_run() would, through the same finalize_fit() guards
 *     (ceiling headroom, physical plausibility, minimum excursion).
 *
 * target_c <= 0 means "use the default": AUTOTUNE_ENGINE_TARGET_DEFAULT_
 * FRACTION (75%) of the zone's configured max_temp_c. ZERO-SEMANTICS TRAP:
 * max_temp_c == 0 means that zone's ceiling guard is DISABLED, not "the
 * ceiling is 0C" -- in that case there is no usable maximum to take 75% of,
 * and this refuses (err_msg filled) rather than computing 0.75 * 0 or
 * inventing a fallback ceiling of its own. An explicit target_c is still
 * accepted with no ceiling configured (nothing to validate it against).
 *
 * Whether given explicitly or defaulted, target_c is validated against the
 * zone's max_temp_c ceiling (when configured) WITH MARGIN
 * (AUTOTUNE_ENGINE_TARGET_CEILING_MARGIN_C) and refused up front if too
 * close -- BEFORE begin_run_locked() is even called, i.e. before any
 * heating starts, same as every other refusal in this function. This is in
 * addition to, not instead of, finalize_fit()'s own ceiling-headroom check
 * on the identification step's ACTUAL fitted rise once PHASE 2 completes.
 *
 * rule is restricted exactly as autotune_engine_run()'s is (SIMC or
 * Cohen-Coon -- see that function's doc comment); the same relay-only
 * refusal applies. Every other refusal condition (no relay mask, a profile
 * active on the zone, a test already running, heat blocked, OTA/current-
 * sweep interlocks) is identical too, since both funnel through the same
 * begin_run_locked(). */
bool autotune_engine_run_to_target(uint8_t zone_index, float target_c, autotune_rule_t rule, char *err_msg,
                                   size_t err_cap);

/* Starts a relay-feedback (Astrom-Hagglund) test on zone_index, oscillating
 * the zone around setpoint_c.
 *
 * setpoint_c is NOT optional and has no default. TODO.md 6A.4's whole
 * argument for the method is that it identifies the plant *at the temperature
 * it will actually run at*; a relay test at an arbitrary temperature measures
 * a kiln nobody fires. It is validated against the zone's own guard limits
 * (AUTOTUNE_RELAY_SETPOINT_HEADROOM_C either side) and refused if the guards
 * would trip on it, rather than started and allowed to abort later.
 *
 * relay_d and hysteresis_c accept <= 0 meaning "use the documented default"
 * (AUTOTUNE_RELAY_DEFAULT_D / AUTOTUNE_RELAY_DEFAULT_H_C); otherwise they are
 * range-checked against the MIN/MAX constants above. Both follow
 * pid_autotune.h's HALF-amplitude convention -- d is half the duty swing, h is
 * half the switching band's width.
 *
 * rule must be one of the two rules defined in terms of (Ku, Tu):
 * AUTOTUNE_RULE_TYREUS_LUYBEN (recommended, roughly half ZN's gain) or
 * AUTOTUNE_RULE_ZIEGLER_NICHOLS. AUTOTUNE_RULE_SIMC is refused here -- it is a
 * model-based rule and a relay test produces no model, so accepting it would
 * silently propose kp=ki=kd=0 (see pid_autotune_tune_from_relay()).
 *
 * Refuses under the same conditions autotune_engine_run() does (no relay mask,
 * a profile active on the zone, a test already running), plus the parameter
 * and setpoint checks above. */
bool autotune_engine_run_relay(uint8_t zone_index, float setpoint_c, float relay_d, float hysteresis_c,
                               autotune_rule_t rule, char *err_msg, size_t err_cap);

/* Abortable at any time (TODO.md 6A.4). Drops the relay, keeps the partial
 * trace's sample_count/state for diagnosis, does not write any tuning. A
 * no-op from IDLE/DONE/ABORTED. */
void autotune_engine_abort(const char *reason);

/* Options for autotune_engine_accept() below. A zeroed struct (or opts ==
 * NULL) means the original default behavior: refuse an unsettled fit,
 * never touch the ramp ceiling. */
typedef struct {
    bool ack_unsettled;  /* see autotune_engine_accept()'s doc comment on the three fit-confidence signals */
    bool adopt_ceiling;  /* see autotune_engine_accept()'s doc comment on ceiling adoption */
} autotune_accept_opts_t;

/* Outcome of the ceiling-adoption attempt inside autotune_engine_accept()
 * below, reported back to the caller instead of only to the log (review
 * finding: an HTTP/UART caller had no way to tell "adopted" from "silently
 * skipped" from "silently clamped"). Never a clamp: an out-of-range request
 * is REJECTED_OUT_OF_RANGE, not adopted at a clamped value. */
typedef enum {
    AUTOTUNE_CEILING_ADOPTED = 0,
    AUTOTUNE_CEILING_SKIPPED_NOT_REQUESTED,   /* adopt_ceiling was false */
    AUTOTUNE_CEILING_SKIPPED_RELAY_METHOD,    /* relay test has no FOPDT model to derive a ceiling from */
    AUTOTUNE_CEILING_SKIPPED_MODEL_NOT_PERSISTED, /* zones_config_set_model() failed for this run */
    AUTOTUNE_CEILING_SKIPPED_ZERO,            /* predicted_max_ramp_ambient_c_per_hr <= 0 -- nothing to adopt */
    AUTOTUNE_CEILING_SKIPPED_WOULD_TIGHTEN,   /* stored ceiling is nonzero and looser than the new estimate --
                                                * adoption never auto-tightens */
    AUTOTUNE_CEILING_REJECTED_OUT_OF_RANGE,   /* new estimate > ZONE_MAX_RAMP_C_PER_HR_MAX -- refused, not clamped */
    AUTOTUNE_CEILING_SKIPPED_READ_FAILED,     /* zones_config_get_max_ramp() failed -- never adopt over a value
                                                * we could not read, even though that reads the same as "no old
                                                * value" would */
    AUTOTUNE_CEILING_FAILED_TO_PERSIST,       /* zones_config_set_max_ramp() itself failed -- but that function
                                                * writes RAM and bumps s_config_generation BEFORE it calls
                                                * nvs_save(), so the new ceiling IS live in RAM for this boot;
                                                * it just was not persisted and will revert on reboot */
} autotune_ceiling_adoption_t;

/* Result of autotune_engine_accept() below: what happened to the
 * ceiling-adoption attempt (see autotune_ceiling_adoption_t above) and the
 * old/new ceiling values involved, so an HTTP or UART caller can show the
 * operator the actual outcome instead of inferring it from a log line.
 * Meaningful only when opts->adopt_ceiling was true; otherwise adoption
 * reads AUTOTUNE_CEILING_SKIPPED_NOT_REQUESTED and both ceilings are 0. */
typedef struct {
    autotune_ceiling_adoption_t adoption;
    float old_ceiling_c_per_hr;
    float new_ceiling_c_per_hr;
} autotune_accept_result_t;

/* Writes proposed_gains into this zone's stored PID config via
 * zones_config_set_pid() (TODO.md 6A.4: "results are proposed, never
 * auto-applied... only then does it get written through zones_http's
 * config"). Only valid from DONE; returns false otherwise. Resets the
 * engine to IDLE on success.
 *
 * opts may be NULL, meaning {ack_unsettled=false, adopt_ceiling=false} --
 * the original, most conservative behavior. out may be NULL when the caller
 * does not need the ceiling-adoption outcome.
 *
 * opts->ack_unsettled: for a STEP-method result, acceptance is refused
 * unless this is true whenever ANY of THREE independent trustworthiness
 * signals reads false -- model.settled (the STEPPING-phase relative-slope
 * detector never genuinely fired; ended via the max-duration backstop
 * instead), model.extrapolation_converged (the asymptote-correction loop
 * hit its iteration cap or safety ceiling without converging), or
 * model.tau_consistent_with_gain (tau_s/dead_time_s could not be re-fitted
 * to match the corrected k_gain_c_per_duty) -- so an operator (or automated
 * caller) must make a deliberate, distinct choice to persist a
 * lower-confidence fit rather than it happening silently by default. ONE
 * acknowledgement covers all three; a refused call's ESP_LOGW names exactly
 * which one(s) tripped, and all three are surfaced distinctly (not
 * collapsed into one bit) through autotune_engine_get_status()/the
 * dashboard JSON/the wire status so a caller can show the operator WHY
 * before they tick the box. Has no effect on a RELAY-method result
 * (relay_model_t has none of these three concepts) or on a STEP result
 * where all three already read true/settled.
 *
 * A step-test acceptance also writes the fitted FOPDT model. A relay-test
 * acceptance writes gains ONLY, and deliberately leaves any stored model
 * alone -- see the .c for the reasoning, which is the difference between
 * "this run measured no model" and "this run measured that there is no
 * model".
 *
 * opts->adopt_ceiling additionally lets the caller adopt the run's
 * predicted_max_ramp_ambient_c_per_hr (TODO.md 6A.4: "the ceiling is shown
 * but not wired into max_ramp_c_per_hr") into the zone's stored ramp
 * ceiling (zones_config_set_max_ramp()) in the same accept action, rather
 * than requiring a separate manual edit on /settings/zones. Adopts
 * predicted_max_ramp_ambient_c_per_hr, never the end-of-step
 * predicted_max_ramp_c_per_hr -- see that field's own comment in
 * autotune_engine_status_t for why the end-of-step value is unsafe to use
 * as a hard block.
 *
 * Adoption is skipped (out->adoption names which condition, never a silent
 * no-op) when:
 *   - the method is RELAY (a relay test measures no FOPDT model, so there
 *     is no predicted ceiling to adopt),
 *   - the STEP result's predicted_max_ramp_ambient_c_per_hr is <= 0 (never
 *     computed, e.g. pid_autotune_estimate_max_ramp_c_per_hr() had no
 *     ambient headroom to extrapolate from),
 *   - zones_config_set_model() itself was rejected/failed for this run (the
 *     same model that produced the estimate did not persist, so adopting a
 *     ceiling derived from it would outlive the model it depends on),
 *   - zones_config_get_max_ramp() (the fresh read of the currently stored
 *     ceiling) fails -- never adopt over a value that could not be read,
 *     even though a failed read and "nothing configured yet" would
 *     otherwise look identical, or
 *   - the currently stored ceiling is nonzero and looser than the new
 *     estimate -- adoption never auto-tightens an existing,
 *     deliberately-set ceiling.
 * An out-of-range estimate (> ZONE_MAX_RAMP_C_PER_HR_MAX) is REJECTED, never
 * clamped. A persist failure on the ceiling write itself is reported as
 * AUTOTUNE_CEILING_FAILED_TO_PERSIST.
 *
 * In every case the gains (and model, if any) are still accepted/persisted
 * regardless of adopt_ceiling's outcome -- adopt_ceiling only ever adds a
 * write, never blocks the ones this function already made. */
bool autotune_engine_accept(const autotune_accept_opts_t *opts, autotune_accept_result_t *out);

void autotune_engine_get_status(autotune_engine_status_t *out);

/* Paged access to the raw trace, oldest-first, for a caller that wants to
 * stream a CSV export (TODO.md 6A.4's "persist the full autotune trace...
 * downloadable as CSV") without allocating an AUTOTUNE_ENGINE_MAX_SAMPLES-
 * sized buffer of its own -- see profile_executor_get_history()'s doc
 * comment for the same reasoning (and the OOM it was written to avoid).
 * Returns the number of samples actually written to out. */
size_t autotune_engine_get_trace(autotune_sample_t *out, size_t start_index, size_t max_entries);

/* One coupling matrix cell: zone i's step response as seen at zone j
 * (i == j is the direct/diagonal gain, identical to the model in
 * autotune_engine_status_t for the run that produced it). valid=false means
 * this cell has never been filled (no completed run on row i yet) or that
 * run's fit for this j didn't converge (e.g. j's sensor was invalid/absent
 * for that run, or the response was too small to fit against the noise
 * floor -- a genuinely decoupled pair looks the same as an unmeasured one
 * from this struct alone; check row i's own model.valid to tell "zone i has
 * never been tested" apart from "zone i was tested but j didn't respond"). */
typedef struct {
    fopdt_model_t model;
    bool valid;
} autotune_coupling_cell_t;

/* Row i, column j -- filled by row when zone i's autotune run finishes with
 * a valid fit. MAX31856_CHANNEL_COUNT x MAX31856_CHANNEL_COUNT, dense
 * (rows/columns for unconfigured zones are simply never written, valid
 * stays false). TODO.md 6A.5(c)'s RGA and (e)'s decoupler both consume this
 * directly once built. */
typedef struct {
    autotune_coupling_cell_t cell[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT];
} autotune_coupling_matrix_t;

/* Copies the whole matrix out under lock. Cheap (<= 9 cells at
 * THERMO_CHANNEL_COUNT=3), no pagination needed unlike the sample traces. */
void autotune_engine_get_coupling_matrix(autotune_coupling_matrix_t *out);

/* TODO.md 6A.5(c): the Relative Gain Array of the coupling matrix -- "are
 * independent per-zone PID loops legitimate on this kiln?", as a number.
 *
 * Pure adapter: flattens `m`'s cells into the row-major arrays
 * pid_autotune_rga() takes and hands back its verdict unmodified. It takes
 * a caller-owned *copy* of the matrix rather than reading the engine's own
 * (and so takes no lock) because callers invariably already have one --
 * dashboard_http's handler serves both in the same response, and computing
 * the RGA from a second, later snapshot could describe a matrix the
 * operator was never shown.
 *
 * The result is only ever as real as its input, and no input cell has ever
 * been filled on hardware: every on-target autotune run to date aborts on
 * guard 6 (sensor invalid) because the bench unit has no thermocouples
 * attached, so this has never run on measured data. The math itself is
 * host-tested (App/test/test_sim_kiln.c). */
void autotune_engine_compute_rga(const autotune_coupling_matrix_t *m, autotune_rga_t *out);

/* True while a test is running -- any of SETTLING/STEPPING (step method) or
 * RELAY_APPROACH/RELAY_CYCLING (relay method). Both methods hold the zone's
 * relay authority for exactly as long as this is true. */
bool autotune_engine_is_active(void);

/* True if an autotune run is currently SETTLING/STEPPING on zone_index
 * specifically -- profile_executor_run() checks this per zone (TODO.md
 * 6A.5 made per-zone the meaningful granularity; before concurrent
 * multi-zone execution existed, "any zone" and "this zone" were the same
 * question). */
bool autotune_engine_is_active_on_zone(uint8_t zone_index);

/* iter_tune_http.c's restore_commissioned race close (step-7 review,
 * 2026-09-23, advisory A1 follow-up): the handler's own check-then-apply
 * shape (autotune_engine_is_active_on_zone() once, then zones_config_set_pid()
 * later) left a window where autotune could begin on the zone in between,
 * clobbering the restore or being silently clobbered by it. Reserve the zone
 * before reading/computing the gains to restore; release it once the write
 * (zones_config_set_pid() + the persisted-store update) is done, on EVERY
 * exit path once reserved, success or failure. Reserve takes and releases
 * s_at.lock internally and never holds it across the caller's own write --
 * the actual interlock is that autotune_begin_run_locked() refuses to start
 * on a reserved zone at the same commit point (under the same lock) it
 * already refuses an already-running zone. Returns false (does not reserve,
 * nothing to release) if autotune already owns this zone or another external
 * writer already holds the single reservation slot; returns true (trivially,
 * nothing to protect against) if the engine has never started. */
bool autotune_engine_reserve_zone_for_external_write(uint8_t zone_index);
void autotune_engine_release_zone_for_external_write(uint8_t zone_index);

#ifdef __cplusplus
}
#endif

#endif // AUTOTUNE_ENGINE_H
