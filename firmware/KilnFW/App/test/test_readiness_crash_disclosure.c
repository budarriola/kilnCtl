// Host tests for GET /api/readiness's crash-report disclosure redaction
// (2026-09-17 ROUTE_TIER_OPEN disclosure audit, finding 2): the "Unacknowledged
// crash report" checklist item's `detail` string (readiness_http.c,
// api_readiness_get_handler(), the crash_report_record_t block) used to embed
// rec.exc_cause_str/rec.exc_task with no auth check at all, sidestepping
// GET /api/crash_report's deliberate ROUTE_TIER_ADMIN classification for the
// exact same facts. The route itself stays ROUTE_TIER_OPEN (an unauthenticated
// caller must still be told a crash is unacknowledged -- capability_preflight
// depends on that, and it is a safety fact, not a secret) -- only the
// cause-string/task-name DETAIL is now gated behind
// `may_disclose = !http_auth_policy_web_enabled() || http_auth_caller_is_admin(req)`,
// same disjunction as the wifi-provision fix in this same commit (see
// wifi_prov.h's wifi_prov_status_redact_field() doc comment for why it is a
// disjunction, not `&&`).
//
// APPROACH: readiness_http.c pulls in a huge ESP-IDF/hardware dependency
// surface (httpd, crash_report, safety link, thermo owner, ...) and, fatally
// for host-compiling it directly, two GCC-only `asm("_binary_...")`
// blob-extern declarations (readiness_page_html_gz_start/_end) that MSVC's
// `cl` cannot parse -- confirmed by inspection, same blocker
// wifi_provision_http.c has for the sibling fix in this commit. Compiling
// api_readiness_get_handler() itself on this host toolchain is not feasible
// without a much larger, out-of-scope stub-and-split effort.
//
// This file instead tests the two things that actually make up the fix,
// for real:
//   1. readiness_crash_report_detail() (readiness_http.h) -- the pure
//      formatter the handler calls, exercised directly across every
//      have_record/acknowledged/may_disclose combination the handler can
//      reach. Pure by the same "facts in, decision out" convention this
//      header's own readiness_*_status() predicates already use, precisely
//      so it CAN be host-tested without the handler around it.
//   2. The real `may_disclose` composition, using the actual production
//      http_auth_policy_web_enabled()/http_auth_caller_is_admin() stack
//      (http_auth_http.c/http_auth_enforce.c/http_auth_policy_iface.c/
//      http_session_iface.c/web_auth_store.c/web_auth_session.c, all linked
//      in for real -- same set test_dashboard_status_http.c links for the
//      sibling GET /api/status fix), across all three auth states: web auth
//      off, web auth on with an admin session, web auth on with no/non-admin
//      session. The resulting bool is fed into readiness_crash_report_detail()
//      exactly as api_readiness_get_handler() does.
//
// Together these cover the same ground a full-handler test would (the real
// gate composition AND the real redaction logic), without requiring
// readiness_http.c itself to compile on this host toolchain.
//
// No real Wi-Fi credential or crash data appears anywhere below -- all
// cause-string/task-name values are obviously synthetic placeholders.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include "esp_err.h"
#include "esp_http_server.h"

#include "psa/crypto.h"
psa_status_t g_stub_psa_import_key_result = PSA_SUCCESS;

#include "http_auth_disclosure_gate.h"
#include "http_auth_http.h"
#include "http_auth_policy_iface.h"
#include "web_auth_session.h"
#include "http_session_iface.h"
#include "web_auth_store.h"
#include "hal_kv.h"
#include "fake_kv.h"

// Same forward-declare-at-point-of-use precedent test_dashboard_status_http.c
// / test_ota_http.c use: http_session_table() has external linkage
// (http_session_iface.c) but is deliberately not declared in
// http_session_iface.h.
web_auth_table_t *http_session_table(void);

#include "readiness_http.h"

