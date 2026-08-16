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
    float d_filtered;
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
 * integral = (u_desired - P - ff_u) / Ki and seeds it (clamped to >= 0,
 * since a negative integral would itself violate the anti-windup clamp on
 * the next tick). Pass 0.0f for a zone with no feedforward model. */
void pid_seed_bumpless(pid_state_t *state, const pid_cfg_t *cfg, float setpoint, float measurement,
                       float u_desired, float ff_u);

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
 * copies that could drift. */
float pid_update_terms(pid_state_t *state, const pid_cfg_t *cfg, float setpoint, float measurement,
                       float dt_s, float ff_u, pid_terms_t *out_terms);

/* One control tick. setpoint/measurement in degC, dt_s the *measured*
 * elapsed time (not the nominal tick period -- a tick delayed by a slow I2C
 * transfer must not silently change the effective Ki/Kd), ff_u an optional
 * feedforward duty term in [0,1] (0.0f if none). Returns commanded duty,
 * already clamped to [0,1]. */
float pid_update(pid_state_t *state, const pid_cfg_t *cfg, float setpoint, float measurement,
                 float dt_s, float ff_u);

#ifdef __cplusplus
}
#endif

#endif // PID_H
