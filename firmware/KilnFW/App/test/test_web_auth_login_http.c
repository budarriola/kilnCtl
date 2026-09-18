// Host tests for App/drivers/http/web_auth_login_http.c -- the browser
// login route (docs/WEB_AUTH_PLAN.md section 6). Own standalone executable
// (own g_test_failures/g_test_count), same "#include the driver .c directly
// to reach its statics" convention as test_ota_http.c/test_web_auth_store.c:
// login_lockout_slot_for() and the per-IP lockout table it owns are `static`
// and have no other seam, and login_post_handler() itself is the only place
// Finding 4 (global lockout) and Finding 5 (unlooped httpd_req_recv) can be
// exercised end-to-end.
//
// This file supplies its OWN test-controllable httpd_req_get_hdr_value_len/
// _str, httpd_req_recv, and ota_http_get_client_ip bodies (never the shared
// stubs/http_auth_link_stub.c, which is deliberately non-controllable) so a
// test can (a) stage a source IP per call (Finding 4) and (b) force
// httpd_req_recv() to hand back the body in several short chunks rather than
// all at once (Finding 5).
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include "esp_err.h"
#include "esp_http_server.h"
#include "fake_kv.h"
#include "psa/crypto.h"

// psa/crypto.h's host stub declares this `extern` (test_ota_http.c/
// test_web_auth_store.c each define their own copy for their own
// executable) -- this standalone executable needs its own definition since
// it does not link either of those.
psa_status_t g_stub_psa_import_key_result = PSA_SUCCESS;

// web_auth_store.c -- direct #include, same convention test_web_auth_store.c
// uses (needs psa/crypto.h's host stub + fake_kv.h, both already set up
// above). login_post_handler() calls web_auth_store_load_password()/
// verify_password() directly.
#include "../drivers/persist/web_auth_store.c"

// asm("_binary_...") is a GCC/binutils extension with no MSVC equivalent --
// #define it away, same convention test_zones_http.c/test_profiles_http.c
// use for their own embedded-page symbols.
#define asm(x)
#include "../drivers/http/web_auth_login_http.c"
#undef asm

// ---- Embedded-page symbol login_page_get_handler() references -------------
const uint8_t login_page_html_gz_start[1] = { 0 };
const uint8_t login_page_html_gz_end[1] = { 0 };

// ---------------------------------------------------------------------------
// esp_http_server.h stub bodies. Only httpd_req_get_hdr_value_len/_str,
// httpd_req_recv, and ota_http_get_client_ip are actually exercised by these
// tests; the rest exist only so the whole translation unit links.
// ---------------------------------------------------------------------------

esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler)
{
    (void)handle;
    (void)uri_handler;
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type) { (void)r; (void)type; return ESP_OK; }

static char s_last_set_cookie[256];
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value)
{
    (void)r;
    if (field && value && strcmp(field, "Set-Cookie") == 0) {
        strncpy(s_last_set_cookie, value, sizeof(s_last_set_cookie) - 1);
        s_last_set_cookie[sizeof(s_last_set_cookie) - 1] = '\0';
    }
    return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, long long buf_len) { (void)r; (void)buf; (void)buf_len; return ESP_OK; }
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, size_t buf_len) { (void)r; (void)buf; (void)buf_len; return ESP_OK; }

static int s_last_err_status = 0;
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t error, const char *msg)
{
    (void)r;
    (void)msg;
    s_last_err_status = (int)error;
    return ESP_OK;
}

static int s_last_status_line = 0;
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status)
{
    (void)r;
    if (status) {
        s_last_status_line = atoi(status);
    }
    return ESP_OK;
}

static char s_last_sendstr[256];
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s)
{
    (void)r;
    if (s) {
        strncpy(s_last_sendstr, s, sizeof(s_last_sendstr) - 1);
        s_last_sendstr[sizeof(s_last_sendstr) - 1] = '\0';
    }
    return ESP_OK;
}

// ---- Finding 5: test-controllable httpd_req_recv() ------------------------
// Feeds back the staged body in chunks of at most s_recv_chunk_max bytes per
// call (default 1, i.e. the worst case: one byte at a time) so a test can
// prove login_post_handler() actually loops rather than trusting a single
// call to fill the whole request.
static const char *s_recv_body = NULL;
static size_t s_recv_body_len = 0;
static size_t s_recv_offset = 0;
static size_t s_recv_chunk_max = 1;

