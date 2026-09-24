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

#ifdef __cplusplus
}
#endif

#endif // AUTH_TOTP_HTTP_H
