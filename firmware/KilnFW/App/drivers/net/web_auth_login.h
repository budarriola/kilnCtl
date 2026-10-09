// web_auth_login.h -- pure, host-testable role-selection logic for
// docs/WEB_AUTH_PLAN.md section 6 (the login page and POST /api/auth/login).
//
// SCOPE: this module answers exactly one question -- given the username a
// caller typed into the login form and the administrator's stored username
// (or NULL/empty if no administrator credential is configured yet), which
// role's password record should be checked against the submitted password?
// It does NOT itself call web_auth_store_verify_password() (that needs the
// real store, ESP-only/hal_kv-backed) and it does NOT mint tokens, create
// sessions, or touch the HTTP layer -- those live in the ESP-glue module
// web_auth_login_http.c, mirroring the pure/glue split every other slice in
// this codebase uses (ota_auth.h/.c vs ota_http_esp.c, web_auth_store.h's
// own pure-strength-check vs I/O-record split).
//
// WHY THIS EXISTS RATHER THAN JUST CALLING web_auth_store_verify_password()
// TWICE: running the PBKDF2-style KDF (WEB_AUTH_ITERATIONS rounds, per the
// record's own stamped iteration count) against both roles' records for
// every login attempt would double the KDF
// cost on the httpd task for no benefit -- the administrator record's
// username is not a secret (it is shown on the login form itself, same as
// web_auth_backend_get_config()'s admin_username field), so comparing it
// first and only ever running the KDF once, against the ONE role the
// submitted username actually selects, is strictly cheaper and no less
// secure: a wrong username still reaches exactly one
// web_auth_store_verify_password() call (against the USER role, whose own
// record may or may not be configured), never zero, so there is no timing
// side-channel that distinguishes "unknown username" from "known username,
// wrong password" at this layer.
//
// The comparison is deliberately case-sensitive, exact-string, no
// normalization -- same convention web_auth_password_check() already uses
// for comparing a candidate password against the username/AP SSID/AP
// password rejection list (web_auth_store.h: "case-SENSITIVE for
// username/SSID/AP password -- those are exact secrets, not English
// words"). A username is not free text to be fuzzy-matched here.
#ifndef KILNCTL_WEB_AUTH_LOGIN_H
#define KILNCTL_WEB_AUTH_LOGIN_H

#include "web_auth_session.h" // web_auth_session_role_t

#ifdef __cplusplus
extern "C" {
#endif

// Selects which role's password record the caller should verify the
// submitted password against.
//
// `submitted_username` is the username field from the login form -- may be
// NULL or empty (a NULL/empty username never matches a real, non-empty
// admin username, so it always resolves to USER).
//
// `admin_username_or_null` is the administrator role's currently configured
// username (web_auth_password_record_t.username from
// web_auth_store_load_password(WEB_AUTH_ROLE_ADMINISTRATOR, ...) when that
// call returned WEB_AUTH_LOAD_OK and record.configured is true), or NULL/
// empty if no administrator credential exists yet (WEB_AUTH_LOAD_ABSENT/
// UNREADABLE, or configured == false) -- an empty/NULL admin username can
// never be matched by any submitted username, so login always falls through
// to USER in that state, which is correct: with no administrator credential
// configured there is nothing an administrator login could ever succeed
// against.
//
// Returns WEB_AUTH_SESSION_ROLE_ADMIN iff `submitted_username` is non-NULL,
// non-empty, and byte-for-byte equal to `admin_username_or_null`; returns
// WEB_AUTH_SESSION_ROLE_USER in every other case (including both NULL,
// mismatched, or either side empty). Never returns
// WEB_AUTH_SESSION_ROLE_NONE -- that value means "no session", not "no
// role to check", and has no meaning as this function's answer.
web_auth_session_role_t web_auth_login_role_for_username(const char *submitted_username,
                                                            const char *admin_username_or_null);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_WEB_AUTH_LOGIN_H