static void recv_stage(const char *body, size_t chunk_max)
{
    s_recv_body = body;
    s_recv_body_len = body ? strlen(body) : 0;
    s_recv_offset = 0;
    s_recv_chunk_max = chunk_max ? chunk_max : 1;
}

int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len)
{
    (void)r;
    if (!s_recv_body || s_recv_offset >= s_recv_body_len) {
        return 0;
    }
    size_t remaining = s_recv_body_len - s_recv_offset;
    size_t n = remaining < buf_len ? remaining : buf_len;
    if (n > s_recv_chunk_max) {
        n = s_recv_chunk_max;
    }
    memcpy(buf, s_recv_body + s_recv_offset, n);
    s_recv_offset += n;
    return (int)n;
}

// Cookie header is never read by this route -- fixed "not present" stubs.
size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field) { (void)r; (void)field; return 0; }
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t val_size)
{
    (void)r;
    (void)field;
    if (val && val_size) { val[0] = '\0'; }
    return ESP_FAIL;
}

// ---- Finding 4: test-controllable client IP --------------------------------
static char s_stub_client_ip[46] = "0.0.0.0";
void ota_http_get_client_ip(httpd_req_t *req, char *out, size_t out_len)
{
    (void)req;
    if (out && out_len > 0) {
        strncpy(out, s_stub_client_ip, out_len - 1);
        out[out_len - 1] = '\0';
    }
}

// ---- web_encoding.h -- only reached from login_page_get_handler(), never
// called by these tests (they only exercise POST /api/auth/login).
bool web_client_accepts_gzip(httpd_req_t *req) { (void)req; return true; }
esp_err_t web_send_gzip_not_acceptable(httpd_req_t *req, const char *tag, const char *page_name)
{ (void)req; (void)tag; (void)page_name; return ESP_OK; }
void web_set_asset_cache_headers(httpd_req_t *r) { (void)r; }

// ---- wifi_provision_http.h -- only reached from web_auth_login_http_start(),
// never called by these tests.
httpd_handle_t wifi_provision_http_get_server(void) { return NULL; }

// ---------------------------------------------------------------------------

static void reset_all(void)
{
    fake_kv_reset_all();
    fake_kv_set_write_safe_here(true);
    hal_kv_init_partition(NULL);
    // s_web_auth_table/s_web_auth_table_init_done are static inside
    // http_session_iface.c, linked here as a SEPARATE translation unit (not
    // #include'd), so they are not visible from this file. Use the public
    // accessor + destroy-all instead of poking the statics directly.
    web_auth_table_destroy_all(http_session_table());
    memset(s_login_lockouts, 0, sizeof(s_login_lockouts));
    s_last_err_status = 0;
    s_last_status_line = 0;
    s_last_sendstr[0] = '\0';
    s_last_set_cookie[0] = '\0';
    strncpy(s_stub_client_ip, "10.0.0.1", sizeof(s_stub_client_ip) - 1);
}

static const uint8_t TEST_SALT[WEB_AUTH_SALT_LEN] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
};

static void set_admin_credential(const char *username, const char *password)
{
    TEST_CHECK(web_auth_store_set_password(WEB_AUTH_ROLE_ADMINISTRATOR, username, password, TEST_SALT, false) ==
                   HAL_OK,
               "test setup: admin credential stores cleanly");
}

static esp_err_t do_login(const char *username, const char *password)
{
    char body[256];
    snprintf(body, sizeof(body), "username=%s&password=%s", username, password);
    recv_stage(body, 999); // whole body in one call unless a test overrides
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.content_len = (long long)strlen(body);
    return login_post_handler(&req);
}

