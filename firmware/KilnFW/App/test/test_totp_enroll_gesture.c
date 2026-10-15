// Host tests for AUDIT_TOTP (docs/audits/AUDIT_TOTP.md): the TOTP enrollment
// commands in security_http.c (test gap 1), the REAL LCD four-corner reset
// wiring (gap 5, MED-1), the totp_status reply (gap 7), totp_config's state
// lock (LOW-1) and the enroll-commit write order (LOW-2).
//
// Own standalone executable. security_http.c and auth_totp_http.c are
// #include'd directly so their static handlers and RAM state (pending
// enrollment, reset-token table) are reachable. The credential store
// (web_auth_store.c), TOTP store (totp_config.c), session table and the LCD
// gesture wiring (auth_reset_gesture_wiring.c) are the REAL modules over
// fake_kv -- nothing the gesture is supposed to clear is a fake.
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
#include "fake_sysinfo.h"
#include "fake_time.h"
#include "psa/crypto.h"

psa_status_t g_stub_psa_import_key_result = PSA_SUCCESS;

#include "../drivers/persist/web_auth_store.c"

#include "../drivers/net/time_sync.h"

#define asm(x)
#define TAG TAG_auth_totp_http
#include "../drivers/http/auth_totp_http.c"
#undef TAG
#define TAG TAG_security_http
#include "../drivers/http/security_http.c"
#undef TAG
#undef asm

const uint8_t security_page_html_gz_start[1] = {0};
const uint8_t security_page_html_gz_end[1] = {0};

#include "../drivers/net/auth_reset_gesture.h"

// ---- httpd stubs -----------------------------------------------------------
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler)
{
    (void)handle;
    (void)uri_handler;
    return ESP_OK;
}
esp_err_t kiln_http_register(httpd_handle_t server, const httpd_uri_t *uri_handler)
{
    (void)server;
    (void)uri_handler;
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type) { (void)r; (void)type; return ESP_OK; }
int httpd_req_to_sockfd(httpd_req_t *r) { (void)r; return -1; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *f, const char *v) { (void)r; (void)f; (void)v; return ESP_OK; }
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, long long n) { (void)r; (void)buf; (void)n; return ESP_OK; }
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, size_t n) { (void)r; (void)buf; (void)n; return ESP_OK; }

static int s_err_status;
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t error, const char *msg)
{
    (void)r;
    (void)msg;
    s_err_status = (int)error;
    return ESP_OK;
}
static int s_status_line;
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status)
{
    (void)r;
    if (status) {
        s_status_line = atoi(status);
    }
    return ESP_OK;
}
static char s_sendstr[1024];
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s)
{
    (void)r;
    snprintf(s_sendstr, sizeof(s_sendstr), "%s", s ? s : "");
    return ESP_OK;
}
size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field) { (void)r; (void)field; return 0; }
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t n)
{
    (void)r;
    (void)field;
    if (val && n) {
        val[0] = '\0';
    }
    return ESP_FAIL;
}

static const char *s_body;
static size_t s_body_len, s_off;
int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len)
{
    (void)r;
    if (!s_body || s_off >= s_body_len) {
        return 0;
    }
    size_t n = s_body_len - s_off;
    if (n > buf_len) {
        n = buf_len;
    }
    memcpy(buf, s_body + s_off, n);
    s_off += n;
    return (int)n;
}

