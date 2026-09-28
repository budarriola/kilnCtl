// Host tests for docs/WEB_AUTH_PLAN.md section 9 ("Safety interaction").
//
// OWNER DECISION, 2026-09-28 (web-auth gate tightening; supersedes section 9's
// original stop rule below): "stop needs login. there is an estop button."
// The bench's physical E-stop interlock (SaftyFW-mediated, always reachable
// regardless of the web session) is now the safety backstop for an
// unauthenticated web user, so POST /api/profile_exec/stop is
// ROUTE_TIER_USER like every other authenticated action, not
// ROUTE_TIER_SAFETY_REDUCE. The tests below now assert the opposite of the
// original acceptance criterion for the stop route specifically:
// DENY_NO_SESSION with no session, ALLOW only with a real session. The
// three other SAFETY_REDUCE routes are unaffected by this decision (they
// abort relay-driving tasks with no physical E-stop equivalent) and keep
// their original ALLOW-always coverage below.
//
// UPDATE 2026-09-17 (historical; no longer describes /api/profile_exec/stop):
// the gap this file originally documented (only 3 of 5 combinations allowed,
// because http_auth_check() had no URI awareness and route_tier_table.h
// tiered the stop route ROUTE_TIER_USER) was closed by giving stop
// ROUTE_TIER_SAFETY_REDUCE. The table-driven mechanism itself (route_tier_t
// carrying ROUTE_TIER_SAFETY_REDUCE, http_auth_check() treating it as an
// unconditional ALLOW before role/session are inspected) is unchanged and
// still used by the three routes named above.
#include "test_common.h"

#include "../drivers/http/http_auth_enforce.h"

static void test_stop_route_tier_is_user(void) {
    TEST_SECTION("route_tier_table.h -- POST /api/profile_exec/stop is ROUTE_TIER_USER "
                 "(owner decision 2026-09-28: stop requires login; the physical E-stop is "
                 "the backstop for an unauthenticated user)");

    route_tier_t tier;
    TEST_CHECK(http_auth_lookup_tier("/api/profile_exec/stop", HTTP_POST, &tier) &&
                   tier == ROUTE_TIER_USER,
               "POST /api/profile_exec/stop is ROUTE_TIER_USER in route_tier_table.h -- a "
               "re-tier back to ROUTE_TIER_SAFETY_REDUCE would silently reopen the "
               "session-less stop path the 2026-09-28 owner decision deliberately closed.");
}

static void test_clear_trip_is_admin(void) {
    TEST_SECTION("route_tier_table.h -- POST /api/safety/clear_trip is ADMIN, deliberately "
                 "(plan section 9: 'Clearing a trip is not a safety action; it re-enables heat "
                 "after one')");

    route_tier_t tier;
    TEST_CHECK(http_auth_lookup_tier("/api/safety/clear_trip", HTTP_POST, &tier) && tier == ROUTE_TIER_ADMIN,
               "POST /api/safety/clear_trip is ADMIN -- this is the deliberate exception to "
               "'nothing that reduces heat is gated': clearing a trip does not reduce heat, it "
               "re-enables heat after a trip, so gating it behind the higher tier is correct, not "
               "a violation of section 9's guarantee.");
}

// OWNER DECISION 2026-09-28: drives http_auth_check() with the REAL
// looked-up tier for the stop route and asserts the ROUTE_TIER_USER
// behavior -- DENY_NO_SESSION with no session (including a locked-out
// client, which resolves to HTTP_AUTH_ROLE_NONE the same as no session at
// all -- see http_auth_role_t's own doc comment in http_auth_enforce.h),
// ALLOW only with a real user or admin session. Auth off still collapses to
// ALLOW unconditionally (section 11).
static void test_stop_route_requires_session_when_auth_on(void) {
    TEST_SECTION("http_auth_check -- POST /api/profile_exec/stop's real looked-up tier requires "
                 "a session when auth is on (owner decision 2026-09-28: no more session-less stop)");

    route_tier_t tier;
    TEST_CHECK(http_auth_lookup_tier("/api/profile_exec/stop", HTTP_POST, &tier),
               "POST /api/profile_exec/stop must have a row in route_tier_table.h at all before "
               "the rest of this test means anything");

    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_NONE, false, false) == HTTP_AUTH_DECISION_ALLOW,
               "auth off -> ALLOW (section 11 collapse, unaffected by this decision)");
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_NONE, true, false) == HTTP_AUTH_DECISION_DENY_NO_SESSION,
               "auth on + no session -> DENY_NO_SESSION (was ALLOW before the 2026-09-28 owner "
               "decision; the login pop-up now gates this the same as any other USER-tier action)");
    // "locked" carries no distinct role value of its own -- a locked-out
    // client is, from the enforcement point's perspective, a client with no
    // resolvable session, i.e. HTTP_AUTH_ROLE_NONE. Asserted again here
    // under its own name so a future reviewer sees this combination named
    // explicitly, not merely inferred from "no session".
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_NONE, true, false) == HTTP_AUTH_DECISION_DENY_NO_SESSION,
               "auth on + locked (no resolvable session) -> DENY_NO_SESSION");
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_USER, true, false) == HTTP_AUTH_DECISION_ALLOW,
               "auth on + user session -> ALLOW");
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_ADMIN, true, false) == HTTP_AUTH_DECISION_ALLOW,
               "auth on + admin session -> ALLOW");
}

