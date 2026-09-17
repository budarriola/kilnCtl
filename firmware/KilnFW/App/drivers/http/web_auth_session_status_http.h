// web_auth_session_status_http.h -- docs/WEB_AUTH_PLAN.md section 8's
// web-GUI half of the inactivity lock: the browser-side status poll and
// explicit "stay unlocked" extension, layered on top of section 4's session
// table and section 6's login flow. No new session table, no new
// cookie-extraction path, no new timeout value -- see http_session_iface.h's
// http_auth_session_status()/http_auth_session_touch() for the primitives
// this just exposes over HTTP.
//
// GET /api/auth/session (ROUTE_TIER_OPEN):
//   A passive poll a page can call on an interval to render its own
//   countdown/prompt. Deliberately OPEN, not USER -- see
//   http_auth_decision_counts_as_activity()'s own comment: a route that is
//   itself activity would mean the poll extends the very session it is
//   reporting on, defeating the whole point of the lock. This handler
//   therefore resolves its own role via http_auth_caller_is_admin()'s sibling
//   http_auth_extract_session_token() + http_auth_session_status(), NOT via
//   the pre-handler's role resolution (which never runs for OPEN tiers).
//   Response body: {"role":"none"|"user"|"admin","prompt":bool,
//   "seconds_left":N} -- role "none" covers absent/expired/auth-disabled
//   alike (section 11: auth off collapses to full access, reported here as
//   role "admin" with prompt always false, matching http_auth_check()'s own
//   ALLOW-everything behaviour so the page never shows a lock UI on a board
//   that has never had auth turned on).
//
// POST /api/auth/session/extend (ROUTE_TIER_USER):
//   The explicit "stay unlocked" action. Its own handler body does nothing
//   beyond returning {"ok":true} -- classifying this route USER means the
//   shared pre-handler's own activity-touch call (kiln_http_prehandler(),
//   http_auth_http.c) already extends the session on every ALLOWed request
//   to a USER/ADMIN route, this one included. No special-case touch code
//   lives in this file.
#ifndef KILNCTL_WEB_AUTH_SESSION_STATUS_HTTP_H
#define KILNCTL_WEB_AUTH_SESSION_STATUS_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers GET /api/auth/session and POST /api/auth/session/extend via
// kiln_http_register(). Call once at startup (main_network_http.c bringup),
// after web_auth_login_http_start() -- same ordering requirement that
// function's own header documents (the HTTP server must already be up).
// Non-fatal on failure is the caller's job, same convention as every other
// *_http_start() in that bringup sequence.
esp_err_t web_auth_session_status_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_WEB_AUTH_SESSION_STATUS_HTTP_H
