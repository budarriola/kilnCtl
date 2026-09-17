// http_auth_http.h -- the ESP-side half of docs/WEB_AUTH_PLAN.md section 5:
// kiln_http_register(), a drop-in replacement for
// httpd_register_uri_handler() that installs a shared pre-handler in front
// of the caller's real handler. The pre-handler consults route_tier_table.h
// (via http_auth_enforce.h's pure http_auth_effective_tier()/
// http_auth_check()) and denies before the real handler body ever runs.
//
// Deliberately NOT wired into every existing httpd_register_uri_handler()
// call site as part of this change -- ~22 driver files across
// firmware/KilnFW/App/drivers register routes today, several of them being
// actively edited by other in-flight sections of this same feature
// (sections 2b, 6, 7). Rewiring all of them here would collide with that
// work file-by-file for no benefit: this header is the finished, tested
// seam a later pass (or each section's own owner, when it adds/changes a
// route) drops in by changing one call from
// `httpd_register_uri_handler(server, &uri)` to
// `kiln_http_register(server, &uri)`. Nothing about the wrapper's behaviour
// depends on being adopted everywhere at once -- an un-migrated route is
// simply unaffected (open exactly as it is today), never MORE open than a
// migrated one.
#ifndef KILNCTL_HTTP_AUTH_HTTP_H
#define KILNCTL_HTTP_AUTH_HTTP_H

#include <stdbool.h>

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers `uri_handler` with `server`, wrapped so every request is
// checked against route_tier_table.h before `uri_handler->handler` runs.
//
// Tier is resolved from route_tier_table.h by (uri_handler->uri,
// uri_handler->method) -- NOT passed in by the caller. This is deliberate:
// route_tier_table.h's own header comment warns that a second,
// independently-supplied tier at each call site is exactly the kind of
// duplicated fact that can drift from the table silently. A route missing
// from the table is not a caller error this function can refuse to
// register over (the route still needs to work once auth is off, or on a
// board that predates this feature) -- instead it is logged loudly
// (ESP_LOGE, "ROUTE WITH NO TIER") and treated as ADMIN, the fail-closed
// default http_auth_effective_tier() already implements.
//
// Fails (returns the underlying httpd_register_uri_handler() error, or
// ESP_ERR_NO_MEM if the fixed-size wrapper-context table is full) rather
// than registering an unwrapped handler -- there is no path through this
// function that falls back to plain httpd_register_uri_handler() on
// failure, since that would silently reintroduce the exact per-route
// allowlist gap this design exists to avoid.
esp_err_t kiln_http_register(httpd_handle_t server, const httpd_uri_t *uri_handler);

// For a route classified ROUTE_TIER_OPEN, kiln_http_prehandler() above never
// resolves a caller role (see its own comment: http_auth_check() would
// ALLOW regardless, so it isn't worth the session-table lookup on the
// dashboard's hot poll paths). That means an OPEN handler that wants to
// serve a DIFFERENT body to an authenticated administrator than to an
// anonymous caller -- e.g. GET /api/ota/esp/status keeping the route itself
// reachable pre-auth for OTA clients while still hiding exact build/commit
// identity from an unauthenticated network onlooker -- cannot lean on the
// pre-handler's decision and must ask this instead.
//
// Returns true iff: web auth is currently off (docs/WEB_AUTH_PLAN.md section
// 11 -- a board with auth off is exactly as open as the board is today, so
// every caller is treated the same as an authenticated admin would be), OR
// the request carries a session cookie that resolves to HTTP_AUTH_ROLE_ADMIN.
// A USER-role session, an expired/absent session, and auth-on-but-no-cookie
// all return false, identically -- same no-distinguishing-why discipline as
// http_auth_check()'s own DENY_NO_SESSION collapse.
//
// Safe to call from any OPEN-tier (or unwrapped) handler; this does its own
// cookie extraction and does not depend on kiln_http_register()'s wrapper
// context.
bool http_auth_caller_is_admin(httpd_req_t *req);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_HTTP_AUTH_HTTP_H
