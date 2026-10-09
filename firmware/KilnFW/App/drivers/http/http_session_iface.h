// http_session_iface.h -- the seam between the fail-closed enforcement point
// (http_auth_enforce.h, docs/WEB_AUTH_PLAN.md section 5) and the session
// mechanism (plan section 4), which was still in flight when section 5 was
// built. Exactly ONE function is the seam: http_auth_session_resolve()
// below. Whoever lands section 4 either (a) replaces this header's
// declaration with the real one under the same name and signature, or (b)
// keeps this header and points http_auth_http.c's #include at the real
// session module and deletes the stub .c file -- either way, nothing that
// calls into the enforcement layer (kiln_http_register(), the shared
// pre-handler) needs to change, because they only ever call this one name.
//
// This is intentionally a single narrow function, not a struct of function
// pointers or a registration callback -- the smallest possible seam, so
// "adopt the real header" really is the one-line change the task called
// for: swap the definition, not the call sites.
#ifndef KILNCTL_HTTP_SESSION_IFACE_H
#define KILNCTL_HTTP_SESSION_IFACE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "http_auth_enforce.h" // http_auth_role_t
#include "web_auth_session.h" // web_auth_table_t

#ifdef __cplusplus
extern "C" {
#endif

// The ONE cookie name this seam's session mechanism uses, shared verbatim
// between the login handler that sets it (web_auth_login_http.c's
// Set-Cookie) and http_auth_http.c's resolve_role_for_request(), which
// parses it back out of the request's Cookie header. Defined here rather
// than in either of those two files so the name can never drift between
// writer and reader -- the same reset-one-side-of-a-pair hazard
// http_session_table()/http_session_hash_token() above already guard
// against.
#define HTTP_SESSION_COOKIE_NAME "kiln_sid"

// Returns the ONE session table this seam's .c file owns (a single static
// web_auth_table_t, lazily initialised). Section 6's login handler MUST
// call web_auth_table_create_session() against THIS table, not a second
// static instance of its own -- see http_session_iface.c's header comment
// for why (the reset-one-side-of-a-pair shape CLAUDE.md warns about).
web_auth_table_t *http_session_table(void);

// Plain SHA-256 of a bearer token, exactly the hash http_auth_session_resolve()
// uses to look a token up. Section 6's login handler MUST hash the token it
// mints with THIS function before calling web_auth_table_create_session(),
// not a second, independently-written hash -- otherwise a login-minted
// token would never resolve here (same reset-one-side-of-a-pair hazard as
// http_session_table() above).
void http_session_hash_token(const char *token, size_t token_len, uint8_t out[32]);

// Resolves the caller's role from whatever the real session mechanism uses
// to identify a session -- a bearer token pulled from a cookie header, in
// UI_PLAN.md's design. `token` and `client_ip` are both NUL-terminated C
// strings (possibly empty, possibly NULL when the request carried no
// cookie/no determinable peer address).
//
// MUST return HTTP_AUTH_ROLE_NONE, and only HTTP_AUTH_ROLE_NONE, for every
// one of: no token present, a token that matches no live slot, an expired
// slot, a slot whose IP binding does not match `client_ip`, and a slot whose
// stored role is somehow invalid. There is deliberately no separate error
// return: http_auth_check() treats "no session" and "a session that failed
// to validate for any reason" identically (DENY_NO_SESSION, answered as
// 401), and a resolver that invented a distinct "error" value would just be
// a second place that same collapse would have to be re-implemented
// correctly.
//
// Until plan section 4 lands, http_session_iface_stub.c below provides a
// trivial always-HTTP_AUTH_ROLE_NONE implementation, matching this file's
// own fail-closed contract exactly rather than merely satisfying the
// linker -- a stub that returns anything else would be *reporting* a valid
// session while none exists, precisely the class of bug this seam exists to
// keep out of reach.
//
// Section 4 has since landed (branch webauth_session_k5, commit b298d13a --
// not yet on origin/main as of this writing) as
// firmware/KilnFW/App/drivers/net/web_auth_session.h's
// web_auth_role_t web_auth_effective_role(const web_auth_table_t *t, bool web_enabled,
//     const uint8_t *token_hash, uint32_t timeout_s, uint32_t now_ms);
// web_auth_role_t's three values (NONE/USER/ADMIN) map 1:1 onto
// http_auth_role_t below by numeric value, so the adopter does not need a
// translation table, only a cast (or a one-line switch, if a project style
// rule disallows enum-to-enum casts across headers). That function already
// folds the "auth off" short-circuit in (it returns ADMIN immediately when
// web_enabled is false) -- deliberately NOT the same shape as this file's
// http_auth_session_resolve(token, client_ip), which only ever resolves a
// session and leaves web-enabled entirely to http_auth_check()'s own
// `web_enabled` parameter. Replacing this stub therefore means writing a
// thin wrapper in the real http_session_iface.c (module-owned session table
// + clock source, neither of which this seam's two-string signature
// carries) that: hashes/looks up `token` into a token_hash, calls
// web_auth_effective_role(&s_table, /*web_enabled=*/true, hash, timeout_s,
// now_ms()) -- passing true here, not the real policy flag, since that
// flag is already applied by http_auth_check() on this seam's return value
// -- and casts the result to http_auth_role_t. Not a one-line body swap
// like a matching-signature seam would be, but still a change confined
// entirely to this seam's own .c file: no call site in http_auth_http.c or
// http_auth_enforce.c changes.
http_auth_role_t http_auth_session_resolve(const char *token, const char *client_ip);

// Section 8's web-GUI inactivity lock: two more entry points against the
// SAME session table http_session_table() owns -- neither adds a second
// table, a second cookie-extraction site, or a second timeout value.
//
// http_auth_session_status() answers a passive status poll (the new
// GET /api/auth/session route, classified ROUTE_TIER_OPEN so it never
// itself counts as activity -- see http_auth_decision_counts_as_activity()).
// It looks the token up and reports the session's role, its last_seen_ms,
// and the policy's resolved timeout_s (WEB_AUTH_TIMEOUT_NEVER_S when
// "never"), WITHOUT touching last_seen_ms -- a client polling this to
// render its own inactivity countdown must never itself be the reason the
// countdown never fires. Returns false (role NONE, last_seen 0) for no
// token, no matching slot, or an expired slot; timeout_s is still filled in
// on a false return so a caller can compute a prompt window even before any
// session exists.
//
// `client_ip` (2026-09-17 review, Finding 3): both this function and
// http_auth_session_touch() below used to take no client IP at all and
// applied no IP check, even though http_auth_session_resolve() above enforces
// the exact-match IP binding this header's own MUST list requires for every
// other route. GET /api/auth/session is ROUTE_TIER_OPEN, so a stolen cookie
// presented from ANY address confirmed itself live and admin -- not
// privilege escalation, but a session oracle for exactly the replay scenario
// the IP binding exists to deny. Both functions now take `client_ip` (same
// NUL-terminated-or-NULL contract as resolve()'s parameter) and apply the
// identical web_auth_effective_role() check -- a mismatched or NULL
// client_ip resolves to "no session" here exactly as it does for resolve().
bool http_auth_session_status(const char *token, const char *client_ip, web_auth_session_role_t *role_out,
                               uint32_t *last_seen_ms_out, uint32_t *timeout_s_out);

// Extends a session exactly the way ordinary authenticated activity does:
// looks the token up, and if (and only if) it is STILL VALID against the
// current timeout AND `client_ip` matches the address the session was
// issued to (Finding 3, see http_auth_session_status()'s comment above for
// the full rationale), calls web_auth_table_touch() to bump last_seen_ms to
// now and clear the prompt flag. A token that is absent, matches no slot, is
// already expired, or is presented from the wrong address is left alone --
// this function must never "revive" an expired session, which is what makes
// server-side expiry independent of the browser: a client that ignores the
// lock prompt and keeps calling this (or any USER/ADMIN route) after the
// deadline gets nothing, because the validity check runs before the touch,
// not after.
// `via_ap`: 2026-09-29 owner decision -- whether THIS request arrived over
// the SoftAP interface (wifi_prov_request_arrived_on_ap(), wifi_prov.h),
// resolved by the caller (http_auth_http.c's kiln_http_prehandler()) exactly
// once per request, same convention as `client_ip`. On a successful touch
// (all the validity/IP checks above pass), this also calls
// web_auth_table_set_via_ap() so the slot's AP-origin tag always reflects the
// interface that LAST used it, not merely the one that created it -- see
// web_auth_session.h's via_ap field comment for why "last used" was chosen.
void http_auth_session_touch(const char *token, const char *client_ip, bool via_ap);

// Explicit logout: hashes `token`, looks it up against the SAME table
// http_session_table() owns, and destroys that slot via
// web_auth_table_destroy_session() (web_auth_session.h's own explicit-
// teardown primitive, documented there as "(logout)"). Deliberately does
// NOT apply the timeout/IP-binding checks http_auth_session_touch() does --
// tearing down a slot is never a capability grant, so there is nothing for
// those checks to protect here, and a caller that already got this far
// should not be told there was nothing to do. A missing token, or a token
// that matches no live slot, is a silent no-op (idempotent: logging out
// twice, or logging out a session that already expired, is not an error).
//
// NOTE, and this is the whole reason that skip is not a hole: this
// function is NOT itself an authorization boundary. Its only caller today,
// POST /api/auth/logout (web_auth_login_http.c), is ROUTE_TIER_USER, and
// the shared pre-handler (kiln_http_prehandler(), http_auth_http.c)
// resolves that tier's role through http_auth_session_resolve(), which DOES
// enforce both expiry and the exact client_ip binding. So a request whose
// cookie is expired, or presented from an address the session was not
// issued to, is answered 401 before this function is ever reached -- the
// route cannot be used as a cross-IP "log the victim out" oracle, and it
// equally cannot clear an expired caller's cookie. Any FUTURE caller added
// on a looser tier (OPEN, or an unauthenticated path) would be adding that
// oracle itself, because this function will not refuse anything.
void http_auth_session_logout(const char *token);

// AP-fallback teardown gate (2026-09-28, owner request: never cut off a
// logged-in operator when Wi-Fi comes home while the fallback AP is up).
// Reports whether ANY slot in http_session_table() currently holds a still-
// VALID session (per web_auth_session_is_valid() against the live policy
// timeout), scanning every slot rather than one token -- unlike every other
// function in this file, this is not resolving one caller's identity, it is
// answering "is anyone at all logged in right now" for wifi_prov_link.c's
// do_ev_got_ip()/do_confirm_static_reachable() to defer an AP teardown on.
// An expired-but-still-in_use slot does NOT count (that is the whole point:
// an idle timed-out session must not hold the AP up forever), matching this
// header's usual is_valid() gate elsewhere.
//
// Fails closed toward TRUE (assume a session might be active) on an
// unreadable auth-policy record, the OPPOSITE direction from this file's
// other resolvers, which fail closed toward DENYING access. The two
// directions protect different things: http_auth_session_resolve() et al.
// guard a capability grant, where "safe" means "assume no session"; this
// function guards a physical action (dropping the fallback AP an operator's
// browser or phone may be depending on right now), where "safe" means
// "assume someone might still be connected" and defer the teardown one more
// tick rather than strand them. Callers only use this when
// http_auth_policy_web_enabled() has already reported auth ON -- with auth
// off there are no sessions to scan and this always returns false.
// Reads the clock itself (hal_time_now_ms(), same source every other
// resolver in this file uses) -- no now_ms parameter, so a caller in another
// module never has to reach into this file's clock source or risk passing a
// stale/mismatched timestamp.
bool http_auth_any_session_active(void);

// 2026-09-29 owner decision, and the actual fix for the bug report above:
// wifi_prov_link.c's ap_teardown_should_defer() no longer consults
// http_auth_any_session_active() (which counts ANY session anywhere,
// including one only ever used over the home LAN -- e.g. the PC's MCP tools,
// which never touch the AP). This is the AP-scoped sibling: reports whether
// any slot's session is both still valid AND was LAST USED over the SoftAP
// interface (web_auth_table_any_ap_session_active()). Same fail-closed-
// toward-TRUE direction on an unreadable policy record as
// http_auth_any_session_active(), for the identical reason (this also guards
// a physical action, not a capability grant). Callers only use this when
// http_auth_policy_web_enabled() has already reported auth ON -- with auth
// off, wifi_prov_link.c's own auth-off branch (any AP station connected)
// applies instead and this function is not consulted.
//
// Review fix (2026-09-29, round 2): a session whose configured timeout is
// WEB_AUTH_TIMEOUT_NEVER_S is, by definition, always "still valid" --
// web_auth_session_is_valid() short-circuits true for it regardless of
// last_seen_ms. Without special-casing that, a single AP login under a
// never-expire policy would pin the fallback AP up forever, even long after
// the operator's device physically disconnected from it, since nothing ever
// makes the session invalid again. `ap_station_present` (the caller's own
// wifi_prov_get_ap_client_count() > 0, already computed at the one call
// site) is consulted ONLY for a never-expire session: such a session defers
// teardown while, and only while, a station is ALSO actually associated to
// the AP radio -- an ordinary (non-never) session's own timeout is trusted
// as before and this parameter plays no part in that case.
bool http_auth_any_ap_session_active(bool ap_station_present);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_HTTP_SESSION_IFACE_H
