// Host tests for App/drivers/net/auth_reset_gesture.c --
// docs/WEB_AUTH_PLAN.md item 10, the physical credential-reset gesture. No
// ESP-IDF dependency.
#include <string.h>

#include "test_common.h"

#include "../drivers/net/auth_reset_gesture.h"

/* --- fake clear_credentials_fn seam --------------------------------- */

static int s_fake_clear_calls;
static bool s_fake_clear_result;

static bool fake_clear_ok(void)
{
    s_fake_clear_calls++;
    return s_fake_clear_result;
}

static void reset_fake_clear(bool result)
{
    s_fake_clear_calls = 0;
    s_fake_clear_result = result;
}

static void tap_full_sequence(auth_reset_gesture_state_t *s, uint32_t start_ms,
                               bool estop, bool firing, bool heat)
{
    auth_reset_gesture_on_corner_tap(s, AUTH_RESET_CORNER_TOP_LEFT, start_ms, estop, firing, heat);
    auth_reset_gesture_on_corner_tap(s, AUTH_RESET_CORNER_TOP_RIGHT, start_ms + 100, estop, firing, heat);
    auth_reset_gesture_on_corner_tap(s, AUTH_RESET_CORNER_BOTTOM_LEFT, start_ms + 200, estop, firing, heat);
    auth_reset_gesture_on_corner_tap(s, AUTH_RESET_CORNER_BOTTOM_RIGHT, start_ms + 300, estop, firing, heat);
}

static void test_preconditions(void)
{
    TEST_SECTION("auth_reset_gesture -- preconditions predicate");

    TEST_CHECK(auth_reset_gesture_preconditions_met(true, false, false),
               "estop asserted, no firing, no heat -- preconditions met");
    TEST_CHECK(!auth_reset_gesture_preconditions_met(false, false, false),
               "estop NOT asserted -- refused");
    TEST_CHECK(!auth_reset_gesture_preconditions_met(true, true, false),
               "firing active -- refused");
    TEST_CHECK(!auth_reset_gesture_preconditions_met(true, false, true),
               "heat enabled -- refused");
    TEST_CHECK(!auth_reset_gesture_preconditions_met(false, true, true),
               "everything wrong -- refused");
}

static void test_full_correct_gesture_arms_and_clears(void)
{
    TEST_SECTION("auth_reset_gesture -- full correct gesture arms, confirm clears");

    auth_reset_gesture_state_t s;
    auth_reset_gesture_reset(&s);
    reset_fake_clear(true);
    s.clear_credentials_fn = fake_clear_ok;

    auth_reset_gesture_tap_result_t r1 =
        auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_TOP_LEFT, 1000, true, false, false);
    TEST_CHECK(r1 == AUTH_RESET_TAP_PROGRESS, "1st correct corner -- PROGRESS");

    auth_reset_gesture_tap_result_t r2 =
        auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_TOP_RIGHT, 1100, true, false, false);
    TEST_CHECK(r2 == AUTH_RESET_TAP_PROGRESS, "2nd correct corner -- PROGRESS");

    auth_reset_gesture_tap_result_t r3 =
        auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_BOTTOM_LEFT, 1200, true, false, false);
    TEST_CHECK(r3 == AUTH_RESET_TAP_PROGRESS, "3rd correct corner -- PROGRESS");

    auth_reset_gesture_tap_result_t r4 =
        auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_BOTTOM_RIGHT, 1300, true, false, false);
    TEST_CHECK(r4 == AUTH_RESET_TAP_ARMED, "4th correct corner within window -- ARMED");
    TEST_CHECK(auth_reset_gesture_is_armed_and_live(&s, 1300), "armed and live right after arming");

    auth_reset_gesture_confirm_result_t cr = auth_reset_gesture_confirm(&s, 1400);
    TEST_CHECK(cr == AUTH_RESET_CONFIRM_OK, "confirm within window with a wired, succeeding seam -- OK");
    TEST_CHECK(s_fake_clear_calls == 1, "seam called exactly once");
    TEST_CHECK(!s.armed, "state returns to idle after a successful confirm");
    TEST_CHECK(!auth_reset_gesture_is_armed_and_live(&s, 1400), "no longer armed after confirm");
}

