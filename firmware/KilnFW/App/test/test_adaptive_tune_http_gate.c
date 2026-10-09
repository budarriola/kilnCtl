// Host tests for App/drivers/http/adaptive_tune_http.c's enable_post_handler()
// and revert_post_handler() -- POST /api/adaptive_tune/enable and
// POST /api/adaptive_tune/revert. Closes a known test gap named in
// docs/SYSTEM_MODE_GATE_PLAN.md section 3.6 slice 4: these two handlers'
// system_mode_gate wiring (owner decision Q2, 2026-09-25, narrowed the same
// day to only gate enable's enabled=true case) was verified only by
// code-pattern review and an ESP-IDF target build, never a host-test
// assertion.
//
// test_adaptive_tune_http.c already exists in this directory but its own
// header comment deliberately explains why it hand-mirrors only
// status_get_handler()'s render format and never links the rest of the
// translation unit (a wider stub surface than that one function needs) --
// so this is a SEPARATE file, and a separate executable, same "own stub
// surface, own executable" convention as test_kiln_cfg_http.c/
// test_zones_http.c. enable_post_handler()/revert_post_handler() are
// `static` with no other seam, so this file #includes adaptive_tune_http.c
// directly.
//
// Rule under test (docs/SYSTEM_MODE_GATE_PLAN.md section 3.6 slice 4, owner
// decision 2026-09-25 "later same day"):
//   - enable_post_handler() with enabled=true is refused 409 by the system
//     mode gate while a firing or autotune run is active.
//   - enable_post_handler() with enabled=false is ALWAYS allowed, even
//     mid-run -- it can only prevent a future change, never apply one.
//   - revert_post_handler() is refused 409 unconditionally while a run is
//     active, regardless of body content.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

// Ahead of adaptive_tune_http.c's own #includes, purely for the TYPES the
// stub bodies below need (same convention test_zones_http.c/
// test_kiln_cfg_http.c use for esp_err.h/esp_http_server.h).
#include "esp_err.h"
#include "esp_http_server.h"
#include "../drivers/control/adaptive_tune.h"

// ---------------------------------------------------------------------------
// wifi_provision_http.h / http_auth_http.h -- only reached from
// adaptive_tune_http_start(), which these tests never call.
// ---------------------------------------------------------------------------
httpd_handle_t wifi_provision_http_get_server(void)
{
    return NULL;
}
esp_err_t kiln_http_register(httpd_handle_t server, const httpd_uri_t *uri)
{
    (void)server;
    (void)uri;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// adaptive_tune.h -- controllable, call-counted bodies. status_get_handler()
// is never exercised by these tests, so adaptive_tune_get_status() only
// needs to exist and zero its out-param.
// ---------------------------------------------------------------------------
int g_stub_set_enabled_calls = 0;
static bool s_stub_set_enabled_last_zone_valid = false;
static uint8_t s_stub_set_enabled_last_zone = 0;
static bool s_stub_set_enabled_last_enabled = false;
bool adaptive_tune_set_enabled(uint8_t zone_index, bool enabled)
{
    g_stub_set_enabled_calls++;
    s_stub_set_enabled_last_zone_valid = true;
    s_stub_set_enabled_last_zone = zone_index;
    s_stub_set_enabled_last_enabled = enabled;
    return true;
}

int g_stub_revert_calls = 0;
static adaptive_tune_revert_result_t s_stub_revert_result = ADAPTIVE_TUNE_REVERT_OK;
adaptive_tune_revert_result_t adaptive_tune_revert(uint8_t zone_index, char *reason, size_t reason_cap)
{
    (void)zone_index;
    g_stub_revert_calls++;
    if (reason && reason_cap) reason[0] = '\0';
    return s_stub_revert_result;
}

void adaptive_tune_get_status(uint8_t zone_index, adaptive_tune_zone_status_t *out)
{
    (void)zone_index;
    if (out) memset(out, 0, sizeof(*out));
}

// ---------------------------------------------------------------------------
// relay_authority.h -- system_mode_gate snapshot input, same convention as
// test_zones_http.c/test_kiln_cfg_http.c's own copy of this stub.
// ---------------------------------------------------------------------------
static bool s_test_profile_running = false;
static bool s_test_autotune_running = false;
void relay_authority_heat_run_active(bool *profile_running_out, bool *autotune_running_out)
{
    if (profile_running_out) *profile_running_out = s_test_profile_running;
    if (autotune_running_out) *autotune_running_out = s_test_autotune_running;
}

// ---------------------------------------------------------------------------
// esp_http_server.h leaf calls -- header extraction and the plain request
// helpers, same convention as test_kiln_cfg_http.c's own copies.
// ---------------------------------------------------------------------------
static char s_test_post_body[256];
int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len)
{
    (void)r;
    size_t n = strlen(s_test_post_body);
    if (n > buf_len) n = buf_len;
    memcpy(buf, s_test_post_body, n);
    return (int)n;
}

