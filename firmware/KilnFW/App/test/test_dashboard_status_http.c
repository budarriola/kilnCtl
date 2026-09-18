// Host tests for GET /api/status's build-identity redaction (2026-09-17
// audit finding 7 follow-up): route_tier_table.h declares this route
// ROUTE_TIER_OPEN, reachable with no credentials at all, and
// dashboard_status_get_handler() (dashboard_status_http.c) used to
// unconditionally emit the RP2040's exact commit hash/dirty flag/build
// timestamp (safety_build_commit/_dirty/_datetime) and the ESP's own build
// timestamp (fw_build) to any caller -- the same class of disclosure
// 1a41a972 fixed on GET /api/ota/esp/status. This file proves the identical
// gate (`is_admin = http_auth_policy_web_enabled() &&
// http_auth_caller_is_admin(req)`, copied from ota_http_esp.c) now redacts
// those four fields to JSON null for a non-admin caller and reports the
// real values only to an authenticated administrator, while
// safety_build_known/safety_config_version/safety_config_crc/fw_version/
// fw_version_known stay unconditional.
//
// APPROACH: dashboard_status_get_handler() is declared in
// dashboard_http_internal.h and defined in dashboard_status_http.c with no
// other public seam into its file-scope helpers (json_f(),
// dashboard_ct_topology_is_summed(), dashboard_ct_summed_attrib_zone()), so
// this file #includes dashboard_status_http.c directly -- same convention
// test_ota_http.c/test_zones_http.c/test_profiles_http.c already use for a
// `static` handler with no other seam. dashboard_status_http.c pulls in a
// large dependency surface (kiln_io_owner, safety_ceiling_sync, sim_backend,
// nvs_report, boot_button, lvgl_port, watchdog_cfg, ...) through
// dashboard_http_internal.h/dashboard_http.h; every function it actually
// CALLS (not just names in a comment -- checked line by line) beyond
// dashboard_get_status() itself is a thin, independent accessor with no
// state this file's tests need to control, so each gets a trivial fixed
// fake below rather than linking its real, much heavier .c file.
// dashboard_get_status() itself is faked with a file-scope
// dashboard_status_t the test controls directly -- this is the ONE part of
// the snapshot these tests actually vary (safety_build_*/fw_build) plus the
// minimum other fields needed to keep the handler's other branches
// (io_ready, thermo_ready, diag_ever_received, trip_event_ever_received,
// etc.) on their "nothing to report yet" path, which they already are once
// the struct is zeroed -- dashboard_status_t is a plain-old-data struct (no
// pointers; confirmed by reading dashboard_http.h's definition end to end),
// so memset(&s_fake_status, 0, sizeof(s_fake_status)) is a safe, complete
// reset between tests.
//
// The real session/auth seam (http_auth_caller_is_admin(), the real session
// table, web_auth_store_set_policy()) is linked in for REAL, same as
// test_ota_http.c's own GET /api/ota/esp/status redaction tests -- this
// exercises the actual production gate, not a transcribed copy of it.
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

#include "http_auth_http.h"
#include "web_auth_session.h"
#include "http_session_iface.h"
#include "web_auth_store.h"
#include "hal_kv.h"
#include "fake_kv.h"

// Same forward-declare-at-point-of-use precedent test_ota_http.c uses:
// http_session_table() has external linkage (http_session_iface.c) but is
// deliberately not declared in http_session_iface.h.
web_auth_table_t *http_session_table(void);

#include "dashboard_http.h"
#include "nvs_report.h"
#include "unit_pref.h"
#include "safety_cfg_store.h"

// ---------------------------------------------------------------------------
// dashboard_get_status() fake -- the ONE seam these tests drive directly.
// Declared as a real (non-static) function in dashboard_http.h and defined
// in dashboard_http.c, which this file does NOT link (it #includes lvgl_port.h
// at file scope, which drags in a GCC-only __attribute__ MSVC's host
// toolchain rejects -- dashboard_http_internal.h's own header comment).
// Providing this definition here is exactly the same "declared elsewhere,
// faked in the test file" seam test_ota_http.c uses for heat_interlock_check().
// ---------------------------------------------------------------------------
static dashboard_status_t s_fake_status;
void dashboard_get_status(dashboard_status_t *out)
{
    memcpy(out, &s_fake_status, sizeof(*out));
}

