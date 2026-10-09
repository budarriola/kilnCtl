// auth_totp_http.c -- TOTP status (ADMIN, its own GET route) and
// password-reset (OPEN, 2 routes) HTTP routes.
// docs/TOTP_PASSWORD_RESET_PLAN.md WT-A part 2, reworked per section 6b
// (2026-09-24, WT-B's chosen shape): TOTP enrollment (begin/confirm) and
// disable are NOT their own routes -- they ride the existing
// POST /api/auth/security cmd= dispatch (security_http.c's
// cmd=totp_enroll_begin / cmd=totp_enroll_confirm&code=NNNNNN /
// cmd=totp_disable&code=NNNNNN), to spend zero new httpd_uri_t slots on
// them while check_uri_handler_cap.ps1 was down to single-digit headroom.
// This file therefore registers only 3 new routes total: GET
// /api/auth/totp_status, POST /api/auth/forgot, POST /api/auth/reset.
//
// Pure token-lifecycle/pending-secret logic lives in totp_http_core.h/.c
// (host-tested); this file is the ESP-only httpd glue around it plus the
// existing WT-A core (net/totp.h, persist/totp_config.h) -- see CLAUDE.md's
// "HTTP handlers are target-build only" note for why the split exists. The
// RAM-only pending-enrollment secret (totp_pending_enrollment_t) is owned
// by security_http.c now, since totp_enroll_begin/confirm live there --
// see that file for the enrollment/disable handlers.
//
// LOCKOUT: /api/auth/forgot and /api/auth/reset share ONE escalating per-IP
// backoff ladder (own instance, same shape as web_auth_login_http.c's --
// WEB_AUTH_PLAN.md section 7's "each authentication surface must not share
// lockout state with any other" -- these two routes are one surface, the
// password-reset flow, so they DO share state with each other, just not
// with /api/auth/login or any other surface). Same REMOTE-subnet pooling
// rule as the login route (owner decision 2026-09-20, login_ip_scope.h).
//
// 503-BEFORE-ANYTHING ORDERING (plan section 6a): both routes check
// totp_http_clock_ready() FIRST, before the lockout table, before reading
// the body, before anything else -- an unsynced clock makes TOTP
// verification meaningless, so nothing downstream may run ahead of that
// check. See totp_http_core.h.
//
// ANTI-ORACLE (plan section 6a): /api/auth/forgot always returns 202 with a
// token-shaped value, whether or not the submitted code was ever real, and
// takes the same code path either way (a token is always stored, real if
// verification succeeded, otherwise generated but never bound to anything a
// consume attempt could match against). /api/auth/reset always answers 400
// generically on any failure -- wrong token, expired token, used token,
// unknown username, or a rejected new password are not distinguished in the
// response body.
//
// BOARD-WIDE FAILED-ATTEMPT CAP (plan section 4, "recommended... defense in
// depth"): a RAM-only, per-boot counter of failed /api/auth/forgot
// verification attempts (wrong code, wrong username, not enrolled). Once it
// reaches TOTP_FORGOT_BOARD_CAP, further /api/auth/forgot calls are refused
// 429 regardless of per-IP state, until the next reboot -- a successful
// verification does NOT reset it (see totp_http_core.h for why). This is
// additive to the per-IP ladder, not a replacement for it.
//
// STACK: no locals here approach the httpd 8 KB stack blob class this
// codebase watches for -- every JSON body emitted is a handful of fields
// (well under 300 bytes), built into a small fixed stack buffer with
// snprintf and sent in one httpd_resp_sendstr()/httpd_resp_send() call,
// same as web_auth_login_http.c's own {"ok":true}/cookie buffers. None of
// these responses are large or dynamically-sized enough to need real
// chunked emission (no reference to that pattern exists elsewhere in this
// component's HTTP handlers as of this writing).
#include "http_auth_http.h" // kiln_http_register()

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "hal_sysinfo.h" // hal_sysinfo_fill_random()
#include "hal_time.h"    // hal_time_now_ms()
#include "http_form.h"   // http_form_find_field()
#include "http_session_iface.h" // http_session_table()
#include "login_ip_scope.h"     // login_ip_scope_classify()
#include "ota_http_util.h"      // ota_http_hex_encode()
#include "security_backend.h"   // security_backend_get_vtable()
#include "security_http_core.h" // SECURITY_HTTP_USERNAME_MAX/PASSWORD_MAX
#include "time_sync.h"           // time_sync_get_status()
#include "totp.h"                // net/totp.h -- on the include path via "net"
#include "totp_config.h"         // persist/totp_config.h -- via "persist"
#include "totp_http_core.h"
#include "web_auth_login.h"  // web_auth_login_role_for_username()
#include "web_auth_session.h" // web_auth_table_destroy_all()
#include "web_auth_store.h"
#include "wifi_prov.h"           // wifi_prov_get_cached_sta_ip_netmask()
#include "wifi_provision_http.h" // wifi_provision_http_get_server()

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "auth_totp_http";

