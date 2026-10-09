// Host tests for firmware/SaftyFW/src/max31856_live_check.c -- the pure
// elapsed-time cadence/episode-counting policy for periodically re-checking
// a VERIFIED MAX31856's live CR0/CR1 registers against the shadow
// max31856.c keeps. No pico-sdk/FreeRTOS/SPI dependency, same discipline as
// test_max31856_reconfig_retry.c.
#include "test_common.h"

#include "../src/max31856_live_check.h"

// Drives simulated time forward in fixed steps with `verified` held true
// throughout, returning the number of ticks that asked for a check (should
// fire exactly once per MAX31856_LIVE_CHECK_INTERVAL_MS of elapsed now_ms).
static uint32_t drive_ticks_verified(max31856_live_check_state_t *state, uint32_t *now_ms,
                                      uint32_t step_ms, uint32_t steps)
{
    uint32_t fires = 0;
    for (uint32_t i = 0; i < steps; i++) {
        *now_ms += step_ms;
        if (max31856_live_check_tick(state, /*verified=*/true, *now_ms)) {
            fires++;
        }
    }
    return fires;
}

// The cadence must not fire before MAX31856_LIVE_CHECK_INTERVAL_MS has
// elapsed, then fire exactly once at the boundary -- the core "SPI cost is
// negligible" guarantee this module exists to provide. Driven in 302ms
// steps (thermo_task's worst-case DRDY-silent iteration period) specifically
// because that is the scenario the elapsed-time cadence was built to keep
// correct regardless of loop period -- see max31856_live_check.h.
static void test_fires_at_interval_boundary(void)
{
    TEST_SECTION("max31856_live_check: fires exactly at the interval boundary");

    max31856_live_check_state_t state;
    max31856_live_check_init(&state);

    uint32_t now_ms = 0;
    // First tick while verified only arms the clock -- it never fires.
    now_ms += 1;
    TEST_CHECK(!max31856_live_check_tick(&state, /*verified=*/true, now_ms),
               "the arming tick itself never fires");

    const uint32_t step_ms = 302u;
    uint32_t steps_to_boundary = MAX31856_LIVE_CHECK_INTERVAL_MS / step_ms; // floor
    uint32_t fires_before = drive_ticks_verified(&state, &now_ms, step_ms, steps_to_boundary - 1u);
    TEST_CHECK(fires_before == 0, "no check requested before the interval elapses");

    // Advance well past the remaining time to guarantee the boundary is
    // crossed regardless of the floor-division remainder above.
    now_ms += MAX31856_LIVE_CHECK_INTERVAL_MS;
    bool fires_at_boundary = max31856_live_check_tick(&state, /*verified=*/true, now_ms);
    TEST_CHECK(fires_at_boundary, "check requested once the interval has elapsed");
}

// While unverified, the clock must be disarmed, not merely paused -- the
// first tick after verification returns must arm a fresh full interval
// starting from that moment, not fire on stale elapsed time.
static void test_unverified_disarms_the_clock(void)
{
    TEST_SECTION("max31856_live_check: unverified disarms the clock");

    max31856_live_check_state_t state;
    max31856_live_check_init(&state);

    uint32_t now_ms = 0;
    // Arm, then get partway toward a check while verified.
    now_ms += 1;
    (void)max31856_live_check_tick(&state, /*verified=*/true, now_ms);
    now_ms += MAX31856_LIVE_CHECK_INTERVAL_MS - 1u;
    (void)max31856_live_check_tick(&state, /*verified=*/true, now_ms);

    // Go unverified for a while, including well past when the interval
    // would otherwise have elapsed -- must never fire, and must disarm.
    for (uint32_t i = 0; i < 5u; i++) {
        now_ms += MAX31856_LIVE_CHECK_INTERVAL_MS;
        bool fired = max31856_live_check_tick(&state, /*verified=*/false, now_ms);
        TEST_CHECK(!fired, "unverified tick never requests a check");
    }
    TEST_CHECK(!state.armed, "unverified ticks disarm the clock, not merely pause it");

    // Re-verified: the arming tick itself must not fire, and a full fresh
    // interval must elapse before the next one does -- no stale progress
    // carried over from before disarming.
    bool arming_tick_fires = max31856_live_check_tick(&state, /*verified=*/true, now_ms);
    TEST_CHECK(!arming_tick_fires, "re-verified tick only arms, does not fire immediately");
    now_ms += MAX31856_LIVE_CHECK_INTERVAL_MS - 1u;
    bool fires_early = max31856_live_check_tick(&state, /*verified=*/true, now_ms);
    TEST_CHECK(!fires_early, "re-verified clock starts a fresh full interval, no stale progress");
}

