#include "on_off_trigger_decide.h"

void on_off_trigger_state_reset(on_off_trigger_state_t *state)
{
    state->quasi_dwell = false;
    state->lock_true_s = 0.0f;
    state->lock_false_s = 0.0f;
    state->commanded_on = false;      /* fail-safe-shaped: never yet commanded */
    state->held_s = 0.0f;
    state->last_segment_index = 0;
    state->have_last_segment_index = false;
}

// Plan sec 3's phase axis: phase_mask == 0 is a tautology (no phase named,
// axis drops out of the AND). Otherwise the rule must name the bit matching
// the CURRENT phase.
static bool axis_phase(const on_off_trigger_input_t *in)
{
    if (in->rule.phase_mask == 0) {
        return true;
    }
    uint8_t current_bit = in->current_phase_is_dwell ? (uint8_t)ON_OFF_PHASE_DWELL : (uint8_t)ON_OFF_PHASE_RAMP;
    return (in->rule.phase_mask & current_bit) != 0;
}

// Same tautology rule for direction. current_direction is documented to
// carry exactly one bit; a caller that ever passed more than one would
// still be handled correctly here (any overlap counts), it's just not the
// contract.
static bool axis_direction(const on_off_trigger_input_t *in)
{
    if (in->rule.direction_mask == 0) {
        return true;
    }
    return (in->rule.direction_mask & in->current_direction) != 0;
}

// Plan sec 3's temperature hysteresis: "An ABOVE rule turns on at
// threshold + hyst_c/2 and off at threshold - hyst_c/2 (BELOW mirrored)."
// `on_ref` is the axis's own memory of which side of the band it is
// currently sitting on -- this module uses the RELAY's current commanded
// state as that memory (a single-output device has exactly one hysteresis
// loop to remember), so entering this function already ON only needs to
// cross the nearer (interior) edge to stay ON, and entering OFF only needs
// to cross the nearer edge to turn ON -- textbook two-point hysteresis, not
// re-derived per axis.
static bool axis_temp(const on_off_trigger_input_t *in, bool on_ref)
{
    if (in->rule.temp_cmp == ON_OFF_TEMP_CMP_NONE) {
        return true;
    }
    float half = in->hyst_c * 0.5f;
    float t = in->temp_measurement_c;
    if (in->rule.temp_cmp == ON_OFF_TEMP_CMP_ABOVE) {
        float edge = on_ref ? (in->rule.temp_threshold_c - half) : (in->rule.temp_threshold_c + half);
        return t >= edge;
    }
    /* ON_OFF_TEMP_CMP_BELOW */
    float edge = on_ref ? (in->rule.temp_threshold_c + half) : (in->rule.temp_threshold_c - half);
    return t <= edge;
}

// time_start_s/time_stop_s window into the segment; time_stop_s == 0 means
// "to end of segment" (tautology on the upper bound).
static bool axis_time(const on_off_trigger_input_t *in)
{
    if (in->segment_elapsed_s < (float)in->rule.time_start_s) {
        return false;
    }
    if (in->rule.time_stop_s != 0 && in->segment_elapsed_s >= (float)in->rule.time_stop_s) {
        return false;
    }
    return true;
}

// Plan sec 3 level 5: all four axes ANDed, `invert` negates the whole AND.
// Level 6 ("no rule for this segment -> OFF") is `!enable`, handled here
// too so the caller of this helper never needs a separate branch for it.
static bool evaluate_rule(const on_off_trigger_input_t *in, bool commanded_on_ref)
{
    if (!in->rule.enable) {
        return false;
    }
    bool result = axis_phase(in) && axis_direction(in) && axis_temp(in, commanded_on_ref) && axis_time(in);
    return in->rule.invert ? !result : result;
}

bool on_off_trigger_decide(on_off_trigger_state_t *state, const on_off_trigger_input_t *in)
{
    // --- quasi_dwell classifier (plan sec 4), updated every tick
    // regardless of which precedence level ends up deciding this tick's
    // verdict -- effective_dwell (current_phase_is_dwell) always reflects
    // the CALLER's read of state->quasi_dwell from BEFORE this call, so the
    // update below deliberately takes effect for the *next* tick, same lag
    // any other latch has relative to its own trigger.
    bool segment_changed = state->have_last_segment_index && state->last_segment_index != in->segment_index;
    if (segment_changed) {
        state->quasi_dwell = false;
        state->lock_true_s = 0.0f;
        state->lock_false_s = 0.0f;
    }
    state->last_segment_index = in->segment_index;
    state->have_last_segment_index = true;

    bool lock_active = in->ramp_lock_held && !in->stretched_this_tick;
    if (lock_active) {
        state->lock_true_s += in->dt_s;
        state->lock_false_s = 0.0f;
        if (!state->quasi_dwell && state->lock_true_s >= ON_OFF_QUASI_DWELL_ENTER_S) {
            state->quasi_dwell = true;
        }
    } else {
        state->lock_false_s += in->dt_s;
        state->lock_true_s = 0.0f;
        if (state->quasi_dwell && state->lock_false_s >= ON_OFF_QUASI_DWELL_EXIT_S) {
            state->quasi_dwell = false;
        }
    }

    // --- precedence, first match wins (plan sec 3) ---
    bool desired;
    if (in->failsafe_override) {
        /* level 1: safety trip / FAULTED / halt / abort / authority-block */
        desired = in->failsafe_state_on;
    } else if (in->guard_5_6_tripped) {
        /* level 2: guard 5/6 trip on this zone */
        desired = in->failsafe_state_on;
    } else if (!in->run_running) {
        /* level 3: run not RUNNING */
        if (in->run_paused && !in->failsafe_on_pause) {
            desired = state->commanded_on; /* PAUSE: hold last commanded state */
        } else {
            desired = in->failsafe_state_on; /* IDLE/DONE/FAULTED/halt, or PAUSE with failsafe_on_pause set */
        }
    } else {
        bool rule_result = evaluate_rule(in, state->commanded_on);
        if (rule_result != state->commanded_on) {
            /* level 4: minimum on/off dwell -- a transition is only allowed
             * once the CURRENT state has been held at least its own minimum
             * (min_on_s while ON, min_off_s while OFF). Not held long
             * enough blocks the flip; the rule's answer is simply not
             * applied yet, it is re-evaluated fresh next tick. */
            uint16_t required_hold = state->commanded_on ? in->min_on_s : in->min_off_s;
            desired = (state->held_s < (float)required_hold) ? state->commanded_on : rule_result;
        } else {
            desired = rule_result; /* == state->commanded_on already, no hold question */
        }
    }

    if (desired != state->commanded_on) {
        state->commanded_on = desired;
        state->held_s = in->dt_s;
    } else {
        state->held_s += in->dt_s;
    }
    return state->commanded_on;
}
