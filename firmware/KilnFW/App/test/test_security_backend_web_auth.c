// Host test for security_backend_web_auth.c (HOST_TEST_COVERAGE_GAPS round 2, R2-10).
//
// The REAL file is #included. The credential store (web_auth_store.c) and the
// session table (web_auth_session.c) are the REAL modules over fake_kv, so every
// assertion is against actual persisted state, not a counter on a stub. The
// vtable is fetched through security_backend_web_auth_start() +
// security_backend_get_vtable(), and the bootstrap route handler through the
// kiln_http_register() capture -- exactly how production reaches them. Only the
// collaborators that are ESP-facing or belong to other subsystems are fakes:
// ui_lcd_lock_force_lock (counted), the TOTP wipe callbacks (counted, result
// scripted), wifi_prov AP credentials, the httpd transport and
// http_auth_policy_web_enabled().
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include "esp_err.h"
#include "esp_http_server.h"
#include "fake_kv.h"
#include "psa/crypto.h"

psa_status_t g_stub_psa_import_key_result = PSA_SUCCESS;

#include "../drivers/http/security_backend_web_auth.c"

// ---- fakes ---------------------------------------------------------------------
static web_auth_table_t s_table;
web_auth_table_t *http_session_table(void) { return &s_table; }

static int f_force_lock_calls;
void ui_lcd_lock_force_lock(void) { f_force_lock_calls++; }

static bool f_web_enabled;
bool http_auth_policy_web_enabled(void) { return f_web_enabled; }

static const char *f_ap_ssid = "KilnAP-1234";
static const char *f_ap_pass = "ApSecret-7788";
const char *wifi_prov_get_ap_ssid(void) { return f_ap_ssid; }
const char *wifi_prov_get_ap_password(void) { return f_ap_pass; }

static bool f_totp_clear_result = true;
static int f_totp_clear_calls, f_tokens_clear_calls, f_pending_clear_calls;
bool totp_config_clear(void) { f_totp_clear_calls++; return f_totp_clear_result; }
void auth_totp_http_clear_reset_tokens(void) { f_tokens_clear_calls++; }
void security_totp_pending_clear(void) { f_pending_clear_calls++; }

static httpd_handle_t f_server = (httpd_handle_t)0x1;
httpd_handle_t wifi_provision_http_get_server(void) { return f_server; }

static esp_err_t (*s_bootstrap)(httpd_req_t *);
static char s_reg_uri[64];
static int s_reg_method, s_reg_calls;
esp_err_t kiln_http_register(httpd_handle_t server, const httpd_uri_t *u)
{
    (void)server;
    s_reg_calls++;
    snprintf(s_reg_uri, sizeof(s_reg_uri), "%s", u->uri);
    s_reg_method = (int)u->method;
    s_bootstrap = u->handler;
    return ESP_OK;
}

static int s_status;
static char s_reply[160];
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *t) { (void)r; (void)t; return ESP_OK; }
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s) { (void)r; s_status = atoi(s); return ESP_OK; }
esp_err_t httpd_resp_send(httpd_req_t *r, const char *b, long long n)
{
    (void)r; (void)n;
    snprintf(s_reply, sizeof(s_reply), "%s", b ? b : "");
    return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s)
{
    (void)r;
    snprintf(s_reply, sizeof(s_reply), "%s", s ? s : "");
    return ESP_OK;
}
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t e, const char *m)
{
    (void)r;
    s_status = (int)e;
    snprintf(s_reply, sizeof(s_reply), "%s", m ? m : "");
    return ESP_OK;
}
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *b, size_t n) { (void)r; (void)b; (void)n; return ESP_OK; }

static const char *s_body;
static size_t s_off;
int httpd_req_recv(httpd_req_t *r, char *buf, size_t len)
{
    (void)r;
    size_t total = s_body ? strlen(s_body) : 0;
    if (s_off >= total) {
        return 0;
    }
    size_t n = total - s_off;
    if (n > len) n = len;
    if (n > 5) n = 5; /* force the recv loop to iterate */
    memcpy(buf, s_body + s_off, n);
    s_off += n;
    return (int)n;
}