// Tight boundary test: exactly one ms before due must never fire, and the
// exact due tick must fire -- Opus advisory B1 flagged the existing boundary
// test above as loose (it only guarantees "well before" / "well past", never
// pins the single ms on either side of next_check_due_ms).
static void test_boundary_off_by_one(void)
{
    TEST_SECTION("max31856_live_check: due-1 never fires, due fires exactly");

    max31856_live_check_state_t state;
    max31856_live_check_init(&state);

    uint32_t now_ms = 1000;
    (void)max31856_live_check_tick(&state, /*verified=*/true, now_ms); // arms
    uint32_t due = state.next_check_due_ms;
    TEST_CHECK(due == now_ms + MAX31856_LIVE_CHECK_INTERVAL_MS, "arm computed the expected due time");

    bool at_due_minus_one = max31856_live_check_tick(&state, /*verified=*/true, due - 1u);
    TEST_CHECK(!at_due_minus_one, "one ms before due never fires");
    TEST_CHECK(state.next_check_due_ms == due, "a non-firing tick leaves due unchanged");

    bool at_due = max31856_live_check_tick(&state, /*verified=*/true, due);
    TEST_CHECK(at_due, "exactly at due fires");
}

// uint32_t millisecond wraparound: next_check_due_ms computed as
// now_ms + INTERVAL can itself overflow past 0xFFFFFFFF and wrap to a small
// value. The comparison must still recognize "due" once now_ms wraps past
// it, and must not fire early on the ticks leading up to the wrapped due
// time. This is exactly the scenario a raw `now_ms >= next_check_due_ms`
// comparison gets wrong (due wraps to a value smaller than now_ms, so a
// plain >= reads as permanently "past due" and fires immediately/every
// tick); the wraparound-safe `(int32_t)(now_ms - due) >= 0` idiom the
// production code already uses handles it correctly.
static void test_wraparound_ms_clock(void)
{
    TEST_SECTION("max31856_live_check: uint32 ms clock wraparound");

    max31856_live_check_state_t state;
    max31856_live_check_init(&state);

    uint32_t now_ms = 0xFFFFF000u; // 4096 ms before the uint32 wrap point
    (void)max31856_live_check_tick(&state, /*verified=*/true, now_ms); // arms
    uint32_t due = state.next_check_due_ms;
    // due = 0xFFFFF000 + 30000 wraps past 0xFFFFFFFF.
    TEST_CHECK(due == (uint32_t)(now_ms + MAX31856_LIVE_CHECK_INTERVAL_MS),
               "due is computed with the same wraparound arithmetic as the clock itself");
    TEST_CHECK(due < now_ms, "due did in fact wrap to a smaller raw value than now_ms");

    // Advance now_ms up to and past the actual uint32 wrap point, one ms at
    // a time near the boundary, and confirm no early fire -- a broken
    // (unsigned, non-wrap-aware) comparison would fire the instant now_ms
    // wraps past 0 and becomes numerically "less than" due only in signed
    // terms, or would fire immediately once now_ms wraps at all.
    now_ms = 0xFFFFFFFFu;
    TEST_CHECK(!max31856_live_check_tick(&state, /*verified=*/true, now_ms),
               "one ms before the uint32 wrap point still does not fire");
    now_ms = 0u; // wrapped
    TEST_CHECK(!max31856_live_check_tick(&state, /*verified=*/true, now_ms),
               "immediately after the uint32 wrap, still before due, does not fire");

    // due - 1 in wrapped arithmetic: still must not fire.
    now_ms = due - 1u;
    TEST_CHECK(!max31856_live_check_tick(&state, /*verified=*/true, now_ms),
               "one ms before the wrapped due time does not fire");

    // Exactly at the wrapped due time: must fire.
    now_ms = due;
    TEST_CHECK(max31856_live_check_tick(&state, /*verified=*/true, now_ms),
               "exactly at the wrapped due time fires correctly after the uint32 wrap");
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
// test-local mirror: MAX31856_LIVE_CHECK_INTERVAL_MS was temporarily
// shortened and, separately, max31856_live_check_tick()'s disarm-while-
// unverified branch was temporarily disabled, each confirmed to fail
// test_fires_at_interval_boundary()/test_unverified_disarms_the_clock()
// respectively, then reverted by hand and rebuilt from clean.
void run_test_max31856_live_check(void)
{
    test_fires_at_interval_boundary();
    test_unverified_disarms_the_clock();
    test_boundary_off_by_one();
    test_wraparound_ms_clock();
    test_note_result_episode_counting();
}