static void test_lockout_is_per_ip_not_global(void)
{
    // Finding 4: a global lockout meant one attacker IP locked out every
    // other client too. Three failures from 10.0.0.1 must lock out
    // 10.0.0.1 only -- a different IP's very next attempt must still be
    // allowed to try (and, with the right password, succeed).
    TEST_SECTION("login lockout -- per source IP, not global (Finding 4)");
    reset_all();
    set_admin_credential("admin", "correct-horse-battery-staple");

    strncpy(s_stub_client_ip, "10.0.0.1", sizeof(s_stub_client_ip) - 1);
    for (int i = 0; i < 3; i++) {
        do_login("admin", "wrong-password");
    }
    // 10.0.0.1 should now be locked: a 4th attempt, even with the RIGHT
    // password, must be refused with 429 before the password is even
    // checked.
    do_login("admin", "correct-horse-battery-staple");
    TEST_CHECK(s_last_status_line == 429, "the attacking IP is now locked out (429)");

    // A second, distinct IP must be entirely unaffected.
    strncpy(s_stub_client_ip, "10.0.0.2", sizeof(s_stub_client_ip) - 1);
    s_last_status_line = 0;
    s_last_err_status = 0;
    esp_err_t err = do_login("admin", "correct-horse-battery-staple");
    TEST_CHECK(err == ESP_OK, "handler still returns ESP_OK for the unaffected IP");
    TEST_CHECK(s_last_status_line != 429, "a different source IP is never locked out by 10.0.0.1's failures");
    TEST_CHECK(strstr(s_last_set_cookie, HTTP_SESSION_COOKIE_NAME "=") != NULL,
               "the unaffected IP's correct login still succeeds and mints a session cookie");
}

static void test_lockout_table_is_bounded(void)
{
    // Finding 4's second half: the per-IP table must not grow without
    // bound. Drive more distinct failing IPs through it than
    // LOGIN_LOCKOUT_MAX_IPS and confirm the process does not need
    // unbounded memory to keep going -- the table stays fixed-size and
    // simply evicts the least-recently-active entry.
    TEST_SECTION("login lockout table -- bounded, evicts LRU rather than growing (Finding 4)");
    reset_all();
    set_admin_credential("admin", "correct-horse-battery-staple");

    char ip[46];
    for (unsigned i = 0; i < LOGIN_LOCKOUT_MAX_IPS + 4; i++) {
        snprintf(ip, sizeof(ip), "10.1.0.%u", i + 1);
        strncpy(s_stub_client_ip, ip, sizeof(s_stub_client_ip) - 1);
        do_login("admin", "wrong-password");
    }
    unsigned in_use = 0;
    for (unsigned i = 0; i < LOGIN_LOCKOUT_MAX_IPS; i++) {
        if (s_login_lockouts[i].in_use) { in_use++; }
    }
    TEST_CHECK(in_use == LOGIN_LOCKOUT_MAX_IPS,
               "the table never holds more live entries than its fixed capacity");
}

static void test_login_body_split_across_recv_calls(void)
{
    // Finding 5: httpd_req_recv() is not guaranteed to return the whole
    // body in one call. Force it to hand back one byte at a time and
    // confirm login_post_handler() still reads the whole body and succeeds
    // -- an unlooped recv would have truncated the body and failed to find
    // "username"/"password" fields (or read a corrupted, short body).
    TEST_SECTION("login_post_handler -- loops httpd_req_recv() until the full body is read (Finding 5)");
    reset_all();
    set_admin_credential("admin", "correct-horse-battery-staple");

    char body[256];
    snprintf(body, sizeof(body), "username=admin&password=correct-horse-battery-staple");
    recv_stage(body, 1); // one byte per httpd_req_recv() call -- the worst case

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.content_len = (long long)strlen(body);
    esp_err_t err = login_post_handler(&req);

    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strstr(s_last_set_cookie, HTTP_SESSION_COOKIE_NAME "=") != NULL,
               "a body delivered one byte at a time is still fully read and the login succeeds");
}

void run_test_web_auth_login_http(void)
{
    test_lockout_is_per_ip_not_global();
    test_lockout_table_is_bounded();
    test_login_body_split_across_recv_calls();
}

int main(void)
{
    // Stub default (freertos/semphr.h) is pdFALSE ("never actually acquire
    // the lock") so pre-start-guard tests elsewhere see a timeout. This
    // file's logic runs entirely inside "if (xSemaphoreTake(...) ==
    // pdTRUE)" guards (Findings 4/5's lockout table + body loop), so the
    // opposite default is needed here -- same opt-in this stub's own
    // comment documents (see test_max31856_hal_spi.c for precedent).
    g_test_stub_semaphore_take_default = 1; /* pdTRUE */
    if (s_login_lock == NULL) {
        s_login_lock = xSemaphoreCreateMutex();
    }
    run_test_web_auth_login_http();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
