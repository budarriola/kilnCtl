// Host tests for firmware/SaftyFW/src/max31856_reconfig_retry.c -- the
// periodic re-probe policy for the bring-up bug where the safety
// MAX31856's ONE main.c configure() attempt can fail if the IC is not yet
// powered/settled, latching "safety TC invalid" forever. No pico-sdk/
// FreeRTOS/hardware dependency (a plain uint32_t ms counter stands in for
// xTaskGetTickCount() * portTICK_PERIOD_MS), same discipline as the other
// max31856_*_policy host tests.
#include "test_common.h"

#include "../src/max31856_reconfig_retry.h"

// Simulates thermo_task.c's own loop shape: N failed configure() attempts,
// then one that succeeds, driving the clock forward by the retry interval
// (plus a comfortable margin, so the "elapsed" comparison is never
// borderline) between iterations. Returns the number of attempts actually
// made (should_attempt() returned true).
static uint32_t drive_retries(max31856_reconfig_retry_state_t *state, uint32_t fail_count,
                               uint32_t iterations)
{
    uint32_t now_ms = 0;
    uint32_t attempts_made = 0;
    for (uint32_t i = 0; i < iterations; i++) {
        now_ms += MAX31856_RECONFIG_RETRY_INTERVAL_MS + 1u;
        bool verified_before = (attempts_made >= fail_count);
        if (max31856_reconfig_retry_should_attempt(state, verified_before, now_ms)) {
            attempts_made++;
            bool verified_after = (attempts_made >= fail_count);
            max31856_reconfig_retry_note_result(state, verified_after, now_ms);
        }
    }
    return attempts_made;
}

// Init-fails-N-then-succeeds: the exact scenario the bug report asks for.
// After enough iterations the state must read "verified, not given up, no
// pending retries owed" -- i.e. the sensor has recovered.
static void test_recovers_after_n_failures(void)
{
    TEST_SECTION("max31856_reconfig_retry: recovers after N failures");

    max31856_reconfig_retry_state_t state;
    max31856_reconfig_retry_init(&state);

    // 5 failures is well under the 20-attempt bound -- must recover, not
    // give up.
    uint32_t attempts = drive_retries(&state, 5, 10);

    TEST_CHECK(attempts == 5, "exactly 5 attempts made before success");
    TEST_CHECK(state.gave_up == false, "did not give up within the bound");

    // Once verified, should_attempt() must go quiet (nothing left to
    // retry) regardless of how much more time passes.
    bool still_wants_retry =
        max31856_reconfig_retry_should_attempt(&state, /*verified=*/true, 1000000u);
    TEST_CHECK(!still_wants_retry, "verified state never asks for another attempt");
}

// A part that never comes back must stop retrying at the documented bound
// (MAX31856_RECONFIG_RETRY_MAX_ATTEMPTS) and report gave_up -- "bounded
// retry count then give up loudly".
static void test_gives_up_after_bound(void)
{
    TEST_SECTION("max31856_reconfig_retry: gives up after the bound");

    max31856_reconfig_retry_state_t state;
    max31856_reconfig_retry_init(&state);

    // fail_count larger than the bound -- it must never actually succeed
    // within MAX31856_RECONFIG_RETRY_MAX_ATTEMPTS tries.
    uint32_t attempts =
        drive_retries(&state, MAX31856_RECONFIG_RETRY_MAX_ATTEMPTS + 100u,
                      MAX31856_RECONFIG_RETRY_MAX_ATTEMPTS + 20u);

    TEST_CHECK(attempts == MAX31856_RECONFIG_RETRY_MAX_ATTEMPTS,
               "retries stop exactly at the documented bound");
    TEST_CHECK(state.gave_up == true, "gave_up set once the bound is reached");

    // Given up: no more attempts, ever, even with time and a (hypothetical)
    // still-unverified reading.
    bool still_wants_retry =
        max31856_reconfig_retry_should_attempt(&state, /*verified=*/false, 100000000u);
    TEST_CHECK(!still_wants_retry, "gave_up state never asks for another attempt");
}

// The retry must respect its own cadence -- calling should_attempt() again
// before MAX31856_RECONFIG_RETRY_INTERVAL_MS has elapsed must not fire a
// second attempt (a hot loop must not hammer the SPI bus every iteration).
static void test_respects_interval(void)
{
    TEST_SECTION("max31856_reconfig_retry: respects the retry interval");

    max31856_reconfig_retry_state_t state;
    max31856_reconfig_retry_init(&state);

    uint32_t now_ms = MAX31856_RECONFIG_RETRY_INTERVAL_MS + 1u;
    TEST_CHECK(max31856_reconfig_retry_should_attempt(&state, false, now_ms),
               "first attempt fires once the initial interval has elapsed");
    max31856_reconfig_retry_note_result(&state, /*verified=*/false, now_ms);

    // One ms later: nowhere near the next deadline.
    TEST_CHECK(!max31856_reconfig_retry_should_attempt(&state, false, now_ms + 1u),
               "no second attempt before the interval elapses again");

    // Just past the next deadline: fires again.
    uint32_t next_due = now_ms + MAX31856_RECONFIG_RETRY_INTERVAL_MS + 1u;
    TEST_CHECK(max31856_reconfig_retry_should_attempt(&state, false, next_due),
               "next attempt fires once the interval elapses again");
}

// Negative-test procedure for this file's coverage (per project standing
// practice: every check must be shown capable of failing, not just
// confirmed to pass) was done by hand against the PRODUCTION function,
// not a test-local mirror -- see the commit message body for the exact
// failing-line transcript. A test-local copy of note_result() proves only
// that the copy behaves as written, never that the real
// max31856_reconfig_retry_note_result() would be caught if it regressed;
// the honest version is to temporarily break the production function
// itself, confirm test_gives_up_after_bound()/test_respects_interval()
// fail, then revert by hand.
void run_test_max31856_reconfig_retry(void)
{
    test_recovers_after_n_failures();
    test_gives_up_after_bound();
    test_respects_interval();
}
