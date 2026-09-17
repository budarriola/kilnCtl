// Real security_backend_vtable_t, wired to web_auth_store.h (WEB_AUTH_PLAN.md
// items 2/3/11). See security_backend_web_auth.h for scope. ESP-only: not
// part of the host test build (build_host_tests.ps1 keeps exercising
// security_http_core.c's dispatch logic against a hand-built fake vtable,
// same as before -- this file is exactly the kind of ESP-facing glue
// ota_http_esp.c is for ota_auth.c, deliberately thin and not itself
// host-tested).
#include "security_backend_web_auth.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"

#include "security_backend.h"
#include "web_auth_store.h"
#include "wifi_prov.h"

static const char *TAG = "security_backend_web_auth";

// security_role_t (this page's seam) and web_auth_role_t (the store's) are
// deliberately two separate enums -- item 6 owns the former, items 2/3 own
// the latter -- so map explicitly rather than casting; a future divergence
// in either enum's numbering is then a compile-time-obvious one-line fix
// here, not a silent reinterpretation.
static web_auth_role_t to_store_role(security_role_t role)
{
    return (role == SECURITY_ROLE_ADMIN) ? WEB_AUTH_ROLE_ADMINISTRATOR : WEB_AUTH_ROLE_USER;
}

// security_policy_t/security_config_t (this page's seam) carry timeouts in
// MINUTES (1-60, or -1 "never" -- security_http_core.c's
// security_timeout_minutes_is_valid() already enforces that range at the
// page layer). web_auth_policy_t stores web_timeout_s/lcd_timeout_s in
// SECONDS. Converting at this one boundary keeps both the page's UI-facing
// unit and the store's on-disk unit each the natural one for their own
// callers, rather than forcing either side to carry the other's unit.
static int32_t minutes_to_seconds(int minutes)
{
    return (minutes < 0) ? -1 : (int32_t)minutes * 60;
}

static int seconds_to_minutes(int32_t seconds)
{
    if (seconds < 0) {
        return -1;
    }
    // Round to the nearest minute rather than truncating -- a policy this
    // backend itself always writes in whole minutes never lands on a
    // non-multiple-of-60 value, but a value written by some other future
    // caller should still render sanely instead of silently losing time.
    return (int)((seconds + 30) / 60);
}

static security_err_t web_auth_backend_set_web_password(security_role_t role, const char *username,
                                                          const char *password)
{
    if (!password) {
        return SECURITY_ERR_INVALID_INPUT;
    }

    const char *ap_ssid = wifi_prov_get_ap_ssid();
    const char *ap_password = wifi_prov_get_ap_password();
    const char *username_for_check = (role == SECURITY_ROLE_ADMIN) ? username : NULL;

    web_auth_pw_check_t strength = web_auth_password_check(password, username_for_check, ap_ssid, ap_password);
    if (strength != WEB_AUTH_PW_OK) {
        return SECURITY_ERR_WEAK;
    }

    if (role == SECURITY_ROLE_ADMIN) {
        if (!username || username[0] == '\0') {
            return SECURITY_ERR_INVALID_INPUT;
        }
    }

    uint8_t salt[WEB_AUTH_SALT_LEN];
    esp_fill_random(salt, sizeof(salt));

    // item 10's forced-change flag applies only to a credential set BY the
    // physical reset gesture, never to an ordinary owner-initiated change
    // through this page -- always false here.
    hal_status_t status = web_auth_store_set_password(to_store_role(role),
                                                        (role == SECURITY_ROLE_ADMIN) ? username : "user",
                                                        password, salt, /*must_change=*/false);
    if (status != HAL_OK) {
        ESP_LOGE(TAG, "web_auth_store_set_password(role=%d) failed, status=%d", (int)role, (int)status);
        return SECURITY_ERR_STORAGE;
    }
    return SECURITY_OK;
}

static security_err_t web_auth_backend_set_lcd_pin(security_role_t role, const char *pin)
{
    if (!pin) {
        return SECURITY_ERR_INVALID_INPUT;
    }

    // The OTHER role's differ-from check already happened in
    // security_http_core.c (item 3: "the web setter" owns it) using the
    // caller-supplied lcd_pin_other -- do not re-derive it here from a
    // stored hash, which is one-way and cannot be compared against a
    // plaintext candidate anyway. web_auth_pin_check() here re-validates
    // format (length/digits) so the store and the page can never disagree
    // about what is an acceptable PIN, matching item 3's own split.
    web_auth_pin_check_t check = web_auth_pin_check(pin, NULL);
    if (check == WEB_AUTH_PIN_TOO_SHORT || check == WEB_AUTH_PIN_TOO_LONG || check == WEB_AUTH_PIN_NOT_DIGITS) {
        return SECURITY_ERR_WEAK;
    }

    uint8_t salt[WEB_AUTH_SALT_LEN];
    esp_fill_random(salt, sizeof(salt));

    hal_status_t status = web_auth_store_set_pin(to_store_role(role), pin, salt);
    if (status != HAL_OK) {
        ESP_LOGE(TAG, "web_auth_store_set_pin(role=%d) failed, status=%d", (int)role, (int)status);
        return SECURITY_ERR_STORAGE;
    }
    return SECURITY_OK;
}

