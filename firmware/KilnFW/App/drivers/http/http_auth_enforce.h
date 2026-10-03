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
#include <stddef.h>

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
//     turned on. (The nine OTA-family routes' own AP-password fallback that
//     used to be a named exception here was retired 2026-09-29 -- see this
//     file's own note below -- so this function's ALLOW/DENY verdict is now
//     the ONLY gate on those routes too, same as every other
//     ROUTE_TIER_ADMIN route.)
//
//   web_enabled == true, tier == ROUTE_TIER_OPEN:
//     ALLOW, regardless of role -- including HTTP_AUTH_ROLE_NONE. The
//     Dashboard must stay viewable with no credential even once auth is on.
//
//   web_enabled == true, tier == ROUTE_TIER_SAFETY_REDUCE:
//     ALLOW, regardless of role -- including HTTP_AUTH_ROLE_NONE. Plan
//     section 9: a route that can only ever reduce heat/risk must never
//     become harder to reach once auth is on than it was with auth off -- a
//     locked-out owner watching a kiln climb is a worse failure mode than
//     the one authentication protects against. This is decided from
//     route_tier_table.h's own per-route classification, not a URI string
//     match inside this function or the pre-handler -- a second,
//     independently maintained match would be exactly the
//     reset-one-side-of-a-pair shape CLAUDE.md documents. UNUSED as of the
//     2026-09-28 owner-decision follow-up: no route carries this tier today
//     (the current-sweep/autotune/danger-mode aborts moved to
//     ROUTE_TIER_ADMIN and POST /api/profile_exec/stop to ROUTE_TIER_USER,
//     both the same day) -- kept for a future route that needs this shape.
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
// 2026-09-29: the nine OTA-family routes' own AP-password MAC check
// (section 2b's "known integration point") was retired along with the
// whole AP-password HMAC scheme -- WEB_AUTH_PLAN.md item 2b, owner decision
// "Retire; open when login off". An administrator WEB SESSION now
// satisfies those routes on its own: this function's ALLOW/DENY verdict for
// ROUTE_TIER_ADMIN is the only gate, exactly like every other admin route,
// and ota_http.c no longer runs any independent in-handler check.
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
//
// `wifi_unprovisioned` is http_auth_policy_wifi_unprovisioned()'s
// already-resolved result (http_auth_policy_iface.h, backed by
// wifi_prov_is_unprovisioned()) -- never re-derived here from raw Wi-Fi
// state, same discipline as `web_enabled`/`bootstrap_needed`. It changes one
// thing, owner decision 2026-09-28:
//
//   tier == ROUTE_TIER_WIFI_SETUP:
//     ALLOW iff wifi_unprovisioned, regardless of role -- the AP
//     captive-portal first-time-setup flow (/wifi, /networks, /scan) must
//     work before any credential exists. Once provisioned, this tier is
//     treated exactly like ROUTE_TIER_ADMIN (an administrator session
//     required) -- it is NOT its own independent role gate past that point.
http_auth_decision_t http_auth_check(route_tier_t tier, http_auth_role_t role, bool web_enabled,
                                      bool bootstrap_needed, bool wifi_unprovisioned);

// Section 8's "what counts as activity" predicate: true only for a request
// that was actually ALLOWED against a real credential tier (USER or ADMIN).
// A passive status poll classified ROUTE_TIER_OPEN (docs/WEB_AUTH_PLAN.md's
// "GET /api/auth/session" keepalive) must NOT count as activity even though
// it is ALLOWED -- that route is deliberately kept OPEN so it never reaches
// this predicate's true branch. ROUTE_TIER_SAFETY_REDUCE (unused as of
// 2026-09-28 -- see http_auth_check()'s doc comment above) and
// ROUTE_TIER_ADMIN_BOOTSTRAP are excluded too: neither implies an
// authenticated session worth extending (SAFETY_REDUCE allows even with no
// session; ADMIN_BOOTSTRAP's ALLOW only ever fires before a credential
// exists). ROUTE_TIER_WIFI_SETUP DOES count, since once provisioned it is
// gated exactly like ROUTE_TIER_ADMIN -- an ALLOW there while still
// unprovisioned has no session to extend anyway (the call site never has a
// non-empty token in that case), so including this tier is safe in both
// states. Any non-ALLOW decision is never activity.
bool http_auth_decision_counts_as_activity(route_tier_t tier, http_auth_decision_t decision);

// True only for a GET whose uri is listed in route_tier_table.h's
// kPageShellUris[] AND has a GET row in kRouteTierTable. The pre-handler
// serves such a request's static shell even on a DENY decision (the page's
// own /api/ fetches keep their tier gate); every other route -- any
// "/api/..." uri, any non-GET method, any unlisted or untabled uri -- keeps
// the ordinary 401/403. Pure, host-tested in test_http_auth_enforce.c.
bool http_auth_is_page_shell_get(const char *uri, httpd_method_t method);

// Largest unread request body (Content-Length) a refused request may leave
// for esp_http_server to purge. The purge runs at CONFIG_HTTPD_PURGE_BUF_LEN
// (32 B) per read on the single CPU0 httpd task, so a multi-MB unauthenticated
// POST (an OTA image) would hog it and trip TASK_WDT on IDLE0 (bench finding
// 2026-10-03). 4 KB = 128 purge reads at most.
#define HTTP_AUTH_REFUSAL_DRAIN_MAX_BYTES 4096u

// True when a refusal sent for a request with `content_len` unread body bytes
// should return ESP_FAIL (httpd closes the socket without purging) instead of
// ESP_OK (httpd purges the body, then keeps the connection). Above the
// threshold only. Trade-off: closing with unread RX data makes lwIP send a
// RST, so a client uploading a large body may see a reset rather than the
// 401/403. Pure, host-tested in test_http_auth_enforce.c.
bool http_auth_refusal_should_close(size_t content_len);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_HTTP_AUTH_ENFORCE_H
