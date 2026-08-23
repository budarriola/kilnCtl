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

// --------------------------------------------------------------------------
// safety_drain_still_waiting() -- bug fix 2026-08-23. Root cause: safety_
// drain_inbox_ex()'s receive loop used to hard-code `wait = 0` after the
// FIRST inbox message, no matter what it was. safety_link_get_config_page()
// (and its siblings safety_link_get_ct_cal()/safety_link_send_commit_
// config()) rely on that loop to keep blocking until the ONE specific
// shared-id reply they asked for shows up -- but the Pico also emits
// periodic/unsolicited broadcasts (GET_STATUS, DIAG, POWER, TRIP_EVENT,
// FW_VERSION) on the very same inbox, and CONFIG_PAGE is this link's
// slowest reply to produce. Whenever one of those unrelated frames arrived
// before the real reply -- which live hardware measurement (2026-08-23)
// showed happening on essentially every attempt -- the loop degraded to a
// non-blocking drain immediately afterward, found the real reply not yet
// queued, and exited having burned only a few ms of the ~1.2s budget.
// safety_link_get_config_page() then reported ESP_ERR_TIMEOUT on every
// single call even though the Pico answered every single request
// (s_diag_get_config_page_handled_count == s_diag_get_config_page_seen_count
// == 203 on the bench, ESP side timing out 203/203 times).
// --------------------------------------------------------------------------

static void test_drain_wait_not_waiting_for_anything_never_blocks(void)
{
    TEST_SECTION("safety_drain_still_waiting -- a plain drain (no out-params) never keeps blocking");

    // This is safety_drain_inbox()'s own case (all out-params NULL, so every
    // want_* is false at the call site) -- the periodic poll's pre-drain and
    // GET_STATUS wait must keep their original "grab one burst, don't block
    // for more" behaviour untouched by this fix.
    TEST_CHECK(safety_drain_still_waiting(false, false, false, false, false, false) == false,
               "nothing wanted, nothing gotten -- never keep blocking");
    TEST_CHECK(safety_drain_still_waiting(false, true, false, true, false, true) == false,
               "nothing wanted even if the got_* flags are (implausibly) true -- still never block");
}

static void test_drain_wait_config_page_wanted_not_yet_captured_keeps_waiting(void)
{
    TEST_SECTION("safety_drain_still_waiting -- CONFIG_PAGE wanted but not yet captured: keep waiting");

    // This is exactly the bug: safety_link_get_config_page() wants
    // CONFIG_PAGE, an unrelated frame (e.g. DIAG) was just dispatched, and
    // CONFIG_PAGE has not arrived yet. The pre-fix code would abandon the
    // wait here; the fix must not.
    TEST_CHECK(safety_drain_still_waiting(false, false, /*want_config_page=*/true,
                                           /*got_config_page=*/false, false, false) == true,
               "an unrelated frame must not end the wait while CONFIG_PAGE is still outstanding");
}

static void test_drain_wait_config_page_captured_stops_waiting(void)
{
    TEST_SECTION("safety_drain_still_waiting -- CONFIG_PAGE wanted and captured: stop waiting");

    TEST_CHECK(safety_drain_still_waiting(false, false, /*want_config_page=*/true,
                                           /*got_config_page=*/true, false, false) == false,
               "once the wanted CONFIG_PAGE reply has been captured, degrade to a zero-wait drain");
}

static void test_drain_wait_ct_cal_and_commit_rejected_same_shape(void)
{
    TEST_SECTION("safety_drain_still_waiting -- CT_CAL and COMMIT_CONFIG_REJECTED share the identical shape");

    // safety_link_get_ct_cal() and safety_link_send_commit_config() hit the
    // exact same bug as safety_link_get_config_page() -- same drain loop,
    // same premature-degrade failure mode -- so the fix must cover them too.
    TEST_CHECK(safety_drain_still_waiting(/*want_ct_cal=*/true, /*got_ct_cal=*/false, false, false, false,
                                           false) == true,
               "CT_CAL wanted, not yet captured -- keep waiting (safety_link_get_ct_cal())");
    TEST_CHECK(safety_drain_still_waiting(/*want_ct_cal=*/true, /*got_ct_cal=*/true, false, false, false,
                                           false) == false,
               "CT_CAL wanted and captured -- stop waiting");
    TEST_CHECK(safety_drain_still_waiting(false, false, false, false, /*want_commit_rejected=*/true,
                                           /*got_commit_rejected=*/false) == true,
               "COMMIT_CONFIG_REJECTED wanted, not yet captured -- keep waiting "
               "(safety_link_send_commit_config())");
    TEST_CHECK(safety_drain_still_waiting(false, false, false, false, /*want_commit_rejected=*/true,
                                           /*got_commit_rejected=*/true) == false,
               "COMMIT_CONFIG_REJECTED wanted and captured -- stop waiting");
}

static void test_drain_wait_only_the_wanted_reply_gates_waiting(void)
{
    TEST_SECTION("safety_drain_still_waiting -- an unrequested type's got_* flag is never consulted");

    // Every real call site passes at most one non-NULL out-param pair, so
    // in practice at most one want_* is ever true -- but the function must
    // not accidentally gate on a got_* flag for a type that was never
    // requested (a caller might pass got_ct_cal=false there simply because
    // it never touched that local at all).
    TEST_CHECK(safety_drain_still_waiting(/*want_ct_cal=*/false, /*got_ct_cal=*/false,
                                           /*want_config_page=*/true, /*got_config_page=*/true,
                                           /*want_commit_rejected=*/false,
                                           /*got_commit_rejected=*/false) == false,
               "only CONFIG_PAGE was wanted and it was captured -- the untouched "
               "ct_cal/commit_rejected flags must not force continued waiting");
}

void run_test_safety_link(void)
{
    test_is_stale_boundary_at_threshold();
    test_is_stale_never_received_is_always_stale();
    test_is_stale_two_independent_thresholds();
    test_stale_ms_constant_is_1500();
    test_firing_abort_ms_constant_is_30000();
    test_drain_wait_not_waiting_for_anything_never_blocks();
    test_drain_wait_config_page_wanted_not_yet_captured_keeps_waiting();
    test_drain_wait_config_page_captured_stops_waiting();
    test_drain_wait_ct_cal_and_commit_rejected_same_shape();
    test_drain_wait_only_the_wanted_reply_gates_waiting();
}