// ---- trivial fakes for every OTHER real function dashboard_status_get_
// handler() actually calls (verified line by line against the handler body,
// not against comment text naming these functions) -----------------------
uint32_t boot_button_bypass_remaining_ms(void) { return 0; }
bool boot_button_ota_bypass_active(void) { return false; }
bool lvgl_port_touch_is_calibrated(void) { return false; }
const nvs_report_section_t *nvs_report_get(size_t *out_count) { *out_count = 0; return NULL; }
bool safety_ceiling_sync_is_standing_diverged(char *reason_out, size_t reason_cap)
{
    (void)reason_out;
    (void)reason_cap;
    return false;
}
bool watchdog_cfg_panic_disabled(void) { return false; }
bool ota_http_auth_disabled(void) { return false; }
const char *unit_pref_suffix(unit_pref_t pref) { (void)pref; return "C"; }
size_t safety_cfg_store_param_count(void) { return 0; }
bool safety_cfg_store_get_by_index(size_t index, safety_cfg_param_t *out)
{
    (void)index;
    (void)out;
    return false;
}
bool zones_config_get_relay_mask(uint8_t zone_index, uint8_t *out_mask)
{
    (void)zone_index;
    *out_mask = 0;
    return false;
}
/* Must match the client_ip literal passed to web_auth_table_create_session()
 * in status_test_make_session() below -- ca7a7d31 bound session cookies to
 * the caller's IP, so a mismatch here makes http_auth_caller_is_admin()
 * silently fail to resolve even a real admin session's role. */
void ota_http_get_client_ip(httpd_req_t *req, char *out, size_t out_len)
{
    (void)req;
    if (out_len > 0) {
        strncpy(out, "10.0.0.9", out_len - 1);
        out[out_len - 1] = '\0';
    }
}

// ---------------------------------------------------------------------------
// esp_http_server.h stub bodies -- same shape as test_ota_http.c's own
// (a tiny test-controllable header field->value map plus fixed no-ops),
// duplicated here rather than shared because this is a separate executable
// with its own translation unit.
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
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value)
{
    (void)r; (void)field; (void)value;
    return ESP_OK;
}
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t error, const char *msg)
{
    (void)r; (void)error; (void)msg;
    return ESP_OK;
}
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler)
{
    (void)handle; (void)uri_handler;
    return ESP_OK;
}
int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len) { (void)r; (void)buf; (void)buf_len; return 0; }
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, size_t buf_len)
{
    (void)r; (void)buf; (void)buf_len;
    return ESP_OK;
}

esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t buf_len)
{
    (void)r;
    (void)buf_len;
    buf[0] = '\0'; // no ?diag=1 in these tests -- want_diag_detail stays false
    return ESP_OK;
}
esp_err_t httpd_query_key_value(const char *qs, const char *key, char *val, size_t val_size)
{
    (void)qs; (void)key; (void)val; (void)val_size;
    return ESP_FAIL;
}

// httpd_resp_set_type/httpd_resp_send/httpd_resp_sendstr/httpd_resp_set_status
// capture the body into s_last_resp_body, same convention as
// test_zones_http.c/test_ota_http.c, so a test can strstr() the JSON.
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type) { (void)r; (void)type; return ESP_OK; }
static char s_last_resp_body[8192];
static size_t s_last_resp_len;
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, long long buf_len)
{
    (void)r;
    size_t n = (buf_len == HTTPD_RESP_USE_STRLEN) ? strlen(buf) : (size_t)buf_len;
    if (n >= sizeof(s_last_resp_body)) {
        n = sizeof(s_last_resp_body) - 1;
    }
    if (n > 0) {
        memcpy(s_last_resp_body, buf, n);
    }
    s_last_resp_body[n] = '\0';
    s_last_resp_len = n;
    return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s)
{
    return httpd_resp_send(r, s, HTTPD_RESP_USE_STRLEN);
}
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status) { (void)r; (void)status; return ESP_OK; }

// dashboard_http_internal.h's shared log tag -- referenced by ESP_LOGE()
// calls in dashboard_status_http.c. s_dash itself is declared extern but
// never dereferenced by this file (grepped -- dashboard_status_http.c never
// touches s_dash.*), so it needs no definition here.
const char *DASH_TAG = "test_dashboard_status_http";

// See this file's header comment: dashboard_status_http.c is #included
// directly for the same seam reasons test_ota_http.c #includes ota_http.c.
#include "../drivers/http/dashboard_status_http.c"

