// security_http_core -- the pure, host-testable logic behind
// POST /api/auth/security and GET /api/auth/config (WEB_AUTH_PLAN.md item
// 6). No esp_http_server.h dependency, same split ota_auth.c/ota_http.c
// already use (item 12's "the new module must be host-buildable" rule):
// this file owns request validation, role gating and the call-through to
// the security_backend_vtable_t seam; security_http.c owns only header
// parsing and response sending.
#ifndef SECURITY_HTTP_CORE_H
#define SECURITY_HTTP_CORE_H

#include <stdbool.h>

#include "security_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SECURITY_CMD_SET_ADMIN_PASSWORD,
    SECURITY_CMD_SET_USER_PASSWORD,
    SECURITY_CMD_SET_LCD_PIN,
    SECURITY_CMD_SET_POLICY,
    SECURITY_CMD_UNKNOWN,
} security_cmd_t;

// One request shape covers every action this page performs; unused fields
// for a given command are ignored. Fixed-size buffers only (item 4's "no
// large stack local on the httpd task" rule) -- these sizes match
// WEB_AUTH_PLAN.md item 2's stored record shapes (username[33],
// PIN 4-8 digits) plus one byte of slack for a defensive NUL.
#define SECURITY_HTTP_USERNAME_MAX 32
#define SECURITY_HTTP_PASSWORD_MAX 128
#define SECURITY_HTTP_PIN_MAX 8

typedef struct {
    security_cmd_t cmd;

    // SET_ADMIN_PASSWORD / SET_USER_PASSWORD
    char username[SECURITY_HTTP_USERNAME_MAX + 1]; // administrator only; ignored for the user record
    char password[SECURITY_HTTP_PASSWORD_MAX + 1];

    // SET_LCD_PIN
    security_role_t lcd_pin_role;
    char lcd_pin[SECURITY_HTTP_PIN_MAX + 1];
    // The OTHER PIN's current value, so this page can enforce "the two PINs
    // must also differ from each other" (item 3) without the backend having
    // to expose the stored PIN back out (it never does -- item 2's records
    // are one-way hashes). Empty string means "not set yet" / "not
    // supplied" and never blocks the change on that ground alone.
    char lcd_pin_other[SECURITY_HTTP_PIN_MAX + 1];

    // SET_POLICY
    security_policy_t policy;
} security_request_t;

#define SECURITY_HTTP_MESSAGE_MAX 160

typedef struct {
    int http_status;                         // 200/400/403/501/500
    char message[SECURITY_HTTP_MESSAGE_MAX];
    bool invalidated_sessions;
    security_role_t invalidated_role;        // meaningful only if invalidated_sessions
} security_result_t;

// Item 3's PIN-specific rule ("4 to 8 digits ... must also differ from each
// other") lives here because the plan assigns it to "the web setter" --
// this page -- explicitly, as distinct from item 2/3's hashing/complexity
// rules which stay behind the backend seam. All-digit, length in [4,8],
// and (when `other` is non-empty) not equal to `other`.
bool security_pin_is_valid(const char *pin, const char *other);

// Item 8's range: 1-60 minutes, or the sentinel -1 meaning "never".
bool security_timeout_minutes_is_valid(int minutes);

// Dispatches one request against `vt` as `caller_role`. Never calls a
// backend entry point when a pre-check already refuses the request (no
// partial side effects). On SECURITY_OK from a password/PIN backend call,
// invalidates that role's sessions via vt->invalidate_sessions_for_role and
// reports it in *out (item 6: "changing a password invalidates every
// session for that role, including the caller's"). A policy change never
// invalidates anything (item 6: "a preference, not a credential").
void security_http_dispatch(const security_backend_vtable_t *vt, security_role_t caller_role,
                             const security_request_t *req, security_result_t *out);

#ifdef __cplusplus
}
#endif

#endif // SECURITY_HTTP_CORE_H
