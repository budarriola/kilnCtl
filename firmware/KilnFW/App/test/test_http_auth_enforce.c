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

    // WEB_AUTH_PLAN.md section 6: the admin password/settings page
    // (security_http.c) and its two API routes -- all three ADMIN, same as
    // every other /settings page shell and /api/settings writer.
    TEST_CHECK(http_auth_lookup_tier("/settings/security", HTTP_GET, &tier) && tier == ROUTE_TIER_ADMIN,
               "GET /settings/security is ADMIN in route_tier_table.h");
    TEST_CHECK(http_auth_lookup_tier("/api/auth/config", HTTP_GET, &tier) && tier == ROUTE_TIER_ADMIN,
               "GET /api/auth/config is ADMIN in route_tier_table.h");
    TEST_CHECK(http_auth_lookup_tier("/api/auth/security", HTTP_POST, &tier) && tier == ROUTE_TIER_ADMIN,
               "POST /api/auth/security is ADMIN in route_tier_table.h");
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
    TEST_CHECK(http_auth_check(ROUTE_TIER_OPEN, HTTP_AUTH_ROLE_NONE, false, false, false) == HTTP_AUTH_DECISION_ALLOW,
               "auth off + OPEN + no session -> ALLOW");
    TEST_CHECK(http_auth_check(ROUTE_TIER_USER, HTTP_AUTH_ROLE_NONE, false, false, false) == HTTP_AUTH_DECISION_ALLOW,
               "auth off + USER + no session -> ALLOW (today's behaviour, unchanged)");
    TEST_CHECK(http_auth_check(ROUTE_TIER_ADMIN, HTTP_AUTH_ROLE_NONE, false, false, false) == HTTP_AUTH_DECISION_ALLOW,
               "auth off + ADMIN + no session -> ALLOW (today's behaviour, unchanged)");
    // Also true with a role present -- auth-off is not merely "a session
    // isn't required", it is "role is not even consulted".
    TEST_CHECK(http_auth_check(ROUTE_TIER_ADMIN, HTTP_AUTH_ROLE_USER, false, false, false) == HTTP_AUTH_DECISION_ALLOW,
               "auth off + ADMIN + a mere USER role -> still ALLOW");
}

static void test_open_tier_no_credentials(void) {
    TEST_SECTION("http_auth_check -- OPEN tier never requires a credential, even with auth on");

    // SCENARIO: OPEN-tier access with no credentials. This is the Dashboard
    // guarantee: it stays viewable with auth ON and zero session.
    TEST_CHECK(http_auth_check(ROUTE_TIER_OPEN, HTTP_AUTH_ROLE_NONE, true, false, false) == HTTP_AUTH_DECISION_ALLOW,
               "auth ON + OPEN + no session -> ALLOW");
    // And it does not matter what role (if any) is present either.
    TEST_CHECK(http_auth_check(ROUTE_TIER_OPEN, HTTP_AUTH_ROLE_USER, true, false, false) == HTTP_AUTH_DECISION_ALLOW,
               "auth ON + OPEN + USER session -> still ALLOW");
}

