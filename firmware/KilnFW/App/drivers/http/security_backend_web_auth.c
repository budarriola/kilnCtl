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

#include "hal_sysinfo.h" // hal_sysinfo_fill_random() -- HAL boundary: no raw
                          // esp_random.h in this file, see check_hal_include_
                          // boundary.ps1's EspRandomAllowlist comment.
#include "http_auth_http.h"       // kiln_http_register()
#include "http_auth_policy_iface.h" // http_auth_policy_web_enabled()
#include "http_form.h"             // http_form_find_field()
#include "http_session_iface.h"    // http_session_table() -- item 2: the real
                                    // web session table this backend must
                                    // actually clear on an off->on transition
#include "security_backend.h"
#include "security_http_core.h" // SECURITY_HTTP_USERNAME_MAX/PASSWORD_MAX
#include "ui_lcd_lock.h"        // ui_lcd_lock_force_lock() -- item 2, LCD half
#include "web_auth_session.h"   // web_auth_admin_bootstrap_needed()
#include "web_auth_store.h"
#include "wifi_prov.h"
#include "wifi_provision_http.h" // wifi_provision_http_get_server()

// SECURITY_HTTP_USERNAME_MAX (security_http_core.h) and
// WEB_AUTH_USERNAME_MAX_LEN (web_auth_store.h) are two views of the SAME
// limit -- the page's request-field size vs. the store's on-disk record
// size -- kept as two separate literals in two separate modules on purpose
// (security_http_core.c must stay storage-agnostic, per this file's own
// header comment), with no shared type or macro tying them together. This
// is exactly the shape of the CRITICAL fixed today: a stored-copy buffer
// and its producer's buffer sized independently and drifting apart with no
// compiler or test signal, discovered only because every real IPv6 address
// overflowed the smaller one and left clients logged in but permanently
// 401. Here the two currently agree (both 32), but nothing stops a future
// "widen the username field" edit from touching only one side. If they
// ever disagree, `username[SECURITY_HTTP_USERNAME_MAX + 1]` either gets
// silently truncated before reaching web_auth_store_set_password() (this
// macro smaller) or the store's fixed-size `username[WEB_AUTH_USERNAME_MAX_LEN
// + 1]` record silently truncates what this page already accepted as valid
// (that macro smaller) -- either way, a username a user believes they set
// is not the one that ends up persisted. This is the one file that
// includes both headers, so it is the only place this relationship can be
// checked at compile time.
_Static_assert(SECURITY_HTTP_USERNAME_MAX == WEB_AUTH_USERNAME_MAX_LEN,
               "SECURITY_HTTP_USERNAME_MAX (security_http_core.h) and "
               "WEB_AUTH_USERNAME_MAX_LEN (web_auth_store.h) are two views of "
               "the same username length limit and must stay equal -- a "
               "mismatch silently truncates or rejects a username between "
               "the HTTP request layer and the credential store");

