// Host tests for GET /status's SSID/static-IP-topology disclosure redaction
// (2026-09-17 ROUTE_TIER_OPEN disclosure audit, finding 1):
// status_get_handler() (wifi_provision_http.c) used to unconditionally emit
// the saved home network's SSID and, when the STA interface is configured
// static, its static_ip/static_netmask/static_gateway to any caller -- no
// auth check at all. The route stays ROUTE_TIER_OPEN (an unprovisioned board
// must answer it before anyone can authenticate at all) -- the fix redacts
// only the payload, via the same
// `may_disclose = !http_auth_policy_web_enabled() || http_auth_caller_is_admin(req)`
// disjunction as this commit's sibling readiness_http.c fix. The pre-existing
// on_ap/ap_password narrowing (2026-08-22) is a separate, independent
// condition and is untouched by this fix. Its decision is the header-inline
// wifi_prov_status_ap_password_view(), pinned by test_ap_password_view()
// below; test_wifi_prov.c covers wifi_prov_request_arrived_on_ap() (what
// computes on_ap), NOT the handler's use of it. That the handler calls the
// helper is not host-testable (wifi_provision_http.c does not host-compile)
// and is checked by review only.
//
// sta_ip is deliberately excluded from this redaction (see
// wifi_prov_status_redact_field()'s doc comment in wifi_prov.h for the full
// reasoning: a caller able to reach this route at all already used an
// address to get here, so withholding the board's own current LAN address
// narrows nothing an unauthenticated LAN caller doesn't already have) -- so
// there is nothing to test for it here; it stays a plain unconditional
// field, unchanged by this commit.
//
// APPROACH: wifi_provision_http.c cannot be host-compiled on this MSVC-based
// toolchain at all -- confirmed by inspection: it declares twelve
// GCC-only `asm("_binary_...")` embedded-blob externs (`cl` cannot parse
// that syntax) and #includes <sys/socket.h>, for which no host stub exists
// anywhere in this repo. Compiling status_get_handler() itself is not
// feasible without a much larger, out-of-scope stub-and-toolchain effort.
//
// This file instead tests the two things that actually make up the fix, for
// real, same split as this commit's sibling test_readiness_crash_disclosure.c:
//   1. wifi_prov_status_redact_field() (wifi_prov.h) -- the pure formatter
//      status_get_handler() now calls for ssid/static_ip/static_netmask/
//      static_gateway, exercised directly. Pure by design specifically so it
//      can be host-tested without the handler around it.
//   2. The real `may_disclose` composition, using the actual production
//      http_auth_policy_web_enabled()/http_auth_caller_is_admin() stack,
//      across all three auth states, fed into the real formatter exactly as
//      status_get_handler() does.
//
// ABSOLUTE CONSTRAINT: no real Wi-Fi SSID or password appears anywhere in
// this file. Every SSID/IP value below is an obviously synthetic
// placeholder or an RFC 5737/1918 documentation address.
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

web_auth_table_t *http_session_table(void);

#include "wifi_prov.h"

// ---------------------------------------------------------------------------
// esp_http_server.h stub bodies -- same minimal set as
// test_readiness_crash_disclosure.c: only what the real auth stack touches.
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
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value) { (void)r; (void)field; (void)value; return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type) { (void)r; (void)type; return ESP_OK; }

void ota_http_get_client_ip(httpd_req_t *req, char *out, size_t out_len)
{
    (void)req;
    if (out_len > 0) {
        strncpy(out, "10.0.0.9", out_len - 1);
        out[out_len - 1] = '\0';
    }
}

static const char *wifi_prov_test_make_session(web_auth_session_role_t role, const char *token)
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
     * which is exactly why it could not detect wifi_provision_http.c's
     * call site being replaced with `bool may_disclose = true;`: the test
     * and the (uncompilable) production call site were two independent
     * copies of the same expression. This is now the one production
     * function both status_get_handler() and this test call. */
    return http_auth_may_disclose(&req);
}

