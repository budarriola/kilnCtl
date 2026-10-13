// Host test for App/drivers/http/cfg_fs_format_http.c (round 3, R3-A):
// GET /api/cfgfs/format_pending and POST /api/cfgfs/format_confirm.
// The confirm handler is the data-loss gate for the cfg LittleFS partition,
// which since the NVS dual-write close holds the only up-to-date config.
// #includes the real .c; links the real cfg_fs_format_gate.c (decision +
// ?force_healthy= parser), system_mode_gate.c and system_mode_gate_http.c.
// Faked: cfg_fs status/recovery flags, the mount module (pending flag, reason,
// format call), relay_authority, and the httpd response leaf calls.
//
// Pins: mode gate (firing or autotune) refuses 409 before cfg_fs is touched;
// recovery mode refuses 409 even with force_healthy=1; a mounted cfg refuses
// 409 without force_healthy=1 and only the exact value "1" counts; unmounted
// formats exactly once; format failure answers 500; pending GET reports JSON.
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
#include "../drivers/persist/cfg_fs.h"

httpd_handle_t wifi_provision_http_get_server(void) { return (httpd_handle_t)1; }

static int s_reg_fail_at = 0; /* 1-based registration call that fails; 0 = none */
static int s_reg_calls = 0;
static const httpd_uri_t *s_reg_uris[4];
esp_err_t kiln_http_register(httpd_handle_t server, const httpd_uri_t *uri)
{
    (void)server;
    s_reg_calls++;
    if (s_reg_calls <= 4) s_reg_uris[s_reg_calls - 1] = uri;
    return (s_reg_fail_at == s_reg_calls) ? ESP_FAIL : ESP_OK;
}

/* cfg_fs.h seams */
static cfg_fs_status_t s_status = CFG_FS_STATUS_MOUNTED;
static bool s_recovery = false;
cfg_fs_status_t cfg_fs_get_status(void) { return s_status; }
bool cfg_fs_skipped_for_recovery(void) { return s_recovery; }

/* cfg_fs_mount.h seams */
static bool s_pending = false;
static const char *s_pending_reason = "";
static esp_err_t s_format_result = ESP_OK;
static int s_format_calls = 0;
bool cfg_fs_mount_format_confirmation_pending(void) { return s_pending; }
const char *cfg_fs_mount_format_pending_reason(void) { return s_pending_reason; }
esp_err_t cfg_fs_confirm_format_device(void)
{
    s_format_calls++;
    return s_format_result;
}

/* relay_authority */
static bool s_profile_running = false;
static bool s_autotune_running = false;
void relay_authority_heat_run_active(bool *p, bool *a)
{
    if (p) *p = s_profile_running;
    if (a) *a = s_autotune_running;
}

void ota_http_get_client_ip(httpd_req_t *req, char *out, size_t out_len)
{
    (void)req;
    snprintf(out, out_len, "127.0.0.1");
}

/* httpd leaf calls */
static int s_status_code = 200;
static char s_body[512];
static int s_err_calls = 0;
esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status)
{
    (void)req;
    s_status_code = atoi(status);
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *t) { (void)req; (void)t; return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *f, const char *v)
{
    (void)req; (void)f; (void)v;
    return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *s)
{
    (void)req;
    snprintf(s_body, sizeof(s_body), "%s", s ? s : "");
    return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t *req, const char *buf, ssize_t len)
{
    (void)req; (void)len;
    snprintf(s_body, sizeof(s_body), "%s", buf ? buf : "");
    return ESP_OK;
}
esp_err_t httpd_resp_send_err(httpd_req_t *req, httpd_err_code_t e, const char *msg)
{
    (void)req; (void)e;
    s_err_calls++;
    snprintf(s_body, sizeof(s_body), "%s", msg ? msg : "");
    return ESP_OK;
}

#include "../drivers/http/cfg_fs_format_http.c"

static void reset(void)
{
    s_status = CFG_FS_STATUS_MOUNTED;
    s_recovery = false;
    s_pending = false;
    s_pending_reason = "";
    s_format_result = ESP_OK;
    s_format_calls = 0;
    s_profile_running = false;
    s_autotune_running = false;
    s_status_code = 200;
    s_body[0] = '\0';
    s_err_calls = 0;
}

static void post(const char *uri)
{
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.uri, sizeof(req.uri), "%s", uri);
    TEST_CHECK(format_confirm_post_handler(&req) == ESP_OK, "handler always returns ESP_OK");
}

static void test_mode_gate(void)
{
    TEST_SECTION("format_confirm -- a firing or autotune run refuses 409 before cfg_fs is consulted");
    reset();
    s_status = CFG_FS_STATUS_UNAVAILABLE;
    s_profile_running = true;
    post("/api/cfgfs/format_confirm?force_healthy=1");
    TEST_CHECK(s_status_code == 409, "firing: 409");
    TEST_CHECK(s_format_calls == 0, "firing: never formats");
    reset();
    s_status = CFG_FS_STATUS_UNAVAILABLE;
    s_autotune_running = true;
    post("/api/cfgfs/format_confirm");
    TEST_CHECK(s_status_code == 409 && s_format_calls == 0, "autotune: 409, never formats");
}

