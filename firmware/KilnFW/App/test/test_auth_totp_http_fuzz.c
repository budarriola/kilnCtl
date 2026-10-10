// Host fuzz tests for App/drivers/http/auth_totp_http.c -- the OPEN-tier
// POST /api/auth/forgot and POST /api/auth/reset routes (HTTP body fuzz
// campaign part 3, docs/audits/HTTP_PARSER_TEST_FINDINGS_2026-10-09.md).
//
// Own standalone executable. auth_totp_http.c is #include'd directly so its
// static handlers and per-IP backoff table are reachable; the credential
// store (web_auth_store.c), TOTP secret/counter store (totp_config.c) and the
// session table are the REAL modules over fake_kv, so "credential state did
// not change" is asserted against the real thing, not a counter on a stub.
// Only the password-setter vtable is a counting fake: a refused body must
// never reach it.
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
#include "fake_time.h"
#include "psa/crypto.h"

psa_status_t g_stub_psa_import_key_result = PSA_SUCCESS;

#include "../drivers/persist/web_auth_store.c"

#include "../drivers/net/time_sync.h"

#define asm(x)
#include "../drivers/http/auth_totp_http.c"
#undef asm

// ---- httpd stubs -----------------------------------------------------------
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler)
{
    (void)handle;
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
static char s_sendstr[256];
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

// recv with fail-after hook (0 = peer closed, <0 = error)
static const char *s_body;
static size_t s_body_len, s_off, s_chunk = 999;
static size_t s_fail_after = (size_t)-1;
static int s_fail_ret;
// A handler loop mutated to keep reading on EOF/error would spin forever. Cap
// the calls per request; past the cap return an error so the loop ends and
// fuzz_post() reports the spin as a test FAILURE instead of a hang.
#define FUZZ_RECV_CALL_CAP 1000
static int s_recv_calls;
static bool s_recv_spun;
int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len)
{
    (void)r;
    if (++s_recv_calls > FUZZ_RECV_CALL_CAP) {
        s_recv_spun = true;
        return -1;
    }
    if (s_off >= s_fail_after) {
        return s_fail_ret;
    }
    if (!s_body || s_off >= s_body_len) {
        return 0;
    }
    size_t rem = s_body_len - s_off;
    if (s_fail_after != (size_t)-1 && s_off + rem > s_fail_after) {
        rem = s_fail_after - s_off;
    }
    size_t n = rem < buf_len ? rem : buf_len;
    if (n > s_chunk) {
        n = s_chunk;
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

// ---- time_sync: always synced, fixed epoch ----------------------------------
#define FUZZ_NOW_EPOCH 1700000000
void time_sync_get_status(time_sync_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->ever_synced = true;
    out->now_epoch = (time_t)FUZZ_NOW_EPOCH;
    out->last_sync_epoch = (time_t)FUZZ_NOW_EPOCH;
}

// ---- security backend: counting password setter -----------------------------
static int s_setpw_calls;
static security_err_t fake_set_web_password(security_role_t role, const char *username, const char *password)
{
    (void)role;
    (void)username;
    // Mirror the real backend's strength gate: a weak password is refused and
    // changes nothing (handler delegates strength to the backend).
    if (!password || strlen(password) < 12) {
        return SECURITY_ERR_WEAK;
    }
    s_setpw_calls++;
    return SECURITY_OK;
}
static const security_backend_vtable_t s_fake_backend = { .set_web_password = fake_set_web_password };
const security_backend_vtable_t *security_backend_get_vtable(void)
{
    return &s_fake_backend;
}

// ---------------------------------------------------------------------------

static const uint8_t TEST_SALT[WEB_AUTH_SALT_LEN] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
static const uint8_t TEST_SECRET[TOTP_SECRET_LEN] = { 11, 22, 33, 44, 55, 66, 77, 88, 99, 10,
                                                      20, 30, 40, 50, 60, 70, 80, 90, 1,  2 };
static const uint8_t TEST_TOKEN_HASH[WEB_AUTH_TOKEN_HASH_LEN] = { 9 };

static void fuzz_fresh(void)
{
    fake_kv_reset_all();
    fake_kv_set_write_safe_here(true);
    hal_kv_init_partition(NULL);
    fake_time_reset_all();
    web_auth_table_destroy_all(http_session_table());
    TEST_CHECK(web_auth_store_set_password(WEB_AUTH_ROLE_ADMINISTRATOR, "admin", "correct-horse-battery-staple", TEST_SALT,
                                           false) == HAL_OK,
               "setup: admin credential");
    totp_config_ram_reset();
    TEST_CHECK(totp_config_set_secret(TEST_SECRET), "setup: TOTP enrolled");
    auth_totp_http_clear_reset_tokens();
    memset(s_totp_lockouts, 0, sizeof(s_totp_lockouts));
    memset(&s_remote_totp_slot, 0, sizeof(s_remote_totp_slot));
    memset(&s_forgot_board_cap, 0, sizeof(s_forgot_board_cap));
    if (s_totp_lock == NULL) {
        s_totp_lock = xSemaphoreCreateMutex();
    }
    (void)web_auth_table_create_session(http_session_table(), TEST_TOKEN_HASH, "10.0.0.1", WEB_AUTH_SESSION_ROLE_ADMIN,
                                        1000);
    s_setpw_calls = 0;
}

static int fuzz_session_count(void)
{
    return web_auth_table_find_by_token(http_session_table(), TEST_TOKEN_HASH) >= 0 ? 1 : 0;
}

static bool fuzz_state_intact(void)
{
    web_auth_password_record_t rec;
    uint32_t ctr = 0;
    return web_auth_store_load_password(WEB_AUTH_ROLE_ADMINISTRATOR, &rec) == WEB_AUTH_LOAD_OK && rec.configured &&
           strcmp(rec.username, "admin") == 0 &&
           web_auth_store_verify_password(WEB_AUTH_ROLE_ADMINISTRATOR, "correct-horse-battery-staple") &&
           totp_config_enrolled() && s_setpw_calls == 0 && fuzz_session_count() == 1 &&
           (!totp_config_ram_last_counter(&ctr) || ctr == 0);
}

static int fuzz_post(esp_err_t (*h)(httpd_req_t *), const char *body, long long clen, size_t fail_after, int fail_ret)
{
    s_body = body;
    s_body_len = body ? strlen(body) : 0;
    s_off = 0;
    s_fail_after = fail_after;
    s_fail_ret = fail_ret;
    s_err_status = 0;
    s_status_line = 0;
    s_sendstr[0] = '\0';
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.content_len = clen;
    s_recv_calls = 0;
    s_recv_spun = false;
    (void)h(&req);
    TEST_CHECK(!s_recv_spun, "handler recv loop terminates on EOF/error (no spin)");
    s_fail_after = (size_t)-1;
    return s_err_status ? s_err_status : s_status_line;
}

static bool fuzz_minted_token_consumable(const char *username)
{
    // Pull the token out of {"reset_token":"<hex>"} and try to spend it.
    const char *p = strstr(s_sendstr, "\"reset_token\":\"");
    if (!p) {
        return false;
    }
    p += strlen("\"reset_token\":\"");
    char tok[TOTP_RESET_TOKEN_HEX_LEN + 1];
    memcpy(tok, p, TOTP_RESET_TOKEN_HEX_LEN);
    tok[TOTP_RESET_TOKEN_HEX_LEN] = '\0';
    return totp_reset_token_consume(reset_tokens(), tok, username, now_ms()) == TOTP_RESET_TOKEN_OK;
}

static void test_fuzz_forgot(void)
{
    TEST_SECTION("POST /api/auth/forgot -- hostile bodies mint no usable token and consume no code");
    const char *good = "username=admin&code=000000";
    size_t gl = strlen(good);
    int st;

    // Every proper truncation (matching Content-Length): never a usable token.
    for (size_t cut = 0; cut < gl; cut++) {
        char b[64];
        memcpy(b, good, cut);
        b[cut] = '\0';
        fuzz_fresh();
        st = fuzz_post(forgot_post_handler, b, (long long)cut, (size_t)-1, 0);
        TEST_CHECK(!fuzz_minted_token_consumable("admin") && fuzz_state_intact(), "truncated forgot body mints nothing");
        (void)st;
    }
    // recv error / EOF at every offset.
    for (size_t at = 0; at < gl; at++) {
        for (int ret = -1; ret <= 0; ret++) {
            fuzz_fresh();
            st = fuzz_post(forgot_post_handler, good, (long long)gl, at, ret);
            TEST_CHECK(st == 400 && strstr(s_sendstr, "reset_token") == NULL && fuzz_state_intact(),
                       "forgot: recv failure mid-body is a 400, nothing minted");
        }
    }
    long long lens[] = { 0, -1, 128, 129, 100000000LL, 0x7fffffffffffffffLL, (long long)gl + 40 };
    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        fuzz_fresh();
        st = fuzz_post(forgot_post_handler, good, lens[i], (size_t)-1, 0);
        TEST_CHECK(st == 400 && strstr(s_sendstr, "reset_token") == NULL && fuzz_state_intact(),
                   "forgot: bad Content-Length is a 400, nothing minted");
    }
    const char *bad[] = { "username=admin", "code=000000", "username=&code=", "username=admin&code=%00",
                          "username=admin%00&code=000000", "username=admin&code=00%", "username=admin&code=%zz",
                          "xusername=admin&xcode=000000", "username[]=admin&code[]=000000",
                          "username=admin&code=0000000000000000000", "" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        fuzz_fresh();
        st = fuzz_post(forgot_post_handler, bad[i], (long long)strlen(bad[i]), (size_t)-1, 0);
        char msg[120];
        snprintf(msg, sizeof(msg), "forgot: no usable token, state intact: %s", bad[i]);
        TEST_CHECK(!fuzz_minted_token_consumable("admin") && fuzz_state_intact(), msg);
    }
    // Wrong code with the right shape: 202 anti-oracle token must not be spendable.
    fuzz_fresh();
    st = fuzz_post(forgot_post_handler, good, (long long)gl, (size_t)-1, 0);
    TEST_CHECK(st == 202 && !fuzz_minted_token_consumable("admin") && fuzz_state_intact(),
               "forgot: wrong code answers 202 but the token is unusable and the counter unchanged");

    // Control: a correct code mints a spendable token.
    fuzz_fresh();
    char cbody[64];
    uint64_t counter = totp_counter_for_time((uint64_t)FUZZ_NOW_EPOCH);
    snprintf(cbody, sizeof(cbody), "username=admin&code=%06u",
             (unsigned)totp_hotp_code(TEST_SECRET, TOTP_SECRET_LEN, counter));
    st = fuzz_post(forgot_post_handler, cbody, (long long)strlen(cbody), (size_t)-1, 0);
    TEST_CHECK(st == 202 && fuzz_minted_token_consumable("admin"), "control: a correct code mints a spendable token");
}

static void test_fuzz_reset(void)
{
    TEST_SECTION("POST /api/auth/reset -- hostile bodies never set the password or drop sessions");
    const char *tok = "0123456789abcdef0123456789abcdef";
    char good[256];
    snprintf(good, sizeof(good), "username=admin&reset_token=%s&new_password=brand-new-passphrase-1", tok);
    size_t gl = strlen(good);
    int st;

    // Every proper truncation, with and without a stored matching token.
    for (size_t cut = 0; cut < gl; cut++) {
        char b[256];
        memcpy(b, good, cut);
        b[cut] = '\0';
        fuzz_fresh();
        totp_reset_token_store(reset_tokens(), tok, "admin", now_ms());
        st = fuzz_post(reset_post_handler, b, (long long)cut, (size_t)-1, 0);
        // A prefix that still names all three fields with the full token is a
        // valid request (the password is just shorter); only the fields' absence
        // or a bad token must refuse. Everything shorter than the full token
        // field must leave state intact.
        const char *tokpos = strstr(good, tok);
        size_t tok_end = (size_t)(tokpos - good) + strlen(tok);
        const char *pw = strstr(good, "new_password=");
        size_t pw_start = (size_t)(pw - good) + strlen("new_password=");
        if (cut < tok_end || cut < pw_start + 12) {
            char msg[100];
            snprintf(msg, sizeof(msg), "truncated reset body (%zu bytes) changes nothing", cut);
            TEST_CHECK(fuzz_state_intact(), msg);
        }
        (void)st;
    }
    for (size_t at = 0; at < gl; at++) {
        for (int ret = -1; ret <= 0; ret++) {
            fuzz_fresh();
            totp_reset_token_store(reset_tokens(), tok, "admin", now_ms());
            st = fuzz_post(reset_post_handler, good, (long long)gl, at, ret);
            TEST_CHECK(st == 400 && fuzz_state_intact(), "reset: recv failure mid-body changes nothing");
            TEST_CHECK(totp_reset_token_consume(reset_tokens(), tok, "admin", now_ms()) == TOTP_RESET_TOKEN_OK,
                       "reset: recv failure does not burn the token");
        }
    }
    long long lens[] = { 0, -1, 512, 513, 100000000LL, 0x7fffffffffffffffLL, (long long)gl + 40 };
    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        fuzz_fresh();
        totp_reset_token_store(reset_tokens(), tok, "admin", now_ms());
        st = fuzz_post(reset_post_handler, good, lens[i], (size_t)-1, 0);
        TEST_CHECK(st == 400 && fuzz_state_intact(), "reset: bad Content-Length changes nothing");
    }
    struct {
        const char *body;
        const char *why;
    } badr[] = {
        { "username=admin&new_password=brand-new-passphrase-1", "no token" },
        { "username=admin&reset_token=0123456789abcdef0123456789abcdef", "no password" },
        { "reset_token=0123456789abcdef0123456789abcdef&new_password=brand-new-passphrase-1", "no username" },
        { "username=admin&reset_token=0123456789abcdef0123456789abcde&new_password=brand-new-passphrase-1",
          "short token" },
        { "username=admin&reset_token=0123456789abcdef0123456789abcdef0&new_password=brand-new-passphrase-1",
          "long token" },
        { "username=admin&reset_token=0123456789abcdef0123456789abcdeg&new_password=brand-new-passphrase-1",
          "wrong token" },
        { "username=root&reset_token=0123456789abcdef0123456789abcdef&new_password=brand-new-passphrase-1",
          "wrong username" },
        { "username=ADMIN&reset_token=0123456789abcdef0123456789abcdef&new_password=brand-new-passphrase-1",
          "username case" },
        { "username=admin&reset_token=%30123456789abcdef0123456789abcdef&new_password=x%00y", "NUL in password" },
        { "username=admin%00&reset_token=0123456789abcdef0123456789abcdef&new_password=brand-new-passphrase-1",
          "NUL in username" },
        { "username=admin&reset_token=0123456789abcdef0123456789abcdef&new_password=%zz", "bad escape in password" },
        { "xusername=admin&xreset_token=0123456789abcdef0123456789abcdef&xnew_password=brand-new-passphrase-1",
          "prefix collisions" },
        { "username=admin&reset_token=&new_password=", "empty fields" },
    };
    for (size_t i = 0; i < sizeof(badr) / sizeof(badr[0]); i++) {
        fuzz_fresh();
        totp_reset_token_store(reset_tokens(), tok, "admin", now_ms());
        st = fuzz_post(reset_post_handler, badr[i].body, (long long)strlen(badr[i].body), (size_t)-1, 0);
        char msg[160];
        snprintf(msg, sizeof(msg), "reset refused, password and sessions unchanged: %s", badr[i].why);
        // "wrong username" must not spend the stored admin token either, but the
        // token table only promises the credential state; assert that.
        TEST_CHECK(st >= 400 && fuzz_state_intact(), msg);
    }
    // 8 KB and an over-long password under the cap.
    static char big[9000];
    memset(big, 'A', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    fuzz_fresh();
    totp_reset_token_store(reset_tokens(), tok, "admin", now_ms());
    st = fuzz_post(reset_post_handler, big, (long long)strlen(big), (size_t)-1, 0);
    TEST_CHECK(st == 400 && fuzz_state_intact(), "reset: 8 KB body refused 400, nothing changed");
    char mid[500];
    int n = snprintf(mid, sizeof(mid), "username=admin&reset_token=%s&new_password=", tok);
    memset(mid + n, 'B', 300);
    mid[n + 300] = '\0';
    fuzz_fresh();
    totp_reset_token_store(reset_tokens(), tok, "admin", now_ms());
    st = fuzz_post(reset_post_handler, mid, (long long)strlen(mid), (size_t)-1, 0);
    TEST_CHECK(fuzz_state_intact(), "reset: 300-byte password does not reach the setter");

    // No token stored at all: a perfectly shaped body is still refused.
    fuzz_fresh();
    st = fuzz_post(reset_post_handler, good, (long long)gl, (size_t)-1, 0);
    TEST_CHECK(st == 400 && fuzz_state_intact(), "reset: well-formed body with no minted token is refused");

    // TOTP disenrolled between forgot and reset: token must not work.
    fuzz_fresh();
    totp_reset_token_store(reset_tokens(), tok, "admin", now_ms());
    (void)totp_config_clear();
    st = fuzz_post(reset_post_handler, good, (long long)gl, (size_t)-1, 0);
    TEST_CHECK(st == 400 && s_setpw_calls == 0, "reset: token for a since-disenrolled TOTP does not set the password");

    // Weak new password: refused WITHOUT burning the token; the same token then
    // works with a strong password.
    {
        char weak[256];
        snprintf(weak, sizeof(weak), "username=admin&reset_token=%s&new_password=short1", tok);
        fuzz_fresh();
        totp_reset_token_store(reset_tokens(), tok, "admin", now_ms());
        st = fuzz_post(reset_post_handler, weak, (long long)strlen(weak), (size_t)-1, 0);
        TEST_CHECK(st >= 400 && s_setpw_calls == 0 && fuzz_state_intact(), "reset: weak password is refused, nothing changed");
        fake_time_advance_ms(6000); /* clear the 5 s failure backoff */
        st = fuzz_post(reset_post_handler, good, (long long)gl, (size_t)-1, 0);
        TEST_CHECK(s_setpw_calls == 1 && strstr(s_sendstr, "\"ok\":true") != NULL,
                   "reset: weak password did not burn the token; strong retry succeeds");
    }

    // Control: the shaped body with a stored token reaches the setter once.
    fuzz_fresh();
    totp_reset_token_store(reset_tokens(), tok, "admin", now_ms());
    st = fuzz_post(reset_post_handler, good, (long long)gl, (size_t)-1, 0);
    TEST_CHECK(s_setpw_calls == 1 && strstr(s_sendstr, "\"ok\":true") != NULL, "control: valid reset reaches the setter once");
    TEST_CHECK(fuzz_session_count() == 0, "control: valid reset drops sessions");
}

int main(void)
{
    g_test_stub_semaphore_take_default = 1; /* pdTRUE */
    test_fuzz_forgot();
    test_fuzz_reset();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