static int s_last_status = 200;
static char s_last_resp_body[512];
static bool s_test_err_called = false;
static bool s_test_ok_called = false;

esp_err_t httpd_resp_send_err(httpd_req_t *req, httpd_err_code_t error, const char *msg)
{
    (void)req; (void)error;
    s_test_err_called = true;
    strncpy(s_last_resp_body, msg ? msg : "", sizeof(s_last_resp_body) - 1);
    s_last_resp_body[sizeof(s_last_resp_body) - 1] = '\0';
    return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status)
{
    (void)req;
    s_last_status = atoi(status);
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type)
{
    (void)req; (void)type;
    return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *s)
{
    (void)req;
    strncpy(s_last_resp_body, s ? s : "", sizeof(s_last_resp_body) - 1);
    s_last_resp_body[sizeof(s_last_resp_body) - 1] = '\0';
    if (s_last_status == 200) s_test_ok_called = true;
    return ESP_OK;
}
// system_mode_gate_http_send_refusal() calls httpd_resp_send(), not
// httpd_resp_sendstr() -- its own leaf call, needed only because
// system_mode_gate_http.c is linked in for real (same convention as
// test_kiln_cfg_http.c's own copy of this stub).
esp_err_t httpd_resp_send(httpd_req_t *req, const char *buf, ssize_t buf_len)
{
    (void)req;
    if (buf) {
        size_t n = (buf_len < 0) ? strlen(buf) : (size_t)buf_len;
        if (n >= sizeof(s_last_resp_body)) n = sizeof(s_last_resp_body) - 1;
        memcpy(s_last_resp_body, buf, n);
        s_last_resp_body[n] = '\0';
    }
    return ESP_OK;
}

#include "../drivers/http/adaptive_tune_http.c"

// ---------------------------------------------------------------------------
// Test harness
// ---------------------------------------------------------------------------

static void test_reset(void)
{
    s_test_profile_running = false;
    s_test_autotune_running = false;
    g_stub_set_enabled_calls = 0;
    s_stub_set_enabled_last_zone_valid = false;
    s_stub_set_enabled_last_zone = 0;
    s_stub_set_enabled_last_enabled = false;
    g_stub_revert_calls = 0;
    s_stub_revert_result = ADAPTIVE_TUNE_REVERT_OK;
    s_last_status = 200;
    s_last_resp_body[0] = '\0';
    s_test_err_called = false;
    s_test_ok_called = false;
}

static void run_enable(int zone, bool enabled)
{
    snprintf(s_test_post_body, sizeof(s_test_post_body), "zone=%d&enabled=%d", zone, enabled ? 1 : 0);
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.content_len = (long long)strlen(s_test_post_body);
    esp_err_t err = enable_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "enable_post_handler must always return ESP_OK");
}

static void run_revert(int zone)
{
    snprintf(s_test_post_body, sizeof(s_test_post_body), "zone=%d", zone);
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.content_len = (long long)strlen(s_test_post_body);
    esp_err_t err = revert_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "revert_post_handler must always return ESP_OK");
}

static void test_enable_on_refused_while_profile_running(void)
{
    TEST_SECTION("enable_post_handler -- enabled=true is refused 409 by the system mode gate while a "
                 "profile is RUNNING");
    test_reset();
    s_test_profile_running = true;
    run_enable(0, true);
    TEST_CHECK(g_stub_set_enabled_calls == 0, "adaptive_tune_set_enabled() must never be reached once "
              "the mode gate has already refused");
    TEST_CHECK(!s_test_err_called, "the mode gate's refusal goes through system_mode_gate_http_send_refusal(), "
              "not httpd_resp_send_err()");
    TEST_CHECK(!s_test_ok_called, "must not report success");
    TEST_CHECK(s_last_status == 409, "system_mode_gate refusals are always 409 (plan section 5, decision 4)");
}