static void test_recovery(void)
{
    TEST_SECTION("format_confirm -- recovery mode never formats, force_healthy does not override");
    reset();
    s_status = CFG_FS_STATUS_UNMOUNTED;
    s_recovery = true;
    post("/api/cfgfs/format_confirm?force_healthy=1");
    TEST_CHECK(s_status_code == 409, "recovery: 409");
    TEST_CHECK(s_format_calls == 0, "recovery: never formats");
    TEST_CHECK(strstr(s_body, "recovery mode") != NULL, "recovery: body names recovery mode");
}

static void test_healthy(void)
{
    TEST_SECTION("format_confirm -- mounted cfg refuses unless force_healthy is exactly 1");
    static const char *const uris[] = {
        "/api/cfgfs/format_confirm",
        "/api/cfgfs/format_confirm?force_healthy=0",
        "/api/cfgfs/format_confirm?force_healthy=11",
        "/api/cfgfs/format_confirm?force_healthy=",
        "/api/cfgfs/format_confirm?force_healthy",
        "/api/cfgfs/format_confirm?xforce_healthy=1",
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        reset();
        post(uris[i]);
        TEST_CHECK(s_status_code == 409 && s_format_calls == 0, "mounted without exact override: refused");
        TEST_CHECK(strstr(s_body, "force_healthy=1") != NULL, "refusal tells how to override");
    }
    reset();
    post("/api/cfgfs/format_confirm?force_healthy=1");
    TEST_CHECK(s_format_calls == 1 && s_status_code == 200, "force_healthy=1: formats once, 200");
    TEST_CHECK(strstr(s_body, "formatted") != NULL, "success body");
    /* Duplicate key: the first occurrence decides. */
    reset();
    post("/api/cfgfs/format_confirm?force_healthy=0&force_healthy=1");
    TEST_CHECK(s_status_code == 409 && s_format_calls == 0, "duplicate key: first (0) wins, refused");
    reset();
    post("/api/cfgfs/format_confirm?force_healthy=1&force_healthy=0");
    TEST_CHECK(s_format_calls == 1, "duplicate key: first (1) wins, formats");
    reset();
    post("/api/cfgfs/format_confirm?a=b&force_healthy=1");
    TEST_CHECK(s_format_calls == 1, "force_healthy=1 as second key: formats");
}

static void test_unmounted_and_failure(void)
{
    TEST_SECTION("format_confirm -- unmounted cfg formats without override; failure answers 500");
    reset();
    s_status = CFG_FS_STATUS_UNAVAILABLE;
    post("/api/cfgfs/format_confirm");
    TEST_CHECK(s_format_calls == 1 && s_status_code == 200, "unmounted: formats once");
    reset();
    s_status = CFG_FS_STATUS_UNAVAILABLE;
    s_format_result = ESP_FAIL;
    post("/api/cfgfs/format_confirm");
    TEST_CHECK(s_format_calls == 1, "failure path still attempted once");
    TEST_CHECK(s_status_code == 500, "failure: 500");
    TEST_CHECK(strstr(s_body, "format failed") != NULL, "failure body names the failure");
    TEST_CHECK(strstr(s_body, "ok --") == NULL, "failure never claims success");
}

static void test_pending_get(void)
{
    TEST_SECTION("format_pending GET -- reports pending flag and reason");
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    reset();
    TEST_CHECK(format_pending_get_handler(&req) == ESP_OK, "ok");
    TEST_CHECK(strcmp(s_body, "{\"pending\":false,\"reason\":\"\"}") == 0, "idle: pending false, empty reason");
    reset();
    s_pending = true;
    s_pending_reason = "corrupt superblock";
    format_pending_get_handler(&req);
    TEST_CHECK(strcmp(s_body, "{\"pending\":true,\"reason\":\"corrupt superblock\"}") == 0,
               "pending: true with reason");
    reset();
    s_pending = false;
    s_pending_reason = "stale reason";
    format_pending_get_handler(&req);
    TEST_CHECK(strstr(s_body, "stale") == NULL, "not pending: reason suppressed");
    reset();
    static char big[400];
    memset(big, 'x', sizeof(big) - 1);
    s_pending = true;
    s_pending_reason = big;
    format_pending_get_handler(&req);
    TEST_CHECK(s_err_calls == 1, "overlong reason answers an error, not truncated JSON");
}

static void test_start(void)
{
    TEST_SECTION("cfg_fs_format_http_start -- registers GET pending and POST confirm; propagates failure");
    s_reg_calls = 0;
    s_reg_fail_at = 0;
    TEST_CHECK(cfg_fs_format_http_start() == ESP_OK && s_reg_calls == 2, "two routes registered");
    TEST_CHECK(strcmp(s_reg_uris[0]->uri, "/api/cfgfs/format_pending") == 0 && s_reg_uris[0]->method == HTTP_GET,
               "route 1 is GET pending");
    TEST_CHECK(strcmp(s_reg_uris[1]->uri, "/api/cfgfs/format_confirm") == 0 && s_reg_uris[1]->method == HTTP_POST,
               "route 2 is POST confirm (never GET)");
    s_reg_calls = 0;
    s_reg_fail_at = 2;
    TEST_CHECK(cfg_fs_format_http_start() == ESP_FAIL, "second registration failure propagates");
    s_reg_calls = 0;
    s_reg_fail_at = 1;
    TEST_CHECK(cfg_fs_format_http_start() == ESP_FAIL && s_reg_calls == 1, "first failure stops registration");
}

int main(void)
{
    test_mode_gate();
    test_recovery();
    test_healthy();
    test_unmounted_and_failure();
    test_pending_get();
    test_start();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures == 0 ? 0 : 1;
}
