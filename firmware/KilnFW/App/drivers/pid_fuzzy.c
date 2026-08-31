#include "pid_fuzzy.h"

#include <math.h>

// Bucket edges (degC / degC-per-s) for the triangular membership functions.
// A kiln's PID loop runs on a very slow, high-dead-time plant, so these are
// deliberately much wider than a printer-hotend fuzzy-PID paper would use --
// tuned to this project's own error/rate scale, not lifted from a citation.
// ERROR_BAND_C: "large" error starts around a single time-proportioning
// window's worth of visible overshoot/undershoot for a mid-size kiln zone.
//
// RATE_BAND_C_PER_S (PID_EXPANSION_PLAN.md Phase 3 hazard 2, re-scaled
// 2026-08-30 from an earlier 0.05): profile_executor.c feeds this axis
// z->pid_state.d_filtered, which is -d(measurement)/dt low-pass filtered
// (derivative on measurement: pid.c never differentiates the setpoint, so
// this equals d(error)/dt only while the setpoint is locally constant --
// see pid.h's pid_state_t.d_filtered comment and pid_fuzzy_adjust()'s
// error_rate_c_per_s doc; the negation is applied once, inside pid.c's
// raw_d computation, so nothing downstream re-derives or re-applies a
// sign). A firing tracking a perfectly normal ramp is NOT idle on this
// axis, and NOT for the reason a reader might first assume: true d(error)/dt
// is near zero while tracking well (that is what tracking well means), but
// this signal ignores dSP/dt, so its magnitude sits close to the commanded
// ramp rate for the whole ramp. Its *sign* is likewise the temperature's
// climb NEGATED -- a kiln climbing at 300 degC/hr feeds -0.083 here, which
// leans toward bucket 0 (NEG, i.e. error falling), not bucket 2. 100 degC/hr (a brisk kiln ramp) is 0.028 degC/s; even a fast
// 300 degC/hr ramp is only 0.083 degC/s in magnitude. The old 0.05 constant
// put an ordinary firing mid-scale on this axis for its entire duration, so
// "large rate" was measuring the profile, not a disturbance -- exactly the
// hazard the plan calls out. (The band below is sized on magnitude only, so
// this re-scale does not depend on getting the sign convention right --
// but readers must still get the sign right, since it determines which
// rule -- RISING vs FALLING -- fires.)
//
// 0.5 degC/s (30 degC/min) is roughly 6x the fastest ramp rate this kiln's
// profiles realistically command, and roughly an order of magnitude above
// the brisk-but-normal 100 degC/hr case. A legitimate firing's own ramp now
// sits well inside the STEADY bucket (see triangular_memberships()) for its
// whole duration; only a rate this axis should actually call "large" --
// a stuck-open lid, a runaway element, a thermocouple that just came
// unstuck and is snapping toward ambient -- reaches toward the RISING/
// FALLING extremes. "Large" on this axis now means, physically, a rate of
// change no ordinary ramp produces, only a genuine disturbance or fault.
#define ERROR_BAND_C 20.0f
#define RATE_BAND_C_PER_S 0.5f

// Maximum fractional nudge any single gain may receive at strength_pct=100
// and full rule membership (degree 1.0). 0.5 == the fuzzy layer may at most
// halve or 1.5x a base gain -- a bounded adjustment, not a re-tune.
#define MAX_NUDGE_FRACTION 0.5f

/* Triangular membership over three buckets (NEG/ZERO/POS, or equivalently
 * FALLING/STEADY/RISING), centered at -band/0/+band, each degree in [0,1]
 * and the three summing to exactly 1.0 for any finite x. */
static void triangular_memberships(float x, float band, float *neg, float *zero, float *pos)
{
    if (x <= -band) {
        *neg = 1.0f;
        *zero = 0.0f;
        *pos = 0.0f;
        return;
    }
    if (x >= band) {
        *neg = 0.0f;
        *zero = 0.0f;
        *pos = 1.0f;
        return;
    }
    if (x <= 0.0f) {
        /* between -band and 0: neg falls 1->0, zero rises 0->1 */
        float t = (-x) / band; /* 1 at x=-band, 0 at x=0 */
        *neg = t;
        *zero = 1.0f - t;
        *pos = 0.0f;
    } else {
        /* between 0 and band: zero falls 1->0, pos rises 0->1 */
        float t = x / band; /* 0 at x=0, 1 at x=band */
        *neg = 0.0f;
        *zero = 1.0f - t;
        *pos = t;
    }
}

/* Rule table directions, PID_EXPANSION_PLAN.md §3.3 / pid_fuzzy.h's header
 * comment -- that comment is the documentation source of truth (and what
 * the UI renders); this array must stay in lockstep with it. Indexed
 * [error_bucket][rate_bucket], each entry {kp_dir, ki_dir, kd_dir} in
 * {-1, 0, +1}. Bucket order for both axes: 0=NEG/FALLING, 1=ZERO/STEADY,
 * 2=POS/RISING. */
typedef struct {
    float kp;
    float ki;
    float kd;
} rule_dir_t;

static const rule_dir_t RULE_TABLE[3][3] = {
    /* error = NEG (large, overshoot) */
    {{1.0f, -1.0f, 1.0f}, /* rate FALLING: overshoot still growing -> attack */
     {1.0f, 0.0f, 0.0f},  /* rate STEADY: steady overshoot -> push harder */
     {-1.0f, 1.0f, -1.0f}}, /* rate RISING: recovering -> ease off */
    /* error = ZERO (near setpoint) */
    {{-1.0f, -1.0f, 1.0f}, /* rate FALLING: crossing target fast -> damp */
     {-1.0f, 1.0f, -1.0f}, /* rate STEADY: settled -> coast on I */
     {1.0f, -1.0f, 1.0f}},  /* rate RISING: just left target -> catch it */
    /* error = POS (large, undershoot) */
    {{-1.0f, 1.0f, -1.0f}, /* rate FALLING: already closing in -> ease off */
     {1.0f, 0.0f, 0.0f},   /* rate STEADY: steady approach -> push harder */
     {1.0f, -1.0f, 1.0f}},  /* rate RISING: far and getting worse -> attack */
};

