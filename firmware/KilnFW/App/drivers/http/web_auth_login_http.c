// web_auth_login_http.c -- see web_auth_login_http.h for scope. Docs/
// WEB_AUTH_PLAN.md section 6, the last missing piece of the browser-side
// auth flow: GET /login (page) and POST /api/auth/login (credential check
// + session mint).
//
// SESSION TABLE / TOKEN HASH OWNERSHIP: this file deliberately does NOT own
// a web_auth_table_t or a token-hashing implementation of its own -- it
// calls http_session_table() and http_session_hash_token()
// (http_session_iface.h), the same table and the same hash
// http_auth_session_resolve() looks sessions up against. Creating a second,
// independently-owned table or hash here would be exactly the
// "reset-one-side-of-a-pair" bug class CLAUDE.md warns about: a session
// minted here would then never resolve there. See http_session_iface.c's
// own header comment, which names this file as the intended adopter.
//
// LOCKOUT: web login gets its OWN ota_auth_lockout_state_t instances,
// separate from the OTA-esp/OTA-pico/LCD-PIN lockouts -- WEB_AUTH_PLAN.md
// section 7's explicit design note that each authentication surface must
// not share lockout state with any other.
//
// Finding 4 fix (2026-09-17 review): a SINGLE global instance meant one
// remote IP failing three logins locked out every other client on the LAN
// too, including the legitimate operator -- a trivial denial-of-service
// against a single-operator device. Lockout state is now tracked per
// source IP, in a small FIXED-SIZE table (LOGIN_LOCKOUT_MAX_IPS below) so
// an attacker sending attempts from many spoofed/rotating source addresses
// cannot grow this table without bound -- once full, the least-recently-
// active IP's slot is evicted for the new one, the same bounded LRU
// discipline web_auth_table_create_session() already uses for the (also
// fixed-size) session table.
//
// STACK: no locals here approach the httpd 8 KB stack blob class this
// codebase watches for (project_httpd_stack_blob_class) -- the largest
// local is LOGIN_BODY_MAX (256) bytes, well under the ~256-byte budget
// WEB_AUTH_PLAN.md section 4 documents for the KDF's own locals, and no
// buffer here is enlarged beyond that.
#include "web_auth_login_http.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "hal_sysinfo.h"    // hal_sysinfo_fill_random() -- HAL boundary, no raw esp_random.h
#include "hal_time.h"       // hal_time_now_ms()
#include "http_auth_http.h" // kiln_http_register()
#include "http_form.h"      // http_form_find_field()
#include "http_session_iface.h" // http_session_table(), http_session_hash_token()
#include "ota_auth.h"        // ota_auth_lockout_state_t (this route's own instance)
#include "ota_http_util.h"   // ota_http_hex_encode()
#include "security_http_core.h" // SECURITY_HTTP_USERNAME_MAX/PASSWORD_MAX
#include "web_auth_login.h"  // web_auth_login_role_for_username()
#include "web_auth_session.h"
#include "web_auth_store.h"
#include "web_encoding.h"
#include "wifi_provision_http.h" // wifi_provision_http_get_server()

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "web_auth_login_http";

// GET client IP -- same extraction helper http_auth_http.c already forward-
// declares against ota_http.c's real (non-static) definition; declared here
// too rather than pulled from a header ota_http.h does not itself expose it
// through, matching that file's own precedent.
void ota_http_get_client_ip(httpd_req_t *req, char *out, size_t out_len);

/* Embedded via EMBED_TXTFILES, pre-gzipped at configure time by
 * App/drivers/CMakeLists.txt -- same convention as every other *_page.html
 * in this component. */
extern const uint8_t login_page_html_gz_start[] asm("_binary_login_page_html_gz_start");
extern const uint8_t login_page_html_gz_end[] asm("_binary_login_page_html_gz_end");

