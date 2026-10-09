// Host tests for App/drivers/persist/display_power_policy.c -- the pure display
// on/off/error-hold decision core for owner request 2026-09-04 (brightness +
// selectable idle timeout + keep-on-while-firing + wake-only-swallows-touch +
// display-on-error). Exercises display_power_policy_step() directly: no
// LVGL, no NVS, no FreeRTOS, exactly the point of keeping this module pure.
#include "test_common.h"

#include "../drivers/persist/display_power_policy.c"

static display_power_input_t base_input(void)
{
    display_power_input_t in;
    in.now_ms = 0;
    in.last_touch_ms = 0;
    in.timeout_setting = DISPLAY_TIMEOUT_5_MIN;
    in.firing_active = false;
    in.keep_on_while_firing = false;
    in.error_active = false;
    in.error_entered_this_tick = false;
    in.display_on_error = false;
    in.current_state = DISPLAY_POWER_ON;
    in.touch_event = false;
    return in;
}

// ---------------------------------------------------------------------------
// display_power_timeout_ms() / display_power_timeout_setting_is_valid()
// ---------------------------------------------------------------------------

static void test_timeout_ms_mapping(void)
{
    TEST_CHECK(display_power_timeout_ms(DISPLAY_TIMEOUT_1_MIN) == 60000u, "1 min == 60000 ms");
    TEST_CHECK(display_power_timeout_ms(DISPLAY_TIMEOUT_5_MIN) == 300000u, "5 min == 300000 ms");
    TEST_CHECK(display_power_timeout_ms(DISPLAY_TIMEOUT_10_MIN) == 600000u, "10 min == 600000 ms");
    TEST_CHECK(display_power_timeout_ms(DISPLAY_TIMEOUT_15_MIN) == 900000u, "15 min == 900000 ms");
    TEST_CHECK(display_power_timeout_ms(DISPLAY_TIMEOUT_60_MIN) == 3600000u, "60 min == 3600000 ms");
    TEST_CHECK(display_power_timeout_ms(DISPLAY_TIMEOUT_NEVER) == UINT32_MAX, "never == UINT32_MAX sentinel");
    TEST_CHECK(display_power_timeout_ms((display_timeout_setting_t)99) == 0,
               "out-of-range setting maps to 0 (fails toward already-expired, not toward never)");
}

static void test_timeout_setting_validity(void)
{
    TEST_CHECK(display_power_timeout_setting_is_valid(DISPLAY_TIMEOUT_1_MIN), "1 min is valid");
    TEST_CHECK(display_power_timeout_setting_is_valid(DISPLAY_TIMEOUT_NEVER), "never is valid");
    TEST_CHECK(!display_power_timeout_setting_is_valid((display_timeout_setting_t)6), "6 is out of range");
    TEST_CHECK(!display_power_timeout_setting_is_valid((display_timeout_setting_t)-1), "-1 is out of range");
}

// ---------------------------------------------------------------------------
// Plain idle timeout, no firing/error involved.
// ---------------------------------------------------------------------------

static void test_stays_on_before_timeout(void)
{
    display_power_input_t in = base_input();
    in.now_ms = 299999; // 1 ms shy of the 5-minute timeout
    display_power_result_t out = display_power_policy_step(&in);
    TEST_CHECK(out.state == DISPLAY_POWER_ON, "1ms before timeout: still ON");
    TEST_CHECK(!out.swallow_touch, "no touch this tick: swallow_touch is false");
}

static void test_blanks_exactly_at_timeout(void)
{
    display_power_input_t in = base_input();
    in.now_ms = 300000; // exactly the 5-minute timeout
    display_power_result_t out = display_power_policy_step(&in);
    TEST_CHECK(out.state == DISPLAY_POWER_OFF, "elapsed == timeout: OFF (>=, not >)");
}

static void test_never_timeout_stays_on_indefinitely(void)
{
    display_power_input_t in = base_input();
    in.timeout_setting = DISPLAY_TIMEOUT_NEVER;
    in.now_ms = 100u * 3600u * 1000u; // 100 hours idle
    display_power_result_t out = display_power_policy_step(&in);
    TEST_CHECK(out.state == DISPLAY_POWER_ON, "Never: still ON after 100 hours idle");
}