// ---------------------------------------------------------------------------
// 1. wifi_prov_status_redact_field() exercised directly.
// ---------------------------------------------------------------------------
static void test_redact_field_disclosed(void)
{
    TEST_SECTION("wifi_prov_status_redact_field -- may_disclose true: quoted value");
    char out[64];
    wifi_prov_status_redact_field(true, "SYNTH-HOME-NET-1", out, sizeof(out));
    TEST_CHECK(strcmp(out, "\"SYNTH-HOME-NET-1\"") == 0, "value quoted verbatim");
}

static void test_redact_field_redacted(void)
{
    TEST_SECTION("wifi_prov_status_redact_field -- may_disclose false: JSON null, key never omitted "
                 "by caller (this function only ever produces the value half of the pair)");
    char out[64];
    wifi_prov_status_redact_field(false, "SYNTH-HOME-NET-1", out, sizeof(out));
    TEST_CHECK(strcmp(out, "null") == 0, "redacted to JSON null, not omitted or empty string");
}

static void test_redact_field_ip_values(void)
{
    TEST_SECTION("wifi_prov_status_redact_field -- static-IP-shaped values (RFC 5737 documentation "
                 "addresses, not a real network)");
    char out[32];
    wifi_prov_status_redact_field(true, "203.0.113.5", out, sizeof(out));
    TEST_CHECK(strcmp(out, "\"203.0.113.5\"") == 0, "disclosed IP quoted verbatim");
    wifi_prov_status_redact_field(false, "203.0.113.5", out, sizeof(out));
    TEST_CHECK(strcmp(out, "null") == 0, "redacted IP -- null, not the real address");
}

static void test_redact_field_null_value(void)
{
    TEST_SECTION("wifi_prov_status_redact_field -- NULL value with may_disclose true doesn't crash "
                 "(defensive; real callers always pass a valid C string)");
    char out[16];
    wifi_prov_status_redact_field(true, NULL, out, sizeof(out));
    TEST_CHECK(strcmp(out, "\"\"") == 0, "NULL treated as empty string, not a crash");
}

// ---------------------------------------------------------------------------
// 2. may_disclose composed from the REAL auth stack, all three states.
// ---------------------------------------------------------------------------
static void test_gate_auth_off(void)
{
    TEST_SECTION("may_disclose composition -- web auth OFF (default/never-configured board): SSID/"
                 "topology disclosed (an operator provisioning a fresh board needs to see what it's "
                 "already joined, same reasoning as the ap_password_known flow)");
    web_auth_policy_t policy = { .web_enabled = false, .lcd_enabled = false,
                                  .web_timeout_s = -1, .lcd_timeout_s = -1 };
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init default nvs partition");
    TEST_CHECK(web_auth_store_set_policy(&policy) == HAL_OK, "setup: policy persisted");
    stub_headers_reset();

    bool may_disclose = compute_may_disclose();
    TEST_CHECK(may_disclose == true, "auth off -- may_disclose true");

    char ssid_field[40];
    wifi_prov_status_redact_field(may_disclose, "SYNTH-HOME-NET-1", ssid_field, sizeof(ssid_field));
    TEST_CHECK(strcmp(ssid_field, "\"SYNTH-HOME-NET-1\"") == 0, "auth off -- SSID disclosed");
}

static void test_gate_auth_on_no_session(void)
{
    TEST_SECTION("may_disclose composition -- web auth ON, no session cookie: SSID/topology redacted");
    web_auth_policy_t policy = { .web_enabled = true, .lcd_enabled = false,
                                  .web_timeout_s = -1, .lcd_timeout_s = -1 };
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init default nvs partition");
    TEST_CHECK(web_auth_store_set_policy(&policy) == HAL_OK, "setup: policy persisted");
    stub_headers_reset();

    bool may_disclose = compute_may_disclose();
    TEST_CHECK(may_disclose == false, "auth on, no session -- may_disclose false");

    char ssid_field[40];
    char static_ip_field[24];
    wifi_prov_status_redact_field(may_disclose, "SYNTH-HOME-NET-1", ssid_field, sizeof(ssid_field));
    wifi_prov_status_redact_field(may_disclose, "203.0.113.5", static_ip_field, sizeof(static_ip_field));
    TEST_CHECK(strcmp(ssid_field, "null") == 0, "no session -- SSID redacted");
    TEST_CHECK(strcmp(static_ip_field, "null") == 0, "no session -- static IP redacted");
}