void ota_http_get_client_ip(httpd_req_t *req, char *out, size_t out_len)
{
    (void)req;
    snprintf(out, out_len, "10.0.0.1");
}
bool ota_http_get_client_ip_checked(httpd_req_t *req, char *out, size_t out_len)
{
    (void)req;
    snprintf(out, out_len, "10.0.0.1");
    return true;
}
bool web_client_accepts_gzip(httpd_req_t *req) { (void)req; return true; }
esp_err_t web_send_gzip_not_acceptable(httpd_req_t *req, const char *tag, const char *page_name)
{ (void)req; (void)tag; (void)page_name; return ESP_OK; }
void web_set_asset_cache_headers(httpd_req_t *r) { (void)r; }
httpd_handle_t wifi_provision_http_get_server(void) { return NULL; }
const char *wifi_prov_get_ap_ssid(void) { return "KilnTestAP"; }
const char *wifi_prov_get_ap_password(void) { return "ap-test-secret"; }
esp_err_t wifi_prov_get_cached_sta_ip_netmask(char *ip_out, size_t ip_cap, char *nm_out, size_t nm_cap)
{
    snprintf(ip_out, ip_cap, "10.0.0.2");
    snprintf(nm_out, nm_cap, "255.0.0.0");
    return ESP_OK;
}
esp_err_t wifi_prov_get_sta_ip_netmask(char *ip_out, size_t ip_cap, char *nm_out, size_t nm_cap)
{
    return wifi_prov_get_cached_sta_ip_netmask(ip_out, ip_cap, nm_out, nm_cap);
}

// ---- auth seams: test-controlled caller role / web-auth policy --------------
static bool s_caller_admin = true;
static bool s_web_enabled = true;
bool http_auth_caller_is_admin(httpd_req_t *req)
{
    (void)req;
    return s_caller_admin;
}
bool http_auth_policy_web_enabled(void)
{
    return s_web_enabled;
}

// ---- time_sync: always synced, test-controlled epoch ------------------------
#define TEST_EPOCH0 1700000000
static time_t s_epoch = TEST_EPOCH0;
void time_sync_get_status(time_sync_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->ever_synced = true;
    out->now_epoch = s_epoch;
    out->last_sync_epoch = s_epoch;
}

// ---- security backend: nothing in this file goes through the vtable ---------
static const security_backend_vtable_t s_fake_backend;
const security_backend_vtable_t *security_backend_get_vtable(void)
{
    return &s_fake_backend;
}

// ---------------------------------------------------------------------------

static const uint8_t TEST_SALT[WEB_AUTH_SALT_LEN] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
static const uint8_t TEST_SECRET[TOTP_SECRET_LEN] = { 11, 22, 33, 44, 55, 66, 77, 88, 99, 10,
                                                      20, 30, 40, 50, 60, 70, 80, 90, 1,  2 };
static const uint8_t TEST_TOKEN_HASH[WEB_AUTH_TOKEN_HASH_LEN] = { 9 };
static const char TEST_RESET_TOKEN[TOTP_RESET_TOKEN_HEX_LEN + 1] = "0123456789abcdef0123456789abcdef";

static void fresh(void)
{
    fake_kv_reset_all();
    fake_kv_set_write_safe_here(true);
    hal_kv_init_partition(NULL);
    fake_time_reset_all();
    fake_sysinfo_reset_all();
    web_auth_table_destroy_all(http_session_table());
    totp_config_ram_reset();
    totp_pending_clear(&s_totp_pending);
    if (s_totp_lock == NULL) {
        s_totp_lock = xSemaphoreCreateMutex();
    }
    auth_totp_http_clear_reset_tokens();
    s_caller_admin = true;
    s_web_enabled = true;
    s_epoch = TEST_EPOCH0;
    g_test_stub_semaphore_take_hook = 0;
    g_test_stub_semaphore_fail_nth = 0;
    // The wipe-request generation is process-global and only grows; sync both
    // httpd-side seen counters so a request posted by an earlier case does not
    // read as pending here.
    s_sec_wipe_seen_gen = totp_wipe_request_gen();
    s_atp_wipe_seen_gen = totp_wipe_request_gen();
}

static void set_admin_credential(void)
{
    TEST_CHECK(web_auth_store_set_password(WEB_AUTH_ROLE_ADMINISTRATOR, "admin", "correct-horse-battery-staple", TEST_SALT,
                                           false) == HAL_OK,
               "setup: admin credential");
}

static void make_session(void)
{
    (void)web_auth_table_create_session(http_session_table(), TEST_TOKEN_HASH, "10.0.0.1", WEB_AUTH_SESSION_ROLE_ADMIN,
                                        1000);
}

