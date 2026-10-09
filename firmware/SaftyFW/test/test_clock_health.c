// test_clock_health.c -- host tests for the stalled-clock detector
// (src/tasks/clock_health.c/.h), the 2026-08-23 fix for the
// DIAG-content-frozen investigation: the RP2040's TIMER_DBGPAUSE register
// pauses to_ms_since_boot(get_absolute_time()) whenever either core is
// halted by a debugger (silicon default), which silently defeated
// safety_core.c's context_valid/reboot_grace_active staleness checks (both
// computed as an age from that clock) -- a dead ESP-side context read as
// fresh forever because the "age" never grew. main.c now clears that
// register at boot; this file is the independent check that does not trust
// that fix took.
//
// clock_health.c has no pico-sdk/FreeRTOS dependency, so unlike
// safety_core.c itself it is directly host-testable here, same pattern
// test_watchdog_gate.c/test_uart_owner_tx_policy.c already established.
#include "test_common.h"
#include "../src/tasks/clock_health.h"

// The first observation ever made can never itself declare a stall -- there
// is nothing yet to compare against. This must hold regardless of what
// now_ms/dt_s are, including a zero-initialised (all-zero) state struct, the
// exact state safety_core.c's static s_clock_health starts in at boot.
static void test_first_observation_never_stalls(void)
{
    clock_health_state_t state = {0};
    bool stalled = clock_health_observe(&state, 12345u, 0.1f);
    TEST_CHECK(!stalled, "the very first observation has no history to compare against -- "
                          "must never report a stall");
    TEST_CHECK(state.have_last, "have_last must be set after the first observation, so the "
                                 "NEXT call actually compares against it");
}

// A healthy clock (advances by roughly what dt_s says it should each call)
// must never be flagged, across many consecutive ticks -- this is the
// steady-state behaviour that must not regress.
static void test_healthy_clock_never_stalls(void)
{
    clock_health_state_t state = {0};
    uint32_t now_ms = 0;
    bool any_stalled = false;
    for (int i = 0; i < 50; i++) {
        now_ms += 100u; // matches dt_s == 0.1f exactly, SAFTYFW_PERIOD_SAFETY_CORE_MS's real value
        if (clock_health_observe(&state, now_ms, 0.1f)) {
            any_stalled = true;
        }
    }
    TEST_CHECK(!any_stalled, "a clock advancing exactly in step with dt_s for 50 consecutive "
                              "ticks must never be flagged as stalled");
}

// The exact bench-observed failure mode: now_ms pinned at one value (the
// RP2040 TIMER frozen by TIMER_DBGPAUSE) while dt_s keeps claiming real time
// passed. Must be caught, and within a bounded number of ticks
// (CLOCK_HEALTH_STALL_DEBOUNCE), not eventually-maybe.
static void test_frozen_clock_is_caught_within_debounce(void)
{
    clock_health_state_t state = {0};
    // Prime with one healthy-looking observation so have_last is set (the
    // very first call can never stall, per its own contract above) --
    // mirrors a boot where the clock was fine for one tick before freezing,
    // the more realistic shape of the bench failure (TIMERAWL froze at
    // 507171us into boot, not at tick zero).
    (void)clock_health_observe(&state, 100u, 0.1f);

    bool stalled = false;
    unsigned tick;
    for (tick = 0; tick < CLOCK_HEALTH_STALL_DEBOUNCE + 2u; tick++) {
        // now_ms never advances again -- the frozen-clock bench symptom
        // exactly: uptime_ms == 507 read over and over across 90+ seconds.
        stalled = clock_health_observe(&state, 100u, 0.1f);
        if (stalled) {
            break;
        }
    }
    TEST_CHECK(stalled, "a clock frozen at one value while dt_s keeps advancing must "
                         "eventually be flagged as stalled");
    TEST_CHECK(tick < CLOCK_HEALTH_STALL_DEBOUNCE + 2u,
               "the stall must be caught within the debounce window, not merely 'eventually'");
}

// A single anomalous non-advancing read (e.g. two calls landing in the same
// millisecond by coincidence) must NOT immediately declare a stall -- that
// would make a perfectly healthy clock flap context_valid/reboot_grace_active
// false on ordinary jitter. Recovery on the very next healthy tick must also
// reset the debounce counter, not leave it primed to fire early next time.
static void test_single_anomalous_read_does_not_stall_and_recovers(void)
{
    clock_health_state_t state = {0};
    (void)clock_health_observe(&state, 1000u, 0.1f); // prime have_last

    bool stalled_after_one_flat_read = clock_health_observe(&state, 1000u, 0.1f); // no advance, once
    TEST_CHECK(!stalled_after_one_flat_read,
               "a single non-advancing read must not immediately declare a stall -- "
               "CLOCK_HEALTH_STALL_DEBOUNCE exists precisely to absorb this");

    bool stalled_after_recovery = clock_health_observe(&state, 1100u, 0.1f); // healthy advance again
    TEST_CHECK(!stalled_after_recovery,
               "a healthy advance immediately after one anomalous flat read must not be "
               "flagged -- the debounce counter must reset on recovery, not just decay");

    // Now prove the counter actually reset: it must take the FULL debounce
    // window again from here, not fire on the very next flat read (which
    // would mean the earlier anomalous read's count silently survived).
    bool stalled = false;
    for (unsigned i = 0; i < CLOCK_HEALTH_STALL_DEBOUNCE - 1u; i++) {
        stalled = clock_health_observe(&state, 1100u, 0.1f);
    }
    TEST_CHECK(!stalled, "the debounce counter must have reset on recovery -- one short of a "
                          "full fresh debounce window must still read healthy");
}

// dt_s <= 0 must never contribute to a stall verdict -- a defensive
// caller-input guard, not something safety_core.c's own fixed positive
// constant can trigger in practice, but this file does not get to assume
// that of every future caller.
static void test_non_positive_dt_s_never_flags_a_stall(void)
{
    clock_health_state_t state = {0};
    (void)clock_health_observe(&state, 5000u, 0.1f); // prime have_last

    bool stalled = false;
    for (int i = 0; i < 10; i++) {
        // now_ms frozen AND dt_s says "no time expected to have passed" --
        // must never flag, since there is nothing to have missed.
        if (clock_health_observe(&state, 5000u, 0.0f)) {
            stalled = true;
        }
    }
    TEST_CHECK(!stalled, "dt_s <= 0 must never contribute to a stall verdict, regardless of "
                          "whether now_ms advances");
}

// A NULL state pointer must be tolerated (never crash) and must never report
// a stall -- there is no history to have been violated.
static void test_null_state_is_tolerated(void)
{
    bool stalled = clock_health_observe(NULL, 42u, 0.1f);
    TEST_CHECK(!stalled, "a NULL state pointer must be tolerated and must never report a "
                          "stall -- nothing to compare against");
}

void run_test_clock_health(void)
{
    TEST_SECTION("clock_health_observe -- the DIAG-content-frozen / TIMER_DBGPAUSE stalled-"
                  "clock detector");
    test_first_observation_never_stalls();
    test_healthy_clock_never_stalls();
    test_frozen_clock_is_caught_within_debounce();
    test_single_anomalous_read_does_not_stall_and_recovers();
    test_non_positive_dt_s_never_flags_a_stall();
    test_null_state_is_tolerated();
}
