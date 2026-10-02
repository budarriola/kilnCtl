// Host tests for App/drivers/http/profiles_live_http.c (docs/LIVE_PROFILE_EDIT_PLAN.md
// pass 2, section 10/11). Its own SEPARATE executable, same "#include the .c
// directly" convention as test_profiles_http.c/test_live_profile.c: this file
// reaches every one of profiles_live_http.c's `static` handlers with no other
// seam. Links the REAL live_profile.c (host hal_kv backend, fake_kv.c) so the
// fork/save/decide storage paths are exercised for real; fakes the httpd-tier
// seams (profiles_http_get/save, profiles_parse_profile_fields,
// profiles_validate_candidate, profile_executor_get_live_status,
// profiles_builtin_*, wifi_provision_http_get_server/kiln_http_register) the
// same way test_live_profile.c fakes profile_encode_current_blob() --
// profiles_http.c/profiles_edit_http.c are NOT linked here, so this file
// supplies deterministic stand-ins instead. Real coverage for those
// functions' own bodies lives in test_profiles_http.c. The httpd request/
// response capture stubs below (s_resp_status/s_resp_body, a capturing
// httpd_req_recv() reading from a test-staged buffer) copy test_ota_http.c's
// pattern.
#include "test_common.h"

int g_test_failures = 0;
int g_test_count = 0;

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "hal_kv.h"
#include "fake_kv.h"

// Pull the REAL type/prototype declarations in ahead of the fakes below
// (profiles_http_internal.h/profiles_builtin.h/profile_executor.h are also
// #included transitively later via profiles_live_http.c's own #includes --
// doing it here first, instead of re-declaring these types/signatures by
// hand, means the fakes below are checked against the one real signature
// rather than risking a silently-mismatched hand copy).
#include "profile_executor.h"
#include "profiles_builtin.h"
#include "../drivers/http/profiles_http_internal.h"

// live_profile.c guards its own internal shim declaration of
// profile_decode_result_t behind this macro (see that file's header
// comment) -- defining the macro here, ahead of its #include below, tells
// it profiles_http_internal.h (pulled in just above) already provides the
// real type, so it does not also declare its own duplicate.
#define PROFILE_DECODE_RESULT_SHIM_DECLARED

// ---- fakes: the shared wire format (profiles_http.c, not linked here) --
// live_profile.c reuses profile_encode_current_blob()/profile_decode_blob();
// same trivial memcpy stand-in test_live_profile.c uses (the real format's
// correctness is test_profiles_http.c's job).
size_t profile_encode_current_blob(const profile_t *profile, void *out, size_t cap)
{
    if (!profile || !out || cap < sizeof(profile_t)) {
        return 0;
    }
    memcpy(out, profile, sizeof(profile_t));
    return sizeof(profile_t);
}

profile_decode_result_t profile_decode_blob(const void *blob, size_t len, profile_t *out, const char **err_reason)
{
    if (!blob || !out || len != sizeof(profile_t)) {
        if (err_reason) {
            *err_reason = "bad length";
        }
        return PROFILE_DECODE_CORRUPT;
    }
    memcpy(out, blob, sizeof(profile_t));
    return PROFILE_DECODE_OK;
}

// ---- fakes: profiles_builtin.h --------------------------------------------
static bool g_fake_builtin_on = false;
static builtin_profile_t g_fake_builtin;