// This route's own PER-IP lockout table -- separate from every OTA/LCD-PIN
// instance, per WEB_AUTH_PLAN.md section 7. Guarded by s_login_lock, same
// "short critical section around plain state, not around I/O" shape
// ota_http.c's s_ota_lock uses around ota_auth_nonce_issue(). Fixed size
// (Finding 4 fix, see this file's header comment) -- bounded LRU eviction,
// never grows.
#define LOGIN_LOCKOUT_MAX_IPS 16u
typedef struct {
    bool in_use;
    char ip[46];
    uint32_t last_activity_ms;
    ota_auth_lockout_state_t lockout;
} login_lockout_slot_t;
static login_lockout_slot_t s_login_lockouts[LOGIN_LOCKOUT_MAX_IPS];
static SemaphoreHandle_t s_login_lock;

// Must be called with s_login_lock already held. Finds the slot for `ip`,
// touching its last_activity_ms; if none exists, claims a free slot or,
// failing that, evicts the least-recently-active slot (bounded table, see
// header comment) and starts that IP with fresh (unlocked) lockout state.
static login_lockout_slot_t *login_lockout_slot_for(const char *ip, uint32_t now)
{
    int free_idx = -1;
    int lru_idx = 0;
    uint32_t lru_time = UINT32_MAX;
    for (unsigned i = 0; i < LOGIN_LOCKOUT_MAX_IPS; i++) {
        login_lockout_slot_t *s = &s_login_lockouts[i];
        if (s->in_use && strcmp(s->ip, ip) == 0) {
            s->last_activity_ms = now;
            return s;
        }
        if (!s->in_use && free_idx < 0) {
            free_idx = (int)i;
        }
        if (s->in_use && s->last_activity_ms < lru_time) {
            lru_time = s->last_activity_ms;
            lru_idx = (int)i;
        }
    }
    int idx = (free_idx >= 0) ? free_idx : lru_idx;
    login_lockout_slot_t *s = &s_login_lockouts[idx];
    memset(s, 0, sizeof(*s));
    s->in_use = true;
    strncpy(s->ip, ip, sizeof(s->ip) - 1);
    s->last_activity_ms = now;
    return s;
}

static uint32_t now_ms(void)
{
    return (uint32_t)hal_time_now_ms();
}

static esp_err_t login_page_get_handler(httpd_req_t *req)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, TAG, "login_page.html");
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)login_page_html_gz_start,
                            (size_t)(login_page_html_gz_end - login_page_html_gz_start));
}

// "username"/"password" form fields, well under the httpd stack budget
// this codebase enforces -- see this file's header comment.
#define LOGIN_BODY_MAX 256

