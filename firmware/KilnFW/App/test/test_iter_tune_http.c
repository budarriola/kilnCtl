// Host test for App/drivers/http/iter_tune_http.c's POST
// /api/iter_tune/restore_commissioned handler (iter_tune_restore_post_
// handler(), static -- reached here by #including the driver file directly,
// same "no other seam" convention test_partition_info_http.c/test_zones_
// http.c already document).
//
// WHY THIS EXISTS (step 7 review, 2026-09-23, finding 2 + advisory A1):
//   - finding 2a (reset-one-side): iter_tune_restore_commissioned() sets
//     the in-RAM state's baseline = the restored gains, but the handler
//     used to persist only enabled/status/stop_reason, leaving the
//     persisted baseline_* stale. Must now match transient.baseline.
//   - finding 2b: a refused zones_config_set_pid() must leave the
//     persisted record completely untouched, not recorded as OFF.
//   - A1: refuse (409) while autotune owns the zone, without touching the
//     persisted record or calling zones_config_set_pid() at all.
// This file exercises all three end to end against a fake iter_tune_store
// (in-RAM struct, no real NVS/cfg_fs) and a fake zones_config_set_pid()/
// autotune_engine_is_active_on_zone(), asserting on the persisted record
// AFTER each call, not merely the handler's return code -- the same
// "assert on the actual output, not just ESP_OK" discipline test_partition_
// info_http.c's own header comment describes.
//
// Own executable (not merged into kilnctl_host_tests.exe), same reason
// test_partition_info_http.c/test_zones_http.c get their own: this file
// supplies its own iter_tune_store_*/zones_config_set_pid/autotune_engine_
// is_active_on_zone bodies, which would multiply-define against another
// test file's own fakes of the same symbols if linked together.
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include "esp_err.h"
#include "esp_http_server.h"

// Forward declarations so the #included driver file below (which calls
// these before this file's own definitions appear lower down) does not
// trip MSVC's C4013 implicit-declaration-as-error -- same "declare before
// #include" pattern test_profile_export_import.c/test_profiles_http.c use.
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *r, const char *str);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t buf_len);
esp_err_t httpd_query_key_value(const char *qs, const char *key, char *val, size_t val_size);

#include "../drivers/http/iter_tune_http.c"

// ---------------------------------------------------------------------
// Fake iter_tune_store -- an in-RAM array, no NVS/cfg_fs. This file only
// needs to prove the HANDLER reads/writes the store correctly; the store's
// own persistence behaviour (NVS/cfg_fs dual-write, rev tie-break, version
// reject) is already covered by test_iter_tune_store.c.
// ---------------------------------------------------------------------
static iter_tune_store_zone_t s_fake_store[ITER_TUNE_STORE_MAX_ZONES];
static bool s_fake_present[ITER_TUNE_STORE_MAX_ZONES];
static int s_set_zone_calls = 0;
static esp_err_t s_set_zone_result = ESP_OK;

static void fake_store_reset(void)
{
    memset(s_fake_store, 0, sizeof(s_fake_store));
    memset(s_fake_present, 0, sizeof(s_fake_present));
    s_set_zone_calls = 0;
    s_set_zone_result = ESP_OK;
}

esp_err_t iter_tune_store_start(void) { return ESP_OK; }

bool iter_tune_store_get_zone(uint8_t zone_index, iter_tune_store_zone_t *out)
{
    if (zone_index >= ITER_TUNE_STORE_MAX_ZONES || !s_fake_present[zone_index]) {
        return false;
    }
    if (out) {
        *out = s_fake_store[zone_index];
    }
    return true;
}

esp_err_t iter_tune_store_set_zone(uint8_t zone_index, const iter_tune_store_zone_t *in)
{
    s_set_zone_calls++;
    if (zone_index >= ITER_TUNE_STORE_MAX_ZONES || in == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_set_zone_result != ESP_OK) {
        return s_set_zone_result;
    }
    s_fake_store[zone_index] = *in;
    s_fake_present[zone_index] = true;
    return ESP_OK;
}

