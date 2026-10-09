// security_http.c -- ESP glue for WEB_AUTH_PLAN.md section 6's password
// page. Owns only header/body parsing and response sending; every real
// decision (role gating, field validation, the call-through to
// security_backend_vtable_t) lives in security_http_core.c's
// security_http_dispatch(), which is host-tested there. See security_http.h
// and net/security_page.html's own wire-contract comment for the exact
// field names this file must match.
#include "security_http.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"

#include "auth_totp_http.h"  // auth_totp_http_clear_reset_tokens()
#include "hal_sysinfo.h"     // hal_sysinfo_fill_random()
#include "hal_time.h"        // hal_time_now_ms()
#include "http_auth_http.h"  // kiln_http_register(), http_auth_caller_is_admin()
#include "http_auth_policy_iface.h" // http_auth_policy_web_enabled()
#include "http_form.h"
#include "security_backend.h"
#include "security_http_core.h"
#include "time_sync.h"       // time_sync_get_status()
#include "totp.h"            // net/totp.h -- on the include path via "net"
#include "totp_config.h"     // persist/totp_config.h -- via "persist"
#include "totp_http_core.h"
#include "web_auth_store.h"
#include "web_encoding.h"
#include "wifi_provision_http.h"

// docs/TOTP_PASSWORD_RESET_PLAN.md section 6b (2026-09-24, WT-B's chosen
// shape): enrollment/disable ride this route's existing cmd= dispatch
// rather than three new routes -- see this file's POST handler comment.
// The RAM-only pending secret is a single-board, single-administrator
// singleton, same lifetime rule as auth_totp_http.c's reset-token table.
static totp_pending_enrollment_t s_totp_pending;

static uint32_t security_http_now_ms(void)
{
    return (uint32_t)hal_time_now_ms();
}

void security_totp_pending_clear(void)
{
    totp_pending_clear(&s_totp_pending);
}

static const char *TAG = "security_http";

/* Embedded via EMBED_TXTFILES, pre-gzipped at configure time -- same
 * convention as every other *_page.html in this component (see
 * settings_http.c's send_gz_page() for the pattern this mirrors). */
extern const uint8_t security_page_html_gz_start[] asm("_binary_security_page_html_gz_start");
extern const uint8_t security_page_html_gz_end[] asm("_binary_security_page_html_gz_end");

static esp_err_t send_gz_page(httpd_req_t *req, const char *page_name, const uint8_t *start, const uint8_t *end)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, TAG, page_name);
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)start, (size_t)(end - start));
}

// GET /settings/security -- the page shell.
static esp_err_t security_page_get_handler(httpd_req_t *req)
{
    return send_gz_page(req, "security_page.html", security_page_html_gz_start, security_page_html_gz_end);
}

