// Host test for App/drivers/http/iter_tune_http.c's POST
// /api/iter_tune/restore_commissioned handler (iter_tune_restore_post_
// handler(), static -- reached here by #including the driver file directly,
// same "no other seam" convention test_partition_info_http.c/test_zones_
// http.c already document).
//
// WHY THIS EXISTS (step 7 review, 2026-09-23, finding 2 + advisory A1, and
// the same day's step-7 follow-up race review):
//   - finding 2a (reset-one-side): iter_tune_restore_commissioned() sets
//     the in-RAM state's baseline = the restored gains, but the handler
//     used to persist only enabled/status/stop_reason, leaving the
//     persisted baseline_* stale. Must now match transient.baseline.
//   - finding 2b: a refused zones_config_set_pid() must leave the
//     persisted record completely untouched, not recorded as OFF.
//   - A1: refuse (409) while autotune owns the zone, without touching the
//     persisted record or calling zones_config_set_pid() at all.
//   - race follow-up: the handler used to call autotune_engine_is_active_
//     on_zone() once and apply gains later, with no interlock against
//     autotune starting in the window between -- closed by replacing that
//     check with autotune_engine_reserve_zone_for_external_write()/
//     autotune_engine_release_zone_for_external_write(), which this file
//     fakes as a single held/not-held flag (s_fake_reservation_held) so the
//     tests below can assert the write happens ONLY while reserved and the
//     reservation is released on every exit path once acquired, never
//     otherwise -- see test_set_pid_observes_reservation_held_and_release_
//     always_paired() below. This does not exercise the real engine's
//     lock-protected flag or autotune_begin_run_locked()'s matching check
//     (both live in autotune_engine.c, which pulls in readiness_gate.h,
//     ota_http.h, relay_authority.h, profile_executor.c and more -- far too
//     much to stub for this file, which is deliberately scoped to the
//     handler alone, same as test_autotune_engine_prestart.c staying
//     separate from this one). What IS host-tested here is the property
//     that makes the real fix race-free: the handler brackets its entire
//     read-compute-write-persist sequence between one reserve() and one
//     release() call, on every path, with no gap where reserve succeeded but
//     release was skipped.
// This file exercises all of the above end to end against a fake iter_tune_
// store (in-RAM struct, no real NVS/cfg_fs) and fake zones_config_set_pid()/
// autotune_engine_reserve_zone_for_external_write()/autotune_engine_release_
// zone_for_external_write(), asserting on the persisted record AFTER each
// call, not merely the handler's return code -- the same "assert on the
// actual output, not just ESP_OK" discipline test_partition_info_http.c's
// own header comment describes.
//
// Own executable (not merged into kilnctl_host_tests.exe), same reason
// test_partition_info_http.c/test_zones_http.c get their own: this file
// supplies its own iter_tune_store_*/zones_config_set_pid/autotune_engine_
// reserve_zone_for_external_write/autotune_engine_release_zone_for_external_
// write bodies, which would multiply-define against another test file's own
// fakes of the same symbols if linked together.
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include "esp_err.h"
#include "esp_http_server.h"
#include "hal_kv.h"  /* worst-case shadow blob fixture, test_status_reports_shadow_summary() */
#include "fake_kv.h" /* fake_kv_reset_all() */

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
// Declared here (ahead of its normal fake-group block below, which needs the
// store fakes' own declarations already visible) so iter_tune_store_set_zone()
// -- defined next -- can read it. See the reservation fakes' own comment
// (below, near autotune_engine_reserve_zone_for_external_write()) for what
// this models.
static bool s_fake_reservation_held = false;
static bool s_reserved_during_set_zone = false;

