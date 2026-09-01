// pid -- the control loop itself. TODO.md section 6A.2.
//
// Pure C, no FreeRTOS, no ESP-IDF, no logging, no I/O, no globals. State
// lives entirely in the caller-owned pid_state_t. This is deliberate, not
// stylistic: it's what lets this module be unit-tested on the host against
// App/drivers/sim_plant.c (6A.8) before it ever runs against a real kiln.
//
// Form: positional PID, derivative on measurement (not error -- a profile's
// ramp steps the setpoint every tick, and derivative-on-error would spike on
// every step), a low-pass filter on the derivative term, conditional-
// integration + clamp anti-windup, output clamped to [0,1] (there is no
// active cooling -- negative output is meaningless, not just out of range),
// and functional-range blending (full-on/full-off outside pid_range_c,
// integrator held) so a cold start doesn't spend an hour winding up I.
//
// Feedforward (ff_u) is an optional duty term in [0,1] from a caller's own
// FOPDT model (autotune, TODO.md 6A.4); pid_update()/pid_seed_bumpless() both
// take it explicitly rather than assuming 0, so a zone with no identified
// model just passes 0.0f.
//
// Integral floor vs feedforward: the classic "integral >= 0" anti-windup
// floor (no active cooling, so a negative I term is meaningless) is only
// correct when ff_u is 0. With feedforward in the loop, ff_u can itself
// over-predict the duty a zone needs (a coupled multi-zone solve routinely
// does this for whichever zone gets the largest hold duty -- see
// PID_EXPANSION_PLAN.md / TODO.md 6A.2's zone-2-droop case), and the PID's
// job explicitly includes subtracting that surplus back out. The floor is
// therefore on ki*integral >= -ff_hold, not >= 0 -- the integral may cancel
// at most what feedforward's HOLD component added, never more, and never
// the CLIMB component.
//
// Why hold only, not the full ff_u=hold+climb (2026-08-31 "hold-only floor"
// fix): an earlier version of this floor used -ff_u (the total feedforward,
// hold plus climb) and fixed a real dwell-offset defect on hardware, but
// regressed ramps -- once the floor bound during a constant-rate ramp, the
// integral exactly cancelled the WHOLE of ff_u including climb, so
// commanded duty ran on P+D alone with feedforward fully absent, and it did
// not release until the ramp ended (climb is roughly constant through a
// constant-rate ramp, so once bound the floor stayed bound). climb is
// exactly 0.0 during a dwell (the profile executor forces
// target_rate_c_per_s to 0 every dwell tick, and the climb solve is
// homogeneous in rate), so flooring at -ff_hold instead of -ff_u leaves the
// dwell fix byte-for-byte unchanged while letting climb survive the floor
// during a ramp. This is a strict generalization -- ff_hold=ff_u reproduces
// the old -ff_u floor exactly (the safe default for a caller with no
// hold/climb split), and ff_hold=ff_u=0.0f reproduces the original >= 0
// floor -- so it changes nothing for a zone with no feedforward model, and
// nothing for a zone whose feedforward under-predicts (its integral stays
// positive and never approaches either floor). Unbounded negative windup
// during a genuine cool-down or heat-blocked period (ff_u ~ 0, so the floor
// is ~0 same as before) is prevented by the conditional-integration freeze,
// not by this floor.
//
// Scope note (see docs referenced in the implementing commit): this recovers
// the ramp regression only partially, and only for a ramp that follows a
// dwell whose own steady-state integral was not meaningfully negative going
// in -- a ramp that inherits a substantially negative integral from a prior
// dwell can still bind the floor on carryover, driven by ff_hold itself, not
// climb. This change does not address that carryover case.
#ifndef PID_H
#define PID_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float kp;
    float ki;
    float kd;
    float d_filter_tau_s; /* low-pass time constant on the D term; default 30s
                           * per TODO.md 6A.2 -- much longer than a 3D
                           * printer's ~2s because the actuator (a 60s
                           * time-proportioning window) quantizes far coarser
                           * than a printer's PWM. */
    float b;               /* setpoint weight on the P term, 0..1; 1.0 = classic PID */
    float pid_range_c;     /* outside this from setpoint: full on/off, no PID math */
} pid_cfg_t;

typedef struct {
    float integral;          /* accumulated error*dt, NOT yet multiplied by Ki */
    float prev_measurement;
    float d_filtered;        /* -d(measurement)/dt, low-pass filtered through
                              * d_filter_tau_s -- see pid_update_terms()'s raw_d.
                              * Equals d(error)/dt whenever setpoint is locally
                              * constant, which is exactly the signal
                              * profile_executor.c's ZONE_CONTROL_MODE_PID_FUZZY
                              * path reuses as pid_fuzzy_adjust()'s
                              * error_rate_c_per_s input (PID_EXPANSION_PLAN.md
                              * Phase 3 hazard 1) rather than differencing a
                              * fresh, unfiltered rate of its own -- one
                              * filtered derivative, not two that could disagree. */
    bool  initialized;
} pid_state_t;