static esp_err_t login_post_handler(httpd_req_t *req)
{
    char ip[46];
    ota_http_get_client_ip(req, ip, sizeof(ip));

    bool locked = false;
    if (xSemaphoreTake(s_login_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        login_lockout_slot_t *slot = login_lockout_slot_for(ip, now_ms());
        locked = ota_auth_lockout_is_locked(&slot->lockout, now_ms());
        xSemaphoreGive(s_login_lock);
    } else {
        ESP_LOGW(TAG, "login from %s: internal lock timeout, refused", ip);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
        return ESP_OK;
    }
    if (locked) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_send(req, "too many failed attempts, try again later", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (req->content_len <= 0 || req->content_len >= LOGIN_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }

    // Finding 5 fix (2026-09-17 review): httpd_req_recv() is not guaranteed
    // to fill the whole request in one call -- ESP-IDF's own docs note it
    // may return fewer bytes than asked even before EOF/error. A single
    // unlooped call silently truncated the body on any connection that
    // split it across TCP segments, corrupting username/password field
    // parsing rather than failing loud. Loop until content_len bytes are
    // read, same shape as security_http.c's security_post_handler().
    char body[LOGIN_BODY_MAX];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, (size_t)req->content_len - received);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "failed to read body");
            return ESP_OK;
        }
        received += (size_t)ret;
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

    // Resolve which role's record to check against WITHOUT running the KDF
    // twice -- see web_auth_login.h's header comment for the full
    // rationale (the administrator username is not a secret).
    web_auth_password_record_t admin_record;
    const char *admin_username = NULL;
    if (web_auth_store_load_password(WEB_AUTH_ROLE_ADMINISTRATOR, &admin_record) == WEB_AUTH_LOAD_OK &&
        admin_record.configured) {
        admin_username = admin_record.username;
    }
    web_auth_session_role_t candidate_role = web_auth_login_role_for_username(username, admin_username);
    web_auth_role_t store_role =
        (candidate_role == WEB_AUTH_SESSION_ROLE_ADMIN) ? WEB_AUTH_ROLE_ADMINISTRATOR : WEB_AUTH_ROLE_USER;

    bool ok = web_auth_store_verify_password(store_role, password);

    // Wipe the plaintext password from this stack frame the moment it is no
    // longer needed -- same discipline web_auth_store.h documents for its
    // own callers ("never logged, never stored, discarded ... immediately
    // after this call").
    memset(password, 0, sizeof(password));

    if (xSemaphoreTake(s_login_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        login_lockout_slot_t *slot = login_lockout_slot_for(ip, now_ms());
        if (ok) {
            ota_auth_lockout_record_success(&slot->lockout);
        } else {
            ota_auth_lockout_record_failure(&slot->lockout, now_ms());
        }
        xSemaphoreGive(s_login_lock);
    }

    if (!ok) {
        ESP_LOGW(TAG, "login failed from %s", ip);
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "invalid username or password");
        return ESP_OK;
    }

    // Mint a fresh 32-byte random token, hash it with the SAME function the
    // resolver (http_session_iface.c) hashes lookups with, and create the
    // session in the SAME table that resolver reads -- see this file's
    // header comment.
    uint8_t token_raw[32];
    hal_sysinfo_fill_random(token_raw, sizeof(token_raw));
    char token_hex[sizeof(token_raw) * 2 + 1];
    ota_http_hex_encode(token_raw, sizeof(token_raw), token_hex);

    uint8_t token_hash[32];
    http_session_hash_token(token_hex, strlen(token_hex), token_hash);

    web_auth_session_role_t session_role =
        (store_role == WEB_AUTH_ROLE_ADMINISTRATOR) ? WEB_AUTH_SESSION_ROLE_ADMIN : WEB_AUTH_SESSION_ROLE_USER;
    web_auth_table_create_session(http_session_table(), token_hash, ip, session_role, now_ms());

    // 128 bytes: "kiln_sid=" (9) + 64 hex chars + "; HttpOnly; SameSite=Strict; Path=/"
    // (36) + NUL = 110 -- the previous 96-byte buffer was too small to hold
    // a real cookie and made every successful login fail with a spurious
    // 500 (found by this pass's host tests, not itself one of the seven
    // audit findings, but blocking their test coverage). Still well under
    // LOGIN_BODY_MAX (256), this file's stated httpd-stack budget.
    char cookie[128];
    int cookie_len = snprintf(cookie, sizeof(cookie), HTTP_SESSION_COOKIE_NAME "=%s; HttpOnly; SameSite=Strict; Path=/",
                               token_hex);
    if (cookie_len < 0 || (size_t)cookie_len >= sizeof(cookie)) {
        ESP_LOGE(TAG, "Set-Cookie formatting failed");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "internal error");
        return ESP_OK;
    }
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);

    ESP_LOGI(TAG, "login succeeded from %s (role=%d)", ip, (int)session_role);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t web_auth_login_http_start(void)
{
    if (s_login_lock == NULL) {
        s_login_lock = xSemaphoreCreateMutex();
        if (s_login_lock == NULL) {
            ESP_LOGE(TAG, "xSemaphoreCreateMutex failed");
            return ESP_ERR_NO_MEM;
        }
    }

    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t login_page_uri = {
        .uri = "/login",
        .method = HTTP_GET,
        .handler = login_page_get_handler,
    };
    esp_err_t err = kiln_http_register(server, &login_page_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/login) failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t login_post_uri = {
        .uri = "/api/auth/login",
        .method = HTTP_POST,
        .handler = login_post_handler,
    };
    err = kiln_http_register(server, &login_post_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/auth/login) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "web login routes up: GET /login, POST /api/auth/login");
    return ESP_OK;
}