// GET /api/auth/config -- read-only status. Field names/shape are fixed by
// net/security_page.html's wire-contract comment; do not rename without
// updating that page too. get_config() returning false means "no
// policy/credential exists yet" (item 11) -- not an error, rendered here as
// an all-defaults/unconfigured snapshot.
static esp_err_t security_config_get_handler(httpd_req_t *req)
{
    security_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (!security_backend_get_vtable()->get_config(&cfg)) {
        cfg.web_enabled = false;
        cfg.lcd_enabled = false;
        cfg.web_timeout_min = -1;
        cfg.lcd_timeout_min = -1;
        cfg.admin_username[0] = '\0';
    }

    // admin_username is validated on the way in (security_http_core.c's
    // username acceptance mirrors web_auth_store.h's printable/non-space
    // rule) but is still escaped defensively here since it is echoed back
    // verbatim into a JSON string value.
    char escaped_user[sizeof(cfg.admin_username) * 2];
    size_t o = 0;
    for (size_t i = 0; cfg.admin_username[i] != '\0' && o + 2 < sizeof(escaped_user); i++) {
        char c = cfg.admin_username[i];
        if (c == '"' || c == '\\') {
            escaped_user[o++] = '\\';
        }
        escaped_user[o++] = c;
    }
    escaped_user[o] = '\0';

    char json[384];
    int n = snprintf(json, sizeof(json),
                     "{\"web_enabled\":%s,\"lcd_enabled\":%s,"
                     "\"web_timeout_min\":%d,\"lcd_timeout_min\":%d,"
                     "\"admin_username\":\"%s\","
                     "\"admin_password_set\":%s,\"user_password_set\":%s,"
                     "\"user_pin_set\":%s,\"admin_pin_set\":%s}",
                     cfg.web_enabled ? "true" : "false", cfg.lcd_enabled ? "true" : "false",
                     cfg.web_timeout_min, cfg.lcd_timeout_min, escaped_user,
                     cfg.admin_password_set ? "true" : "false", cfg.user_password_set ? "true" : "false",
                     cfg.lcd_user_pin_set ? "true" : "false", cfg.lcd_admin_pin_set ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof(json)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "response too large");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

// POST /api/auth/security -- one action per call, per the wire contract:
//   cmd=set_web_password&role=admin|user&username=...&password=...
//   cmd=set_lcd_pin&role=admin|user&pin=...
//   cmd=set_policy&web_enabled=0|1&lcd_enabled=0|1&web_timeout_min=N&lcd_timeout_min=N
//   cmd=clear_credentials (item 12b -- no other fields)
//   cmd=totp_enroll_begin (docs/TOTP_PASSWORD_RESET_PLAN.md section 6b) --
//     returns {"ok":true,"secret_base32","otpauth_uri","board_time_utc",
//     "sntp_synced"}. 2026-09-25: refused 409
//     {"ok":false,"web_auth_disabled":true} while web login is off.
//   cmd=totp_enroll_confirm&code=NNNNNN -- returns {"ok"} only. Same 409
//     web_auth_disabled refusal as totp_enroll_begin above.
//   cmd=totp_disable&code=NNNNNN -- requires a currently-valid code, not
//     merely the ADMIN session already required by this route; {"ok"} only.
//     NOT gated on web auth being on -- must keep working regardless, so an
//     enrollment left over from before web auth was turned off can still be
//     removed.
//
// The three totp_* commands are handled directly below, before the
// set_web_password/etc dispatch machinery, and return early -- they do not
// go through security_http_dispatch()/security_backend_vtable_t (no backend
// seam exists for a TOTP secret; it lives in persist/totp_config.h
// directly). All three answer through the same generic {"ok":bool} JSON
// shape as security_http_dispatch()'s other commands (transport always 200,
// same as that dispatch path -- see security_post_handler()'s own comment),
// whether the underlying failure is a bad code, no pending enrollment, or a
// clock issue (reported as {"ok":false,"clock_unsynced":true}, never a raised
// 503, since this route's page-side JS only reads the JSON body's `ok`).
// Plan section 6b's enumeration-safety note: this surface is ADMIN-gated
// already, so the concern is a stolen-session attacker brute-forcing
// disable, not an anonymous oracle.
// Bounded-body-then-parse-then-dispatch, same shape as settings_http.c's
// POST handlers. This route is ROUTE_TIER_ADMIN so kiln_http_register()'s
// pre-handler has already refused a non-admin caller before this body runs;
// http_auth_caller_is_admin() is still consulted here and passed into
// security_http_dispatch(), which repeats the ADMIN-only check on its own
// terms (defence in depth, see that function's own comment).
#define SECURITY_HTTP_BODY_MAX 512

static bool parse_role(const char *body, security_role_t *out)
{
    char role_val[8];
    int n = http_form_find_field(body, "role", role_val, sizeof(role_val));
    if (n <= 0) {
        return false;
    }
    if (strcmp(role_val, "admin") == 0) {
        *out = SECURITY_ROLE_ADMIN;
        return true;
    }
    if (strcmp(role_val, "user") == 0) {
        *out = SECURITY_ROLE_USER;
        return true;
    }
    return false;
}

static int parse_int_field(const char *body, const char *key, int *out)
{
    char val[16];
    int n = http_form_find_field(body, key, val, sizeof(val));
    if (n <= 0) {
        return n; // -1 missing, -2 too long, 0 empty
    }
    long v = 0;
    if (!http_form_parse_long(val, n, -2147483647L, 2147483647L, &v)) {
        return -2;
    }
    *out = (int)v;
    return n;
}

// --- cmd=totp_enroll_begin / totp_enroll_confirm / totp_disable -------------
//
// CLOCK-BEFORE-ANYTHING (plan section 6a's rule for /api/auth/forgot and
// /api/auth/reset, applied here too, but through THIS route's own always-200
// cmd= transport per section 6b -- not those two OPEN routes' raw 503, since
// this route's page-side fetch().then(r => r.json()) already only inspects
// the JSON body's `ok`, never r.status, same as every other cmd= outcome
// dispatched below): a clock-not-ready check here returns 200 with
// {"ok":false,"clock_unsynced":true} instead of raising a transport error.
// Checked before touching the pending secret, NVS, or the request body.
//
// STACK: all locals here are small fixed buffers (well under the httpd 8 KB
// stack blob class this codebase watches for), same convention as
// auth_totp_http.c's forgot/reset handlers.

static esp_err_t security_send_totp_clock_unsynced(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"clock_unsynced\":true}");
}