static bool session_alive(void)
{
    return web_auth_table_find_by_token(http_session_table(), TEST_TOKEN_HASH) >= 0;
}

static void store_token(void)
{
    totp_reset_token_store(reset_tokens(), TEST_RESET_TOKEN, "admin", now_ms());
}

static bool token_alive(void)
{
    return totp_reset_token_consume(reset_tokens(), TEST_RESET_TOKEN, "admin", now_ms()) == TOTP_RESET_TOKEN_OK;
}

static void post_security(const char *body)
{
    s_body = body;
    s_body_len = strlen(body);
    s_off = 0;
    s_err_status = 0;
    s_status_line = 0;
    s_sendstr[0] = '\0';
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.content_len = (long long)s_body_len;
    (void)security_post_handler(&req);
}

static void get_status(void)
{
    s_err_status = 0;
    s_status_line = 0;
    s_sendstr[0] = '\0';
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    (void)totp_status_get_handler(&req);
}

static bool reply_ok(void)
{
    return strstr(s_sendstr, "\"ok\":true") != NULL;
}

static void code_for(const uint8_t *secret, time_t epoch, char out[8])
{
    snprintf(out, 8, "%06u", (unsigned)totp_hotp_code(secret, TOTP_SECRET_LEN, totp_counter_for_time((uint64_t)epoch)));
}

static void post_with_code(const char *cmd, const char *code)
{
    char body[96];
    snprintf(body, sizeof(body), "cmd=%s&code=%s", cmd, code);
    post_security(body);
}

// Begin an enrollment; returns true and copies the pending secret out.
static bool begin_enrollment(uint8_t secret_out[TOTP_SECRET_LEN])
{
    post_security("cmd=totp_enroll_begin");
    if (!reply_ok() || !totp_pending_is_valid(&s_totp_pending, security_http_now_ms())) {
        return false;
    }
    memcpy(secret_out, s_totp_pending.secret, TOTP_SECRET_LEN);
    return true;
}

static bool secret_is(const uint8_t *want)
{
    uint8_t got[TOTP_SECRET_LEN];
    bool ok = totp_config_load_secret(got) == TOTP_CONFIG_LOAD_OK && memcmp(got, want, TOTP_SECRET_LEN) == 0;
    totp_secure_zero(got, sizeof(got));
    return ok;
}