// Same relationship, same reasoning, for the LCD PIN length: this page's
// request buffer `lcd_pin[SECURITY_HTTP_PIN_MAX + 1]` /
// `lcd_pin_other[SECURITY_HTTP_PIN_MAX + 1]` (security_http_core.h) must be
// sized to hold the longest PIN the store will ever accept
// (WEB_AUTH_PIN_MAX_LEN, web_auth_store.h -- web_auth_pin_check() and
// web_auth_store_set_pin() are the two functions that actually enforce it).
// If this page's buffer were ever smaller than the store's real maximum, a
// valid long PIN would be silently truncated before it ever reached
// web_auth_pin_check(), so the store's own upper-bound rejection could never
// fire and the truncated value would be what actually gets hashed and
// stored -- a PIN entry a user believes they set is not the one that ends
// up persisted.
_Static_assert(SECURITY_HTTP_PIN_MAX == WEB_AUTH_PIN_MAX_LEN,
               "SECURITY_HTTP_PIN_MAX (security_http_core.h) and "
               "WEB_AUTH_PIN_MAX_LEN (web_auth_store.h) are two views of the "
               "same LCD PIN length limit and must stay equal -- a mismatch "
               "lets this page's request buffer silently truncate a PIN "
               "before the store's own length check ever sees it");

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
    hal_sysinfo_fill_random(salt, sizeof(salt)); // HAL Phase 4: routed through hal_sysinfo, same single entropy boundary as safety_link.c's esp_boot_id and ota_http_esp.c's challenge nonce -- the credential store deliberately never calls esp_fill_random() itself.

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
    hal_sysinfo_fill_random(salt, sizeof(salt)); // HAL Phase 4: routed through hal_sysinfo, same single entropy boundary as safety_link.c's esp_boot_id and ota_http_esp.c's challenge nonce -- the credential store deliberately never calls esp_fill_random() itself.

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

    // Item 1 fix (2026-09-17 adversarial review, 1179e2d3): this used to
    // re-derive its own, looser version of the enable gate right here --
    // refusing web_enabled only when NEITHER an admin NOR a user password
    // was set. web_auth_policy_check_transition() (web_auth_store.h/.c)
    // correctly requires the ADMINISTRATOR credential specifically, and
    // until this fix it was called from nowhere but its own host test.
    // Failure sequence the old code allowed: configure only a USER web
    // password, enable web auth -> the old gate above was satisfied (a
    // password IS configured, just not the administrator's) ->
    // web_auth_admin_bootstrap_needed() reads true -> http_auth_check()
    // denies every ADMIN-tier route to every caller, including
    // POST /api/security/policy itself, the one route that could undo the
    // mistake. There must be exactly one place that decides whether a
    // policy transition is legal -- this now calls that one place instead
    // of maintaining a second, weaker copy of its rule.
    web_auth_policy_t current;
    web_auth_load_status_t current_status = web_auth_store_load_policy(&current);
    if (current_status != WEB_AUTH_LOAD_OK) {
        // ABSENT or UNREADABLE: no persisted transition to compare against.
        // Treat the current state as fully off so an off->on request is
        // still correctly seen as an edge (and therefore clears sessions
        // below), matching web_auth_policy_effective_enabled()'s own
        // ABSENT-reads-as-off convention rather than inventing a third
        // interpretation here.
        current.web_enabled = false;
        current.lcd_enabled = false;
        current.web_timeout_s = -1;
        current.lcd_timeout_s = -1;
    }

    web_auth_policy_t requested = {
        .web_enabled = policy->web_enabled,
        .lcd_enabled = policy->lcd_enabled,
        .web_timeout_s = minutes_to_seconds(policy->web_timeout_min),
        .lcd_timeout_s = minutes_to_seconds(policy->lcd_timeout_min),
    };

    bool admin_password_configured = web_auth_store_password_configured(WEB_AUTH_ROLE_ADMINISTRATOR);
    bool admin_pin_configured = web_auth_store_pin_configured(WEB_AUTH_ROLE_ADMINISTRATOR);
    bool clear_web_sessions = false;
    bool clear_lcd_session = false;

    web_auth_policy_transition_t transition = web_auth_policy_check_transition(
        &current, &requested, admin_password_configured, admin_pin_configured,
        &clear_web_sessions, &clear_lcd_session);
    if (transition != WEB_AUTH_POLICY_TRANSITION_OK) {
        return SECURITY_ERR_INVALID_INPUT;
    }

    hal_status_t status = web_auth_store_set_policy(&requested);
    if (status != HAL_OK) {
        ESP_LOGE(TAG, "web_auth_store_set_policy failed, status=%d", (int)status);
        return SECURITY_ERR_STORAGE;
    }

    // Item 2 fix: the transition's out_clear_web_sessions/out_clear_lcd_session
    // outputs previously had no consumer anywhere outside test_web_auth_store.c
    // -- plan section 11's "enabling auth clears every session" was computed
    // correctly and then discarded. Wire them to the real session mechanisms:
    // http_session_table() (http_session_iface.h, the same table
    // http_auth_session_resolve() looks sessions up in) for the web half, and
    // ui_lcd_lock_force_lock() for the LCD half.
    if (clear_web_sessions) {
        web_auth_table_destroy_all(http_session_table());
    }
    if (clear_lcd_session) {
        ui_lcd_lock_force_lock();
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
    // Item 2 fix: WEB_AUTH_PLAN.md item 4/5's session table
    // (net/web_auth_session.h) and its http_session_table() owner
    // (http_session_iface.h) have both since landed -- this no longer stays
    // a documented placeholder. security_http_core.c calls this vtable slot
    // from THREE call sites sharing one security_role_t parameter: an admin
    // web password change, a user web password change, and an LCD PIN
    // change (section 6: "changing a password invalidates every session for
    // that role" / "changing the LCD PIN drops the LCD session") -- the role
    // value alone does not say which credential changed, and the LCD has
    // exactly one session, not a slot per role (item 4), so it cannot be
    // selectively targeted by role the way the web table can. Rather than
    // widen this vtable slot's signature to carry that distinction (a larger
    // change than this review's four items call for), tear down BOTH: the
    // matching role's web sessions, and the single LCD session outright.
    // This is a safe superset for the two web-password call sites (an admin
    // password change forcing a re-tap of an already-open LCD session is not
    // a correctness problem, only slightly more conservative than strictly
    // necessary) and is exactly correct for the LCD-PIN call site, which
    // otherwise had no consumer for its own invalidation at all.
    web_auth_session_role_t session_role = (role == SECURITY_ROLE_ADMIN) ? WEB_AUTH_SESSION_ROLE_ADMIN
                                                                          : WEB_AUTH_SESSION_ROLE_USER;
    web_auth_table_destroy_role(http_session_table(), session_role);
    ui_lcd_lock_force_lock();
    ESP_LOGI(TAG, "invalidate_sessions_for_role(role=%d): web sessions for that role and the LCD "
                  "session torn down",
             (int)role);
}