bool ota_http_get_client_ip_checked(httpd_req_t *req, char *out, size_t out_len);

static uint32_t now_ms(void)
{
    return (uint32_t)hal_time_now_ms();
}

// --- SNTP-sync gate (plan section 6a) --------------------------------------

static bool sntp_ready(time_sync_status_t *out_status)
{
    time_sync_get_status(out_status);
    return totp_http_clock_ready(out_status->ever_synced);
}

static esp_err_t send_service_unavailable_clock(httpd_req_t *req)
{
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "board clock not synced yet");
}

// --- Reset-token table -------------------------------------------------------

static totp_reset_token_table_t s_reset_tokens;
static bool s_reset_tokens_init_done;

static totp_reset_token_table_t *reset_tokens(void)
{
    if (!s_reset_tokens_init_done) {
        totp_reset_token_table_init(&s_reset_tokens);
        s_reset_tokens_init_done = true;
    }
    return &s_reset_tokens;
}

// See auth_totp_http.h: called by security_http.c after a successful
// totp_enroll_confirm or totp_disable so a reset token minted against a
// prior enrollment can never survive to be consumed against a new or
// disabled one (a "same-name, different-enrollment" reset-one-side-of-a-
// pair gap; CLAUDE.md).
void auth_totp_http_clear_reset_tokens(void)
{
    totp_reset_token_table_init(reset_tokens());
}

// --- Board-wide failed-forgot-attempt cap (plan section 4) ------------------

// TOTP_FORGOT_BOARD_CAP and the blocked/record logic live in
// totp_http_core.h (host-tested); cleared only by a reboot.
static totp_forgot_board_cap_t s_forgot_board_cap;

// --- Shared per-IP backoff ladder for /api/auth/forgot + /api/auth/reset
// (own instance, same shape as web_auth_login_http.c's -- see this file's
// header comment) -----------------------------------------------------------

static const uint32_t TOTP_BACKOFF_LADDER_MS[] = { 5000u, 10000u, 30000u, 60000u, 300000u };
#define TOTP_BACKOFF_LADDER_LEN (sizeof(TOTP_BACKOFF_LADDER_MS) / sizeof(TOTP_BACKOFF_LADDER_MS[0]))
_Static_assert(TOTP_BACKOFF_LADDER_LEN == 5, "totp reset backoff ladder must have exactly 5 steps");

#define TOTP_LOCKOUT_MAX_IPS 16u

typedef struct {
    uint32_t failure_count;
    uint32_t locked_until_ms;
} totp_backoff_state_t;

static bool totp_backoff_is_locked(const totp_backoff_state_t *s, uint32_t now)
{
    if (s->locked_until_ms == 0u) {
        return false;
    }
    return (int32_t)(s->locked_until_ms - now) > 0;
}