// ---------------------------------------------------------------------------
// Session helper -- mints a real ADMIN/USER session in the real session
// table, same as test_ota_http.c's ota_status_test_make_session().
// ---------------------------------------------------------------------------
static const char *status_test_make_session(web_auth_session_role_t role, const char *token)
{
    uint8_t hash[32];
    size_t hash_len = 0;
    psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)token, strlen(token), hash, sizeof(hash), &hash_len);
    web_auth_table_create_session(http_session_table(), hash, "10.0.0.9", role, (uint32_t)0);
    return token;
}

static void reset_fake_status_with_known_build_identity(void)
{
    memset(&s_fake_status, 0, sizeof(s_fake_status));
    s_fake_status.safety_build_known = true;
    s_fake_status.safety_build_dirty = true;
    strncpy(s_fake_status.safety_build_commit, "deadbee5", sizeof(s_fake_status.safety_build_commit) - 1);
    strncpy(s_fake_status.safety_build_datetime, "2026-09-17 00:00:00",
            sizeof(s_fake_status.safety_build_datetime) - 1);
    s_fake_status.safety_config_version = 3;
    s_fake_status.safety_config_crc = 0xABCD;
    s_fake_status.fw_version_known = true;
    strncpy(s_fake_status.fw_version, "1.2.3", sizeof(s_fake_status.fw_version) - 1);
    strncpy(s_fake_status.fw_build, "Sep 17 2026 00:00:00", sizeof(s_fake_status.fw_build) - 1);
}

static void test_status_web_auth_off_redacts_build_identity(void)
{
    TEST_SECTION("dashboard_status_get_handler -- web auth OFF (default/never-configured board): "
                 "fw_build/safety_build_commit/_datetime/_dirty redacted to null, not trusted to "
                 "caller_is_admin() alone (2026-09-17 finding 7 follow-up, same gate as 1a41a972)");
    web_auth_policy_t policy = { .web_enabled = false, .lcd_enabled = false,
                                  .web_timeout_s = -1, .lcd_timeout_s = -1 };
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init default nvs partition");
    TEST_CHECK(web_auth_store_set_policy(&policy) == HAL_OK, "setup: policy persisted");
    reset_fake_status_with_known_build_identity();
    stub_headers_reset();

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = dashboard_status_get_handler(&req);

    TEST_CHECK(err == ESP_OK, "handler always returns ESP_OK");
    TEST_CHECK(strstr(s_last_resp_body, "\"fw_build\":null") != NULL,
              "auth off (default board) -- fw_build redacted, not trusted to caller_is_admin() alone");
    TEST_CHECK(strstr(s_last_resp_body, "\"safety_build_commit\":null") != NULL,
              "auth off -- safety_build_commit redacted");
    TEST_CHECK(strstr(s_last_resp_body, "\"safety_build_datetime\":null") != NULL,
              "auth off -- safety_build_datetime redacted");
    TEST_CHECK(strstr(s_last_resp_body, "\"safety_build_dirty\":null") != NULL,
              "auth off -- safety_build_dirty redacted");
    TEST_CHECK(strstr(s_last_resp_body, "\"safety_build_known\":true") != NULL,
              "safety_build_known stays unconditional");
    TEST_CHECK(strstr(s_last_resp_body, "\"safety_config_version\":3") != NULL,
              "safety_config_version stays unconditional");
    TEST_CHECK(strstr(s_last_resp_body, "\"fw_version_known\":true") != NULL,
              "fw_version_known stays unconditional");
    TEST_CHECK(strstr(s_last_resp_body, "\"fw_version\":\"1.2.3\"") != NULL,
              "fw_version stays unconditional and unredacted");
}

