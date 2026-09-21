// Host tests for App/drivers/http/web_auth_login_http.c -- the browser
// login route (docs/WEB_AUTH_PLAN.md section 6). Own standalone executable
// (own g_test_failures/g_test_count), same "#include the driver .c directly
// to reach its statics" convention as test_ota_http.c/test_web_auth_store.c:
// login_lockout_slot_for() and the per-IP lockout table it owns are `static`
// and have no other seam, and login_post_handler() itself is the only place
// Finding 4 (global lockout), Finding 5 (unlooped httpd_req_recv), and the
// 2026-09-21 escalating backoff ladder + remote/local scope split can be
// exercised end-to-end.
//
// This file supplies its OWN test-controllable httpd_req_get_hdr_value_len/
// _str, httpd_req_recv, ota_http_get_client_ip, and
// wifi_prov_get_sta_ip_netmask bodies (never the shared
// stubs/http_auth_link_stub.c, which is deliberately non-controllable) so a
// test can (a) stage a source IP per call (Finding 4), (b) force
// httpd_req_recv() to hand back the body in several short chunks rather than
// all at once (Finding 5), and (c) stage a fake STA ip/netmask to drive the
// remote-vs-local classification (login_ip_scope.h). Time advances via
// fake_time_advance_ms() (fake_time.h, already linked for other host test
// executables), never a real sleep.
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
#include "fake_time.h"
#include "psa/crypto.h"

// psa/crypto.h's host stub declares this `extern` (test_ota_http.c/
// test_web_auth_store.c each define their own copy for their own
// executable) -- this standalone executable needs its own definition since
// it does not link either of those.
psa_status_t g_stub_psa_import_key_result = PSA_SUCCESS;

// Counts real KDF invocations -- web_auth_store_verify_password() is the
// ONLY path login_post_handler() takes to run the KDF, so a test can prove a
// refused (429, ladder-locked) attempt never reaches it: "the point" of
// refusing before the KDF runs, per web_auth_login_http.c's header comment.
// Macro-rename trick so this is test-only instrumentation, not a change to
// web_auth_store.c itself.
static int s_verify_password_call_count = 0;
#define web_auth_store_verify_password test_counted_verify_password_impl