void iter_tune_store_reset_for_test(void) { fake_store_reset(); }

// Fake, controllable (step 7 review, 2026-09-23, advisory finding 10): most
// of this file's http-surface tests want "not refused" (schema-version
// refusal itself is test_iter_tune_store.c's job), but
// test_status_reports_schema_refused_version() below needs to drive the
// status handler's schema_refused_version JSON branch.
static bool s_fake_schema_refused = false;
static uint8_t s_fake_schema_refused_version = 0;

bool iter_tune_store_schema_refused(uint8_t *out_version)
{
    if (s_fake_schema_refused && out_version != NULL) {
        *out_version = s_fake_schema_refused_version;
    }
    return s_fake_schema_refused;
}

// ---------------------------------------------------------------------
// Fake zones_config_set_pid()/autotune_engine_is_active_on_zone() -- the
// two seams the handler decides its whole outcome on.
// ---------------------------------------------------------------------
static bool s_set_pid_result = true;
static int s_set_pid_calls = 0;
static float s_last_set_pid_kp, s_last_set_pid_ki, s_last_set_pid_kd;
static uint8_t s_last_set_pid_zone;

bool zones_config_set_pid(uint8_t zone_index, float kp, float ki, float kd)
{
    s_set_pid_calls++;
    s_last_set_pid_zone = zone_index;
    s_last_set_pid_kp = kp;
    s_last_set_pid_ki = ki;
    s_last_set_pid_kd = kd;
    return s_set_pid_result;
}

static bool s_autotune_active_zone_result = false;
bool autotune_engine_is_active_on_zone(uint8_t zone_index)
{
    (void)zone_index;
    return s_autotune_active_zone_result;
}

// ---------------------------------------------------------------------
// httpd surface -- query string + response capture, same convention
// test_dashboard_status_http.c/test_profiles_http.c use.
// ---------------------------------------------------------------------
httpd_handle_t wifi_provision_http_get_server(void) { return (httpd_handle_t)1; }

esp_err_t kiln_http_register(httpd_handle_t server, const httpd_uri_t *uri_handler)
{
    (void)server;
    (void)uri_handler;
    return ESP_OK;
}

static const char *s_fake_query = ""; // e.g. "zone=1"

esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t buf_len)
{
    (void)r;
    snprintf(buf, buf_len, "%s", s_fake_query);
    return ESP_OK;
}

esp_err_t httpd_query_key_value(const char *qs, const char *key, char *val, size_t val_size)
{
    (void)qs;
    if (strcmp(key, "zone") != 0) {
        return ESP_FAIL;
    }
    const char *eq = strstr(qs, "zone=");
    if (!eq) {
        return ESP_FAIL;
    }
    snprintf(val, val_size, "%s", eq + 5);
    return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type) { (void)r; (void)type; return ESP_OK; }

static char s_resp_body[512];

// iter_tune_status_get_handler() (GET /api/iter_tune/status) chunks its body
// via httpd_resp_sendstr_chunk() (the "httpd stack blob class" convention,
// CLAUDE.md) -- appended into s_resp_body so
// test_status_reports_schema_refused_version() (advisory finding 10) can
// assert on the assembled JSON. A NULL str is httpd's end-of-chunked-response
// marker, not a chunk to append.
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *r, const char *str)
{
    (void)r;
    if (str == NULL) {
        return ESP_OK;
    }
    size_t used = strlen(s_resp_body);
    snprintf(s_resp_body + used, sizeof(s_resp_body) - used, "%s", str);
    return ESP_OK;
}

static int s_resp_status = 200; // tracked as the numeric prefix of the last httpd_resp_set_status() string

esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status)
{
    (void)r;
    s_resp_status = atoi(status);
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, long long buf_len)
{
    (void)r;
    size_t n = (buf_len < 0) ? 0 : (size_t)buf_len;
    if (n >= sizeof(s_resp_body)) {
        n = sizeof(s_resp_body) - 1;
    }
    memcpy(s_resp_body, buf, n);
    s_resp_body[n] = '\0';
    return ESP_OK;
}

static void reset_capture(void)
{
    s_resp_body[0] = '\0';
    s_resp_status = 200; // no explicit set_status call means httpd's real default, 200
    s_set_pid_calls = 0;
    s_set_pid_result = true;
    s_autotune_active_zone_result = false;
    s_fake_schema_refused = false;
    s_fake_schema_refused_version = 0;
    fake_store_reset();
}

static iter_tune_store_zone_t make_commissioned_zone(float anchor_kp, float baseline_kp)
{
    iter_tune_store_zone_t z = {0};
    z.enabled = 1;
    z.has_anchor = 1;
    z.anchor_kp = anchor_kp;
    z.anchor_ki = anchor_kp * 0.1f;
    z.anchor_kd = anchor_kp * 0.01f;
    z.has_baseline = 1;
    z.baseline_kp = baseline_kp;
    z.baseline_ki = baseline_kp * 0.1f;
    z.baseline_kd = baseline_kp * 0.01f;
    z.status = 3; // arbitrary non-OFF value the pre-restore record had
    z.stop_reason = 2;
    return z;
}

// ---------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------

// Finding 2a: a successful restore must persist the RESTORED gains
// (transient.baseline, as iter_tune_restore_commissioned() sets it) into
// stored.baseline_*/has_baseline, not leave the pre-restore baseline.
static void test_successful_restore_persists_new_baseline(void)
{
    reset_capture();
    s_fake_store[0] = make_commissioned_zone(/*anchor_kp=*/12.0f, /*baseline_kp=*/99.0f);
    s_fake_present[0] = true;
    s_fake_query = "zone=0";

    httpd_req_t req = {0};
    esp_err_t err = iter_tune_restore_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK on a successful restore");
    TEST_CHECK(s_set_pid_calls == 1, "zones_config_set_pid called exactly once");
    TEST_CHECK(strstr(s_resp_body, "\"ok\":true") != NULL, "response reports ok:true");

    iter_tune_store_zone_t out;
    TEST_CHECK(iter_tune_store_get_zone(0, &out), "zone 0 still persisted after restore");
    // iter_tune_restore_commissioned() sets state->baseline = the anchor
    // (has_anchor was set), so the persisted baseline must now read the
    // ANCHOR value (12.0), not the stale pre-restore baseline (99.0).
    TEST_CHECK(out.has_baseline == 1, "has_baseline still set after restore");
    TEST_CHECK(out.baseline_kp == 12.0f, "persisted baseline_kp matches the restored gains, not the stale baseline (finding 2a)");
    TEST_CHECK(out.enabled == 0, "restore always leaves the zone disabled");
}

// Finding 2b: a refused zones_config_set_pid() must leave the persisted
// record completely untouched -- no iter_tune_store_set_zone() call at all.
static void test_refused_apply_leaves_record_untouched(void)
{
    reset_capture();
    iter_tune_store_zone_t before = make_commissioned_zone(12.0f, 99.0f);
    s_fake_store[0] = before;
    s_fake_present[0] = true;
    s_fake_query = "zone=0";
    s_set_pid_result = false; // zones_config_set_pid refuses

    httpd_req_t req = {0};
    esp_err_t err = iter_tune_restore_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler still returns ESP_OK (the 500 is in the JSON/status, not the return code)");
    TEST_CHECK(s_resp_status == 500, "refused apply reports 500");
    TEST_CHECK(strstr(s_resp_body, "\"ok\":false") != NULL, "response reports ok:false");
    TEST_CHECK(s_set_zone_calls == 0, "iter_tune_store_set_zone() is never called on a refused apply (finding 2b)");

    iter_tune_store_zone_t out;
    TEST_CHECK(iter_tune_store_get_zone(0, &out), "zone 0 still present");
    TEST_CHECK(out.baseline_kp == before.baseline_kp && out.enabled == before.enabled &&
                   out.status == before.status && out.stop_reason == before.stop_reason,
               "persisted record is byte-for-byte unchanged after a refused apply (finding 2b)");
}

