// Host test for App/drivers/http/profiles_export_http.c's POST
// /api/profile/import JSON decode (import_post_handler(), `static` with no
// public seam) -- added 2026-09-06 alongside the dwell_min upper-bound fix
// (previously only `dd < 0` was checked, so an absurd value like
// "dwell_min":1e30 sailed through the (uint32_t) cast -- undefined behavior
// for a double outside uint32_t's range, and even where defined, silently
// wraps into some other in-range dwell_min the caller never asked for).
//
// Same convention as test_zones_http.c/test_backup_import.c: the function
// under test is reachable only by #including profiles_export_http.c
// directly, which pulls in the whole file's handler surface (export GET,
// import POST, profiles_export_http_start()) and must compile+link even
// though these tests only ever call import_post_handler(). This is its own
// SEPARATE host-test executable (own main()), not merged into
// test_main.c/kilnctl_host_tests.exe, for the same multiple-definition
// reason test_zones_http.c/test_profiles_http.c are their own executables:
// this file's fake profiles_http_get()/profiles_http_save()/
// profiles_http_get_bounds()/profiles_http_delete() would collide with the
// real ones test_profiles_http.c links.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

// Ahead of profiles_export_http.c's own #includes, purely for the TYPES the
// stub bodies below need (same convention every other #include-the-.c host
// test in this directory uses).
#include "esp_err.h"
#include "esp_http_server.h"
#include "psa/crypto.h"

// psa/crypto.h's host stub declares this `extern` (only ONE definition per
// executable) -- this executable now links web_auth_store.c
// (docs/WEB_AUTH_PLAN.md section 5/9 route rewiring pulling in
// http_auth_policy_iface.c) so it needs its own copy.
psa_status_t g_stub_psa_import_key_result = PSA_SUCCESS;

// stubs/esp_http_server.h doesn't declare these two query-string helpers or
// the 404 code (same gap test_profiles_http.c hit first) -- declared here,
// ahead of profiles_export_http.c's own #include, so the compiler has real
// prototypes instead of assuming implicit-int. Bodies are defined further
// down.
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t buf_len);
esp_err_t httpd_query_key_value(const char *qs, const char *key, char *val, size_t val_size);
#define HTTPD_404_NOT_FOUND 404

#include "../drivers/http/profiles_export_http.c"

// ---------------------------------------------------------------------------
// profiles_http.h stubs -- import_post_handler() only ever calls
// profiles_http_save() (plus profiles_http_get_bounds() after this fix);
// profiles_http_get()/profiles_http_delete() are only reached from
// export_get_handler(), which these tests don't exercise, but must still
// resolve for the link.
// ---------------------------------------------------------------------------
static bool s_save_called;
static profile_t s_last_saved;
static uint8_t s_last_requested_id;
static bool s_save_should_fail;
static char s_save_fail_msg[128] = "stub refusal";

bool profiles_http_get(uint8_t id, profile_t *out)
{
    (void)id;
    (void)out;
    return false;
}

bool profiles_http_save(uint8_t requested_id, const profile_t *candidate, uint8_t *out_id,
                        uint8_t *out_warning_count, char *err_msg, size_t err_cap)
{
    s_save_called = true;
    s_last_requested_id = requested_id;
    s_last_saved = *candidate;
    if (s_save_should_fail) {
        snprintf(err_msg, err_cap, "%s", s_save_fail_msg);
        return false;
    }
    if (out_id) *out_id = requested_id < PROFILES_MAX_COUNT ? requested_id : 0;
    if (out_warning_count) *out_warning_count = 0;
    return true;
}

bool profiles_http_delete(uint8_t id)
{
    (void)id;
    return false;
}

// The real bound this fix reuses -- overridable per-test so the "no bound at
// all" regression scenario (the negative test) can be simulated without
// touching production code, by simply not calling
// profiles_http_get_bounds_test_set_dwell_max() (default matches the real
// PROFILE_DWELL_MIN_MAX constant, 1440).
static uint32_t s_stub_dwell_max = 1440;

