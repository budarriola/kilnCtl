// Host tests for App/drivers/control/heat_enable.c -- the shared, refcounted holder
// of the SAFETY_CMD_REQUEST_ENABLE request that closes K4 on the safety
// processor.
//
// What this module exists to prevent is a real, shipped bug (2026-08-29):
// profile_executor.c and autotune_engine.c never called
// safety_link_request_enable() at all, so every firing and every autotune
// closed its own zone relay against an open K4 and heated nothing. These
// tests pin the three properties that fix depends on:
//
//   1. a run start puts EXACTLY ONE REQUEST_ENABLE(true) on the wire, not
//      one per control tick;
//   2. the LAST claimant letting go puts exactly one REQUEST_ENABLE(false)
//      on the wire, and every extra release after that sends nothing (which
//      is what makes the per-tick "state is not RUNNING" backstops in both
//      callers free);
//   3. a link that is down when the run starts is reported honestly (the
//      request is NOT claimed as granted) and is retried, rather than the
//      run believing it asked for heat when nothing was sent.
//
// The fake safety_link_request_enable() below stands in for safety_link.c's
// real one and reproduces its actual contract (safety_link.h): enable=true
// is REFUSED with ESP_ERR_INVALID_STATE and sends nothing when the link is
// down; enable=false is always attempted, link up or not.
//
// Linked into the MAIN host-test executable (build_host_tests.ps1's
// $sources) alongside the real heat_enable.c -- no other host test defines
// safety_link_request_enable(), so there is nothing for this file's fake to
// collide with, and heat_enable.c's own file-scope statics are the only ones
// it adds.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "test_common.h"

#include "../drivers/control/heat_enable.h"

// ---------------------------------------------------------------------------
// Fake safety link
// ---------------------------------------------------------------------------

static bool g_link_up = true;       // whether enable=true is accepted
static int  g_enable_true_calls = 0;  // attempted (including refused ones)
static int  g_enable_false_calls = 0;
static bool g_last_enable_value = false;
// Forces the enable=false (release) direction to fail -- used to prove the
// 2026-09 fix actually checks safety_link_request_enable()'s result on
// release rather than casting it to (void) and logging success regardless
// (the exact seed-bug shape from danger_mode.c, commit 2bcdc2d).
static bool g_release_should_fail = false;
// Deterministic reorder-race hook (2026-09-15 review of 1c8d7f6e, finding
// HIGH-1): host tests are single-threaded, so a real concurrent
// safety_poll_task/resume interleaving cannot be reproduced directly. This
// fake stands in for it by acting AS the blocking exchange: while
// heat_enable_service_pending_release() is inside its (blocked, in the real
// world) call to send enable=false, this hook reenters heat_enable_acquire()
// right here, synchronously, before returning -- exactly modelling "a resume
// runs on another task while the release's wire exchange is still in
// flight". Whether that reentrant acquire can jump the queue and land
// enable=true first is exactly the race the review found.
static bool g_reenter_acquire_during_release = false;
static bool g_reentrant_acquire_result;
static bool g_reentrant_acquire_ran = false;
// Review-2 LOW-1: a fatal Pico reboot is classified while an enable send is in flight.
static bool g_hold_during_enable = false;
static bool g_hold_during_enable_ran = false;
static bool g_classify_during_enable = false; /* SL3-R2 A3 */
static bool g_classify_during_enable_ran = false;

esp_err_t safety_link_request_enable(SafetyLinkClass *link, bool enable)
{
    (void)link;
    g_last_enable_value = enable;
    if (enable) {
        g_enable_true_calls++;
        if (g_classify_during_enable && !g_classify_during_enable_ran) {
            g_classify_during_enable_ran = true;
            heat_enable_note_pico_boot(6u, false, 0u, 2000u); /* reboot noticed, cause undecided */
        }
        if (g_hold_during_enable && !g_hold_during_enable_ran) {
            g_hold_during_enable_ran = true;
            heat_enable_note_pico_boot(6u, true, SAFETY_LINK_DIAG_BOOT_WATCHDOG, 2000u);
        }
        // safety_link.c: refuses and sends NOTHING when the link is down.
        return g_link_up ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    // safety_link.c: the fail-safe direction is always attempted.
    g_enable_false_calls++;
    if (g_reenter_acquire_during_release && !g_reentrant_acquire_ran) {
        g_reentrant_acquire_ran = true;
        g_reentrant_acquire_result = heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    }
    return g_release_should_fail ? ESP_ERR_TIMEOUT : ESP_OK;
}

// Fresh module state for each test. heat_enable_init() resets everything
// except the send counters (lifetime, like relay cycle counts), so the tests
// read those as deltas.
static uint32_t g_base_enable_sends;
static uint32_t g_base_release_sends;

static void reset_all(bool link_up)
{
    g_link_up = link_up;
    g_enable_true_calls = 0;
    g_enable_false_calls = 0;
    g_release_should_fail = false;
    g_reenter_acquire_during_release = false;
    g_reentrant_acquire_result = false;
    g_reentrant_acquire_ran = false;
    g_hold_during_enable = false;
    g_hold_during_enable_ran = false;
    g_classify_during_enable = false;
    g_classify_during_enable_ran = false;
    heat_enable_init((SafetyLinkClass *)0x1);
    g_base_enable_sends = heat_enable_enable_send_count();
    g_base_release_sends = heat_enable_release_send_count();
}

static uint32_t enable_sends(void) { return heat_enable_enable_send_count() - g_base_enable_sends; }
static uint32_t release_sends(void) { return heat_enable_release_send_count() - g_base_release_sends; }

// ---------------------------------------------------------------------------

static void test_start_requests_exactly_once(void)
{
    TEST_SECTION("heat_enable -- a run start requests K4 exactly once, not once per tick");

    reset_all(true);
    TEST_CHECK(heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE) == true,
               "acquire on a healthy link reports the request went out");
    TEST_CHECK(enable_sends() == 1, "exactly one REQUEST_ENABLE(true) on the wire");
    TEST_CHECK(heat_enable_is_granted(), "the request is granted");
    TEST_CHECK(heat_enable_is_held(HEAT_ENABLE_CLAIMANT_PROFILE), "the claim is recorded");
    TEST_CHECK(!heat_enable_retry_pending(), "nothing pending after a successful request");

    // The property that matters for a control loop: repeating the acquire,
    // as a per-tick call would, sends nothing more.
    bool all_ok = true;
    for (int i = 0; i < 50; i++) {
        if (!heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE)) all_ok = false;
    }
    TEST_CHECK(all_ok, "every repeat acquire still reports the request is standing");
    TEST_CHECK(enable_sends() == 1, "50 further acquires sent nothing (still one frame total)");
    TEST_CHECK(g_enable_true_calls == 1, "the link was not even called again");
}

static void test_release_on_stop(void)
{
    TEST_SECTION("heat_enable -- the last claimant releasing drops K4, once");

    reset_all(true);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
    // 2026-09-15 fix: the wire send is now deferred off heat_enable_release()'s
    // own caller -- nothing lands until a servicer (safety_poll_task on
    // target; called by hand here, standing in for it) actually drains it.
    TEST_CHECK(release_sends() == 0, "the release send is deferred, not sent from this call's own stack");
    TEST_CHECK(!heat_enable_is_granted(), "no longer granted -- bookkeeping flips synchronously either way");
    TEST_CHECK(!heat_enable_is_held(HEAT_ENABLE_CLAIMANT_PROFILE), "the claim is gone");

    heat_enable_service_pending_release();
    TEST_CHECK(release_sends() == 1, "exactly one REQUEST_ENABLE(false) on the wire once drained");
    TEST_CHECK(g_last_enable_value == false, "the last thing sent was a release");

    // The per-tick backstop case: both callers call release on EVERY tick
    // they spend in a non-running state. That must be free.
    for (int i = 0; i < 50; i++) {
        heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
        heat_enable_service_pending_release();
    }
    TEST_CHECK(release_sends() == 1, "50 further releases sent nothing (backstop is free)");

    // ...and a release from a claimant that never acquired sends nothing at
    // all, which is what makes it safe to call from a path that may not have
    // started a run.
    reset_all(true);
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_service_pending_release();
    TEST_CHECK(release_sends() == 0, "releasing a claim that was never taken sends nothing");
}

static void test_two_claimants_refcount(void)
{
    TEST_SECTION("heat_enable -- one claimant letting go does not drop the other's heat");

    reset_all(true);
    TEST_CHECK(heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE) == true, "firing acquires");
    TEST_CHECK(heat_enable_acquire(HEAT_ENABLE_CLAIMANT_AUTOTUNE) == true, "autotune acquires");
    TEST_CHECK(enable_sends() == 1, "the second claimant did not send a second request");

    heat_enable_release(HEAT_ENABLE_CLAIMANT_AUTOTUNE);
    heat_enable_service_pending_release();
    TEST_CHECK(release_sends() == 0, "one of two claimants leaving sends no release");
    TEST_CHECK(heat_enable_is_granted(), "heat is still requested for the remaining claimant");
    TEST_CHECK(!heat_enable_is_held(HEAT_ENABLE_CLAIMANT_AUTOTUNE), "the leaver's claim is gone");

    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_service_pending_release();
    TEST_CHECK(release_sends() == 1, "the last claimant leaving sends the release");
    TEST_CHECK(!heat_enable_is_granted(), "no longer granted");
}

static void test_link_down_at_start_is_honest(void)
{
    TEST_SECTION("heat_enable -- a link that is down at start is reported, not claimed as granted");

    reset_all(false);
    TEST_CHECK(heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE) == false,
               "acquire reports failure when the link refuses (never claims a request it did not send)");
    TEST_CHECK(enable_sends() == 0, "nothing landed on the wire");
    TEST_CHECK(!heat_enable_is_granted(), "NOT granted -- this is the dishonest answer the fix exists to avoid");
    TEST_CHECK(heat_enable_retry_pending(), "the failure is visible as a pending retry");
    // The claim is still recorded even though the request failed -- see
    // heat_enable.h. Without this the caller's release would be a no-op and
    // the retry would have nothing to retry for.
    TEST_CHECK(heat_enable_is_held(HEAT_ENABLE_CLAIMANT_PROFILE),
               "the claim is recorded anyway, so release still tears it down and reconcile still retries");
}

