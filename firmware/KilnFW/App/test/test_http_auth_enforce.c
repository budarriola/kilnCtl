// Host tests for App/drivers/http/http_auth_enforce.c -- the fail-closed
// enforcement decision, docs/WEB_AUTH_PLAN.md section 5.
//
// Each TEST_CHECK below names the exact scenario section 5's task called
// for: denial with no session, denial with an expired session, denial on
// insufficient tier, denial on an unresolvable tier, the auth-disabled inert
// path, and OPEN-tier access with no credentials. No ESP-IDF dependency
// beyond the httpd_method_t/httpd_uri_t types route_tier_table.h names
// (App/test/stubs/esp_http_server.h supplies those).
#include <string.h>

#include "test_common.h"

#include "../drivers/http/http_auth_enforce.h"

static void test_lookup_tier_real_routes(void) {
    TEST_SECTION("http_auth_lookup_tier -- against the real route_tier_table.h, not a mirror");

    // These three assertions read the SAME table
    // tools/check_route_tier_coverage.ps1 enforces is complete, not a
    // second copy kept here -- if route_tier_table.h's classification for
    // any of these three ever changes, this test changes with it rather
    // than silently testing a stale expectation.
    route_tier_t tier;
    TEST_CHECK(http_auth_lookup_tier("/api/status", HTTP_GET, &tier) && tier == ROUTE_TIER_OPEN,
               "GET /api/status is OPEN in route_tier_table.h");
    TEST_CHECK(http_auth_lookup_tier("/api/profile_exec/start", HTTP_POST, &tier) && tier == ROUTE_TIER_USER,
               "POST /api/profile_exec/start is USER in route_tier_table.h");
    TEST_CHECK(http_auth_lookup_tier("/api/zones", HTTP_GET, &tier) && tier == ROUTE_TIER_ADMIN,
               "GET /api/zones is ADMIN in route_tier_table.h (a config dump, not telemetry)");

    TEST_CHECK(!http_auth_lookup_tier("/api/does_not_exist", HTTP_GET, &tier),
               "a uri with no row returns false, not a guessed tier");

    // Same uri, different method, different tier -- proves the lookup keys
    // on (uri, method) together, not uri alone (plan section 1's
    // /api/unit_pref/zones/etc. examples of a uri carrying different tiers
    // per method).
    TEST_CHECK(http_auth_lookup_tier("/api/zones", HTTP_POST, &tier) && tier == ROUTE_TIER_ADMIN,
               "POST /api/zones is also ADMIN (both methods happen to agree here, but looked up "
               "independently)");
}

static void test_effective_tier_fail_closed_default(void) {
    TEST_SECTION("http_auth_effective_tier -- unresolvable tier defaults to ADMIN, not OPEN");

    // SCENARIO: denial on an unresolvable tier. A route with no row in
    // route_tier_table.h must not default to the LEAST restrictive tier by
    // omission -- it must default to the MOST restrictive one.
    route_tier_t tier = http_auth_effective_tier("/api/totally_unclassified_route", HTTP_POST);
    TEST_CHECK(tier == ROUTE_TIER_ADMIN, "an unresolvable route's effective tier is ADMIN, never OPEN or USER");
}

static void test_auth_disabled_inert_path(void) {
    TEST_SECTION("http_auth_check -- auth disabled collapses every tier to full access");

    // SCENARIO: the auth-disabled inert path (plan section 11: "a board
    // with auth off is exactly as open as the board is today"). Every tier,
    // with no session at all, must ALLOW.
    TEST_CHECK(http_auth_check(ROUTE_TIER_OPEN, HTTP_AUTH_ROLE_NONE, false) == HTTP_AUTH_DECISION_ALLOW,
               "auth off + OPEN + no session -> ALLOW");
    TEST_CHECK(http_auth_check(ROUTE_TIER_USER, HTTP_AUTH_ROLE_NONE, false) == HTTP_AUTH_DECISION_ALLOW,
               "auth off + USER + no session -> ALLOW (today's behaviour, unchanged)");
    TEST_CHECK(http_auth_check(ROUTE_TIER_ADMIN, HTTP_AUTH_ROLE_NONE, false) == HTTP_AUTH_DECISION_ALLOW,
               "auth off + ADMIN + no session -> ALLOW (today's behaviour, unchanged)");
    // Also true with a role present -- auth-off is not merely "a session
    // isn't required", it is "role is not even consulted".
    TEST_CHECK(http_auth_check(ROUTE_TIER_ADMIN, HTTP_AUTH_ROLE_USER, false) == HTTP_AUTH_DECISION_ALLOW,
               "auth off + ADMIN + a mere USER role -> still ALLOW");
}