void profiles_http_get_bounds(float *out_target_c_min, float *out_target_c_max,
                              float *out_ramp_c_per_hr_min, float *out_ramp_c_per_hr_max,
                              uint32_t *out_dwell_min_max)
{
    if (out_target_c_min) *out_target_c_min = 0.0f;
    if (out_target_c_max) *out_target_c_max = 1400.0f;
    if (out_ramp_c_per_hr_min) *out_ramp_c_per_hr_min = 0.0f;
    if (out_ramp_c_per_hr_max) *out_ramp_c_per_hr_max = 1000.0f;
    if (out_dwell_min_max) *out_dwell_min_max = s_stub_dwell_max;
}

// ---------------------------------------------------------------------------
// wifi_provision_http.h / esp_http_server stubs -- only reached from
// profiles_export_http_start(), which these tests never call.
// ---------------------------------------------------------------------------
httpd_handle_t wifi_provision_http_get_server(void)
{
    return NULL;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler)
{
    (void)handle;
    (void)uri_handler;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// httpd_req_t body/response plumbing -- a fake request whose body is a
// caller-supplied JSON string, and a captured response buffer so a test can
// inspect ok/error without a real httpd connection.
// ---------------------------------------------------------------------------
typedef struct {
    httpd_req_t base;
    const char *body;
    size_t sent;
} fake_req_t;

int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len)
{
    fake_req_t *fr = (fake_req_t *)r;
    size_t remaining = strlen(fr->body) - fr->sent;
    size_t n = remaining < buf_len ? remaining : buf_len;
    if (n == 0) {
        return 0;
    }
    memcpy(buf, fr->body + fr->sent, n);
    fr->sent += n;
    return (int)n;
}

static char s_response[512];
static int s_response_len;
static int s_response_status; // 0 = not sent

esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, long long buf_len)
{
    (void)r;
    long long n = buf_len < 0 ? (long long)strlen(buf) : buf_len;
    if (n >= (long long)sizeof(s_response)) {
        n = (long long)sizeof(s_response) - 1;
    }
    memcpy(s_response, buf, (size_t)n);
    s_response[n] = '\0';
    s_response_len = (int)n;
    if (s_response_status == 0) {
        s_response_status = 200; // set_status() not called before a plain 200 send
    }
    return ESP_OK;
}
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, size_t buf_len)
{
    (void)r;
    (void)buf;
    (void)buf_len;
    return ESP_OK;
}
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t error, const char *msg)
{
    (void)r;
    (void)msg;
    s_response_status = (int)error;
    return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status)
{
    (void)r;
    s_response_status = atoi(status);
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type)
{
    (void)r;
    (void)type;
    return ESP_OK;
}
// 2026-09-29: http_auth_http.c now calls wifi_prov_request_arrived_on_ap(httpd_req_to_sockfd(req))
// to tag each session touch with whether it arrived over the SoftAP interface.
int httpd_req_to_sockfd(httpd_req_t *r) { (void)r; return -1; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value)
{
    (void)r;
    (void)field;
    (void)value;
    return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s)
{
    return httpd_resp_send(r, s, HTTPD_RESP_USE_STRLEN);
}
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t buf_len)
{
    (void)r;
    (void)buf_len;
    buf[0] = '\0'; // no query string in these tests -- import always picks "first free slot"
    return ESP_OK;
}
esp_err_t httpd_query_key_value(const char *qs, const char *key, char *val, size_t val_size)
{
    (void)qs;
    (void)key;
    (void)val;
    (void)val_size;
    return ESP_FAIL; // "id" never present -- see httpd_req_get_url_query_str() above
}

static void reset_state(void)
{
    s_save_called = false;
    s_save_should_fail = false;
    memset(&s_last_saved, 0, sizeof(s_last_saved));
    s_last_requested_id = 0;
    s_response[0] = '\0';
    s_response_len = 0;
    s_response_status = 0;
    s_stub_dwell_max = 1440;
}

static esp_err_t run_import(const char *json_body)
{
    fake_req_t fr = {0};
    fr.base.content_len = (long long)strlen(json_body);
    fr.body = json_body;
    fr.sent = 0;
    return import_post_handler((httpd_req_t *)&fr);
}

