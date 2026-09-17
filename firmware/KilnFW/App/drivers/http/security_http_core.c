#include "security_http_core.h"

#include <ctype.h>
#include <string.h>

bool security_pin_is_valid(const char *pin, const char *other)
{
    if (!pin) {
        return false;
    }
    size_t len = strlen(pin);
    if (len < 4 || len > SECURITY_HTTP_PIN_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)pin[i])) {
            return false;
        }
    }
    if (other && other[0] != '\0' && strcmp(pin, other) == 0) {
        return false; // item 3: the two PINs must differ from each other
    }
    return true;
}

bool security_timeout_minutes_is_valid(int minutes)
{
    if (minutes == -1) {
        return true; // "never" (item 8)
    }
    return minutes >= 1 && minutes <= 60;
}

static void set_result(security_result_t *out, int status, const char *msg)
{
    out->http_status = status;
    out->invalidated_sessions = false;
    // snprintf is not available in every host-test TU without <stdio.h>;
    // keep this a bounded strncpy since every call site below passes a
    // short, static literal, never anything caller-controlled -- no
    // password or username is ever echoed back into this field.
    size_t n = strlen(msg);
    if (n >= SECURITY_HTTP_MESSAGE_MAX) {
        n = SECURITY_HTTP_MESSAGE_MAX - 1;
    }
    memcpy(out->message, msg, n);
    out->message[n] = '\0';
}

static bool username_is_valid(const char *username)
{
    size_t len = strlen(username);
    if (len == 0 || len > SECURITY_HTTP_USERNAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)username[i];
        if (!isprint(c) || isspace(c)) {
            return false;
        }
    }
    return true;
}

// Translates a security_err_t into the result's status/message. Never
// called for SECURITY_OK -- callers handle that branch themselves so they
// can also set invalidated_sessions/invalidated_role.
static void set_result_from_err(security_result_t *out, security_err_t err)
{
    switch (err) {
    case SECURITY_ERR_INVALID_INPUT:
        set_result(out, 400, "invalid input");
        return;
    case SECURITY_ERR_WEAK:
        set_result(out, 400, "rejected: does not meet the password/PIN strength rule");
        return;
    case SECURITY_ERR_STORAGE:
        set_result(out, 500, "applied checks passed but the value could not be saved");
        return;
    case SECURITY_ERR_NOT_IMPLEMENTED:
        set_result(out, 501, "credential storage is not available on this build yet");
        return;
    case SECURITY_OK:
        break;
    }
    set_result(out, 500, "internal error");
}

