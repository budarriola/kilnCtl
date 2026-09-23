// Host tests for App/drivers/bridge/boot_button.c -- the BOOT-button (GPIO0)
// long-press recovery hatch behind the owner's "I lost the AP password"
// request (see boot_button.h's header comment for the full feature).
//
// #includes boot_button.c directly (same convention as test_boot_guard.c/
// test_watchdog_cfg.c) to reach its two pure decision functions:
//   - boot_button_step() -- the hold-detector state machine
//   - state_refuses_bypass() -- "is this profile_exec_state_t a firing"
// Both are exercised directly here; NEITHER touches GPIO, a FreeRTOS
// mutex, or a task -- boot_button_start()/boot_button_task()/
// handle_open_requested() are deliberately NOT exercised by these tests
// (see the note below on why).
//
// A fake profile_executor_get_status() is defined below, NOT because any
// test here calls it (none do -- only the pure functions above are under
// test), but because boot_button.c's handle_open_requested() references
// the real one, and this translation unit is linked into the MAIN host-test
// executable, which does not otherwise link profile_executor.c (or
// MAX31856.c/kiln_io.c/profiles_http.c/safety_link.c -- profile_executor.h
// pulls in their declarations, never their definitions, and nothing here
// calls into any of them). Same "own fake body so the TU links" reasoning
// test_profile_executor_prestart.c already documents for zones_config_*()/
// profiles_http_get(); the difference here is those symbols never actually
// run in EITHER test file, whereas that one's guard-under-test does call
// through to some of its fakes.
//
// WHY handle_open_requested()/boot_button_ota_bypass_active()/etc. are NOT
// host-tested here: they take s_bb.lock via xSemaphoreTake(..., pdMS_TO_
// TICKS(50)) and treat a failed take as "refuse/report inactive" (the same
// fail-safe convention ota_http.c's own lock uses). stubs/freertos/
// semphr.h's xSemaphoreTake() stub deliberately always returns pdFALSE
// (see that header's own comment -- it exists to prove OTHER modules'
// lock-timeout fallback paths, like profile_executor.c's pre-start guard,
// fail loudly rather than silently). Under that stub, every lock-guarded
// branch in boot_button.c always takes its "could not take the lock"
// path -- exercising that path only proves the stub works, not this
// module's actual open/refuse decision. The two PURE functions below carry
// that decision instead, are unaffected by the lock stub, and are exactly
// what boot_button.h's own header comment describes splitting out for this
// reason -- same rationale as boot_guard.c's next_boot_count()/
// record_is_valid() split.
#include <string.h>

#include "test_common.h"

#include "esp_err.h"

#include "../drivers/control/profile_executor.h"

/* See this file's header comment. Never actually invoked by any test below
 * (state_refuses_bypass() and boot_button_step() are pure and call
 * nothing) -- exists purely so ../drivers/bridge/boot_button.c links. */
void profile_executor_get_status(profile_exec_status_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
}

/* Same reasoning as profile_executor_get_status() above -- handle_open_
 * requested() now calls this narrow accessor instead of the full-struct
 * getter (2026-09-22 exec-status stack-local audit), so this TU needs a
 * fake for it too. Reports "no firing" (false), matching the zeroed
 * profile_executor_get_status() fake's PROFILE_EXEC_IDLE-equivalent
 * behavior above. */
bool profile_executor_get_active_id(uint8_t *out_id)
{
    if (out_id) {
        *out_id = 0;
    }
    return false;
}

#include "../drivers/bridge/boot_button.c"

// ---------------------------------------------------------------------------
// boot_button_step() -- the hold-detector state machine.
// ---------------------------------------------------------------------------

