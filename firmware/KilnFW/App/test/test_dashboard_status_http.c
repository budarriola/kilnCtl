// Host tests for GET /api/status's build-identity redaction (2026-09-17
// audit finding 7 follow-up, corrected the same day): route_tier_table.h
// declares this route ROUTE_TIER_OPEN, reachable with no credentials at
// all, and dashboard_status_get_handler() (dashboard_status_http.c) used to
// unconditionally emit the RP2040's exact commit hash/dirty flag/build
// timestamp (safety_build_commit/_dirty/_datetime) and the ESP's own build
// timestamp (fw_build) to any caller -- the same class of disclosure
// 1a41a972 fixed on GET /api/ota/esp/status.
//
// The first version of this fix gated the four fields on
// `is_admin = http_auth_policy_web_enabled() && http_auth_caller_is_admin(req)`,
// which redacted them from EVERYONE whenever web auth is off -- the board's
// default, out-of-the-box state -- since anyone can already resolve as
// "admin" via the bootstrap path in that state, redacting from that same
// caller protected nothing while blinding this project's own tooling
// (flash_firmware()'s post-flash verification) and the web UI. Corrected
// gate: `may_see_build_identity = !http_auth_policy_web_enabled() ||
// http_auth_caller_is_admin(req)` -- redact only when auth is ON and the
// caller is not an authenticated admin. This file proves that: real values
// with auth off (regardless of session), real values with auth on for an
// ADMIN session, and null for the four fields with auth on and no session
// or a USER session. safety_build_known/safety_config_version/
// safety_config_crc/fw_version/fw_version_known stay unconditional in every
// case.
//
// APPROACH: dashboard_status_get_handler() is declared in
// dashboard_http_internal.h and defined in dashboard_status_http.c with no
// other public seam into its file-scope helpers (json_f(),
// dashboard_ct_topology_is_summed(), dashboard_ct_summed_attrib_zone()), so
// this file #includes dashboard_status_http.c directly -- same convention
// test_ota_http.c/test_zones_http.c/test_profiles_http.c already use for a
// `static` handler with no other seam. dashboard_status_http.c pulls in a
// large dependency surface (kiln_io_owner, safety_ceiling_sync, sim_backend,
// nvs_report, lvgl_port, watchdog_cfg, ...) through
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
/* The real touch_dev.h (pure stdint/stdbool math, host-compilable, and this
 * executable links the real touch_dev.c) for touch_cal_support_t. Included
 * here at file scope because the fake lvgl_port_touch_cal_support() below is
 * defined BEFORE the #include of dashboard_status_http.c that would
 * otherwise pull the type in via the exe9 lvgl_port.h shim. */
#include "touch_dev.h"

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
bool lvgl_port_touch_is_calibrated(void) { return false; }
/* Test-settable, because the whole point of touch_cal_supported is that its
 * three values render differently on the web Diagnostics page -- a fixed
 * fake would only ever exercise one of them. The VALUE is faked here (this
 * executable cannot run lvgl_port.c); the mapping from value to wire string
 * is the real touch_cal_support_name(), linked in for real. */
static touch_cal_support_t s_fake_touch_cal_support = TOUCH_CAL_SUPPORT_SUPPORTED;
touch_cal_support_t lvgl_port_touch_cal_support(void) { return s_fake_touch_cal_support; }
const nvs_report_section_t *nvs_report_get(size_t *out_count) { *out_count = 0; return NULL; }
bool safety_ceiling_sync_is_standing_diverged(char *reason_out, size_t reason_cap)
{
    (void)reason_out;
    (void)reason_cap;
    return false;
}
bool watchdog_cfg_panic_disabled(void) { return false; }
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