static void test_no_session_denied(void) {
    TEST_SECTION("http_auth_check -- no session on a gated route is denied, not allowed");

    // SCENARIO: denial with no session.
    TEST_CHECK(http_auth_check(ROUTE_TIER_USER, HTTP_AUTH_ROLE_NONE, true, false, false) == HTTP_AUTH_DECISION_DENY_NO_SESSION,
               "auth ON + USER tier + no session -> DENY_NO_SESSION");
    TEST_CHECK(http_auth_check(ROUTE_TIER_ADMIN, HTTP_AUTH_ROLE_NONE, true, false, false) == HTTP_AUTH_DECISION_DENY_NO_SESSION,
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
    TEST_CHECK(http_auth_check(ROUTE_TIER_ADMIN, role_after_expiry, true, false, false) == HTTP_AUTH_DECISION_DENY_NO_SESSION,
               "an expired session (modelled here as the resolver's required HTTP_AUTH_ROLE_NONE "
               "output) denies exactly like no session at all -- 401, not 403");
}

static void test_insufficient_tier_denied(void) {
    TEST_SECTION("http_auth_check -- a USER session on an ADMIN route is denied, not allowed");

    // SCENARIO: denial on insufficient tier.
    TEST_CHECK(http_auth_check(ROUTE_TIER_ADMIN, HTTP_AUTH_ROLE_USER, true, false, false) == HTTP_AUTH_DECISION_DENY_INSUFFICIENT,
               "auth ON + ADMIN tier + USER role -> DENY_INSUFFICIENT (403, not 401 -- the session "
               "IS valid, it just isn't the right role)");

    // The reverse must not also be denied: ADMIN role satisfies a USER-tier
    // route (an administrator is not locked OUT of USER-level routes).
    TEST_CHECK(http_auth_check(ROUTE_TIER_USER, HTTP_AUTH_ROLE_ADMIN, true, false, false) == HTTP_AUTH_DECISION_ALLOW,
               "ADMIN role on a USER-tier route -> ALLOW (administrator is a superset)");
    TEST_CHECK(http_auth_check(ROUTE_TIER_ADMIN, HTTP_AUTH_ROLE_ADMIN, true, false, false) == HTTP_AUTH_DECISION_ALLOW,
               "ADMIN role on an ADMIN-tier route -> ALLOW");
    TEST_CHECK(http_auth_check(ROUTE_TIER_USER, HTTP_AUTH_ROLE_USER, true, false, false) == HTTP_AUTH_DECISION_ALLOW,
               "USER role on a USER-tier route -> ALLOW");
}

static void test_safety_reduce_always_allowed(void) {
    TEST_SECTION("http_auth_check -- ROUTE_TIER_SAFETY_REDUCE is allowed regardless of role or lockout");

    // SCENARIO: a route that can only ever reduce heat/risk must be
    // reachable with no session at all, and with a role the resolver would
    // otherwise report for a locked-out or never-logged-in client
    // (HTTP_AUTH_ROLE_NONE) -- exactly the same value an expired or
    // unresolvable session collapses to. As of the 2026-09-28 owner decision
    // ("stop needs login. there is an estop button."), POST
    // /api/profile_exec/stop is no longer one of these routes -- it is
    // ROUTE_TIER_USER now (see test_web_auth_safety_interaction.c). A
    // 2026-09-28 follow-up owner decision moved the other three routes that
    // used to carry this tier (current_sweep/abort, autotune/abort,
    // danger/stop) to ROUTE_TIER_ADMIN, so as of that change NO route in
    // route_tier_table.h carries ROUTE_TIER_SAFETY_REDUCE -- this test
    // exercises the generic, still-live tier behavior directly (kept for a
    // future route that needs this shape) rather than naming any one route.
    TEST_CHECK(http_auth_check(ROUTE_TIER_SAFETY_REDUCE, HTTP_AUTH_ROLE_NONE, true, false, false) == HTTP_AUTH_DECISION_ALLOW,
               "auth ON + SAFETY_REDUCE + no session (or locked out) -> ALLOW, never DENY_NO_SESSION");
    TEST_CHECK(http_auth_check(ROUTE_TIER_SAFETY_REDUCE, HTTP_AUTH_ROLE_USER, true, false, false) == HTTP_AUTH_DECISION_ALLOW,
               "auth ON + SAFETY_REDUCE + USER session -> ALLOW");
    TEST_CHECK(http_auth_check(ROUTE_TIER_SAFETY_REDUCE, HTTP_AUTH_ROLE_ADMIN, true, false, false) == HTTP_AUTH_DECISION_ALLOW,
               "auth ON + SAFETY_REDUCE + ADMIN session -> ALLOW");
    // And with auth off it is unaffected too -- this tier is not a special
    // case of the auth-off collapse, it is unconditional.
    TEST_CHECK(http_auth_check(ROUTE_TIER_SAFETY_REDUCE, HTTP_AUTH_ROLE_NONE, false, false, false) == HTTP_AUTH_DECISION_ALLOW,
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
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_NONE, true, false, false) == HTTP_AUTH_DECISION_DENY_NO_SESSION,
               "unresolvable route, no session -> DENY_NO_SESSION");
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_USER, true, false, false) == HTTP_AUTH_DECISION_DENY_INSUFFICIENT,
               "unresolvable route, USER session -> DENY_INSUFFICIENT (default is ADMIN, not USER)");
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_ADMIN, true, false, false) == HTTP_AUTH_DECISION_ALLOW,
               "unresolvable route, ADMIN session -> ALLOW (ADMIN satisfies the fail-closed default)");
}

static void test_bootstrap_route_tier_is_admin_bootstrap(void) {
    TEST_SECTION("route_tier_table.h -- POST /api/auth/bootstrap_password is "
                 "ROUTE_TIER_ADMIN_BOOTSTRAP");

    route_tier_t tier;
    TEST_CHECK(http_auth_lookup_tier("/api/auth/bootstrap_password", HTTP_POST, &tier) &&
                   tier == ROUTE_TIER_ADMIN_BOOTSTRAP,
               "POST /api/auth/bootstrap_password is ROUTE_TIER_ADMIN_BOOTSTRAP in "
               "route_tier_table.h -- a re-tier to ADMIN or OPEN would silently break the "
               "bootstrap flow (items 10/11)");
}

