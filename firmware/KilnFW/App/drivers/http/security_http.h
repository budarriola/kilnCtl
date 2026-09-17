// security_http.h -- the ESP-side glue for WEB_AUTH_PLAN.md section 6's
// password page: registers GET /settings/security (the page shell),
// GET /api/auth/config (read-only status for that page) and
// POST /api/auth/security (the one dispatch route the page's saved buttons
// all call), per the wire contract net/security_page.html's own header
// comment already defines.
//
// This is deliberately the last piece named by that page's own DEFERRED
// comment: route registration, this HTTP glue layer, the CMakeLists.txt
// SRCS/EMBED_TXTFILES wiring, and the nav.js entry. The pure logic this
// file calls into -- request validation, role gating, the call-through to
// security_backend_vtable_t -- already lives in security_http_core.c/.h and
// is host-tested there; this file owns only header/body parsing and
// response sending, same split ota_auth.c/ota_http_esp.c and
// web_auth_login.h/web_auth_login_http.c already use.
//
// ESP-only: not part of the host test build (httpd_req_t and the real
// security_backend_get_vtable() installation both live below this header).
#ifndef KILNCTL_SECURITY_HTTP_H
#define KILNCTL_SECURITY_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers the three routes above via kiln_http_register(). Call once at
// startup (main_network_http.c bringup), after wifi_provision_http_start()
// has brought the HTTP server up and after security_backend_web_auth_start()
// has installed the real vtable -- same ordering convention every other
// *_http_start() in that bringup sequence follows. Non-fatal on failure is
// the caller's job.
//
// All three routes are ROUTE_TIER_ADMIN in route_tier_table.h, so
// kiln_http_register()'s shared pre-handler already refuses a non-admin
// caller before any handler body here runs; this file's handlers still
// resolve the caller's role themselves (http_auth_caller_is_admin()) and
// pass it into security_http_dispatch(), which repeats the same ADMIN-only
// check on its own terms (defence in depth -- see security_http_core.c's
// own comment on why that check is unconditional there too).
esp_err_t security_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_SECURITY_HTTP_H