static void test_status_web_auth_off_shows_build_identity(void)
{
    TEST_SECTION("dashboard_status_get_handler -- web auth OFF (default/never-configured board): "
                 "fw_build/safety_build_commit/_datetime/_dirty show their REAL values -- with auth "
                 "off anyone can already resolve as admin via the bootstrap path, so redacting from "
                 "that same caller protects nothing (corrected 2026-09-17, was wrongly redacted by "
                 "the first version of the finding-7 fix)");
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
    TEST_CHECK(strstr(s_last_resp_body, "\"fw_build\":\"Sep 17 2026 00:00:00\"") != NULL,
              "auth off (default board) -- real fw_build value present");
    TEST_CHECK(strstr(s_last_resp_body, "\"safety_build_commit\":\"deadbee5\"") != NULL,
              "auth off -- real safety_build_commit value present");
    TEST_CHECK(strstr(s_last_resp_body, "\"safety_build_datetime\":\"2026-09-17 00:00:00\"") != NULL,
              "auth off -- real safety_build_datetime value present");
    TEST_CHECK(strstr(s_last_resp_body, "\"safety_build_dirty\":true") != NULL,
              "auth off -- real safety_build_dirty value present");
    TEST_CHECK(strstr(s_last_resp_body, "\"safety_build_known\":true") != NULL,
              "safety_build_known stays unconditional");
    TEST_CHECK(strstr(s_last_resp_body, "\"safety_config_version\":3") != NULL,
              "safety_config_version stays unconditional");
    TEST_CHECK(strstr(s_last_resp_body, "\"safety_config_crc\":43981") != NULL,
              "safety_config_crc stays unconditional (0xABCD == 43981)");
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
    TEST_CHECK(strstr(s_last_resp_body, "\"safety_config_crc\":43981") != NULL,
              "safety_config_crc stays unconditional (0xABCD == 43981)");
}

/* The WEB half of "do not offer calibration where it is unsupported". The
 * web UI has no calibration ACTION to hide -- /api/status is read-only and
 * no route can start a calibration (the only invokers are the LCD's own
 * config nav cell and kiln_ui.c's boot gate) -- so what it must not do is
 * TELL the operator to run one on a panel that does not support it, which
 * is exactly what diagnostics_page.html did while touch_calibrated:false
 * was the only signal it had.
 *
 * ROUTE_TIER_OPEN: this field is unauthenticated on purpose, like the
 * touch_calibrated flag beside it -- it is a hardware capability, carrying
 * no build identity or credential material (2026-09-17 disclosure audit). */
static void test_status_touch_cal_supported_reports_each_state(void)
{
    TEST_SECTION("dashboard_status_get_handler -- touch_cal_supported reports the panel's real "
                 "calibration capability, so the web UI cannot demand a calibration run the "
                 "device does not offer");
    web_auth_policy_t policy = { .web_enabled = false, .lcd_enabled = false,
                                  .web_timeout_s = -1, .lcd_timeout_s = -1 };
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init default nvs partition");
    TEST_CHECK(web_auth_store_set_policy(&policy) == HAL_OK, "setup: policy persisted");

    httpd_req_t req;

    /* Resistive panel: calibration IS supported, and touch_calibrated stays
     * the actionable signal (the fake above returns false). */
    reset_fake_status_with_known_build_identity();
    stub_headers_reset();
    s_fake_touch_cal_support = TOUCH_CAL_SUPPORT_SUPPORTED;
    memset(&req, 0, sizeof(req));
    TEST_CHECK(dashboard_status_get_handler(&req) == ESP_OK, "handler returns ESP_OK (supported)");
    TEST_CHECK(strstr(s_last_resp_body, "\"touch_cal_supported\":\"supported\"") != NULL,
              "supported panel -- touch_cal_supported:\"supported\"");
    TEST_CHECK(strstr(s_last_resp_body, "\"touch_calibrated\":false") != NULL,
              "supported panel -- touch_calibrated still reported independently (a supported "
              "panel that has never been calibrated is the normal first-boot state)");

    /* The bench unit's own FT6336U: no calibration is offered anywhere, and
     * the web page must say "not required", not "NOT CALIBRATED". */
    reset_fake_status_with_known_build_identity();
    stub_headers_reset();
    s_fake_touch_cal_support = TOUCH_CAL_SUPPORT_SELF_CALIBRATING;
    memset(&req, 0, sizeof(req));
    TEST_CHECK(dashboard_status_get_handler(&req) == ESP_OK,
              "handler returns ESP_OK (self-calibrating)");
    TEST_CHECK(strstr(s_last_resp_body, "\"touch_cal_supported\":\"self_calibrating\"") != NULL,
              "self-calibrating panel -- touch_cal_supported:\"self_calibrating\" (the motivating "
              "case: this bench unit's capacitive panel)");

    /* Bring-up failed / no touch hardware. Must be DISTINGUISHABLE on the
     * wire from the self-calibrating case above -- an undetected controller
     * is an unknown and must never be published as "none needed". */
    reset_fake_status_with_known_build_identity();
    stub_headers_reset();
    s_fake_touch_cal_support = TOUCH_CAL_SUPPORT_NO_TOUCH;
    memset(&req, 0, sizeof(req));
    TEST_CHECK(dashboard_status_get_handler(&req) == ESP_OK, "handler returns ESP_OK (no touch)");
    TEST_CHECK(strstr(s_last_resp_body, "\"touch_cal_supported\":\"no_touch\"") != NULL,
              "no touch controller -- touch_cal_supported:\"no_touch\", NOT \"self_calibrating\"");
    TEST_CHECK(strstr(s_last_resp_body, "\"self_calibrating\"") == NULL,
              "no touch controller -- the self-calibrating spelling appears nowhere in the "
              "payload, so a bring-up failure cannot be read as 'no calibration needed'");

    /* Leave the module-level fake where every other test in this file found
     * it -- these tests run in sequence in one process. */
    s_fake_touch_cal_support = TOUCH_CAL_SUPPORT_SUPPORTED;
}

