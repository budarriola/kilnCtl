// Host tests for relay_grace.c -- extracted from relay_owner_task()'s
// ARMED/GRACE/TRIPPED state machine (this session's guard-test-matrix pass,
// commit 9a6d3e9 flagged the startup GRACE timing as a host-test gap).
//
// relay_owner_task() itself stays untested here (it's a FreeRTOS task
// function -- gpio_put, xTaskGetTickCount, watchdog checkins); these tests
// only exercise the pure decisions factored out of it into relay_grace.c:
//   - relay_grace_tick(): the GRACE -> ARMED timeout comparison
//   - relay_trip_transition(): the unconditional TRIPPED latch
//   - relay_clear_trip_transition(): TRIPPED -> GRACE-or-ARMED on a clear,
//     against the original boot-relative grace window (2026-08-27 audit fix)
//   - relay_trip_command_still_owed(): safety_core_task()'s trip-command
//     retry-until-it-lands decision (2026-08-27 audit fix item 2)
#include "test_common.h"
#include "../src/tasks/relay_grace.h"

static void test_grace_not_yet_elapsed(void)
{
    TEST_SECTION("relay_grace_tick -- GRACE, not yet elapsed");

    TEST_CHECK(relay_grace_tick(RELAY_OWNER_STATE_GRACE, 0u, 60000u) == RELAY_OWNER_STATE_GRACE,
               "elapsed==0, grace==60000 -> still GRACE");
    TEST_CHECK(relay_grace_tick(RELAY_OWNER_STATE_GRACE, 59999u, 60000u) == RELAY_OWNER_STATE_GRACE,
               "elapsed one tick short of grace -> still GRACE (may-energize stays refused: "
               "relay_owner_command_energize()/the ENERGIZE case only honours ARMED)");
}

static void test_grace_elapsed(void)
{
    TEST_SECTION("relay_grace_tick -- GRACE, elapsed");

    TEST_CHECK(relay_grace_tick(RELAY_OWNER_STATE_GRACE, 60000u, 60000u) == RELAY_OWNER_STATE_ARMED,
               "elapsed==grace exactly -> ARMED (boundary is inclusive, >=, matching the "
               "original relay_owner_task() comparison verbatim)");
    TEST_CHECK(relay_grace_tick(RELAY_OWNER_STATE_GRACE, 60001u, 60000u) == RELAY_OWNER_STATE_ARMED,
               "elapsed one tick past grace -> ARMED");
    TEST_CHECK(relay_grace_tick(RELAY_OWNER_STATE_GRACE, 0xFFFFFFFFu, 60000u) ==
                   RELAY_OWNER_STATE_ARMED,
               "elapsed hugely past grace -> ARMED (no upper bound on the comparison)");
}

static void test_grace_tick_other_states_unchanged(void)
{
    TEST_SECTION("relay_grace_tick -- non-GRACE states are returned unchanged");

    TEST_CHECK(relay_grace_tick(RELAY_OWNER_STATE_INIT, 999999u, 60000u) == RELAY_OWNER_STATE_INIT,
               "INIT is never touched by the GRACE timeout check");
    TEST_CHECK(relay_grace_tick(RELAY_OWNER_STATE_ARMED, 999999u, 60000u) == RELAY_OWNER_STATE_ARMED,
               "ARMED stays ARMED regardless of elapsed/grace -- this is a one-way GRACE->ARMED "
               "transition, not a general re-evaluation");
    TEST_CHECK(relay_grace_tick(RELAY_OWNER_STATE_TRIPPED, 999999u, 60000u) ==
                   RELAY_OWNER_STATE_TRIPPED,
               "TRIPPED stays TRIPPED -- the GRACE timeout can never un-latch a trip");
}

static void test_trip_during_grace_latches(void)
{
    TEST_SECTION("relay_trip_transition -- trip during GRACE latches");

    TEST_CHECK(relay_trip_transition(RELAY_OWNER_STATE_GRACE) == RELAY_OWNER_STATE_TRIPPED,
               "a trip arriving while still in GRACE latches TRIPPED, same as any other state "
               "-- relay_owner_task()'s RELAY_OWNER_CMD_TRIP case has no state guard at all");
}

static void test_trip_transition_unconditional(void)
{
    TEST_SECTION("relay_trip_transition -- unconditional for every state");

    TEST_CHECK(relay_trip_transition(RELAY_OWNER_STATE_INIT) == RELAY_OWNER_STATE_TRIPPED,
               "INIT -> TRIPPED");
    TEST_CHECK(relay_trip_transition(RELAY_OWNER_STATE_ARMED) == RELAY_OWNER_STATE_TRIPPED,
               "ARMED -> TRIPPED");
    TEST_CHECK(relay_trip_transition(RELAY_OWNER_STATE_TRIPPED) == RELAY_OWNER_STATE_TRIPPED,
               "TRIPPED -> TRIPPED (a second trip while already tripped is a no-op re-latch, "
               "not an error)");
}

static void test_clear_trip_during_grace_returns_to_grace(void)
{
    TEST_SECTION("relay_clear_trip_transition -- a clear during GRACE returns to GRACE, "
                 "not ARMED (2026-08-27 audit fix)");

    TEST_CHECK(relay_clear_trip_transition(RELAY_OWNER_STATE_TRIPPED, 10000u, 60000u) ==
                   RELAY_OWNER_STATE_GRACE,
               "tripped 10s into a 60s boot-relative grace window, cleared immediately -> "
               "GRACE, not ARMED -- the remaining ~50s of grace is not discarded");
    TEST_CHECK(relay_clear_trip_transition(RELAY_OWNER_STATE_TRIPPED, 0u, 60000u) ==
                   RELAY_OWNER_STATE_GRACE,
               "tripped/cleared at t==0 (the earliest possible clear) -> GRACE");
    TEST_CHECK(relay_clear_trip_transition(RELAY_OWNER_STATE_TRIPPED, 59999u, 60000u) ==
                   RELAY_OWNER_STATE_GRACE,
               "cleared one tick before grace elapses -> still GRACE");
}

