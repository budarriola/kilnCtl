#include "display_power_policy.h"

uint32_t display_power_timeout_ms(display_timeout_setting_t setting)
{
    switch (setting) {
    case DISPLAY_TIMEOUT_1_MIN:  return 60u * 1000u;
    case DISPLAY_TIMEOUT_5_MIN:  return 5u * 60u * 1000u;
    case DISPLAY_TIMEOUT_10_MIN: return 10u * 60u * 1000u;
    case DISPLAY_TIMEOUT_15_MIN: return 15u * 60u * 1000u;
    case DISPLAY_TIMEOUT_60_MIN: return 60u * 60u * 1000u;
    case DISPLAY_TIMEOUT_NEVER:  return UINT32_MAX; // sentinel -- see header comment; never compared against
    default: return 0; // out-of-range: fail toward "already expired", never toward "never" -- see header comment
    }
}

bool display_power_timeout_setting_is_valid(display_timeout_setting_t setting)
{
    return setting >= DISPLAY_TIMEOUT_1_MIN && setting < DISPLAY_TIMEOUT_COUNT;
}

display_power_result_t display_power_policy_step(const display_power_input_t *in)
{
    display_power_result_t out = { .state = in->current_state, .swallow_touch = false };

    // Rule 5: an error arriving forces the display on and holds it, no
    // matter what state it was already in (ON, OFF, or already mid-hold from
    // a prior error) and no matter what "keep on while firing" or the
    // timeout setting say -- an error is more urgent than either. This check
    // runs before everything else specifically so it wins over a same-tick
    // touch_event too: a touch landing on the exact tick a NEW error starts
    // is not the dismissing touch for THIS error, it is unrelated input that
    // arrived first -- the hold must still engage.
    if (in->error_entered_this_tick && in->display_on_error) {
        out.state = DISPLAY_POWER_ERROR_HOLD;
        // Deliberately not consuming touch_event here: if a touch also
        // landed this same tick, it is still eligible to be swallowed by the
        // ERROR_HOLD touch-dismissal branch below -- but only on a LATER
        // tick, since this function only sees one edge at a time and this
        // tick's touch_event (if any) has not been evaluated against the
        // new state yet. Falling through (not `return`) lets that happen in
        // the same call when both are true.
    }

    if (out.state == DISPLAY_POWER_ERROR_HOLD) {
        if (in->touch_event) {
            // Rule 5's last sentence, via rule 4: the dismissing touch wakes
            // (no-op here, display was already forced on) and resumes normal
            // timeout behaviour, but does not itself act on the UI.
            out.state = DISPLAY_POWER_ON;
            out.swallow_touch = true;
        }
        // No idle-timeout check while held -- that only applies to
        // DISPLAY_POWER_ON below, and only on a tick that reaches it.
        return out;
    }

    if (in->touch_event) {
        if (out.state == DISPLAY_POWER_OFF) {
            // Rule 4: this touch only wakes the screen, it must not act on
            // the UI underneath.
            out.state = DISPLAY_POWER_ON;
            out.swallow_touch = true;
        }
        // A touch while already ON passes through untouched (acts on the
        // UI normally) -- and is not evaluated against the idle timeout
        // this tick either way: `last_touch_ms` the caller passed in is
        // stale as of THIS touch (the caller updates it only after this
        // call returns, same as screen_idle_mark_active()'s ordering), so
        // computing elapsed-since-last-touch on a touch tick would compare
        // against the wrong, pre-touch timestamp. Returning here instead of
        // falling into the timeout check below is what keeps "timeout
        // expiring exactly at a touch" resolved in the touch's favour.
        return out;
    }

    if (out.state == DISPLAY_POWER_ON) {
        bool never = (in->timeout_setting == DISPLAY_TIMEOUT_NEVER);
        bool held_on_for_firing = in->firing_active && in->keep_on_while_firing;
        if (!never && !held_on_for_firing) {
            uint32_t timeout_ms = display_power_timeout_ms(in->timeout_setting);
            // Unsigned subtraction: correct across a wraparound of `now_ms`
            // the same way screen_idle.c's TickType_t elapsed-ticks
            // subtraction already relies on for its ~49-day tick rollover.
            uint32_t elapsed_ms = in->now_ms - in->last_touch_ms;
            if (elapsed_ms >= timeout_ms) {
                out.state = DISPLAY_POWER_OFF;
            }
        }
    }

    return out;
}