// ---------------------------------------------------------------------------
// esp_http_server.h stub bodies -- minimal set actually needed to link the
// real auth stack (http_auth_http.c calls httpd_req_get_hdr_value_len/_str
// and httpd_req_to_sockfd; nothing else in that stack touches the req at
// all). No handler under test here uses httpd_resp_*, so those aren't
// stubbed -- this executable never calls them.
// ---------------------------------------------------------------------------
#define STUB_HDR_MAX 4
typedef struct { const char *field; char value[80]; bool set; } stub_hdr_t;
static stub_hdr_t s_stub_hdrs[STUB_HDR_MAX];

static void stub_headers_reset(void) { memset(s_stub_hdrs, 0, sizeof(s_stub_hdrs)); }

static void stub_header_set(const char *field, const char *value)
{
    for (int i = 0; i < STUB_HDR_MAX; i++) {
        if (!s_stub_hdrs[i].set || strcmp(s_stub_hdrs[i].field, field) == 0) {
            s_stub_hdrs[i].field = field;
            strncpy(s_stub_hdrs[i].value, value, sizeof(s_stub_hdrs[i].value) - 1);
            s_stub_hdrs[i].set = true;
            return;
        }
    }
}

size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field)
{
    (void)r;
    for (int i = 0; i < STUB_HDR_MAX; i++) {
        if (s_stub_hdrs[i].set && strcmp(s_stub_hdrs[i].field, field) == 0) {
            return strlen(s_stub_hdrs[i].value);
        }
    }
    return 0;
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t val_size)
{
    (void)r;
    for (int i = 0; i < STUB_HDR_MAX; i++) {
        if (s_stub_hdrs[i].set && strcmp(s_stub_hdrs[i].field, field) == 0) {
            strncpy(val, s_stub_hdrs[i].value, val_size - 1);
            val[val_size - 1] = '\0';
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

int httpd_req_to_sockfd(httpd_req_t *r) { (void)r; return -1; }

esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler)
{
    (void)handle; (void)uri_handler;
    return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, long long buf_len)
{
    (void)r; (void)buf; (void)buf_len;
    return ESP_OK;
}
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t error, const char *msg)
{
    (void)r; (void)error; (void)msg;
    return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status) { (void)r; (void)status; return ESP_OK; }

// ota_http_get_client_ip() -- http_auth_http.c's resolve_role_for_request()
// path calls this to bind a session lookup to the caller's IP (ca7a7d31).
// Must match the client_ip literal passed to web_auth_table_create_session()
// below, same pairing test_dashboard_status_http.c documents.
void ota_http_get_client_ip(httpd_req_t *req, char *out, size_t out_len)
{
    (void)req;
    if (out_len > 0) {
        strncpy(out, "10.0.0.9", out_len - 1);
        out[out_len - 1] = '\0';
    }
}

// ---------------------------------------------------------------------------
// Session helper -- mints a real ADMIN/USER session in the real session
// table, same as test_dashboard_status_http.c's own helper.
// ---------------------------------------------------------------------------
static const char *readiness_test_make_session(web_auth_session_role_t role, const char *token)
{
    uint8_t hash[32];
    size_t hash_len = 0;
    psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)token, strlen(token), hash, sizeof(hash), &hash_len);
    web_auth_table_create_session(http_session_table(), hash, "10.0.0.9", role, (uint32_t)0);
    return token;
}

static bool compute_may_disclose(void)
{
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    /* 2026-09-17 adversarial-review follow-up: calls the real, shared
     * http_auth_may_disclose() (http_auth_disclosure_gate.c, linked below
     * for real, never stubbed) rather than re-deriving the disjunction by
     * hand here -- the original version of this test file did the latter,
     * which is exactly why it could not detect readiness_http.c's call
     * site being replaced with `bool may_disclose = true;`: the test and
     * the (uncompilable) production call site were two independent copies
     * of the same expression. This is now the one production function both
     * the readiness handler and this test call. */
    return http_auth_may_disclose(&req);
}

// ---------------------------------------------------------------------------
// 1. readiness_crash_report_detail() exercised directly -- pure function,
//    every branch the handler can reach.
// ---------------------------------------------------------------------------
static void test_detail_no_record(void)
{
    TEST_SECTION("readiness_crash_report_detail -- no crash on record");
    char detail[256];
    readiness_crash_report_detail(false, false, true, NULL, NULL, detail, sizeof(detail));
    TEST_CHECK(strcmp(detail, "no crash on record") == 0, "no-record message, may_disclose irrelevant");
    readiness_crash_report_detail(false, false, false, NULL, NULL, detail, sizeof(detail));
    TEST_CHECK(strcmp(detail, "no crash on record") == 0, "no-record message, may_disclose false too");
}

