// Host tests for App/drivers/heat_enable.c -- the shared, refcounted holder
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

#include "../drivers/heat_enable.h"

// ---------------------------------------------------------------------------
// Fake safety link
// ---------------------------------------------------------------------------

static bool g_link_up = true;       // whether enable=true is accepted
static int  g_enable_true_calls = 0;  // attempted (including refused ones)
static int  g_enable_false_calls = 0;
static bool g_last_enable_value = false;

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
    return ESP_OK;
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
    TEST_CHECK(release_sends() == 1, "exactly one REQUEST_ENABLE(false) on the wire");
    TEST_CHECK(g_last_enable_value == false, "the last thing sent was a release");
    TEST_CHECK(!heat_enable_is_granted(), "no longer granted");
    TEST_CHECK(!heat_enable_is_held(HEAT_ENABLE_CLAIMANT_PROFILE), "the claim is gone");

    // The per-tick backstop case: both callers call release on EVERY tick
    // they spend in a non-running state. That must be free.
    for (int i = 0; i < 50; i++) {
        heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
    }
    TEST_CHECK(release_sends() == 1, "50 further releases sent nothing (backstop is free)");

    // ...and a release from a claimant that never acquired sends nothing at
    // all, which is what makes it safe to call from a path that may not have
    // started a run.
    reset_all(true);
    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
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
    TEST_CHECK(release_sends() == 0, "one of two claimants leaving sends no release");
    TEST_CHECK(heat_enable_is_granted(), "heat is still requested for the remaining claimant");
    TEST_CHECK(!heat_enable_is_held(HEAT_ENABLE_CLAIMANT_AUTOTUNE), "the leaver's claim is gone");

    heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);
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
    // enable=false is the fail-safe direction and safety_link.c attempts it
    // whether or not the link looks up -- refusing it because the link looks
    // down is the one refusal that could leave heat on.
    TEST_CHECK(release_sends() == 1, "the release still goes out on a down link");
    TEST_CHECK(!heat_enable_retry_pending(), "and the pending retry is cancelled by the release");
    heat_enable_reconcile();
    TEST_CHECK(g_enable_true_calls == 1, "reconcile does not resurrect a released claim");
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
    test_bad_claimant();
}
