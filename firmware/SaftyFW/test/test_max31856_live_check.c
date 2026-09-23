// Host tests for firmware/SaftyFW/src/max31856_live_check.c -- the pure
// poll-count cadence/episode-counting policy for periodically re-checking a
// VERIFIED MAX31856's live CR0/CR1 registers against the shadow max31856.c
// keeps. No pico-sdk/FreeRTOS/SPI dependency, same discipline as
// test_max31856_reconfig_retry.c.
#include "test_common.h"

#include "../src/max31856_live_check.h"

// Drives N ticks with `verified` held true throughout, returning the number
// of ticks that asked for a check (should have fired exactly once per
// MAX31856_LIVE_CHECK_INTERVAL_POLLS ticks).
static uint32_t drive_ticks_verified(max31856_live_check_state_t *state, uint32_t ticks)
{
    uint32_t fires = 0;
    for (uint32_t i = 0; i < ticks; i++) {
        if (max31856_live_check_tick(state, /*verified=*/true)) {
            fires++;
        }
    }
    return fires;
}

// The cadence must not fire before MAX31856_LIVE_CHECK_INTERVAL_POLLS ticks,
// then fire exactly once at the boundary -- the core "SPI cost is
// negligible" guarantee this module exists to provide.
static void test_fires_at_interval_boundary(void)
{
    TEST_SECTION("max31856_live_check: fires exactly at the interval boundary");

    max31856_live_check_state_t state;
    max31856_live_check_init(&state);

    uint32_t fires_before = drive_ticks_verified(&state, MAX31856_LIVE_CHECK_INTERVAL_POLLS - 1u);
    TEST_CHECK(fires_before == 0, "no check requested before the interval elapses");

    bool fires_at_boundary = max31856_live_check_tick(&state, /*verified=*/true);
    TEST_CHECK(fires_at_boundary, "check requested exactly at the interval boundary");
}

// While unverified, the counter must be held at 0, not merely paused -- the
// first tick after verification returns must start a fresh full interval.
static void test_unverified_holds_counter_at_zero(void)
{
    TEST_SECTION("max31856_live_check: unverified holds the counter at zero");

    max31856_live_check_state_t state;
    max31856_live_check_init(&state);

    // Get partway toward a check while verified.
    (void)drive_ticks_verified(&state, MAX31856_LIVE_CHECK_INTERVAL_POLLS - 1u);

    // Go unverified for a while -- must never fire, and must reset progress.
    for (uint32_t i = 0; i < 50u; i++) {
        bool fired = max31856_live_check_tick(&state, /*verified=*/false);
        TEST_CHECK(!fired, "unverified tick never requests a check");
    }
    TEST_CHECK(state.polls_since_check == 0,
               "unverified ticks hold the counter at zero, not merely pause it");

    // Re-verified: must take a FULL fresh interval, not fire on old progress.
    uint32_t fires = drive_ticks_verified(&state, MAX31856_LIVE_CHECK_INTERVAL_POLLS - 1u);
    TEST_CHECK(fires == 0, "re-verified tick starts a fresh full interval, no stale progress");
}

// note_result()'s episode/counter semantics: mismatch_count increments on
// EVERY mismatched check, but the "new episode" bool is true only the first
// time (mismatch_active latches), matching the header's documented "a part
// that resets, gets reconfigured back to matching, then resets again later
// is visible as 2, not 1" contract.
static void test_note_result_episode_counting(void)
{
    TEST_SECTION("max31856_live_check: note_result counts checks, gates episodes");

    max31856_live_check_state_t state;
    max31856_live_check_init(&state);

    bool first_episode = max31856_live_check_note_result(&state, /*match=*/false);
    TEST_CHECK(first_episode, "first mismatch in a row is a new episode");
    TEST_CHECK(state.mismatch_count == 1, "mismatch_count incremented once");

    bool second_call_same_episode = max31856_live_check_note_result(&state, /*match=*/false);
    TEST_CHECK(!second_call_same_episode, "a second consecutive mismatch is not a new episode");
    TEST_CHECK(state.mismatch_count == 2,
               "mismatch_count still increments on every mismatched check, not just episodes");

    // A match clears the episode latch.
    bool match_result = max31856_live_check_note_result(&state, /*match=*/true);
    TEST_CHECK(!match_result, "a match never reports a new episode");
    TEST_CHECK(state.mismatch_count == 2, "a match never increments mismatch_count");
    TEST_CHECK(!state.mismatch_active, "a match clears the active-episode latch");

    // A later, separate mismatch is a new episode again.
    bool third_episode = max31856_live_check_note_result(&state, /*match=*/false);
    TEST_CHECK(third_episode, "a mismatch after a match starts a new episode");
    TEST_CHECK(state.mismatch_count == 3, "mismatch_count keeps accumulating across episodes");
}

// Negative-test procedure for this file's coverage (per project standing
// practice) was done by hand against the PRODUCTION function, not a
// test-local mirror -- see the commit message body for the exact
// failing-line transcript: MAX31856_LIVE_CHECK_INTERVAL_POLLS was temporarily
// changed and, separately, max31856_live_check_tick()'s reset-while-
// unverified branch was temporarily disabled, each confirmed to fail
// test_fires_at_interval_boundary()/test_unverified_holds_counter_at_zero()
// respectively, then reverted by hand and rebuilt from clean.
void run_test_max31856_live_check(void)
{
    test_fires_at_interval_boundary();
    test_unverified_holds_counter_at_zero();
    test_note_result_episode_counting();
}