static void test_reconcile_retries_then_stops(void)
{
    TEST_SECTION("heat_enable -- reconcile retries a request that never landed, and only that");

    reset_all(false);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_reconcile();
    heat_enable_reconcile();
    TEST_CHECK(enable_sends() == 0, "still nothing granted while the link is down");
    TEST_CHECK(g_enable_true_calls == 3, "but it is genuinely retried each time (1 acquire + 2 reconciles)");
    TEST_CHECK(heat_enable_retry_pending(), "still pending");

    g_link_up = true; // the safety processor comes back mid-run
    heat_enable_reconcile();
    TEST_CHECK(enable_sends() == 1, "the retry lands the moment the link is back");
    TEST_CHECK(heat_enable_is_granted(), "granted now");
    TEST_CHECK(!heat_enable_retry_pending(), "no longer pending");

    // Exactly-once again: a granted request is never re-asserted by the
    // reconciler, or the watchdog task would put a frame on the wire every
    // 2 s for the whole firing.
    for (int i = 0; i < 20; i++) {
        heat_enable_reconcile();
    }
    TEST_CHECK(enable_sends() == 1, "reconcile on a granted request sends nothing");
}

static void test_release_after_failed_request_still_sends(void)
{
    TEST_SECTION("heat_enable -- release is attempted even when the request never landed");

    reset_all(false);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
    TEST_CHECK(!heat_enable_retry_pending(), "the pending retry is cancelled by the release synchronously, "
                                             "even before the deferred wire send below is drained");
    heat_enable_service_pending_release();
    // enable=false is the fail-safe direction and safety_link.c attempts it
    // whether or not the link looks up -- refusing it because the link looks
    // down is the one refusal that could leave heat on.
    TEST_CHECK(release_sends() == 1, "the release still goes out on a down link");
    heat_enable_reconcile();
    TEST_CHECK(g_enable_true_calls == 1, "reconcile does not resurrect a released claim");
}

// g_esp_loge_calls (stubs/esp_log.h) is a per-TU `static` counter -- it
// cannot see ESP_LOGE calls made inside heat_enable.c, which this suite
// links as a separately-compiled object rather than #including into this
// test file the way test_dashboard_json.c does with dashboard_json.c. So the
// "was a failure actually reported" half of this fix is pinned by a
// source-text scan of the real, compiled heat_enable.c instead -- same
// precedent as test_display_power_wiring.c / test_zone_sweep_relay_off_
// wiring.c -- while the behavioral half (the send is still attempted, the
// return value affects nothing about the state machine) is proved live,
// below.
#include <stdio.h>
#include <stdlib.h>

static char *heat_enable_read_source(void)
{
    /* test_read_source_anchored() (test_common.h) tries the path anchored
     * to this test file's own on-disk location first -- correct for ANY
     * working directory the test binary is launched from -- and falls
     * back to these literal candidates (which only covered App/test, App,
     * and the repo root) as a second layer. */
    static const char *const candidates[] = {
        "../drivers/control/heat_enable.c",
        "App/drivers/control/heat_enable.c",
        "firmware/KilnFW/App/drivers/control/heat_enable.c",
    };
    return test_read_source_anchored(__FILE__, "../drivers/control/heat_enable.c", candidates,
                                      sizeof(candidates) / sizeof(candidates[0]));
}

static void test_release_failure_is_checked_and_logged(void)
{
    TEST_SECTION("heat_enable -- a release send that FAILS is still attempted (behavioral), and "
                 "the fix's result-checking is present in the compiled source (source-text scan) "
                 "-- not the fire-and-forget (void) cast that used to print a success line "
                 "regardless of the result (seed bug: danger_mode.c, commit 2bcdc2d)");

    // Behavioral: the release attempt itself is unconditional and unaffected
    // by whether it is going to fail -- the fix must not turn this into a
    // refusal to even try.
    reset_all(true);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    g_release_should_fail = true;
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_service_pending_release();
    TEST_CHECK(release_sends() == 1, "the release is still attempted even though it will fail");
    TEST_CHECK(g_last_enable_value == false, "the attempted send was still enable=false");

    // Structural: heat_enable.c must actually capture and check both
    // fire-and-forget release sends this fix touched, not just call the
    // function and drop the result the way it used to.
    char *text = heat_enable_read_source();
    if (!text) {
        TEST_CHECK(false, "could not locate drivers/heat_enable.c from the host test's working "
                           "directory -- update the candidate paths in this test if the build "
                           "layout moved");
        return;
    }

    TEST_CHECK(strstr(text, "(void)safety_link_request_enable(") == NULL,
               "heat_enable.c must not cast any safety_link_request_enable() result to (void) "
               "any more -- if this matches, one of the two release sites this fix touched went "
               "back to fire-and-forget.");

    int rel_err_assigns = 0;
    const char *p = text;
    while ((p = strstr(p, "esp_err_t rel_err = safety_link_request_enable(")) != NULL) {
        rel_err_assigns++;
        p += 1;
    }
    TEST_CHECK(rel_err_assigns == 2,
               "expected exactly 2 checked release sends (heat_enable_release()'s normal path "
               "and send_enable()'s orphaned-request corrective release) -- found a different "
               "count, meaning one of the two sites was not converted or a third, unreviewed "
               "site was added without also being checked.");

    int rel_err_checks = 0;
    p = text;
    while ((p = strstr(p, "if (rel_err != ESP_OK)")) != NULL) {
        rel_err_checks++;
        p += 1;
    }
    TEST_CHECK(rel_err_checks == 2,
               "each captured rel_err must actually be inspected (if (rel_err != ESP_OK)) -- a "
               "count below 2 means a result is captured into a variable nobody reads, the same "
               "silent-drop bug wearing a variable name.");

    free(text);
}

static void test_release_defers_the_wire_send_off_the_callers_stack(void)
{
    TEST_SECTION("heat_enable -- 2026-09-15 fix: heat_enable_release() itself never calls "
                 "safety_link_request_enable() -- the deep UART chain that smashed profile_executor's "
                 "4096 B stack four times (docs/audits/profile_executor_coredump_2026-09-15.md) must "
                 "run on a servicer's stack, not the caller's");

    // Behavioral half, already exercised above (test_release_on_stop): a
    // release with nobody draining it leaves release_sends() at 0. Pinned
    // again here, directly, as the property this whole fix exists for.
    reset_all(true);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
    TEST_CHECK(release_sends() == 0,
               "MUST GO RED if heat_enable_release() goes back to sending synchronously -- an "
               "un-drained release must leave nothing on the wire yet");
    heat_enable_service_pending_release();
    TEST_CHECK(release_sends() == 1, "sanity: it does still go out once actually drained");

    // Structural half: heat_enable_release()'s own function body must not
    // contain the call. A source-text scan rather than a call-graph tool
    // because that is this suite's existing precedent for pinning "this
    // exact function does/doesn't call that exact function" (see
    // test_release_failure_is_checked_and_logged() just above).
    char *text = heat_enable_read_source();
    if (!text) {
        TEST_CHECK(false, "could not locate drivers/heat_enable.c to source-scan");
        return;
    }
    /* 2026-09-16: the body moved into he_release_common(), shared by
     * heat_enable_release() and heat_enable_release_backstop(). Anchored on
     * the helper so this scan keeps covering the REAL code rather than a
     * two-line wrapper that trivially contains no send -- the region from here
     * to heat_enable_service_pending_release() spans the helper AND both
     * wrappers, so it is strictly more coverage than before, not less. */
    const char *fn = strstr(text,
                            "static void he_release_common(heat_enable_claimant_t who, uint32_t bit, bool stop_transition)");
    TEST_CHECK(fn != NULL, "sanity: he_release_common()'s definition must be findable in the source");
    if (fn != NULL) {
        const char *next_fn = strstr(fn + 1, "\nvoid heat_enable_service_pending_release(void)");
        TEST_CHECK(next_fn != NULL, "sanity: the next function boundary must be findable");
        if (next_fn != NULL) {
            size_t body_len = (size_t)(next_fn - fn);
            char *body = (char *)malloc(body_len + 1);
            if (body) {
                memcpy(body, fn, body_len);
                body[body_len] = '\0';
                TEST_CHECK(strstr(body, "safety_link_request_enable(") == NULL,
                           "the release path's own function bodies must not call "
                           "safety_link_request_enable() -- if this matches, the synchronous send "
                           "came back onto profile_executor_status.c's/profile_executor.c's caller "
                           "stacks, recreating the 2026-09-15 panic");
                free(body);
            }
        }
    }
    free(text);
}

static void test_reenable_never_races_ahead_of_a_pending_release(void)
{
    TEST_SECTION("heat_enable -- a re-enable can never overtake a still-pending release "
                 "(reset-one-side class: CLAUDE.md)");

    reset_all(true);
    TEST_CHECK(heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE) == true, "sanity: first acquire lands");
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
    // Nothing drained the pending release yet -- exactly the state a
    // safety_poll_task tick hasn't caught up to.
    TEST_CHECK(release_sends() == 0, "sanity: the release is still only pending");

    // A re-enable arrives before any servicer ran (e.g. profile_executor_
    // resume() right after profile_executor_pause()). send_enable() must
    // flush the pending release BEFORE asking for enable=true, or the two
    // frames could reorder on the wire and leave the safety processor
    // seeing enable-then-disable when this side intended disable-then-enable.
    TEST_CHECK(heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE) == true, "the re-enable lands");
    TEST_CHECK(release_sends() == 1,
               "MUST GO RED if a re-enable can be sent while a release is still pending -- the "
               "pending release must be flushed first, not left stranded behind a fresh enable");
    TEST_CHECK(enable_sends() == 2, "and the re-enable itself still goes out (1 initial + 1 after)");
}