// Enrollment is only ever for a board with NO secret stored. An already-
// enrolled board must go through cmd=totp_disable (which demands a valid
// code from the CURRENT secret) first -- otherwise begin+confirm would
// silently replace the secret using a code from the new one, letting a
// hijacked admin session alone swap the owner's reset factor for its own,
// exactly what section 6b's "disable requires a valid code" rule forbids.
// UNREADABLE refuses too (fail closed), same as ENROLLED.
static bool security_totp_not_enrolled(void)
{
    uint8_t scratch[TOTP_SECRET_LEN];
    totp_config_load_status_t st = totp_config_load_secret(scratch);
    totp_secure_zero(scratch, sizeof(scratch));
    return st == TOTP_CONFIG_LOAD_ABSENT;
}

static esp_err_t security_send_totp_refused(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":false}");
}

// 2026-09-25 owner decision: enrolling a TOTP second factor only makes
// sense once web login itself is turned on -- an authenticator enrolled
// while web auth is off would recover a password nobody is being asked for
// yet, and worse, would silently survive to gate a LATER enable with a
// factor the operator may not have meant to keep. Refused with a DISTINCT,
// machine-readable reason (409 + `web_auth_disabled`) rather than the
// generic `{"ok":false}` every other enrollment refusal uses, so the
// settings page can show an actionable message instead of a bare failure.
// Only the two enrollment routes are gated this way -- cmd=totp_disable
// must keep working regardless of the web-auth toggle, since disabling an
// existing enrollment (e.g. one left over from before web auth was turned
// off) is never something this gate should block.
static esp_err_t security_send_totp_web_auth_off(httpd_req_t *req)
{
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"web_auth_disabled\":true}");
}

