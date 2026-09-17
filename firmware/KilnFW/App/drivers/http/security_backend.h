// security_backend -- the seam between the password page (security_http.c/
// security_http_core.c, WEB_AUTH_PLAN.md item 6) and the credential-storage
// module WEB_AUTH_PLAN.md items 2/2b/3 describe, plus a thin hook into the
// session layer item 4/5 own.
//
// SCOPE NOTE, so a future reader does not mistake this file for the
// storage/strength-rule implementation itself: this header and its default
// ("placeholder") implementation in security_backend_placeholder.c belong to
// the password-page work (item 6). Items 2 ("PBKDF2-HMAC-SHA256, salt,
// kiln_auth NVS namespace"), 3 ("10-char/non-lowercase/rejection-list web
// rule") and 4/5 (session table, enforcement pre-handler) are each a
// separate, explicitly out-of-scope piece of work. Nothing here hashes a
// password, touches NVS, or resolves a session -- it only declares the
// shape that page's HTTP layer calls through, so the real modules can be
// wired in later by replacing ONE vtable (security_backend_set_vtable())
// rather than touching security_http.c/security_http_core.c at all.
//
// Host-buildable: no esp_http_server.h, no NVS, no PSA/mbedtls dependency.
#ifndef SECURITY_BACKEND_H
#define SECURITY_BACKEND_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mirrors WEB_AUTH_PLAN.md item 1's two non-OPEN tiers. This page is
// ADMIN-only (item 6), but the dispatch core still takes a caller role
// explicitly (rather than assuming ADMIN) so the "a user session gets 403
// from every method on the security endpoints" acceptance criterion is
// enforced right here in a host-testable place, independent of whether
// item 5's registration-time tier table has landed yet.
typedef enum {
    SECURITY_ROLE_USER = 0,
    SECURITY_ROLE_ADMIN = 1,
} security_role_t;

typedef enum {
    SECURITY_OK = 0,
    SECURITY_ERR_INVALID_INPUT,   // caller-supplied value is malformed/out of range
    SECURITY_ERR_WEAK,            // rejected by the strength/complexity rule (item 3)
    SECURITY_ERR_STORAGE,         // the backend could not persist the change
    SECURITY_ERR_NOT_IMPLEMENTED, // placeholder backend: item 2/2b hasn't landed yet
} security_err_t;

// Read-only snapshot for GET /api/auth/config -- deliberately carries no
// secret material (no hash, no salt, no plaintext), only enough for the page
// to render its current state and decide what to show/prompt for.
typedef struct {
    bool web_enabled;
    bool lcd_enabled;
    int web_timeout_min;  // 1-60, or -1 meaning "never" (item 8)
    int lcd_timeout_min;  // 1-60, or -1 meaning "never"
    bool admin_password_set;
    bool user_password_set;
    bool lcd_user_pin_set;
    bool lcd_admin_pin_set;
    bool admin_must_change; // item 10's post-reset forced-change flag
    char admin_username[33];
} security_config_t;

typedef struct {
    int web_timeout_min; // -1 = never
    int lcd_timeout_min; // -1 = never
    bool web_enabled;
    bool lcd_enabled;
} security_policy_t;

// Every entry point returns SECURITY_ERR_NOT_IMPLEMENTED until a real
// backend is installed via security_backend_set_vtable() -- see
// security_backend_placeholder.c. None of them may block: this vtable is
// called from the httpd task (item 4's stack-budget note applies) and,
// via item 9, must never be reachable from any safety path in the first
// place, so it never needs to be.
typedef struct {
    // Sets the administrator or user WEB password. `username` is only
    // meaningful (and only accepted) for the administrator record -- the
    // plan's single `user` record has no separate username field to change.
    // Strength validation (item 3) happens inside this call; a rejection
    // must come back as SECURITY_ERR_WEAK, not a generic invalid-input.
    security_err_t (*set_web_password)(security_role_t role, const char *username, const char *password);

    // Sets one LCD PIN. `pin` is a NUL-terminated ASCII digit string,
    // already range/uniqueness-checked by security_http_core (item 3: "the
    // web setter rejects out-of-range before hashing" -- that check is
    // this page's job, done before this call, not the backend's).
    security_err_t (*set_lcd_pin)(security_role_t role, const char *pin);

    security_err_t (*set_policy)(const security_policy_t *policy);

    // Fills *out and returns true, or returns false if no policy/credential
    // record exists yet at all (item 11: "no credential set" is the shipped
    // default, not an error -- the caller must render that as an
    // unconfigured board, not a failure).
    bool (*get_config)(security_config_t *out);

    // Seam into item 4/5's session table: "changing a password invalidates
    // every session for that role, including the caller's" (item 6). Must
    // be a no-op, not a crash, until the session layer lands.
    void (*invalidate_sessions_for_role)(security_role_t role);
} security_backend_vtable_t;

// Test/production injection point. Passing NULL restores the default
// (placeholder) vtable. Not thread-safe by design -- call once at startup
// (production) or once per test case (host tests), never concurrently.
void security_backend_set_vtable(const security_backend_vtable_t *vt);

// Always non-NULL: returns whatever was last installed via
// security_backend_set_vtable(), or the built-in placeholder if nothing
// was ever installed.
const security_backend_vtable_t *security_backend_get_vtable(void);

#ifdef __cplusplus
}
#endif

#endif // SECURITY_BACKEND_H