static int post_bootstrap(const char *body, long long declared_len)
{
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.content_len = declared_len;
    s_body = body;
    s_off = 0;
    s_status = 200;
    s_reply[0] = '\0';
    (void)s_bootstrap(&req);
    return s_status;
}

// ---- fixture ---------------------------------------------------------------------
static const security_backend_vtable_t *vt;
static const char *PW = "Cone6-Strong-262";
static const uint8_t TOK_A[WEB_AUTH_TOKEN_HASH_LEN] = {1};
static const uint8_t TOK_U[WEB_AUTH_TOKEN_HASH_LEN] = {2};

static void reset_all(void)
{
    fake_kv_reset_all();
    fake_kv_set_write_safe_here(true);
    hal_kv_init_partition(NULL);
    web_auth_table_init(&s_table);
    f_force_lock_calls = 0;
    f_web_enabled = false;
    f_totp_clear_result = true;
    f_totp_clear_calls = f_tokens_clear_calls = f_pending_clear_calls = 0;
    PW = "Cone6-Strong-262";
    vt = security_backend_get_vtable();
}

static void seed_sessions(void)
{
    web_auth_table_init(&s_table);
    web_auth_table_create_session(&s_table, TOK_A, "10.0.0.1", WEB_AUTH_SESSION_ROLE_ADMIN, 1000);
    web_auth_table_create_session(&s_table, TOK_U, "10.0.0.2", WEB_AUTH_SESSION_ROLE_USER, 1000);
}

static void seed_admin(void)
{
    uint8_t salt[WEB_AUTH_SALT_LEN] = {9};
    TEST_CHECK(web_auth_store_set_password(WEB_AUTH_ROLE_ADMINISTRATOR, "admin", PW, salt, false) == HAL_OK, "seed admin password");
}

static void seed_admin_pin(void)
{
    uint8_t salt[WEB_AUTH_SALT_LEN] = {8};
    TEST_CHECK(web_auth_store_set_pin(WEB_AUTH_ROLE_ADMINISTRATOR, "4321", salt) == HAL_OK, "seed admin pin");
}

static security_policy_t pol(bool web, bool lcd, int wmin, int lmin)
{
    security_policy_t p;
    memset(&p, 0, sizeof(p));
    p.web_enabled = web;
    p.lcd_enabled = lcd;
    p.web_timeout_min = wmin;
    p.lcd_timeout_min = lmin;
    return p;
}

// ---- tests -----------------------------------------------------------------------
static void test_install_and_start(void)
{
    TEST_SECTION("install / start: vtable installed, bootstrap route registered once, no server is a loud error");
    reset_all();
    f_server = NULL;
    s_reg_calls = 0;
    TEST_CHECK(security_backend_web_auth_start() == ESP_ERR_INVALID_STATE, "no HTTP server: start fails INVALID_STATE");
    TEST_CHECK(s_reg_calls == 0, "and registers nothing");
    TEST_CHECK(security_backend_get_vtable()->set_policy != NULL, "vtable still installed before the server check");
    f_server = (httpd_handle_t)0x1;
    TEST_CHECK(security_backend_web_auth_start() == ESP_OK, "start ok");
    TEST_CHECK(s_reg_calls == 1 && strcmp(s_reg_uri, "/api/auth/bootstrap_password") == 0 && s_reg_method == HTTP_POST,
               "POST /api/auth/bootstrap_password registered");
    vt = security_backend_get_vtable();
    TEST_CHECK(vt->set_web_password && vt->set_lcd_pin && vt->set_policy && vt->get_config &&
                   vt->invalidate_sessions_for_role && vt->clear_all_credentials,
               "all six slots populated");
}

