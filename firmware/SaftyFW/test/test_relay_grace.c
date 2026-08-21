// Host tests for relay_grace.c -- extracted from relay_owner_task()'s
// ARMED/GRACE/TRIPPED state machine (this session's guard-test-matrix pass,
// commit 9a6d3e9 flagged the startup GRACE timing as a host-test gap).
//
// relay_owner_task() itself stays untested here (it's a FreeRTOS task
// function -- gpio_put, xTaskGetTickCount, watchdog checkins); these tests
// only exercise the two pure decisions factored out of it into relay_grace.c:
//   - relay_grace_tick(): the GRACE -> ARMED timeout comparison
//   - relay_trip_transition(): the unconditional TRIPPED latch
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
    test_energize_allowed_during_update();
}