static void fake_store_reset(void)
{
    memset(s_fake_store, 0, sizeof(s_fake_store));
    memset(s_fake_present, 0, sizeof(s_fake_present));
    s_set_zone_calls = 0;
    s_set_zone_result = ESP_OK;
    s_reserved_during_set_zone = false;
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
    /* Advisory 5 (opus review, 2026-09-23 second pass): the persisted-record
     * write must ALSO happen strictly inside the reserved window, not just
     * the live-gains write above -- the handler's own comment says the
     * reservation is "held across both the live-gains write above and this
     * persisted-record write", and moving the release() to before this call
     * would leave exactly that gap unprotected. */
    s_reserved_during_set_zone = s_fake_reservation_held;
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
// Fake zones_config_set_pid()/autotune_engine_reserve_zone_for_external_
// write()/autotune_engine_release_zone_for_external_write() -- the seams
// the handler decides its whole outcome on, and the pair that brackets the
// race window (see this file's header comment).
//
// s_fake_reservation_held models the real engine's s_at.external_write_
// reserved flag: set true by a successful reserve(), false by release().
// zones_config_set_pid() captures its value at call time (s_reserved_
// during_set_pid) so a test can assert the write only ever happens while
// reserved -- the property that makes the real, lock-protected version of
// this flag actually close the race against autotune_begin_run_locked().
// ---------------------------------------------------------------------
static bool s_set_pid_result = true;
static int s_set_pid_calls = 0;
static float s_last_set_pid_kp, s_last_set_pid_ki, s_last_set_pid_kd;
static uint8_t s_last_set_pid_zone;
static bool s_reserved_during_set_pid = false;

bool zones_config_set_pid(uint8_t zone_index, float kp, float ki, float kd)
{
    s_set_pid_calls++;
    s_last_set_pid_zone = zone_index;
    s_last_set_pid_kp = kp;
    s_last_set_pid_ki = ki;
    s_last_set_pid_kd = kd;
    s_reserved_during_set_pid = s_fake_reservation_held;
    return s_set_pid_result;
}

// ITER_TUNE_REDESIGN_PLAN.md step 8: iter_tune_status_get_handler() now
// calls firing_shadow_get_status(), and firing_shadow.c is linked into this
// executable for real (build_host_tests.ps1's $cmdIth) -- but firing_shadow.c
// itself calls zone_model_at()/zones_config_get_progress_band_c() (real
// implementations live in zones_config_accessors.c, not linked here, same
// as this file's existing zones_config_set_pid()/autotune_engine_* fakes
// avoid pulling in the full zones_config_accessors.c or autotune_engine.c
// translation units). These two are never exercised by any test in this
// file (no test here drives firing_shadow_zone_tick() to the point of
// needing a real model) -- fixed, harmless stand-ins, same pattern as
// test_profile_executor_prestart.c's own fakes of the same two functions.
bool zone_model_at(uint8_t zone_index, float T_c, float *out_k_dc, float *out_tau_s, float *out_dead_time_s)
{
    (void)zone_index;
    (void)T_c;
    if (out_k_dc) *out_k_dc = 0.0f;
    if (out_tau_s) *out_tau_s = 0.0f;
    if (out_dead_time_s) *out_dead_time_s = 0.0f;
    return false;
}

bool zones_config_get_progress_band_c(uint8_t zone_index, float *out_band_c)
{
    (void)zone_index;
    if (out_band_c) *out_band_c = 0.0f;
    return false;
}

static bool s_reserve_result = true;   // false simulates autotune already owning the zone
static int s_reserve_calls = 0;
static int s_release_calls = 0;
static uint8_t s_last_reserve_zone = 0xFF, s_last_release_zone = 0xFF;

bool autotune_engine_reserve_zone_for_external_write(uint8_t zone_index)
{
    s_reserve_calls++;
    s_last_reserve_zone = zone_index;
    if (!s_reserve_result) {
        return false; // refused -- nothing reserved, nothing to release
    }
    s_fake_reservation_held = true;
    return true;
}

void autotune_engine_release_zone_for_external_write(uint8_t zone_index)
{
    s_release_calls++;
    s_last_release_zone = zone_index;
    s_fake_reservation_held = false;
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
    s_fake_schema_refused = false;
    s_fake_schema_refused_version = 0;
    s_reserved_during_set_pid = false;
    s_reserve_result = true;
    s_reserve_calls = 0;
    s_release_calls = 0;
    s_last_reserve_zone = 0xFF;
    s_last_release_zone = 0xFF;
    s_fake_reservation_held = false;
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

    // Race close: the zone must be reserved for the write and released
    // exactly once afterward, on the success path.
    TEST_CHECK(s_reserve_calls == 1, "zone reserved exactly once");
    TEST_CHECK(s_reserved_during_set_pid, "zones_config_set_pid runs while the zone is reserved");
    // Advisory 5 (opus review, 2026-09-23 second pass): the persisted-record
    // write must be inside the reservation too, not just the live-gains
    // write above -- moving the release() call above the
    // iter_tune_store_set_zone() call in iter_tune_http.c would make this
    // fail.
    TEST_CHECK(s_reserved_during_set_zone, "iter_tune_store_set_zone runs while the zone is reserved");
    TEST_CHECK(s_release_calls == 1, "zone reservation released exactly once");
    TEST_CHECK(!s_fake_reservation_held, "reservation is not left held after a successful restore");

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

    // Race close: even on a refused apply, the reservation taken before the
    // write must still be released -- a leaked reservation would wedge every
    // future autotune start on this zone forever.
    TEST_CHECK(s_reserve_calls == 1, "zone reserved exactly once even on a refused apply");
    TEST_CHECK(s_release_calls == 1, "zone reservation released even on a refused apply (no leak)");
    TEST_CHECK(!s_fake_reservation_held, "reservation is not left held after a refused apply");

    iter_tune_store_zone_t out;
    TEST_CHECK(iter_tune_store_get_zone(0, &out), "zone 0 still present");
    TEST_CHECK(out.baseline_kp == before.baseline_kp && out.enabled == before.enabled &&
                   out.status == before.status && out.stop_reason == before.stop_reason,
               "persisted record is byte-for-byte unchanged after a refused apply (finding 2b)");
}

// A1 + race close: refuse with 409 when autotune_engine_reserve_zone_for_
// external_write() refuses (autotune already owns the zone, or -- in the
// real engine -- started in the window this reservation exists to close),
// touching neither zones_config_set_pid() nor the persisted record. Since
// reserve() itself refused, there is nothing to release.
static void test_refuses_when_reservation_refused(void)
{
    reset_capture();
    iter_tune_store_zone_t before = make_commissioned_zone(12.0f, 99.0f);
    s_fake_store[0] = before;
    s_fake_present[0] = true;
    s_fake_query = "zone=0";
    s_reserve_result = false;

    httpd_req_t req = {0};
    esp_err_t err = iter_tune_restore_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK (409 is in the JSON/status)");
    TEST_CHECK(s_resp_status == 409, "a refused reservation reports 409 (A1)");
    TEST_CHECK(strstr(s_resp_body, "autotune is active") != NULL, "409 body names autotune as one possible reason");
    TEST_CHECK(strstr(s_resp_body, "another restore is already in progress") != NULL,
               "409 body also names a concurrent restore as the other possible reason (advisory 4)");
    TEST_CHECK(s_set_pid_calls == 0, "zones_config_set_pid is never called when the reservation is refused (A1)");
    TEST_CHECK(s_set_zone_calls == 0, "iter_tune_store_set_zone is never called when the reservation is refused (A1)");
    TEST_CHECK(s_reserve_calls == 1, "reserve is attempted exactly once");
    TEST_CHECK(s_release_calls == 0, "nothing is released when reserve() itself refused -- there was never a reservation to release");

    iter_tune_store_zone_t out;
    TEST_CHECK(iter_tune_store_get_zone(0, &out), "zone 0 still present");
    TEST_CHECK(out.baseline_kp == before.baseline_kp, "persisted record is unchanged when the reservation is refused (A1)");
}

// Race close, direct: the handler must bracket its ENTIRE read-compute-
// write-persist sequence between exactly one reserve() and one release(),
// with the write observably happening while reserved. This is the property
// that makes the real (lock-protected) flag in autotune_engine.c actually
// exclude a concurrent autotune_begin_run_locked() for the whole window this
// handler is doing its work, not just at the single instant of the old
// autotune_engine_is_active_on_zone() check this replaced.
static void test_reservation_brackets_entire_write_and_pairs_exactly(void)
{
    reset_capture();
    s_fake_store[0] = make_commissioned_zone(12.0f, 99.0f);
    s_fake_present[0] = true;
    s_fake_query = "zone=0";

    TEST_CHECK(!s_fake_reservation_held, "no reservation held before the handler runs");

    httpd_req_t req = {0};
    esp_err_t err = iter_tune_restore_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(s_reserve_calls == 1 && s_release_calls == 1, "reserve/release called exactly once each, paired");
    TEST_CHECK(s_last_reserve_zone == 0 && s_last_release_zone == 0, "reserve and release name the same zone");
    TEST_CHECK(s_reserved_during_set_pid, "the live-gains write happened strictly inside the reserved window");
    TEST_CHECK(!s_fake_reservation_held, "the reservation is fully released once the handler returns");
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
    TEST_CHECK(s_reserve_calls == 0, "the zone is never reserved on a bad request -- refused before that point");
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
    TEST_CHECK(s_reserve_calls == 0, "the zone is never reserved for a never-commissioned zone -- refused before that point");
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

// ITER_TUNE_REDESIGN_PLAN.md step 8: the status body opens with a top-level
// "shadow" object -- null until firing_shadow_store_start() has run (the
// handler never loads it lazily from the httpd task), the counters after.
// Both shapes must still be one well-formed object continuing into "zones".
static void test_status_reports_shadow_summary(void)
{
    firing_shadow_reset_for_test();
    reset_capture();
    httpd_req_t req = {0};
    esp_err_t err = iter_tune_status_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "status handler returns ESP_OK before the shadow store is loaded");
    TEST_CHECK(strncmp(s_resp_body, "{\"shadow\":null,\"zones\":[", strlen("{\"shadow\":null,\"zones\":[")) == 0,
               "status JSON reports shadow:null (not a lazy load) before firing_shadow_store_start()");

    firing_shadow_store_start();
    reset_capture();
    err = iter_tune_status_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "status handler returns ESP_OK once the shadow store is loaded");
    TEST_CHECK(strncmp(s_resp_body,
                       "{\"shadow\":{\"firings_scored\":0,\"accept_count\":0,\"reject_count\":0,"
                       "\"insufficient_count\":0,\"no_matched_pairs_count\":0,\"alloc_failed_count\":0},"
                       "\"zones\":[",
                       strlen("{\"shadow\":{\"firings_scored\":0,\"accept_count\":0,\"reject_count\":0,"
                              "\"insufficient_count\":0,\"no_matched_pairs_count\":0,"
                              "\"alloc_failed_count\":0},\"zones\":[")) == 0,
               "status JSON opens with the shadow counters object, then zones");

    // Worst case: every counter at UINT32_MAX (ten digits each). A persisted
    // v2 blob is written raw -- layout pinned by firing_shadow.c's own
    // _Static_assert (36 B: u8 version, 3 reserved, six u32 counters,
    // u8 last_verdict, 3 reserved, float) -- so this proves the handler's
    // fixed chunk still fits all six counters rather than silently falling
    // back to "shadow":null.
    uint8_t raw[36];
    memset(raw, 0xFF, sizeof(raw));
    raw[0] = 2; // FIRING_SHADOW_STORE_VERSION
    hal_kv_init_partition("kiln_nvs");
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, "shadow_tune", HAL_KV_MODE_READ_WRITE, "kiln_nvs") == HAL_OK,
               "open the shadow store for the worst-case fixture");
    TEST_CHECK(hal_kv_set_blob(&h, "sdwblob", raw, sizeof(raw)) == HAL_OK, "worst-case blob written");
    hal_kv_commit(&h);
    hal_kv_close(&h);
    firing_shadow_reset_for_test();
    firing_shadow_store_start();
    reset_capture();
    err = iter_tune_status_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "status handler returns ESP_OK with every shadow counter at UINT32_MAX");
    TEST_CHECK(strstr(s_resp_body, "\"no_matched_pairs_count\":4294967295,\"alloc_failed_count\":4294967295}") !=
                   NULL,
               "worst-case shadow object is emitted in full (alloc_failed_count included), not truncated to null");
    TEST_CHECK(strstr(s_resp_body, "{\"shadow\":{\"firings_scored\":4294967295,\"accept_count\":4294967295,"
                                   "\"reject_count\":4294967295,\"insufficient_count\":4294967295,") != NULL,
               "worst-case shadow object's two halves join into one contiguous object");
    TEST_CHECK(strstr(s_resp_body, "\"shadow\":null") == NULL && strstr(s_resp_body, "\"truncated\"") == NULL,
               "worst-case shadow object takes neither the null nor the truncated fallback");
    firing_shadow_reset_for_test();
    fake_kv_reset_all();
}

int main(void)
{
    TEST_SECTION("iter_tune_http");

    test_successful_restore_persists_new_baseline();
    test_refused_apply_leaves_record_untouched();
    test_refuses_when_reservation_refused();
    test_reservation_brackets_entire_write_and_pairs_exactly();
    test_missing_zone_query_refuses_400();
    test_never_commissioned_zone_refuses_409();
    test_status_reports_schema_refused_version();
    test_status_reports_shadow_summary();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures > 0 ? 1 : 0;
}