static void test_reenable_cannot_overtake_an_inflight_release(void)
{
    TEST_SECTION("heat_enable -- 2026-09-15 review of 1c8d7f6e (HIGH-1): a re-enable arriving while "
                 "a release's wire exchange is still IN FLIGHT (not merely flagged) must not overtake "
                 "it -- the reorder that left the Pico disabled with the ESP believing it was granted, "
                 "and nothing left to retry");

    reset_all(true);
    TEST_CHECK(heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE) == true, "sanity: first acquire lands");
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
    TEST_CHECK(release_sends() == 0, "sanity: the release is only pending so far");

    // Arm the hook: while the drain's safety_link_request_enable(false) is
    // "in flight" (release_inflight is already true by the time this fires,
    // set by heat_enable_service_pending_release() before it calls out), a
    // re-enable reenters right here -- modelling profile_executor_resume()
    // running on a different task while safety_poll_task's drain is blocked
    // on the wire, which is exactly the scenario the review traced.
    g_reenter_acquire_during_release = true;
    heat_enable_service_pending_release();

    TEST_CHECK(g_reentrant_acquire_ran, "sanity: the reentrant acquire actually fired mid-release");
    TEST_CHECK(g_reentrant_acquire_result == false,
               "MUST GO RED if a re-enable can land while a release is still in flight -- it must "
               "defer instead of sending enable=true ahead of the release that has not landed yet");
    TEST_CHECK(g_enable_true_calls == 1,
               "the reentrant acquire must NOT have put a second enable=true on the wire -- only the "
               "original acquire's frame is on it");
    TEST_CHECK(release_sends() == 1, "the release itself still went out exactly once");
    TEST_CHECK(!heat_enable_is_granted(),
               "NOT granted -- the dangerous outcome this fix exists to avoid is granted=true with "
               "the Pico actually left disabled");
    TEST_CHECK(heat_enable_retry_pending(), "the deferred re-enable is visible as a pending retry");

    // Recovery: the deferred request is not lost -- reconcile() (the
    // watchdog task, independent of safety_poll_task) picks it up once the
    // release has actually cleared.
    heat_enable_reconcile();
    TEST_CHECK(heat_enable_is_granted(), "reconcile lands the deferred re-enable once the release cleared");
    TEST_CHECK(!heat_enable_retry_pending(), "no longer pending");
    TEST_CHECK(enable_sends() == 2, "exactly one more enable=true frame -- the recovered retry");
}

static void test_failed_release_is_retried_not_dropped(void)
{
    TEST_SECTION("heat_enable -- 2026-09-15 review of 1c8d7f6e (LOW-5): a release send that FAILS "
                 "stays queued and is retried, rather than being dropped after one attempt (the "
                 "header used to claim 'never dropped' while actually meaning 'attempted once')");

    reset_all(true);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);

    g_release_should_fail = true;
    heat_enable_service_pending_release();
    TEST_CHECK(release_sends() == 1, "the first (failing) attempt is still made");

    // A second drain call, still failing, must ATTEMPT AGAIN -- if the flag
    // had been dropped after the first failure (the old behaviour), this
    // would be a silent no-op and release_sends() would not move.
    heat_enable_service_pending_release();
    TEST_CHECK(release_sends() == 2,
               "MUST GO RED if a failed release is dropped instead of retried -- a second drain call "
               "must attempt it again");

    // Once the link recovers, the very next drain succeeds and stops retrying.
    g_release_should_fail = false;
    heat_enable_service_pending_release();
    TEST_CHECK(release_sends() == 3, "the retry lands once the link is healthy again");
    heat_enable_service_pending_release();
    TEST_CHECK(release_sends() == 3, "and nothing more is sent once it has actually gone out");
}

static void test_flush_drives_the_exchange_at_most_once(void)
{
    TEST_SECTION("heat_enable -- 2026-09-15 review of 059a896e, HIGH-1: he_flush_release_blocking() "
                 "must drive heat_enable_service_pending_release() at most ONCE per call, not once "
                 "per retry iteration -- the old shape made HE_FLUSH_MAX_ATTEMPTS*HE_FLUSH_RETRY_MS "
                 "a delay budget, not a wall-clock bound, since each of up to 20 iterations could "
                 "itself run the full blocking safety_exchange() (SAFETY_XACT_LOCK_TIMEOUT_MS plus "
                 "the reply wait) on the caller's own stack -- httpd or the UART bridge.");

    char *text = heat_enable_read_source();
    if (!text) {
        TEST_CHECK(false, "could not locate drivers/heat_enable.c to source-scan");
        return;
    }
    const char *fn = strstr(text, "static bool he_flush_release_blocking(void)");
    TEST_CHECK(fn != NULL, "sanity: he_flush_release_blocking()'s definition must be findable");
    if (fn != NULL) {
        const char *next_fn = strstr(fn + 1, "\nstatic bool send_enable(");
        TEST_CHECK(next_fn != NULL, "sanity: the next function boundary must be findable");
        if (next_fn != NULL) {
            size_t body_len = (size_t)(next_fn - fn);
            char *body = (char *)malloc(body_len + 1);
            if (body) {
                memcpy(body, fn, body_len);
                body[body_len] = '\0';

                int drive_calls = 0;
                const char *p = body;
                while ((p = strstr(p, "heat_enable_service_pending_release()")) != NULL) {
                    drive_calls++;
                    p += 1;
                }
                TEST_CHECK(drive_calls == 1,
                           "MUST GO RED if he_flush_release_blocking() calls the blocking exchange "
                           "zero times (nothing would ever drive a stuck release) or more than once "
                           "(the retry loop would go back to driving the exchange on every "
                           "iteration, recreating HIGH-1's unbounded caller-stack stall)");

                // The one drive call, if present, must not be textually inside the
                // "for (" retry loop -- i.e. it must appear before the loop's
                // opening brace, not between it and its matching close. This
                // suite's existing precedent (test_release_defers_the_wire_send_
                // off_the_callers_stack()) already source-scans by function body;
                // this pins the finer-grained "which half of the body" property a
                // whole-body substring search can't distinguish.
                const char *for_loop = strstr(body, "for (int attempt");
                const char *drive_call = strstr(body, "heat_enable_service_pending_release()");
                TEST_CHECK(for_loop != NULL, "sanity: the retry loop must be findable");
                TEST_CHECK(drive_call != NULL && for_loop != NULL && drive_call < for_loop,
                           "MUST GO RED if the single drive call moved to at or after the retry "
                           "loop's start -- it must run once, before the passive-poll loop begins, "
                           "not from inside it");

                free(body);
            }
        }
    }
    free(text);
}

static void test_reconcile_never_drives_the_blocking_exchange_unconditionally(void)
{
    TEST_SECTION("heat_enable -- 2026-09-15 review of 059a896e, HIGH-2: heat_enable_reconcile() "
                 "must not unconditionally call heat_enable_service_pending_release() (or "
                 "he_flush_release_blocking()) on every watchdog tick -- that put the deep UART "
                 "release chain on profile_exec_wdt's stack, the SAME 4096 B size as "
                 "profile_executor's, the exact hazard 1c8d7f6e existed to remove, just relocated "
                 "onto the guard task.");

    char *text = heat_enable_read_source();
    if (!text) {
        TEST_CHECK(false, "could not locate drivers/heat_enable.c to source-scan");
        return;
    }
    const char *fn = strstr(text, "void heat_enable_reconcile(void)");
    TEST_CHECK(fn != NULL, "sanity: heat_enable_reconcile()'s definition must be findable");
    if (fn != NULL) {
        const char *next_fn = strstr(fn + 1, "\nuint32_t heat_enable_enable_send_count(void)");
        TEST_CHECK(next_fn != NULL, "sanity: the next function boundary must be findable");
        if (next_fn != NULL) {
            size_t body_len = (size_t)(next_fn - fn);
            char *body = (char *)malloc(body_len + 1);
            if (body) {
                memcpy(body, fn, body_len);
                body[body_len] = '\0';
                // Skip past this function's own explanatory header comment
                // (which, by necessity, names the very call it explains is no
                // longer made here) before scanning the executable code.
                char *code = strstr(body, "*/");
                code = code ? code + 2 : body;
                TEST_CHECK(strstr(code, "heat_enable_service_pending_release()") == NULL,
                           "MUST GO RED if heat_enable_reconcile() goes back to calling "
                           "heat_enable_service_pending_release() directly -- that call must only "
                           "be reachable from here (if at all) conditionally, through send_enable()'s "
                           "own retry path, never unconditionally on every tick");
                free(body);
            }
        }
    }
    free(text);
}

/* Last match, not first. Source-text scans that assert "X appears before Y"
 * are trivially satisfied by a #define or a comment near the top of the file
 * -- see test_reconcile_runs_last_in_the_watchdog_loop()'s own note on the
 * vacuous first version of exactly that check. */
static const char *last_occurrence(const char *hay, const char *needle)
{
    const char *found = NULL;
    const char *p = hay;
    while ((p = strstr(p, needle)) != NULL) {
        found = p;
        p += 1;
    }
    return found;
}

static char *profile_executor_read_source(void)
{
    /* Same idiom as heat_enable_read_source() above -- anchored to this test
     * file's own location first, literal candidates as a fallback. */
    static const char *const candidates[] = {
        "../drivers/control/profile_executor.c",
        "App/drivers/control/profile_executor.c",
        "firmware/KilnFW/App/drivers/control/profile_executor.c",
    };
    return test_read_source_anchored(__FILE__, "../drivers/control/profile_executor.c", candidates,
                                      sizeof(candidates) / sizeof(candidates[0]));
}

static char *profile_executor_status_read_source(void)
{
    static const char *const candidates[] = {
        "../drivers/control/profile_executor_status.c",
        "App/drivers/control/profile_executor_status.c",
        "firmware/KilnFW/App/drivers/control/profile_executor_status.c",
    };
    return test_read_source_anchored(__FILE__, "../drivers/control/profile_executor_status.c", candidates,
                                      sizeof(candidates) / sizeof(candidates[0]));
}