static void test_open_tier_no_credentials(void) {
    TEST_SECTION("http_auth_check -- OPEN tier never requires a credential, even with auth on");

    // SCENARIO: OPEN-tier access with no credentials. This is the Dashboard
    // guarantee: it stays viewable with auth ON and zero session.
    TEST_CHECK(http_auth_check(ROUTE_TIER_OPEN, HTTP_AUTH_ROLE_NONE, true) == HTTP_AUTH_DECISION_ALLOW,
               "auth ON + OPEN + no session -> ALLOW");
    // And it does not matter what role (if any) is present either.
    TEST_CHECK(http_auth_check(ROUTE_TIER_OPEN, HTTP_AUTH_ROLE_USER, true) == HTTP_AUTH_DECISION_ALLOW,
               "auth ON + OPEN + USER session -> still ALLOW");
}

static void test_no_session_denied(void) {
    TEST_SECTION("http_auth_check -- no session on a gated route is denied, not allowed");

    // SCENARIO: denial with no session.
    TEST_CHECK(http_auth_check(ROUTE_TIER_USER, HTTP_AUTH_ROLE_NONE, true) == HTTP_AUTH_DECISION_DENY_NO_SESSION,
               "auth ON + USER tier + no session -> DENY_NO_SESSION");
    TEST_CHECK(http_auth_check(ROUTE_TIER_ADMIN, HTTP_AUTH_ROLE_NONE, true) == HTTP_AUTH_DECISION_DENY_NO_SESSION,
               "auth ON + ADMIN tier + no session -> DENY_NO_SESSION");
}

static void test_expired_session_denied(void) {
    TEST_SECTION("http_auth_check -- an expired session is denied identically to no session");

    // SCENARIO: denial with an expired session. Per http_session_iface.h's
    // contract, the session resolver collapses "no session", "expired", and
    // "unresolvable" all into HTTP_AUTH_ROLE_NONE before this function ever
    // sees them -- so from this function's point of view an expired session
    // IS the no-session case. This test exists to nail that collapse point
    // down explicitly (naming it "expired" in the test, not just "none"),
    // so a future refactor that tried to give "expired" its own role value
    // without updating this function would have to break this assertion to
    // do it.
    http_auth_role_t role_after_expiry = HTTP_AUTH_ROLE_NONE; // what a real resolver must report
    TEST_CHECK(http_auth_check(ROUTE_TIER_ADMIN, role_after_expiry, true) == HTTP_AUTH_DECISION_DENY_NO_SESSION,
               "an expired session (modelled here as the resolver's required HTTP_AUTH_ROLE_NONE "
               "output) denies exactly like no session at all -- 401, not 403");
}

static void test_insufficient_tier_denied(void) {
    TEST_SECTION("http_auth_check -- a USER session on an ADMIN route is denied, not allowed");

    // SCENARIO: denial on insufficient tier.
    TEST_CHECK(http_auth_check(ROUTE_TIER_ADMIN, HTTP_AUTH_ROLE_USER, true) == HTTP_AUTH_DECISION_DENY_INSUFFICIENT,
               "auth ON + ADMIN tier + USER role -> DENY_INSUFFICIENT (403, not 401 -- the session "
               "IS valid, it just isn't the right role)");

    // The reverse must not also be denied: ADMIN role satisfies a USER-tier
    // route (an administrator is not locked OUT of USER-level routes).
    TEST_CHECK(http_auth_check(ROUTE_TIER_USER, HTTP_AUTH_ROLE_ADMIN, true) == HTTP_AUTH_DECISION_ALLOW,
               "ADMIN role on a USER-tier route -> ALLOW (administrator is a superset)");
    TEST_CHECK(http_auth_check(ROUTE_TIER_ADMIN, HTTP_AUTH_ROLE_ADMIN, true) == HTTP_AUTH_DECISION_ALLOW,
               "ADMIN role on an ADMIN-tier route -> ALLOW");
    TEST_CHECK(http_auth_check(ROUTE_TIER_USER, HTTP_AUTH_ROLE_USER, true) == HTTP_AUTH_DECISION_ALLOW,
               "USER role on a USER-tier route -> ALLOW");
}