static security_err_t web_auth_backend_clear_all_credentials(void)
{
    // Item 12b's "Clear login credentials" ADMIN action. Thin wrapper: all
    // the actual clearing (both roles, both blobs, read-back verified) is
    // web_auth_store_clear_all_credentials()'s job; this file only maps its
    // bool result onto this seam's security_err_t and logs the event at the
    // same loud level the physical-reset gesture and boot_guard's own
    // "never trust a write's return code alone" lesson use elsewhere in
    // this tree -- a rare, security-relevant event that must be findable
    // afterwards.
    bool ok = web_auth_store_clear_all_credentials();
    if (!ok) {
        ESP_LOGE(TAG, "clear_all_credentials() FAILED -- one or more records could not be confirmed cleared "
                      "by read-back; credentials may be left in a partially-cleared state");
        return SECURITY_ERR_STORAGE;
    }
    ESP_LOGW(TAG, "clear_all_credentials(): administrator and user web passwords and LCD PINs cleared "
                  "via the authenticated /settings/security route (WEB_AUTH_PLAN.md item 12b)");
    return SECURITY_OK;
}

static const security_backend_vtable_t s_web_auth_vtable = {
    .set_web_password = web_auth_backend_set_web_password,
    .set_lcd_pin = web_auth_backend_set_lcd_pin,
    .set_policy = web_auth_backend_set_policy,
    .get_config = web_auth_backend_get_config,
    .invalidate_sessions_for_role = web_auth_backend_invalidate_sessions_for_role,
    .clear_all_credentials = web_auth_backend_clear_all_credentials,
};

void security_backend_web_auth_install(void)
{
    security_backend_set_vtable(&s_web_auth_vtable);
}

// POST /api/auth/bootstrap_password -- the one consumer of
// web_auth_admin_bootstrap_needed() (net/web_auth_session.h). The REAL gate
// is the enforcement pre-handler: this route is classified
// ROUTE_TIER_ADMIN_BOOTSTRAP (route_tier_table.h), and http_auth_check()
// (http_auth_enforce.c) denies it outright once bootstrap_needed is false --
// never a hardcoded URI match, that decision already lives in the one place
// it belongs. The re-check below is defence in depth only (a second,
// independent gate against the same real predicate, not a re-derivation of
// it -- both calls resolve through web_auth_admin_bootstrap_needed()
// itself), in case this handler is ever reached by a path that bypasses
// kiln_http_register()'s wrapper.
//
// Body is form-urlencoded (this codebase's convention, see http_form.h),
// capped well under this task's stack-local budget. Reuses
// web_auth_backend_set_web_password() directly -- the same strength check,
// username validation, random salt and must_change=false write an ordinary
// owner-initiated password change gets, since a bootstrap-set password is
// not a "forced default", it is the operator's own first real credential
// and must satisfy exactly the same strength rule as any other. Never
// invents or writes a placeholder/default password anywhere.
#define AUTH_BOOTSTRAP_BODY_MAX 512

static esp_err_t auth_bootstrap_password_post_handler(httpd_req_t *req)
{
    // Defence in depth (see comment above) -- re-resolves through the same
    // predicate the enforcement pre-handler already gated on, never a
    // separate/looser check.
    bool effective_enabled = http_auth_policy_web_enabled();
    bool admin_configured = web_auth_store_password_configured(WEB_AUTH_ROLE_ADMINISTRATOR);
    if (!web_auth_admin_bootstrap_needed(effective_enabled, admin_configured)) {
        // No HTTPD_409_CONFLICT in esp_http_server.h's httpd_err_code_t --
        // set the status line directly, same convention
        // kiln_http_prehandler()'s own 500 branch (http_auth_http.c) uses.
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_send(req, "administrator credential already configured", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (req->content_len <= 0 || req->content_len >= AUTH_BOOTSTRAP_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }

    char body[AUTH_BOOTSTRAP_BODY_MAX];
    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "failed to read body");
        return ESP_OK;
    }
    body[received] = '\0';

    char username[SECURITY_HTTP_USERNAME_MAX + 1];
    char password[SECURITY_HTTP_PASSWORD_MAX + 1];
    int username_len = http_form_find_field(body, "username", username, sizeof(username));
    int password_len = http_form_find_field(body, "password", password, sizeof(password));
    if (username_len < 0 || password_len < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "username and password are required");
        return ESP_OK;
    }

    security_err_t result = web_auth_backend_set_web_password(SECURITY_ROLE_ADMIN, username, password);
    switch (result) {
        case SECURITY_OK:
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req, "{\"ok\":true}");
        case SECURITY_ERR_WEAK:
        case SECURITY_ERR_INVALID_INPUT:
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "password rejected");
            return ESP_OK;
        default:
            ESP_LOGE(TAG, "bootstrap_password: set_web_password failed, err=%d", (int)result);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to set password");
            return ESP_OK;
    }
}

esp_err_t security_backend_web_auth_start(void)
{
    security_backend_web_auth_install();

    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t bootstrap_uri = {
        .uri = "/api/auth/bootstrap_password",
        .method = HTTP_POST,
        .handler = auth_bootstrap_password_post_handler,
    };

    esp_err_t err = kiln_http_register(server, &bootstrap_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/auth/bootstrap_password) failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "web auth backend installed, bootstrap route up");
    return ESP_OK;
}