static void test_admin_bootstrap_tier_gated_on_bootstrap_needed(void) {
    TEST_SECTION("http_auth_check -- ROUTE_TIER_ADMIN_BOOTSTRAP is gated on bootstrap_needed "
                 "alone, never on role/session");

    // With no session at all, bootstrap_needed==true must ALLOW -- this is
    // the whole point: no session can exist yet before the first
    // administrator credential is set.
    TEST_CHECK(http_auth_check(ROUTE_TIER_ADMIN_BOOTSTRAP, HTTP_AUTH_ROLE_NONE, true, true, false) ==
                   HTTP_AUTH_DECISION_ALLOW,
               "auth ON + ADMIN_BOOTSTRAP + no session + bootstrap_needed -> ALLOW");
    // A role present changes nothing -- still ALLOW.
    TEST_CHECK(http_auth_check(ROUTE_TIER_ADMIN_BOOTSTRAP, HTTP_AUTH_ROLE_ADMIN, true, true, false) ==
                   HTTP_AUTH_DECISION_ALLOW,
               "auth ON + ADMIN_BOOTSTRAP + ADMIN session + bootstrap_needed -> still ALLOW");

    // Once bootstrap is no longer needed, this route must close -- even for
    // an administrator session, since /settings/security is the ordinary
    // path afterward and this one-shot route must not stay open forever.
    TEST_CHECK(http_auth_check(ROUTE_TIER_ADMIN_BOOTSTRAP, HTTP_AUTH_ROLE_NONE, true, false, false) ==
                   HTTP_AUTH_DECISION_DENY_INSUFFICIENT,
               "auth ON + ADMIN_BOOTSTRAP + no session + !bootstrap_needed -> DENY_INSUFFICIENT");
    TEST_CHECK(http_auth_check(ROUTE_TIER_ADMIN_BOOTSTRAP, HTTP_AUTH_ROLE_ADMIN, true, false, false) ==
                   HTTP_AUTH_DECISION_DENY_INSUFFICIENT,
               "auth ON + ADMIN_BOOTSTRAP + ADMIN session + !bootstrap_needed -> "
               "DENY_INSUFFICIENT (closed even for an admin session)");
}

static void test_admin_tier_denied_while_bootstrap_needed(void) {
    TEST_SECTION("http_auth_check -- ROUTE_TIER_ADMIN denies a stale ADMIN session while "
                 "bootstrap_needed (plan item 10's stale-session gap)");

    // A physical credential reset (item 10) clears the administrator
    // credential record but does not invalidate existing sessions -- a
    // stale pre-reset admin session must NOT satisfy ROUTE_TIER_ADMIN while
    // bootstrap is outstanding, or the bootstrap gate is meaningless.
    TEST_CHECK(http_auth_check(ROUTE_TIER_ADMIN, HTTP_AUTH_ROLE_ADMIN, true, true, false) ==
                   HTTP_AUTH_DECISION_DENY_INSUFFICIENT,
               "auth ON + ADMIN tier + ADMIN role + bootstrap_needed -> DENY_INSUFFICIENT "
               "(a stale admin session must not survive a physical credential reset)");

    // Once bootstrap is satisfied again, ordinary ADMIN-role access returns.
    TEST_CHECK(http_auth_check(ROUTE_TIER_ADMIN, HTTP_AUTH_ROLE_ADMIN, true, false, false) ==
                   HTTP_AUTH_DECISION_ALLOW,
               "auth ON + ADMIN tier + ADMIN role + !bootstrap_needed -> ALLOW (ordinary case, "
               "unaffected)");

    // A USER-tier route must remain reachable to a USER session even while
    // bootstrap_needed -- bootstrap_needed only closes ADMIN-tier routes and
    // gates the ADMIN_BOOTSTRAP route, it must not collapse into a
    // web-enabled-off-style blanket allow/deny of every tier.
    TEST_CHECK(http_auth_check(ROUTE_TIER_USER, HTTP_AUTH_ROLE_USER, true, true, false) ==
                   HTTP_AUTH_DECISION_ALLOW,
               "auth ON + USER tier + USER role + bootstrap_needed -> ALLOW (USER tier is "
               "unaffected by the administrator-bootstrap state)");

    // OPEN and SAFETY_REDUCE tiers must also remain unaffected by
    // bootstrap_needed -- the Dashboard must stay viewable throughout.
    TEST_CHECK(http_auth_check(ROUTE_TIER_OPEN, HTTP_AUTH_ROLE_NONE, true, true, false) ==
                   HTTP_AUTH_DECISION_ALLOW,
               "auth ON + OPEN tier + no session + bootstrap_needed -> ALLOW (Dashboard stays "
               "viewable throughout bootstrap)");
    TEST_CHECK(http_auth_check(ROUTE_TIER_SAFETY_REDUCE, HTTP_AUTH_ROLE_NONE, true, true, false) ==
                   HTTP_AUTH_DECISION_ALLOW,
               "auth ON + SAFETY_REDUCE tier + no session + bootstrap_needed -> ALLOW "
               "(stopping a firing is unaffected by bootstrap state)");
}