/* cfg_fs ask-first format refusal on /api/status: the pair of fields is
 * emitted ONLY while pending (the buffer's headroom is ~200 B), the reason
 * is passed through when it is JSON-safe, and an unsafe reason degrades to a
 * fixed string rather than corrupting the document. */
static void test_status_cfg_fs_format_pending_field(void)
{
    TEST_SECTION("dashboard_status_get_handler -- cfg_fs_format_pending/_reason appear only while "
                 "the cfg partition's ask-first format refusal is pending");
    web_auth_policy_t policy = { .web_enabled = false, .lcd_enabled = false,
                                  .web_timeout_s = -1, .lcd_timeout_s = -1 };
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init default nvs partition");
    TEST_CHECK(web_auth_store_set_policy(&policy) == HAL_OK, "setup: policy persisted");
    httpd_req_t req;

    reset_fake_status_with_known_build_identity();
    stub_headers_reset();
    memset(&req, 0, sizeof(req));
    TEST_CHECK(dashboard_status_get_handler(&req) == ESP_OK, "handler returns ESP_OK (not pending)");
    TEST_CHECK(strstr(s_last_resp_body, "cfg_fs_format") == NULL,
              "not pending -- neither cfg_fs_format_ field is emitted");

    reset_fake_status_with_known_build_identity();
    stub_headers_reset();
    s_fake_status.cfg_fs_format_pending = true;
    s_fake_status.cfg_fs_format_reason = "LittleFS superblock found (corrupt)";
    memset(&req, 0, sizeof(req));
    TEST_CHECK(dashboard_status_get_handler(&req) == ESP_OK, "handler returns ESP_OK (pending)");
    TEST_CHECK(strstr(s_last_resp_body, "\"cfg_fs_format_pending\":true") != NULL,
              "pending -- cfg_fs_format_pending:true present");
    TEST_CHECK(strstr(s_last_resp_body, "\"cfg_fs_format_reason\":\"LittleFS superblock found (corrupt)\"") != NULL,
              "pending -- reason string passed through");
    TEST_CHECK(s_last_resp_body[strlen(s_last_resp_body) - 1] == '}', "pending -- document still closes");

    reset_fake_status_with_known_build_identity();
    stub_headers_reset();
    s_fake_status.cfg_fs_format_pending = true;
    s_fake_status.cfg_fs_format_reason = "bad \"quote\"";
    memset(&req, 0, sizeof(req));
    TEST_CHECK(dashboard_status_get_handler(&req) == ESP_OK, "handler returns ESP_OK (unsafe reason)");
    TEST_CHECK(strstr(s_last_resp_body, "\"cfg_fs_format_reason\":\"reason unavailable\"") != NULL,
              "unsafe reason degrades to a fixed string, JSON stays valid");
    TEST_CHECK(strstr(s_last_resp_body, "bad") == NULL, "unsafe reason text is not echoed");
}

static void run_test_dashboard_status_http(void)
{
    test_status_cfg_fs_format_pending_field();
    test_status_touch_cal_supported_reports_each_state();
    test_status_web_auth_off_shows_build_identity();
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
