// tc_lag_comp -- thermocouple protection-tube (sheath) lag compensation.
//
// A mullite (or other ceramic) protection tube over a thermocouple adds a
// thermal mass between the kiln air/ware and the junction. To first order the
// junction sees T_meas' = (T_true - T_meas) / tau, so the reading trails the
// kiln by about tau * dT/dt on a ramp and overshoots on a hold. This module
// inverts that first-order lag:
//
//     T_est = T_meas + tau * dT_meas/dt
//
// with dT_meas/dt taken from a finite difference passed through two
// cascaded first-order low-pass stages (time constant tf each, see
// tc_lag_comp_filter_tau_s()). The net transfer from T_true to T_est is then
// 1 / (1 + tf*s)^2 instead of 1 / (1 + tau*s): the residual lag shrinks from
// tau to about 2*tf while TC noise (MAX31856, ~0.1-0.25 C) is amplified by
// roughly tau/tf at most -- see docs/TC_SHEATH_LAG_SIM.md for the measured
// numbers.
//
// Safety posture (owner rule: never weaken a guard):
//   - tau == 0 is an exact passthrough: the returned value IS the input and
//     the correction is 0.0f, so a board with the setting off behaves
//     byte-identically to firmware without this module.
//   - |correction| is clamped to TC_LAG_COMP_MAX_CORR_C, so noise, a spike
//     or a mis-set tau can never move the estimate further than that from
//     the real reading.
//   - Every state that pairs with the filter lives in this one struct and
//     tc_lag_comp_reset() clears all of it together (reset-one-side bug
//     class). A reset happens on: invalid/non-finite reading, a gap longer
//     than TC_LAG_COMP_MAX_DT_S (stale/paused), a jump bigger than
//     TC_LAG_COMP_GLITCH_C that is still there on the next tick (a real
//     step), or a tau change. A jump that lasts ONE tick (a noise spike) is
//     held instead: that tick returns the reading itself with no correction
//     (corr_c 0, so the guards see raw for it) and the filter state is kept,
//     so a lone spike does not throw away the accumulated correction. After a reset the
//     estimate equals the raw reading and the correction grows back in
//     smoothly as the filter re-fills -- never a wild value.
//   - The estimate is a CONTROL input only. Guards never see it in place of
//     the raw reading; they get the correction as thermal_guard_input_t::
//     lead_c and use whichever of raw and compensated reacts first: guard 5
//     (over-temperature) checks raw + max(0, lead), guards 1/2 (no-progress)
//     take the smaller window rise, guard 3 (runaway) the larger rise.
//
// Pure C, no ESP-IDF dependency: host-tested in App/test/test_tc_lag_comp.c
// and driven closed-loop in App/test/test_tc_lag_sim.c.
#ifndef TC_LAG_COMP_H
#define TC_LAG_COMP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Setting range (seconds). 0 = no sheath, compensation off. 600 s covers
 * thick-wall mullite tubes with an air gap; anything slower is better fixed
 * with a thinner tube than with a bigger lead term. */
#define TC_LAG_COMP_TAU_MIN_S 0u
#define TC_LAG_COMP_TAU_MAX_S 600u

/* Hard ceiling on |T_est - T_meas|. A 300 C/h ramp seen through a 600 s
 * sheath lags by 50 C; 60 C leaves headroom for that and no more. */
#define TC_LAG_COMP_MAX_CORR_C 60.0f

/* A gap between two readings longer than this (paused run, missed ticks,
 * stale sensor) resets the filter instead of differencing across it. */
#define TC_LAG_COMP_MAX_DT_S 5.0f

/* One-tick jump that no kiln can produce through a sheath (the executor
 * ticks at 1 Hz; 10 C/s is ~36000 C/h). Treated as a glitch: filter reset. */
#define TC_LAG_COMP_GLITCH_C 10.0f

typedef struct {
    bool     primed;     /* prev_c holds a trusted reading */
    float    prev_c;     /* previous valid reading */
    float    d1_c_per_s; /* first low-pass stage of dT/dt */
    float    d2_c_per_s; /* second low-pass stage of dT/dt (the one used) */
    float    tau_s;      /* tau the filter state was built with */
    float    corr_c;     /* last applied correction (T_est - T_meas), clamped */
    bool     glitch_held; /* previous tick was a one-tick jump: held, not used */
    float    held_dt_s;   /* time skipped while glitch_held (added to the next dt) */
    uint32_t resets;     /* filter resets since the struct was zeroed (diagnostic) */
} tc_lag_comp_state_t;

/* Clears every field together (primed, prev, both filter stages, tau, corr).
 * resets is incremented, not cleared, so callers can see that a reset
 * happened. A zeroed struct is a valid, unprimed state. */
void tc_lag_comp_reset(tc_lag_comp_state_t *st);

/* Filter time constant of each of the two derivative stages for a sheath
 * tau: clamp(tau / 4, 5 s, 60 s). */
float tc_lag_comp_filter_tau_s(float tau_s);

/* One tick. Returns the compensated estimate (or meas_c itself when tau <= 0,
 * !valid, or the filter was just reset). st->corr_c holds the applied
 * correction afterwards. *out_reset (may be NULL) is set true when this call
 * discarded filter state that carried a non-zero correction, so the caller
 * can re-base anything that differenced the previous estimate (the PID's
 * derivative memory). */
float tc_lag_comp_step(tc_lag_comp_state_t *st, float tau_s, float meas_c, bool valid, float dt_s,
                       bool *out_reset);

#ifdef __cplusplus
}
#endif

#endif // TC_LAG_COMP_H