static void test_out_of_order_does_not_arm(void)
{
    TEST_SECTION("auth_reset_gesture -- out-of-order taps do not arm, and abandon cleanly");

    auth_reset_gesture_state_t s;
    auth_reset_gesture_reset(&s);
    reset_fake_clear(true);
    s.clear_credentials_fn = fake_clear_ok;

    /* Correct first corner, then a wrong one instead of TOP_RIGHT. */
    auth_reset_gesture_tap_result_t r1 =
        auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_TOP_LEFT, 1000, true, false, false);
    TEST_CHECK(r1 == AUTH_RESET_TAP_PROGRESS, "1st correct corner -- PROGRESS");

    auth_reset_gesture_tap_result_t r2 =
        auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_BOTTOM_RIGHT, 1100, true, false, false);
    TEST_CHECK(r2 == AUTH_RESET_TAP_ABANDONED, "out-of-order corner -- ABANDONED, not latched");
    TEST_CHECK(!s.armed, "not armed after an out-of-order tap");

    /* Confirming now must be refused -- nothing is armed. */
    TEST_CHECK(auth_reset_gesture_confirm(&s, 1200) == AUTH_RESET_CONFIRM_NOT_ARMED,
               "confirm after an abandoned sequence -- NOT_ARMED");
    TEST_CHECK(s_fake_clear_calls == 0, "seam never called for an out-of-order sequence");

    /* A subsequent, correct, full sequence must still work -- abandoning
     * must not leave the state machine stuck. */
    tap_full_sequence(&s, 2000, true, false, false);
    TEST_CHECK(s.armed, "a fresh correct sequence after an abandon still arms");
    TEST_CHECK(auth_reset_gesture_confirm(&s, 2400) == AUTH_RESET_CONFIRM_OK,
               "and still confirms/clears normally");
}

static void test_missing_corner_does_not_arm(void)
{
    TEST_SECTION("auth_reset_gesture -- a missing corner (only 3 of 4) does not arm");

    auth_reset_gesture_state_t s;
    auth_reset_gesture_reset(&s);
    reset_fake_clear(true);
    s.clear_credentials_fn = fake_clear_ok;

    auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_TOP_LEFT, 1000, true, false, false);
    auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_TOP_RIGHT, 1100, true, false, false);
    auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_BOTTOM_LEFT, 1200, true, false, false);
    /* Stop here -- never tap BOTTOM_RIGHT. */
    TEST_CHECK(!s.armed, "three correct taps, no fourth -- never armed");

    TEST_CHECK(auth_reset_gesture_confirm(&s, 1300) == AUTH_RESET_CONFIRM_NOT_ARMED,
               "confirm with a missing corner -- NOT_ARMED");
    TEST_CHECK(s_fake_clear_calls == 0, "seam never called");
}

static void test_taps_without_estop_do_not_progress(void)
{
    TEST_SECTION("auth_reset_gesture -- taps without E-stop asserted do not clear");

    auth_reset_gesture_state_t s;
    auth_reset_gesture_reset(&s);
    reset_fake_clear(true);
    s.clear_credentials_fn = fake_clear_ok;

    /* estop_asserted = false throughout. */
    auth_reset_gesture_tap_result_t r1 =
        auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_TOP_LEFT, 1000, false, false, false);
    TEST_CHECK(r1 == AUTH_RESET_TAP_IGNORED_PRECONDITIONS, "no E-stop -- tap ignored outright");

    tap_full_sequence(&s, 2000, false, false, false);
    TEST_CHECK(!s.armed, "full 'sequence' with no E-stop never arms");
    TEST_CHECK(auth_reset_gesture_confirm(&s, 2500) == AUTH_RESET_CONFIRM_NOT_ARMED,
               "confirm afterward -- NOT_ARMED");
    TEST_CHECK(s_fake_clear_calls == 0, "seam never called without E-stop");
}