// One well-formed segment, name/zone_mask filled in, dwell_min substituted
// via %s so a test can inject any literal (including a bad one) verbatim,
// same way profiles_page.html's own exporter would encode a real number.
#define MAKE_BODY(dwell_literal)                                                                    \
    "{\"kind\":\"kilnctl_profile\",\"version\":1,\"name\":\"Cone6\",\"zone_mask\":1,"                \
    "\"segments\":[{\"seg_kind\":0,\"target_c\":1200,\"ramp_c_per_hr\":100,"                         \
    "\"dwell_min\":" dwell_literal ",\"io_target\":0,\"io_state\":0,\"io_blocking\":0,"              \
    "\"io_leave_on_at_end\":0}]}"

static void test_valid_dwell_min_is_accepted(void)
{
    reset_state();
    esp_err_t err = run_import(MAKE_BODY("30"));
    TEST_CHECK(err == ESP_OK, "a well-formed import call returns ESP_OK");
    TEST_CHECK(s_save_called, "profiles_http_save() is reached for a valid body");
    TEST_CHECK(s_last_saved.segments[0].dwell_min == 30, "dwell_min 30 round-trips into the candidate");
    TEST_CHECK(strstr(s_response, "\"ok\":true") != NULL, "response reports ok:true");
}

static void test_dwell_min_at_bound_is_accepted(void)
{
    reset_state();
    esp_err_t err = run_import(MAKE_BODY("1440"));
    TEST_CHECK(err == ESP_OK, "dwell_min exactly at the bound (1440) is accepted");
    TEST_CHECK(s_save_called, "profiles_http_save() is reached at the bound");
}

// The regression this file exists for: dwell_min far past any sane bound
// (and past what a uint32_t can even represent) must be REJECTED before the
// (uint32_t) cast, not silently wrapped into some other in-range value.
static void test_absurd_dwell_min_is_rejected(void)
{
    reset_state();
    esp_err_t err = run_import(MAKE_BODY("1e30"));
    TEST_CHECK(err == ESP_OK, "handler still returns ESP_OK (it replied 400 itself, not a transport error)");
    TEST_CHECK(!s_save_called, "profiles_http_save() is NEVER reached for dwell_min:1e30");
    TEST_CHECK(s_response_status == 400, "dwell_min:1e30 is answered with 400 Bad Request");
    TEST_CHECK(strstr(s_response, "\"ok\":false") != NULL, "response reports ok:false for dwell_min:1e30");
}

static void test_dwell_min_one_past_bound_is_rejected(void)
{
    reset_state();
    esp_err_t err = run_import(MAKE_BODY("1441"));
    TEST_CHECK(err == ESP_OK, "handler still returns ESP_OK");
    TEST_CHECK(!s_save_called, "profiles_http_save() is not reached for dwell_min one past the bound");
    TEST_CHECK(s_response_status == 400, "dwell_min:1441 is answered with 400 Bad Request");
}

static void test_negative_dwell_min_is_still_rejected(void)
{
    reset_state();
    esp_err_t err = run_import(MAKE_BODY("-5"));
    TEST_CHECK(err == ESP_OK, "handler still returns ESP_OK");
    TEST_CHECK(!s_save_called, "profiles_http_save() is not reached for a negative dwell_min "
                               "(pre-existing check, must survive this fix)");
    TEST_CHECK(s_response_status == 400, "dwell_min:-5 is answered with 400 Bad Request");
}

// ---------------------------------------------------------------------------
// docs/ON_OFF_ZONE.md plan step 5 -- export/import compatibility for
// the new "on_off_rules" top-level key. See import_post_handler()'s own
// comment (profiles_export_http.c) for the full both-directions writeup;
// these two tests exercise it against the real production import path.
// ---------------------------------------------------------------------------

// An OLD (version:1, pre-rules) export body -- MAKE_BODY() above already IS
// exactly this shape (no "on_off_rules" key at all). Imported into this
// (current) firmware, it must land as a rules-free candidate: rule_count 0,
// same as before the field existed -- the "old export into new firmware"
// half of the compatibility claim.
static void test_old_export_without_rules_key_imports_as_rules_free(void)
{
    reset_state();
    esp_err_t err = run_import(MAKE_BODY("30"));
    TEST_CHECK(err == ESP_OK, "an old (no on_off_rules key) export still imports");
    TEST_CHECK(s_save_called, "profiles_http_save() is reached");
    TEST_CHECK(s_last_saved.on_off_rule_count == 0,
              "no \"on_off_rules\" key in the body -> on_off_rule_count must be 0, "
              "identical to a profile that never had any rules");
}