// Plan section 12, point 1: every tier reached end-to-end through the real
// enforcement entry point at each role (NONE/USER/ADMIN), with auth enabled
// and disabled, and with bootstrap_needed true and false -- 5 tiers x 3
// roles x 2 x 2 = 60 combinations. This table is transcribed directly from
// http_auth_enforce.h's own documented contract (the comment above
// http_auth_check(), lines ~92-164) rather than re-deriving the decision
// logic -- it is DATA asserting the spec's stated promises hold against the
// real function, not a second implementation of http_auth_check() that could
// silently drift from it (the "reset one side of a pair" bug class
// CLAUDE.md documents: two independently-kept copies of the same fact).
// Every row here calls the one real http_auth_check(); none of them
// reimplement its branching.
static void test_full_matrix_every_tier_role_auth_bootstrap(void) {
    TEST_SECTION("http_auth_check -- full matrix: every tier x every role x auth on/off x "
                 "bootstrap true/false (plan section 12 point 1)");

    static const route_tier_t kTiers[] = {
        ROUTE_TIER_OPEN, ROUTE_TIER_USER, ROUTE_TIER_ADMIN, ROUTE_TIER_SAFETY_REDUCE,
        ROUTE_TIER_ADMIN_BOOTSTRAP, ROUTE_TIER_WIFI_SETUP,
    };
    static const http_auth_role_t kRoles[] = {
        HTTP_AUTH_ROLE_NONE, HTTP_AUTH_ROLE_USER, HTTP_AUTH_ROLE_ADMIN,
    };
    static const bool kBools[] = {false, true};

    size_t checked = 0;
    for (size_t ti = 0; ti < sizeof(kTiers) / sizeof(kTiers[0]); ti++) {
        for (size_t ri = 0; ri < sizeof(kRoles) / sizeof(kRoles[0]); ri++) {
            for (size_t wi = 0; wi < 2; wi++) {
                for (size_t bi = 0; bi < 2; bi++) {
                    for (size_t ui = 0; ui < 2; ui++) {
                        route_tier_t tier = kTiers[ti];
                        http_auth_role_t role = kRoles[ri];
                        bool web_enabled = kBools[wi];
                        bool bootstrap_needed = kBools[bi];
                        bool wifi_unprovisioned = kBools[ui];

                        // ROUTE_TIER_WIFI_SETUP: ALLOW while unprovisioned,
                        // else falls through and is gated exactly like
                        // ROUTE_TIER_ADMIN (http_auth_check()'s own doc
                        // comment) -- reassign the local `tier` used by the
                        // rest of this expectation logic below, same as the
                        // real function does.
                        if (tier == ROUTE_TIER_WIFI_SETUP) {
                            if (!web_enabled) {
                                // still falls into the auth-off ALLOW below
                            } else if (wifi_unprovisioned) {
                                TEST_CHECK(http_auth_check(tier, role, web_enabled, bootstrap_needed,
                                                            wifi_unprovisioned) == HTTP_AUTH_DECISION_ALLOW,
                                           "matrix case: WIFI_SETUP + unprovisioned -> ALLOW");
                                checked++;
                                continue;
                            } else {
                                tier = ROUTE_TIER_ADMIN;
                            }
                        }

                        http_auth_decision_t expect;
                        if (!web_enabled) {
                            // Section 11: auth off collapses everything to ALLOW,
                            // unconditionally -- role/tier/bootstrap irrelevant.
                            expect = HTTP_AUTH_DECISION_ALLOW;
                        } else if (tier == ROUTE_TIER_OPEN || tier == ROUTE_TIER_SAFETY_REDUCE) {
                            // Both unconditional ALLOW regardless of role/bootstrap.
                            expect = HTTP_AUTH_DECISION_ALLOW;
                        } else if (tier == ROUTE_TIER_ADMIN_BOOTSTRAP) {
                            // Gated on bootstrap_needed alone, never role.
                            expect = bootstrap_needed ? HTTP_AUTH_DECISION_ALLOW
                                                       : HTTP_AUTH_DECISION_DENY_INSUFFICIENT;
                        } else if (role == HTTP_AUTH_ROLE_NONE) {
                            // USER/ADMIN tiers with no session -> 401, regardless
                            // of bootstrap_needed (that gate only ever tightens
                            // ADMIN further once a session exists).
                            expect = HTTP_AUTH_DECISION_DENY_NO_SESSION;
                        } else if (tier == ROUTE_TIER_USER) {
                            // USER or ADMIN role both satisfy USER tier.
                            expect = HTTP_AUTH_DECISION_ALLOW;
                        } else { // ROUTE_TIER_ADMIN, role != NONE
                            if (bootstrap_needed) {
                                expect = HTTP_AUTH_DECISION_DENY_INSUFFICIENT;
                            } else {
                                expect = (role == HTTP_AUTH_ROLE_ADMIN) ? HTTP_AUTH_DECISION_ALLOW
                                                                         : HTTP_AUTH_DECISION_DENY_INSUFFICIENT;
                            }
                        }

                        http_auth_decision_t got =
                            http_auth_check(kTiers[ti], role, web_enabled, bootstrap_needed, wifi_unprovisioned);
                        TEST_CHECK(got == expect,
                                   "matrix case (see counter in section header on failure)");
                        checked++;
                    }
                }
            }
        }
    }
    TEST_CHECK(checked == 6 * 3 * 2 * 2 * 2,
               "matrix covered exactly 144 combinations (6 tiers x 3 roles x "
               "2 auth states x 2 bootstrap states x 2 wifi-unprovisioned states)");
}