static void test_firing_or_heat_blocks_the_gesture(void)
{
    TEST_SECTION("auth_reset_gesture -- a firing running, or heat enabled, blocks the gesture");

    auth_reset_gesture_state_t s;

    auth_reset_gesture_reset(&s);
    reset_fake_clear(true);
    s.clear_credentials_fn = fake_clear_ok;
    tap_full_sequence(&s, 1000, true, /*firing_active=*/true, false);
    TEST_CHECK(!s.armed, "E-stop asserted but a firing is active -- never arms");

    auth_reset_gesture_reset(&s);
    reset_fake_clear(true);
    s.clear_credentials_fn = fake_clear_ok;
    tap_full_sequence(&s, 1000, true, false, /*heat_enabled=*/true);
    TEST_CHECK(!s.armed, "E-stop asserted but heat is enabled -- never arms");
}

static void test_estop_released_mid_sequence_aborts(void)
{
    TEST_SECTION("auth_reset_gesture -- E-stop released mid-sequence aborts, even with earlier correct taps");

    auth_reset_gesture_state_t s;
    auth_reset_gesture_reset(&s);
    reset_fake_clear(true);
    s.clear_credentials_fn = fake_clear_ok;

    auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_TOP_LEFT, 1000, true, false, false);
    auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_TOP_RIGHT, 1100, true, false, false);
    TEST_CHECK(s.next_index == 2, "two correct taps registered so far");

    /* E-stop released between taps. */
    auth_reset_gesture_tap_result_t r =
        auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_BOTTOM_LEFT, 1200, false, false, false);
    TEST_CHECK(r == AUTH_RESET_TAP_IGNORED_PRECONDITIONS, "tap while E-stop released -- ignored");
    TEST_CHECK(s.next_index == 0, "in-progress sequence was abandoned when E-stop was released");

    /* Re-asserting E-stop and retapping from the start still works. */
    tap_full_sequence(&s, 2000, true, false, false);
    TEST_CHECK(s.armed, "a fresh sequence after E-stop is re-asserted still arms cleanly");
}

static void test_sequence_window_expiry(void)
{
    TEST_SECTION("auth_reset_gesture -- the 10 s sequence window");

    auth_reset_gesture_state_t s;
    auth_reset_gesture_reset(&s);
    reset_fake_clear(true);
    s.clear_credentials_fn = fake_clear_ok;

    auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_TOP_LEFT, 0, true, false, false);
    auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_TOP_RIGHT, 1000, true, false, false);

    /* Third tap arrives just past the 10 s budget measured from the first
     * correct tap. */
    uint32_t late_ms = AUTH_RESET_GESTURE_SEQUENCE_WINDOW_MS + 1;
    auth_reset_gesture_tap_result_t r =
        auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_TOP_LEFT, late_ms, true, false, false);
    /* The stale sequence is abandoned before this tap is evaluated; since
     * TOP_LEFT is correct for position 0 (the fixed order's first corner),
     * it starts a brand-new in-progress sequence rather than completing the
     * old one. */
    TEST_CHECK(r == AUTH_RESET_TAP_PROGRESS, "tap past the window starts a fresh sequence, not a completion");
    TEST_CHECK(s.next_index == 1, "the stale sequence was discarded, this tap began a new one at position 1");
    TEST_CHECK(!s.armed, "not armed -- only one corner of the new sequence has landed");
}