static void test_enable_on_refused_while_autotune_running(void)
{
    TEST_SECTION("enable_post_handler -- enabled=true is refused 409 while autotune (not a profile) is running");
    test_reset();
    s_test_autotune_running = true;
    run_enable(1, true);
    TEST_CHECK(g_stub_set_enabled_calls == 0, "adaptive_tune_set_enabled() must not be reached");
    TEST_CHECK(s_last_status == 409, "must be refused 409");
}

static void test_enable_off_allowed_while_profile_running(void)
{
    TEST_SECTION("enable_post_handler -- enabled=false is ALLOWED even while a profile is RUNNING "
                 "(owner decision 2026-09-25, later same day: turning OFF can only prevent a future "
                 "change, never apply one)");
    test_reset();
    s_test_profile_running = true;
    run_enable(2, false);
    TEST_CHECK(g_stub_set_enabled_calls == 1, "adaptive_tune_set_enabled() must be reached for enabled=false "
              "even mid-run");
    TEST_CHECK(s_stub_set_enabled_last_zone_valid && s_stub_set_enabled_last_zone == 2,
              "must be called with the requested zone");
    TEST_CHECK(!s_stub_set_enabled_last_enabled, "must be called with enabled=false");
    TEST_CHECK(s_test_ok_called, "must report success");
    TEST_CHECK(s_last_status == 200, "must not be refused");
}

static void test_enable_off_allowed_while_autotune_running(void)
{
    TEST_SECTION("enable_post_handler -- enabled=false is ALLOWED even while autotune is running");
    test_reset();
    s_test_autotune_running = true;
    run_enable(0, false);
    TEST_CHECK(g_stub_set_enabled_calls == 1, "adaptive_tune_set_enabled() must be reached");
    TEST_CHECK(s_test_ok_called, "must report success");
}

static void test_enable_idle_reaches_set_enabled(void)
{
    TEST_SECTION("enable_post_handler -- idle (no firing/autotune), enabled=true reaches the real "
                 "dispatch (positive control)");
    test_reset();
    run_enable(0, true);
    TEST_CHECK(g_stub_set_enabled_calls == 1, "adaptive_tune_set_enabled() must be reached exactly once");
    TEST_CHECK(s_stub_set_enabled_last_enabled, "must be called with enabled=true");
    TEST_CHECK(s_test_ok_called, "must report success");
}

static void test_revert_refused_while_profile_running(void)
{
    TEST_SECTION("revert_post_handler -- refused 409 unconditionally while a profile is RUNNING");
    test_reset();
    s_test_profile_running = true;
    run_revert(0);
    TEST_CHECK(g_stub_revert_calls == 0, "adaptive_tune_revert() must never be reached once the mode "
              "gate has already refused");
    TEST_CHECK(!s_test_err_called, "the mode gate's refusal goes through system_mode_gate_http_send_refusal()");
    TEST_CHECK(!s_test_ok_called, "must not report success");
    TEST_CHECK(s_last_status == 409, "must be refused 409");
}

static void test_revert_refused_while_autotune_running(void)
{
    TEST_SECTION("revert_post_handler -- refused 409 unconditionally while autotune is running "
                 "(unlike enable, revert has no enabled=false carve-out)");
    test_reset();
    s_test_autotune_running = true;
    run_revert(1);
    TEST_CHECK(g_stub_revert_calls == 0, "adaptive_tune_revert() must not be reached");
    TEST_CHECK(s_last_status == 409, "must be refused 409");
}

static void test_revert_idle_reaches_real_dispatch(void)
{
    TEST_SECTION("revert_post_handler -- idle (no firing/autotune) reaches the real dispatch "
                 "(positive control -- proves the refusal tests above are refusing for the right "
                 "reason, not because the handler never gets anywhere)");
    test_reset();
    run_revert(0);
    TEST_CHECK(g_stub_revert_calls == 1, "an unblocked revert must reach adaptive_tune_revert() exactly once");
    TEST_CHECK(s_test_ok_called, "must report success");
}

int main(void)
{
    test_enable_on_refused_while_profile_running();
    test_enable_on_refused_while_autotune_running();
    test_enable_off_allowed_while_profile_running();
    test_enable_off_allowed_while_autotune_running();
    test_enable_idle_reaches_set_enabled();
    test_revert_refused_while_profile_running();
    test_revert_refused_while_autotune_running();
    test_revert_idle_reaches_real_dispatch();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures == 0 ? 0 : 1;
}