static void test_gate_auth_on_user_session(void)
{
    TEST_SECTION("may_disclose composition -- web auth ON, USER-role session: still redacted");
    web_auth_policy_t policy = { .web_enabled = true, .lcd_enabled = false,
                                  .web_timeout_s = -1, .lcd_timeout_s = -1 };
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init default nvs partition");
    TEST_CHECK(web_auth_store_set_policy(&policy) == HAL_OK, "setup: policy persisted");
    const char *token = wifi_prov_test_make_session(WEB_AUTH_SESSION_ROLE_USER, "user-token-wifiprov-1");
    stub_headers_reset();
    char cookie[64];
    snprintf(cookie, sizeof(cookie), HTTP_SESSION_COOKIE_NAME "=%s", token);
    stub_header_set("Cookie", cookie);

    bool may_disclose = compute_may_disclose();
    TEST_CHECK(may_disclose == false, "USER role -- may_disclose false");

    char ssid_field[40];
    wifi_prov_status_redact_field(may_disclose, "SYNTH-HOME-NET-1", ssid_field, sizeof(ssid_field));
    TEST_CHECK(strcmp(ssid_field, "null") == 0, "USER role -- SSID redacted");
}

static void test_gate_auth_on_admin_session(void)
{
    TEST_SECTION("may_disclose composition -- web auth ON, ADMIN-role session: disclosed");
    web_auth_policy_t policy = { .web_enabled = true, .lcd_enabled = false,
                                  .web_timeout_s = -1, .lcd_timeout_s = -1 };
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init default nvs partition");
    TEST_CHECK(web_auth_store_set_policy(&policy) == HAL_OK, "setup: policy persisted");
    const char *token = wifi_prov_test_make_session(WEB_AUTH_SESSION_ROLE_ADMIN, "admin-token-wifiprov-1");
    stub_headers_reset();
    char cookie[64];
    snprintf(cookie, sizeof(cookie), HTTP_SESSION_COOKIE_NAME "=%s", token);
    stub_header_set("Cookie", cookie);

    bool may_disclose = compute_may_disclose();
    TEST_CHECK(may_disclose == true, "ADMIN role -- may_disclose true");

    char ssid_field[40];
    char static_ip_field[24];
    char static_netmask_field[24];
    char static_gateway_field[24];
    wifi_prov_status_redact_field(may_disclose, "SYNTH-HOME-NET-1", ssid_field, sizeof(ssid_field));
    wifi_prov_status_redact_field(may_disclose, "203.0.113.5", static_ip_field, sizeof(static_ip_field));
    wifi_prov_status_redact_field(may_disclose, "255.255.255.0", static_netmask_field,
                                   sizeof(static_netmask_field));
    wifi_prov_status_redact_field(may_disclose, "203.0.113.1", static_gateway_field,
                                   sizeof(static_gateway_field));
    TEST_CHECK(strcmp(ssid_field, "\"SYNTH-HOME-NET-1\"") == 0, "ADMIN role -- SSID disclosed");
    TEST_CHECK(strcmp(static_ip_field, "\"203.0.113.5\"") == 0, "ADMIN role -- static IP disclosed");
    TEST_CHECK(strcmp(static_netmask_field, "\"255.255.255.0\"") == 0, "ADMIN role -- netmask disclosed");
    TEST_CHECK(strcmp(static_gateway_field, "\"203.0.113.1\"") == 0, "ADMIN role -- gateway disclosed");
}

static void test_ap_password_view(void)
{
    TEST_SECTION("wifi_prov_status_ap_password_view -- AP password only on AP-arrived requests");
    TEST_CHECK(strcmp(wifi_prov_status_ap_password_view(true, "apsecret1"), "apsecret1") == 0, "on_ap -- shown");
    TEST_CHECK(wifi_prov_status_ap_password_view(false, "apsecret1")[0] == '\0', "off AP -- empty");
    TEST_CHECK(wifi_prov_status_ap_password_view(true, NULL)[0] == '\0', "NULL -- empty");
}