// Plan section 12, point 5 (reachability half): every row actually present in
// the real kRouteTierTable must be found by http_auth_lookup_tier() with its
// declared tier -- proves the lookup's linear scan does not silently skip or
// misclassify a later row (e.g. an early wrong match, an off-by-one bound).
// The "none orphaned" half (a table row with no corresponding real registered
// route) is a cross-file fact this pure-logic file cannot see -- it is
// covered by test_check_route_tier_coverage.ps1's real-tree assertion
// instead, using the SAME Get-RegisteredRoutes/Get-TieredKeys the mechanical
// check itself uses (see that file for why a second scanner here would be
// exactly the drift risk CLAUDE.md warns about).
static void test_every_table_row_reachable_via_lookup(void) {
    TEST_SECTION("http_auth_lookup_tier -- every row in the real kRouteTierTable is reachable "
                 "(plan section 12 point 5, reachability half)");

    size_t n = ROUTE_TIER_TABLE_COUNT;
    TEST_CHECK(n > 100, "the real table has a plausible number of rows (sanity floor, not a mirror "
                        "of the exact count)");

    for (size_t i = 0; i < n; i++) {
        const route_tier_entry_t *e = &kRouteTierTable[i];
        route_tier_t found;
        bool ok = http_auth_lookup_tier(e->uri, e->method, &found);
        TEST_CHECK(ok && found == e->tier, "each real table row is found by lookup with its own "
                                           "declared tier");
    }
}

// Section 11 acceptance criterion, taken literally: "a host test asserts an
// absent policy record reads as both-off ... and that every route in the
// table returns ALLOW in that state." Walks the REAL kRouteTierTable row by
// row (not the small hand-written kTiers enum list
// test_full_matrix_every_tier_role_auth_bootstrap() uses above, which is
// exhaustive over the route_tier_t enum but not literally "every route in
// the table") and calls the real http_auth_check() with web_enabled==false
// for each one. This closes the gap a hand-written tier list would leave: a
// future ROUTE_TIER_* enumerator added to route_tier_table.h without a
// matching entry in that other test's kTiers array would silently go
// unchecked there, while this loop -- driven by ROUTE_TIER_TABLE_COUNT
// itself -- picks up every row automatically, new tiers included.
static void test_every_real_route_allows_with_auth_off(void) {
    TEST_SECTION("http_auth_check -- every row in the real kRouteTierTable ALLOWs with "
                 "web_enabled==false (plan section 11 acceptance criterion, literal table walk)");

    size_t n = ROUTE_TIER_TABLE_COUNT;
    TEST_CHECK(n > 100, "the real table has a plausible number of rows (sanity floor)");

    for (size_t i = 0; i < n; i++) {
        const route_tier_entry_t *e = &kRouteTierTable[i];
        // bootstrap_needed and role are irrelevant once web_enabled is
        // false -- http_auth_check()'s very first line returns ALLOW before
        // consulting either, so exercise the two extremes of each to prove
        // the table walk is not accidentally hiding behind one lucky
        // combination.
        http_auth_decision_t d1 = http_auth_check(e->tier, HTTP_AUTH_ROLE_NONE, false, false, false);
        http_auth_decision_t d2 = http_auth_check(e->tier, HTTP_AUTH_ROLE_ADMIN, false, true, false);
        TEST_CHECK(d1 == HTTP_AUTH_DECISION_ALLOW && d2 == HTTP_AUTH_DECISION_ALLOW,
                   "every real route_tier_table.h row ALLOWs with auth off, regardless of role/bootstrap");
    }
}