static void test_already_off_stays_off_with_no_touch(void)
{
    display_power_input_t in = base_input();
    in.current_state = DISPLAY_POWER_OFF;
    in.now_ms = 1000000;
    display_power_result_t out = display_power_policy_step(&in);
    TEST_CHECK(out.state == DISPLAY_POWER_OFF, "already OFF, no touch: stays OFF");
    TEST_CHECK(!out.swallow_touch, "no touch this tick: swallow_touch is false");
}

// ---------------------------------------------------------------------------
// Rule 4: wake-only touch is swallowed; a touch while already ON passes
// through untouched.
// ---------------------------------------------------------------------------

static void test_touch_while_off_wakes_and_is_swallowed(void)
{
    display_power_input_t in = base_input();
    in.current_state = DISPLAY_POWER_OFF;
    in.touch_event = true;
    display_power_result_t out = display_power_policy_step(&in);
    TEST_CHECK(out.state == DISPLAY_POWER_ON, "touch while OFF: wakes to ON");
    TEST_CHECK(out.swallow_touch, "touch while OFF: swallowed (rule 4)");
}

static void test_touch_while_on_passes_through(void)
{
    display_power_input_t in = base_input();
    in.current_state = DISPLAY_POWER_ON;
    in.touch_event = true;
    display_power_result_t out = display_power_policy_step(&in);
    TEST_CHECK(out.state == DISPLAY_POWER_ON, "touch while ON: stays ON");
    TEST_CHECK(!out.swallow_touch, "touch while ON: NOT swallowed, acts on the UI");
}

static void test_timeout_expiring_exactly_at_a_touch_favours_the_touch(void)
{
    // The touch arrives on the exact tick the idle timeout would otherwise
    // have expired -- `last_touch_ms` is stale (the caller hasn't bumped it
    // yet), so the touch must win outright rather than being evaluated
    // against the timeout using the pre-touch timestamp.
    display_power_input_t in = base_input();
    in.current_state = DISPLAY_POWER_ON;
    in.now_ms = 300000; // == timeout
    in.last_touch_ms = 0;
    in.touch_event = true;
    display_power_result_t out = display_power_policy_step(&in);
    TEST_CHECK(out.state == DISPLAY_POWER_ON, "touch exactly at timeout: stays ON, not blanked");
    TEST_CHECK(!out.swallow_touch, "was already ON: touch acts on the UI, not swallowed");
}

// ---------------------------------------------------------------------------
// Rule 3: keep display on while firing overrides the idle timeout.
// ---------------------------------------------------------------------------

static void test_keep_on_while_firing_overrides_timeout(void)
{
    display_power_input_t in = base_input();
    in.firing_active = true;
    in.keep_on_while_firing = true;
    in.now_ms = 100u * 3600u * 1000u; // way past any timeout
    display_power_result_t out = display_power_policy_step(&in);
    TEST_CHECK(out.state == DISPLAY_POWER_ON, "firing + keep-on: stays ON despite huge idle time");
}

static void test_firing_without_keep_on_still_times_out(void)
{
    display_power_input_t in = base_input();
    in.firing_active = true;
    in.keep_on_while_firing = false; // switch is off
    in.now_ms = 300000;
    display_power_result_t out = display_power_policy_step(&in);
    TEST_CHECK(out.state == DISPLAY_POWER_OFF,
               "firing but keep-on switch OFF: normal timeout still applies");
}

static void test_keep_on_while_firing_does_not_apply_when_not_firing(void)
{
    display_power_input_t in = base_input();
    in.firing_active = false; // no firing right now
    in.keep_on_while_firing = true;
    in.now_ms = 300000;
    display_power_result_t out = display_power_policy_step(&in);
    TEST_CHECK(out.state == DISPLAY_POWER_OFF,
               "keep-on switch ON but not firing: normal timeout still applies");
}

// ---------------------------------------------------------------------------
// Rule 5 + rule 4 interaction: display-on-error.
// ---------------------------------------------------------------------------