static void test_set_web_password(void)
{
    TEST_SECTION("set_web_password: strength, role/username rules, exact persisted record, storage failure");
    reset_all();
    TEST_CHECK(vt->set_web_password(SECURITY_ROLE_ADMIN, "admin", NULL) == SECURITY_ERR_INVALID_INPUT, "NULL password");
    TEST_CHECK(vt->set_web_password(SECURITY_ROLE_ADMIN, "admin", "short") == SECURITY_ERR_WEAK, "too short -> WEAK");
    TEST_CHECK(vt->set_web_password(SECURITY_ROLE_ADMIN, "admin", "alllowercasepassword") == SECURITY_ERR_WEAK, "all lowercase -> WEAK");
    TEST_CHECK(vt->set_web_password(SECURITY_ROLE_ADMIN, "admin", f_ap_pass) == SECURITY_ERR_WEAK, "equal to the AP password -> WEAK (cross-credential reuse)");
    TEST_CHECK(vt->set_web_password(SECURITY_ROLE_ADMIN, "admin", f_ap_ssid) == SECURITY_ERR_WEAK, "equal to the AP SSID -> WEAK");
    TEST_CHECK(vt->set_web_password(SECURITY_ROLE_ADMIN, "", PW) == SECURITY_ERR_INVALID_INPUT, "admin with empty username");
    TEST_CHECK(vt->set_web_password(SECURITY_ROLE_ADMIN, NULL, PW) == SECURITY_ERR_INVALID_INPUT, "admin with NULL username");
    TEST_CHECK(!web_auth_store_password_configured(WEB_AUTH_ROLE_ADMINISTRATOR), "nothing persisted by any refusal");

    TEST_CHECK(vt->set_web_password(SECURITY_ROLE_ADMIN, "kilnboss", PW) == SECURITY_OK, "valid admin set");
    web_auth_password_record_t rec;
    TEST_CHECK(web_auth_store_load_password(WEB_AUTH_ROLE_ADMINISTRATOR, &rec) == WEB_AUTH_LOAD_OK, "record loads");
    TEST_CHECK(strcmp(rec.username, "kilnboss") == 0 && !rec.must_change, "username kept, must_change false (owner-initiated change)");
    TEST_CHECK(web_auth_store_verify_password(WEB_AUTH_ROLE_ADMINISTRATOR, PW), "the password verifies");
    TEST_CHECK(!web_auth_store_verify_password(WEB_AUTH_ROLE_ADMINISTRATOR, "Cone6-Strong-263"), "a different password does not");

    TEST_CHECK(vt->set_web_password(SECURITY_ROLE_USER, "ignored-name", "Another-Strong-9") == SECURITY_OK, "valid user set");
    TEST_CHECK(web_auth_store_load_password(WEB_AUTH_ROLE_USER, &rec) == WEB_AUTH_LOAD_OK && strcmp(rec.username, "user") == 0,
               "user record always stores the fixed name 'user', supplied name ignored");
    TEST_CHECK(web_auth_store_verify_password(WEB_AUTH_ROLE_USER, "Another-Strong-9"), "user password verifies");
    TEST_CHECK(web_auth_store_verify_password(WEB_AUTH_ROLE_ADMINISTRATOR, PW), "admin record untouched by the user write");

    reset_all();
    fake_kv_script_next_write_status(HAL_NO_MEM);
    TEST_CHECK(vt->set_web_password(SECURITY_ROLE_ADMIN, "admin", PW) == SECURITY_ERR_STORAGE, "store write failure -> STORAGE");
    TEST_CHECK(!web_auth_store_password_configured(WEB_AUTH_ROLE_ADMINISTRATOR), "and nothing is configured");
}