// ===========================================================================
// Gap 1: enroll begin / confirm / disable through the real handlers
// ===========================================================================
static void test_enroll_flow(void)
{
    TEST_SECTION("security_http totp_enroll_begin / confirm / disable (AUDIT_TOTP gap 1)");
    uint8_t secret[TOTP_SECRET_LEN];
    char code[8];

    fresh();
    set_admin_credential();

    // confirm without begin
    code_for(TEST_SECRET, s_epoch, code);
    post_with_code("totp_enroll_confirm", code);
    TEST_CHECK(!reply_ok() && !totp_config_enrolled(), "confirm with no pending enrollment is refused");

    TEST_CHECK(begin_enrollment(secret), "begin on an unenrolled board returns ok with a valid pending secret");
    TEST_CHECK(strstr(s_sendstr, "secret_base32") != NULL && strstr(s_sendstr, "otpauth://") != NULL,
               "begin reply carries the secret for the authenticator app");

    // wrong code
    post_with_code("totp_enroll_confirm", "000000");
    TEST_CHECK(!reply_ok() && !totp_config_enrolled(), "confirm with a wrong code persists nothing");

    // reset token minted before must not survive a successful confirm
    store_token();
    code_for(secret, s_epoch, code);
    post_with_code("totp_enroll_confirm", code);
    TEST_CHECK(reply_ok(), "confirm with the right code succeeds");
    TEST_CHECK(secret_is(secret), "confirm persisted exactly the pending secret");
    uint32_t last = 0;
    TEST_CHECK(totp_config_load_last_counter(&last) == TOTP_CONFIG_LOAD_OK &&
                   last == (uint32_t)totp_counter_for_time((uint64_t)s_epoch),
               "confirm persisted the used step as the replay counter (the confirming code is not replayable)");
    TEST_CHECK(!token_alive(), "confirm cleared outstanding reset tokens");
    TEST_CHECK(!totp_pending_is_valid(&s_totp_pending, security_http_now_ms()), "confirm cleared the pending secret");

    // already enrolled: begin refused
    post_security("cmd=totp_enroll_begin");
    TEST_CHECK(!reply_ok() && secret_is(secret), "begin on an already-enrolled board is refused, secret untouched");

    // replaying the confirming code against disable must fail (same step)
    post_with_code("totp_disable", code);
    TEST_CHECK(!reply_ok() && totp_config_enrolled(), "disable with the already-used code is refused (replay)");

    // wrong code
    s_epoch += 30;
    post_with_code("totp_disable", "000000");
    TEST_CHECK(!reply_ok() && totp_config_enrolled(), "disable with a wrong code leaves the secret");

    // right code: disables, clears tokens
    store_token();
    code_for(secret, s_epoch, code);
    post_with_code("totp_disable", code);
    TEST_CHECK(reply_ok() && !totp_config_enrolled(), "disable with a fresh valid code removes the secret");
    TEST_CHECK(!token_alive(), "disable cleared outstanding reset tokens");

    // non-admin caller: every totp command refused
    s_caller_admin = false;
    post_security("cmd=totp_enroll_begin");
    TEST_CHECK(!reply_ok() && !totp_pending_is_valid(&s_totp_pending, security_http_now_ms()),
               "begin from a non-admin caller is refused");
    s_caller_admin = true;

    // web auth off: 409 web_auth_disabled for both enrollment commands
    s_web_enabled = false;
    post_security("cmd=totp_enroll_begin");
    TEST_CHECK(s_status_line == 409 && strstr(s_sendstr, "web_auth_disabled") != NULL,
               "begin while web auth is off is a 409 web_auth_disabled");
    TEST_CHECK(!totp_pending_is_valid(&s_totp_pending, security_http_now_ms()), "refused begin left no pending secret");
    s_web_enabled = true;
}

static void test_enroll_persist_failure_rollback(void)
{
    TEST_SECTION("enroll confirm: persist failure rolls back to not-enrolled (AUDIT_TOTP gap 1)");
    uint8_t secret[TOTP_SECRET_LEN];
    char code[8];

    // Fail the write at every point of the commit: whichever write fails, the
    // reply is {"ok":false}, the board ends NOT enrolled and the pending
    // secret is gone.
    for (unsigned skip = 0; skip < 8; skip++) {
        fresh();
        set_admin_credential();
        TEST_CHECK(begin_enrollment(secret), "begin ok");
        code_for(secret, s_epoch, code);
        fake_kv_script_write_status_after(skip, HAL_IO);
        post_with_code("totp_enroll_confirm", code);
        if (reply_ok()) {
            // The scripted failure landed after the commit finished (skip >=
            // number of writes): a genuine success, nothing to roll back.
            TEST_CHECK(secret_is(secret), "late scripted failure: commit fully succeeded");
            continue;
        }
        TEST_CHECK(!totp_config_enrolled(), "a failed confirm leaves the board not enrolled");
        TEST_CHECK(!totp_pending_is_valid(&s_totp_pending, security_http_now_ms()),
                   "a failed confirm drops the pending secret");
    }
}

// ===========================================================================
// LOW-2: write order -- never "enrolled with a replayable confirming code"
// ===========================================================================
static void test_enroll_commit_write_order(void)
{
    TEST_SECTION("totp_config_enroll_commit: no failure point leaves an enrolled secret with a stale counter (LOW-2)");
    const uint32_t counter = 56666667u;
    unsigned failed_runs = 0;
    for (unsigned skip = 0; skip < 12; skip++) {
        fresh();
        fake_kv_script_write_status_after(skip, HAL_IO);
        bool ok = totp_config_enroll_commit(TEST_SECRET, counter, NULL);
        uint32_t last = 0;
        bool have_last = totp_config_load_last_counter(&last) == TOTP_CONFIG_LOAD_OK;
        bool enrolled = totp_config_enrolled();
        if (!ok) {
            failed_runs++;
        }
        TEST_CHECK(!(enrolled && !(have_last && last == counter)),
                   "never enrolled while the persisted replay counter is below the confirming step");
        if (ok) {
            TEST_CHECK(enrolled && have_last && last == counter && secret_is(TEST_SECRET),
                       "a successful commit persisted secret and counter");
        }
    }
    TEST_CHECK(failed_runs >= 2, "sweep actually injected failures at several write points");
}