static void test_step_short_press_no_fire(void)
{
    boot_button_press_state_t st;
    memset(&st, 0, sizeof(st));

    // Press begins at t=0.
    boot_button_event_t ev = boot_button_step(&st, true, 0);
    TEST_CHECK(ev == BOOT_BUTTON_EVENT_NONE, "press begins: no event on the rising edge");

    // Held for less than BOOT_BUTTON_HOLD_MS -- must not fire.
    ev = boot_button_step(&st, true, BOOT_BUTTON_HOLD_MS - 1);
    TEST_CHECK(ev == BOOT_BUTTON_EVENT_NONE, "held just under the threshold: no event yet");

    // Released before crossing the threshold.
    ev = boot_button_step(&st, false, BOOT_BUTTON_HOLD_MS + 500);
    TEST_CHECK(ev == BOOT_BUTTON_EVENT_NONE, "released before threshold: still no event");
}

static void test_step_fires_exactly_once_at_threshold(void)
{
    boot_button_press_state_t st;
    memset(&st, 0, sizeof(st));

    boot_button_step(&st, true, 0); /* rising edge */

    // The tick BEFORE the threshold: must not fire yet.
    boot_button_event_t ev = boot_button_step(&st, true, BOOT_BUTTON_HOLD_MS - 1);
    TEST_CHECK(ev == BOOT_BUTTON_EVENT_NONE, "one tick before the threshold: no event");

    // The exact tick the hold crosses BOOT_BUTTON_HOLD_MS: fires.
    ev = boot_button_step(&st, true, BOOT_BUTTON_HOLD_MS);
    TEST_CHECK(ev == BOOT_BUTTON_EVENT_OPEN_REQUESTED, "hold crosses the threshold: fires OPEN_REQUESTED");

    // THIS is the negative-tested behavior: a continued hold past the
    // threshold must NOT re-fire on every subsequent tick. (Temporarily
    // removing the `st->fired_this_press` latch check in boot_button_step()
    // makes this assertion fail -- see the task report for the exact
    // failure output captured with that break in place.)
    ev = boot_button_step(&st, true, BOOT_BUTTON_HOLD_MS + 100);
    TEST_CHECK(ev == BOOT_BUTTON_EVENT_NONE, "continued hold, +100ms past threshold: does NOT re-fire");

    ev = boot_button_step(&st, true, BOOT_BUTTON_HOLD_MS + 60000);
    TEST_CHECK(ev == BOOT_BUTTON_EVENT_NONE, "continued hold, +60s past threshold: still does NOT re-fire");
}

static void test_step_release_then_new_press_fires_again(void)
{
    boot_button_press_state_t st;
    memset(&st, 0, sizeof(st));

    boot_button_step(&st, true, 0);
    boot_button_event_t ev = boot_button_step(&st, true, BOOT_BUTTON_HOLD_MS);
    TEST_CHECK(ev == BOOT_BUTTON_EVENT_OPEN_REQUESTED, "first press: fires at the threshold");

    // Release -- must reset the latch and the press-start time.
    ev = boot_button_step(&st, false, BOOT_BUTTON_HOLD_MS + 200);
    TEST_CHECK(ev == BOOT_BUTTON_EVENT_NONE, "release: no event");
    TEST_CHECK(!st.pressed_last, "release: pressed_last cleared");
    TEST_CHECK(!st.fired_this_press, "release: fired_this_press cleared");

    // A brand new press, well after the first, must be able to fire again
    // once IT crosses the threshold -- proves this isn't a one-shot latch
    // for the whole module, only for a single continuous press.
    uint32_t new_press_start = BOOT_BUTTON_HOLD_MS + 1000;
    ev = boot_button_step(&st, true, new_press_start); /* rising edge #2 */
    TEST_CHECK(ev == BOOT_BUTTON_EVENT_NONE, "second press begins: no event on the rising edge");

    ev = boot_button_step(&st, true, new_press_start + BOOT_BUTTON_HOLD_MS - 1);
    TEST_CHECK(ev == BOOT_BUTTON_EVENT_NONE, "second press, one tick before its own threshold: no event");

    ev = boot_button_step(&st, true, new_press_start + BOOT_BUTTON_HOLD_MS);
    TEST_CHECK(ev == BOOT_BUTTON_EVENT_OPEN_REQUESTED, "second press crosses ITS threshold: fires again");
}

