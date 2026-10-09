// auth_totp_http.h -- see auth_totp_http.c for scope.
#ifndef AUTH_TOTP_HTTP_H
#define AUTH_TOTP_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers GET /api/auth/totp_status, POST /api/auth/forgot and
// POST /api/auth/reset via kiln_http_register(). TOTP enrollment/disable
// ride the existing POST /api/auth/security cmd= dispatch instead
// (security_http.c) -- see docs/TOTP_PASSWORD_RESET_PLAN.md section 6b.
// Call once at startup (main_network_http.c bringup), after NVS and the
// httpd server are up. Resolves the server handle itself via
// wifi_provision_http_get_server(), same "no server param" convention as
// the rest of this component's *_http_start() functions.
esp_err_t auth_totp_http_start(void);

// Clears every outstanding /api/auth/forgot reset token immediately.
// Call after a SUCCESSFUL totp_enroll_confirm or totp_disable
// (security_http.c) so a token minted against the OLD enrollment can never
// be consumed against a NEW one: an attacker who holds the old factor could
// mint a token, have the owner disable+re-enroll TOTP within
// TOTP_RESET_TOKEN_TTL_MS, and still set the admin password with the stale
// token, since /api/auth/reset's own re-check only asks "is TOTP enrolled",
// not "is it the SAME enrollment". Safe to call with no server started yet
// or no tokens outstanding. Runs on the httpd worker; no lock needed.
void auth_totp_http_clear_reset_tokens(void);

#ifdef __cplusplus
}
#endif

#endif // AUTH_TOTP_HTTP_H
