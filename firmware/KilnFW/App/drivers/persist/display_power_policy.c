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
        // RETURNS here -- it must not fall through. 2026-09-04 opus review:
        // this used to fall through into the ERROR_HOLD dismissal branch
        // below, which meant a touch edge landing on the SAME call that
        // first observed the error immediately dismissed the hold it had
        // just created -- state came back DISPLAY_POWER_ON with
        // swallow_touch=true, and the display then blanked on the ordinary
        // idle timeout while the error was still active. That directly
        // contradicts this branch's own rule above ("the hold must still
        // engage") and the header's rule 5, and it is reachable in normal
        // operation, not a corner: screen_idle.c computes
        // error_entered_this_tick inside screen_idle_run_policy_locked(),
        // which is called from BOTH the 20 Hz poll tick (screen_idle_task)
        // and every touch press edge (lvgl_port.c's touch_read_cb, a
        // different task) -- so whether the tick that first sees a new
        // error is a poll tick or a touch tick is a plain race between two
        // tasks. The old fall-through comment argued the dismissal could
        // only happen "on a LATER tick" while the code it was attached to
        // made it happen in the same call; the code, not the comment, is
        // what ran.
        //
        // A touch on this tick is swallowed rather than passed through: the
        // display state changed underneath the finger on this very call
        // (forced on, or held on), so acting on whatever widget is beneath
        // it is exactly the hazard rule 4 exists to prevent. The NEXT press
        // edge is the dismissal, via the branch below.
        out.swallow_touch = in->touch_event;
        return out;
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

bool display_power_touch_gate_press(display_power_touch_gate_t *gate, uint32_t now_ms)
{
    if (gate->held) {
        return false; // held repeat of the same press
    }
    gate->held = true;
    if (gate->have_release && gate->held_swallow &&
        (uint32_t)(now_ms - gate->last_release_ms) < DISPLAY_POWER_TOUCH_REPRESS_MS) {
        return false; // dropout inside a swallowed touch: same touch, still swallowed
    }
    return true;
}

void display_power_touch_gate_record(display_power_touch_gate_t *gate, bool swallow)
{
    gate->held_swallow = swallow;
}

void display_power_touch_gate_release(display_power_touch_gate_t *gate, uint32_t now_ms)
{
    if (gate->held) {
        gate->last_release_ms = now_ms;
        gate->have_release = true;
    }
    gate->held = false;
}