/* Clamp a single adjusted gain: never negative, never non-finite. A rule
 * table cell, an input magnitude, or a strength value producing a bad gain
 * here would reach pid_update() on a real kiln, so this is the last line of
 * defense, not a style clamp. */
static float clamp_gain(float g)
{
    if (!isfinite(g) || g < 0.0f) {
        return 0.0f;
    }
    return g;
}

/* Base gain sanitized the same way before any arithmetic touches it, so a
 * bad base_kp/ki/kd (should never happen -- Autotune writes these -- but
 * this module does not get to assume its caller is perfect) cannot produce
 * a NaN that survives multiplication by a finite nudge factor. */
static float sanitize_base(float base)
{
    if (!isfinite(base) || base < 0.0f) {
        return 0.0f;
    }
    return base;
}

void pid_fuzzy_adjust(float error_c, float error_rate_c_per_s,
                      float base_kp, float base_ki, float base_kd,
                      uint8_t strength_pct,
                      float *out_kp, float *out_ki, float *out_kd)
{
    float kp = sanitize_base(base_kp);
    float ki = sanitize_base(base_ki);
    float kd = sanitize_base(base_kd);

    if (strength_pct > 100) {
        strength_pct = 100;
    }

    /* strength_pct == 0 is the safety contract: reproduce the base gains
     * exactly, bit-for-bit, with no fuzzy math in the path at all -- not
     * "multiply by a zero factor and hope it round-trips".
     *
     * The sanitized copies are what's returned, NOT the raw arguments. For
     * every legitimate gain sanitize_base() is the identity function, so the
     * bit-for-bit contract holds exactly as documented; but returning the raw
     * arguments here would let a NaN or negative base gain pass straight
     * through untouched -- and strength_pct == 0 is the *most conservative*
     * setting, the one a cautious operator picks and the one a zeroed config
     * blob defaults to. Leaking a NaN gain into the PID loop specifically in
     * the safest configuration is the exact hazard this module exists to
     * prevent, so the short-circuit sanitizes too. */
    if (strength_pct == 0) {
        *out_kp = kp;
        *out_ki = ki;
        *out_kd = kd;
        return;
    }

    /* Defend against a faulted-thermocouple NaN/inf reaching the membership
     * math. An earlier version mapped a non-finite input to 0.0 on both axes
     * and called that "the most conservative cell" -- it is not. 0/0 lands on
     * ZERO/STEADY, whose rule is {Kp-, Ki+, Kd-}: it *raises* the integral
     * gain by up to 50%. Winding up harder on a 30-300 s dead-time plant
     * precisely when the temperature reading has failed is the worst
     * available response, and because the accumulated integral in pid.c
     * persists across the gain change, the I term steps discontinuously the
     * moment the fault appears.
     *
     * There is no informative fuzzy answer to "the sensor is lying", so this
     * makes no adjustment at all: return the base gains, exactly as
     * strength_pct == 0 does. Hold the last known-good tuning and let the
     * thermal guards -- which own fault handling -- decide what happens to
     * the firing. */
    if (!isfinite(error_c) || !isfinite(error_rate_c_per_s)) {
        *out_kp = kp;
        *out_ki = ki;
        *out_kd = kd;
        return;
    }

    float error = error_c;
    float rate = error_rate_c_per_s;

    float e_neg, e_zero, e_pos;
    float r_neg, r_zero, r_pos;
    triangular_memberships(error, ERROR_BAND_C, &e_neg, &e_zero, &e_pos);
    triangular_memberships(rate, RATE_BAND_C_PER_S, &r_neg, &r_zero, &r_pos);

    float e_deg[3] = {e_neg, e_zero, e_pos};
    float r_deg[3] = {r_neg, r_zero, r_pos};

    /* Mamdani AND (product) over all 9 cells, weighted average
     * defuzzification -- a lookup-plus-interpolation, exactly the shape
     * PID_EXPANSION_PLAN.md §2b describes ("a few hundred lines, no
     * floating-point library beyond what pid.c already uses"). */
    float kp_sum = 0.0f, ki_sum = 0.0f, kd_sum = 0.0f, weight_sum = 0.0f;
    for (int ei = 0; ei < 3; ei++) {
        for (int ri = 0; ri < 3; ri++) {
            float firing = e_deg[ei] * r_deg[ri];
            const rule_dir_t *dir = &RULE_TABLE[ei][ri];
            kp_sum += firing * dir->kp;
            ki_sum += firing * dir->ki;
            kd_sum += firing * dir->kd;
            weight_sum += firing;
        }
    }

    float kp_dir = (weight_sum > 0.0f) ? kp_sum / weight_sum : 0.0f;
    float ki_dir = (weight_sum > 0.0f) ? ki_sum / weight_sum : 0.0f;
    float kd_dir = (weight_sum > 0.0f) ? kd_sum / weight_sum : 0.0f;

    float scale = ((float)strength_pct / 100.0f) * MAX_NUDGE_FRACTION;

    *out_kp = clamp_gain(kp * (1.0f + scale * kp_dir));
    *out_ki = clamp_gain(ki * (1.0f + scale * ki_dir));
    *out_kd = clamp_gain(kd * (1.0f + scale * kd_dir));
}
