// pid_fuzzy_confidence.c -- see pid_fuzzy_confidence.h for the design
// rationale (docs/ADAPTIVE_FUZZY_EVALUATION.md sec 3). Pure C, no
// FreeRTOS/ESP-IDF/logging/I-O -- host-testable like pid.c/pid_fuzzy.c.

#include "pid_fuzzy_confidence.h"

#include <math.h>

// (N2) breakpoints -- plan sec 3.2, chosen not measured (see header).
#define CAP_L_FULL_BELOW   0.10f
#define CAP_L_ZERO_ABOVE   0.30f

float pid_fuzzy_confidence_cap_l(float dead_time_s, float tau_s)
{
    if (!isfinite(dead_time_s) || !isfinite(tau_s) || dead_time_s < 0.0f || !(tau_s > 0.0f)) {
        return 0.0f; // fail safe -- unknown ratio must never read as "no cap"
    }
    float ratio = dead_time_s / tau_s;
    if (ratio <= CAP_L_FULL_BELOW) return 1.0f;
    if (ratio >= CAP_L_ZERO_ABOVE) return 0.0f;
    // Linear taper 1.0 -> 0.0 over (CAP_L_FULL_BELOW, CAP_L_ZERO_ABOVE].
    float span = CAP_L_ZERO_ABOVE - CAP_L_FULL_BELOW;
    float frac = (ratio - CAP_L_FULL_BELOW) / span;
    float cap = 1.0f - frac;
    if (cap < 0.0f) cap = 0.0f;
    if (cap > 1.0f) cap = 1.0f;
    return cap;
}

uint8_t pid_fuzzy_confidence_strength_pct(uint8_t consecutive_good_runs, float cap_l)
{
    uint8_t c = consecutive_good_runs;
    if (c > PID_FUZZY_CONFIDENCE_MAX_C) c = PID_FUZZY_CONFIDENCE_MAX_C;

    if (!isfinite(cap_l)) return 0; // defence in depth -- see cap_l's own header comment
    float cl = cap_l;
    if (cl < 0.0f) cl = 0.0f;
    if (cl > 1.0f) cl = 1.0f;

    if (c == 0 || cl <= 0.0f) return 0; // deliberate AND -- see header comment

    float strength_f = (float)PID_FUZZY_CONFIDENCE_S_MAX_PCT * ((float)c / (float)PID_FUZZY_CONFIDENCE_MAX_C) * cl;
    if (!isfinite(strength_f) || strength_f < 0.0f) return 0;
    if (strength_f > (float)PID_FUZZY_CONFIDENCE_S_MAX_PCT) strength_f = (float)PID_FUZZY_CONFIDENCE_S_MAX_PCT;
    return (uint8_t)(strength_f + 0.5f); // round(), matches pid_fuzzy_prepare_gains()'s own rounding convention
}

void pid_fuzzy_oscillation_reset(pid_fuzzy_oscillation_state_t *st)
{
    if (!st) return;
    st->prev_error_c = 0.0f;
    st->have_prev_error = false;
    st->crossings_in_window = 0;
    st->window_elapsed_s = 0.0f;
    st->tripped_this_firing = false;
}

// Chosen, not measured (plan sec 3.2 measured 29 vs 0 -- an enormous
// margin; this project's own standing lesson, per CLAUDE.md's "Negative
// test every check", is to say plainly when a number is picked rather than
// fit). PID_FUZZY_OSCILLATION_TRIP_CROSSINGS sits well below the measured
// oscillating-arm count and well above ordinary single-crossing noise (a
// normal approach to setpoint crosses it once, settling dwells commonly
// cross zero 0-1 times from measurement noise near the setpoint -- more
// than a handful within one window is the "not merely a converged approach"
// signal, not a novel discovery of exactly where the line has to be).
#define PID_FUZZY_OSCILLATION_TRIP_CROSSINGS 4u
// A tumbling window -- see pid_fuzzy_oscillation_state_t's own doc comment
// for why tumbling (not sliding) is adequate here. 600 s (10 min) is
// shorter than a typical dwell segment so a limit cycle gets multiple
// chances to trip within one dwell, and long enough that a single ordinary
// settle-then-cross does not by itself fill more than one window.
#define PID_FUZZY_OSCILLATION_WINDOW_S 600.0f

bool pid_fuzzy_oscillation_tick(pid_fuzzy_oscillation_state_t *st, float error_c, float dt_s)
{
    if (!st) return false;
    if (!isfinite(error_c) || !isfinite(dt_s) || dt_s <= 0.0f) {
        return false; // bad tick: no-op, not a false crossing and not a silently-early window reset
    }

    bool crossed = false;
    if (st->have_prev_error) {
        // Strict sign change only -- exactly 0.0f on either side is treated
        // as neutral (neither side of a crossing), matching ordinary
        // floating-point practice for a zero-crossing test and avoiding
        // manufacturing crossings out of a value that is legitimately
        // sitting at exactly zero for more than one tick.
        bool prev_pos = st->prev_error_c > 0.0f;
        bool prev_neg = st->prev_error_c < 0.0f;
        bool now_pos = error_c > 0.0f;
        bool now_neg = error_c < 0.0f;
        if ((prev_pos && now_neg) || (prev_neg && now_pos)) {
            crossed = true;
        }
    }
    st->prev_error_c = error_c;
    st->have_prev_error = true;

    if (crossed) st->crossings_in_window++;
    st->window_elapsed_s += dt_s;

    bool just_tripped = false;
    if (!st->tripped_this_firing && st->crossings_in_window >= PID_FUZZY_OSCILLATION_TRIP_CROSSINGS) {
        st->tripped_this_firing = true;
        just_tripped = true;
    }

    if (st->window_elapsed_s >= PID_FUZZY_OSCILLATION_WINDOW_S) {
        st->window_elapsed_s = 0.0f;
        st->crossings_in_window = 0; // tumble -- see header comment. tripped_this_firing is untouched: sticky.
    }

    return just_tripped;
}
