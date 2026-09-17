// web_auth_login_http.h -- docs/WEB_AUTH_PLAN.md section 6: the browser-side
// login. Registers GET /login (the login page) and POST /api/auth/login
// (the credential check that mints a session), the last missing piece
// named by that plan's section 0 status note ("Section 6, the login page
// and route ... has not landed yet").
//
// ESP-only: not part of the host test build (httpd_req_t/hal_sysinfo/NVS
// all live below this header). The pure role-selection logic this calls
// into (web_auth_login_role_for_username(), net/web_auth_login.h) IS
// host-tested, same pure/glue split as ota_auth.h/.c vs ota_http_esp.c.
//
// Both new routes are classified ROUTE_TIER_OPEN in route_tier_table.h --
// same precedent as GET /api/ota/challenge: a route that is part of an
// authenticated flow but must be reachable by a caller who does not yet
// have a session at all (there is no credential to check without first
// being able to load the login form and submit to it).
#ifndef KILNCTL_WEB_AUTH_LOGIN_HTTP_H
#define KILNCTL_WEB_AUTH_LOGIN_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers GET /login and POST /api/auth/login via kiln_http_register().
// Call once at startup (main_network_http.c bringup), after
// wifi_provision_http_start() has brought the HTTP server up -- same
// ordering requirement security_backend_web_auth_start() already documents.
// Non-fatal on failure is the caller's job, same convention as every other
// *_http_start() in that bringup sequence.
esp_err_t web_auth_login_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_WEB_AUTH_LOGIN_HTTP_H