static void totp_backoff_cycle_reset_if_due(totp_backoff_state_t *s, uint32_t now)
{
    if (s->failure_count >= TOTP_BACKOFF_LADDER_LEN && !totp_backoff_is_locked(s, now)) {
        s->failure_count = 0;
        s->locked_until_ms = 0;
    }
}

static void totp_backoff_record_failure(totp_backoff_state_t *s, uint32_t now)
{
    if (s->failure_count < TOTP_BACKOFF_LADDER_LEN) {
        s->failure_count++;
    }
    s->locked_until_ms = now + TOTP_BACKOFF_LADDER_MS[s->failure_count - 1];
}

static void totp_backoff_record_success(totp_backoff_state_t *s)
{
    s->failure_count = 0;
    s->locked_until_ms = 0;
}

typedef struct {
    bool in_use;
    char ip[46];
    uint32_t last_activity_ms;
    totp_backoff_state_t backoff;
} totp_lockout_slot_t;
static totp_lockout_slot_t s_totp_lockouts[TOTP_LOCKOUT_MAX_IPS];
static totp_lockout_slot_t s_remote_totp_slot;
static SemaphoreHandle_t s_totp_lock;

static totp_lockout_slot_t *totp_lockout_slot_for(const char *ip, uint32_t now)
{
    int free_idx = -1;
    int lru_idx = -1;
    uint32_t lru_time = UINT32_MAX;
    for (unsigned i = 0; i < TOTP_LOCKOUT_MAX_IPS; i++) {
        totp_lockout_slot_t *s = &s_totp_lockouts[i];
        totp_backoff_cycle_reset_if_due(&s->backoff, now);
        if (s->in_use && strcmp(s->ip, ip) == 0) {
            s->last_activity_ms = now;
            return s;
        }
        if (!s->in_use && free_idx < 0) {
            free_idx = (int)i;
        }
        if (s->in_use && !totp_backoff_is_locked(&s->backoff, now) && s->last_activity_ms < lru_time) {
            lru_time = s->last_activity_ms;
            lru_idx = (int)i;
        }
    }
    int idx = (free_idx >= 0) ? free_idx : lru_idx;
    if (idx < 0) {
        return NULL;
    }
    totp_lockout_slot_t *s = &s_totp_lockouts[idx];
    memset(s, 0, sizeof(*s));
    s->in_use = true;
    strncpy(s->ip, ip, sizeof(s->ip) - 1);
    s->last_activity_ms = now;
    return s;
}

static totp_lockout_slot_t *claim_remote_totp_slot(uint32_t now)
{
    totp_backoff_cycle_reset_if_due(&s_remote_totp_slot.backoff, now);
    s_remote_totp_slot.in_use = true;
    strncpy(s_remote_totp_slot.ip, "*remote*", sizeof(s_remote_totp_slot.ip) - 1);
    s_remote_totp_slot.last_activity_ms = now;
    return &s_remote_totp_slot;
}

static totp_lockout_slot_t *totp_backoff_slot_for(const char *ip, bool ip_known, uint32_t now)
{
    if (ip_known) {
        char sta_ip[16] = { 0 };
        char sta_netmask[16] = { 0 };
        (void)wifi_prov_get_cached_sta_ip_netmask(sta_ip, sizeof(sta_ip), sta_netmask, sizeof(sta_netmask));
        login_ip_scope_t scope = login_ip_scope_classify(ip, sta_ip, sta_netmask);
        if (scope == LOGIN_IP_SCOPE_REMOTE || scope == LOGIN_IP_SCOPE_UNKNOWN) {
            return claim_remote_totp_slot(now);
        }
    }
    return totp_lockout_slot_for(ip, now);
}