static void test_set_lcd_pin(void)
{
    TEST_SECTION("set_lcd_pin: format re-validation, persisted PIN, storage failure");
    reset_all();
    TEST_CHECK(vt->set_lcd_pin(SECURITY_ROLE_ADMIN, NULL) == SECURITY_ERR_INVALID_INPUT, "NULL pin");
    TEST_CHECK(vt->set_lcd_pin(SECURITY_ROLE_ADMIN, "123") == SECURITY_ERR_WEAK, "too short -> WEAK");
    TEST_CHECK(vt->set_lcd_pin(SECURITY_ROLE_ADMIN, "12a4") == SECURITY_ERR_WEAK, "non-digit -> WEAK");
    TEST_CHECK(vt->set_lcd_pin(SECURITY_ROLE_ADMIN, "1234567890123") == SECURITY_ERR_WEAK, "too long -> WEAK");
    TEST_CHECK(!web_auth_store_pin_configured(WEB_AUTH_ROLE_ADMINISTRATOR), "no refusal persists");
    TEST_CHECK(vt->set_lcd_pin(SECURITY_ROLE_ADMIN, "4321") == SECURITY_OK, "valid admin PIN");
    TEST_CHECK(web_auth_store_verify_pin(WEB_AUTH_ROLE_ADMINISTRATOR, "4321") && !web_auth_store_verify_pin(WEB_AUTH_ROLE_ADMINISTRATOR, "4322"),
               "admin PIN verifies, a neighbour does not");
    TEST_CHECK(!web_auth_store_pin_configured(WEB_AUTH_ROLE_USER), "user PIN untouched");
    TEST_CHECK(vt->set_lcd_pin(SECURITY_ROLE_USER, "8765") == SECURITY_OK && web_auth_store_verify_pin(WEB_AUTH_ROLE_USER, "8765"), "valid user PIN");
    fake_kv_script_next_write_status(HAL_NO_MEM);
    TEST_CHECK(vt->set_lcd_pin(SECURITY_ROLE_USER, "1111") == SECURITY_ERR_STORAGE, "write failure -> STORAGE");
    TEST_CHECK(web_auth_store_verify_pin(WEB_AUTH_ROLE_USER, "8765"), "previous user PIN still intact after the failed write");
}

static void test_set_policy(void)
{
    TEST_SECTION("set_policy: one transition gate, minutes->seconds, sessions cleared only on a real enable edge, failed write clears nothing");
    reset_all();
    TEST_CHECK(vt->set_policy(NULL) == SECURITY_ERR_INVALID_INPUT, "NULL policy");

    security_policy_t on = pol(true, false, 15, -1);
    seed_sessions();
    TEST_CHECK(vt->set_policy(&on) == SECURITY_ERR_INVALID_INPUT, "enable web with NO administrator password: refused");
    web_auth_policy_t stored;
    TEST_CHECK(web_auth_store_load_policy(&stored) != WEB_AUTH_LOAD_OK, "refusal persists nothing");
    TEST_CHECK(web_auth_table_find_by_token(&s_table, TOK_A) >= 0 && f_force_lock_calls == 0, "refusal clears no session, locks no LCD");

    /* a USER password alone must not satisfy the gate (the 2026-09-17 review's hole) */
    TEST_CHECK(vt->set_web_password(SECURITY_ROLE_USER, "u", "Another-Strong-9") == SECURITY_OK, "user password set");
    TEST_CHECK(vt->set_policy(&on) == SECURITY_ERR_INVALID_INPUT, "user password alone does not allow enabling web auth");

    seed_admin();
    fake_kv_script_next_write_status(HAL_NO_MEM);
    TEST_CHECK(vt->set_policy(&on) == SECURITY_ERR_STORAGE, "policy write failure -> STORAGE");
    TEST_CHECK(web_auth_table_find_by_token(&s_table, TOK_A) >= 0 && web_auth_table_find_by_token(&s_table, TOK_U) >= 0 && f_force_lock_calls == 0,
               "a FAILED write must not clear sessions (they would otherwise be torn down for a policy that never took effect)");

    TEST_CHECK(vt->set_policy(&on) == SECURITY_OK, "enable web with admin credential: ok");
    TEST_CHECK(web_auth_store_load_policy(&stored) == WEB_AUTH_LOAD_OK && stored.web_enabled && !stored.lcd_enabled &&
                   stored.web_timeout_s == 900 && stored.lcd_timeout_s == -1,
               "stored: web on, 15 min -> 900 s, lcd never -> -1");
    TEST_CHECK(web_auth_table_find_by_token(&s_table, TOK_A) < 0 && web_auth_table_find_by_token(&s_table, TOK_U) < 0, "off->on edge destroyed every web session");
    TEST_CHECK(f_force_lock_calls == 0, "LCD policy unchanged: LCD not force-locked");

    seed_sessions();
    security_policy_t retime = pol(true, false, 60, -1);
    TEST_CHECK(vt->set_policy(&retime) == SECURITY_OK, "timeout-only change ok");
    TEST_CHECK(web_auth_store_load_policy(&stored) == WEB_AUTH_LOAD_OK && stored.web_timeout_s == 3600, "60 min -> 3600 s");
    TEST_CHECK(web_auth_table_find_by_token(&s_table, TOK_A) >= 0 && web_auth_table_find_by_token(&s_table, TOK_U) >= 0,
               "on->on does not log anyone out");

    security_policy_t lcd_on = pol(true, true, 60, 5);
    TEST_CHECK(vt->set_policy(&lcd_on) == SECURITY_ERR_INVALID_INPUT, "enable LCD auth with no admin PIN: refused");
    TEST_CHECK(web_auth_store_load_policy(&stored) == WEB_AUTH_LOAD_OK && !stored.lcd_enabled, "LCD stays off");
    seed_admin_pin();
    f_force_lock_calls = 0;
    TEST_CHECK(vt->set_policy(&lcd_on) == SECURITY_OK, "enable LCD auth with admin PIN");
    TEST_CHECK(f_force_lock_calls == 1, "off->on LCD edge force-locks the panel exactly once");
    TEST_CHECK(web_auth_store_load_policy(&stored) == WEB_AUTH_LOAD_OK && stored.lcd_timeout_s == 300, "5 min -> 300 s");

    security_policy_t off = pol(false, false, 60, 5);
    seed_sessions();
    f_force_lock_calls = 0;
    TEST_CHECK(vt->set_policy(&off) == SECURITY_OK, "disabling both is always allowed");
    TEST_CHECK(web_auth_store_load_policy(&stored) == WEB_AUTH_LOAD_OK && !stored.web_enabled && !stored.lcd_enabled, "stored off");
    (void)0;
}