static int s_still_valid_calls;
static int s_still_valid_depth;
static bool still_valid_yes(void)
{
    s_still_valid_calls++;
    s_still_valid_depth = g_test_stub_lock_depth;
    return true;
}
static bool still_valid_no(void)
{
    s_still_valid_calls++;
    return false;
}

static void test_enroll_commit_guard(void)
{
    TEST_SECTION("totp_config_enroll_commit: still_valid guard runs under the state lock and aborts with no write");
    fresh();
    s_still_valid_calls = 0;
    g_test_stub_lock_depth = 0;
    TEST_CHECK(totp_config_enroll_commit(TEST_SECRET, 5u, still_valid_yes), "commit with a true guard succeeds");
    TEST_CHECK(s_still_valid_calls == 1 && s_still_valid_depth == 1, "guard is called once, with the state lock held");
    TEST_CHECK(g_test_stub_lock_depth == 0, "lock released after the commit");

    fresh();
    s_still_valid_calls = 0;
    TEST_CHECK(!totp_config_enroll_commit(TEST_SECRET, 5u, still_valid_no), "a false guard aborts the commit");
    TEST_CHECK(s_still_valid_calls == 1 && !totp_config_enrolled(), "an aborted commit wrote nothing");
    uint32_t last = 0;
    TEST_CHECK(totp_config_load_last_counter(&last) != TOTP_CONFIG_LOAD_OK || last == 0,
               "an aborted commit wrote no counter either");
    TEST_CHECK(g_test_stub_lock_depth == 0, "lock released after an aborted commit");
}

// ===========================================================================
// LOW-1: every public totp_config entry point takes the state lock exactly once
// ===========================================================================
static int s_takes;
static void count_take_hook(void)
{
    s_takes++;
    g_test_stub_semaphore_take_hook = count_take_hook; // re-arm; the stub clears it before each call
}

static int takes_during_set_secret(void) { s_takes = 0; g_test_stub_semaphore_take_hook = count_take_hook; (void)totp_config_set_secret(TEST_SECRET); g_test_stub_semaphore_take_hook = 0; return s_takes; }
static int takes_during_set_counter(void) { s_takes = 0; g_test_stub_semaphore_take_hook = count_take_hook; (void)totp_config_set_last_counter(7u); g_test_stub_semaphore_take_hook = 0; return s_takes; }
static int takes_during_ram_counter(void) { uint32_t v; s_takes = 0; g_test_stub_semaphore_take_hook = count_take_hook; (void)totp_config_ram_last_counter(&v); g_test_stub_semaphore_take_hook = 0; return s_takes; }
static int takes_during_ram_reset(void) { s_takes = 0; g_test_stub_semaphore_take_hook = count_take_hook; totp_config_ram_reset(); g_test_stub_semaphore_take_hook = 0; return s_takes; }
static int takes_during_clear(void) { s_takes = 0; g_test_stub_semaphore_take_hook = count_take_hook; (void)totp_config_clear(); g_test_stub_semaphore_take_hook = 0; return s_takes; }
static int takes_during_verify(void) { s_takes = 0; g_test_stub_semaphore_take_hook = count_take_hook; (void)totp_config_verify_and_consume("000000", (uint64_t)s_epoch); g_test_stub_semaphore_take_hook = 0; return s_takes; }
static int takes_during_commit(void) { s_takes = 0; g_test_stub_semaphore_take_hook = count_take_hook; (void)totp_config_enroll_commit(TEST_SECRET, 3u, NULL); g_test_stub_semaphore_take_hook = 0; return s_takes; }