// A1: refuse with 409 while autotune owns the zone, touching neither
// zones_config_set_pid() nor the persisted record.
static void test_refuses_while_autotune_active_on_zone(void)
{
    reset_capture();
    iter_tune_store_zone_t before = make_commissioned_zone(12.0f, 99.0f);
    s_fake_store[0] = before;
    s_fake_present[0] = true;
    s_fake_query = "zone=0";
    s_autotune_active_zone_result = true;

    httpd_req_t req = {0};
    esp_err_t err = iter_tune_restore_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK (409 is in the JSON/status)");
    TEST_CHECK(s_resp_status == 409, "autotune-active zone refuses with 409 (A1)");
    TEST_CHECK(strstr(s_resp_body, "autotune is running") != NULL, "409 body names autotune as the reason");
    TEST_CHECK(s_set_pid_calls == 0, "zones_config_set_pid is never called while autotune owns the zone (A1)");
    TEST_CHECK(s_set_zone_calls == 0, "iter_tune_store_set_zone is never called while autotune owns the zone (A1)");

    iter_tune_store_zone_t out;
    TEST_CHECK(iter_tune_store_get_zone(0, &out), "zone 0 still present");
    TEST_CHECK(out.baseline_kp == before.baseline_kp, "persisted record is unchanged while autotune owns the zone (A1)");
}

static void test_missing_zone_query_refuses_400(void)
{
    reset_capture();
    s_fake_query = "";

    httpd_req_t req = {0};
    esp_err_t err = iter_tune_restore_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK on a missing zone param (400 is in the JSON/status)");
    TEST_CHECK(s_resp_status == 400, "missing zone param refuses with 400");
    TEST_CHECK(s_set_pid_calls == 0, "zones_config_set_pid never called on a bad request");
}

static void test_never_commissioned_zone_refuses_409(void)
{
    reset_capture();
    s_fake_query = "zone=1"; // never present in the store
    s_fake_present[1] = false;

    httpd_req_t req = {0};
    esp_err_t err = iter_tune_restore_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK on a never-commissioned zone (409 is in the JSON/status)");
    TEST_CHECK(s_resp_status == 409, "never-commissioned zone refuses with 409");
    TEST_CHECK(s_set_pid_calls == 0, "zones_config_set_pid never called for a never-commissioned zone");
}

// Advisory finding 10 (step 7 review, 2026-09-23): GET /api/iter_tune/status
// must surface a refused newer-than-known schema version as
// "schema_refused_version" in its JSON preamble, not silently.
static void test_status_reports_schema_refused_version(void)
{
    reset_capture();
    s_fake_schema_refused = true;
    s_fake_schema_refused_version = 3;

    httpd_req_t req = {0};
    esp_err_t err = iter_tune_status_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "status handler returns ESP_OK even when a schema refusal is reported");
    TEST_CHECK(strstr(s_resp_body, "\"schema_refused_version\":3") != NULL,
               "status JSON reports the refused version number");

    reset_capture(); // not refused: no such field at all
    err = iter_tune_status_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "status handler returns ESP_OK when nothing was refused");
    TEST_CHECK(strstr(s_resp_body, "schema_refused_version") == NULL,
               "status JSON omits schema_refused_version entirely when nothing was refused");
}

int main(void)
{
    TEST_SECTION("iter_tune_http");

    test_successful_restore_persists_new_baseline();
    test_refused_apply_leaves_record_untouched();
    test_refuses_while_autotune_active_on_zone();
    test_missing_zone_query_refuses_400();
    test_never_commissioned_zone_refuses_409();
    test_status_reports_schema_refused_version();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures > 0 ? 1 : 0;
}