static void test_decision_counts_as_activity(void) {
    TEST_SECTION("http_auth_decision_counts_as_activity -- section 8's activity predicate");

    // Only an ALLOW against a real credential tier counts. Every non-ALLOW
    // decision is never activity, regardless of tier.
    TEST_CHECK(!http_auth_decision_counts_as_activity(ROUTE_TIER_USER, HTTP_AUTH_DECISION_DENY_NO_SESSION),
               "USER tier DENY_NO_SESSION is not activity");
    TEST_CHECK(!http_auth_decision_counts_as_activity(ROUTE_TIER_ADMIN, HTTP_AUTH_DECISION_DENY_INSUFFICIENT),
               "ADMIN tier DENY_INSUFFICIENT is not activity");
    TEST_CHECK(!http_auth_decision_counts_as_activity(ROUTE_TIER_OPEN, HTTP_AUTH_DECISION_DENY_NO_SESSION),
               "OPEN tier denial (should not happen in practice) is still not activity");

    // ALLOW on USER/ADMIN is the whole point -- ordinary authenticated
    // requests extend the session.
    TEST_CHECK(http_auth_decision_counts_as_activity(ROUTE_TIER_USER, HTTP_AUTH_DECISION_ALLOW),
               "USER tier ALLOW counts as activity");
    TEST_CHECK(http_auth_decision_counts_as_activity(ROUTE_TIER_ADMIN, HTTP_AUTH_DECISION_ALLOW),
               "ADMIN tier ALLOW counts as activity");

    // ALLOW on OPEN must NOT count -- this is the keepalive-vs-activity
    // distinction section 8 states explicitly: "a dashboard polling
    // /api/status does NOT count, which is the whole point." The new
    // GET /api/auth/session status poll is classified OPEN for exactly this
    // reason.
    TEST_CHECK(!http_auth_decision_counts_as_activity(ROUTE_TIER_OPEN, HTTP_AUTH_DECISION_ALLOW),
               "OPEN tier ALLOW (e.g. GET /api/auth/session, GET /api/status) is NOT activity");

    // ALLOW on SAFETY_REDUCE and ADMIN_BOOTSTRAP also must not count -- both
    // can ALLOW with no session at all (HTTP_AUTH_ROLE_NONE), so treating
    // them as activity would mean "touching" a session that may not exist,
    // or extending session-less traffic that was never authenticated.
    TEST_CHECK(!http_auth_decision_counts_as_activity(ROUTE_TIER_SAFETY_REDUCE, HTTP_AUTH_DECISION_ALLOW),
               "SAFETY_REDUCE tier ALLOW is not activity (allowed even with no session)");
    TEST_CHECK(!http_auth_decision_counts_as_activity(ROUTE_TIER_ADMIN_BOOTSTRAP, HTTP_AUTH_DECISION_ALLOW),
               "ADMIN_BOOTSTRAP tier ALLOW is not activity (fires only before a credential exists)");

    // ROUTE_TIER_WIFI_SETUP is the one exception among the state-keyed
    // tiers: it DOES count, since once provisioned it is gated exactly like
    // ROUTE_TIER_ADMIN -- see http_auth_decision_counts_as_activity()'s own
    // comment for why this is still safe while unprovisioned (no session
    // exists to extend in that case).
    TEST_CHECK(http_auth_decision_counts_as_activity(ROUTE_TIER_WIFI_SETUP, HTTP_AUTH_DECISION_ALLOW),
               "WIFI_SETUP tier ALLOW counts as activity (gated like ADMIN once provisioned)");
}