static void test_state_lock(void)
{
    TEST_SECTION("totp_config state lock: one take per public entry point, none nested (AUDIT_TOTP LOW-1)");
    fresh();
    TEST_CHECK(takes_during_set_secret() == 1, "set_secret takes the lock once");
    TEST_CHECK(takes_during_set_counter() == 1, "set_last_counter takes the lock once");
    TEST_CHECK(takes_during_ram_counter() == 1, "ram_last_counter takes the lock once");
    TEST_CHECK(takes_during_verify() == 1, "verify_and_consume takes the lock once (verify + persist under one hold)");
    TEST_CHECK(takes_during_clear() == 1, "clear takes the lock once (the LCD gesture's path)");
    TEST_CHECK(takes_during_ram_reset() == 1, "ram_reset takes the lock once");
    TEST_CHECK(takes_during_commit() == 1, "enroll_commit takes the lock once");
    TEST_CHECK(g_test_stub_lock_depth == 0, "every hold was released");

    // The race the lock closes: the other task (gesture clear) acts while
    // verify_and_consume waits for the lock. After the verify finishes, the
    // clear must be the LAST word -- no stale counter cache surviving it.
    fresh();
    TEST_CHECK(totp_config_set_secret(TEST_SECRET), "setup: enrolled");
    char code[8];
    code_for(TEST_SECRET, s_epoch, code);
    (void)totp_config_verify_and_consume(code, (uint64_t)s_epoch);
    (void)totp_config_clear();
    uint32_t v = 123;
    TEST_CHECK(!totp_config_enrolled() && (!totp_config_ram_last_counter(&v) || v == 0),
               "after clear no counter cache or secret survives");
}

// ===========================================================================
// Gap 5 / MED-1: the REAL gesture wiring wipes everything
// ===========================================================================
static bool run_gesture(void)
{
    auth_reset_gesture_state_t *g = auth_reset_gesture_singleton();
    TEST_CHECK(g->clear_credentials_fn != NULL, "singleton wires a clear function");
    return g->clear_credentials_fn ? g->clear_credentials_fn() : false;
}

static void touch_forgot_handler(void)
{
    // Any request reaching the OPEN handlers applies the posted wipe request
    // first (before sntp/backoff/body checks); an empty body is enough.
    s_body = "";
    s_body_len = 0;
    s_off = 0;
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.content_len = 0;
    (void)forgot_post_handler(&req);
}

static void test_gesture_wipes_everything(void)
{
    TEST_SECTION("LCD four-corner reset (real wiring) clears credentials, secret, sessions, tokens (AUDIT_TOTP MED-1/gap 5)");

    fresh();
    set_admin_credential();
    TEST_CHECK(totp_config_set_secret(TEST_SECRET), "setup: TOTP enrolled");
    make_session();
    store_token();
    TEST_CHECK(session_alive(), "setup: session exists");
    TEST_CHECK(web_auth_store_password_configured(WEB_AUTH_ROLE_ADMINISTRATOR), "setup: credential configured");

    TEST_CHECK(run_gesture(), "gesture clear reports success");
    TEST_CHECK(!web_auth_store_password_configured(WEB_AUTH_ROLE_ADMINISTRATOR), "gesture cleared the web credential");
    TEST_CHECK(!totp_config_enrolled(), "gesture cleared the TOTP secret");
    TEST_CHECK(!session_alive(), "gesture ended every live web session");
    touch_forgot_handler();
    TEST_CHECK(!token_alive(), "reset token minted before the gesture is dead after the next route hit");

    // Pending enrollment begun by a pre-reset session must not survive.
    fresh();
    set_admin_credential();
    make_session();
    uint8_t secret[TOTP_SECRET_LEN];
    char code[8];
    TEST_CHECK(begin_enrollment(secret), "setup: enrollment pending");
    TEST_CHECK(run_gesture(), "gesture clear reports success (pending case)");
    TEST_CHECK(!session_alive(), "pending case: sessions ended");
    code_for(secret, s_epoch, code);
    post_with_code("totp_enroll_confirm", code);
    TEST_CHECK(!reply_ok(), "confirm of a pre-gesture pending enrollment is refused");
    TEST_CHECK(!totp_config_enrolled(), "pre-gesture pending enrollment did not enroll after the reset");

    // A NEW enrollment begun after the gesture is not wiped by the old request.
    TEST_CHECK(begin_enrollment(secret), "post-gesture begin works");
    code_for(secret, s_epoch, code);
    post_with_code("totp_enroll_confirm", code);
    TEST_CHECK(reply_ok() && secret_is(secret), "post-gesture enrollment confirms (old wipe request applied once only)");
}

