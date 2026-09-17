// http_auth_enforce.h -- the pure, host-testable enforcement decision for
// docs/WEB_AUTH_PLAN.md section 5: "a route that reaches the enforcement
// point without a resolvable tier must be denied, not allowed."
//
// This is deliberately the ONLY place tier vs. role vs. auth-enabled is
// decided. Two properties make that fail-closed by construction rather than
// by a default value someone has to remember to write:
//
//   1. Tier resolution (http_auth_lookup_tier()/http_auth_effective_tier())
//      consults route_tier_table.h -- the SAME table
//      tools/check_route_tier_coverage.ps1 enforces is complete for every
//      real route -- rather than maintaining a second, independently kept
//      classification. A second copy here is exactly the "reset one side of
//      a pair" bug class CLAUDE.md documents four prior instances of: two
//      tables that start identical and can silently drift apart. A route not
//      found in the table does not fall through to any default the caller
//      chooses -- http_auth_effective_tier() itself returns ROUTE_TIER_ADMIN,
//      the most restrictive tier, so an unclassified route can only ever
//      become MORE locked down than intended, never less.
//
//   2. http_auth_check() below is a single small function with an explicit
//      case for every (auth-enabled, tier, role) combination and an
//      unreachable default that still denies. There is no code path through
//      it that returns ALLOW because a case was left unhandled -- see its
//      own comment.
//
// Pure: no ESP-IDF dependency beyond the httpd_method_t/httpd_uri_t types
// route_tier_table.h itself names (App/test/stubs/esp_http_server.h supplies
// those on the host, same convention as ota_auth.h/heater_output.c). No I/O,
// no locks, no allocation -- safe to call from inside the httpd task's
// shared 8 KB stack with no locals over a few dozen bytes, and safe to call
// from a host test with an injected role/policy.
//
// The session lookup itself is NOT here -- see http_session_iface.h's own
// comment for that seam. This header only ever receives an already-resolved
// http_auth_role_t; it has no idea whether that role came from a real
// session table, an in-flight stub, or a test.
#ifndef KILNCTL_HTTP_AUTH_ENFORCE_H
#define KILNCTL_HTTP_AUTH_ENFORCE_H

#include <stdbool.h>

#include "route_tier_table.h" // route_tier_t, kRouteTierTable, httpd_method_t