// ROUTE_TIER_USER routes are also subject to bootstrap_needed the same way
// as any other authenticated route -- unlike ROUTE_TIER_ADMIN, a USER-tier
// route does not itself close for bootstrap_needed (http_auth_check() only
// special-cases ADMIN and ADMIN_BOOTSTRAP for that flag), so a user session
// still reaches stop while bootstrap_needed is set; a session-less client
// still does not.
static void test_stop_route_session_rule_holds_during_bootstrap_needed(void) {
    TEST_SECTION("http_auth_check -- POST /api/profile_exec/stop's session requirement is "
                 "unaffected by bootstrap_needed (USER tier is not gated by that flag)");

    route_tier_t tier;
    TEST_CHECK(http_auth_lookup_tier("/api/profile_exec/stop", HTTP_POST, &tier),
               "POST /api/profile_exec/stop must have a row in route_tier_table.h");

    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_NONE, true, true) == HTTP_AUTH_DECISION_DENY_NO_SESSION,
               "auth on + no session (locked out) + bootstrap_needed -> DENY_NO_SESSION");
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_USER, true, true) == HTTP_AUTH_DECISION_ALLOW,
               "auth on + user session + bootstrap_needed -> ALLOW (bootstrap_needed only ever "
               "tightens ADMIN-tier routes, never USER)");
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_ADMIN, true, true) == HTTP_AUTH_DECISION_ALLOW,
               "auth on + admin session + bootstrap_needed -> ALLOW");
}

// UPDATE 2026-09-17: adversarial review (route_tier_table.h at origin/main
// 1179e2d3) found three more routes that abort operations actively driving
// heat but were left at ROUTE_TIER_ADMIN -- an expired session could not
// reach them, so /api/profile_exec/stop's always-reachable stop did not
// actually cover an in-progress current sweep, autotune, or danger-mode
// relay window. All three are now ROUTE_TIER_SAFETY_REDUCE in
// route_tier_table.h; the checks below are the generic, table-driven form of
// test_stop_route_all_five_combinations_allow() /
// test_stop_route_allowed_during_bootstrap_needed() above, parameterized by
// route so a future accidental re-tier of any of the four SAFETY_REDUCE
// routes is caught here.
static void check_route_is_safety_reduce_and_always_allowed(const char *uri, httpd_method_t method,
                                                              const char *label) {
    route_tier_t tier;
    char msg[256];

    TEST_CHECK(http_auth_lookup_tier(uri, method, &tier), label);
    snprintf(msg, sizeof(msg), "%s: tier is ROUTE_TIER_SAFETY_REDUCE, not ADMIN", label);
    TEST_CHECK(tier == ROUTE_TIER_SAFETY_REDUCE, msg);

    snprintf(msg, sizeof(msg), "%s: auth off -> ALLOW", label);
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_NONE, false, false) == HTTP_AUTH_DECISION_ALLOW, msg);

    snprintf(msg, sizeof(msg), "%s: auth on + no session (role NONE) -> ALLOW", label);
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_NONE, true, false) == HTTP_AUTH_DECISION_ALLOW, msg);

    snprintf(msg, sizeof(msg), "%s: auth on + user session -> ALLOW", label);
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_USER, true, false) == HTTP_AUTH_DECISION_ALLOW, msg);

    snprintf(msg, sizeof(msg), "%s: auth on + admin session -> ALLOW", label);
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_ADMIN, true, false) == HTTP_AUTH_DECISION_ALLOW, msg);

    snprintf(msg, sizeof(msg), "%s: auth on + no session + bootstrap_needed -> ALLOW", label);
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_NONE, true, true) == HTTP_AUTH_DECISION_ALLOW, msg);

    snprintf(msg, sizeof(msg), "%s: auth on + user session + bootstrap_needed -> ALLOW", label);
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_USER, true, true) == HTTP_AUTH_DECISION_ALLOW, msg);

    snprintf(msg, sizeof(msg), "%s: auth on + admin session + bootstrap_needed -> ALLOW", label);
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_ADMIN, true, true) == HTTP_AUTH_DECISION_ALLOW, msg);
}

static void test_sweep_abort_is_safety_reduce_and_always_allowed(void) {
    TEST_SECTION("route_tier_table.h -- POST /api/zones/current_sweep/abort is ROUTE_TIER_SAFETY_REDUCE "
                 "(it aborts the current-sweep task, which drives relays)");
    check_route_is_safety_reduce_and_always_allowed("/api/zones/current_sweep/abort", HTTP_POST,
                                                     "POST /api/zones/current_sweep/abort");
}

static void test_autotune_abort_is_safety_reduce_and_always_allowed(void) {
    TEST_SECTION("route_tier_table.h -- POST /api/autotune/abort is ROUTE_TIER_SAFETY_REDUCE "
                 "(it aborts a running autotune via force_relays_off())");
    check_route_is_safety_reduce_and_always_allowed("/api/autotune/abort", HTTP_POST,
                                                     "POST /api/autotune/abort");
}

static void test_danger_stop_is_safety_reduce_and_always_allowed(void) {
    TEST_SECTION("route_tier_table.h -- POST /api/diagnostics/danger/stop is ROUTE_TIER_SAFETY_REDUCE "
                 "(it drops relays and releases heat-enable via danger_mode_stop())");
    check_route_is_safety_reduce_and_always_allowed("/api/diagnostics/danger/stop", HTTP_POST,
                                                     "POST /api/diagnostics/danger/stop");
}

void run_test_web_auth_safety_interaction(void) {
    test_stop_route_tier_is_user();
    test_clear_trip_is_admin();
    test_stop_route_requires_session_when_auth_on();
    test_stop_route_session_rule_holds_during_bootstrap_needed();
    test_sweep_abort_is_safety_reduce_and_always_allowed();
    test_autotune_abort_is_safety_reduce_and_always_allowed();
    test_danger_stop_is_safety_reduce_and_always_allowed();
}