bool profiles_builtin_id_valid(uint8_t id)
{
    return g_fake_builtin_on && id == PROFILE_BUILTIN_ID_BASE;
}
const builtin_profile_t *profiles_builtin_entry(uint8_t id)
{
    return profiles_builtin_id_valid(id) ? &g_fake_builtin : NULL;
}
bool profiles_builtin_get(uint8_t id, profile_t *out)
{
    if (!profiles_builtin_id_valid(id) || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    strncpy(out->name, g_fake_builtin.code, sizeof(out->name) - 1);
    out->segment_count = g_fake_builtin.segment_count;
    memcpy(out->segments, g_fake_builtin.segments, sizeof(out->segments));
    return true;
}

// ---- fakes: s_profiles/profiles_slot_used (profiles_http_internal.h) ------
static profiles_state_t g_fake_profiles_state;
profiles_state_t *profiles_storage_ensure(void)
{
    return &g_fake_profiles_state;
}
bool profiles_slot_used(uint8_t id)
{
    if (id >= PROFILES_MAX_COUNT) {
        return false;
    }
    return (g_fake_profiles_state.used_bitmap.words[0] & (1u << id)) != 0;
}
void profiles_slot_set(uint8_t id)
{
    if (id < PROFILES_MAX_COUNT) {
        g_fake_profiles_state.used_bitmap.words[0] |= (1u << id);
    }
}
void profiles_slot_clear(uint8_t id)
{
    if (id < PROFILES_MAX_COUNT) {
        g_fake_profiles_state.used_bitmap.words[0] &= ~(1u << id);
    }
}

// ---- fakes: profiles_http_get/save/delete (real impl is profiles_http.c,
// not linked here) -- a tiny in-memory slot array is enough to exercise
// profiles_live_http.c's own call sites (fork's origin read, decide's
// save_as/overwrite writes). ----------------------------------------------
static profile_t g_fake_slots[PROFILES_MAX_COUNT];
static bool g_fake_profiles_http_get_fail = false;
static bool g_fake_profiles_http_save_fail = false;

bool profiles_http_get(uint8_t id, profile_t *out)
{
    if (g_fake_profiles_http_get_fail || id >= PROFILES_MAX_COUNT || !profiles_slot_used(id) || !out) {
        return false;
    }
    *out = g_fake_slots[id];
    return true;
}
bool profiles_http_save(uint8_t requested_id, const profile_t *candidate, uint8_t *out_id, uint8_t *out_warning_count,
                         char *err_msg, size_t err_cap)
{
    if (g_fake_profiles_http_save_fail) {
        snprintf(err_msg, err_cap, "save refused (test)");
        return false;
    }
    uint8_t id = requested_id;
    if (id >= PROFILES_MAX_COUNT) {
        for (id = 0; id < PROFILES_MAX_COUNT; id++) {
            if (!profiles_slot_used(id)) {
                break;
            }
        }
        if (id >= PROFILES_MAX_COUNT) {
            snprintf(err_msg, err_cap, "no free slot");
            return false;
        }
    }
    g_fake_slots[id] = *candidate;
    profiles_slot_set(id);
    if (out_id) {
        *out_id = id;
    }
    if (out_warning_count) {
        *out_warning_count = 0;
    }
    return true;
}
bool profiles_http_delete(uint8_t id)
{
    if (id >= PROFILES_MAX_COUNT || !profiles_slot_used(id)) {
        return false;
    }
    profiles_slot_clear(id);
    return true;
}

// ---- fakes: profiles_parse_profile_fields/profiles_validate_candidate
// (real impl is profiles_edit_http.c, not linked here). Body format for
// these tests is a single token: any body containing "badbound" parses to
// a candidate named "badbound" that validate_candidate always rejects
// (simulating a bound violation whose err_msg names the segment/value/
// limit, per the real function's contract); g_fake_parse_ok=false
// simulates a parse failure regardless of body. -----------------------------
static bool g_fake_parse_ok = true;

bool profiles_parse_profile_fields(const char *body, profile_t *p, char *err_msg, size_t err_cap)
{
    if (!g_fake_parse_ok) {
        snprintf(err_msg, err_cap, "bad parse (test)");
        return false;
    }
    memset(p, 0, sizeof(*p));
    strncpy(p->name, (body && strstr(body, "badbound")) ? "badbound" : "cand", sizeof(p->name) - 1);
    p->segment_count = 1;
    p->segments[0].target_c = 100.0f;
    return true;
}

bool profiles_validate_candidate(const profile_t *candidate, profile_validate_mode_t mode, char *warnings_json,
                                  size_t warnings_json_cap, char *err_msg, size_t err_cap)
{
    (void)mode;
    if (warnings_json && warnings_json_cap) {
        warnings_json[0] = '\0';
    }
    if (strcmp(candidate->name, "badbound") == 0) {
        snprintf(err_msg, err_cap, "segment 0 target 3000.0 exceeds limit 2015.0");
        return false;
    }
    return true;
}

// ---- fakes: profile_executor.h's live-status accessor ---------------------
static profile_executor_live_status_t g_fake_live_status;
void profile_executor_get_live_status(profile_executor_live_status_t *out)
{
    *out = g_fake_live_status;
}

// ---- fakes: wifi_provision_http.h / http_auth_http.h ----------------------
static httpd_handle_t g_fake_server = (httpd_handle_t)0x1234;
static bool g_fake_no_server = false;
httpd_handle_t wifi_provision_http_get_server(void)
{
    return g_fake_no_server ? NULL : g_fake_server;
}
static int g_fake_register_count = 0;
esp_err_t kiln_http_register(httpd_handle_t server, const httpd_uri_t *uri_handler)
{
    (void)server;
    (void)uri_handler;
    g_fake_register_count++;
    return ESP_OK;
}

// ---- fakes: web_encoding.h --------------------------------------------
bool web_client_accepts_gzip(httpd_req_t *req)
{
    (void)req;
    return true;
}
esp_err_t web_send_gzip_not_acceptable(httpd_req_t *req, const char *tag, const char *page_name)
{
    (void)req;
    (void)tag;
    (void)page_name;
    return ESP_FAIL;
}
void web_set_asset_cache_headers(httpd_req_t *req)
{
    (void)req;
}

// gzip-embedded page symbols the real firmware gets from the linker.
const uint8_t live_profile_page_html_gz_start[] = {0x1f, 0x8b};
const uint8_t live_profile_page_html_gz_end[] = {0};

// ---- httpd request/response capture stubs (test_ota_http.c's pattern) -----
static const char *s_stub_body_ptr;
static size_t s_stub_body_len;
static size_t s_stub_body_pos;

static char s_resp_status[32];
static char s_resp_body[1024];

static void stub_reset_http(void)
{
    s_stub_body_ptr = NULL;
    s_stub_body_len = 0;
    s_stub_body_pos = 0;
    strncpy(s_resp_status, "200 OK", sizeof(s_resp_status) - 1);
    s_resp_status[sizeof(s_resp_status) - 1] = '\0';
    s_resp_body[0] = '\0';
}

static void stub_set_body(const char *body)
{
    s_stub_body_ptr = body;
    s_stub_body_len = body ? strlen(body) : 0;
    s_stub_body_pos = 0;
}

int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len)
{
    (void)r;
    size_t remaining = s_stub_body_len - s_stub_body_pos;
    size_t n = remaining < buf_len ? remaining : buf_len;
    if (n == 0) {
        return 0;
    }
    memcpy(buf, s_stub_body_ptr + s_stub_body_pos, n);
    s_stub_body_pos += n;
    return (int)n;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler)
{
    (void)handle;
    (void)uri_handler;
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type)
{
    (void)r;
    (void)type;
    return ESP_OK;
}
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value)
{
    (void)r;
    (void)field;
    (void)value;
    return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, long long buf_len)
{
    (void)r;
    // Captured, not discarded: the landing-pass review fix moved every
    // refusal on this surface onto httpd_resp_send() with a JSON
    // {"ok":false,"error":...} body, so a stub that threw the body away
    // would silently hollow out every err-message assertion below.
    if (buf && buf_len > 0) {
        size_t n = (size_t)buf_len;
        if (n >= sizeof(s_resp_body)) {
            n = sizeof(s_resp_body) - 1;
        }
        memcpy(s_resp_body, buf, n);
        s_resp_body[n] = '\0';
    }
    return ESP_OK;
}
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, size_t buf_len)
{
    (void)r;
    if (buf && buf_len > 0) {
        size_t n = buf_len;
        if (n >= sizeof(s_resp_body)) {
            n = sizeof(s_resp_body) - 1;
        }
        memcpy(s_resp_body, buf, n);
        s_resp_body[n] = '\0';
    }
    return ESP_OK;
}
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t error, const char *msg)
{
    (void)r;
    snprintf(s_resp_status, sizeof(s_resp_status), "%d", (int)error);
    strncpy(s_resp_body, msg ? msg : "", sizeof(s_resp_body) - 1);
    s_resp_body[sizeof(s_resp_body) - 1] = '\0';
    return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status)
{
    (void)r;
    strncpy(s_resp_status, status ? status : "", sizeof(s_resp_status) - 1);
    s_resp_status[sizeof(s_resp_status) - 1] = '\0';
    return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s)
{
    (void)r;
    strncpy(s_resp_body, s ? s : "", sizeof(s_resp_body) - 1);
    s_resp_body[sizeof(s_resp_body) - 1] = '\0';
    return ESP_OK;
}