// Review-2 LOW-3: pin the watchdog-loop wiring (M1, M3, LOW-2 order) and the bounded pause (M4b).
static void test_watchdog_loop_and_bounded_pause_wiring(void)
{
    TEST_SECTION("profile_executor -- review-2 LOW-3: guard 9 merge wired into the loop before the unknown-relay "
                 "release, status-read gate, bounded pause really bounded");
    char *text = profile_executor_read_source();
    if (!text) {
        TEST_CHECK(false, "could not locate drivers/control/profile_executor.c");
        return;
    }
    const char *wd = strstr(text, "void watchdog_task_entry(void *arg)");
    TEST_CHECK(wd != NULL, "watchdog_task_entry found");
    const char *merge = wd ? strstr(wd, "tick_stale = guard9_merge_pending(tick_stale") : NULL;
    const char *assert_fn = wd ? strstr(wd, "guard9_assert_stale_tick_fault();") : NULL;
    const char *rel = wd ? strstr(wd, "\n        relay_unknown_release_locked();") : NULL;
    TEST_CHECK(merge != NULL, "MUST GO RED if the watchdog loop stops calling guard9_merge_pending");
    TEST_CHECK(merge && assert_fn && rel && merge < assert_fn && assert_fn < rel,
               "guard 9 merge+assert precede relay_unknown_release_locked() so APP never drops (LOW-2)");
    const char *gate = strstr(text, "if (safety_status_ok)");
    const char *note = gate ? strstr(gate, "heat_enable_note_pico_boot(") : NULL;
    TEST_CHECK(gate && note && (note - gate) < 80,
               "MUST GO RED if note_pico_boot is no longer gated on a successful status read");
    TEST_CHECK(strstr(text, "if (true)") == NULL, "no if (true) stand-in for the status gate");
    free(text);

    char *st = profile_executor_status_read_source();
    if (!st) {
        TEST_CHECK(false, "could not locate drivers/control/profile_executor_status.c");
        return;
    }
    const char *bf = strstr(st, "bool profile_executor_pause_with_reason_bounded(const char *reason)");
    const char *end = bf ? strstr(bf, "bool profile_executor_resume(void)") : NULL;
    bool bounded = false, unbounded = false;
    if (bf && end) {
        for (const char *q = bf; q < end; q++) {
            if (strncmp(q, "pdMS_TO_TICKS(", 14) == 0) bounded = true;
            if (strncmp(q, "portMAX_DELAY", 13) == 0) unbounded = true;
        }
    }
    TEST_CHECK(bf && end && bounded && !unbounded,
               "MUST GO RED if the bounded pause waits portMAX_DELAY instead of a finite pdMS_TO_TICKS bound");
    free(st);
}

static void test_stale_claim_is_not_resurrected(void)
{
    TEST_SECTION("heat_enable -- an acquire whose claim was released after the caller committed to "
                 "it (operator halt in the window between unlocking s_exec.lock and the unlocked "
                 "acquire call) REFUSES instead of re-claiming heat for a run that no longer "
                 "exists (2026-09-15 review of 8813bedd, MEDIUM-5)");

    /* The real sequence: a run acquires, samples nothing; an operator halt
     * releases; a start path that sampled its epoch BEFORE that release then
     * calls acquire_since with the stale value. */
    reset_all(true);
    uint32_t epoch = heat_enable_claim_epoch(HEAT_ENABLE_CLAIMANT_PROFILE);
    TEST_CHECK(heat_enable_acquire_since(HEAT_ENABLE_CLAIMANT_PROFILE, epoch) == true,
               "a fresh epoch acquires normally");
    TEST_CHECK(enable_sends() == 1, "and it put exactly one REQUEST_ENABLE(true) on the wire");

    /* The halt. This bumps the epoch, so the value captured above is stale. */
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_service_pending_release();

    uint32_t sends_before = enable_sends();
    TEST_CHECK(heat_enable_acquire_since(HEAT_ENABLE_CLAIMANT_PROFILE, epoch) == false,
               "the stale-epoch acquire is REFUSED");
    TEST_CHECK(enable_sends() == sends_before,
               "and it sent no REQUEST_ENABLE(true) -- K4 was not asked for on behalf of a run "
               "that had already stopped");
    TEST_CHECK(heat_enable_is_held(HEAT_ENABLE_CLAIMANT_PROFILE) == false,
               "and the claim was NOT resurrected -- held_mask stays clear, so "
               "heat_owner_active_decide() does not report a heat owner to the Pico either");
    TEST_CHECK(heat_enable_is_granted() == false, "and nothing reads as granted");

    /* The refusal must be scoped to the claimant that was released: a global
     * epoch would make an unrelated autotune release refuse a legitimate
     * firing start, which is strictly worse than the window it closes. */
    reset_all(true);
    uint32_t prof_epoch = heat_enable_claim_epoch(HEAT_ENABLE_CLAIMANT_PROFILE);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_AUTOTUNE);
    heat_enable_release(HEAT_ENABLE_CLAIMANT_AUTOTUNE);
    heat_enable_service_pending_release();
    TEST_CHECK(heat_enable_acquire_since(HEAT_ENABLE_CLAIMANT_PROFILE, prof_epoch) == true,
               "an autotune release does NOT invalidate a profile's pending acquire");
    TEST_CHECK(heat_enable_is_held(HEAT_ENABLE_CLAIMANT_PROFILE) == true, "the firing holds its claim");

    /* An IDLE BACKSTOP tick that finds nothing held must not advance the stop
     * generation -- otherwise the per-tick backstop callers (profile_executor's
     * "state is not RUNNING" branch and autotune_engine.c's "not running"
     * branch, which call it on EVERY such tick) would invalidate every
     * in-flight acquire.
     *
     * 2026-09-16: this assertion used to be written against plain
     * heat_enable_release(), and that is what pinned the resurrection hole
     * open -- see
     * test_stop_in_the_sample_then_spend_window_is_not_resurrected() below.
     * The requirement it encodes is RIGHT; it just belongs to the BACKSTOP
     * form of the call, which is the only caller that actually has it.
     * Re-pointed, not deleted or weakened: same property, asserted against the
     * call that needs it. */
    reset_all(true);
    uint32_t e2 = heat_enable_claim_epoch(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_release_backstop(HEAT_ENABLE_CLAIMANT_PROFILE);
    TEST_CHECK(heat_enable_acquire_since(HEAT_ENABLE_CLAIMANT_PROFILE, e2) == true,
               "an idle-backstop tick with nothing held does not invalidate a legitimate acquire");
    TEST_CHECK(heat_enable_is_held(HEAT_ENABLE_CLAIMANT_PROFILE) == true,
               "sanity: that legitimate acquire really did record its claim");

    /* ...and the backstop still counts when the tick IS the teardown: a claim
     * it actually tore down must not be re-acquirable on a stale generation
     * either. */
    reset_all(true);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    uint32_t e2b = heat_enable_claim_epoch(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_release_backstop(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_service_pending_release();
    TEST_CHECK(heat_enable_acquire_since(HEAT_ENABLE_CLAIMANT_PROFILE, e2b) == false,
               "a backstop tick that was itself the teardown DOES invalidate a stale acquire");

    /* Plain heat_enable_acquire() must remain an explicit opt-out that always
     * proceeds -- callers not carrying an earlier decision are unaffected. */
    reset_all(true);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_service_pending_release();
    TEST_CHECK(heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE) == true,
               "plain acquire() still acquires after a release -- it samples a fresh epoch");
}

static void test_stop_in_the_sample_then_spend_window_is_not_resurrected(void)
{
    TEST_SECTION("heat_enable -- 2026-09-16: a stop landing INSIDE the sample-then-spend window "
                 "must not let the acquire resurrect the claim. The 2026-09-15 epoch pair did not "
                 "close this: at every acquire_since() call site the claimant's bit is CLEAR when "
                 "the epoch is sampled, so the stop's release found was_held==false, advanced "
                 "nothing, and the stale epoch compared EQUAL -- REQUEST_ENABLE(true) on the wire "
                 "and a heat owner reported to the Pico, for a run that had already stopped");

    /* ---- Case 1: a FRESH START (profile_executor_run.c). The run flips to
     * RUNNING under s_exec.lock, samples the epoch, drops the lock -- and has
     * NOT acquired yet, so held_mask is 0 here. An operator Stop then runs the
     * whole halt path (heat_enable_release() included) in the gap before the
     * unlocked acquire_since() runs. */
    reset_all(true);
    uint32_t start_epoch = heat_enable_claim_epoch(HEAT_ENABLE_CLAIMANT_PROFILE);
    TEST_CHECK(heat_enable_is_held(HEAT_ENABLE_CLAIMANT_PROFILE) == false,
               "sanity: this models the real call sites -- the claimant's bit is CLEAR at the "
               "moment the epoch is sampled, which is exactly why 'advance only if was_held' was "
               "inert");

    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);   /* the operator Stop */
    heat_enable_service_pending_release();

    TEST_CHECK(heat_enable_acquire_since(HEAT_ENABLE_CLAIMANT_PROFILE, start_epoch) == false,
               "MUST GO RED if a stop inside the sample-then-spend window can be overtaken -- the "
               "acquire must REFUSE, not re-claim heat for a firing that has already stopped");
    TEST_CHECK(g_enable_true_calls == 0,
               "and nothing was put on the wire: no REQUEST_ENABLE(true) may be sent on behalf of "
               "a run that no longer exists");
    TEST_CHECK(enable_sends() == 0, "the link was never asked to permit heating");
    TEST_CHECK(heat_enable_is_held(HEAT_ENABLE_CLAIMANT_PROFILE) == false,
               "and held_mask stays clear -- heat_owner_active_decide() (safety_link_frames.c) "
               "reads that same mask, so the Pico is not told a heat owner is active either");
    TEST_CHECK(heat_enable_is_granted() == false, "nothing reads as granted");
    TEST_CHECK(heat_enable_retry_pending() == false,
               "and a refused acquire leaves nothing queued for heat_enable_reconcile() to retry "
               "-- a resurrection on the watchdog's next tick would be the same bug, delayed");

    /* ---- Case 2: a RESUME (profile_executor_status.c). pause() already
     * released the claim, so the bit is clear here too; resume() samples the
     * epoch under s_exec.lock and spends it unlocked, and the operator's Stop
     * lands in that gap. Structurally the same window, reached the other
     * way. */
    reset_all(true);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);      /* the run was going */
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);            /* pause() */
    heat_enable_service_pending_release();
    uint32_t resume_epoch = heat_enable_claim_epoch(HEAT_ENABLE_CLAIMANT_PROFILE);
    uint32_t sends_before_stop = enable_sends();
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);            /* halt(), in the gap */
    heat_enable_service_pending_release();
    TEST_CHECK(heat_enable_acquire_since(HEAT_ENABLE_CLAIMANT_PROFILE, resume_epoch) == false,
               "a stop landing between resume()'s unlock and its acquire must refuse too");
    TEST_CHECK(enable_sends() == sends_before_stop, "and it sent no re-enable");
    TEST_CHECK(heat_enable_is_held(HEAT_ENABLE_CLAIMANT_PROFILE) == false, "no claim resurrected");

    /* ---- Case 3: the autotune claimant, whose start paths
     * (autotune_engine.c) sample and spend the same way. */
    reset_all(true);
    uint32_t at_epoch = heat_enable_claim_epoch(HEAT_ENABLE_CLAIMANT_AUTOTUNE);
    heat_enable_release(HEAT_ENABLE_CLAIMANT_AUTOTUNE);           /* abort, in the gap */
    heat_enable_service_pending_release();
    TEST_CHECK(heat_enable_acquire_since(HEAT_ENABLE_CLAIMANT_AUTOTUNE, at_epoch) == false,
               "the autotune claimant's start path refuses a stop in the same window");
    TEST_CHECK(g_enable_true_calls == 0, "and asked for no heat");

    /* ---- Structural: the two per-tick idle backstops must be the ONLY
     * release sites spelled as the backstop form. Every other release site is
     * a real transition and must keep the always-advance spelling -- if one of
     * them were quietly converted to the backstop form, this fix would be
     * inert again for that path, silently. */
    char *pe = profile_executor_read_source();
    if (!pe) {
        TEST_CHECK(false, "could not locate drivers/control/profile_executor.c to source-scan");
        return;
    }
    TEST_CHECK(strstr(pe, "heat_enable_release_backstop(HEAT_ENABLE_CLAIMANT_PROFILE);") != NULL,
               "profile_executor.c's per-tick 'state is not RUNNING' branch must use the BACKSTOP "
               "form -- with the plain form every idle tick would advance the stop generation and "
               "spuriously refuse a legitimate run start that raced one");
    TEST_CHECK(strstr(pe, "heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);") == NULL,
               "...and profile_executor.c's tick loop has no plain release left in it");
    free(pe);

    char *he = heat_enable_read_source();
    if (!he) {
        TEST_CHECK(false, "could not locate drivers/control/heat_enable.c to source-scan");
        return;
    }
    TEST_CHECK(strstr(he, "if (stop_transition || was_held) {") != NULL,
               "MUST GO RED if the stop-generation advance goes back to being gated on was_held "
               "alone -- that is precisely the inert form this fix replaced");
    free(he);
}