static security_err_t web_auth_backend_set_policy(const security_policy_t *policy)
{
    if (!policy) {
        return SECURITY_ERR_INVALID_INPUT;
    }

    // Item 11: refusing to enable auth without a credential is this page's
    // job (the store's setter just persists what it is given) -- enforce it
    // here, at the one place both roles' configured-state is checked
    // together, rather than duplicating this gate in security_http_core.c
    // where neither role's storage state is otherwise visible.
    if (policy->web_enabled &&
        !web_auth_store_password_configured(WEB_AUTH_ROLE_ADMINISTRATOR) &&
        !web_auth_store_password_configured(WEB_AUTH_ROLE_USER)) {
        return SECURITY_ERR_INVALID_INPUT;
    }
    if (policy->lcd_enabled &&
        !web_auth_store_pin_configured(WEB_AUTH_ROLE_ADMINISTRATOR) &&
        !web_auth_store_pin_configured(WEB_AUTH_ROLE_USER)) {
        return SECURITY_ERR_INVALID_INPUT;
    }

    web_auth_policy_t store_policy = {
        .web_enabled = policy->web_enabled,
        .lcd_enabled = policy->lcd_enabled,
        .web_timeout_s = minutes_to_seconds(policy->web_timeout_min),
        .lcd_timeout_s = minutes_to_seconds(policy->lcd_timeout_min),
    };

    hal_status_t status = web_auth_store_set_policy(&store_policy);
    if (status != HAL_OK) {
        ESP_LOGE(TAG, "web_auth_store_set_policy failed, status=%d", (int)status);
        return SECURITY_ERR_STORAGE;
    }
    return SECURITY_OK;
}

static bool web_auth_backend_get_config(security_config_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    web_auth_policy_t policy;
    web_auth_load_status_t policy_status = web_auth_store_load_policy(&policy);
    bool web_enabled_stored = (policy_status == WEB_AUTH_LOAD_OK) ? policy.web_enabled : false;
    bool lcd_enabled_stored = (policy_status == WEB_AUTH_LOAD_OK) ? policy.lcd_enabled : false;

    // web_auth_policy_effective_enabled() is the one function that turns
    // ABSENT/OK/UNREADABLE into the real yes/no this page must show -- do
    // not re-derive that collapse by hand here (see web_auth_store.h's
    // header comment on why ABSENT and UNREADABLE must never look alike).
    out->web_enabled = web_auth_policy_effective_enabled(policy_status, web_enabled_stored);
    out->lcd_enabled = web_auth_policy_effective_enabled(policy_status, lcd_enabled_stored);
    out->web_timeout_min = (policy_status == WEB_AUTH_LOAD_OK) ? seconds_to_minutes(policy.web_timeout_s) : -1;
    out->lcd_timeout_min = (policy_status == WEB_AUTH_LOAD_OK) ? seconds_to_minutes(policy.lcd_timeout_s) : -1;

    out->admin_password_set = web_auth_store_password_configured(WEB_AUTH_ROLE_ADMINISTRATOR);
    out->user_password_set = web_auth_store_password_configured(WEB_AUTH_ROLE_USER);
    out->lcd_admin_pin_set = web_auth_store_pin_configured(WEB_AUTH_ROLE_ADMINISTRATOR);
    out->lcd_user_pin_set = web_auth_store_pin_configured(WEB_AUTH_ROLE_USER);

    web_auth_password_record_t admin_record;
    if (web_auth_store_load_password(WEB_AUTH_ROLE_ADMINISTRATOR, &admin_record) == WEB_AUTH_LOAD_OK) {
        out->admin_must_change = admin_record.must_change;
        // Never echo a hash/salt back -- only the username, which is not a
        // secret (it is shown in the login form itself).
        strncpy(out->admin_username, admin_record.username, sizeof(out->admin_username) - 1);
        out->admin_username[sizeof(out->admin_username) - 1] = '\0';
    }

    return true;
}

static void web_auth_backend_invalidate_sessions_for_role(security_role_t role)
{
    // WEB_AUTH_PLAN.md item 4/5's session table (web_auth_session.h) has not
    // landed on main yet -- this stays a documented no-op, exactly like the
    // placeholder it replaces, until that module exists. Replacing this one
    // function body (e.g. web_auth_session_invalidate_role(to_store_role(role)))
    // is then the entire remaining integration step, no other file in this
    // adapter changes.
    ESP_LOGW(TAG, "invalidate_sessions_for_role(role=%d) is a no-op -- the session table "
                  "(WEB_AUTH_PLAN.md item 4/5) has not landed yet",
             (int)role);
}

static const security_backend_vtable_t s_web_auth_vtable = {
    .set_web_password = web_auth_backend_set_web_password,
    .set_lcd_pin = web_auth_backend_set_lcd_pin,
    .set_policy = web_auth_backend_set_policy,
    .get_config = web_auth_backend_get_config,
    .invalidate_sessions_for_role = web_auth_backend_invalidate_sessions_for_role,
};

void security_backend_web_auth_install(void)
{
    security_backend_set_vtable(&s_web_auth_vtable);
}