static void test_hook_run_gesture(void)
{
    (void)auth_reset_gesture_singleton()->clear_credentials_fn();
}

static void test_gesture_cannot_be_overtaken_by_inflight_confirm(void)
{
    TEST_SECTION("gesture lands between the confirm's code check and its commit: nothing is enrolled");
    fresh();
    set_admin_credential();
    uint8_t secret[TOTP_SECRET_LEN];
    char code[8];
    TEST_CHECK(begin_enrollment(secret), "setup: enrollment pending");
    code_for(secret, s_epoch, code);
    // The first state-lock take inside confirm's commit is where the other
    // task (the LVGL gesture) gets in: model it with the take hook.
    g_test_stub_semaphore_take_hook = test_hook_run_gesture;
    post_with_code("totp_enroll_confirm", code);
    TEST_CHECK(!reply_ok(), "confirm racing the gesture does not report success");
    TEST_CHECK(!totp_config_enrolled(), "a secret never lands after the physical reset wiped the board");
}

// ===========================================================================
// Gap 7: the totp_status reply never contains the secret
// ===========================================================================
static bool reply_leaks(const uint8_t *secret)
{
    char b32[64];
    size_t n = totp_base32_encode(secret, TOTP_SECRET_LEN, b32, sizeof(b32));
    if (n > 0 && strstr(s_sendstr, b32) != NULL) {
        return true;
    }
    char hex[2 * TOTP_SECRET_LEN + 1];
    for (size_t i = 0; i < TOTP_SECRET_LEN; i++) {
        snprintf(hex + 2 * i, 3, "%02x", secret[i]);
    }
    if (strstr(s_sendstr, hex) != NULL) {
        return true;
    }
    return strstr(s_sendstr, "secret") != NULL || strstr(s_sendstr, "otpauth") != NULL;
}

static void test_status_never_leaks_secret(void)
{
    TEST_SECTION("GET /api/auth/totp_status never contains the secret (AUDIT_TOTP gap 7)");
    fresh();
    set_admin_credential();
    get_status();
    TEST_CHECK(strstr(s_sendstr, "\"enrolled\":false") != NULL, "status reports not enrolled");
    uint8_t pending[TOTP_SECRET_LEN];
    TEST_CHECK(begin_enrollment(pending), "setup: pending enrollment");
    get_status();
    TEST_CHECK(!reply_leaks(pending), "status during a pending enrollment does not leak the pending secret");

    fresh();
    TEST_CHECK(totp_config_set_secret(TEST_SECRET), "setup: enrolled");
    get_status();
    TEST_CHECK(strstr(s_sendstr, "\"enrolled\":true") != NULL, "status reports enrolled");
    TEST_CHECK(!reply_leaks(TEST_SECRET), "status of an enrolled board does not leak the stored secret");
}

int main(void)
{
    g_test_stub_semaphore_take_default = 1; /* pdTRUE */
    test_enroll_flow();
    test_enroll_persist_failure_rollback();
    test_enroll_commit_write_order();
    test_enroll_commit_guard();
    test_state_lock();
    test_gesture_wipes_everything();
    test_gesture_cannot_be_overtaken_by_inflight_confirm();
    test_status_never_leaks_secret();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
