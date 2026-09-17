// Host tests for docs/WEB_AUTH_PLAN.md section 9 ("Safety interaction").
//
// Section 9's acceptance criterion: "a host test drives the enforcement
// function with every combination of {auth off, auth on + no session, auth
// on + locked, auth on + user, auth on + admin} against
// /api/profile_exec/stop and asserts ALLOW in all five."
//
// UPDATE 2026-09-17: the gap this file originally documented (only 3 of 5
// combinations allowed, because http_auth_check() had no URI awareness and
// route_tier_table.h tiered the stop route ROUTE_TIER_USER) is now closed.
// The fix is table-driven, not a URI string match: route_tier_table.h gains
// ROUTE_TIER_SAFETY_REDUCE, assigned to POST /api/profile_exec/stop in place
// of ROUTE_TIER_USER, and http_auth_check() (http_auth_enforce.c) treats
// that tier as an unconditional ALLOW -- checked in the same position as
// ROUTE_TIER_OPEN, before role/session are even inspected. See
// route_tier_table.h's and http_auth_enforce.h's own comments on
// ROUTE_TIER_SAFETY_REDUCE for why this stays a single source of truth
// (check_route_tier_coverage.ps1's regex is generic over any ROUTE_TIER_*
// value, so this needed no change there).
//
// All 5 of section 9's required combinations are exercised below, driven
// through the REAL looked-up tier for /api/profile_exec/stop (never a bare
// literal), so a future accidental re-tier of the route is caught here
// rather than only in test_http_auth_enforce.c's own
// test_safety_reduce_always_allowed().
#include "test_common.h"

#include "../drivers/http/http_auth_enforce.h"

static void test_stop_route_tier_is_safety_reduce(void) {
    TEST_SECTION("route_tier_table.h -- POST /api/profile_exec/stop is ROUTE_TIER_SAFETY_REDUCE, "
                 "not USER (plan section 9: a stop is never refused for lack of a session, "
                 "including a locked-out or session-less client)");

    route_tier_t tier;
    TEST_CHECK(http_auth_lookup_tier("/api/profile_exec/stop", HTTP_POST, &tier) &&
                   tier == ROUTE_TIER_SAFETY_REDUCE,
               "POST /api/profile_exec/stop is ROUTE_TIER_SAFETY_REDUCE in route_tier_table.h -- a "
               "re-tier back to USER (or to ADMIN) would silently reopen the DENY_NO_SESSION gap "
               "this section exists to close, so this must never drift.");
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

// Drives http_auth_check() with the REAL looked-up tier for the stop route
// (never ROUTE_TIER_SAFETY_REDUCE as a bare literal) through all 5 of
// section 9's required combinations. "locked" and "no session" both resolve
// to HTTP_AUTH_ROLE_NONE at the enforcement point -- see http_auth_role_t's
// own doc comment in http_auth_enforce.h for why a locked-out session and an
// absent one are deliberately indistinguishable here.
static void test_stop_route_all_five_combinations_allow(void) {
    TEST_SECTION("http_auth_check -- POST /api/profile_exec/stop's real looked-up tier ALLOWs all "
                 "5 of section 9's required combinations");

    route_tier_t tier;
    TEST_CHECK(http_auth_lookup_tier("/api/profile_exec/stop", HTTP_POST, &tier),
               "POST /api/profile_exec/stop must have a row in route_tier_table.h at all before "
               "the rest of this test means anything");

    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_NONE, false) == HTTP_AUTH_DECISION_ALLOW,
               "1/5: auth off -> ALLOW");
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_NONE, true) == HTTP_AUTH_DECISION_ALLOW,
               "2/5: auth on + no session -> ALLOW (was DENY_NO_SESSION before this fix)");
    // "locked" carries no distinct role value of its own -- a locked-out
    // client is, from the enforcement point's perspective, a client with no
    // resolvable session, i.e. HTTP_AUTH_ROLE_NONE. Asserted again here
    // under its own name so a future reviewer sees section 9's "locked"
    // combination named explicitly, not merely inferred from "no session".
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_NONE, true) == HTTP_AUTH_DECISION_ALLOW,
               "3/5: auth on + locked (no resolvable session) -> ALLOW (was DENY_NO_SESSION before "
               "this fix)");
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_USER, true) == HTTP_AUTH_DECISION_ALLOW,
               "4/5: auth on + user session -> ALLOW");
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_ADMIN, true) == HTTP_AUTH_DECISION_ALLOW,
               "5/5: auth on + admin session -> ALLOW");
}

void run_test_web_auth_safety_interaction(void) {
    test_stop_route_tier_is_safety_reduce();
    test_clear_trip_is_admin();
    test_stop_route_all_five_combinations_allow();
}