static void test_clear_trip_after_grace_returns_to_armed(void)
{
    TEST_SECTION("relay_clear_trip_transition -- a clear after grace has elapsed goes "
                 "straight to ARMED, same as an un-tripped boot would");

    TEST_CHECK(relay_clear_trip_transition(RELAY_OWNER_STATE_TRIPPED, 60000u, 60000u) ==
                   RELAY_OWNER_STATE_ARMED,
               "elapsed==grace exactly -> ARMED (inclusive boundary, matches relay_grace_tick())");
    TEST_CHECK(relay_clear_trip_transition(RELAY_OWNER_STATE_TRIPPED, 90000u, 60000u) ==
                   RELAY_OWNER_STATE_ARMED,
               "tripped well after grace elapsed (steady-state trip), cleared -> ARMED");
}

static void test_clear_trip_uses_original_boot_clock_not_a_fresh_window(void)
{
    TEST_SECTION("relay_clear_trip_transition -- resumes the ORIGINAL boot-relative window; "
                 "repeated trip/clear cycling cannot hold the board un-armed forever");

    // Simulates: trip at t=10s, clear at t=10s (-> GRACE, per the test above), then a SECOND
    // trip/clear cycle at t=65s -- i.e. elapsed_ticks keeps advancing off the SAME grace_start
    // relay_owner.c captures once at task entry, never a clock restarted by the clear itself.
    // If a clear instead granted a fresh 60000-tick window every time, this second call would
    // still read GRACE at t=65s; because the window is boot-relative, not clear-relative, it
    // does not.
    TEST_CHECK(relay_clear_trip_transition(RELAY_OWNER_STATE_TRIPPED, 65000u, 60000u) ==
                   RELAY_OWNER_STATE_ARMED,
               "a second trip/clear cycle well past the ORIGINAL grace window -> ARMED, proving "
               "the window is boot-relative and cannot be reset by repeated clears "
               "(the availability-attack case relay_grace.h's doc comment rejects (a) over)");
}

static void test_clear_trip_transition_other_states_unchanged(void)
{
    TEST_SECTION("relay_clear_trip_transition -- non-TRIPPED states are returned unchanged");

    TEST_CHECK(relay_clear_trip_transition(RELAY_OWNER_STATE_INIT, 0u, 60000u) ==
                   RELAY_OWNER_STATE_INIT,
               "INIT is never touched by a clear");
    TEST_CHECK(relay_clear_trip_transition(RELAY_OWNER_STATE_GRACE, 0u, 60000u) ==
                   RELAY_OWNER_STATE_GRACE,
               "GRACE (nothing latched to clear) stays GRACE");
    TEST_CHECK(relay_clear_trip_transition(RELAY_OWNER_STATE_ARMED, 0u, 60000u) ==
                   RELAY_OWNER_STATE_ARMED,
               "ARMED (nothing latched to clear) stays ARMED");
}

static void test_trip_command_still_owed(void)
{
    TEST_SECTION("relay_trip_command_still_owed -- safety_core_task()'s retry-until-it-lands "
                 "decision (2026-08-27 audit fix item 2)");

    TEST_CHECK(relay_trip_command_still_owed(true, false) == true,
               "still tripped, this attempt's send was dropped -- still owed, retry next tick");
    TEST_CHECK(relay_trip_command_still_owed(true, true) == false,
               "still tripped, this attempt's send landed -- no longer owed");
    TEST_CHECK(relay_trip_command_still_owed(false, false) == false,
               "cleared out from under the retry (a CLEAR_TRIP landed while the first send was "
               "still stuck) -- drop the flag rather than re-sending a now-stale trip command "
               "after a legitimate clear, regardless of the (unattempted) send result");
    TEST_CHECK(relay_trip_command_still_owed(false, true) == false,
               "not tripped -- never owed, even if the argument claims the send 'succeeded' "
               "(the caller should never actually attempt the send in this case, but the "
               "function itself must not depend on that discipline to be safe)");
}

static void test_energize_allowed_during_update(void)
{
    TEST_SECTION("relay_energize_allowed_during_update -- the Pico's own half of the mutual "
                 "'heating is not allowed during updates' interlock");

    TEST_CHECK(relay_energize_allowed_during_update(false) == true,
               "no update transfer active -- a new energize request is allowed");
    TEST_CHECK(relay_energize_allowed_during_update(true) == false,
               "an update transfer is active on this processor -- a new energize request is refused, "
               "independent of anything the ESP believes");
}

void run_test_relay_grace(void)
{
    test_grace_not_yet_elapsed();
    test_grace_elapsed();
    test_grace_tick_other_states_unchanged();
    test_trip_during_grace_latches();
    test_trip_transition_unconditional();
    test_clear_trip_during_grace_returns_to_grace();
    test_clear_trip_after_grace_returns_to_armed();
    test_clear_trip_uses_original_boot_clock_not_a_fresh_window();
    test_clear_trip_transition_other_states_unchanged();
    test_trip_command_still_owed();
    test_energize_allowed_during_update();
}