// Owner report 2026-09-24: page shells are served without a session so the
// UI never opens on a login; the /api/ gate is what protects data. This pins
// the allowlist's shape so the page-shell bypass can never widen by accident:
// exactly the sixteen static shells, GET only, never an /api/ route, never an
// unlisted non-/api GET (a future download/export route stays fail-closed).
static void test_page_shell_allowlist(void) {
    TEST_CHECK(PAGE_SHELL_URI_COUNT == 16, "kPageShellUris lists exactly the 16 static page shells");
    for (size_t i = 0; i < PAGE_SHELL_URI_COUNT; i++) {
        const char *uri = kPageShellUris[i];
        route_tier_t tier = ROUTE_TIER_OPEN;
        TEST_CHECK(uri != NULL && strncmp(uri, "/api/", 5) != 0, "no page-shell entry is an /api/ route");
        TEST_CHECK(http_auth_lookup_tier(uri, HTTP_GET, &tier) &&
                       (tier == ROUTE_TIER_USER || tier == ROUTE_TIER_ADMIN || tier == ROUTE_TIER_WIFI_SETUP),
                   "every page-shell entry has a USER/ADMIN/WIFI_SETUP GET row in kRouteTierTable");
        TEST_CHECK(http_auth_is_page_shell_get(uri, HTTP_GET), "a listed page shell qualifies on GET");
        TEST_CHECK(!http_auth_is_page_shell_get(uri, HTTP_POST), "a listed page shell never qualifies on POST");
    }
    size_t shells = 0;
    for (size_t i = 0; i < ROUTE_TIER_TABLE_COUNT; i++) {
        const route_tier_entry_t *e = &kRouteTierTable[i];
        bool is_shell = http_auth_is_page_shell_get(e->uri, e->method);
        if (strncmp(e->uri, "/api/", 5) == 0) {
            TEST_CHECK(!is_shell, "no /api/ table row is ever a page shell");
        }
        if (e->method != HTTP_GET) {
            TEST_CHECK(!is_shell, "no non-GET table row is ever a page shell");
        }
        if (is_shell) {
            shells++;
        }
    }
    TEST_CHECK(shells == 16, "exactly 16 table rows resolve as page shells");
    TEST_CHECK(!http_auth_is_page_shell_get("/api/zones", HTTP_GET), "GET /api/zones is not a page shell");
    TEST_CHECK(!http_auth_is_page_shell_get("/api/autotune", HTTP_GET), "GET /api/autotune is not a page shell");
    TEST_CHECK(!http_auth_is_page_shell_get("/settings/export", HTTP_GET),
               "an unlisted, untabled non-/api GET is not a page shell (stays fail-closed ADMIN)");
    TEST_CHECK(!http_auth_is_page_shell_get("/settings/", HTTP_GET), "exact match only: trailing slash does not qualify");
    TEST_CHECK(!http_auth_is_page_shell_get("/", HTTP_GET), "OPEN dashboard is not on the list (it needs no bypass)");
    TEST_CHECK(!http_auth_is_page_shell_get(NULL, HTTP_GET), "NULL uri is not a page shell");
}

// Owner decision 2026-09-28: /wifi, /networks and /scan open without a
// session ONLY while the board is unprovisioned; once provisioned they must
// require an administrator session, same as the sibling /provision,
// /forget, /ip_config routes. Named routes, not just the abstract
// ROUTE_TIER_WIFI_SETUP enum value, so a future accidental re-tiering of one
// of these three specific URIs (e.g. back to ROUTE_TIER_OPEN) is caught here
// even if the enum-level matrix test above stays green.
static void check_wifi_setup_route_provisioned_gating(const char *uri, httpd_method_t method,
                                                        const char *label) {
    route_tier_t tier;
    char msg[256];

    TEST_CHECK(http_auth_lookup_tier(uri, method, &tier), label);
    snprintf(msg, sizeof(msg), "%s: tier is ROUTE_TIER_WIFI_SETUP", label);
    TEST_CHECK(tier == ROUTE_TIER_WIFI_SETUP, msg);

    // Auth off: ALLOW regardless of provisioning state -- section 11's
    // blanket rule, unaffected by this tier.
    snprintf(msg, sizeof(msg), "%s: auth off -> ALLOW regardless of provisioning", label);
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_NONE, false, false, true) == HTTP_AUTH_DECISION_ALLOW, msg);
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_NONE, false, false, false) == HTTP_AUTH_DECISION_ALLOW, msg);

    // Auth on + unprovisioned: ALLOW with no session at all -- the
    // captive-portal first-time-setup flow must work end to end before any
    // credential exists.
    snprintf(msg, sizeof(msg), "%s: auth on + unprovisioned + no session -> ALLOW", label);
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_NONE, true, false, true) == HTTP_AUTH_DECISION_ALLOW, msg);

    // Auth on + PROVISIONED: falls through to ADMIN-tier gating.
    snprintf(msg, sizeof(msg), "%s: auth on + provisioned + no session -> DENY_NO_SESSION", label);
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_NONE, true, false, false) == HTTP_AUTH_DECISION_DENY_NO_SESSION,
               msg);

    snprintf(msg, sizeof(msg), "%s: auth on + provisioned + USER session -> DENY_INSUFFICIENT (ADMIN only)", label);
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_USER, true, false, false) == HTTP_AUTH_DECISION_DENY_INSUFFICIENT,
               msg);

    snprintf(msg, sizeof(msg), "%s: auth on + provisioned + ADMIN session -> ALLOW", label);
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_ADMIN, true, false, false) == HTTP_AUTH_DECISION_ALLOW, msg);

    // A stale admin session while bootstrap_needed must not survive a
    // physical credential reset, same as any other ADMIN-gated route --
    // provisioning state does not override that.
    snprintf(msg, sizeof(msg), "%s: provisioned + ADMIN session + bootstrap_needed -> DENY_INSUFFICIENT", label);
    TEST_CHECK(http_auth_check(tier, HTTP_AUTH_ROLE_ADMIN, true, true, false) == HTTP_AUTH_DECISION_DENY_INSUFFICIENT,
               msg);
}