static void test_step_millisecond_wraparound(void)
{
    // Press starts near the top of the uint32_t range, crosses the
    // threshold AFTER wrapping through 0 -- the same
    // "(uint32_t)(now - started)" subtraction ota_auth.c's nonce-expiry
    // check uses is correct across this wrap (both mod 2^32); a naive
    // `now >= started + threshold` comparison would NOT be, since
    // started + BOOT_BUTTON_HOLD_MS overflows into a tiny number that
    // `now` (also tiny, post-wrap) would incorrectly already exceed on the
    // very first post-wrap tick.
    boot_button_press_state_t st;
    memset(&st, 0, sizeof(st));

    uint32_t start = (uint32_t)0u - 2000u; /* 2000ms before the wrap */
    boot_button_step(&st, true, start); /* rising edge */

    // 1000ms into the press (still pre-wrap): under threshold, no fire.
    uint32_t t1 = start + 1000u; /* has not wrapped yet: still < start */
    boot_button_event_t ev = boot_button_step(&st, true, t1);
    TEST_CHECK(ev == BOOT_BUTTON_EVENT_NONE, "wraparound case: 1s held (pre-wrap), no fire yet");

    // BOOT_BUTTON_HOLD_MS after start, having wrapped through 0: must fire
    // on schedule, exactly as if no wraparound had occurred.
    uint32_t t2 = (uint32_t)(start + BOOT_BUTTON_HOLD_MS); /* wraps */
    ev = boot_button_step(&st, true, t2);
    TEST_CHECK(ev == BOOT_BUTTON_EVENT_OPEN_REQUESTED,
               "wraparound case: hold crosses the threshold correctly across a uint32_t wrap");

    // And still does not re-fire on a further post-wrap tick.
    ev = boot_button_step(&st, true, t2 + 500u);
    TEST_CHECK(ev == BOOT_BUTTON_EVENT_NONE, "wraparound case: continued hold past threshold does not re-fire");
}

// ---------------------------------------------------------------------------
// state_refuses_bypass() -- the refusal rule (RUNNING/PAUSED refuse; IDLE/
// DONE/FAULTED allow -- see boot_button.h's "WHY A FIRING BLOCKS IT").
// ---------------------------------------------------------------------------

static void test_state_refuses_bypass(void)
{
    // THIS is the other negative-tested behavior: "refused during a
    // firing". Temporarily changing state_refuses_bypass() to, e.g., only
    // check `state == PROFILE_EXEC_RUNNING` (dropping PAUSED) makes the
    // PAUSED assertion below fail -- see the task report for the captured
    // failure output with that break in place.
    TEST_CHECK(state_refuses_bypass(PROFILE_EXEC_RUNNING), "RUNNING refuses the bypass window");
    TEST_CHECK(state_refuses_bypass(PROFILE_EXEC_PAUSED), "PAUSED refuses the bypass window");

    TEST_CHECK(!state_refuses_bypass(PROFILE_EXEC_IDLE), "IDLE allows the bypass window");
    TEST_CHECK(!state_refuses_bypass(PROFILE_EXEC_DONE), "DONE allows the bypass window");
    // FAULTED is the state most likely to be exactly the situation a
    // locked-out operator is stuck in (see boot_button.h) -- relays are
    // already off, latched by the guard that faulted the run, so there is
    // no live firing left for this hatch to interrupt.
    TEST_CHECK(!state_refuses_bypass(PROFILE_EXEC_FAULTED), "FAULTED allows the bypass window");
}

void run_test_boot_button(void)
{
    test_step_short_press_no_fire();
    test_step_fires_exactly_once_at_threshold();
    test_step_release_then_new_press_fires_again();
    test_step_millisecond_wraparound();
    test_state_refuses_bypass();
}