static void test_detail_acknowledged(void)
{
    TEST_SECTION("readiness_crash_report_detail -- acknowledged crash on record");
    char detail[256];
    readiness_crash_report_detail(true, true, true, "SYNTH_TEST_CAUSE", "synth_task", detail, sizeof(detail));
    TEST_CHECK(strcmp(detail, "last crash on record has been acknowledged") == 0,
              "acknowledged message, may_disclose irrelevant");
    TEST_CHECK(strstr(detail, "SYNTH_TEST_CAUSE") == NULL, "acknowledged message never echoes cause anyway");
}

static void test_detail_unacknowledged_may_disclose(void)
{
    TEST_SECTION("readiness_crash_report_detail -- unacknowledged, may_disclose true: cause/task present");
    char detail[256];
    readiness_crash_report_detail(true, false, true, "SYNTH_TEST_CAUSE", "synth_task", detail, sizeof(detail));
    TEST_CHECK(strstr(detail, "unacknowledged") != NULL, "existence of unacknowledged crash still reported");
    TEST_CHECK(strstr(detail, "SYNTH_TEST_CAUSE") != NULL, "may_disclose true -- cause string present");
    TEST_CHECK(strstr(detail, "synth_task") != NULL, "may_disclose true -- task name present");
}

static void test_detail_unacknowledged_redacted(void)
{
    TEST_SECTION("readiness_crash_report_detail -- unacknowledged, may_disclose false: existence kept, "
                 "cause/task redacted (this commit's finding 2 fix)");
    char detail[256];
    readiness_crash_report_detail(true, false, false, "SYNTH_TEST_CAUSE", "synth_task", detail, sizeof(detail));
    TEST_CHECK(strstr(detail, "unacknowledged") != NULL,
              "existence of unacknowledged crash MUST still be reported -- capability_preflight depends on it");
    TEST_CHECK(strstr(detail, "SYNTH_TEST_CAUSE") == NULL, "may_disclose false -- cause string redacted");
    TEST_CHECK(strstr(detail, "synth_task") == NULL, "may_disclose false -- task name redacted");
}

static void test_detail_unacknowledged_redacted_null_fields(void)
{
    TEST_SECTION("readiness_crash_report_detail -- unacknowledged, may_disclose false, NULL cause/task "
                 "(matches handler's have_record?rec.exc_cause_str:NULL call shape)");
    char detail[256];
    readiness_crash_report_detail(true, false, false, NULL, NULL, detail, sizeof(detail));
    TEST_CHECK(strstr(detail, "unacknowledged") != NULL, "existence still reported with NULL inputs");
    TEST_CHECK(strlen(detail) < sizeof(detail), "no overrun with NULL cause/task");
}

// ---------------------------------------------------------------------------
// 2. may_disclose composed from the REAL auth stack, all three states, then
//    fed into the real formatter -- proves the gate itself, not just the
//    formatter in isolation.
// ---------------------------------------------------------------------------
static void test_gate_auth_off(void)
{
    TEST_SECTION("may_disclose composition -- web auth OFF (default/never-configured board): "
                 "http_auth_caller_is_admin() alone would already return true here, so the "
                 "disjunction's first term is what actually matters on a fresh board");
    web_auth_policy_t policy = { .web_enabled = false, .lcd_enabled = false,
                                  .web_timeout_s = -1, .lcd_timeout_s = -1 };
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init default nvs partition");
    TEST_CHECK(web_auth_store_set_policy(&policy) == HAL_OK, "setup: policy persisted");
    stub_headers_reset();

    bool may_disclose = compute_may_disclose();
    TEST_CHECK(may_disclose == true, "auth off -- may_disclose true");

    char detail[256];
    readiness_crash_report_detail(true, false, may_disclose, "SYNTH_TEST_CAUSE", "synth_task", detail,
                                  sizeof(detail));
    TEST_CHECK(strstr(detail, "SYNTH_TEST_CAUSE") != NULL, "auth off -- real crash detail disclosed");
}