static void test_status_unauthenticated_redacts_build_identity(void)
{
    TEST_SECTION("dashboard_status_get_handler -- web auth ON, no cookie: the four build-identity "
                 "fields redacted to null");
    web_auth_policy_t policy = { .web_enabled = true, .lcd_enabled = false,
                                  .web_timeout_s = -1, .lcd_timeout_s = -1 };
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init default nvs partition");
    TEST_CHECK(web_auth_store_set_policy(&policy) == HAL_OK, "setup: policy persisted");
    reset_fake_status_with_known_build_identity();
    stub_headers_reset();

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = dashboard_status_get_handler(&req);

    TEST_CHECK(err == ESP_OK, "handler always returns ESP_OK");
    TEST_CHECK(strstr(s_last_resp_body, "\"fw_build\":null") != NULL, "no session -- fw_build redacted");
    TEST_CHECK(strstr(s_last_resp_body, "\"safety_build_commit\":null") != NULL,
              "no session -- safety_build_commit redacted");
    TEST_CHECK(strstr(s_last_resp_body, "\"safety_build_datetime\":null") != NULL,
              "no session -- safety_build_datetime redacted");
    TEST_CHECK(strstr(s_last_resp_body, "\"safety_build_dirty\":null") != NULL,
              "no session -- safety_build_dirty redacted");
}

static void test_status_user_session_redacts_build_identity(void)
{
    TEST_SECTION("dashboard_status_get_handler -- web auth ON, USER-role session: still redacted");
    web_auth_policy_t policy = { .web_enabled = true, .lcd_enabled = false,
                                  .web_timeout_s = -1, .lcd_timeout_s = -1 };
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init default nvs partition");
    TEST_CHECK(web_auth_store_set_policy(&policy) == HAL_OK, "setup: policy persisted");
    reset_fake_status_with_known_build_identity();
    const char *token = status_test_make_session(WEB_AUTH_SESSION_ROLE_USER, "user-token-status-1");
    stub_headers_reset();
    char cookie[64];
    snprintf(cookie, sizeof(cookie), HTTP_SESSION_COOKIE_NAME "=%s", token);
    stub_header_set("Cookie", cookie);

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = dashboard_status_get_handler(&req);

    TEST_CHECK(err == ESP_OK, "handler always returns ESP_OK");
    TEST_CHECK(strstr(s_last_resp_body, "\"fw_build\":null") != NULL, "USER role -- fw_build still redacted");
    TEST_CHECK(strstr(s_last_resp_body, "\"safety_build_commit\":null") != NULL,
              "USER role -- safety_build_commit still redacted");
    TEST_CHECK(strstr(s_last_resp_body, "\"safety_build_datetime\":null") != NULL,
              "USER role -- safety_build_datetime still redacted");
    TEST_CHECK(strstr(s_last_resp_body, "\"safety_build_dirty\":null") != NULL,
              "USER role -- safety_build_dirty still redacted");
}

static void test_status_admin_session_gets_full_payload(void)
{
    TEST_SECTION("dashboard_status_get_handler -- web auth ON, ADMIN-role session: real values");
    web_auth_policy_t policy = { .web_enabled = true, .lcd_enabled = false,
                                  .web_timeout_s = -1, .lcd_timeout_s = -1 };
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init default nvs partition");
    TEST_CHECK(web_auth_store_set_policy(&policy) == HAL_OK, "setup: policy persisted");
    reset_fake_status_with_known_build_identity();
    const char *token = status_test_make_session(WEB_AUTH_SESSION_ROLE_ADMIN, "admin-token-status-1");
    stub_headers_reset();
    char cookie[64];
    snprintf(cookie, sizeof(cookie), HTTP_SESSION_COOKIE_NAME "=%s", token);
    stub_header_set("Cookie", cookie);

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = dashboard_status_get_handler(&req);

    TEST_CHECK(err == ESP_OK, "handler always returns ESP_OK");
    TEST_CHECK(strstr(s_last_resp_body, "\"fw_build\":\"Sep 17 2026 00:00:00\"") != NULL,
              "ADMIN role -- real fw_build value present");
    TEST_CHECK(strstr(s_last_resp_body, "\"safety_build_commit\":\"deadbee5\"") != NULL,
              "ADMIN role -- real safety_build_commit value present");
    TEST_CHECK(strstr(s_last_resp_body, "\"safety_build_datetime\":\"2026-09-17 00:00:00\"") != NULL,
              "ADMIN role -- real safety_build_datetime value present");
    TEST_CHECK(strstr(s_last_resp_body, "\"safety_build_dirty\":true") != NULL,
              "ADMIN role -- real safety_build_dirty value present");
}

static void run_test_dashboard_status_http(void)
{
    test_status_web_auth_off_redacts_build_identity();
    test_status_unauthenticated_redacts_build_identity();
    test_status_user_session_redacts_build_identity();
    test_status_admin_session_gets_full_payload();
}

int main(void)
{
    run_test_dashboard_status_http();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