// A NEW (version:2, has rules) export body, with one on/off rule -- imported
// into this (current) firmware, every rule field must survive intact. This
// is the "new export into new firmware" half; "new export into OLD
// firmware" (the key is silently ignored, profile still imports without its
// rules) cannot be exercised from THIS firmware's own test tree -- see the
// handler's own comment for that direction's reasoning.
#define MAKE_BODY_WITH_RULE                                                                       \
    "{\"kind\":\"kilnctl_profile\",\"version\":2,\"name\":\"Cone6\",\"zone_mask\":1,"              \
    "\"segments\":[{\"seg_kind\":0,\"target_c\":1200,\"ramp_c_per_hr\":100,"                       \
    "\"dwell_min\":30,\"io_target\":0,\"io_state\":0,\"io_blocking\":0,"                           \
    "\"io_leave_on_at_end\":0}],"                                                                   \
    "\"on_off_rules\":[{\"zone\":2,\"segment\":0,\"enable\":1,\"phase_mask\":2,"                    \
    "\"direction_mask\":1,\"temp_cmp\":1,\"temp_c\":650.5,\"time_start_s\":5,"                      \
    "\"time_stop_s\":0,\"invert\":0}]}"

static void test_new_export_with_rules_key_round_trips_every_field(void)
{
    reset_state();
    esp_err_t err = run_import(MAKE_BODY_WITH_RULE);
    TEST_CHECK(err == ESP_OK, "a new (has on_off_rules key) export imports");
    TEST_CHECK(s_save_called, "profiles_http_save() is reached");
    TEST_CHECK(s_last_saved.on_off_rule_count == 1, "exactly one rule imported");
    const profile_on_off_rule_t *r = &s_last_saved.on_off_rules[0];
    TEST_CHECK(r->zone_index == 2, "zone_index round-trips");
    TEST_CHECK(r->segment_index == 0, "segment_index round-trips");
    TEST_CHECK(r->enable == 1, "enable round-trips");
    TEST_CHECK(r->phase_mask == 2, "phase_mask round-trips");
    TEST_CHECK(r->direction_mask == 1, "direction_mask round-trips");
    TEST_CHECK(r->temp_cmp == 1, "temp_cmp round-trips");
    TEST_CHECK_NEAR(r->temp_threshold_c, 650.5f, 1e-4, "temp_threshold_c round-trips");
    TEST_CHECK(r->time_start_s == 5 && r->time_stop_s == 0, "time window round-trips");
    TEST_CHECK(r->invert == 0, "invert round-trips");
}

// An out-of-range temp_c must not be stored unbounded, even for a rule whose
// temp_cmp is NONE (0) -- validate_on_off_rules() (profiles_http.c) only
// range-checks temp_threshold_c when temp_cmp != NONE, so a rule that never
// uses its threshold used to be able to carry an absurd stored value
// straight through import with no bound applied at all (Opus review N5).
// backup_json_field_opt_num() rejects an out-of-range value the same way
// every sibling optional rule field already does -- has_v stays false and
// the field is left at its zeroed default, not clamped to a boundary.
#define MAKE_BODY_WITH_OUT_OF_RANGE_TEMP_C                                                        \
    "{\"kind\":\"kilnctl_profile\",\"version\":2,\"name\":\"Cone6\",\"zone_mask\":1,"              \
    "\"segments\":[{\"seg_kind\":0,\"target_c\":1200,\"ramp_c_per_hr\":100,"                       \
    "\"dwell_min\":30,\"io_target\":0,\"io_state\":0,\"io_blocking\":0,"                           \
    "\"io_leave_on_at_end\":0}],"                                                                   \
    "\"on_off_rules\":[{\"zone\":2,\"segment\":0,\"enable\":1,\"phase_mask\":2,"                    \
    "\"direction_mask\":1,\"temp_cmp\":0,\"temp_c\":999999,\"time_start_s\":5,"                     \
    "\"time_stop_s\":0,\"invert\":0}]}"