static esp_err_t security_totp_enroll_begin(httpd_req_t *req)
{
    time_sync_status_t st;
    time_sync_get_status(&st);
    if (!totp_http_clock_ready(st.ever_synced)) {
        return security_send_totp_clock_unsynced(req);
    }
    if (!security_totp_not_enrolled()) {
        return security_send_totp_refused(req);
    }

    uint8_t secret[TOTP_SECRET_LEN];
    hal_sysinfo_fill_random(secret, sizeof(secret));
    totp_pending_begin(&s_totp_pending, secret, security_http_now_ms());
    totp_secure_zero(secret, sizeof(secret));

    web_auth_password_record_t admin_record;
    const char *username = "administrator";
    if (web_auth_store_load_password(WEB_AUTH_ROLE_ADMINISTRATOR, &admin_record) == WEB_AUTH_LOAD_OK &&
        admin_record.configured && admin_record.username[0] != '\0') {
        username = admin_record.username;
    }

    char uri[192];
    size_t uri_len = totp_build_otpauth_uri(username, s_totp_pending.secret, TOTP_SECRET_LEN, uri, sizeof(uri));
    totp_secure_zero(&admin_record, sizeof(admin_record)); // username already copied into uri
    char base32[64];
    size_t base32_len = (uri_len != 0) ? totp_base32_encode(s_totp_pending.secret, TOTP_SECRET_LEN, base32, sizeof(base32)) : 0;
    if (uri_len == 0 || base32_len == 0) {
        totp_pending_clear(&s_totp_pending);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "totp enrollment secret build failed");
        return ESP_OK;
    }

    char body[320];
    int n = snprintf(body, sizeof(body),
                      "{\"ok\":true,\"secret_base32\":\"%s\",\"otpauth_uri\":\"%s\",\"board_time_utc\":%lld,\"sntp_synced\":%s}",
                      base32, uri, (long long)st.now_epoch, st.ever_synced ? "true" : "false");
    totp_secure_zero(uri, sizeof(uri));
    totp_secure_zero(base32, sizeof(base32));
    if (n < 0 || (size_t)n >= sizeof(body)) {
        totp_pending_clear(&s_totp_pending);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "response too large");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, body);
    totp_secure_zero(body, sizeof(body));
    return ret;
}