static void test_reconcile_runs_last_in_the_watchdog_loop(void)
{
    TEST_SECTION("profile_executor -- heat_enable_reconcile() is the LAST statement in the guard-9 "
                 "watchdog loop body, not the first: it can block ~5.2 s and every liveness check "
                 "in that body sits behind it (2026-09-15 review of 8813bedd)");

    char *text = profile_executor_read_source();
    if (!text) {
        TEST_CHECK(false, "could not locate drivers/control/profile_executor.c -- update the "
                           "candidate paths in this test if the build layout moved");
        return;
    }

    const char *call = strstr(text, "heat_enable_reconcile();");
    TEST_CHECK(call != NULL, "the watchdog loop still calls heat_enable_reconcile()");
    if (!call) {
        free(text);
        return;
    }
    TEST_CHECK(strstr(call + 1, "heat_enable_reconcile();") == NULL,
               "exactly one call site -- a second one would need its own ordering argument");

    /* The stale-tick test is the guard-9 check whose latency the call used to
     * push out; it must run BEFORE the reconcile call.
     *
     * Anchor on the LAST occurrence of each marker, not the first. The first
     * version of this test used strstr() (first match) for
     * WATCHDOG_TICK_DEAD_MS, which finds its #define near the top of the
     * file -- always far above the call site whether the call sits first or
     * last in the loop body. That made the test vacuous, and it was caught by
     * actually running it against the parent commit, where it passed while
     * the call was still FIRST in the loop. The last occurrence is the
     * in-loop use, which is the thing whose ordering matters. */
    const char *stale = last_occurrence(text, "WATCHDOG_TICK_DEAD_MS");
    TEST_CHECK(stale != NULL, "the loop still has its stale-tick deadline check");
    if (stale) {
        TEST_CHECK(stale < call,
                   "the stale-tick (guard 9) check runs BEFORE heat_enable_reconcile() -- if this "
                   "fails, the blocking retry was moved back ahead of the liveness checks and "
                   "guard 9's detection latency grew by up to a full ~5.2 s link timeout");
    }

    /* Second, independent anchor on the far end of the loop body: the
     * faulted-run breadcrumb is the last real work the body does. The
     * reconcile call must come after it, which is only true if it is genuinely
     * last rather than merely after the stale-tick check. */
    const char *tail = last_occurrence(text, "run_state_note(RUN_STATE_PHASE_FAULTED");
    TEST_CHECK(tail != NULL, "the loop still writes the faulted-run breadcrumb");
    if (tail) {
        TEST_CHECK(tail < call,
                   "heat_enable_reconcile() comes after the loop body's final breadcrumb write -- "
                   "i.e. it is genuinely the LAST statement in the body, not just after the "
                   "stale-tick check");
    }

    /* Nothing may be reintroduced after it that reads heat-enable state: the
     * move is only safe because nothing in the body depends on the reconcile
     * having already run this tick. */
    TEST_CHECK(strstr(call + strlen("heat_enable_reconcile();"), "heat_enable_") == NULL,
               "no heat_enable_* use follows the reconcile call -- if this fails, something was "
               "added after it that may depend on it having run, which is the ordering the move "
               "was checked against");
    free(text);
}

static void test_flush_bound_comment_is_correctly_derived(void)
{
    TEST_SECTION("heat_enable -- the worst-case timing comment names the term that actually "
                 "blocks (xact_lock) and not a reply timeout that does not exist on a "
                 "fire-and-forget broadcast (2026-09-15 review of 8813bedd)");

    char *text = heat_enable_read_source();
    if (!text) {
        TEST_CHECK(false, "could not locate drivers/control/heat_enable.c");
        return;
    }

    /* REQUEST_ENABLE goes out via safety_exchange(..., expect_status=false),
     * and safety_link_inbox.c only enters the SAFETY_LINK_REPLY_TIMEOUT_MS
     * wait inside `if (expect_status)`. Quoting that constant as part of this
     * path's bound is the mis-derivation the review found. */
    TEST_CHECK(strstr(text, "SAFETY_LINK_REPLY_TIMEOUT_MS, safety_link.h") == NULL,
               "the bound must not be derived from SAFETY_LINK_REPLY_TIMEOUT_MS -- REQUEST_ENABLE "
               "is a broadcast with no reply to wait for");
    TEST_CHECK(strstr(text, "roughly 5.5 s") == NULL,
               "the discredited ~5.5 s figure (which included that phantom reply term) is gone");
    TEST_CHECK(strstr(text, "SAFETY_XACT_LOCK_TIMEOUT_MS") != NULL,
               "the bound names SAFETY_XACT_LOCK_TIMEOUT_MS, which is the term that actually "
               "blocks");
    TEST_CHECK(strstr(text, "5.2 s") != NULL, "and quotes the corrected ~5.2 s worst case");

    /* A 5.2 s block exceeds the 5 s panic-on-expiry Task WDT period. The only
     * reason that is survivable is that no task is subscribed. Whoever first
     * calls esp_task_wdt_add() must find that out here. */
    TEST_CHECK(strstr(text, "esp_task_wdt_add()") != NULL,
               "the blocking site names its dependency on there being ZERO esp_task_wdt_add() "
               "call sites in the tree -- without that note, the first task subscribed to the "
               "Task WDT learns about this 5.2 s block from a panic reset instead");
    free(text);
}

static void test_bad_claimant(void)
{
    TEST_SECTION("heat_enable -- an out-of-range claimant is refused, not indexed");

    reset_all(true);
    TEST_CHECK(heat_enable_acquire(HEAT_ENABLE_CLAIMANT_COUNT) == false, "acquire refuses");
    heat_enable_release(HEAT_ENABLE_CLAIMANT_COUNT);
    TEST_CHECK(enable_sends() == 0 && release_sends() == 0, "and neither touched the wire");
}

static void test_k4_mismatch_rerequests_with_backoff_then_gives_up(void)
{
    TEST_SECTION("heat_enable -- F1: grant held but Pico ARMED with K4 open is re-requested, bounded");

    const uint8_t ARMED = SAFETY_LINK_DIAG_STATE_ARMED;
    reset_all(true);
    TEST_CHECK(heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE), "granted");
    uint32_t t = 1000;
    // GRACE (Pico rebooted): K4 cannot be expected closed, nothing timed.
    heat_enable_note_pico_state(true, SAFETY_LINK_DIAG_STATE_GRACE, false, t);
    t += 10000;
    heat_enable_note_pico_state(true, SAFETY_LINK_DIAG_STATE_GRACE, false, t);
    heat_enable_reconcile();
    TEST_CHECK(enable_sends() == 1, "no re-request during GRACE");
    // ARMED, K4 open: clock starts, no resend before 3 s.
    heat_enable_note_pico_state(true, ARMED, false, t);
    t += 2900;
    heat_enable_note_pico_state(true, ARMED, false, t);
    heat_enable_reconcile();
    TEST_CHECK(enable_sends() == 1 && heat_enable_is_granted(), "still granted inside the 3 s window");
    // 3 s: first re-request; granted reads false until it lands.
    t += 200;
    heat_enable_note_pico_state(true, ARMED, false, t);
    TEST_CHECK(!heat_enable_is_granted(), "not claimed granted while re-requesting");
    heat_enable_reconcile();
    TEST_CHECK(enable_sends() == 2, "first re-request sent");
    TEST_CHECK(heat_enable_is_granted(), "granted again once the re-request is accepted");
    // Backoff: next one only after 6 s more.
    t += 5000;
    heat_enable_note_pico_state(true, ARMED, false, t);
    heat_enable_reconcile();
    TEST_CHECK(enable_sends() == 2, "backoff holds the second re-request");
    t += 1100;
    heat_enable_note_pico_state(true, ARMED, false, t);
    heat_enable_reconcile();
    TEST_CHECK(enable_sends() == 3, "second re-request after 6 s");
    for (int i = 0; i < 40 && !heat_enable_grant_unconfirmed(); i++) {
        t += 30000;
        heat_enable_note_pico_state(true, ARMED, false, t);
        heat_enable_reconcile();
    }
    TEST_CHECK(enable_sends() == 5, "exactly 4 re-requests (5 sends total), then no more");
    TEST_CHECK(heat_enable_grant_unconfirmed(), "gave up: unconfirmed flag set");
    TEST_CHECK(!heat_enable_is_granted(), "never claims heat enabled while K4 is open");
    TEST_CHECK(heat_enable_retry_pending(), "surfaced as retry-pending");
    t += 60000;
    heat_enable_note_pico_state(true, ARMED, false, t);
    heat_enable_reconcile();
    TEST_CHECK(enable_sends() == 5, "bounded: no further sends");
    // K4 finally closes: everything clears.
    heat_enable_note_pico_state(true, ARMED, true, t + 100);
    TEST_CHECK(!heat_enable_grant_unconfirmed() && heat_enable_is_granted(), "K4 closed clears the fault");
}