// GET /api/profile/live?content=1 -- query-string seam.
static const char *s_stub_query = NULL;
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t buf_len)
{
    (void)r;
    if (!s_stub_query) {
        return ESP_ERR_NOT_FOUND;
    }
    strncpy(buf, s_stub_query, buf_len - 1);
    buf[buf_len - 1] = '\0';
    return ESP_OK;
}
esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t val_size)
{
    size_t klen = strlen(key);
    const char *p2 = qry;
    while (p2 && *p2) {
        if (strncmp(p2, key, klen) == 0 && p2[klen] == '=') {
            const char *v = p2 + klen + 1;
            const char *amp = strchr(v, '&');
            size_t n = amp ? (size_t)(amp - v) : strlen(v);
            if (n >= val_size) {
                n = val_size - 1;
            }
            memcpy(val, v, n);
            val[n] = '\0';
            return ESP_OK;
        }
        p2 = strchr(p2, '&');
        if (p2) {
            p2++;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

// profiles_live_http.c itself calls dashboard_json.h's PROJECT-WIDE
// json_escape() (quote/backslash only -- out of scope for the Opus review of
// 5dd23944 finding 3, which only widened the THREE profiles_*_http.c copies
// now unified as profiles_http_json_escape() in profiles_http_internal.h,
// a distinct name chosen specifically so it would not collide with this
// dashboard-wide one). dashboard_json.c is not linked into this executable
// (see the file banner), so this remains a faithful minimal stand-in for
// the two characters that actually matter to these responses.
void json_escape(const char *src, char *out, size_t out_cap)
{
    size_t o = 0;
    for (const char *c = src ? src : ""; *c && o + 2 < out_cap; c++) {
        if (*c == '"' || *c == '\\') {
            out[o++] = '\\';
        }
        out[o++] = *c;
    }
    out[o < out_cap ? o : out_cap - 1] = '\0';
}

// asm("_binary_...") is a GCC/binutils extension with no MSVC equivalent --
// #define it away, same convention test_profiles_http.c/test_zones_http.c
// use for the identical embedded-page-blob externs.
#define asm(x)

// Pull in the real modules under test: live_profile.c AFTER the fakes above
// (same convention test_live_profile.c uses for its own real-storage
// backend), then profiles_live_http.c which #includes live_profile.h only
// (declarations) and needs live_profile.c's real bodies linked in.
#include "../drivers/persist/live_profile.c"
#include "../drivers/http/profiles_live_http.c"

#undef asm

// ---------------------------------------------------------------------------
// test helpers

static httpd_req_t make_req(const char *body)
{
    stub_reset_http();
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    if (body) {
        req.content_len = (long long)strlen(body);
        stub_set_body(body);
    }
    return req;
}

static void reset_fakes(void)
{
    memset(&g_fake_live_status, 0, sizeof(g_fake_live_status));
    memset(&g_fake_profiles_state, 0, sizeof(g_fake_profiles_state));
    memset(g_fake_slots, 0, sizeof(g_fake_slots));
    g_fake_profiles_http_get_fail = false;
    g_fake_profiles_http_save_fail = false;
    g_fake_parse_ok = true;
    g_fake_builtin_on = false;
    g_fake_no_server = false;
    s_stub_query = NULL;
    char discard_err[64];
    live_profile_clear(discard_err, sizeof(discard_err));
}

// ---------------------------------------------------------------------------

static void test_get_status_inactive(void)
{
    TEST_SECTION("GET /api/profile/live -- inactive, no pending");
    reset_fakes();
    httpd_req_t req = make_req(NULL);
    esp_err_t err = api_profile_live_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strstr(s_resp_body, "\"active\":false") != NULL, "reports active:false");
}

static void test_get_status_active_with_refusal(void)
{
    TEST_SECTION("GET /api/profile/live -- active, has_refusal");
    reset_fakes();
    g_fake_live_status.active = true;
    g_fake_live_status.profile_id = 2;
    g_fake_live_status.segment_index = 1;
    g_fake_live_status.has_refusal = true;
    g_fake_live_status.refusal_generation = 5;
    g_fake_live_status.refusal_result = 1;
    strncpy(g_fake_live_status.refusal_err_msg, "window violation", sizeof(g_fake_live_status.refusal_err_msg) - 1);

    httpd_req_t req = make_req(NULL);
    esp_err_t err = api_profile_live_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strstr(s_resp_body, "\"active\":true") != NULL, "reports active:true");
    TEST_CHECK(strstr(s_resp_body, "\"origin_id\":2") != NULL, "reports origin_id");
    TEST_CHECK(strstr(s_resp_body, "window violation") != NULL, "reports last_refusal message");
}

static void test_fork_refused_when_inactive(void)
{
    TEST_SECTION("POST /api/profile/live/fork -- 409 when nothing running");
    reset_fakes();
    httpd_req_t req = make_req(NULL);
    esp_err_t err = api_profile_live_fork_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK (sendstr success)");
    TEST_CHECK(strcmp(s_resp_status, "409 Conflict") == 0, "status is 409");
}

static void test_fork_success(void)
{
    TEST_SECTION("POST /api/profile/live/fork -- success, idempotent");
    reset_fakes();
    g_fake_live_status.active = true;
    g_fake_live_status.profile_id = 0;
    profiles_slot_set(0);
    strncpy(g_fake_slots[0].name, "origin", sizeof(g_fake_slots[0].name) - 1);

    httpd_req_t req = make_req(NULL);
    esp_err_t err = api_profile_live_fork_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "first fork ok");
    TEST_CHECK(strstr(s_resp_body, "\"ok\":true") != NULL, "reports ok:true");

    httpd_req_t req2 = make_req(NULL);
    esp_err_t err2 = api_profile_live_fork_post_handler(&req2);
    TEST_CHECK(err2 == ESP_OK, "second fork ok (idempotent)");
    TEST_CHECK(strstr(s_resp_body, "\"ok\":true") != NULL, "idempotent fork still reports ok:true");
}

