// Host tests for App/drivers/safety_link.h's pure, host-testable pieces --
// added alongside SaftyFW/TODO.md item 0.6 ("redefine SAFETY_FAULT_SRC_
// SAFETY_LINK as 'no telemetry within 1.5 s'"). safety_link_is_stale() is a
// `static inline`, dependency-free comparison (no locking, no hardware, no
// FreeRTOS call) -- exactly the same "pure logic lives where it can be
// host-tested" split profile_executor.h's watchdog decision function
// documents, just smaller: one comparison instead of a whole classifier.
//
// safety_link.h itself pulls in esp_err.h/freertos headers/uart_owner.h/
// uart_protocol.h purely for SafetyLinkClass's field types -- test_backup_
// import.c already proved this header includes cleanly against App/test/
// stubs/ (added 2026-08-21 for exactly that), so this file reuses the same
// stub surface rather than adding anything new.
//
// What this closes: nothing in the existing suite exercised safety_link_is_
// stale() itself, or pinned the two LINK_PROTOCOL.md sec 8 constants
// (SAFETY_LINK_STALE_MS = 1500, SAFETY_LINK_FIRING_ABORT_SILENCE_MS =
// 30000) to their documented values -- a future edit could silently drift
// either number (e.g. "helpfully" rounding 1500 to 1000, or reusing the
// firing-abort constant for the fault-source check) with nothing here to
// catch it.
#include <stdint.h>

#include "test_common.h"
#include "../drivers/safety_link.h"

static void test_is_stale_boundary_at_threshold(void)
{
    TEST_SECTION("safety_link_is_stale -- exactly at the threshold is NOT yet stale");

    // "> threshold_ms", not ">=" -- an age reading exactly equal to the
    // threshold is the sample that arrived right on time, not one that
    // missed it.
    TEST_CHECK(safety_link_is_stale(1500u, 1500u) == false,
               "age == threshold is not stale (strict greater-than)");
    TEST_CHECK(safety_link_is_stale(1501u, 1500u) == true,
               "one ms past the threshold is stale");
    TEST_CHECK(safety_link_is_stale(1499u, 1500u) == false,
               "one ms short of the threshold is not stale");
}

static void test_is_stale_never_received_is_always_stale(void)
{
    TEST_SECTION("safety_link_is_stale -- SAFETY_LINK_AGE_NEVER is always stale");

    // "Absence is not proof of safety" (LINK_PROTOCOL.md sec 8): a link that
    // has never produced a frame must read stale against ANY threshold,
    // including a deliberately huge one -- there is no age at which
    // "nothing has ever arrived" becomes acceptable.
    TEST_CHECK(safety_link_is_stale(SAFETY_LINK_AGE_NEVER, SAFETY_LINK_STALE_MS) == true,
               "never-received is stale against the 1.5s threshold");
    TEST_CHECK(safety_link_is_stale(SAFETY_LINK_AGE_NEVER, SAFETY_LINK_FIRING_ABORT_SILENCE_MS) == true,
               "never-received is stale against the 30s threshold too");
    TEST_CHECK(safety_link_is_stale(SAFETY_LINK_AGE_NEVER, 0xFFFFFFFFu) == true,
               "never-received is stale against an arbitrarily large threshold");
}

static void test_is_stale_two_independent_thresholds(void)
{
    TEST_SECTION("safety_link_is_stale -- the 1.5s and 30s checks are genuinely independent");

    // LINK_PROTOCOL.md sec 8's whole point: a single dropped frame (age just
    // past 1.5s) must trip the fault-source (new relay-on refused) WITHOUT
    // also crossing the 30s firing-abort threshold. Same age, two different
    // verdicts depending on which constant it's compared against -- that's
    // what "two timeouts, not one" means in code, not just in the doc.
    uint16_t age_one_dropped_frame = 1600u; // one poll period past 1.5s
    TEST_CHECK(safety_link_is_stale(age_one_dropped_frame, SAFETY_LINK_STALE_MS) == true,
               "1.6s of silence trips the 1.5s liveness check (blocks new relay-on)");
    TEST_CHECK(safety_link_is_stale(age_one_dropped_frame, SAFETY_LINK_FIRING_ABORT_SILENCE_MS) == false,
               "the SAME 1.6s of silence must NOT trip the 30s firing-abort check -- "
               "a single dropped frame must not abort a firing already in progress");
}

static void test_stale_ms_constant_is_1500(void)
{
    TEST_SECTION("SAFETY_LINK_STALE_MS -- pinned at 1500 (LINK_PROTOCOL.md sec 8: '> 1.5s')");

    // Regression pin: this constant is what "the safety processor must be
    // alive to heat" actually means in code. A silent change here (e.g. to
    // 3000, matching SAFETY_LINK_UP_PERIODS * some other period) would
    // loosen the liveness guarantee without touching a single guard.
    TEST_CHECK(SAFETY_LINK_STALE_MS == 1500u, "the fault-source staleness ceiling is exactly 1500 ms");
}

static void test_firing_abort_ms_constant_is_30000(void)
{
    TEST_SECTION("SAFETY_LINK_FIRING_ABORT_SILENCE_MS -- pinned at 30000 (LINK_PROTOCOL.md sec 8)");

    TEST_CHECK(SAFETY_LINK_FIRING_ABORT_SILENCE_MS == 30000u,
               "the firing-abort silence ceiling is exactly 30000 ms");
    TEST_CHECK(SAFETY_LINK_FIRING_ABORT_SILENCE_MS > SAFETY_LINK_STALE_MS,
               "the firing-abort ceiling is strictly larger than the fault-source ceiling -- "
               "aborting a run must always be the SLOWER of the two reactions");
}

void run_test_safety_link(void)
{
    test_is_stale_boundary_at_threshold();
    test_is_stale_never_received_is_always_stale();
    test_is_stale_two_independent_thresholds();
    test_stale_ms_constant_is_1500();
    test_firing_abort_ms_constant_is_30000();
}