static void test_k4_closed_or_released_never_resends(void)
{
    TEST_SECTION("heat_enable -- F1: K4 closed, stale status, or no claim never triggers a re-request");

    reset_all(true);
    heat_enable_note_pico_state(true, SAFETY_LINK_DIAG_STATE_ARMED, false, 100);
    heat_enable_note_pico_state(true, SAFETY_LINK_DIAG_STATE_ARMED, false, 100000);
    TEST_CHECK(enable_sends() == 0, "no claim held: nothing re-requested");
    TEST_CHECK(heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE), "granted");
    for (uint32_t t = 1000; t < 60000; t += 1000) {
        heat_enable_note_pico_state(true, SAFETY_LINK_DIAG_STATE_ARMED, true, t);
        heat_enable_note_pico_state(false, SAFETY_LINK_DIAG_STATE_ARMED, false, t);
        heat_enable_reconcile();
    }
    TEST_CHECK(enable_sends() == 1, "K4 closed / stale status: still exactly one send");
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_note_pico_state(true, SAFETY_LINK_DIAG_STATE_ARMED, false, 70000);
    heat_enable_note_pico_state(true, SAFETY_LINK_DIAG_STATE_ARMED, false, 90000);
    heat_enable_reconcile();
    TEST_CHECK(enable_sends() == 1, "after release nothing is re-requested");
}

/* ---- Safety-link fix batch 2 -------------------------------------------- */

static void test_k4_timer_and_episode_restart(void)
{
    TEST_SECTION("heat_enable -- LOW-1/LOW-2: WARN is timed like ARMED; leaving ARMED ends the episode");

    const uint8_t ARMED = SAFETY_LINK_DIAG_STATE_ARMED;
    const uint8_t WARN = SAFETY_LINK_DIAG_STATE_WARN;
    reset_all(true);
    TEST_CHECK(heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE), "granted");
    uint32_t t = 1000;
    /* WARN with K4 open is timed and re-requested exactly like ARMED. */
    heat_enable_note_pico_state(true, WARN, false, t);
    t += 3100;
    heat_enable_note_pico_state(true, WARN, false, t);
    heat_enable_reconcile();
    TEST_CHECK(enable_sends() == 2, "WARN + K4 open re-requests after 3 s (LOW-2)");

    /* ARMED, K4 open: timer running mid-episode. Then GRACE: the episode ends, so the
     * timer AND the resend budget restart (LOW-1, LOW-8-2). Without the clear the next
     * ARMED tick would resend at once on the old deadline. */
    reset_all(true);
    TEST_CHECK(heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE), "granted again");
    t = 1000;
    heat_enable_note_pico_state(true, ARMED, false, t); /* timer starts */
    heat_enable_note_pico_state(true, SAFETY_LINK_DIAG_STATE_GRACE, false, t + 500);
    t += 20000; /* far past the old deadline */
    heat_enable_note_pico_state(true, ARMED, false, t); /* ARMED again: clock restarts */
    heat_enable_reconcile();
    TEST_CHECK(enable_sends() == 1, "GRACE -> ARMED restarts the 3 s clock: no instant re-request");
    t += 3100;
    heat_enable_note_pico_state(true, ARMED, false, t);
    heat_enable_reconcile();
    TEST_CHECK(enable_sends() == 2, "and the re-request comes 3 s after the NEW ARMED stretch began");

    /* Stale status: the timer stops; a later fresh ARMED restarts it (no stale deadline). */
    reset_all(true);
    TEST_CHECK(heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE), "granted (stale case)");
    t = 1000;
    heat_enable_note_pico_state(true, ARMED, false, t);
    heat_enable_note_pico_state(false, ARMED, false, t + 500);
    t += 20000;
    heat_enable_note_pico_state(true, ARMED, false, t);
    heat_enable_reconcile();
    TEST_CHECK(enable_sends() == 1, "stale then fresh ARMED restarts the clock: no instant re-request");

    /* The give-up latch is per ARMED episode: GRACE clears it. */
    reset_all(true);
    TEST_CHECK(heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE), "granted (latch case)");
    t = 1000;
    for (int i = 0; i < 40 && !heat_enable_grant_unconfirmed(); i++) {
        t += 30000;
        heat_enable_note_pico_state(true, ARMED, false, t);
        heat_enable_reconcile();
    }
    TEST_CHECK(heat_enable_grant_unconfirmed(), "gave up in the first episode");
    heat_enable_note_pico_state(true, SAFETY_LINK_DIAG_STATE_GRACE, false, t + 1000);
    TEST_CHECK(!heat_enable_grant_unconfirmed(), "leaving ARMED clears the unconfirmed latch (LOW-1)");
}

static void test_pico_reboot_after_tripped_holds(void)
{
    TEST_SECTION("heat_enable -- T3: a reboot that followed a TRIPPED DIAG is fatal even if benign-looking");
    reset_all(true);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_note_pico_boot(1u, true, 0u, 1000u);
    heat_enable_note_pico_state(true, SAFETY_LINK_DIAG_STATE_TRIPPED, false, 1000u);
    heat_enable_note_pico_boot(2u, true, SAFETY_LINK_DIAG_BOOT_POWERON, 2000u);
    TEST_CHECK(heat_enable_reboot_hold(), "POWERON reboot right after a TRIPPED DIAG -> hold (trip latch lost)");

    reset_all(true);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_note_pico_boot(1u, true, 0u, 1000u);
    heat_enable_note_pico_state(true, SAFETY_LINK_DIAG_STATE_TRIPPED, false, 1000u);
    heat_enable_note_pico_state(true, SAFETY_LINK_DIAG_STATE_ARMED, true, 1500u); /* trip cleared */
    heat_enable_note_pico_boot(2u, true, SAFETY_LINK_DIAG_BOOT_POWERON, 2000u);
    TEST_CHECK(!heat_enable_reboot_hold(), "a POWERON reboot after the trip was cleared stays benign");

    /* I4: the executor watchdog feeds INIT while no DIAG of the new boot has arrived; the
     * snapshot (not the live flag) must still carry the trip into the verdict. */
    reset_all(true);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_note_pico_boot(1u, true, 0u, 1000u);
    heat_enable_note_pico_state(true, SAFETY_LINK_DIAG_STATE_TRIPPED, false, 1000u);
    heat_enable_note_pico_boot(2u, false, 0u, 2000u);
    heat_enable_note_pico_state(true, SAFETY_LINK_DIAG_STATE_INIT, false, 2100u);
    heat_enable_note_pico_boot(2u, true, SAFETY_LINK_DIAG_BOOT_POWERON, 2200u);
    TEST_CHECK(heat_enable_reboot_hold(), "I4: INIT tick between reboot and DIAG does not lose the trip snapshot");

    /* F3: a second reboot before the first boot's DIAG keeps the snapshot. */
    reset_all(true);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_note_pico_boot(1u, true, 0u, 1000u);
    heat_enable_note_pico_state(true, SAFETY_LINK_DIAG_STATE_TRIPPED, false, 1000u);
    heat_enable_note_pico_boot(2u, false, 0u, 2000u);
    heat_enable_note_pico_state(true, SAFETY_LINK_DIAG_STATE_INIT, false, 2100u);
    heat_enable_note_pico_boot(3u, false, 0u, 2200u);
    heat_enable_note_pico_boot(3u, true, SAFETY_LINK_DIAG_BOOT_POWERON, 2300u);
    TEST_CHECK(heat_enable_reboot_hold(), "F3: double reboot before any DIAG still holds (trip lost)");

    /* F8: TRIPPED, release, reboot with no claim, POWERON DIAG, then acquire -> hold. */
    reset_all(true);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_note_pico_boot(1u, true, 0u, 1000u);
    heat_enable_note_pico_state(true, SAFETY_LINK_DIAG_STATE_TRIPPED, false, 1000u);
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_note_pico_boot(2u, false, 0u, 2000u);
    heat_enable_note_pico_boot(2u, true, SAFETY_LINK_DIAG_BOOT_POWERON, 2100u);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    TEST_CHECK(heat_enable_reboot_hold(), "F8: lost trip across a claim-less reboot holds on the next acquire");

    /* F8 caveat: reboot while no claim, DIAG arrives after the next acquire. */
    reset_all(true);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_note_pico_boot(1u, true, 0u, 1000u);
    heat_enable_note_pico_state(true, SAFETY_LINK_DIAG_STATE_TRIPPED, false, 1000u);
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_note_pico_boot(2u, false, 0u, 2000u);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_note_pico_boot(2u, true, SAFETY_LINK_DIAG_BOOT_POWERON, 2100u);
    TEST_CHECK(heat_enable_reboot_hold(), "F8: DIAG after the re-acquire still holds");

    /* F8: release while the reboot is unresolved must not clear the snapshot. */
    reset_all(true);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_note_pico_boot(1u, true, 0u, 1000u);
    heat_enable_note_pico_state(true, SAFETY_LINK_DIAG_STATE_TRIPPED, false, 1000u);
    heat_enable_note_pico_boot(2u, false, 0u, 2000u);
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_note_pico_boot(2u, true, SAFETY_LINK_DIAG_BOOT_POWERON, 2100u);
    TEST_CHECK(heat_enable_reboot_hold(), "F8: release/re-acquire with the reboot unresolved keeps the trip snapshot");
}