static void test_error_forces_display_on_from_off(void)
{
    display_power_input_t in = base_input();
    in.current_state = DISPLAY_POWER_OFF;
    in.display_on_error = true;
    in.error_active = true;
    in.error_entered_this_tick = true;
    display_power_result_t out = display_power_policy_step(&in);
    TEST_CHECK(out.state == DISPLAY_POWER_ERROR_HOLD, "error arrives while OFF: forced to ERROR_HOLD");
    TEST_CHECK(!out.swallow_touch, "no touch this tick: nothing to swallow yet");
}

static void test_error_arriving_while_firing_keeps_display_on(void)
{
    // "error arrives while firing keeps it on" -- whether or not
    // keep_on_while_firing is even set, the error hold wins.
    display_power_input_t in = base_input();
    in.current_state = DISPLAY_POWER_OFF;
    in.firing_active = true;
    in.keep_on_while_firing = false;
    in.display_on_error = true;
    in.error_active = true;
    in.error_entered_this_tick = true;
    display_power_result_t out = display_power_policy_step(&in);
    TEST_CHECK(out.state == DISPLAY_POWER_ERROR_HOLD, "error while firing: ERROR_HOLD regardless of keep-on switch");
}

static void test_error_hold_ignores_idle_timeout(void)
{
    display_power_input_t in = base_input();
    in.current_state = DISPLAY_POWER_ERROR_HOLD;
    in.now_ms = 100u * 3600u * 1000u; // way past any timeout
    in.timeout_setting = DISPLAY_TIMEOUT_1_MIN;
    display_power_result_t out = display_power_policy_step(&in);
    TEST_CHECK(out.state == DISPLAY_POWER_ERROR_HOLD, "held for error: idle timeout does not apply");
}

static void test_touch_during_error_hold_resumes_and_is_swallowed(void)
{
    display_power_input_t in = base_input();
    in.current_state = DISPLAY_POWER_ERROR_HOLD;
    in.touch_event = true;
    // error_active/error_entered_this_tick both false: the error condition
    // itself may or may not still be present -- either way this touch is
    // the dismissal.
    display_power_result_t out = display_power_policy_step(&in);
    TEST_CHECK(out.state == DISPLAY_POWER_ON, "touch during error hold: resumes to ON");
    TEST_CHECK(out.swallow_touch, "touch during error hold: swallowed too (rule 4 via rule 5)");
}

static void test_touch_after_dismissal_resumes_normal_timeout_behaviour(void)
{
    // Once dismissed, normal timeout behaviour resumes even if the error is
    // still active at the device level (not yet cleared) -- only a NEW edge
    // (error_entered_this_tick) may force ERROR_HOLD again.
    display_power_input_t in = base_input();
    in.current_state = DISPLAY_POWER_ON; // already resumed by a prior dismissal
    in.error_active = true;              // error condition persists
    in.error_entered_this_tick = false;  // but it is not a NEW error this tick
    in.now_ms = 300000;                  // idle timeout elapsed
    display_power_result_t out = display_power_policy_step(&in);
    TEST_CHECK(out.state == DISPLAY_POWER_OFF,
               "dismissed error still active (no new edge): normal timeout applies, blanks as usual");
}

static void test_never_timeout_plus_error_still_forces_hold(void)
{
    // "Never" plus error: the error hold is a distinct state from the
    // timeout decision entirely, so DISPLAY_TIMEOUT_NEVER does not exempt an
    // error from forcing the display on.
    display_power_input_t in = base_input();
    in.timeout_setting = DISPLAY_TIMEOUT_NEVER;
    in.current_state = DISPLAY_POWER_ON; // "never" already means always-on
    in.display_on_error = true;
    in.error_active = true;
    in.error_entered_this_tick = true;
    display_power_result_t out = display_power_policy_step(&in);
    TEST_CHECK(out.state == DISPLAY_POWER_ERROR_HOLD, "Never + error: still forced to ERROR_HOLD (distinct state)");
}