#ifdef __cplusplus
extern "C" {
#endif

// The caller's resolved role, AFTER the session layer has already collapsed
// every failure mode -- absent session, expired session, a token matching no
// live slot, a slot whose role somehow reads as invalid -- into the same
// value. See http_session_iface.h: there is deliberately no separate "error"
// return distinct from "no session".
typedef enum {
    HTTP_AUTH_ROLE_NONE = 0, // no valid session for this request
    HTTP_AUTH_ROLE_USER,
    HTTP_AUTH_ROLE_ADMIN,
} http_auth_role_t;

typedef enum {
    HTTP_AUTH_DECISION_ALLOW = 0,
    HTTP_AUTH_DECISION_DENY_NO_SESSION,   // caller should answer 401
    HTTP_AUTH_DECISION_DENY_INSUFFICIENT, // caller should answer 403
} http_auth_decision_t;

// Looks up (uri, method) in kRouteTierTable by exact string match. Returns
// false, with *out_tier left untouched, when no row matches -- callers that
// need a tier regardless of whether one was found should use
// http_auth_effective_tier() instead, which is what makes "not found" a safe
// default rather than something every call site has to remember to check.
//
// This function does no logging: emitting the loud "ROUTE WITH NO TIER"
// diagnostic the plan calls for belongs to the ESP-side registration wrapper
// (kiln_http_register(), http_auth_http.c), which has ESP_LOGE available and
// runs once per route at boot, not once per request.
bool http_auth_lookup_tier(const char *uri, httpd_method_t method, route_tier_t *out_tier);

// Same lookup, but a miss resolves to ROUTE_TIER_ADMIN -- the fail-closed
// default the plan requires ("treated as ADMIN -- the most restrictive
// tier"). This is what http_auth_check() and the registration wrapper should
// call; http_auth_lookup_tier() exists separately only so a caller that
// needs to distinguish "found ADMIN" from "not found at all" (the
// registration-time diagnostic) still can.
route_tier_t http_auth_effective_tier(const char *uri, httpd_method_t method);

// The one decision function. Every input combination maps to exactly one of
// the three outcomes below; there is no fall-through path that reaches the
// end of the function body with no explicit case having matched -- read the
// .c file's comment on this if changing it, since that structural property,
// not any single case, is what "fail closed" means here.
//
//   web_enabled == false:
//     ALLOW, unconditionally, for every tier. This is section 11's
//     mandated behaviour -- "a board with auth off is exactly as open as
//     the board is today" -- and it is deliberately checked FIRST, before
//     role or tier are even inspected, so a bug in role resolution can
//     never accidentally deny access on a board that has never had auth
//     turned on. (The one named exception, the nine OTA-family routes'
//     own AP-password fallback, lives entirely inside ota_http.c/
//     ota_auth.c's independent in-handler check -- see this file's own
//     "Known integration point" note below -- and is untouched by this
//     function returning ALLOW here.)
//
//   web_enabled == true, tier == ROUTE_TIER_OPEN:
//     ALLOW, regardless of role -- including HTTP_AUTH_ROLE_NONE. The
//     Dashboard must stay viewable with no credential even once auth is on.
//
//   web_enabled == true, tier == ROUTE_TIER_SAFETY_REDUCE:
//     ALLOW, regardless of role -- including HTTP_AUTH_ROLE_NONE. Plan
//     section 9: a route that can only ever reduce heat/risk (today, POST
//     /api/profile_exec/stop) must never become harder to reach once auth
//     is on than it was with auth off -- a locked-out owner watching a kiln
//     climb is a worse failure mode than the one authentication protects
//     against. This is decided from route_tier_table.h's own per-route
//     classification, not a URI string match inside this function or the
//     pre-handler -- a second, independently maintained match would be
//     exactly the reset-one-side-of-a-pair shape CLAUDE.md documents.
//
//   web_enabled == true, tier != ROUTE_TIER_OPEN, tier != ROUTE_TIER_SAFETY_REDUCE,
//   role == HTTP_AUTH_ROLE_NONE:
//     DENY_NO_SESSION. No session, an expired session, and an unresolvable
//     session all arrive here as the same role value (see the type's own
//     comment) and are denied identically -- a gated route never leaks
//     whether a session existed and merely expired.
//
//   web_enabled == true, tier == ROUTE_TIER_USER, role == USER or ADMIN:
//     ALLOW.
//
//   web_enabled == true, tier == ROUTE_TIER_ADMIN, role == ADMIN:
//     ALLOW.
//
//   web_enabled == true, tier == ROUTE_TIER_ADMIN, role == USER:
//     DENY_INSUFFICIENT.
//
// KNOWN INTEGRATION POINT for whoever implements section 2b: an
// administrator WEB SESSION is supposed to satisfy the nine OTA-family
// routes with no MAC at all. That is not decided here -- ota_http.c's own
// ota_http_authenticate_request() call inside each of those nine handlers
// runs independently of this pre-handler and, until section 2b wires it to
// consult a resolved role, still demands the legacy AP-password MAC even
// when this function has already returned ALLOW for an admin session. That
// is not a security hole (the two checks compose as AND, so the OTA route
// stays at least as strict as either check alone) but it does mean OTA
// currently requires BOTH an admin session AND a MAC until section 2b lands
// -- tracked there, not fixed here.
//
// `bootstrap_needed` is the caller's already-resolved
// web_auth_admin_bootstrap_needed() result (net/web_auth_session.h) -- never
// re-derived here from raw policy/credential state, same discipline as
// `web_enabled` itself. It changes two things:
//
//   tier == ROUTE_TIER_ADMIN_BOOTSTRAP:
//     ALLOW iff bootstrap_needed, else DENY_INSUFFICIENT -- unconditional on
//     role (there can be no session yet in the true case, and once bootstrap
//     is no longer needed this one-shot route must close even for an
//     administrator, who has /settings/security instead).
//
//   tier == ROUTE_TIER_ADMIN, bootstrap_needed == true:
//     DENY_INSUFFICIENT, even when role == HTTP_AUTH_ROLE_ADMIN. A physical
//     credential reset (plan item 10) clears the administrator credential
//     record but does NOT invalidate existing sessions -- without this check
//     a stale pre-reset admin session would still satisfy every ADMIN route
//     while the board is waiting for a fresh credential, defeating the
//     bootstrap gate entirely.
http_auth_decision_t http_auth_check(route_tier_t tier, http_auth_role_t role, bool web_enabled,
                                      bool bootstrap_needed);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_HTTP_AUTH_ENFORCE_H
