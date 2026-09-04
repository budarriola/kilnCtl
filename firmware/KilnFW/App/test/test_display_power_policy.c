// Host tests for App/drivers/display_power_policy.c -- the pure display
// on/off/error-hold decision core for owner request 2026-09-04 (brightness +
// selectable idle timeout + keep-on-while-firing + wake-only-swallows-touch +
// display-on-error). Exercises display_power_policy_step() directly: no
// LVGL, no NVS, no FreeRTOS, exactly the point of keeping this module pure.
#include "test_common.h"

#include "../drivers/display_power_policy.c"

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

void run_test_display_power_policy(void)
{
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
    test_display_on_error_switch_off_error_does_not_force_hold();
}