/* Zeroes state and marks it uninitialized -- the next pid_update() call
 * seeds prev_measurement from its first reading rather than differencing
 * against a fabricated zero. */
void pid_reset(pid_state_t *state);

/* Bumpless transfer (TODO.md 6A.2): call whenever control resumes after a
 * discontinuity (mode change, tuning change, profile resume, autotune
 * handoff) so the very next pid_update() produces u_desired instead of
 * whatever a cold integral would compute. ff_u is the same feedforward duty
 * the next pid_update()/pid_update_terms() call will be given -- solves
 * integral = (u_desired - P - ff_u) / Ki and seeds it. ff_hold is the
 * steady-state HOLD component of ff_u only (never the climb/ramp component
 * -- see pid_update_terms()'s floor comment below for why the two must stay
 * separate); the floor is Ki*integral >= -ff_hold, not -ff_u. Pass ff_hold
 * == ff_u for a caller with no hold/climb split (e.g. no feedforward model,
 * or a dwell where climb is exactly 0), which reduces the floor to the
 * original -ff_u form byte-for-byte, and pass 0.0f/0.0f for a zone with no
 * feedforward model at all, which reduces it further to the classic >= 0
 * floor. */
void pid_seed_bumpless(pid_state_t *state, const pid_cfg_t *cfg, float setpoint, float measurement,
                       float u_desired, float ff_u, float ff_hold);

/* Bump-transfer for a gain-only change mid-run (PID_EXPANSION_PLAN.md Phase 3
 * hazard 3), distinct from pid_seed_bumpless() above: that function
 * re-derives the WHOLE integral from a desired output, which is right for a
 * deliberate discontinuity (mode change, tuning-panel edit, resume from
 * pause) but overkill -- and the wrong tool -- for a caller (a fuzzy-
 * adjustment layer) that moves only Ki, tick to tick, while every other
 * fact about the loop's running state (measurement, d_filtered, the duty
 * the element is actually at) stays valid and should not be disturbed.
 *
 * Rescales integral so ki_old*integral == ki_new*integral' -- the I term's
 * actual contribution to duty (ki*integral) is unchanged by the rescale
 * itself; only the caller's own deliberate change in Ki moves it, and it
 * moves smoothly rather than stepping. A no-op if either gain is <= 0 (no
 * sensible ratio to rescale against/onto) or the gains are equal (nothing to
 * do) -- call this unconditionally every tick a caller's effective Ki may
 * have changed; it is cheap and correct to call when it didn't. */
void pid_rescale_integral_for_new_ki(pid_state_t *state, float old_ki, float new_ki);

/* Term breakdown from the most recent pid_update_terms() call -- "tuning by
 * evidence, not intuition" (TODO.md 6A.9's /api/control bullet). p/i/d/ff
 * are each the term's contribution to u *before* the final [0,1] clamp, so
 * p+i+d+ff can legitimately fall outside [0,1] even though the returned u
 * never does -- that gap is itself diagnostic (e.g. i pinned at its clamp
 * while p+d alone would already saturate the output). */
typedef struct {
    float p;
    float i;
    float d;
    float ff;
} pid_terms_t;

/* Same contract as pid_update() below, plus an optional (NULL-able)
 * out_terms for the P/I/D/FF breakdown. pid_update() is a thin wrapper over
 * this with out_terms=NULL -- one control-math implementation, not two
 * copies that could drift. ff_hold is the steady-state HOLD-only portion of
 * ff_u (see the floor comment inside pid_update_terms() in pid.c for why the
 * climb/ramp portion must NOT be included) -- ff_u itself still flows into
 * the P+I+D+ff sum unchanged; only the floor's basis differs. */
float pid_update_terms(pid_state_t *state, const pid_cfg_t *cfg, float setpoint, float measurement,
                       float dt_s, float ff_u, float ff_hold, pid_terms_t *out_terms);

/* One control tick. setpoint/measurement in degC, dt_s the *measured*
 * elapsed time (not the nominal tick period -- a tick delayed by a slow I2C
 * transfer must not silently change the effective Ki/Kd), ff_u an optional
 * feedforward duty term in [0,1] (0.0f if none), ff_hold its steady-state
 * hold-only component (see pid_update_terms()'s doc comment). Returns
 * commanded duty, already clamped to [0,1]. */
float pid_update(pid_state_t *state, const pid_cfg_t *cfg, float setpoint, float measurement,
                 float dt_s, float ff_u, float ff_hold);

#ifdef __cplusplus
}
#endif

#endif // PID_H