static void test_accept_requires_active_and_forked(void)
{
    TEST_SECTION("POST /api/profile/live -- 409 when no active firing");
    reset_fakes();
    httpd_req_t req = make_req("body=ok");
    esp_err_t err = api_profile_live_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strcmp(s_resp_status, "409 Conflict") == 0, "409 with nothing running");

    TEST_SECTION("POST /api/profile/live -- 409 when not forked yet");
    g_fake_live_status.active = true;
    g_fake_live_status.profile_id = 0;
    httpd_req_t req2 = make_req("body=ok");
    esp_err_t err2 = api_profile_live_post_handler(&req2);
    TEST_CHECK(err2 == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strcmp(s_resp_status, "409 Conflict") == 0, "409 without a fork");
}

static void fork_for_tests(uint8_t origin_id)
{
    g_fake_live_status.active = true;
    g_fake_live_status.profile_id = origin_id;
    g_fake_live_status.segment_index = 0;
    profiles_slot_set(origin_id);
    strncpy(g_fake_slots[origin_id].name, "origin", sizeof(g_fake_slots[origin_id].name) - 1);
    if (origin_id < PROFILES_MAX_COUNT) {
        /* live_http_name_at() (collision checks, fork's origin_name) reads s_profiles, not g_fake_slots. */
        strncpy(g_fake_profiles_state.profiles[origin_id].name, "origin",
                sizeof(g_fake_profiles_state.profiles[origin_id].name) - 1);
    }
    httpd_req_t freq = make_req(NULL);
    esp_err_t ferr = api_profile_live_fork_post_handler(&freq);
    TEST_CHECK(ferr == ESP_OK, "fork_for_tests: fork ok");
    TEST_CHECK(strstr(s_resp_body, "\"ok\":true") != NULL, "fork_for_tests: fork reports ok");
}

static void test_accept_bad_parse_400(void)
{
    TEST_SECTION("POST /api/profile/live -- 400 on parse failure");
    reset_fakes();
    fork_for_tests(0);
    g_fake_parse_ok = false;

    httpd_req_t req = make_req("garbage");
    esp_err_t err = api_profile_live_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strcmp(s_resp_status, "400 Bad Request") == 0, "400 on bad parse");
}

static void test_accept_bad_bound_400(void)
{
    TEST_SECTION("POST /api/profile/live -- 400 naming segment/value/limit on a bound violation");
    reset_fakes();
    fork_for_tests(0);

    httpd_req_t req = make_req("body=badbound");
    esp_err_t err = api_profile_live_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strcmp(s_resp_status, "400 Bad Request") == 0, "400 on bound violation");
    TEST_CHECK(strstr(s_resp_body, "segment 0") != NULL, "err names the segment");
}

static void test_accept_success_200(void)
{
    TEST_SECTION("POST /api/profile/live -- 200 accept, working slot saved");
    reset_fakes();
    fork_for_tests(0);

    httpd_req_t req = make_req("body=ok");
    esp_err_t err = api_profile_live_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strstr(s_resp_body, "\"ok\":true") != NULL, "200 with ok:true");

    profile_t working;
    TEST_CHECK(live_profile_load_working(&working), "working slot readable after accept");
    TEST_CHECK(strcmp(working.name, "cand") == 0, "working slot holds the parsed candidate");
}

static void test_decide_nothing_pending_409(void)
{
    TEST_SECTION("POST /api/profile/live/decide -- 409 when nothing pending");
    reset_fakes();
    httpd_req_t req = make_req("action=discard");
    esp_err_t err = api_profile_live_decide_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strcmp(s_resp_status, "409 Conflict") == 0, "409 nothing pending");
}

static void test_decide_discard(void)
{
    TEST_SECTION("POST /api/profile/live/decide -- discard clears the pending record");
    reset_fakes();
    fork_for_tests(0);

    httpd_req_t req = make_req("action=discard");
    esp_err_t err = api_profile_live_decide_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strstr(s_resp_body, "\"ok\":true") != NULL, "discard reports ok:true");

    live_edit_record_t rec;
    TEST_CHECK(!live_profile_load_record(&rec) || !rec.pending, "record no longer pending after discard");
}

static void test_decide_save_as_missing_name_400(void)
{
    TEST_SECTION("POST /api/profile/live/decide -- save_as without name is 400");
    reset_fakes();
    fork_for_tests(0);
    httpd_req_t req = make_req("action=save_as");
    esp_err_t err = api_profile_live_decide_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strcmp(s_resp_status, "400 Bad Request") == 0, "400 missing name");
}

static void test_decide_save_as_success(void)
{
    TEST_SECTION("POST /api/profile/live/decide -- save_as into a free slot");
    reset_fakes();
    fork_for_tests(0);

    httpd_req_t req = make_req("action=save_as&name=NewOne");
    esp_err_t err = api_profile_live_decide_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strstr(s_resp_body, "\"ok\":true") != NULL, "save_as ok:true");
    TEST_CHECK(profiles_slot_used(1), "landed in the next free slot (0 is origin)");
}