static void test_out_of_range_temp_c_is_not_stored_unbounded(void)
{
    reset_state();
    esp_err_t err = run_import(MAKE_BODY_WITH_OUT_OF_RANGE_TEMP_C);
    TEST_CHECK(err == ESP_OK, "handler replies itself");
    TEST_CHECK(s_response_status == 400, "a present-but-out-of-range temp_c fails the import (400), not coerced");
    TEST_CHECK(!s_save_called, "nothing is saved when an optional rule field is present but invalid");
}

// WP-4: an aux target (zone byte 8..11 = aux relay 1..4) is carried through
// import verbatim under the same "zone" key; the handler does not interpret it
// (profiles_http_save()'s validate_on_off_rules() does, faked here).
#define MAKE_BODY_WITH_AUX_RULES                                                                  \
    "{\"kind\":\"kilnctl_profile\",\"version\":2,\"name\":\"Cone6\",\"zone_mask\":1,"              \
    "\"segments\":[{\"seg_kind\":0,\"target_c\":1200,\"ramp_c_per_hr\":100,"                       \
    "\"dwell_min\":30,\"io_target\":0,\"io_state\":0,\"io_blocking\":0,"                           \
    "\"io_leave_on_at_end\":0}],"                                                                   \
    "\"on_off_rules\":[{\"zone\":11,\"segment\":0,\"enable\":1,\"temp_source\":1,"                  \
    "\"temp_cmp\":1,\"temp_c\":100,\"time_start_s\":0,\"time_stop_s\":0,\"invert\":0},"             \
    "{\"zone\":8,\"segment\":0,\"enable\":1}]}"

static void test_aux_target_byte_round_trips_through_import(void)
{
    reset_state();
    esp_err_t err = run_import(MAKE_BODY_WITH_AUX_RULES);
    TEST_CHECK(err == ESP_OK, "an export carrying aux targets 11 and 8 imports");
    TEST_CHECK(s_last_saved.on_off_rule_count == 2, "both rules imported");
    TEST_CHECK(s_last_saved.on_off_rules[0].zone_index == 11, "aux target 11 survives import verbatim");
    TEST_CHECK(s_last_saved.on_off_rules[1].zone_index == 8, "aux target 8 survives import verbatim");
    TEST_CHECK(s_last_saved.on_off_rules[0].temp_source == 1, "temp_source 1 survives for an aux rule");
    reset_state();
    TEST_CHECK(run_import(MAKE_BODY_WITH_RULE) == ESP_OK && s_last_saved.on_off_rules[0].zone_index == 2,
              "an old-style zone-targeted rule still imports as zone 2");
}

static void test_busy_save_refusal_is_409(void)
{
    reset_state();
    s_save_should_fail = true;
    snprintf(s_save_fail_msg, sizeof(s_save_fail_msg), "busy: zone conversion running, retry");
    run_import(MAKE_BODY("30"));
    TEST_CHECK(s_response_status == 409, "a busy: save refusal is answered with 409");
    snprintf(s_save_fail_msg, sizeof(s_save_fail_msg), "stub refusal");
    reset_state();
    s_save_should_fail = true;
    run_import(MAKE_BODY("30"));
    TEST_CHECK(s_response_status == 400, "any other save refusal stays 400");
}

int main(void)
{
    TEST_SECTION("profile_export_import");
    test_aux_target_byte_round_trips_through_import();

    test_valid_dwell_min_is_accepted();
    test_dwell_min_at_bound_is_accepted();
    test_absurd_dwell_min_is_rejected();
    test_dwell_min_one_past_bound_is_rejected();
    test_negative_dwell_min_is_still_rejected();
    test_old_export_without_rules_key_imports_as_rules_free();
    test_new_export_with_rules_key_round_trips_every_field();
    test_out_of_range_temp_c_is_not_stored_unbounded();
    test_busy_save_refusal_is_409();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures > 0 ? 1 : 0;
}