// 2026-09-04 opus review, regression for a real defect: the error-entering
// tick used to FALL THROUGH into the ERROR_HOLD dismissal branch, so a touch
// press edge arriving on the same call that first observed the error
// dismissed the hold it had just created. Reachable in normal operation --
// screen_idle.c runs display_power_policy_step() from two different tasks
// (the 20 Hz poll tick and lvgl_port.c's touch_read_cb press edge), so which
// one first sees a newly-cached error is a race, not a corner.
static void test_error_entering_with_same_tick_touch_still_engages_hold(void)
{
    display_power_input_t in = base_input();
    in.current_state = DISPLAY_POWER_ON;
    in.display_on_error = true;
    in.error_active = true;
    in.error_entered_this_tick = true;
    in.touch_event = true; // a press edge on the very tick the error arrives
    display_power_result_t out = display_power_policy_step(&in);
    TEST_CHECK(out.state == DISPLAY_POWER_ERROR_HOLD,
               "error entering with a same-tick touch: the hold must still ENGAGE, not be "
               "dismissed by the touch that merely arrived alongside it");
    TEST_CHECK(out.swallow_touch,
               "error entering with a same-tick touch: that touch is swallowed (the display "
               "state changed under the finger), it must not act on the UI beneath it");
}

// The other half of the same defect: having engaged, the hold must survive
// until a LATER press edge dismisses it. Before the fix the hold never
// existed to be dismissed, so this sequence ended ON and then blanked on the
// ordinary idle timeout with the error still active.
static void test_hold_engaged_by_same_tick_touch_is_dismissed_only_by_a_later_touch(void)
{
    display_power_input_t in = base_input();
    in.current_state = DISPLAY_POWER_ON;
    in.display_on_error = true;
    in.error_active = true;
    in.error_entered_this_tick = true;
    in.touch_event = true;
    display_power_result_t out = display_power_policy_step(&in);
    TEST_CHECK(out.state == DISPLAY_POWER_ERROR_HOLD, "tick 1: hold engaged");

    // Tick 2: no new error edge, no touch, and the idle timeout long past --
    // the hold must still be in force.
    in.current_state = out.state;
    in.error_entered_this_tick = false;
    in.touch_event = false;
    in.now_ms = 100u * 3600u * 1000u;
    in.timeout_setting = DISPLAY_TIMEOUT_1_MIN;
    out = display_power_policy_step(&in);
    TEST_CHECK(out.state == DISPLAY_POWER_ERROR_HOLD,
               "tick 2: hold survives the idle timeout -- it was never dismissed");

    // Tick 3: the next real press edge is the dismissal.
    in.current_state = out.state;
    in.touch_event = true;
    out = display_power_policy_step(&in);
    TEST_CHECK(out.state == DISPLAY_POWER_ON, "tick 3: a LATER touch dismisses the hold");
    TEST_CHECK(out.swallow_touch, "tick 3: the dismissing touch is swallowed");
}

// Negative-direction guard on the same branch: with the display-on-error
// switch OFF, a same-tick touch must behave exactly as it always did (wake
// from OFF, swallowed) and must NOT be diverted into the error path.
static void test_switch_off_same_tick_touch_still_takes_the_normal_wake_path(void)
{
    display_power_input_t in = base_input();
    in.current_state = DISPLAY_POWER_OFF;
    in.display_on_error = false; // switch OFF
    in.error_active = true;
    in.error_entered_this_tick = true;
    in.touch_event = true;
    display_power_result_t out = display_power_policy_step(&in);
    TEST_CHECK(out.state == DISPLAY_POWER_ON,
               "switch off + same-tick touch while OFF: ordinary rule-4 wake, no error hold");
    TEST_CHECK(out.swallow_touch, "switch off + same-tick touch while OFF: wake touch swallowed");
}

static void test_display_on_error_switch_off_error_does_not_force_hold(void)
{
    // If the owner has left the "display on error" switch off, an error
    // must NOT force the display on -- this proves the switch actually
    // gates the behaviour rather than the error always winning.
    display_power_input_t in = base_input();
    in.current_state = DISPLAY_POWER_OFF;
    in.display_on_error = false; // switch OFF
    in.error_active = true;
    in.error_entered_this_tick = true;
    display_power_result_t out = display_power_policy_step(&in);
    TEST_CHECK(out.state == DISPLAY_POWER_OFF, "display-on-error switch OFF: error does not force the display on");
}