static void test_decide_overwrite_builtin_403(void)
{
    TEST_SECTION("POST /api/profile/live/decide -- overwrite of a builtin origin is 403");
    reset_fakes();
    g_fake_builtin_on = true;
    fork_for_tests(PROFILE_BUILTIN_ID_BASE);

    httpd_req_t req = make_req("action=overwrite&confirm=1");
    esp_err_t err = api_profile_live_decide_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strcmp(s_resp_status, "403 Forbidden") == 0, "403 on builtin overwrite");
}

static void test_decide_overwrite_missing_confirm_400(void)
{
    TEST_SECTION("POST /api/profile/live/decide -- overwrite without confirm=1 is 400");
    reset_fakes();
    fork_for_tests(0);

    httpd_req_t req = make_req("action=overwrite");
    esp_err_t err = api_profile_live_decide_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strcmp(s_resp_status, "400 Bad Request") == 0, "400 missing confirm");
}

static void test_decide_overwrite_success(void)
{
    TEST_SECTION("POST /api/profile/live/decide -- overwrite with confirm=1 succeeds");
    reset_fakes();
    fork_for_tests(0);

    httpd_req_t req = make_req("action=overwrite&confirm=1");
    esp_err_t err = api_profile_live_decide_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strstr(s_resp_body, "\"ok\":true") != NULL, "overwrite ok:true");
}

static void test_decide_unknown_action_400(void)
{
    TEST_SECTION("POST /api/profile/live/decide -- unknown action is 400");
    reset_fakes();
    fork_for_tests(0);
    httpd_req_t req = make_req("action=bogus");
    esp_err_t err = api_profile_live_decide_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strcmp(s_resp_status, "400 Bad Request") == 0, "400 unknown action");
}

/* ---- shared decide core (HTTP and LCD both call it) ---------------------- */

static void test_decide_core_nothing_pending(void)
{
    TEST_SECTION("profiles_live_decide_apply -- NOTHING_PENDING with no record");
    reset_fakes();
    char err[64];
    profiles_live_decide_status_t st;
    profiles_live_decide_status(&st);
    TEST_CHECK(!st.record_pending && !st.pending_decision, "status: nothing pending");
    TEST_CHECK(profiles_live_decide_apply(LIVE_EDIT_DECISION_DISCARD, NULL, false, NULL, err, sizeof(err)) ==
                   LIVE_DECIDE_NOTHING_PENDING,
               "discard with no record -> NOTHING_PENDING");
}

static void test_decide_core_status_pending_decision_follows_executor(void)
{
    TEST_SECTION("profiles_live_decide_status -- pending_decision false while running, true after the end");
    reset_fakes();
    fork_for_tests(0);
    profiles_live_decide_status_t st;
    profiles_live_decide_status(&st);
    TEST_CHECK(st.record_pending && !st.pending_decision, "running: record pending but no decision owed yet");
    TEST_CHECK(!st.origin_is_builtin, "user origin is not builtin");
    TEST_CHECK(strcmp(st.origin_name, "origin") == 0, "origin name carried through");
    g_fake_live_status.active = false;
    profiles_live_decide_status(&st);
    TEST_CHECK(st.pending_decision, "firing ended: decision owed");
}

static void test_decide_core_discard_save_overwrite(void)
{
    TEST_SECTION("profiles_live_decide_apply -- discard / save_as / overwrite results");
    char err[96];
    uint8_t id = 0xFF;

    reset_fakes();
    fork_for_tests(0);
    g_fake_live_status.active = false;
    TEST_CHECK(profiles_live_decide_apply(LIVE_EDIT_DECISION_DISCARD, NULL, false, NULL, err, sizeof(err)) ==
                   LIVE_DECIDE_OK,
               "discard OK");
    live_edit_record_t rec;
    TEST_CHECK(!live_profile_load_record(&rec) || !rec.pending, "record cleared after discard");

    reset_fakes();
    fork_for_tests(0);
    TEST_CHECK(profiles_live_decide_apply(LIVE_EDIT_DECISION_SAVE_AS, NULL, false, &id, err, sizeof(err)) ==
                   LIVE_DECIDE_BAD_REQUEST,
               "save_as with no name -> BAD_REQUEST");
    TEST_CHECK(profiles_live_decide_apply(LIVE_EDIT_DECISION_SAVE_AS, "origin", false, &id, err, sizeof(err)) ==
                   LIVE_DECIDE_BAD_REQUEST,
               "save_as onto an existing user name -> BAD_REQUEST");
    TEST_CHECK(profiles_live_decide_apply(LIVE_EDIT_DECISION_SAVE_AS, "Fresh", false, &id, err, sizeof(err)) ==
                   LIVE_DECIDE_OK,
               "save_as new name OK");
    TEST_CHECK(id == 1 && profiles_slot_used(1), "landed in first free slot");
    TEST_CHECK(!live_profile_load_record(&rec) || !rec.pending, "record cleared after save_as");

    reset_fakes();
    fork_for_tests(0);
    TEST_CHECK(profiles_live_decide_apply(LIVE_EDIT_DECISION_OVERWRITE, NULL, false, NULL, err, sizeof(err)) ==
                   LIVE_DECIDE_BAD_REQUEST,
               "overwrite without confirm -> BAD_REQUEST");
    TEST_CHECK(profiles_live_decide_apply(LIVE_EDIT_DECISION_OVERWRITE, NULL, true, &id, err, sizeof(err)) ==
                   LIVE_DECIDE_OK,
               "overwrite with confirm OK");
    TEST_CHECK(id == 0, "overwrite targets the origin slot");
}

static void test_decide_core_overwrite_builtin_forbidden_even_with_confirm(void)
{
    TEST_SECTION("profiles_live_decide_apply -- builtin origin: overwrite FORBIDDEN, save_as still allowed");
    reset_fakes();
    g_fake_builtin_on = true;
    fork_for_tests(PROFILE_BUILTIN_ID_BASE);
    char err[96];
    profiles_live_decide_status_t st;
    profiles_live_decide_status(&st);
    TEST_CHECK(st.origin_is_builtin, "status flags builtin origin (LCD disables Overwrite from this)");
    TEST_CHECK(profiles_live_decide_apply(LIVE_EDIT_DECISION_OVERWRITE, NULL, true, NULL, err, sizeof(err)) ==
                   LIVE_DECIDE_FORBIDDEN,
               "overwrite of builtin -> FORBIDDEN even with confirm");
    live_edit_record_t rec;
    TEST_CHECK(live_profile_load_record(&rec) && rec.pending, "refusal leaves the record pending");
    TEST_CHECK(profiles_live_decide_apply(LIVE_EDIT_DECISION_SAVE_AS, "FromBuiltin", false, NULL, err, sizeof(err)) ==
                   LIVE_DECIDE_OK,
               "save_as of a builtin-origin edit OK");
}