static void test_reboot_fatal_latch_survives_newer_seq(void)
{
    TEST_SECTION("heat_enable -- firing audit 2 MED-1/LOW-3: an unconsumed fatal verdict is never "
                 "downgraded by a newer reboot seq (double count of one reboot)");
    /* verdict without a claim: fatal cause latches, benign cause does not */
    reset_all(true);
    heat_enable_note_pico_boot(1u, true, 0u, 1000u);
    heat_enable_note_pico_boot(2u, false, 0u, 2000u);
    heat_enable_note_pico_boot(2u, true, SAFETY_LINK_DIAG_BOOT_WATCHDOG, 2100u);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    TEST_CHECK(heat_enable_reboot_hold(), "fatal cause while claim-less latches: next acquire holds");
    reset_all(true);
    heat_enable_note_pico_boot(1u, true, 0u, 1000u);
    heat_enable_note_pico_boot(2u, false, 0u, 2000u);
    heat_enable_note_pico_boot(2u, true, SAFETY_LINK_DIAG_BOOT_POWERON, 2100u);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    TEST_CHECK(!heat_enable_reboot_hold(), "benign cause while claim-less: next acquire does not hold");

    /* P2: lost trip latched, then the SAME reboot counted again (seq+2) after the executor refreshed
     * last_pico_tripped from the new boot's GRACE DIAG: the latch must survive. */
    reset_all(true);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_note_pico_boot(1u, true, 0u, 1000u);
    heat_enable_note_pico_state(true, SAFETY_LINK_DIAG_STATE_TRIPPED, false, 1000u);
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_note_pico_boot(2u, true, SAFETY_LINK_DIAG_BOOT_POWERON, 2000u); /* DIAG first: T3 latch */
    heat_enable_note_pico_state(true, SAFETY_LINK_DIAG_STATE_GRACE, false, 2100u);
    heat_enable_note_pico_boot(3u, false, 0u, 2200u);                           /* FW_VERSION double count */
    heat_enable_note_pico_boot(3u, true, SAFETY_LINK_DIAG_BOOT_POWERON, 4200u); /* benign DIAG */
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    TEST_CHECK(heat_enable_reboot_hold(), "MUST GO RED if a newer seq erases the unconsumed T3 lost-trip latch");
}

static void test_pico_reboot_cause_holds_or_retries(void)
{
    TEST_SECTION("heat_enable -- MED-1: a fatal-cause Pico reboot is never silently re-requested");

    const uint8_t ARMED = SAFETY_LINK_DIAG_STATE_ARMED;
    const uint8_t WD = SAFETY_LINK_DIAG_BOOT_WATCHDOG;
    reset_all(true);
    TEST_CHECK(heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE), "granted");
    uint32_t t = 1000;
    heat_enable_note_pico_boot(5u, true, 0u, t); /* first sight: establishes the baseline only */
    heat_enable_note_pico_state(true, ARMED, true, t);
    TEST_CHECK(!heat_enable_reboot_hold(), "no hold without a reboot");

    /* Reboot detected, new boot's DIAG not seen yet: undecided, not a hold. */
    t += 100;
    heat_enable_note_pico_boot(6u, false, 0u, t);
    TEST_CHECK(!heat_enable_reboot_hold(), "cause unknown yet: not a hold");
    /* First DIAG of the new boot says WATCHDOG. */
    t += 200;
    heat_enable_note_pico_boot(6u, true, WD, t);
    TEST_CHECK(heat_enable_reboot_hold(), "WATCHDOG boot reason -> hold");
    TEST_CHECK(!heat_enable_is_granted(), "the grant is withdrawn, never claimed while held");
    for (int i = 0; i < 10; i++) {
        t += 5000;
        heat_enable_note_pico_boot(6u, true, WD, t);
        heat_enable_note_pico_state(true, ARMED, false, t);
        heat_enable_reconcile();
    }
    TEST_CHECK(enable_sends() == 1, "K4 stays open: no REQUEST_ENABLE after a fatal reboot, ever");
    TEST_CHECK(heat_enable_reboot_hold(), "the hold persists while the claim is held");
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
    TEST_CHECK(!heat_enable_reboot_hold(), "the hold ends with the claim");
    TEST_CHECK(heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE) && enable_sends() == 2,
               "a fresh operator-started claim requests heat normally");

    /* Each fatal bit holds; a benign one does not. */
    const uint8_t fatal_bits[] = {SAFETY_LINK_DIAG_BOOT_WATCHDOG, SAFETY_LINK_DIAG_BOOT_BROWNOUT,
                                  SAFETY_LINK_DIAG_BOOT_STACK_OVERFLOW, SAFETY_LINK_DIAG_BOOT_MALLOC_FAILED,
                                  SAFETY_LINK_DIAG_BOOT_ASSERT_FAILED};
    for (unsigned i = 0; i < sizeof(fatal_bits); i++) {
        reset_all(true);
        (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
        heat_enable_note_pico_boot(1u, true, 0u, 1000u);
        heat_enable_note_pico_boot(2u, true, (uint8_t)(SAFETY_LINK_DIAG_BOOT_POWERON | fatal_bits[i]), 2000u);
        TEST_CHECK(heat_enable_reboot_hold(), "every fatal boot-reason bit holds");
    }
    /* MED-A: a fatal-reboot hold on a standing grant owes the wire a REQUEST_ENABLE(false). */
    reset_all(true);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_note_pico_boot(1u, true, 0u, 1000u);
    heat_enable_note_pico_boot(2u, true, SAFETY_LINK_DIAG_BOOT_WATCHDOG, 2000u);
    heat_enable_service_pending_release();
    TEST_CHECK(release_sends() >= 1, "fatal reboot hold sends REQUEST_ENABLE(false) for the standing grant");
    reset_all(true);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_note_pico_boot(1u, true, 0u, 1000u);
    heat_enable_note_pico_boot(2u, true, SAFETY_LINK_DIAG_BOOT_POWERON, 2000u);
    TEST_CHECK(!heat_enable_reboot_hold(), "POWERON alone is benign: no hold");
    /* Benign: F1 re-requests once ARMED and K4 reads open (existing behaviour, TODO owner decision). */
    heat_enable_note_pico_state(true, ARMED, false, 3000u);
    heat_enable_note_pico_state(true, ARMED, false, 6200u);
    heat_enable_reconcile();
    TEST_CHECK(enable_sends() == 2, "benign reboot keeps the F1 re-request");

    /* No DIAG for a long time: still undecided (no timeout fallback), and a LATE fatal DIAG holds. */
    reset_all(true);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_note_pico_boot(1u, true, 0u, 1000u);
    heat_enable_note_pico_boot(2u, false, 0u, 2000u);
    heat_enable_note_pico_boot(2u, false, 0u, 12100u);
    heat_enable_note_pico_boot(2u, true, SAFETY_LINK_DIAG_BOOT_WATCHDOG, 12200u);
    TEST_CHECK(heat_enable_reboot_hold(), "a late fatal DIAG still holds: no timeout turns it benign");

    /* While the cause is undecided, a pending (link-down) request is not retried. */
    reset_all(false);
    TEST_CHECK(!heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE), "link down: pending");
    heat_enable_note_pico_boot(1u, true, 0u, 1000u);
    heat_enable_note_pico_boot(2u, false, 0u, 2000u);
    g_link_up = true;
    heat_enable_reconcile();
    TEST_CHECK(g_enable_true_calls == 1,
               "while the cause is undecided the pending request is not re-sent");
}

static void test_reboot_verdict_survives_release_and_resume(void)
{
    TEST_SECTION("heat_enable -- firing audit MED-5: an undecided reboot verdict survives pause/resume; "
                 "resume withholds heat and a late fatal DIAG still holds");
    const uint8_t WD = SAFETY_LINK_DIAG_BOOT_WATCHDOG;
    /* (a) pause while undecided, resume before the DIAG: heat withheld, late fatal DIAG holds. */
    reset_all(true);
    TEST_CHECK(heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE), "granted");
    heat_enable_note_pico_boot(1u, true, 0u, 1000u);
    heat_enable_note_pico_boot(2u, false, 0u, 2000u);
    TEST_CHECK(heat_enable_reboot_undecided(), "setup: undecided");
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE); /* operator pause */
    int sends = enable_sends();
    TEST_CHECK(!heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE), "resume while undecided is withheld");
    TEST_CHECK(enable_sends() == sends, "MUST GO RED if resume sends REQUEST_ENABLE during an undecided reboot");
    TEST_CHECK(heat_enable_reboot_undecided(), "the verdict is still pending after resume");
    heat_enable_note_pico_boot(2u, true, WD, 3000u);
    TEST_CHECK(heat_enable_reboot_hold(), "MUST GO RED if the late fatal DIAG is ignored after pause/resume");
    TEST_CHECK(enable_sends() == sends, "no heat requested");

    /* (b) benign verdict after the withheld resume: reconcile re-requests. */
    reset_all(true);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_note_pico_boot(1u, true, 0u, 1000u);
    heat_enable_note_pico_boot(2u, false, 0u, 2000u);
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
    sends = enable_sends();
    TEST_CHECK(!heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE), "withheld");
    heat_enable_note_pico_boot(2u, true, SAFETY_LINK_DIAG_BOOT_POWERON, 3000u);
    TEST_CHECK(!heat_enable_reboot_hold() && !heat_enable_reboot_undecided(), "benign: decided, no hold");
    heat_enable_reconcile();
    TEST_CHECK(enable_sends() == sends + 1, "benign verdict: the withheld claim is re-requested");

    /* (c) fatal DIAG arrives while paused (no claim): next acquire starts under a hold. */
    reset_all(true);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_note_pico_boot(1u, true, 0u, 1000u);
    heat_enable_note_pico_boot(2u, false, 0u, 2000u);
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
    heat_enable_note_pico_boot(2u, true, WD, 3000u);
    sends = enable_sends();
    TEST_CHECK(!heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE), "fatal verdict landed while paused: withheld");
    TEST_CHECK(heat_enable_reboot_hold(), "MUST GO RED if a fatal DIAG that arrived while paused is dropped");
    TEST_CHECK(enable_sends() == sends, "no heat requested");
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
    TEST_CHECK(heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE), "the second, deliberate resume proceeds");
}