static void test_confirm_window_expiry(void)
{
    TEST_SECTION("auth_reset_gesture -- the 30 s confirm window");

    auth_reset_gesture_state_t s;
    auth_reset_gesture_reset(&s);
    reset_fake_clear(true);
    s.clear_credentials_fn = fake_clear_ok;

    tap_full_sequence(&s, 1000, true, false, false);
    TEST_CHECK(s.armed, "armed after the full sequence");

    uint32_t armed_at = 1300; /* per tap_full_sequence's 4th tap timestamp */
    uint32_t too_late = armed_at + AUTH_RESET_GESTURE_CONFIRM_WINDOW_MS + 1;
    TEST_CHECK(!auth_reset_gesture_is_armed_and_live(&s, too_late), "no longer live past the confirm window");

    auth_reset_gesture_confirm_result_t cr = auth_reset_gesture_confirm(&s, too_late);
    TEST_CHECK(cr == AUTH_RESET_CONFIRM_EXPIRED, "confirm past the 30 s window -- EXPIRED, not OK");
    TEST_CHECK(s_fake_clear_calls == 0, "seam never called on an expired confirm");
    TEST_CHECK(!s.armed, "state returns to idle even on expiry");
}

static void test_not_wired_and_clear_failed(void)
{
    TEST_SECTION("auth_reset_gesture -- confirm outcomes when the seam is unwired or fails");

    auth_reset_gesture_state_t s;

    /* Unwired: clear_credentials_fn left NULL, as auth_reset_gesture_reset()
     * leaves it (it never touches that field) when the caller never set it. */
    memset(&s, 0, sizeof(s));
    tap_full_sequence(&s, 1000, true, false, false);
    TEST_CHECK(s.armed, "arms even with no seam wired -- arming is independent of the credential store");
    TEST_CHECK(auth_reset_gesture_confirm(&s, 1300) == AUTH_RESET_CONFIRM_NOT_WIRED,
               "confirm with clear_credentials_fn == NULL -- NOT_WIRED, not a crash");

    /* Wired but the seam itself reports failure (e.g. read-back verification
     * failed). */
    auth_reset_gesture_reset(&s);
    reset_fake_clear(false);
    s.clear_credentials_fn = fake_clear_ok;
    tap_full_sequence(&s, 2000, true, false, false);
    TEST_CHECK(auth_reset_gesture_confirm(&s, 2300) == AUTH_RESET_CONFIRM_CLEAR_FAILED,
               "confirm when the seam itself reports failure -- CLEAR_FAILED");
    TEST_CHECK(s_fake_clear_calls == 1, "seam was actually called");
}

static void test_cancel_is_clean_and_idempotent(void)
{
    TEST_SECTION("auth_reset_gesture -- cancel");

    auth_reset_gesture_state_t s;
    auth_reset_gesture_reset(&s);
    reset_fake_clear(true);
    s.clear_credentials_fn = fake_clear_ok;

    /* Cancel on a fresh/idle state -- harmless no-op. */
    auth_reset_gesture_cancel(&s);
    TEST_CHECK(!s.armed, "cancel on idle state stays idle");

    auth_reset_gesture_on_corner_tap(&s, AUTH_RESET_CORNER_TOP_LEFT, 1000, true, false, false);
    auth_reset_gesture_cancel(&s);
    TEST_CHECK(s.next_index == 0, "cancel mid-sequence clears in-progress state");

    tap_full_sequence(&s, 2000, true, false, false);
    TEST_CHECK(s.armed, "armed before cancel");
    auth_reset_gesture_cancel(&s);
    TEST_CHECK(!s.armed, "cancel while armed drops back to idle");
    TEST_CHECK(auth_reset_gesture_confirm(&s, 2100) == AUTH_RESET_CONFIRM_NOT_ARMED,
               "confirm after cancel -- NOT_ARMED");
    TEST_CHECK(s_fake_clear_calls == 0, "seam never called after a cancel");
}

void run_test_auth_reset_gesture(void)
{
    test_preconditions();
    test_full_correct_gesture_arms_and_clears();
    test_out_of_order_does_not_arm();
    test_missing_corner_does_not_arm();
    test_taps_without_estop_do_not_progress();
    test_firing_or_heat_blocks_the_gesture();
    test_estop_released_mid_sequence_aborts();
    test_sequence_window_expiry();
    test_confirm_window_expiry();
    test_not_wired_and_clear_failed();
    test_cancel_is_clean_and_idempotent();
}