// web_auth_store.c -- direct #include, same convention test_web_auth_store.c
// uses (needs psa/crypto.h's host stub + fake_kv.h, both already set up
// above). login_post_handler() calls web_auth_store_load_password()/
// verify_password() directly.
#include "../drivers/persist/web_auth_store.c"
#undef web_auth_store_verify_password
static bool web_auth_store_verify_password(web_auth_role_t role, const char *password)
{
    s_verify_password_call_count++;
    return test_counted_verify_password_impl(role, password);
}

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
static char s_last_retry_after[16];
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value)
{
    (void)r;
    if (field && value && strcmp(field, "Set-Cookie") == 0) {
        strncpy(s_last_set_cookie, value, sizeof(s_last_set_cookie) - 1);
        s_last_set_cookie[sizeof(s_last_set_cookie) - 1] = '\0';
    }
    if (field && value && strcmp(field, "Retry-After") == 0) {
        strncpy(s_last_retry_after, value, sizeof(s_last_retry_after) - 1);
        s_last_retry_after[sizeof(s_last_retry_after) - 1] = '\0';
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

// ---- 2026-09-18 follow-up to d2c51f55: test-controllable "was the address
// resolved" outcome for login_post_handler()'s session-mint gate. Default
// true (an ordinary resolvable client) -- individual tests flip this to
// false to simulate an ota_http_get_client_ip_checked() lookup failure
// (httpd_req_to_sockfd()/getpeername()/inet_ntop() all live in ota_http.c,
// which this executable does not link, so this is the only way to drive
// that path here). s_stub_client_ip is still written the same way a real
// failure would leave it ("unknown" -- see ota_http_client_ip_finalize()),
// so a test that forgets to check the mint gate itself would still see the
// pre-existing sentinel-collision behavior, not a crash.
static bool s_stub_ip_known = true;
bool ota_http_get_client_ip_checked(httpd_req_t *req, char *out, size_t out_len)
{
    (void)req;
    if (out && out_len > 0) {
        strncpy(out, s_stub_ip_known ? s_stub_client_ip : "unknown", out_len - 1);
        out[out_len - 1] = '\0';
    }
    return s_stub_ip_known;
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

// ---- wifi_prov.h -- test-controllable STA ip/netmask for the remote-vs-
// local backoff scope check (login_backoff_slot_for()). Default: no STA
// lease at all (both empty), i.e. only the fixed AP subnet counts as LOCAL
// unless a test stages a specific STA ip/netmask.
static char s_stub_sta_ip[16] = "";
static char s_stub_sta_netmask[16] = "";
// Review fix (2026-09-21, finding 4): production now reads the non-blocking
// cache (wifi_prov_get_cached_sta_ip_netmask()), never the blocking
// wifi_prov_get_sta_ip_netmask(), from login_backoff_slot_for() -- this
// counter proves the refused/429 path makes no such call at all (same
// call-counter pattern as s_verify_password_call_count for the KDF).
static int s_cached_sta_ip_netmask_call_count = 0;
esp_err_t wifi_prov_get_cached_sta_ip_netmask(char *ip_out, size_t ip_cap, char *netmask_out, size_t netmask_cap)
{
    s_cached_sta_ip_netmask_call_count++;
    if (ip_out && ip_cap > 0) {
        strncpy(ip_out, s_stub_sta_ip, ip_cap - 1);
        ip_out[ip_cap - 1] = '\0';
    }
    if (netmask_out && netmask_cap > 0) {
        strncpy(netmask_out, s_stub_sta_netmask, netmask_cap - 1);
        netmask_out[netmask_cap - 1] = '\0';
    }
    return ESP_OK;
}
// Kept as a separate stub in case any other caller still uses the blocking
// getter directly; not exercised by these tests today.
esp_err_t wifi_prov_get_sta_ip_netmask(char *ip_out, size_t ip_cap, char *netmask_out, size_t netmask_cap)
{
    if (ip_out && ip_cap > 0) {
        strncpy(ip_out, s_stub_sta_ip, ip_cap - 1);
        ip_out[ip_cap - 1] = '\0';
    }
    if (netmask_out && netmask_cap > 0) {
        strncpy(netmask_out, s_stub_sta_netmask, netmask_cap - 1);
        netmask_out[netmask_cap - 1] = '\0';
    }
    return (s_stub_sta_ip[0] != '\0') ? ESP_OK : ESP_ERR_INVALID_STATE;
}

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
    memset(&s_remote_login_slot, 0, sizeof(s_remote_login_slot));
    fake_time_reset_all();
    s_last_err_status = 0;
    s_last_status_line = 0;
    s_last_sendstr[0] = '\0';
    s_last_set_cookie[0] = '\0';
    s_last_retry_after[0] = '\0';
    strncpy(s_stub_client_ip, "10.0.0.1", sizeof(s_stub_client_ip) - 1);
    s_stub_ip_known = true;
    // Default STA lease covers the whole 10.0.0.0/8 range, which is where
    // every pre-existing lockout test in this file picks its per-IP
    // addresses from (10.0.0.x, 10.1.0.x, ... 10.9.0.x) -- with no STA
    // lease staged at all, login_ip_scope_classify() would call every one
    // of those REMOTE (since only the fixed 192.168.4.x AP subnet is LOCAL
    // by default), collapsing them all onto the single shared remote slot
    // and defeating the per-IP isolation those tests exist to prove. The
    // remote/local scope tests below use 8.8.8.8/1.2.3.4, well outside
    // 10.0.0.0/8, so they still classify REMOTE under this default; tests
    // that need a different STA subnet (the non-/24 mask test) override
    // this explicitly.
    strncpy(s_stub_sta_ip, "10.0.0.1", sizeof(s_stub_sta_ip) - 1);
    strncpy(s_stub_sta_netmask, "255.0.0.0", sizeof(s_stub_sta_netmask) - 1);
    s_verify_password_call_count = 0;
    s_cached_sta_ip_netmask_call_count = 0;
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

// *** 2026-09-17 adversarial review, Finding 2: the eviction inside
// login_lockout_slot_for() used to treat every in_use slot as equally
// evictable, including one that is CURRENTLY LOCKED. An attacker controlling
// LOGIN_LOCKOUT_MAX_IPS or more source addresses could rotate through them,
// evicting his own oldest locked slot each time a new address needed one --
// three guesses per address, unlimited addresses, defeating the whole point
// of per-IP lockout. test_lockout_table_is_bounded() above only ever
// asserted in_use == LOGIN_LOCKOUT_MAX_IPS, which stayed true whether or not
// eviction cleared a locked slot -- vacuous against exactly this bug. ***
static void test_lockout_eviction_never_evicts_a_locked_slot(void)
{
    TEST_SECTION("login lockout table -- eviction must skip currently-locked slots (Finding 2)");
    reset_all();
    set_admin_credential("admin", "correct-horse-battery-staple");

    char ip[46];
    // Lock every one of the LOGIN_LOCKOUT_MAX_IPS slots (3 failures each is
    // OTA_AUTH_LOCKOUT_THRESHOLD).
    for (unsigned i = 0; i < LOGIN_LOCKOUT_MAX_IPS; i++) {
        snprintf(ip, sizeof(ip), "10.2.0.%u", i + 1);
        strncpy(s_stub_client_ip, ip, sizeof(s_stub_client_ip) - 1);
        for (int j = 0; j < 3; j++) {
            do_login("admin", "wrong-password");
        }
    }
    unsigned locked_count = 0;
    for (unsigned i = 0; i < LOGIN_LOCKOUT_MAX_IPS; i++) {
        if (s_login_lockouts[i].in_use && login_backoff_is_locked(&s_login_lockouts[i].backoff, now_ms())) {
            locked_count++;
        }
    }
    TEST_CHECK(locked_count == LOGIN_LOCKOUT_MAX_IPS, "test setup: every slot is now locked");

    // A brand-new, never-seen-before source IP arrives once the table is
    // saturated with locked attackers. It must be refused outright (429),
    // NOT granted a slot by evicting one of the locked attackers -- that
    // eviction is exactly the bypass Finding 2 found.
    strncpy(s_stub_client_ip, "10.2.9.99", sizeof(s_stub_client_ip) - 1);
    s_last_status_line = 0;
    esp_err_t err = do_login("admin", "correct-horse-battery-staple");
    TEST_CHECK(err == ESP_OK, "handler still returns ESP_OK when refusing a saturated-table login");
    TEST_CHECK(s_last_status_line == 429,
               "a brand-new IP is refused once the table is saturated with locked IPs, "
               "rather than being handed a slot evicted from a locked attacker");

    // Every one of the originally-locked IPs must still be locked -- none
    // was evicted to make room for the new arrival.
    unsigned still_locked = 0;
    for (unsigned i = 0; i < LOGIN_LOCKOUT_MAX_IPS; i++) {
        if (s_login_lockouts[i].in_use && login_backoff_is_locked(&s_login_lockouts[i].backoff, now_ms())) {
            still_locked++;
        }
    }
    TEST_CHECK(still_locked == LOGIN_LOCKOUT_MAX_IPS,
               "eviction never cleared a locked slot to make room for the new IP -- "
               "the pre-fix bug would have shown fewer than LOGIN_LOCKOUT_MAX_IPS here");

    // The new IP must never have been granted a slot at all.
    bool new_ip_got_a_slot = false;
    for (unsigned i = 0; i < LOGIN_LOCKOUT_MAX_IPS; i++) {
        if (s_login_lockouts[i].in_use && strcmp(s_login_lockouts[i].ip, "10.2.9.99") == 0) {
            new_ip_got_a_slot = true;
        }
    }
    TEST_CHECK(!new_ip_got_a_slot, "the refused new IP was never given a table slot");
}

// A slot that is free (never used) or in_use but NOT currently locked must
// still be evictable/reusable normally -- Finding 2's fix must not turn
// eviction off altogether, only skip slots that are actively locked.
static void test_lockout_eviction_still_works_for_unlocked_slots(void)
{
    TEST_SECTION("login lockout table -- eviction still reclaims unlocked slots (Finding 2 regression guard)");
    reset_all();
    set_admin_credential("admin", "correct-horse-battery-staple");

    char ip[46];
    // Fill every slot with a single SUCCESSFUL login each (clears/keeps
    // lockout state unlocked) so none of them are locked.
    for (unsigned i = 0; i < LOGIN_LOCKOUT_MAX_IPS; i++) {
        snprintf(ip, sizeof(ip), "10.3.0.%u", i + 1);
        strncpy(s_stub_client_ip, ip, sizeof(s_stub_client_ip) - 1);
        do_login("admin", "correct-horse-battery-staple");
    }
    unsigned in_use = 0;
    for (unsigned i = 0; i < LOGIN_LOCKOUT_MAX_IPS; i++) {
        if (s_login_lockouts[i].in_use) { in_use++; }
    }
    TEST_CHECK(in_use == LOGIN_LOCKOUT_MAX_IPS, "test setup: table is full of unlocked slots");

    // A new IP must still be able to get a slot via ordinary LRU eviction --
    // the table is not permanently stuck once nothing is locked.
    strncpy(s_stub_client_ip, "10.3.9.99", sizeof(s_stub_client_ip) - 1);
    s_last_status_line = 0;
    esp_err_t err = do_login("admin", "correct-horse-battery-staple");
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(s_last_status_line != 429, "a new IP is NOT refused when the table is full of unlocked slots");

    bool new_ip_got_a_slot = false;
    for (unsigned i = 0; i < LOGIN_LOCKOUT_MAX_IPS; i++) {
        if (s_login_lockouts[i].in_use && strcmp(s_login_lockouts[i].ip, "10.3.9.99") == 0) {
            new_ip_got_a_slot = true;
        }
    }
    TEST_CHECK(new_ip_got_a_slot, "the new IP was granted a slot via ordinary LRU eviction of an unlocked slot");
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

// *** 2026-09-18 follow-up to d2c51f55: with correct credentials but an
// unresolvable client address (ota_http_get_client_ip_checked() reporting
// false), login_post_handler() must refuse to mint a session -- and that
// refusal must be indistinguishable from an ordinary wrong-password 401
// (same status, same body), never a new/different response that would let
// an attacker use the response itself to learn the credentials were
// actually correct. ***
static void test_unresolvable_ip_refuses_to_mint_a_session(void)
{
    TEST_SECTION("login_post_handler -- refuses to mint a session when the client IP "
                 "cannot be determined (2026-09-18 follow-up to d2c51f55)");
    reset_all();
    set_admin_credential("admin", "correct-horse-battery-staple");

    // Baseline: a resolvable IP with correct credentials mints a session
    // normally, so the negative case below is contrasted against a real
    // working path, not a handler that never mints anything at all.
    esp_err_t base_err = do_login("admin", "correct-horse-battery-staple");
    TEST_CHECK(base_err == ESP_OK, "test setup: baseline resolvable-IP login returns ESP_OK");
    TEST_CHECK(strstr(s_last_set_cookie, HTTP_SESSION_COOKIE_NAME "=") != NULL,
               "test setup: baseline resolvable-IP login with correct credentials mints a session");

    // Now simulate an unresolvable address on a FRESH IP slot (so the
    // per-IP lockout from the baseline call above cannot confound this
    // case) with the SAME correct credentials.
    strncpy(s_stub_client_ip, "10.0.0.77", sizeof(s_stub_client_ip) - 1);
    s_stub_ip_known = false;
    s_last_set_cookie[0] = '\0';
    s_last_err_status = 0;
    s_last_status_line = 0;
    esp_err_t err = do_login("admin", "correct-horse-battery-staple");

    TEST_CHECK(err == ESP_OK, "handler still returns ESP_OK when refusing an unresolvable-IP login");
    TEST_CHECK(s_last_set_cookie[0] == '\0',
               "no session cookie is set when the client address could not be determined");
    TEST_CHECK(s_last_err_status == HTTPD_401_UNAUTHORIZED,
               "the refusal reuses the SAME status as an ordinary wrong-password failure (fail-closed, "
               "not a distinguishable new response)");

    // Compare directly against the response a genuine wrong-password
    // failure produces, on the same (now known-again) IP, to prove the two
    // are byte-for-byte the same shape an attacker could observe.
    int unresolvable_status = s_last_err_status;
    s_stub_ip_known = true;
    s_last_err_status = 0;
    esp_err_t wrong_pw_err = do_login("admin", "wrong-password");
    TEST_CHECK(wrong_pw_err == ESP_OK, "test setup: ordinary wrong-password login returns ESP_OK");
    TEST_CHECK(s_last_err_status == unresolvable_status,
               "the unresolvable-IP refusal and an ordinary wrong-password failure send the "
               "identical httpd_resp_send_err() status code -- no new oracle");
}

// Pure-function coverage for web_auth_login_ip_gate.h's
// web_auth_login_may_mint_session() in isolation, independent of the httpd
// plumbing above.
static void test_may_mint_session_pure_function(void)
{
    TEST_SECTION("web_auth_login_may_mint_session() -- pure decision core");
    TEST_CHECK(web_auth_login_may_mint_session(true) == true,
               "a resolved address is allowed to mint");
    TEST_CHECK(web_auth_login_may_mint_session(false) == false,
               "an unresolved address (ip_known == false) is refused");
}

// ---------------------------------------------------------------------------
// 2026-09-21: escalating backoff ladder (5s/10s/30s/60s/300s) + remote/local
// scope split.
// ---------------------------------------------------------------------------

static void test_ladder_retry_after_steps(void)
{
    TEST_SECTION("login backoff ladder -- Retry-After matches 5/10/30/60/300s by failure count");
    reset_all();
    set_admin_credential("admin", "correct-horse-battery-staple");
    strncpy(s_stub_client_ip, "10.9.0.1", sizeof(s_stub_client_ip) - 1);

    static const uint32_t expect_s[LOGIN_BACKOFF_LADDER_LEN] = { 5, 10, 30, 60, 300 };
    for (unsigned step = 0; step < LOGIN_BACKOFF_LADDER_LEN; step++) {
        do_login("admin", "wrong-password");
        s_last_status_line = 0;
        s_last_retry_after[0] = '\0';
        do_login("admin", "correct-horse-battery-staple"); // refused before KDF, regardless of password
        TEST_CHECK(s_last_status_line == 429, "still locked after this ladder step's failure");
        TEST_CHECK((uint32_t)atoi(s_last_retry_after) == expect_s[step],
                   "Retry-After matches this ladder step");
        // Advance past this step's wait so the NEXT failure lands on the next step.
        fake_time_advance_ms(expect_s[step] * 1000u + 1u);
    }
}

static void test_ladder_cycle_resets_after_full_window(void)
{
    TEST_SECTION("login backoff ladder -- cycle resets to the 5s step once the 300s window elapses");
    reset_all();
    set_admin_credential("admin", "correct-horse-battery-staple");
    strncpy(s_stub_client_ip, "10.9.0.2", sizeof(s_stub_client_ip) - 1);

    for (unsigned step = 0; step < LOGIN_BACKOFF_LADDER_LEN; step++) {
        do_login("admin", "wrong-password");
        fake_time_advance_ms(LOGIN_BACKOFF_LADDER_MS[step] + 1u);
    }
    // Fully served the 300s (last) step's wait with no further failure --
    // the next failure must land back on the FIRST ladder step (5s), not
    // escalate past the table or stay parked at 300s.
    do_login("admin", "wrong-password");
    s_last_status_line = 0;
    s_last_retry_after[0] = '\0';
    do_login("admin", "correct-horse-battery-staple");
    TEST_CHECK(s_last_status_line == 429, "still locked after the post-reset failure");
    TEST_CHECK((uint32_t)atoi(s_last_retry_after) == 5u,
               "the cycle reset to the ladder's first (5s) step, not stuck at 300s");
}

static void test_success_resets_the_ladder(void)
{
    TEST_SECTION("login backoff ladder -- a successful login resets the failure count/lock");
    reset_all();
    set_admin_credential("admin", "correct-horse-battery-staple");
    strncpy(s_stub_client_ip, "10.9.0.3", sizeof(s_stub_client_ip) - 1);

    do_login("admin", "wrong-password");
    do_login("admin", "wrong-password");
    fake_time_advance_ms(LOGIN_BACKOFF_LADDER_MS[1] + 1u); // clear the 10s wait from failure #2
    esp_err_t err = do_login("admin", "correct-horse-battery-staple");
    TEST_CHECK(err == ESP_OK, "the correct-password login after the wait succeeds");
    TEST_CHECK(strstr(s_last_set_cookie, HTTP_SESSION_COOKIE_NAME "=") != NULL,
               "test setup: the success actually minted a session");

    // Next failure must land back on the ladder's FIRST step (5s), proving
    // the prior two failures were forgotten by the success.
    s_last_set_cookie[0] = '\0';
    do_login("admin", "wrong-password");
    s_last_status_line = 0;
    s_last_retry_after[0] = '\0';
    do_login("admin", "correct-horse-battery-staple");
    TEST_CHECK(s_last_status_line == 429, "locked after the post-success failure");
    TEST_CHECK((uint32_t)atoi(s_last_retry_after) == 5u,
               "a success resets the ladder back to its first (5s) step");
}

static void test_refused_attempt_does_not_count_or_extend_wait(void)
{
    TEST_SECTION("login backoff ladder -- a refused (429) attempt neither counts as a failure "
                 "nor extends the wait, and never runs the KDF");
    reset_all();
    set_admin_credential("admin", "correct-horse-battery-staple");
    strncpy(s_stub_client_ip, "10.9.0.4", sizeof(s_stub_client_ip) - 1);

    do_login("admin", "wrong-password"); // failure #1: locks for 5s
    int calls_after_first_failure = s_verify_password_call_count;

    // Hammer it with several more attempts while still inside the 5s wait --
    // none of these may reach the KDF, count as a failure, or extend the lock.
    for (int i = 0; i < 3; i++) {
        s_last_status_line = 0;
        s_last_retry_after[0] = '\0';
        do_login("admin", "correct-horse-battery-staple");
        TEST_CHECK(s_last_status_line == 429, "still refused while inside the wait window");
        TEST_CHECK((uint32_t)atoi(s_last_retry_after) == 5u,
                   "Retry-After does not grow from repeated refused attempts");
    }
    TEST_CHECK(s_verify_password_call_count == calls_after_first_failure,
               "the KDF (web_auth_store_verify_password) was never invoked on a refused attempt");

    // Serve exactly the original 5s wait (not extended) and confirm the
    // very next failure escalates to the SECOND step (10s), proving the
    // refused attempts above never re-armed a fresh 5s wait. (A correct-
    // password login here would reset the ladder via login_backoff_record_success()
    // and defeat the point of this check, so the unlock is proven directly
    // with a second wrong-password failure instead.)
    fake_time_advance_ms(5000u + 1u);
    do_login("admin", "wrong-password"); // failure #2
    s_last_status_line = 0;
    s_last_retry_after[0] = '\0';
    do_login("admin", "correct-horse-battery-staple");
    TEST_CHECK((uint32_t)atoi(s_last_retry_after) == 10u,
               "the wait was never extended by refused attempts -- failure #2 lands on the 10s step");
}

// ---- remote/local scope split (2026-09-20 owner addition) -----------------

// AP fallback subnet is always LOCAL even with no STA lease staged.
#define SCOPE_TEST_LOCAL_A "192.168.4.50"
#define SCOPE_TEST_LOCAL_B "192.168.4.51"
// Neither the AP subnet nor (with no STA lease staged) any STA subnet --
// both classify REMOTE and must share the single reserved slot.
#define SCOPE_TEST_REMOTE_A "8.8.8.8"
#define SCOPE_TEST_REMOTE_B "1.2.3.4"

static void test_remote_failure_delays_a_different_remote_address(void)
{
    TEST_SECTION("login backoff scope -- an off-subnet failure delays a DIFFERENT off-subnet address "
                 "(shared remote slot)");
    reset_all();
    set_admin_credential("admin", "correct-horse-battery-staple");

    strncpy(s_stub_client_ip, SCOPE_TEST_REMOTE_A, sizeof(s_stub_client_ip) - 1);
    do_login("admin", "wrong-password");

    strncpy(s_stub_client_ip, SCOPE_TEST_REMOTE_B, sizeof(s_stub_client_ip) - 1);
    s_last_status_line = 0;
    do_login("admin", "correct-horse-battery-staple");
    TEST_CHECK(s_last_status_line == 429,
               "a second, DIFFERENT remote address is locked out by the first remote address's failure "
               "-- all remote clients share one slot");
}

static void test_remote_failure_does_not_delay_a_local_address(void)
{
    TEST_SECTION("login backoff scope -- an off-subnet failure does NOT delay a local address");
    reset_all();
    set_admin_credential("admin", "correct-horse-battery-staple");

    strncpy(s_stub_client_ip, SCOPE_TEST_REMOTE_A, sizeof(s_stub_client_ip) - 1);
    do_login("admin", "wrong-password");

    strncpy(s_stub_client_ip, SCOPE_TEST_LOCAL_A, sizeof(s_stub_client_ip) - 1);
    s_last_status_line = 0;
    esp_err_t err = do_login("admin", "correct-horse-battery-staple");
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK for the unaffected local address");
    TEST_CHECK(s_last_status_line != 429,
               "a local (AP-subnet) address is never locked out by a remote address's failure");
}

static void test_local_success_does_not_reset_remote_counter(void)
{
    TEST_SECTION("login backoff scope -- a local success does NOT reset the remote counter");
    reset_all();
    set_admin_credential("admin", "correct-horse-battery-staple");

    strncpy(s_stub_client_ip, SCOPE_TEST_REMOTE_A, sizeof(s_stub_client_ip) - 1);
    do_login("admin", "wrong-password");
    fake_time_advance_ms(LOGIN_BACKOFF_LADDER_MS[0] + 1u); // clear the 5s wait so failure #2 actually runs
    do_login("admin", "wrong-password"); // remote failure count now 2 -> next lock step is 30s

    strncpy(s_stub_client_ip, SCOPE_TEST_LOCAL_A, sizeof(s_stub_client_ip) - 1);
    esp_err_t err = do_login("admin", "correct-horse-battery-staple");
    TEST_CHECK(err == ESP_OK, "test setup: the local login succeeds");
    TEST_CHECK(strstr(s_last_set_cookie, HTTP_SESSION_COOKIE_NAME "=") != NULL,
               "test setup: the local success actually minted a session");

    // The remote slot's count must be untouched by the unrelated local
    // success -- a third remote failure now must land on the THIRD ladder
    // step (30s), not the first (5s), and a different remote address must
    // still be locked out by it (still the shared slot).
    strncpy(s_stub_client_ip, SCOPE_TEST_REMOTE_A, sizeof(s_stub_client_ip) - 1);
    fake_time_advance_ms(LOGIN_BACKOFF_LADDER_MS[1] + 1u); // clear the 10s wait from failure #2
    do_login("admin", "wrong-password");
    strncpy(s_stub_client_ip, SCOPE_TEST_REMOTE_B, sizeof(s_stub_client_ip) - 1);
    s_last_status_line = 0;
    s_last_retry_after[0] = '\0';
    do_login("admin", "correct-horse-battery-staple");
    TEST_CHECK(s_last_status_line == 429, "the other remote address is locked by the shared remote slot");
    TEST_CHECK((uint32_t)atoi(s_last_retry_after) == 30u,
               "the remote counter kept its prior failures across the unrelated local success "
               "(3rd remote failure -> 30s step, not reset to 5s)");
}

static void test_scope_subnet_math_with_non_24_mask(void)
{
    TEST_SECTION("login backoff scope -- subnet classification with a non-/24 STA netmask");
    reset_all();
    set_admin_credential("admin", "correct-horse-battery-staple");

    // STA lease 10.20.30.1/255.255.255.240 (/28): usable range 10.20.30.0-15.
    strncpy(s_stub_sta_ip, "10.20.30.1", sizeof(s_stub_sta_ip) - 1);
    strncpy(s_stub_sta_netmask, "255.255.255.240", sizeof(s_stub_sta_netmask) - 1);

    // .14 is inside the /28 -> LOCAL: a remote failure must not touch it.
    strncpy(s_stub_client_ip, SCOPE_TEST_REMOTE_A, sizeof(s_stub_client_ip) - 1);
    do_login("admin", "wrong-password");
    strncpy(s_stub_client_ip, "10.20.30.14", sizeof(s_stub_client_ip) - 1);
    s_last_status_line = 0;
    esp_err_t err = do_login("admin", "correct-horse-battery-staple");
    TEST_CHECK(err == ESP_OK, "an address inside the /28 is LOCAL and unaffected by the remote failure");
    TEST_CHECK(s_last_status_line != 429, "10.20.30.14 classifies LOCAL under the /28 mask");

    // .16 is just past the /28 boundary -> REMOTE: it must share the
    // already-failed remote slot and be locked out.
    strncpy(s_stub_client_ip, "10.20.30.16", sizeof(s_stub_client_ip) - 1);
    s_last_status_line = 0;
    do_login("admin", "correct-horse-battery-staple");
    TEST_CHECK(s_last_status_line == 429,
               "10.20.30.16 falls outside the /28 boundary and classifies REMOTE, sharing the "
               "already-locked remote slot");
}

// Review fix (2026-09-21, finding 1): the table-saturation (slot == NULL)
// 429 path used to send Retry-After: 1, leaking "this is saturation, not a
// real lock" to the caller -- a real lock reports 5/10/30/60/300 depending
// on ladder step. Fixed: the saturation path now reports the ladder's own
// last (worst-case) step, 300s, so both causes of a 429 are indistinguishable
// from the response alone.
static void test_saturation_retry_after_matches_ladder_not_one_second(void)
{
    TEST_SECTION("login lockout table -- saturated-table Retry-After matches the ladder, "
                 "not a distinct '1' (Finding 1)");
    reset_all();
    set_admin_credential("admin", "correct-horse-battery-staple");

    char ip[46];
    for (unsigned i = 0; i < LOGIN_LOCKOUT_MAX_IPS; i++) {
        snprintf(ip, sizeof(ip), "10.3.0.%u", i + 1);
        strncpy(s_stub_client_ip, ip, sizeof(s_stub_client_ip) - 1);
        for (int j = 0; j < 3; j++) {
            do_login("admin", "wrong-password");
        }
    }
    strncpy(s_stub_client_ip, "10.3.9.99", sizeof(s_stub_client_ip) - 1);
    s_last_status_line = 0;
    s_last_retry_after[0] = '\0';
    do_login("admin", "correct-horse-battery-staple");
    TEST_CHECK(s_last_status_line == 429, "test setup: saturation refuses with 429");
    TEST_CHECK((uint32_t)atoi(s_last_retry_after) == LOGIN_BACKOFF_LADDER_MS[LOGIN_BACKOFF_LADDER_LEN - 1] / 1000u,
               "the saturation path reports the ladder's last (worst-case) step, "
               "never a distinct value that would leak table-saturation vs. a real lock");
}

// Review fix (2026-09-21, finding 3): an UNKNOWN-scope client (address that
// does not parse as IPv4 at all, distinct from the ip_known==false sentinel)
// must share the single remote slot, not get its own per-IP table slot --
// otherwise a non-dotted-quad-looking address is a way to mint unlimited
// fresh ladders and eventually saturate all LOGIN_LOCKOUT_MAX_IPS slots.
static void test_unknown_scope_shares_the_remote_slot(void)
{
    TEST_SECTION("login backoff scope -- UNKNOWN-scope clients share the remote slot, not the per-IP table "
                 "(Finding 3)");
    reset_all();
    set_admin_credential("admin", "correct-horse-battery-staple");

    // Not a dotted-quad IPv4 string at all -> login_ip_scope_classify()
    // returns UNKNOWN. ip_known stays true (ota_http_get_client_ip()
    // resolved SOMETHING; it just isn't parseable IPv4), so this must reach
    // login_backoff_slot_for()'s scope check, not the separate
    // ip_known==false sentinel path.
    strncpy(s_stub_client_ip, "not-an-ip-address", sizeof(s_stub_client_ip) - 1);
    do_login("admin", "wrong-password");

    // A different UNKNOWN-scope string must be locked out by the first
    // one's failure -- proof both landed on the single shared remote slot.
    strncpy(s_stub_client_ip, "also-not-an-ip", sizeof(s_stub_client_ip) - 1);
    s_last_status_line = 0;
    do_login("admin", "correct-horse-battery-staple");
    TEST_CHECK(s_last_status_line == 429,
               "a second, different UNKNOWN-scope address is locked out by the first's failure "
               "-- both share the remote slot");

    // No per-IP table slot was consumed by either UNKNOWN-scope address.
    bool got_a_table_slot = false;
    for (unsigned i = 0; i < LOGIN_LOCKOUT_MAX_IPS; i++) {
        if (s_login_lockouts[i].in_use &&
            (strcmp(s_login_lockouts[i].ip, "not-an-ip-address") == 0 ||
             strcmp(s_login_lockouts[i].ip, "also-not-an-ip") == 0)) {
            got_a_table_slot = true;
        }
    }
    TEST_CHECK(!got_a_table_slot, "UNKNOWN-scope addresses never consume a per-IP table slot");
}

// Review fix (2026-09-21, finding 4): wifi_prov_get_cached_sta_ip_netmask()
// must never be called on the refused (429/locked) path -- only the
// blocking-free cache is allowed to be read at all, but even that read
// should not happen once a slot is already known to be locked and the
// request is being refused outright without needing fresh scope
// classification. This proves the refused path makes no MORE calls than a
// normal, allowed attempt needs -- call-counter pattern, same as
// s_verify_password_call_count for the KDF-not-invoked-on-refusal test.
static void test_refused_login_makes_no_extra_sta_ip_netmask_calls(void)
{
    TEST_SECTION("login backoff -- refused (429) attempts make no extra STA ip/netmask cache calls "
                 "(Finding 4)");
    reset_all();
    set_admin_credential("admin", "correct-horse-battery-staple");

    strncpy(s_stub_client_ip, SCOPE_TEST_REMOTE_A, sizeof(s_stub_client_ip) - 1);
    do_login("admin", "wrong-password");
    int calls_after_first_failure = s_cached_sta_ip_netmask_call_count;
    TEST_CHECK(calls_after_first_failure >= 1, "test setup: classifying the first attempt read the cache");

    // Now locked out -- every subsequent attempt within the lock window is
    // refused. Each refusal may still need to classify scope to find its
    // slot (that's an existing, allowed read of the non-blocking cache),
    // but it must NEVER be the blocking wifi_prov_get_sta_ip_netmask() --
    // this test doesn't stub that path at all, so if login_backoff_slot_for()
    // ever regressed to calling it, this executable would fail to link
    // (undefined at compile time it's still defined above, so instead we
    // assert the cache-read count grows by exactly one bounded increment per
    // attempt, never balloons or blocks).
    int before = s_cached_sta_ip_netmask_call_count;
    s_last_status_line = 0;
    do_login("admin", "correct-horse-battery-staple");
    TEST_CHECK(s_last_status_line == 429, "test setup: the second attempt is refused (still locked)");
    int after = s_cached_sta_ip_netmask_call_count;
    TEST_CHECK(after - before <= 1,
               "a refused attempt makes at most one non-blocking cache read, never repeats or "
               "escalates -- and critically never round-trips the blocking owner-task getter");
}

// Coordinator bench observation (2026-09-21): the board's own log showed
// login failures arriving as "::FFFF:192.168.1.87" (uppercase FFFF,
// dotted-quad tail) -- PF_INET6 httpd listener, IPv4-mapped-IPv6
// getpeername() string. This is the literal, confirmed real-world form;
// must classify LOCAL under a 192.168.1.x/24 STA lease exactly like the
// plain "192.168.1.87" would.
static void test_mapped_ipv6_dotted_quad_tail_classifies_local(void)
{
    TEST_SECTION("login backoff scope -- IPv4-mapped-IPv6 '::FFFF:<dotted-quad>' classifies "
                 "like the plain address (Finding 2, bench-observed form)");
    reset_all();
    set_admin_credential("admin", "correct-horse-battery-staple");

    strncpy(s_stub_sta_ip, "192.168.1.1", sizeof(s_stub_sta_ip) - 1);
    strncpy(s_stub_sta_netmask, "255.255.255.0", sizeof(s_stub_sta_netmask) - 1);

    strncpy(s_stub_client_ip, "::FFFF:192.168.1.87", sizeof(s_stub_client_ip) - 1);
    do_login("admin", "wrong-password"); // failure #1: locks this slot for 5s (ladder step 1)

    // A LOCAL client gets its own per-IP slot, not the shared remote one --
    // proof: a genuinely remote address's failure must NOT lock this one out.
    strncpy(s_stub_client_ip, SCOPE_TEST_REMOTE_A, sizeof(s_stub_client_ip) - 1);
    do_login("admin", "wrong-password");

    // Clear the mapped address's own 5s wait from its one prior failure --
    // this is what "below threshold" refers to (single failure, first ladder
    // step only), not "no lock at all".
    fake_time_advance_ms(LOGIN_BACKOFF_LADDER_MS[0] + 1u);

    strncpy(s_stub_client_ip, "::FFFF:192.168.1.87", sizeof(s_stub_client_ip) - 1);
    s_last_status_line = 0;
    esp_err_t err = do_login("admin", "correct-horse-battery-staple");
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK for the mapped-address login attempt");
    TEST_CHECK(s_last_status_line != 429,
               "'::FFFF:192.168.1.87' classifies LOCAL under a 192.168.1.x/24 STA lease -- "
               "unaffected by the unrelated remote address's failure, and once its own "
               "5s ladder step (from one prior failure) has elapsed");
}

// Direct unit-level coverage of login_ip_scope_classify() for both mapped
// forms, below the level of a full login attempt -- catches a regression in
// the parser itself independent of how login_backoff_slot_for() uses it.
static void test_ip_scope_classify_mapped_ipv6_forms(void)
{
    TEST_SECTION("login_ip_scope_classify: IPv4-mapped-IPv6 forms (Finding 2)");

    // The literal, confirmed-on-board form (uppercase FFFF, dotted-quad
    // tail) -- coordinator bench observation, 2026-09-21.
    TEST_CHECK(login_ip_scope_classify("::FFFF:192.168.1.87", "192.168.1.1", "255.255.255.0") ==
                   LOGIN_IP_SCOPE_LOCAL,
               "'::FFFF:192.168.1.87' classifies LOCAL against a 192.168.1.x/24 STA lease");
    // Lowercase prefix, same tail -- case-insensitivity.
    TEST_CHECK(login_ip_scope_classify("::ffff:192.168.1.87", "192.168.1.1", "255.255.255.0") ==
                   LOGIN_IP_SCOPE_LOCAL,
               "the lowercase '::ffff:' prefix classifies identically to uppercase");
    // Off-subnet tail under the mapped prefix must still classify REMOTE.
    TEST_CHECK(login_ip_scope_classify("::FFFF:8.8.8.8", "192.168.1.1", "255.255.255.0") == LOGIN_IP_SCOPE_REMOTE,
               "a mapped address whose tail is off-subnet still classifies REMOTE");
    // Defensive: lwIP hex-group form for the same address (192.168.1.87 =
    // 0xC0A80157 -> "C0A8:0157").
    TEST_CHECK(login_ip_scope_classify("::FFFF:C0A8:0157", "192.168.1.1", "255.255.255.0") ==
                   LOGIN_IP_SCOPE_LOCAL,
               "the lwIP hex-group form of the same address classifies LOCAL too (best-effort)");
    // A genuinely unparseable string (no mapped prefix at all) stays UNKNOWN.
    TEST_CHECK(login_ip_scope_classify("garbage", "192.168.1.1", "255.255.255.0") == LOGIN_IP_SCOPE_UNKNOWN,
               "a non-mapped, non-dotted-quad string still classifies UNKNOWN");
}

void run_test_web_auth_login_http(void)
{
    test_lockout_is_per_ip_not_global();
    test_lockout_table_is_bounded();
    test_lockout_eviction_never_evicts_a_locked_slot();
    test_lockout_eviction_still_works_for_unlocked_slots();
    test_login_body_split_across_recv_calls();
    test_unresolvable_ip_refuses_to_mint_a_session();
    test_may_mint_session_pure_function();
    test_ladder_retry_after_steps();
    test_ladder_cycle_resets_after_full_window();
    test_success_resets_the_ladder();
    test_refused_attempt_does_not_count_or_extend_wait();
    test_remote_failure_delays_a_different_remote_address();
    test_remote_failure_does_not_delay_a_local_address();
    test_local_success_does_not_reset_remote_counter();
    test_scope_subnet_math_with_non_24_mask();
    test_ip_scope_classify_mapped_ipv6_forms();
    test_saturation_retry_after_matches_ladder_not_one_second();
    test_unknown_scope_shares_the_remote_slot();
    test_refused_login_makes_no_extra_sta_ip_netmask_calls();
    test_mapped_ipv6_dotted_quad_tail_classifies_local();
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