static void test_enable_in_flight_under_classify_pending_queues_release(void)
{
    TEST_SECTION("heat_enable -- SL3-R2 A3: a reboot noticed (undecided) while an enable send is IN FLIGHT "
                 "must not record granted=true");
    reset_all(true);
    heat_enable_note_pico_boot(5u, true, 0u, 1000u); /* baseline */
    g_classify_during_enable = true;
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    TEST_CHECK(g_classify_during_enable_ran, "sanity: classification became pending mid-send");
    TEST_CHECK(heat_enable_reboot_undecided(), "sanity: undecided");
    TEST_CHECK(!heat_enable_is_granted(),
               "MUST GO RED if send_enable records granted=true under a pending classification");
    uint32_t rel0 = release_sends();
    heat_enable_service_pending_release();
    TEST_CHECK(release_sends() == rel0 + 1, "the standing enable is released on the wire");
}

static void test_enable_in_flight_under_reboot_hold_queues_release(void)
{
    TEST_SECTION("heat_enable -- review-2 LOW-1: a fatal-reboot hold set while an enable send is IN FLIGHT "
                 "must not leave granted=true; the standing enable is released");
    reset_all(true);
    heat_enable_note_pico_boot(5u, true, 0u, 1000u); /* baseline */
    g_hold_during_enable = true;
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    TEST_CHECK(g_hold_during_enable_ran, "sanity: the reboot hold was set mid-send");
    TEST_CHECK(heat_enable_reboot_hold(), "sanity: hold is set");
    TEST_CHECK(!heat_enable_is_granted(),
               "MUST GO RED if send_enable leaves granted=true under a reboot hold set while in flight");
    uint32_t rel0 = release_sends();
    heat_enable_service_pending_release();
    TEST_CHECK(release_sends() == rel0 + 1 && g_last_enable_value == false,
               "the standing enable is released on the wire (release queued)");
    uint32_t en0 = enable_sends();
    heat_enable_reconcile();
    TEST_CHECK(enable_sends() == en0, "no re-request while the hold stands");
}

static void test_executor_wires_k4_and_reboot_state(void){
    TEST_SECTION("profile_executor -- the watchdog feeds heat_enable the real K4 / reboot facts and "
                 "pauses (LOW-8-1, MED-1, MED-2)");
    char *text = profile_executor_read_source();
    if (!text) {
        TEST_CHECK(false, "could not locate drivers/control/profile_executor.c");
        return;
    }
    const char *k4 = strstr(text, "safety_k4_closed = safety_status.link_up && (safety_status.flags & SAFETY_FLAG_RELAY) != 0u;");
    TEST_CHECK(k4 != NULL, "K4-closed comes from link_up AND the Pico's RELAY flag, not a constant");
    const char *seq = strstr(text, "safety_reboot_seq = safety_status.pico_reboot_seq;");
    const char *since = strstr(text, "safety_diag_since_reboot = safety_status.diag_since_reboot;");
    const char *br = strstr(text, "safety_boot_reason = safety_status.diag_boot_reason;");
    TEST_CHECK(seq && since && br, "reboot seq / diag-since-reboot / boot reason come from safety_status");
    const char *boot = strstr(text, "heat_enable_note_pico_boot(safety_reboot_seq, safety_diag_since_reboot, safety_boot_reason,");
    const char *state = strstr(text, "heat_enable_note_pico_state(safety_diag_valid,");
    const char *hold = strstr(text, "profile_executor_pause_with_reason_bounded(\"pico_fatal_reboot\")");
    const char *unc = strstr(text, "profile_executor_pause_with_reason_bounded(\"heat_grant_unconfirmed\")");
    const char *rec = strstr(text, "heat_enable_reconcile();");
    TEST_CHECK(boot && state && boot < state, "note_pico_boot runs before note_pico_state");
    TEST_CHECK(hold && unc && state && rec && state < hold && hold < unc && unc < rec,
               "reboot-hold then unconfirmed-grant pauses sit between note_pico_state and reconcile");
    TEST_CHECK(strstr(text, "heat_enable_reboot_hold()") != NULL && strstr(text, "heat_enable_grant_unconfirmed()") != NULL,
               "the pause decisions read heat_enable_reboot_hold()/heat_enable_grant_unconfirmed()");
    free(text);
}

static void test_reboot_resets_k4_episode_and_undecided_is_visible(void)
{
    TEST_SECTION("heat_enable -- K5: a Pico reboot restarts the K4 episode; undecided state is visible (sl3)");
    const uint8_t ARMED = SAFETY_LINK_DIAG_STATE_ARMED;
    reset_all(true);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    uint32_t t = 1000;
    heat_enable_note_pico_boot(1u, true, 0u, t);
    for (int i = 0; i < 40 && !heat_enable_grant_unconfirmed(); i++) {
        t += 30000;
        heat_enable_note_pico_state(true, ARMED, false, t);
        heat_enable_reconcile();
    }
    TEST_CHECK(heat_enable_grant_unconfirmed(), "setup: old boot's K4 episode gave up");
    TEST_CHECK(!heat_enable_reboot_undecided(), "no reboot yet: not undecided");
    int sends_before = enable_sends();
    t += 100;
    heat_enable_note_pico_boot(2u, false, 0u, t);
    TEST_CHECK(heat_enable_reboot_undecided(), "reboot seen, no DIAG of the new boot: undecided");
    TEST_CHECK(!heat_enable_grant_unconfirmed(),
               "the new boot starts a new K4 episode: the old boot's give-up is cleared");
    t += 100;
    heat_enable_note_pico_boot(2u, true, SAFETY_LINK_DIAG_BOOT_POWERON, t);
    TEST_CHECK(!heat_enable_reboot_undecided(), "DIAG decides it");
    /* The new episode gets its own full retry budget, not the exhausted one. */
    heat_enable_note_pico_state(true, ARMED, false, t);
    t += 3200;
    heat_enable_note_pico_state(true, ARMED, false, t);
    heat_enable_reconcile();
    TEST_CHECK(enable_sends() > sends_before, "new episode re-requests again after the reboot");
}

static void test_executor_autotune_and_stale_diag_wiring(void)
{
    TEST_SECTION("profile_executor -- autotune is stopped with the run, stale-boot DIAG fed as INIT (K9, sl3)");
    char *text = profile_executor_read_source();
    if (!text) {
        TEST_CHECK(false, "could not locate drivers/control/profile_executor.c");
        return;
    }
    const char *hold = strstr(text, "profile_executor_pause_with_reason_bounded(\"pico_fatal_reboot\")");
    const char *at1 = strstr(text, "autotune_engine_abort_bounded(\"pico_fatal_reboot\"");
    TEST_CHECK(strstr(text, "autotune_engine_abort(\"pico_fatal_reboot\")") == NULL &&
                   strstr(text, "autotune_engine_abort(\"heat_grant_unconfirmed\")") == NULL,
               "SL3-R2 A2: the watchdog must not call the unbounded autotune_engine_abort()");
    const char *unc = strstr(text, "profile_executor_pause_with_reason_bounded(\"heat_grant_unconfirmed\")");
    const char *at2 = strstr(text, "autotune_engine_abort_bounded(\"heat_grant_unconfirmed\"");
    TEST_CHECK(hold && at1 && at1 > hold && at1 < unc, "fatal-reboot hold also aborts autotune");
    TEST_CHECK(unc && at2 && at2 > unc, "unconfirmed grant also aborts autotune");
    TEST_CHECK(strstr(text, "safety_diag_since_reboot ? safety_diag_state : SAFETY_LINK_DIAG_STATE_INIT") != NULL,
               "an old-boot DIAG is fed as INIT so its ARMED cannot satisfy the new boot's K4 check");
    free(text);
}

void run_test_heat_enable(void)
{
    test_reboot_resets_k4_episode_and_undecided_is_visible();
    test_executor_autotune_and_stale_diag_wiring();
    test_k4_timer_and_episode_restart();
    test_pico_reboot_cause_holds_or_retries();
    test_pico_reboot_after_tripped_holds();
    test_reboot_fatal_latch_survives_newer_seq();
    test_enable_in_flight_under_classify_pending_queues_release();
    test_reboot_verdict_survives_release_and_resume();
    test_enable_in_flight_under_reboot_hold_queues_release();
    test_watchdog_loop_and_bounded_pause_wiring();
    test_executor_wires_k4_and_reboot_state();
    test_k4_mismatch_rerequests_with_backoff_then_gives_up();
    test_k4_closed_or_released_never_resends();
    test_start_requests_exactly_once();
    test_release_on_stop();
    test_two_claimants_refcount();
    test_link_down_at_start_is_honest();
    test_reconcile_retries_then_stops();
    test_release_after_failed_request_still_sends();
    test_release_failure_is_checked_and_logged();
    test_release_defers_the_wire_send_off_the_callers_stack();
    test_reenable_never_races_ahead_of_a_pending_release();
    test_reenable_cannot_overtake_an_inflight_release();
    test_failed_release_is_retried_not_dropped();
    test_flush_drives_the_exchange_at_most_once();
    test_reconcile_never_drives_the_blocking_exchange_unconditionally();
    test_stale_claim_is_not_resurrected();
    test_stop_in_the_sample_then_spend_window_is_not_resurrected();
    test_reconcile_runs_last_in_the_watchdog_loop();
    test_flush_bound_comment_is_correctly_derived();
    test_bad_claimant();
}