static void test_decide_core_default_name(void)
{
    TEST_SECTION("profiles_live_decide_default_name -- origin-E, then -E2 on collision, clipped to 15");
    reset_fakes();
    fork_for_tests(0);
    profiles_live_decide_status_t st;
    profiles_live_decide_status(&st);
    char name[PROFILE_NAME_MAX_LEN + 1];
    TEST_CHECK(profiles_live_decide_default_name(&st, name, sizeof(name)), "name generated");
    TEST_CHECK(strcmp(name, "origin-E") == 0, "first candidate is <origin>-E");

    profiles_slot_set(2);
    strncpy(g_fake_profiles_state.profiles[2].name, "origin-E", sizeof(g_fake_profiles_state.profiles[2].name) - 1);
    TEST_CHECK(profiles_live_decide_default_name(&st, name, sizeof(name)), "name generated after collision");
    TEST_CHECK(strcmp(name, "origin-E2") == 0, "collision moves to -E2");

    strncpy(st.origin_name, "ABCDEFGHIJKLMNO", sizeof(st.origin_name) - 1);
    TEST_CHECK(profiles_live_decide_default_name(&st, name, sizeof(name)), "long origin handled");
    TEST_CHECK(strlen(name) <= PROFILE_NAME_MAX_LEN && strcmp(name, "ABCDEFGHIJKLM-E") == 0,
               "clipped to 15 chars keeping the suffix");
}

static void test_decide_core_default_name_utf8_boundary(void)
{
    TEST_SECTION("profiles_live_decide_default_name -- clip never splits a UTF-8 sequence");
    reset_fakes();
    profiles_live_decide_status_t st;
    memset(&st, 0, sizeof(st));
    char name[PROFILE_NAME_MAX_LEN + 1];

    /* keep = 13 bytes of base. 12 ASCII + 2-byte e-acute: bytes 12..13, so a
     * 13-byte cut lands between the lead and continuation byte. */
    strcpy(st.origin_name, "ABCDEFGHIJKL\xC3\xA9Z");
    TEST_CHECK(profiles_live_decide_default_name(&st, name, sizeof(name)), "2-byte straddle: name generated");
    TEST_CHECK(strcmp(name, "ABCDEFGHIJKL-E") == 0, "2-byte sequence dropped whole, not split");

    /* 11 ASCII + 3-byte euro sign: bytes 11..13, cut at 13 leaves 2 of 3. */
    strcpy(st.origin_name, "ABCDEFGHIJK\xE2\x82\xACZ");
    TEST_CHECK(profiles_live_decide_default_name(&st, name, sizeof(name)), "3-byte straddle: name generated");
    TEST_CHECK(strcmp(name, "ABCDEFGHIJK-E") == 0, "3-byte sequence dropped whole, not split");

    /* A multibyte char that ends exactly on the cut is kept. */
    strcpy(st.origin_name, "ABCDEFGHIJK\xC3\xA9Z");
    TEST_CHECK(profiles_live_decide_default_name(&st, name, sizeof(name)), "exact-fit: name generated");
    TEST_CHECK(strcmp(name, "ABCDEFGHIJK\xC3\xA9-E") == 0, "complete sequence at the cut is kept");
}

static void test_decide_core_default_name_all_collide(void)
{
    TEST_SECTION("auto-name -- all nine -E..-E9 names taken: clean failure, no overwrite");
    reset_fakes();
    fork_for_tests(0);
    static const char *const taken[9] = {"origin-E",  "origin-E2", "origin-E3", "origin-E4", "origin-E5",
                                         "origin-E6", "origin-E7", "origin-E8", "origin-E9"};
    for (uint8_t i = 0; i < 9; i++) {
        profiles_slot_set((uint8_t)(i + 1));
        strncpy(g_fake_profiles_state.profiles[i + 1].name, taken[i],
                sizeof(g_fake_profiles_state.profiles[i + 1].name) - 1);
    }
    profiles_live_decide_status_t st;
    profiles_live_decide_status(&st);
    char name[PROFILE_NAME_MAX_LEN + 1] = "untouched";
    TEST_CHECK(!profiles_live_decide_default_name(&st, name, sizeof(name)), "no free auto-name -> false");

    /* The LCD falls back to "save on the web"; forcing a colliding name through
     * apply must be refused, never silently replace the existing profile. */
    char err[96];
    uint8_t id = 0xFF;
    TEST_CHECK(profiles_live_decide_apply(LIVE_EDIT_DECISION_SAVE_AS, "origin-E", false, &id, err, sizeof(err)) ==
                   LIVE_DECIDE_BAD_REQUEST,
               "save_as onto a taken auto-name -> BAD_REQUEST");
    TEST_CHECK(err[0] != '\0', "refusal carries a reason");
    TEST_CHECK(id == 0xFF, "no slot id reported");
    live_edit_record_t rec;
    TEST_CHECK(live_profile_load_record(&rec) && rec.pending, "record still pending after the refusal");
}