static void test_gate_auth_on_no_session(void)
{
    TEST_SECTION("may_disclose composition -- web auth ON, no session cookie: redacted");
    web_auth_policy_t policy = { .web_enabled = true, .lcd_enabled = false,
                                  .web_timeout_s = -1, .lcd_timeout_s = -1 };
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init default nvs partition");
    TEST_CHECK(web_auth_store_set_policy(&policy) == HAL_OK, "setup: policy persisted");
    stub_headers_reset();

    bool may_disclose = compute_may_disclose();
    TEST_CHECK(may_disclose == false, "auth on, no session -- may_disclose false");

    char detail[256];
    readiness_crash_report_detail(true, false, may_disclose, "SYNTH_TEST_CAUSE", "synth_task", detail,
                                  sizeof(detail));
    TEST_CHECK(strstr(detail, "unacknowledged") != NULL, "existence still reported with no session");
    TEST_CHECK(strstr(detail, "SYNTH_TEST_CAUSE") == NULL, "no session -- cause redacted");
}

static void test_gate_auth_on_user_session(void)
{
    TEST_SECTION("may_disclose composition -- web auth ON, USER-role session: still redacted");
    web_auth_policy_t policy = { .web_enabled = true, .lcd_enabled = false,
                                  .web_timeout_s = -1, .lcd_timeout_s = -1 };
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init default nvs partition");
    TEST_CHECK(web_auth_store_set_policy(&policy) == HAL_OK, "setup: policy persisted");
    const char *token = readiness_test_make_session(WEB_AUTH_SESSION_ROLE_USER, "user-token-readiness-1");
    stub_headers_reset();
    char cookie[64];
    snprintf(cookie, sizeof(cookie), HTTP_SESSION_COOKIE_NAME "=%s", token);
    stub_header_set("Cookie", cookie);

    bool may_disclose = compute_may_disclose();
    TEST_CHECK(may_disclose == false, "USER role -- may_disclose false");

    char detail[256];
    readiness_crash_report_detail(true, false, may_disclose, "SYNTH_TEST_CAUSE", "synth_task", detail,
                                  sizeof(detail));
    TEST_CHECK(strstr(detail, "SYNTH_TEST_CAUSE") == NULL, "USER role -- cause redacted");
}

static void test_gate_auth_on_admin_session(void)
{
    TEST_SECTION("may_disclose composition -- web auth ON, ADMIN-role session: disclosed");
    web_auth_policy_t policy = { .web_enabled = true, .lcd_enabled = false,
                                  .web_timeout_s = -1, .lcd_timeout_s = -1 };
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init default nvs partition");
    TEST_CHECK(web_auth_store_set_policy(&policy) == HAL_OK, "setup: policy persisted");
    const char *token = readiness_test_make_session(WEB_AUTH_SESSION_ROLE_ADMIN, "admin-token-readiness-1");
    stub_headers_reset();
    char cookie[64];
    snprintf(cookie, sizeof(cookie), HTTP_SESSION_COOKIE_NAME "=%s", token);
    stub_header_set("Cookie", cookie);

    bool may_disclose = compute_may_disclose();
    TEST_CHECK(may_disclose == true, "ADMIN role -- may_disclose true");

    char detail[256];
    readiness_crash_report_detail(true, false, may_disclose, "SYNTH_TEST_CAUSE", "synth_task", detail,
                                  sizeof(detail));
    TEST_CHECK(strstr(detail, "SYNTH_TEST_CAUSE") != NULL, "ADMIN role -- real cause disclosed");
    TEST_CHECK(strstr(detail, "synth_task") != NULL, "ADMIN role -- real task disclosed");
}

int main(void)
{
    test_detail_no_record();
    test_detail_acknowledged();
    test_detail_unacknowledged_may_disclose();
    test_detail_unacknowledged_redacted();
    test_detail_unacknowledged_redacted_null_fields();
    test_gate_auth_off();
    test_gate_auth_on_no_session();
    test_gate_auth_on_user_session();
    test_gate_auth_on_admin_session();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