// ---------------------------------------------------------------------------
// Touch gate (release debounce): a controller dropout mid-wake-touch must not
// let the continuing touch through as a fresh press (2026-10-07).
// ---------------------------------------------------------------------------

static void test_gate_wake_touch_dropout_stays_swallowed(void)
{
    display_power_touch_gate_t g = {0};
    TEST_CHECK(display_power_touch_gate_press(&g, 1000), "first press is an edge");
    display_power_touch_gate_record(&g, true); // wake swallow
    TEST_CHECK(!display_power_touch_gate_press(&g, 1030), "held repeat is not an edge");
    display_power_touch_gate_release(&g, 1060); // dropout
    TEST_CHECK(!display_power_touch_gate_press(&g, 1090),
               "re-press 30 ms after a swallowed touch's release is NOT a new edge");
    TEST_CHECK(g.held_swallow, "and the verdict is still swallow");
    display_power_touch_gate_release(&g, 1200);
    TEST_CHECK(!display_power_touch_gate_press(&g, 1200 + DISPLAY_POWER_TOUCH_REPRESS_MS - 1),
               "re-press just inside the window is still the same touch");
}

static void test_gate_new_touch_after_window_is_an_edge(void)
{
    display_power_touch_gate_t g = {0};
    (void)display_power_touch_gate_press(&g, 1000);
    display_power_touch_gate_record(&g, true);
    display_power_touch_gate_release(&g, 1100);
    TEST_CHECK(display_power_touch_gate_press(&g, 1100 + DISPLAY_POWER_TOUCH_REPRESS_MS),
               "a press a full window after the release is a fresh edge");
}

static void test_gate_passed_touch_repress_is_a_fresh_edge(void)
{
    display_power_touch_gate_t g = {0};
    (void)display_power_touch_gate_press(&g, 1000);
    display_power_touch_gate_record(&g, false); // passed through
    display_power_touch_gate_release(&g, 1050);
    TEST_CHECK(display_power_touch_gate_press(&g, 1100),
               "quick second tap after a PASSED touch is never debounced away");
}

static void test_gate_release_without_press_is_harmless(void)
{
    display_power_touch_gate_t g = {0};
    display_power_touch_gate_release(&g, 500);
    TEST_CHECK(display_power_touch_gate_press(&g, 510), "poll-tick releases with nothing held do not arm the debounce");
}

void run_test_display_power_policy(void)
{
    test_gate_wake_touch_dropout_stays_swallowed();
    test_gate_new_touch_after_window_is_an_edge();
    test_gate_passed_touch_repress_is_a_fresh_edge();
    test_gate_release_without_press_is_harmless();
    test_timeout_ms_mapping();
    test_timeout_setting_validity();
    test_stays_on_before_timeout();
    test_blanks_exactly_at_timeout();
    test_never_timeout_stays_on_indefinitely();
    test_already_off_stays_off_with_no_touch();
    test_touch_while_off_wakes_and_is_swallowed();
    test_touch_while_on_passes_through();
    test_timeout_expiring_exactly_at_a_touch_favours_the_touch();
    test_keep_on_while_firing_overrides_timeout();
    test_firing_without_keep_on_still_times_out();
    test_keep_on_while_firing_does_not_apply_when_not_firing();
    test_error_forces_display_on_from_off();
    test_error_arriving_while_firing_keeps_display_on();
    test_error_hold_ignores_idle_timeout();
    test_touch_during_error_hold_resumes_and_is_swallowed();
    test_touch_after_dismissal_resumes_normal_timeout_behaviour();
    test_never_timeout_plus_error_still_forces_hold();
    test_error_entering_with_same_tick_touch_still_engages_hold();
    test_hold_engaged_by_same_tick_touch_is_dismissed_only_by_a_later_touch();
    test_switch_off_same_tick_touch_still_takes_the_normal_wake_path();
    test_display_on_error_switch_off_error_does_not_force_hold();
}