static void test_decide_discard_clear_failure_500(void)
{
    TEST_SECTION("discard -- live_profile_clear failure is a 500, record stays pending");
    reset_fakes();
    fork_for_tests(0);
    fake_kv_set_write_safe_here(false); /* live_profile_clear() refuses on a non-write-safe caller */
    char err[96];
    TEST_CHECK(profiles_live_decide_apply(LIVE_EDIT_DECISION_DISCARD, NULL, false, NULL, err, sizeof(err)) ==
                   LIVE_DECIDE_SERVER_ERROR,
               "core: discard with a failing clear -> SERVER_ERROR");
    TEST_CHECK(err[0] != '\0', "core: reason reported");

    httpd_req_t req = make_req("action=discard");
    esp_err_t e = api_profile_live_decide_post_handler(&req);
    fake_kv_set_write_safe_here(true);
    TEST_CHECK(e == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strcmp(s_resp_status, "500 Internal Server Error") == 0, "HTTP 500, not a false ok:true");
    TEST_CHECK(strstr(s_resp_body, "\"ok\":true") == NULL, "body does not claim success");
    live_edit_record_t rec;
    TEST_CHECK(live_profile_load_record(&rec) && rec.pending, "record still pending");
}

static void test_decide_lock_timeout_is_server_error(void)
{
    TEST_SECTION("decide lock -- timeout refuses with SERVER_ERROR and touches nothing");
    reset_fakes();
    fork_for_tests(0);
    decide_lock_init();
    TEST_CHECK(s_decide_lock != NULL, "lock created");

    g_test_stub_semaphore_take_default = pdTRUE;
    g_test_stub_lock_depth = 0;
    char err[96];
    TEST_CHECK(profiles_live_decide_apply(LIVE_EDIT_DECISION_DISCARD, NULL, false, NULL, err, sizeof(err)) ==
                   LIVE_DECIDE_OK,
               "lock acquired -> discard proceeds");
    TEST_CHECK(g_test_stub_lock_depth == 0, "lock released after the apply");

    fork_for_tests(0);
    g_test_stub_semaphore_take_default = pdFALSE; /* simulate another decision holding the lock */
    g_test_stub_lock_depth = 0;
    TEST_CHECK(profiles_live_decide_apply(LIVE_EDIT_DECISION_DISCARD, NULL, false, NULL, err, sizeof(err)) ==
                   LIVE_DECIDE_SERVER_ERROR,
               "lock timeout -> SERVER_ERROR");
    TEST_CHECK(strstr(err, "in progress") != NULL, "reason names the cause");
    live_edit_record_t rec;
    TEST_CHECK(live_profile_load_record(&rec) && rec.pending, "record untouched on timeout");
    g_test_stub_semaphore_take_default = pdTRUE;
}

/* ---- landing-pass review-fix regression tests --------------------------- */

// working_id must be -1 until a fork has actually happened. Before this fix
// the handler reported LIVE_EDIT_WORKING_SLOT_ID unconditionally whenever a
// firing was active, so live_profile_page.html could never tell "running,
// nothing forked" from "a working copy exists": its Fork button never
// appeared and it immediately tried to load a working copy that was not
// there.
static void test_get_status_working_id_minus_one_until_forked(void)
{
    TEST_SECTION("GET /api/profile/live -- working_id is -1 until a fork exists");
    reset_fakes();
    g_fake_live_status.active = true;
    g_fake_live_status.profile_id = 0;
    profiles_slot_set(0);

    httpd_req_t req = make_req(NULL);
    TEST_CHECK(api_profile_live_get_handler(&req) == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strstr(s_resp_body, "\"working_id\":-1") != NULL, "no fork yet -> working_id:-1");

    fork_for_tests(0);
    httpd_req_t req2 = make_req(NULL);
    TEST_CHECK(api_profile_live_get_handler(&req2) == ESP_OK, "handler returns ESP_OK after fork");
    char expect[32];
    snprintf(expect, sizeof(expect), "\"working_id\":%d", (int)LIVE_EDIT_WORKING_SLOT_ID);
    TEST_CHECK(strstr(s_resp_body, expect) != NULL, "after fork -> working_id is the working slot id");
}

// last_refusal is an object or null -- never a bare boolean. The inactive
// branch used to emit `"last_refusal":false`, which the page rendered into
// its red banner as the literal word "false".
static void test_get_status_last_refusal_is_null_not_false(void)
{
    TEST_SECTION("GET /api/profile/live -- last_refusal is null, never a boolean");
    reset_fakes();
    httpd_req_t req = make_req(NULL);
    TEST_CHECK(api_profile_live_get_handler(&req) == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strstr(s_resp_body, "\"last_refusal\":null") != NULL, "inactive, no refusal -> null");
    TEST_CHECK(strstr(s_resp_body, "\"last_refusal\":false") == NULL, "never the boolean false");
    TEST_CHECK(strstr(s_resp_body, "\"last_refusal\":true") == NULL, "never the boolean true");
}

// ?content=1 serves the working copy's body. The page needs this because the
// working slot deliberately lives OUTSIDE profiles_http.c's slot array (so it
// can never appear in the catalogue or in favorites), which means
// GET /api/profile?id=<working_id> can only ever 404 on it.
static void test_get_content_requires_a_working_copy(void)
{
    TEST_SECTION("GET /api/profile/live?content=1 -- 409 before any fork");
    reset_fakes();
    s_stub_query = "content=1";
    httpd_req_t req = make_req(NULL);
    TEST_CHECK(api_profile_live_get_handler(&req) == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strcmp(s_resp_status, "409 Conflict") == 0, "409 with no working copy");
    TEST_CHECK(strstr(s_resp_body, "\"ok\":false") != NULL, "refusal body is JSON, not bare text");
}

static void test_get_content_serves_the_working_copy(void)
{
    TEST_SECTION("GET /api/profile/live?content=1 -- serves the working copy body");
    reset_fakes();
    fork_for_tests(0);
    httpd_req_t areq = make_req("body=ok");
    TEST_CHECK(api_profile_live_post_handler(&areq) == ESP_OK, "accept an edit first");

    s_stub_query = "content=1";
    httpd_req_t req = make_req(NULL);
    TEST_CHECK(api_profile_live_get_handler(&req) == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strstr(s_resp_body, "\"name\":\"cand\"") != NULL, "body carries the working copy's name");
    TEST_CHECK(strstr(s_resp_body, "\"segments\":[") != NULL, "body carries a segments array");
    TEST_CHECK(strstr(s_resp_body, "\"seg_kind\":0") != NULL, "segment objects carry seg_kind");
    TEST_CHECK(strstr(s_resp_body, "\"target_c\":100.00") != NULL, "segment objects carry target_c");
    char expect[40];
    snprintf(expect, sizeof(expect), "\"id\":%u", (unsigned)LIVE_EDIT_WORKING_SLOT_ID);
    TEST_CHECK(strstr(s_resp_body, expect) != NULL, "body reports the working slot id");
}