static void test_get_config(void)
{
    TEST_SECTION("get_config: absent store reads as off/never; minutes round to nearest; no secret echoed");
    reset_all();
    security_config_t c;
    memset(&c, 0xAA, sizeof(c));
    TEST_CHECK(!vt->get_config(NULL), "NULL out");
    TEST_CHECK(vt->get_config(&c), "absent store still answers");
    TEST_CHECK(!c.web_enabled && !c.lcd_enabled && c.web_timeout_min == -1 && c.lcd_timeout_min == -1, "defaults: off, never");
    TEST_CHECK(!c.admin_password_set && !c.user_password_set && !c.lcd_admin_pin_set && !c.lcd_user_pin_set, "nothing configured");
    TEST_CHECK(c.admin_username[0] == '\0' && !c.admin_must_change, "no admin username (struct was memset first, not stale 0xAA)");

    seed_admin();
    seed_admin_pin();
    web_auth_policy_t p = {.web_enabled = true, .lcd_enabled = true, .web_timeout_s = 89, .lcd_timeout_s = 90};
    TEST_CHECK(web_auth_store_set_policy(&p) == HAL_OK, "seed policy directly");
    TEST_CHECK(vt->get_config(&c), "config reads");
    TEST_CHECK(c.web_enabled && c.lcd_enabled, "enabled flags");
    TEST_CHECK(c.web_timeout_min == 1 && c.lcd_timeout_min == 2, "89 s -> 1 min, 90 s -> 2 min (round to nearest, not truncate)");
    TEST_CHECK(c.admin_password_set && !c.user_password_set && c.lcd_admin_pin_set && !c.lcd_user_pin_set, "per-credential flags");
    TEST_CHECK(strcmp(c.admin_username, "admin") == 0, "username shown");
    p.web_timeout_s = -1;
    p.lcd_timeout_s = 3600;
    web_auth_store_set_policy(&p);
    vt->get_config(&c);
    TEST_CHECK(c.web_timeout_min == -1 && c.lcd_timeout_min == 60, "-1 stays never; 3600 s -> 60");
}

