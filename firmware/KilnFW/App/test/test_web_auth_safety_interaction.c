// Host tests for docs/WEB_AUTH_PLAN.md section 9 ("Safety interaction") --
// the part of that section decidable purely from route_tier_table.h and
// http_auth_check() (App/drivers/http/http_auth_enforce.c), with no
// dependency on the enforcement wiring's still-in-flight URI-level bypass
// (see the large comment block below before assuming more is covered here
// than actually is).
//
// SCOPE AND A KNOWN GAP, READ THIS FIRST:
//
// Section 9's own acceptance criterion is: "a host test drives the
// enforcement function with every combination of {auth off, auth on + no
// session, auth on + locked, auth on + user, auth on + admin} against
// /api/profile_exec/stop and asserts ALLOW in all five."
//
// As of this writing, http_auth_check() (the only "enforcement function"
// that exists) takes a route_tier_t, a role, and a web_enabled flag -- it
// has NO uri parameter at all, so it structurally cannot special-case
// /api/profile_exec/stop. Neither it nor http_auth_http.c's
// kiln_http_prehandler() contains any reference to
// "/api/profile_exec/stop" anywhere. route_tier_table.h classifies that
// route ROUTE_TIER_USER (nominally correct per plan section 1), which means
// today, with web auth on:
//   - auth off                        -> ALLOW  (covered below, test 3)
//   - auth on + admin session         -> ALLOW  (USER tier, ADMIN role satisfies it)
//   - auth on + user session          -> ALLOW  (USER tier, USER role satisfies it)
//   - auth on + no session            -> DENY_NO_SESSION  <-- violates section 9
//   - auth on + locked (no session)   -> DENY_NO_SESSION  <-- violates section 9
//
// Only 3 of the 5 required combinations currently ALLOW. The missing piece
// is a URI-aware safety bypass in the enforcement point itself (plan item
// 5), which this task's scope explicitly excludes (owned by the in-flight
// route-rewiring work) -- fixing http_auth_enforce.c/http_auth_http.c is
// NOT done here. This file tests only what current code can honestly
// support without asserting the two wrong outcomes as if they were correct;
// see docs/WEB_AUTH_PLAN.md section 9 for the note pointing at this gap.
//
// What IS tested here, all dependency-free and true today:
//   1. /api/profile_exec/stop's nominal tier is USER, matching plan section
//      1/9's own description ("Nominally USER") -- a regression pin so a
//      future accidental re-tier to ADMIN (which would only make the gap
//      above worse) is caught immediately.
//   2. /api/safety/clear_trip is ADMIN, matching plan section 9's explicit
//      "deliberately, ADMIN" -- clearing a trip is not itself a safety
//      action, it re-enables heat after one, so it correctly needs the
//      higher tier (this is the ONE deliberate exception to "everything
//      that reduces heat must stay reachable": clearing a trip does not
//      reduce heat, it is a precondition for re-arming).
//   3. The "auth off" combination of section 9's list, driven through the
//      REAL looked-up tier for /api/profile_exec/stop (not a bare
//      ROUTE_TIER_USER literal), proving that one of the five required
//      combinations already holds end-to-end against the real route table.
#include "test_common.h"

#include "../drivers/http/http_auth_enforce.h"

static void test_stop_route_tier_is_user(void) {
    TEST_SECTION("route_tier_table.h -- POST /api/profile_exec/stop is USER (plan section 9's own "
                 "description: 'Nominally USER, but a stop is never refused for lack of a session')");

    route_tier_t tier;
    TEST_CHECK(http_auth_lookup_tier("/api/profile_exec/stop", HTTP_POST, &tier) && tier == ROUTE_TIER_USER,
               "POST /api/profile_exec/stop is USER in route_tier_table.h -- a re-tier to ADMIN would "
               "only widen the gap documented at the top of this file (an admin-only nominal tier "
               "still has no URI-level bypass either), so this must never silently drift.");
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

static void test_stop_route_auth_off_allows(void) {
    TEST_SECTION("http_auth_check -- section 9's 'auth off' combination against the REAL looked-up "
                 "tier for POST /api/profile_exec/stop (1 of the 5 required combinations; see this "
                 "file's header comment for the other 4, which do not hold today)");

    route_tier_t tier;
    TEST_CHECK(http_auth_lookup_tier("/api/profile_exec/stop", HTTP_POST, &tier),
               "POST /api/profile_exec/stop must have a row in route_tier_table.h at all before "
               "the rest of this test means anything");
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_NONE, false) == HTTP_AUTH_DECISION_ALLOW,
               "auth off + /api/profile_exec/stop's real tier + no session -> ALLOW (section 9's "
               "first required combination)");
}

void run_test_web_auth_safety_interaction(void) {
    test_stop_route_tier_is_user();
    test_clear_trip_is_admin();
    test_stop_route_auth_off_allows();
}