// The edit-window rule's polarity, pinned at the handler. live_edit_check_
// window() returns TRUE on a REFUSAL, so `if (check_window(...)) 409` is the
// correct reading -- an inverted handler would 409 every legal edit and wave
// every illegal one through. Here the running profile has two segments and
// the executor is on index 1, while the candidate has only one segment: the
// running segment itself would be deleted out from under the run.
static void test_accept_window_violation_409(void)
{
    TEST_SECTION("POST /api/profile/live -- 409 on an edit-window violation");
    reset_fakes();
    fork_for_tests(0);
    g_fake_slots[0].segment_count = 2;
    g_fake_live_status.segment_index = 1;

    httpd_req_t req = make_req("body=ok");
    TEST_CHECK(api_profile_live_post_handler(&req) == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strcmp(s_resp_status, "409 Conflict") == 0, "409 on a window violation");
    TEST_CHECK(strstr(s_resp_body, "\"ok\":false") != NULL, "refusal body is JSON");
}

// ...and the same handler must NOT refuse a legal edit: same fork, executor
// on the segment the candidate still has. Without this pair the test above
// would pass just as happily against an always-409 handler.
static void test_accept_inside_the_window_200(void)
{
    TEST_SECTION("POST /api/profile/live -- an in-window edit is accepted");
    reset_fakes();
    fork_for_tests(0);
    g_fake_slots[0].segment_count = 1;
    g_fake_live_status.segment_index = 0;

    httpd_req_t req = make_req("body=ok");
    TEST_CHECK(api_profile_live_post_handler(&req) == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strcmp(s_resp_status, "200 OK") == 0, "no refusal status set");
    TEST_CHECK(strstr(s_resp_body, "\"ok\":true") != NULL, "accepted");
}

// Every refusal on this surface is {"ok":false,"error":...} JSON with the
// right status -- the shape POST /api/profile already uses, and the shape the
// page's `r.json().then(d => d.error)` error path requires. A plain-text body
// made the page report a lost connection instead of the server's own message.
static void test_refusals_are_json_with_an_error_field(void)
{
    TEST_SECTION("refusals carry {\"ok\":false,\"error\":...} JSON");
    reset_fakes();
    fork_for_tests(0);
    httpd_req_t req = make_req("body=badbound");
    TEST_CHECK(api_profile_live_post_handler(&req) == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strcmp(s_resp_status, "400 Bad Request") == 0, "400 on a bound violation");
    TEST_CHECK(strstr(s_resp_body, "\"ok\":false") != NULL, "body has ok:false");
    TEST_CHECK(strstr(s_resp_body, "\"error\":\"segment 0") != NULL,
               "body has an error field naming the segment");
}

static void test_registration_registers_all_five_routes(void)
{
    TEST_SECTION("profiles_live_http_start() registers all five routes");
    reset_fakes();
    g_fake_register_count = 0;
    esp_err_t err = profiles_live_http_start();
    TEST_CHECK(err == ESP_OK, "start() returns ESP_OK");
    TEST_CHECK(g_fake_register_count == 5, "exactly five routes registered");
}

static void test_registration_no_server(void)
{
    TEST_SECTION("profiles_live_http_start() fails cleanly with no server");
    reset_fakes();
    g_fake_no_server = true;
    esp_err_t err = profiles_live_http_start();
    TEST_CHECK(err != ESP_OK, "start() fails without a server");
    g_fake_no_server = false;
}

int main(void)
{
    // fake_kv.c requires every partition to be explicitly initialized before
    // hal_kv_open() will succeed -- same requirement test_live_profile.c's
    // main() documents. LIVE_PROFILE_NVS_PARTITION's literal is duplicated
    // here since that macro is private to live_profile.c.
    hal_kv_init_partition("profiles_nvs");
    // The host stub's xSemaphoreTake() defaults to pdFALSE (timeout); the
    // decide lock (created here exactly as profiles_live_http_start() does)
    // must be takeable for every other test.
    g_test_stub_semaphore_take_default = pdTRUE;
    decide_lock_init();

    test_get_status_inactive();
    test_get_status_active_with_refusal();
    test_fork_refused_when_inactive();
    test_fork_success();
    test_accept_requires_active_and_forked();
    test_accept_bad_parse_400();
    test_accept_bad_bound_400();
    test_accept_success_200();
    test_decide_nothing_pending_409();
    test_decide_discard();
    test_decide_save_as_missing_name_400();
    test_decide_save_as_success();
    test_decide_overwrite_builtin_403();
    test_decide_overwrite_missing_confirm_400();
    test_decide_overwrite_success();
    test_decide_unknown_action_400();
    test_decide_core_nothing_pending();
    test_decide_core_status_pending_decision_follows_executor();
    test_decide_core_discard_save_overwrite();
    test_decide_core_overwrite_builtin_forbidden_even_with_confirm();
    test_decide_core_default_name();
    test_decide_core_default_name_utf8_boundary();
    test_decide_core_default_name_all_collide();
    test_decide_discard_clear_failure_500();
    test_decide_lock_timeout_is_server_error();
    test_get_status_working_id_minus_one_until_forked();
    test_get_status_last_refusal_is_null_not_false();
    test_get_content_requires_a_working_copy();
    test_get_content_serves_the_working_copy();
    test_accept_window_violation_409();
    test_accept_inside_the_window_200();
    test_refusals_are_json_with_an_error_field();
    test_registration_registers_all_five_routes();
    test_registration_no_server();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    return 0;
}