void security_http_dispatch(const security_backend_vtable_t *vt, security_role_t caller_role,
                             const security_request_t *req, security_result_t *out)
{
    memset(out, 0, sizeof(*out));

    // Item 6: "/settings/security" is ADMIN only -- a `user` cannot open it
    // and cannot change their own password. Checked first and unconditionally,
    // independent of whatever item 5's registration-time tier table does (or
    // does not yet) enforce -- belt and suspenders on the one page in this
    // feature that can create or destroy every other credential.
    if (caller_role != SECURITY_ROLE_ADMIN) {
        set_result(out, 403, "administrator only");
        return;
    }

    if (!vt) {
        set_result(out, 500, "no backend installed");
        return;
    }

    switch (req->cmd) {
    case SECURITY_CMD_SET_ADMIN_PASSWORD: {
        if (!username_is_valid(req->username)) {
            set_result(out, 400, "invalid administrator username");
            return;
        }
        if (req->password[0] == '\0') {
            set_result(out, 400, "password must not be empty");
            return;
        }
        security_err_t err = vt->set_web_password(SECURITY_ROLE_ADMIN, req->username, req->password);
        if (err != SECURITY_OK) {
            set_result_from_err(out, err);
            return;
        }
        vt->invalidate_sessions_for_role(SECURITY_ROLE_ADMIN);
        set_result(out, 200, "administrator password updated");
        out->invalidated_sessions = true;
        out->invalidated_role = SECURITY_ROLE_ADMIN;
        return;
    }
    case SECURITY_CMD_SET_USER_PASSWORD: {
        if (req->password[0] == '\0') {
            set_result(out, 400, "password must not be empty");
            return;
        }
        // The `user` record has no separate username in item 2's schema
        // (one fixed `user` role); pass NULL so the backend cannot mistake
        // an unrelated field for one.
        security_err_t err = vt->set_web_password(SECURITY_ROLE_USER, NULL, req->password);
        if (err != SECURITY_OK) {
            set_result_from_err(out, err);
            return;
        }
        vt->invalidate_sessions_for_role(SECURITY_ROLE_USER);
        set_result(out, 200, "user password updated");
        out->invalidated_sessions = true;
        out->invalidated_role = SECURITY_ROLE_USER;
        return;
    }
    case SECURITY_CMD_SET_LCD_PIN: {
        if (!security_pin_is_valid(req->lcd_pin, req->lcd_pin_other)) {
            set_result(out, 400, "PIN must be 4-8 digits and differ from the other PIN");
            return;
        }
        security_err_t err = vt->set_lcd_pin(req->lcd_pin_role, req->lcd_pin);
        if (err != SECURITY_OK) {
            set_result_from_err(out, err);
            return;
        }
        // Item 6: "Changing the LCD PIN drops the LCD session." The LCD has
        // exactly one session, not a slot per role (item 4) -- the backend
        // decides what that means; this call site's contract is simply
        // "a PIN changed, tell the session layer which role's PIN it was".
        vt->invalidate_sessions_for_role(req->lcd_pin_role);
        set_result(out, 200, "LCD PIN updated");
        out->invalidated_sessions = true;
        out->invalidated_role = req->lcd_pin_role;
        return;
    }
    case SECURITY_CMD_SET_POLICY: {
        if (!security_timeout_minutes_is_valid(req->policy.web_timeout_min) ||
            !security_timeout_minutes_is_valid(req->policy.lcd_timeout_min)) {
            set_result(out, 400, "timeout must be 1-60 minutes or \"never\"");
            return;
        }
        security_err_t err = vt->set_policy(&req->policy);
        if (err != SECURITY_OK) {
            set_result_from_err(out, err);
            return;
        }
        // Item 6: "Changing the lock timeout does not invalidate anything
        // -- it is a preference, not a credential." Deliberately no
        // invalidate_sessions_for_role call in this branch, even though
        // this command can also flip the enabled switches -- item 11's own
        // "what happens to live sessions on a policy change" rule
        // (disable keeps sessions, enable clears them) belongs to the
        // backend that actually owns the session table, not to this
        // dispatch, which only forwards the requested policy.
        set_result(out, 200, "policy updated");
        return;
    }
    case SECURITY_CMD_CLEAR_CREDENTIALS: {
        // Item 12b's "Clear login credentials" action -- see
        // security_backend.h's clear_all_credentials() comment for the full
        // rationale. No request fields to validate; this command takes none.
        security_err_t err = vt->clear_all_credentials();
        if (err != SECURITY_OK) {
            set_result_from_err(out, err);
            return;
        }
        // Both roles' web passwords and both roles' LCD PINs were just
        // cleared -- every session for both roles must go, not only the
        // caller's own, same "changing a password invalidates every
        // session for that role" reasoning item 6 already applies per-role.
        // invalidate_sessions_for_role() also unconditionally force-locks
        // the single LCD session (security_backend_web_auth.c), so calling
        // it for USER after ADMIN is what actually reaches the `user`
        // web-session slots; the LCD side is covered by the first call.
        vt->invalidate_sessions_for_role(SECURITY_ROLE_ADMIN);
        vt->invalidate_sessions_for_role(SECURITY_ROLE_USER);
        set_result(out, 200, "login credentials cleared");
        out->invalidated_sessions = true;
        out->invalidated_role = SECURITY_ROLE_ADMIN;
        return;
    }
    case SECURITY_CMD_UNKNOWN:
    default:
        set_result(out, 400, "unknown action");
        return;
    }
}