static void test_invalidate_sessions(void)
{
    TEST_SECTION("invalidate_sessions_for_role: that role's web sessions plus the LCD session, other role untouched");
    reset_all();
    seed_sessions();
    vt->invalidate_sessions_for_role(SECURITY_ROLE_ADMIN);
    TEST_CHECK(web_auth_table_find_by_token(&s_table, TOK_A) < 0, "admin web session destroyed");
    TEST_CHECK(web_auth_table_find_by_token(&s_table, TOK_U) >= 0, "user web session survives an admin invalidation");
    TEST_CHECK(f_force_lock_calls == 1, "LCD force-locked (single LCD session cannot be role-targeted)");
    seed_sessions();
    vt->invalidate_sessions_for_role(SECURITY_ROLE_USER);
    TEST_CHECK(web_auth_table_find_by_token(&s_table, TOK_U) < 0 && web_auth_table_find_by_token(&s_table, TOK_A) >= 0, "user invalidation spares admin");
    TEST_CHECK(f_force_lock_calls == 2, "LCD force-locked again");
}

static void test_clear_all_credentials(void)
{
    TEST_SECTION("clear_all_credentials: web + TOTP wiped; success and half-wipe (web gone, TOTP stuck) both tear down live sessions");
    reset_all();
    seed_admin();
    seed_admin_pin();
    vt->set_web_password(SECURITY_ROLE_USER, "u", "Another-Strong-9");
    seed_sessions();
    TEST_CHECK(vt->clear_all_credentials() == SECURITY_OK, "both halves ok -> OK");
    TEST_CHECK(!web_auth_store_password_configured(WEB_AUTH_ROLE_ADMINISTRATOR) && !web_auth_store_password_configured(WEB_AUTH_ROLE_USER) &&
                   !web_auth_store_pin_configured(WEB_AUTH_ROLE_ADMINISTRATOR),
               "store really cleared (admin pw, user pw, admin pin)");
    TEST_CHECK(f_totp_clear_calls == 1 && f_tokens_clear_calls == 1 && f_pending_clear_calls == 1, "TOTP config, reset tokens and pending enrollment each cleared once");
    TEST_CHECK(web_auth_table_find_by_token(&s_table, TOK_A) < 0 && web_auth_table_find_by_token(&s_table, TOK_U) < 0,
               "clean success also destroys BOTH roles' live sessions (consistent with the half-wipe branch)");
    TEST_CHECK(f_force_lock_calls == 2, "LCD force-locked once per role invalidation on clean success");

    reset_all();
    seed_admin();
    seed_sessions();
    f_totp_clear_result = false;
    TEST_CHECK(vt->clear_all_credentials() == SECURITY_ERR_STORAGE, "TOTP clear failed -> STORAGE");
    TEST_CHECK(!web_auth_store_password_configured(WEB_AUTH_ROLE_ADMINISTRATOR), "web half was still wiped");
    TEST_CHECK(f_tokens_clear_calls == 1 && f_pending_clear_calls == 1, "reset tokens and pending secret cleared anyway (no short-circuit on the TOTP result)");
    TEST_CHECK(web_auth_table_find_by_token(&s_table, TOK_A) < 0 && web_auth_table_find_by_token(&s_table, TOK_U) < 0,
               "web wiped but TOTP stuck: BOTH roles' live sessions destroyed (they sit on a credential that no longer exists)");
    TEST_CHECK(f_force_lock_calls == 2, "LCD force-locked once per role invalidation");

    reset_all();
    seed_admin();
    seed_sessions();
    fake_kv_script_silent_set_noops(16);
    bool web_still_there = true;
    (void)0;
    {
        security_err_t r = vt->clear_all_credentials();
        web_still_there = web_auth_store_password_configured(WEB_AUTH_ROLE_ADMINISTRATOR);
        TEST_CHECK(r == SECURITY_ERR_STORAGE, "web clear that cannot be confirmed by read-back -> STORAGE");
        TEST_CHECK(web_still_there, "(precondition for this case: the lying erase left the credential in place)");
        TEST_CHECK(web_auth_table_find_by_token(&s_table, TOK_A) >= 0 && f_force_lock_calls == 0,
                   "web NOT cleared: sessions are still valid against a credential that still exists, so none are torn down");
        TEST_CHECK(f_totp_clear_calls == 1, "TOTP wipe still attempted unconditionally");
    }
}

