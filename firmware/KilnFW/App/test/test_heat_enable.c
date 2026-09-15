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

esp_err_t safety_link_request_enable(SafetyLinkClass *link, bool enable)
{
    (void)link;
    g_last_enable_value = enable;
    if (enable) {
        g_enable_true_calls++;
        // safety_link.c: refuses and sends NOTHING when the link is down.
        return g_link_up ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    // safety_link.c: the fail-safe direction is always attempted.
    g_enable_false_calls++;
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
    const char *fn = strstr(text, "void heat_enable_release(heat_enable_claimant_t who)");
    TEST_CHECK(fn != NULL, "sanity: heat_enable_release()'s definition must be findable in the source");
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
                           "heat_enable_release()'s own function body must not call "
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

static void test_bad_claimant(void)
{
    TEST_SECTION("heat_enable -- an out-of-range claimant is refused, not indexed");

    reset_all(true);
    TEST_CHECK(heat_enable_acquire(HEAT_ENABLE_CLAIMANT_COUNT) == false, "acquire refuses");
    heat_enable_release(HEAT_ENABLE_CLAIMANT_COUNT);
    TEST_CHECK(enable_sends() == 0 && release_sends() == 0, "and neither touched the wire");
}

void run_test_heat_enable(void)
{
    test_start_requests_exactly_once();
    test_release_on_stop();
    test_two_claimants_refcount();
    test_link_down_at_start_is_honest();
    test_reconcile_retries_then_stops();
    test_release_after_failed_request_still_sends();
    test_release_failure_is_checked_and_logged();
    test_release_defers_the_wire_send_off_the_callers_stack();
    test_reenable_never_races_ahead_of_a_pending_release();
    test_bad_claimant();
}