// Returns true if the request should proceed; on false, has already sent the
// 429 response. `*out_locked_slot_touched` reports whether a slot was found
// to record success/failure against afterward.
static bool totp_backoff_gate(httpd_req_t *req, const char *ip, bool ip_known, bool *out_have_slot)
{
    *out_have_slot = false;
    bool locked = false;
    uint32_t retry_after_ms = 0;
    if (xSemaphoreTake(s_totp_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        uint32_t now = now_ms();
        totp_lockout_slot_t *slot = totp_backoff_slot_for(ip, ip_known, now);
        *out_have_slot = (slot != NULL);
        locked = (slot == NULL) || totp_backoff_is_locked(&slot->backoff, now);
        if (locked) {
            retry_after_ms = (slot != NULL) ? (slot->backoff.locked_until_ms - now)
                                             : TOTP_BACKOFF_LADDER_MS[TOTP_BACKOFF_LADDER_LEN - 1];
        }
        xSemaphoreGive(s_totp_lock);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
        return false;
    }
    if (locked) {
        uint32_t retry_after_s = (retry_after_ms == 0) ? 1u : (retry_after_ms + 999u) / 1000u;
        char hdr[16];
        snprintf(hdr, sizeof(hdr), "%u", (unsigned)retry_after_s);
        httpd_resp_set_hdr(req, "Retry-After", hdr);
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_send(req, "too many failed attempts, try again later", HTTPD_RESP_USE_STRLEN);
        return false;
    }
    return true;
}

static void totp_backoff_record(const char *ip, bool ip_known, bool ok)
{
    if (xSemaphoreTake(s_totp_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        uint32_t now = now_ms();
        totp_lockout_slot_t *slot = totp_backoff_slot_for(ip, ip_known, now);
        if (slot != NULL) {
            if (ok) {
                totp_backoff_record_success(&slot->backoff);
            } else {
                totp_backoff_record_failure(&slot->backoff, now);
            }
        }
        xSemaphoreGive(s_totp_lock);
    }
}

// --- ADMIN: GET /api/auth/totp_status ---------------------------------------

static esp_err_t totp_status_get_handler(httpd_req_t *req)
{
    time_sync_status_t st;
    time_sync_get_status(&st);
    char body[128];
    snprintf(body, sizeof(body), "{\"enrolled\":%s,\"board_time_utc\":%lld,\"sntp_synced\":%s}",
             totp_config_enrolled() ? "true" : "false", (long long)st.now_epoch,
             st.ever_synced ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

// --- OPEN: POST /api/auth/forgot --------------------------------------------

#define FORGOT_BODY_MAX 128

static esp_err_t forgot_post_handler(httpd_req_t *req)
{
    time_sync_status_t st;
    if (!sntp_ready(&st)) {
        return send_service_unavailable_clock(req);
    }

    char ip[46];
    bool ip_known = ota_http_get_client_ip_checked(req, ip, sizeof(ip));
    bool have_slot = false;
    if (!totp_backoff_gate(req, ip, ip_known, &have_slot)) {
        return ESP_OK;
    }

    if (totp_forgot_board_cap_blocked(&s_forgot_board_cap)) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_send(req, "too many failed attempts, try again later", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (req->content_len <= 0 || req->content_len >= FORGOT_BODY_MAX) {
        totp_backoff_record(ip, ip_known, false); /* malformed attempts count too (audit L44) */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }
    char body[FORGOT_BODY_MAX];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, (size_t)req->content_len - received);
        if (ret <= 0) {
            totp_backoff_record(ip, ip_known, false); /* malformed attempts count too (audit L44) */
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "failed to read body");
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    char username[SECURITY_HTTP_USERNAME_MAX + 1];
    char code[8];
    int username_len = http_form_find_field(body, "username", username, sizeof(username));
    int code_len = http_form_find_field(body, "code", code, sizeof(code));
    if (username_len < 0 || code_len < 0) {
        totp_backoff_record(ip, ip_known, false); /* malformed attempts count too (audit L44) */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "username and code are required");
        return ESP_OK;
    }

    // Resolve whether this claims to be the administrator, same seam
    // web_auth_login_http.c uses for the login route -- TOTP is only ever
    // enrolled for the administrator record.
    web_auth_password_record_t admin_record;
    const char *admin_username = NULL;
    if (web_auth_store_load_password(WEB_AUTH_ROLE_ADMINISTRATOR, &admin_record) == WEB_AUTH_LOAD_OK &&
        admin_record.configured) {
        admin_username = admin_record.username;
    }
    bool is_admin_claim = admin_username != NULL &&
                          web_auth_login_role_for_username(username, admin_username) == WEB_AUTH_SESSION_ROLE_ADMIN;

    bool verified = false;
    bool did_hmac = false;
    if (is_admin_claim) {
        totp_consume_result_t r = totp_config_verify_and_consume(code, (uint64_t)st.now_epoch);
        verified = (r == TOTP_CONSUME_OK);
        // REJECTED and OK both ran totp_verify()'s HMACs; NOT_ENROLLED and
        // the early UNAVAILABLE exits did not.
        did_hmac = (r == TOTP_CONSUME_OK || r == TOTP_CONSUME_REJECTED);
    } else {
        // Anti-oracle timing (plan section 6a): a non-administrator username
        // must not answer measurably faster than the administrator's --
        // otherwise the 202's response time alone enumerates the admin
        // username. Mirror the real path's NVS secret read here too.
        uint8_t scratch[TOTP_SECRET_LEN];
        (void)totp_config_load_secret(scratch);
        totp_secure_zero(scratch, sizeof(scratch));
    }
    if (!did_hmac) {
        // Same window of HMAC work as a real verification, against an
        // all-zero key whose result is discarded -- never consulted, never
        // persisted, so a coincidental match grants nothing.
        uint8_t dummy_secret[TOTP_SECRET_LEN];
        memset(dummy_secret, 0, sizeof(dummy_secret));
        uint64_t dummy_matched = 0;
        (void)totp_verify(dummy_secret, sizeof(dummy_secret), code, (uint64_t)st.now_epoch, 0u, &dummy_matched);
    }
    totp_secure_zero(code, sizeof(code));
    totp_secure_zero(&admin_record, sizeof(admin_record));

    // Anti-oracle (plan section 6a): always generate a token-shaped value and
    // always answer 202, whether or not verification succeeded. Only a
    // REAL verification stores it where /api/auth/reset can ever match it.
    uint8_t token_raw[16];
    hal_sysinfo_fill_random(token_raw, sizeof(token_raw));
    char token_hex[TOTP_RESET_TOKEN_HEX_LEN + 1];
    ota_http_hex_encode(token_raw, sizeof(token_raw), token_hex);
    totp_secure_zero(token_raw, sizeof(token_raw));

    totp_forgot_board_cap_record(&s_forgot_board_cap, verified);
    if (verified) {
        totp_reset_token_store(reset_tokens(), token_hex, username, now_ms());
    } else {
        ESP_LOGW(TAG, "forgot: verification failed from %s", ip);
    }
    totp_backoff_record(ip, ip_known, verified);

    char resp[80];
    snprintf(resp, sizeof(resp), "{\"reset_token\":\"%s\"}", token_hex);
    totp_secure_zero(token_hex, sizeof(token_hex));
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, resp);
    totp_secure_zero(resp, sizeof(resp));
    return ret;
}

// --- OPEN: POST /api/auth/reset ---------------------------------------------

#define RESET_BODY_MAX 256

static esp_err_t reset_post_handler(httpd_req_t *req)
{
    time_sync_status_t st;
    if (!sntp_ready(&st)) {
        return send_service_unavailable_clock(req);
    }

    char ip[46];
    bool ip_known = ota_http_get_client_ip_checked(req, ip, sizeof(ip));
    bool have_slot = false;
    if (!totp_backoff_gate(req, ip, ip_known, &have_slot)) {
        return ESP_OK;
    }

    if (req->content_len <= 0 || req->content_len >= RESET_BODY_MAX) {
        totp_backoff_record(ip, ip_known, false); /* malformed attempts count too (audit L44) */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }
    char body[RESET_BODY_MAX];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, (size_t)req->content_len - received);
        if (ret <= 0) {
            totp_backoff_record(ip, ip_known, false); /* malformed attempts count too (audit L44) */
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "failed to read body");
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    char username[SECURITY_HTTP_USERNAME_MAX + 1];
    char reset_token[TOTP_RESET_TOKEN_HEX_LEN + 1];
    char new_password[SECURITY_HTTP_PASSWORD_MAX + 1];
    int username_len = http_form_find_field(body, "username", username, sizeof(username));
    int token_len = http_form_find_field(body, "reset_token", reset_token, sizeof(reset_token));
    int password_len = http_form_find_field(body, "new_password", new_password, sizeof(new_password));
    if (username_len < 0 || token_len < 0 || password_len < 0) {
        totp_backoff_record(ip, ip_known, false); /* malformed attempts count too (audit L44) */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "username, reset_token and new_password are required");
        return ESP_OK;
    }

    totp_reset_token_result_t tr = totp_reset_token_consume(reset_tokens(), reset_token, username, now_ms());
    totp_secure_zero(reset_token, sizeof(reset_token));
    bool ok = false;
    // A token only proves a code was valid when /api/auth/forgot minted it
    // (up to TOTP_RESET_TOKEN_TTL_MS ago). Re-check, at the moment of use,
    // everything that made it valid then: TOTP still enrolled (not disabled
    // via cmd=totp_disable or cleared by the LCD reset gesture in the
    // meantime), and an administrator record still configured under exactly
    // this username (not cleared or renamed since). Otherwise a token
    // outstanding across any of those events would still set the admin
    // password -- the "reset one side of a pair" class (CLAUDE.md).
    web_auth_password_record_t admin_record;
    bool still_bound = false;
    if (tr == TOTP_RESET_TOKEN_OK && totp_config_enrolled() &&
        web_auth_store_load_password(WEB_AUTH_ROLE_ADMINISTRATOR, &admin_record) == WEB_AUTH_LOAD_OK &&
        admin_record.configured &&
        web_auth_login_role_for_username(username, admin_record.username) == WEB_AUTH_SESSION_ROLE_ADMIN) {
        still_bound = true;
    }
    totp_secure_zero(&admin_record, sizeof(admin_record));
    if (still_bound) {
        security_err_t serr = security_backend_get_vtable()->set_web_password(SECURITY_ROLE_ADMIN, username, new_password);
        ok = (serr == SECURITY_OK);
        if (ok) {
            web_auth_table_destroy_all(http_session_table());
        }
    }
    totp_secure_zero(new_password, sizeof(new_password));
    totp_backoff_record(ip, ip_known, ok);

    if (!ok) {
        ESP_LOGW(TAG, "reset failed from %s", ip);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    ESP_LOGI(TAG, "password reset succeeded from %s", ip);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// --- Registration ------------------------------------------------------------

esp_err_t auth_totp_http_start(void)
{
    if (s_totp_lock == NULL) {
        s_totp_lock = xSemaphoreCreateMutex();
        if (s_totp_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    httpd_handle_t server = wifi_provision_http_get_server();
    if (server == NULL) {
        return ESP_FAIL;
    }

    httpd_uri_t status_uri = { .uri = "/api/auth/totp_status", .method = HTTP_GET, .handler = totp_status_get_handler };
    httpd_uri_t forgot_uri = { .uri = "/api/auth/forgot", .method = HTTP_POST, .handler = forgot_post_handler };
    httpd_uri_t reset_uri = { .uri = "/api/auth/reset", .method = HTTP_POST, .handler = reset_post_handler };

    esp_err_t err = kiln_http_register(server, &status_uri);
    if (err == ESP_OK) err = kiln_http_register(server, &forgot_uri);
    if (err == ESP_OK) err = kiln_http_register(server, &reset_uri);
    return err;
}