static void test_safety_reduce_always_allowed(void) {
    TEST_SECTION("http_auth_check -- ROUTE_TIER_SAFETY_REDUCE is allowed regardless of role or lockout");

    // SCENARIO (plan section 9): POST /api/profile_exec/stop can only ever
    // reduce heat/risk, so it must be reachable with no session at all, and
    // with a role the resolver would otherwise report for a locked-out or
    // never-logged-in client (HTTP_AUTH_ROLE_NONE) -- exactly the same value
    // an expired or unresolvable session collapses to.
    route_tier_t tier;
    TEST_CHECK(http_auth_lookup_tier("/api/profile_exec/stop", HTTP_POST, &tier) &&
                   tier == ROUTE_TIER_SAFETY_REDUCE,
               "POST /api/profile_exec/stop is ROUTE_TIER_SAFETY_REDUCE in route_tier_table.h, not USER");

    TEST_CHECK(http_auth_check(ROUTE_TIER_SAFETY_REDUCE, HTTP_AUTH_ROLE_NONE, true) == HTTP_AUTH_DECISION_ALLOW,
               "auth ON + SAFETY_REDUCE + no session (or locked out) -> ALLOW, never DENY_NO_SESSION");
    TEST_CHECK(http_auth_check(ROUTE_TIER_SAFETY_REDUCE, HTTP_AUTH_ROLE_USER, true) == HTTP_AUTH_DECISION_ALLOW,
               "auth ON + SAFETY_REDUCE + USER session -> ALLOW");
    TEST_CHECK(http_auth_check(ROUTE_TIER_SAFETY_REDUCE, HTTP_AUTH_ROLE_ADMIN, true) == HTTP_AUTH_DECISION_ALLOW,
               "auth ON + SAFETY_REDUCE + ADMIN session -> ALLOW");
    // And with auth off it is unaffected too -- this tier is not a special
    // case of the auth-off collapse, it is unconditional.
    TEST_CHECK(http_auth_check(ROUTE_TIER_SAFETY_REDUCE, HTTP_AUTH_ROLE_NONE, false) == HTTP_AUTH_DECISION_ALLOW,
               "auth OFF + SAFETY_REDUCE + no session -> ALLOW");
}

static void test_unresolvable_tier_end_to_end(void) {
    TEST_SECTION("http_auth_check -- an unresolvable tier ends up denied, not silently open");

    // Chains http_auth_effective_tier()'s fail-closed default straight into
    // http_auth_check(), the way kiln_http_register()'s pre-handler
    // actually will: an unclassified route, hit with no session, must be
    // denied (401) -- and hit with a mere USER session, must still be
    // denied (403), because the default is ADMIN, not USER.
    route_tier_t tier = http_auth_effective_tier("/api/some_new_route_nobody_classified", HTTP_POST);
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_NONE, true) == HTTP_AUTH_DECISION_DENY_NO_SESSION,
               "unresolvable route, no session -> DENY_NO_SESSION");
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_USER, true) == HTTP_AUTH_DECISION_DENY_INSUFFICIENT,
               "unresolvable route, USER session -> DENY_INSUFFICIENT (default is ADMIN, not USER)");
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_ADMIN, true) == HTTP_AUTH_DECISION_ALLOW,
               "unresolvable route, ADMIN session -> ALLOW (ADMIN satisfies the fail-closed default)");
}

void run_test_http_auth_enforce(void) {
    test_lookup_tier_real_routes();
    test_effective_tier_fail_closed_default();
    test_auth_disabled_inert_path();
    test_open_tier_no_credentials();
    test_no_session_denied();
    test_expired_session_denied();
    test_insufficient_tier_denied();
    test_safety_reduce_always_allowed();
    test_unresolvable_tier_end_to_end();
}
