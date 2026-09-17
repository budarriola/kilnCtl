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
bool http_auth_session_status(const char *token, web_auth_session_role_t *role_out,
                               uint32_t *last_seen_ms_out, uint32_t *timeout_s_out);

// Extends a session exactly the way ordinary authenticated activity does:
// looks the token up, and if (and only if) it is STILL VALID against the
// current timeout, calls web_auth_table_touch() to bump last_seen_ms to now
// and clear the prompt flag. A token that is absent, matches no slot, or is
// already expired is left alone -- this function must never "revive" an
// expired session, which is what makes server-side expiry independent of
// the browser: a client that ignores the lock prompt and keeps calling this
// (or any USER/ADMIN route) after the deadline gets nothing, because the
// validity check runs before the touch, not after.
void http_auth_session_touch(const char *token);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_HTTP_SESSION_IFACE_H