/* REVIEW_LD01_WWFIX LOW-5: GET /status with every escaped field at its maximum and the saved-networks record
 * refused is the largest body; it must fit WIFI_PROV_STATUS_JSON_CAP whole (a truncated body is invalid JSON). */
static bool json_shape_ok(const char *j)
{
    size_t len = strlen(j);
    if (len < 2 || j[0] != '{' || j[len - 1] != '}') return false;
    int depth = 0;
    bool in_str = false;
    for (size_t i = 0; i < len; i++) {
        char c = j[i];
        if (in_str) {
            if (c == '\\') { i++; continue; }
            if (c == '"') in_str = false;
        } else if (c == '"') in_str = true;
        else if (c == '{') depth++;
        else if (c == '}') { if (--depth < 0) return false; if (depth == 0 && i != len - 1) return false; }
    }
    return depth == 0 && !in_str;
}

static void test_status_json_worst_case_fits(void)
{
    TEST_SECTION("GET /status JSON: worst-case fields fit the cap and stay valid (LOW-5)");
    char ssid[WIFI_PROV_SSID_MAX_LEN * 2 + 3], apssid[WIFI_PROV_SSID_MAX_LEN * 2 + 1];
    char appw[WIFI_PROV_PASSWORD_MAX_LEN * 2 + 1];
    memset(apssid, 'a', sizeof(apssid) - 1); apssid[sizeof(apssid) - 1] = '\0';
    memset(appw, 'b', sizeof(appw) - 1); appw[sizeof(appw) - 1] = '\0';
    snprintf(ssid, sizeof(ssid), "\"%.64s\"", apssid);
    const char *ip = "\"255.255.255.255\"";
    const char *hint = "The saved Wi-Fi record is unreadable or from newer firmware; factory reset with scope wifi to recover";
    wifi_prov_status_json_in_t in = {
        .mode = "home", .state = "reconnecting", .ssid_field = ssid, .sta_ip_field = ip,
        .ap_ssid_escaped = apssid, .ap_password_escaped = appw, .ip_mode = "static", .static_ip_field = ip,
        .static_netmask_field = ip, .static_gateway_field = ip, .static_dns_field = ip, .static_dns2_field = ip,
        .sta_connected = true, .ap_password_known = true, .ap_password_set = true, .ap_pending_teardown = true,
        .sta_rssi = -128, .ap_clients = 4294967295u, .recovery_hint = hint,
    };
    char buf[WIFI_PROV_STATUS_JSON_CAP];
    int n = wifi_prov_status_json_format(buf, sizeof(buf), &in);
    TEST_CHECK(n > 680, "worst case exceeds the old 680 B cap (this is the regression being pinned)");
    TEST_CHECK(n > 0 && (size_t)n < sizeof(buf), "worst case is not truncated by WIFI_PROV_STATUS_JSON_CAP");
    TEST_CHECK((size_t)n + 64 < sizeof(buf), "at least 64 B of margin remains");
    TEST_CHECK(json_shape_ok(buf), "worst-case body is structurally valid JSON");
    TEST_CHECK(strstr(buf, "\"recovery_hint\":\"The saved") != NULL, "recovery hint present");
    in.recovery_hint = NULL;
    n = wifi_prov_status_json_format(buf, sizeof(buf), &in);
    TEST_CHECK((size_t)n < sizeof(buf) && json_shape_ok(buf) && strstr(buf, "\"saved_nets_refused\":false}") != NULL,
              "unrefused body valid and ends with saved_nets_refused:false");
    /* The cap must be what truncates, not a silent pass: a deliberately small buffer reports truncation. */
    char small[200];
    n = wifi_prov_status_json_format(small, sizeof(small), &in);
    TEST_CHECK((size_t)n >= sizeof(small), "a too-small buffer is reported as truncation by the return value");
}

int main(void)
{
    test_redact_field_disclosed();
    test_redact_field_redacted();
    test_redact_field_ip_values();
    test_redact_field_null_value();
    test_gate_auth_off();
    test_gate_auth_on_no_session();
    test_gate_auth_on_user_session();
    test_gate_auth_on_admin_session();
    test_ap_password_view();
    test_status_json_worst_case_fits();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