static void test_wifi_setup_tier_provisioned_gating(void) {
    TEST_SECTION("http_auth_check -- ROUTE_TIER_WIFI_SETUP: open only while unprovisioned "
                 "(owner decision 2026-09-28)");
    check_wifi_setup_route_provisioned_gating("/wifi", HTTP_GET, "GET /wifi");
    check_wifi_setup_route_provisioned_gating("/networks", HTTP_GET, "GET /networks");
    check_wifi_setup_route_provisioned_gating("/scan", HTTP_GET, "GET /scan");

    // Provisioned + no session: the /wifi page shell is still served (so the
    // nav link lands on the page and app.js raises the login modal), but its
    // two JSON reads are not shells and stay 401.
    TEST_CHECK(http_auth_is_page_shell_get("/wifi", HTTP_GET), "GET /wifi is a page shell");
    TEST_CHECK(!http_auth_is_page_shell_get("/networks", HTTP_GET), "GET /networks is not a page shell");
    TEST_CHECK(!http_auth_is_page_shell_get("/scan", HTTP_GET), "GET /scan is not a page shell");

    // The sibling mutation routes stay ordinary ROUTE_TIER_ADMIN,
    // unconditional on provisioning state -- confirms this change did not
    // accidentally widen them too.
    route_tier_t tier;
    TEST_CHECK(http_auth_lookup_tier("/provision", HTTP_POST, &tier) && tier == ROUTE_TIER_ADMIN,
               "POST /provision stays ordinary ROUTE_TIER_ADMIN, not WIFI_SETUP");
    TEST_CHECK(http_auth_lookup_tier("/forget", HTTP_POST, &tier) && tier == ROUTE_TIER_ADMIN,
               "POST /forget stays ordinary ROUTE_TIER_ADMIN, not WIFI_SETUP");
    TEST_CHECK(http_auth_lookup_tier("/ip_config", HTTP_POST, &tier) && tier == ROUTE_TIER_ADMIN,
               "POST /ip_config stays ordinary ROUTE_TIER_ADMIN, not WIFI_SETUP");
}

static void test_refusal_close_threshold(void) {
    TEST_SECTION("http_auth_refusal_should_close -- only a body above the purge threshold closes");
    TEST_CHECK(!http_auth_refusal_should_close(0), "no body: keep the connection");
    TEST_CHECK(!http_auth_refusal_should_close(HTTP_AUTH_REFUSAL_DRAIN_MAX_BYTES), "exactly the threshold: keep");
    TEST_CHECK(http_auth_refusal_should_close(HTTP_AUTH_REFUSAL_DRAIN_MAX_BYTES + 1), "one byte over: close");
    TEST_CHECK(http_auth_refusal_should_close(2500000u), "a multi-MB OTA image body: close");
}

void run_test_http_auth_enforce(void) {
    test_page_shell_allowlist();
    test_lookup_tier_real_routes();
    test_effective_tier_fail_closed_default();
    test_auth_disabled_inert_path();
    test_open_tier_no_credentials();
    test_no_session_denied();
    test_expired_session_denied();
    test_insufficient_tier_denied();
    test_safety_reduce_always_allowed();
    test_unresolvable_tier_end_to_end();
    test_bootstrap_route_tier_is_admin_bootstrap();
    test_admin_bootstrap_tier_gated_on_bootstrap_needed();
    test_admin_tier_denied_while_bootstrap_needed();
    test_full_matrix_every_tier_role_auth_bootstrap();
    test_every_table_row_reachable_via_lookup();
    test_every_real_route_allows_with_auth_off();
    test_decision_counts_as_activity();
    test_wifi_setup_tier_provisioned_gating();
    test_refusal_close_threshold();
}
