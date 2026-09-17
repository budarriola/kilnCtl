// Default (placeholder) security_backend_vtable_t -- see security_backend.h
// for what this seam is and is not. Every mutating entry point refuses with
// SECURITY_ERR_NOT_IMPLEMENTED and logs loudly rather than silently
// accepting a password it cannot actually store: a credential the page
// claims to have "saved" but did not persist is worse than a page that
// visibly refuses, per the same "do not log unchecked success" lesson
// CLAUDE.md's boot_guard section already draws elsewhere in this tree.
// get_config() reports the safe, defined "no credential set" state
// (WEB_AUTH_PLAN.md item 11): both interfaces report disabled, nothing is
// marked as set. That is deliberately indistinguishable from a freshly
// flashed board that has never touched auth at all.
//
// Portable (no ESP-IDF headers) so this file, like security_backend.h, can
// be linked into a host test unchanged -- though host tests normally inject
// their own fake vtable via security_backend_set_vtable() rather than
// exercising this one directly.
#include "security_backend.h"

#include <stdio.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#define SECURITY_BACKEND_LOGW(fmt, ...) ESP_LOGW("security_backend", fmt, ##__VA_ARGS__)
#else
#define SECURITY_BACKEND_LOGW(fmt, ...) fprintf(stderr, "W security_backend: " fmt "\n", ##__VA_ARGS__)
#endif

static security_err_t placeholder_set_web_password(security_role_t role, const char *username, const char *password)
{
    (void)username;
    (void)password;
    SECURITY_BACKEND_LOGW("set_web_password(role=%d) refused -- credential storage (WEB_AUTH_PLAN.md item 2) "
                           "is not wired in yet; password NOT saved",
                           (int)role);
    return SECURITY_ERR_NOT_IMPLEMENTED;
}

static security_err_t placeholder_set_lcd_pin(security_role_t role, const char *pin)
{
    (void)pin;
    SECURITY_BACKEND_LOGW("set_lcd_pin(role=%d) refused -- credential storage (WEB_AUTH_PLAN.md item 2) "
                           "is not wired in yet; PIN NOT saved",
                           (int)role);
    return SECURITY_ERR_NOT_IMPLEMENTED;
}

static security_err_t placeholder_set_policy(const security_policy_t *policy)
{
    (void)policy;
    SECURITY_BACKEND_LOGW("set_policy() refused -- policy storage (WEB_AUTH_PLAN.md item 2) "
                           "is not wired in yet; policy NOT saved");
    return SECURITY_ERR_NOT_IMPLEMENTED;
}

static bool placeholder_get_config(security_config_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    // Item 11: an absent policy record reads as both-off, which is exactly
    // what a genuinely unconfigured board (or, today, this placeholder)
    // must report -- never a synthesized "on" or a fabricated username.
    out->web_enabled = false;
    out->lcd_enabled = false;
    out->web_timeout_min = -1;
    out->lcd_timeout_min = -1;
    return true;
}

static void placeholder_invalidate_sessions_for_role(security_role_t role)
{
    SECURITY_BACKEND_LOGW("invalidate_sessions_for_role(role=%d) is a no-op -- the session table "
                           "(WEB_AUTH_PLAN.md item 4/5) is not wired in yet",
                           (int)role);
}

static const security_backend_vtable_t s_placeholder_vtable = {
    .set_web_password = placeholder_set_web_password,
    .set_lcd_pin = placeholder_set_lcd_pin,
    .set_policy = placeholder_set_policy,
    .get_config = placeholder_get_config,
    .invalidate_sessions_for_role = placeholder_invalidate_sessions_for_role,
};

static const security_backend_vtable_t *s_active_vtable = &s_placeholder_vtable;

void security_backend_set_vtable(const security_backend_vtable_t *vt)
{
    s_active_vtable = vt ? vt : &s_placeholder_vtable;
}

const security_backend_vtable_t *security_backend_get_vtable(void)
{
    return s_active_vtable;
}