static esp_err_t security_totp_enroll_confirm(httpd_req_t *req, const char *body)
{
    time_sync_status_t st;
    time_sync_get_status(&st);
    if (!totp_http_clock_ready(st.ever_synced)) {
        return security_send_totp_clock_unsynced(req);
    }
    if (!security_totp_not_enrolled()) {
        totp_pending_clear(&s_totp_pending);
        return security_send_totp_refused(req);
    }
    if (!totp_pending_is_valid(&s_totp_pending, security_http_now_ms())) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    char code[8];
    if (http_form_find_field(body, "code", code, sizeof(code)) < 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    uint64_t matched_counter = 0;
    bool ok = totp_verify(s_totp_pending.secret, TOTP_SECRET_LEN, code, (uint64_t)st.now_epoch, 0, &matched_counter);
    totp_secure_zero(code, sizeof(code));
    if (!ok) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    bool persisted =
        totp_config_set_secret(s_totp_pending.secret) && totp_config_set_last_counter((uint32_t)matched_counter);
    totp_pending_clear(&s_totp_pending);
    if (!persisted) {
        totp_config_clear();
    } else {
        // A reset token minted against a PRIOR enrollment (or before any
        // enrollment existed, via the anti-oracle path) must not survive to
        // be consumed against this new one -- /api/auth/reset's own re-check
        // only asks "is TOTP enrolled", not "is it the SAME enrollment".
        auth_totp_http_clear_reset_tokens();
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, persisted ? "{\"ok\":true}" : "{\"ok\":false}");
}

static esp_err_t security_totp_disable(httpd_req_t *req, const char *body)
{
    time_sync_status_t st;
    time_sync_get_status(&st);
    if (!totp_http_clock_ready(st.ever_synced)) {
        return security_send_totp_clock_unsynced(req);
    }
    char code[8];
    if (http_form_find_field(body, "code", code, sizeof(code)) < 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    // Requires a currently-valid TOTP code, not merely the ADMIN session
    // already required by this route (plan section 6b) -- a hijacked web
    // session alone must not be able to remove a locked-out owner's only
    // non-admin-session recovery path.
    totp_consume_result_t r = totp_config_verify_and_consume(code, (uint64_t)st.now_epoch);
    totp_secure_zero(code, sizeof(code));
    if (r != TOTP_CONSUME_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    totp_pending_clear(&s_totp_pending); // hygiene: never leave a stale pending secret behind either
    bool cleared = totp_config_clear();
    if (cleared) {
        // Same reset-token hazard as security_totp_enroll_confirm() above:
        // a token minted before this disable must not still set the admin
        // password afterward -- clear the table so nothing outstanding
        // survives the enrollment it was minted against.
        auth_totp_http_clear_reset_tokens();
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, cleared ? "{\"ok\":true}" : "{\"ok\":false}");
}

static esp_err_t security_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > SECURITY_HTTP_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }

    char body[SECURITY_HTTP_BODY_MAX + 1];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    char cmd_val[24];
    int cmd_len = http_form_find_field(body, "cmd", cmd_val, sizeof(cmd_val));
    if (cmd_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "cmd is required");
        return ESP_OK;
    }

    bool totp_cmd = strcmp(cmd_val, "totp_enroll_begin") == 0 || strcmp(cmd_val, "totp_enroll_confirm") == 0 ||
                    strcmp(cmd_val, "totp_disable") == 0;
    if (totp_cmd && !http_auth_caller_is_admin(req)) {
        // Defence in depth, same as the security_http_dispatch() path below:
        // route_tier_table.h already gates this route ADMIN, but the TOTP
        // commands bypass that dispatcher's own caller_role check, so make
        // it here explicitly rather than rely on the tier gate alone.
        return security_send_totp_refused(req);
    }
    bool totp_enroll_cmd =
        strcmp(cmd_val, "totp_enroll_begin") == 0 || strcmp(cmd_val, "totp_enroll_confirm") == 0;
    if (totp_enroll_cmd && !totp_enroll_allowed(http_auth_policy_web_enabled())) {
        return security_send_totp_web_auth_off(req);
    }
    if (strcmp(cmd_val, "totp_enroll_begin") == 0) {
        return security_totp_enroll_begin(req);
    }
    if (strcmp(cmd_val, "totp_enroll_confirm") == 0) {
        return security_totp_enroll_confirm(req, body);
    }
    if (strcmp(cmd_val, "totp_disable") == 0) {
        return security_totp_disable(req, body);
    }

    security_request_t sreq;
    memset(&sreq, 0, sizeof(sreq));

    if (strcmp(cmd_val, "set_web_password") == 0) {
        security_role_t role;
        if (!parse_role(body, &role)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "role must be admin or user");
            return ESP_OK;
        }
        sreq.cmd = (role == SECURITY_ROLE_ADMIN) ? SECURITY_CMD_SET_ADMIN_PASSWORD : SECURITY_CMD_SET_USER_PASSWORD;
        if (role == SECURITY_ROLE_ADMIN) {
            int un = http_form_find_field(body, "username", sreq.username, sizeof(sreq.username));
            if (un <= 0) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "username is required for the admin record");
                return ESP_OK;
            }
        }
        int pw = http_form_find_field(body, "password", sreq.password, sizeof(sreq.password));
        if (pw <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "password is required");
            return ESP_OK;
        }
    } else if (strcmp(cmd_val, "set_lcd_pin") == 0) {
        security_role_t role;
        if (!parse_role(body, &role)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "role must be admin or user");
            return ESP_OK;
        }
        sreq.cmd = SECURITY_CMD_SET_LCD_PIN;
        sreq.lcd_pin_role = role;
        int pn = http_form_find_field(body, "pin", sreq.lcd_pin, sizeof(sreq.lcd_pin));
        if (pn <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "pin is required");
            return ESP_OK;
        }
        // The other role's current PIN is never exposed by this backend
        // (item 2's records are one-way hashed) -- security_http_core.c's
        // security_pin_is_valid() treats an empty lcd_pin_other as "not
        // supplied", never blocking the change on that ground alone.
        sreq.lcd_pin_other[0] = '\0';
    } else if (strcmp(cmd_val, "set_policy") == 0) {
        sreq.cmd = SECURITY_CMD_SET_POLICY;
        int web_en = 0, lcd_en = 0;
        char flag_val[4];
        int n = http_form_find_field(body, "web_enabled", flag_val, sizeof(flag_val));
        if (!http_form_is_bool01(flag_val, n)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "web_enabled must be 0 or 1");
            return ESP_OK;
        }
        web_en = (flag_val[0] == '1');
        n = http_form_find_field(body, "lcd_enabled", flag_val, sizeof(flag_val));
        if (!http_form_is_bool01(flag_val, n)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "lcd_enabled must be 0 or 1");
            return ESP_OK;
        }
        lcd_en = (flag_val[0] == '1');
        int web_timeout = -1, lcd_timeout = -1;
        if (parse_int_field(body, "web_timeout_min", &web_timeout) <= 0 ||
            parse_int_field(body, "lcd_timeout_min", &lcd_timeout) <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "web_timeout_min and lcd_timeout_min are required");
            return ESP_OK;
        }
        sreq.policy.web_enabled = web_en;
        sreq.policy.lcd_enabled = lcd_en;
        sreq.policy.web_timeout_min = web_timeout;
        sreq.policy.lcd_timeout_min = lcd_timeout;
    } else if (strcmp(cmd_val, "clear_credentials") == 0) {
        // Item 12b's "Clear login credentials" action. No fields beyond
        // cmd itself.
        sreq.cmd = SECURITY_CMD_CLEAR_CREDENTIALS;
    } else {
        sreq.cmd = SECURITY_CMD_UNKNOWN;
    }

    security_role_t caller_role = http_auth_caller_is_admin(req) ? SECURITY_ROLE_ADMIN : SECURITY_ROLE_USER;
    security_result_t result;
    memset(&result, 0, sizeof(result));
    security_http_dispatch(security_backend_get_vtable(), caller_role, &sreq, &result);

    if (result.http_status == 200 && result.invalidated_sessions) {
        security_backend_get_vtable()->invalidate_sessions_for_role(result.invalidated_role);
    }

    char resp[SECURITY_HTTP_MESSAGE_MAX + 32];
    int n;
    if (result.http_status == 200) {
        n = snprintf(resp, sizeof(resp), "{\"ok\":true}");
    } else {
        // result.message is bounded (SECURITY_HTTP_MESSAGE_MAX) and comes
        // only from security_http_core.c's own fixed set of static strings
        // -- never caller-supplied text -- so no JSON-escaping is needed
        // here.
        n = snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"%s\"}", result.message);
    }
    if (n < 0 || (size_t)n >= sizeof(resp)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "response too large");
        return ESP_OK;
    }

    // The dispatch result's HTTP status is reported inside the JSON body's
    // ok/error shape the page's fetch().then(r => r.json()) already expects
    // (see net/security_page.html's fetch handlers, which check d.ok, not
    // r.status) -- the transport status stays 200 for every outcome here,
    // same as settings_http.c's POST handlers.
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

esp_err_t security_http_start(void)
{
    httpd_handle_t server = wifi_provision_http_get_server();
    if (server == NULL) {
        ESP_LOGE(TAG, "no httpd server available");
        return ESP_FAIL;
    }

    static const httpd_uri_t page_uri = {
        .uri = "/settings/security", .method = HTTP_GET, .handler = security_page_get_handler,
    };
    static const httpd_uri_t config_uri = {
        .uri = "/api/auth/config", .method = HTTP_GET, .handler = security_config_get_handler,
    };
    static const httpd_uri_t post_uri = {
        .uri = "/api/auth/security", .method = HTTP_POST, .handler = security_post_handler,
    };

    esp_err_t err = kiln_http_register(server, &page_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/settings/security) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = kiln_http_register(server, &config_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(GET /api/auth/config) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = kiln_http_register(server, &post_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST /api/auth/security) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "security/password page up");
    return ESP_OK;
}