static void test_bootstrap_handler(void)
{
    TEST_SECTION("POST /api/auth/bootstrap_password: only while bootstrap is needed, body validation, strength, one-shot");
    reset_all();
    security_backend_web_auth_start();

    f_web_enabled = false;
    TEST_CHECK(post_bootstrap("username=admin&password=Cone6-Strong-262", 40) == 409, "auth effectively off: 409, no bootstrap");
    TEST_CHECK(strstr(s_reply, "already configured") != NULL, "409 body names the reason");
    TEST_CHECK(!web_auth_store_password_configured(WEB_AUTH_ROLE_ADMINISTRATOR), "nothing stored under a refusal");

    f_web_enabled = true; /* enabled, no admin credential: bootstrap needed */
    TEST_CHECK(post_bootstrap("", 0) == 400, "empty body: 400");
    TEST_CHECK(post_bootstrap("x", 512) == 400, "content_len at the 512 cap: 400");
    TEST_CHECK(post_bootstrap("username=admin", 14) == 400, "missing password field: 400");
    TEST_CHECK(post_bootstrap("password=Cone6-Strong-262", 25) == 400, "missing username field: 400");
    TEST_CHECK(post_bootstrap("username=admin&password=short", 29) == 400, "weak password: 400");
    TEST_CHECK(strstr(s_reply, "password rejected") != NULL, "weak reply text");
    TEST_CHECK(post_bootstrap("username=&password=Cone6-Strong-262", 35) == 400, "empty username: 400");
    TEST_CHECK(!web_auth_store_password_configured(WEB_AUTH_ROLE_ADMINISTRATOR), "no refusal stored anything");

    /* declared length longer than the bytes that arrive: recv returns 0 early */
    TEST_CHECK(post_bootstrap("username=admin&password=Cone6-Strong-262", 60) == 400, "truncated body: 400 (never stores a short password)");
    TEST_CHECK(!web_auth_store_password_configured(WEB_AUTH_ROLE_ADMINISTRATOR), "truncation stored nothing");

    fake_kv_script_next_write_status(HAL_NO_MEM);
    TEST_CHECK(post_bootstrap("username=admin&password=Cone6-Strong-262", 40) == 500, "store failure: 500");
    TEST_CHECK(!web_auth_store_password_configured(WEB_AUTH_ROLE_ADMINISTRATOR), "failed write left nothing");

    TEST_CHECK(post_bootstrap("username=admin&password=Cone6-Strong-262", 40) == 200, "valid body (split over several recv calls): 200");
    TEST_CHECK(strcmp(s_reply, "{\"ok\":true}") == 0, "ok reply");
    web_auth_password_record_t rec;
    TEST_CHECK(web_auth_store_load_password(WEB_AUTH_ROLE_ADMINISTRATOR, &rec) == WEB_AUTH_LOAD_OK && strcmp(rec.username, "admin") == 0 && !rec.must_change,
               "admin record written, must_change false");
    TEST_CHECK(web_auth_store_verify_password(WEB_AUTH_ROLE_ADMINISTRATOR, "Cone6-Strong-262"), "full password stored intact");

    TEST_CHECK(post_bootstrap("username=evil&password=Evil-Strong-2026", 39) == 409, "second call: 409, bootstrap is one-shot");
    TEST_CHECK(web_auth_store_verify_password(WEB_AUTH_ROLE_ADMINISTRATOR, "Cone6-Strong-262") && !web_auth_store_verify_password(WEB_AUTH_ROLE_ADMINISTRATOR, "Evil-Strong-2026"),
               "the first credential is untouched by the rejected second call");
}

int main(void)
{
    test_install_and_start();
    test_set_web_password();
    test_set_lcd_pin();
    test_set_policy();
    test_get_config();
    test_invalidate_sessions();
    test_clear_all_credentials();
    test_bootstrap_handler();
    printf("security_backend_web_auth: %d checks, %d failures\n", g_test_count, g_test_failures);
    return g_test_failures ? 1 : 0;
}
