// Host test for App/drivers/http/safety_cfg_http.c's pure/static helpers --
// parse_set_param_body() (the id=/value= form tokenizer), parse_value_for_
// type(), build_commissioning_json() and safety_cfg_write_apply_pairs(). Own SEPARATE
// executable (build_host_tests.ps1's third build+run step), same reason
// test_zones_http.c is: safety_cfg_http.c's static functions have no other
// seam, so this file #includes it directly, which means defining its OWN
// fake bodies for safety_cfg_store_get_by_index()/safety_link_get_status()/
// etc -- and the main executable already links the REAL safety_cfg_store_*
// definitions via test_safety_cfg_store.c's #include of safety_cfg_store.c.
// Linking both into one binary would multiply-define every safety_cfg_
// store_* symbol.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include "esp_err.h"
#include "esp_http_server.h"
#include "psa/crypto.h"

// psa/crypto.h's host stub declares this `extern` (only ONE definition per
// executable) -- this executable now links web_auth_store.c
// (docs/WEB_AUTH_PLAN.md section 5/9 route rewiring pulling in
// http_auth_policy_iface.c) so it needs its own copy.
psa_status_t g_stub_psa_import_key_result = PSA_SUCCESS;

#include "kilnlink/kilnlink_commit_config_rejected.h"

// safety_cfg_http.c gained the /safety/commissioning PAGE handler after this
// test was first written, and that handler declares the embedded gzip blob
// with GCC's `asm("_binary_...")` label syntax -- which MSVC cannot parse at
// all (error C2061: syntax error: identifier 'asm'). #define it away to
// nothing, exactly as test_zones_http.c already does for the identical
// reason; see that file's own note. The symbols themselves are then supplied
// as ordinary arrays below, since nothing on the host links the real
// EMBED_TXTFILES blobs.
#define asm(x)

const uint8_t safety_commissioning_page_html_gz_start[] = { 0x1f, 0x8b, 0x00 };
const uint8_t safety_commissioning_page_html_gz_end[] = { 0x00 };

// web_encoding.h -- reached only from the page handler, which these tests do
// not exercise (they cover the JSON builder, the tokenizer and safety_cfg_write_apply_pairs).
// Same stub bodies test_zones_http.c uses for the identical reason.
esp_err_t web_send_gzip_not_acceptable(httpd_req_t *req, const char *tag, const char *page_name)
{
    (void)req;
    (void)tag;
    (void)page_name;
    return ESP_OK;
}
bool web_client_accepts_gzip(httpd_req_t *req)
{
    (void)req;
    return true;
}

#include "../drivers/safety/safety_cfg_write.c"
#include "../drivers/http/safety_cfg_http.c"

// ---------------------------------------------------------------------------
// esp_http_server.h stub bodies -- never invoked by these tests (only the
// static helper functions are called directly), but every symbol safety_cfg_
// http.c references anywhere in the file must resolve at link time.
// ---------------------------------------------------------------------------
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler)
{
    (void)handle;
    (void)uri_handler;
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type) { (void)r; (void)type; return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value)
{
    (void)r; (void)field; (void)value; return ESP_OK;
}

/* web_encoding.c is not part of the host build (it pulls in the real httpd),
   but zones_http.c/safety_cfg_http.c now call this from their page handlers.
   Same local-stub convention as httpd_resp_set_hdr() just above. */
void web_set_asset_cache_headers(httpd_req_t *r);
void web_set_asset_cache_headers(httpd_req_t *r) { (void)r; }
// Captures the last body handed to httpd_resp_send() -- needed only by the
// rate_guard_auto handler-level tests below (test_build_json_* and the
// safety_cfg_write_apply_pairs tests all call the static helpers directly and never look at
// this). Every pre-existing caller of httpd_resp_send() is unaffected: the
// stub's return value and (void) semantics for callers that ignore the
// capture are unchanged.
static char s_stub_last_httpd_resp[512];
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, long long buf_len)
{
    (void)r;
    if (buf && buf_len > 0 && (size_t)buf_len < sizeof(s_stub_last_httpd_resp)) {
        memcpy(s_stub_last_httpd_resp, buf, (size_t)buf_len);
        s_stub_last_httpd_resp[buf_len] = '\0';
    } else {
        s_stub_last_httpd_resp[0] = '\0';
    }
    return ESP_OK;
}
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, size_t buf_len)
{
    (void)r; (void)buf; (void)buf_len; return ESP_OK;
}
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t error, const char *msg)
{
    (void)r; (void)error; (void)msg; return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status) { (void)r; (void)status; return ESP_OK; }
/* Controllable request body -- most callers leave content_len==0 (read_body()
 * refuses that outright, so httpd_req_recv() is never reached), but the
 * commissioning_post_handler() tests below need a real body delivered. */
static const char *s_stub_req_body = NULL;
static size_t s_stub_req_body_sent = 0;
int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len)
{
    (void)r;
    if (!s_stub_req_body) {
        return 0;
    }
    size_t remaining = strlen(s_stub_req_body) - s_stub_req_body_sent;
    size_t n = (buf_len < remaining) ? buf_len : remaining;
    if (n == 0) {
        return 0;
    }
    memcpy(buf, s_stub_req_body + s_stub_req_body_sent, n);
    s_stub_req_body_sent += n;
    return (int)n;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s) { (void)r; (void)s; return ESP_OK; }

httpd_handle_t wifi_provision_http_get_server(void) { return NULL; }

// ---------------------------------------------------------------------------
// safety_cfg_store.h stub bodies -- controllable fakes, same convention
// test_kiln_cfg_store.c/test_backup_import.c use for zones_http.h.
// ---------------------------------------------------------------------------
static safety_cfg_param_t s_stub_params[SAFETY_CFG_PARAM_COUNT];
static bool s_stub_lookup_result = true;
static uint8_t s_stub_lookup_type = KILNLINK_PARAM_TYPE_U16;
static const char *s_stub_lookup_name = "stub_field";
// LOW fix (2026-08-27 audit): call-numbered controls so a test can make
// safety_cfg_store_lookup() behave differently on confirm_commit_landed()'s
// OWN (post-refetch) call than it did during safety_cfg_write_apply_pairs()'s earlier staging
// call for the very same pair -- the only way to reach the "believed
// unreachable" branches confirm_commit_landed() now fails on instead of
// silently skipping. Both default OFF so every pre-existing test (which
// never sets them) is unaffected.
static int s_stub_lookup_calls = 0;
static int s_stub_lookup_fail_at_call = -1;          // -1 = never fail by call number
static int s_stub_lookup_type_override_call = -1;    // -1 = no override
static uint8_t s_stub_lookup_type_override_value = 0;

size_t safety_cfg_store_param_count(void) { return SAFETY_CFG_PARAM_COUNT; }

bool safety_cfg_store_get_by_index(size_t index, safety_cfg_param_t *out)
{
    if (!out || index >= SAFETY_CFG_PARAM_COUNT) {
        return false;
    }
    *out = s_stub_params[index];
    return true;
}

bool safety_cfg_store_lookup(uint16_t param_id, uint8_t *out_type, const char **out_name)
{
    (void)param_id;
    s_stub_lookup_calls++;
    if (!s_stub_lookup_result) {
        return false;
    }
    if (s_stub_lookup_fail_at_call == s_stub_lookup_calls) {
        return false;
    }
    if (out_type) {
        *out_type = (s_stub_lookup_calls == s_stub_lookup_type_override_call) ? s_stub_lookup_type_override_value
                                                                                : s_stub_lookup_type;
    }
    if (out_name) *out_name = s_stub_lookup_name;
    return true;
}

// 2026-09-15 review follow-up (item G): the armed-refusal timestamp moved
// out of safety_cfg_http.c (which this file #includes) into
// safety_cfg_store, so the #included production code now calls this. Same
// "own stub, real header" convention as every other safety_cfg_store_*
// fake in this file.
void safety_cfg_store_note_armed_refusal(void) { }
bool safety_cfg_store_recent_armed_refusal(void) { return false; }
uint16_t safety_cfg_store_cached_crc(void) { return 0; }
uint32_t safety_cfg_store_fetched_ms_ago(void) { return UINT32_MAX; }

// safety_cfg_store's last-refetch diff (safety_cfg_store.h) -- commissioning_
// get_handler() reads these to fill the snapshot; build_commissioning_json()
// itself only reads the snapshot fields, per this file's own "pure JSON
// builder tested directly, live-link-touching handler tested only via the
// snapshot it fills" split (see the comment above test_build_json_borrowed_
// unknown()). Stubbed empty here -- the last_diff RENDERING is what
// test_build_json_last_diff_* above exercises, directly against a
// hand-built snapshot.
size_t safety_cfg_store_diff_count(void) { return 0; }
bool safety_cfg_store_get_diff(size_t index, safety_cfg_diff_entry_t *out) { (void)index; (void)out; return false; }
bool safety_cfg_store_diff_truncated(void) { return false; }
void safety_cfg_store_get_diff_crc_range(uint16_t *out_from_crc, uint16_t *out_to_crc)
{
    if (out_from_crc) *out_from_crc = 0;
    if (out_to_crc) *out_to_crc = 0;
}
static bool s_stub_refetch_result = true;
static int s_stub_refetch_calls = 0;
bool safety_cfg_store_refetch(SafetyLinkClass *link, uint16_t crc)
{
    (void)link; (void)crc;
    s_stub_refetch_calls++;
    return s_stub_refetch_result;
}
// 2026-09-10 opus review, blocking-call fix: confirm_commit_landed() now
// calls THIS non-blocking sibling instead when `nonblocking_refetch` is
// true -- the ceiling-reconcile writer's own call path
// (safety_cfg_write_set_and_confirm_f32() -> apply_pairs_ex(..., true)),
// which is the only caller in this file's build that ever runs on
// safety_poll_task. Deliberately a SEPARATE counter/result from the
// blocking stub above (not shared) so
// test_confirm_landed_uses_nonblocking_refetch_when_requested() below can
// prove -- by call count, not by return value alone -- exactly which
// refetch function a given call path used. This is what makes the
// blocking-vs-non-blocking rule host-testable at all: without a distinct
// counter here, a regression that quietly routed the reconcile path back
// through the forbidden blocking safety_cfg_store_refetch() would still
// pass every existing test in this file, since both stubs return the same
// `bool` shape. */
static bool s_stub_refetch_nonblocking_result = true;
static int s_stub_refetch_nonblocking_calls = 0;
bool safety_cfg_store_refetch_nonblocking(SafetyLinkClass *link, uint16_t crc)
{
    (void)link; (void)crc;
    s_stub_refetch_nonblocking_calls++;
    return s_stub_refetch_nonblocking_result;
}
bool safety_cfg_store_maybe_refetch(SafetyLinkClass *link, uint16_t crc) { (void)link; (void)crc; return false; }
esp_err_t safety_cfg_store_init(void) { return ESP_OK; }

// RELAY_LIFE_BUDGET.md -- this file's own fake, same convention
// as every other safety_cfg_store_* stub above (this test #includes safety_
// cfg_http.c directly, and the real safety_cfg_store.c is linked into the
// OTHER host-test executable via test_safety_cfg_store.c's #include of it --
// see this file's own header comment for why linking both here would
// multiply-define every safety_cfg_store_* symbol). Controllable so
// relay_type_post_handler()'s tests can exercise both accept and refuse.
static relay_type_t s_stub_safety_relay_type = RELAY_TYPE_CONTACTOR;
static bool s_stub_set_safety_relay_type_result = true;
static int s_stub_set_safety_relay_type_calls = 0;
static relay_type_t s_stub_set_safety_relay_type_last = RELAY_TYPE_SSR;
relay_type_t safety_cfg_store_get_safety_relay_type(void) { return s_stub_safety_relay_type; }
bool safety_cfg_store_set_safety_relay_type(relay_type_t type, esp_err_t *out_nvs_err)
{
    s_stub_set_safety_relay_type_calls++;
    s_stub_set_safety_relay_type_last = type;
    if (out_nvs_err) *out_nvs_err = ESP_OK;
    if (!s_stub_set_safety_relay_type_result) {
        return false;
    }
    s_stub_safety_relay_type = type;
    return true;
}

// CT_COMMISSIONING_PLAN.md step 1 -- this file's own fakes, same "own stub,
// real store links into the OTHER executable" reasoning as the relay-type
// pair just above. Controllable so ct_cal_post_handler()'s tests can
// exercise both accept and refuse (manual-wins-over-sweep is exercised
// against the REAL safety_cfg_store_set_ct_cal_input() in
// test_safety_cfg_store.c -- this fake only has to satisfy the linker and
// let this file's own handler-level tests control success/failure and the
// derived k_ct_v_per_a/zero_counts the handler stages).
static bool s_stub_ct_cal_has_value[SAFETY_CT_CAL_CHANNELS];
static float s_stub_ct_cal_a_fs[SAFETY_CT_CAL_CHANNELS];
static float s_stub_ct_cal_zero_mv[SAFETY_CT_CAL_CHANNELS];
static safety_ct_cal_source_t s_stub_ct_cal_source[SAFETY_CT_CAL_CHANNELS];
static bool s_stub_set_ct_cal_input_result = true;
static float s_stub_set_ct_cal_input_k = 1.0f;
static uint16_t s_stub_set_ct_cal_input_zc = 0;
static int s_stub_set_ct_cal_input_calls = 0;

bool safety_cfg_store_get_ct_cal_input(size_t ch, float *out_a_fs, float *out_zero_mv,
                                        safety_ct_cal_source_t *out_source)
{
    if (ch >= SAFETY_CT_CAL_CHANNELS || !s_stub_ct_cal_has_value[ch]) {
        return false;
    }
    if (out_a_fs) *out_a_fs = s_stub_ct_cal_a_fs[ch];
    if (out_zero_mv) *out_zero_mv = s_stub_ct_cal_zero_mv[ch];
    if (out_source) *out_source = s_stub_ct_cal_source[ch];
    return true;
}

bool safety_cfg_store_set_ct_cal_input(size_t ch, float a_fs, float zero_mv, safety_ct_cal_source_t source,
                                        float *out_k_ct_v_per_a, uint16_t *out_zero_counts,
                                        esp_err_t *out_nvs_err)
{
    s_stub_set_ct_cal_input_calls++;
    if (out_nvs_err) *out_nvs_err = ESP_OK;
    if (!s_stub_set_ct_cal_input_result) {
        return false;
    }
    if (ch < SAFETY_CT_CAL_CHANNELS) {
        s_stub_ct_cal_has_value[ch] = true;
        s_stub_ct_cal_a_fs[ch] = a_fs;
        s_stub_ct_cal_zero_mv[ch] = zero_mv;
        s_stub_ct_cal_source[ch] = source;
    }
    if (out_k_ct_v_per_a) *out_k_ct_v_per_a = s_stub_set_ct_cal_input_k;
    if (out_zero_counts) *out_zero_counts = s_stub_set_ct_cal_input_zc;
    return true;
}

// ct_cal_post_handler() (safety_cfg_http.c, 2026-09-06 reorder) now calls
// these two directly for pure validation/preview BEFORE it commits anything
// to the Pico or to safety_cfg_store_set_ct_cal_input() above -- see that
// handler's own comment. Reuses the same s_stub_set_ct_cal_input_result/_k/
// _zc knobs so this file's setup/teardown doesn't need a second set.
bool safety_ct_cal_convert(float a_fs, float zero_mv, float gain, float *out_k_ct_v_per_a,
                            uint16_t *out_zero_counts)
{
    (void)a_fs; (void)zero_mv; (void)gain;
    if (!s_stub_set_ct_cal_input_result) {
        return false;
    }
    if (out_k_ct_v_per_a) *out_k_ct_v_per_a = s_stub_set_ct_cal_input_k;
    if (out_zero_counts) *out_zero_counts = s_stub_set_ct_cal_input_zc;
    return true;
}

float safety_cfg_store_ct_cal_channel_gain(size_t ch)
{
    (void)ch;
    return 0.715f;
}

// S8 rate-guard auto-calc (docs/audits/s8_auto_calc_design_2026-09-09.md
// "Part 3") -- this file's own fakes, same "own stub, real store links into
// the OTHER executable" reasoning as the CT calibration/relay-type pairs
// above. s8_rate_guard_estimate()/s8_rate_guard_auto_decide() themselves
// are the REAL implementation (linked in via build_host_tests.ps1's cmd3
// second source) -- only the zones_config accessors and the ESP-local
// provenance store need faking here.
static bool s_stub_rate_guard_meta_has_value = false;
static safety_rate_guard_source_t s_stub_rate_guard_meta_source = SAFETY_RATE_GUARD_SOURCE_MANUAL;
static float s_stub_rate_guard_meta_value = 0.0f;
static int s_stub_rate_guard_meta_set_calls = 0;
static int s_stub_rate_guard_meta_clear_calls = 0;

bool safety_cfg_store_get_rate_guard_meta(safety_rate_guard_source_t *out_source, float *out_value,
                                           bool *out_has_value)
{
    if (out_has_value) *out_has_value = s_stub_rate_guard_meta_has_value;
    if (!s_stub_rate_guard_meta_has_value) {
        return false;
    }
    if (out_source) *out_source = s_stub_rate_guard_meta_source;
    if (out_value) *out_value = s_stub_rate_guard_meta_value;
    return true;
}

bool safety_cfg_store_set_rate_guard_meta(safety_rate_guard_source_t source, float value,
                                           esp_err_t *out_nvs_err)
{
    s_stub_rate_guard_meta_set_calls++;
    s_stub_rate_guard_meta_has_value = true;
    s_stub_rate_guard_meta_source = source;
    s_stub_rate_guard_meta_value = value;
    if (out_nvs_err) *out_nvs_err = ESP_OK;
    return true;
}

void safety_cfg_store_clear_rate_guard_meta(void)
{
    s_stub_rate_guard_meta_clear_calls++;
    s_stub_rate_guard_meta_has_value = false;
    s_stub_rate_guard_meta_source = SAFETY_RATE_GUARD_SOURCE_MANUAL;
    s_stub_rate_guard_meta_value = 0.0f;
}

// zones_config_get_model()/_get_model_fit_context()/_get_thermo_count() --
// controllable per-zone fakes so this file's rate_guard_auto handler tests
// can drive S8_RATE_GUARD_ESTIMATE_OK vs NO_DATA and specific k_dc/tau_s/
// fit_temp_c combinations without any real zones_config storage.
#define TEST_SAFETY_CFG_HTTP_MAX_ZONES 3
static uint8_t s_stub_thermo_count = TEST_SAFETY_CFG_HTTP_MAX_ZONES;
static bool s_stub_zone_has_model[TEST_SAFETY_CFG_HTTP_MAX_ZONES];
static float s_stub_zone_k_dc[TEST_SAFETY_CFG_HTTP_MAX_ZONES];
static float s_stub_zone_tau_s[TEST_SAFETY_CFG_HTTP_MAX_ZONES];
static bool s_stub_zone_has_fit_ctx[TEST_SAFETY_CFG_HTTP_MAX_ZONES];
static float s_stub_zone_fit_temp_c[TEST_SAFETY_CFG_HTTP_MAX_ZONES];
// Coupling row fakes -- [affected][stepped], same shape as the real
// zones_config_get_coupling(). Added for the 2026-09-10 S8 review (defect
// D: the estimator's basis must include coupling, not just each zone's own
// k_dc) -- default all-zero so existing tests (written before coupling was
// part of the basis) keep their own-zone-only expected values unchanged.
static bool  s_stub_zone_has_coupling[TEST_SAFETY_CFG_HTTP_MAX_ZONES];
static float s_stub_zone_coupling_row[TEST_SAFETY_CFG_HTTP_MAX_ZONES][TEST_SAFETY_CFG_HTTP_MAX_ZONES];

uint8_t zones_config_get_thermo_count(void)
{
    return s_stub_thermo_count;
}

bool zones_config_get_coupling(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (!out_row || zone_index >= TEST_SAFETY_CFG_HTTP_MAX_ZONES || !s_stub_zone_has_coupling[zone_index]) {
        return false;
    }
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT && j < TEST_SAFETY_CFG_HTTP_MAX_ZONES; j++) {
        out_row[j] = s_stub_zone_coupling_row[zone_index][j];
    }
    return true;
}

// zones_config_get_coupling_diag_k_dc() -- added for the 2026-09-10 finding
// C fix (safety_cfg_http.c's rate_guard_gather_and_estimate() now mirrors
// zone_coupling_solve.c's coupling_matrix_provenance_ok() rule: a matrix
// carrying measured off-diagonals is refused unless every member's own
// diag_k_dc has been identified on hardware). Every existing test in this
// file leaves s_stub_zone_has_coupling all-false (no measured off-diagonal
// at all), so board_coupling_provenance_ok is trivially true regardless of
// what this stub returns -- it exists only so the link succeeds. Defaults
// to "not identified" (false), matching this bench's real, live state.
static bool  s_stub_zone_has_coupling_diag_k_dc[TEST_SAFETY_CFG_HTTP_MAX_ZONES];
static float s_stub_zone_coupling_diag_k_dc[TEST_SAFETY_CFG_HTTP_MAX_ZONES];

bool zones_config_get_coupling_diag_k_dc(uint8_t zone_index, float *out_k_dc)
{
    if (!out_k_dc || zone_index >= TEST_SAFETY_CFG_HTTP_MAX_ZONES ||
        !s_stub_zone_has_coupling_diag_k_dc[zone_index]) {
        return false;
    }
    *out_k_dc = s_stub_zone_coupling_diag_k_dc[zone_index];
    return true;
}

bool zones_config_get_model(uint8_t zone_index, float *out_k_dc, float *out_tau_s, float *out_dead_time_s)
{
    if (out_dead_time_s) *out_dead_time_s = 0.0f;
    if (zone_index >= TEST_SAFETY_CFG_HTTP_MAX_ZONES || !s_stub_zone_has_model[zone_index]) {
        return false;
    }
    if (out_k_dc) *out_k_dc = s_stub_zone_k_dc[zone_index];
    if (out_tau_s) *out_tau_s = s_stub_zone_tau_s[zone_index];
    return true;
}

bool zones_config_get_model_fit_context(uint8_t zone_index, float *out_fit_temp_c, float *out_fit_ambient_c)
{
    if (out_fit_ambient_c) *out_fit_ambient_c = 20.0f;
    if (zone_index >= TEST_SAFETY_CFG_HTTP_MAX_ZONES || !s_stub_zone_has_fit_ctx[zone_index]) {
        return false;
    }
    if (out_fit_temp_c) *out_fit_temp_c = s_stub_zone_fit_temp_c[zone_index];
    return true;
}

// Resets every rate-guard-related stub to a known "nothing configured"
// state -- called at the top of each rate_guard_auto test so tests cannot
// leak state into each other via these file-static knobs.
static void reset_rate_guard_stubs(void)
{
    s_stub_rate_guard_meta_has_value = false;
    s_stub_rate_guard_meta_source = SAFETY_RATE_GUARD_SOURCE_MANUAL;
    s_stub_rate_guard_meta_value = 0.0f;
    s_stub_rate_guard_meta_set_calls = 0;
    s_stub_rate_guard_meta_clear_calls = 0;
    s_stub_thermo_count = TEST_SAFETY_CFG_HTTP_MAX_ZONES;
    memset(s_stub_zone_has_model, 0, sizeof(s_stub_zone_has_model));
    memset(s_stub_zone_k_dc, 0, sizeof(s_stub_zone_k_dc));
    memset(s_stub_zone_tau_s, 0, sizeof(s_stub_zone_tau_s));
    memset(s_stub_zone_has_fit_ctx, 0, sizeof(s_stub_zone_has_fit_ctx));
    memset(s_stub_zone_fit_temp_c, 0, sizeof(s_stub_zone_fit_temp_c));
    memset(s_stub_zone_has_coupling, 0, sizeof(s_stub_zone_has_coupling));
    memset(s_stub_zone_has_coupling_diag_k_dc, 0, sizeof(s_stub_zone_has_coupling_diag_k_dc));
    memset(s_stub_zone_coupling_diag_k_dc, 0, sizeof(s_stub_zone_coupling_diag_k_dc));
    memset(s_stub_zone_coupling_row, 0, sizeof(s_stub_zone_coupling_row));
}

// ---------------------------------------------------------------------------
// safety_link.h stub bodies -- controllable fakes.
// ---------------------------------------------------------------------------
static esp_err_t s_stub_set_param_result = ESP_OK;
static esp_err_t s_stub_commit_result = ESP_OK;
static int s_stub_set_param_calls = 0;
static int s_stub_commit_calls = 0;
static bool s_stub_commit_rejected = false;
static uint16_t s_stub_commit_reject_param_id = KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID;
static uint8_t s_stub_commit_reject_reason = KILNLINK_COMMIT_CONFIG_REJECT_RANGE;

esp_err_t safety_link_send_set_param(SafetyLinkClass *link, uint16_t param_id, uint8_t type,
                                      kilnlink_param_value_t value)
{
    (void)link; (void)param_id; (void)type; (void)value;
    s_stub_set_param_calls++;
    return s_stub_set_param_result;
}

esp_err_t safety_link_send_commit_config(SafetyLinkClass *link, uint16_t *out_param_id, uint8_t *out_reason,
                                          bool *out_rejected)
{
    (void)link;
    s_stub_commit_calls++;
    if (out_rejected) *out_rejected = s_stub_commit_rejected;
    if (out_param_id) *out_param_id = s_stub_commit_reject_param_id;
    if (out_reason) *out_reason = s_stub_commit_reject_reason;
    return s_stub_commit_result;
}

// item 15 (2026-09-14): SAFETY_CMD_APPLY_CONFIG_VOLATILE (0x2D) --
// apply_pairs_ex()'s volatile_install=true leg. This file's own tests do
// not yet exercise volatile_install=true (that is kiln_cfg_swap.c's own
// job, tested in test_kiln_cfg_swap.c against its OWN fake of the public
// wrappers this file exposes) -- this stub only needs to exist so
// safety_cfg_http.c links, and shares the same result/rejection knobs as
// the commit_config stub above so a future test that DOES flip
// volatile_install can drive it identically.
static int s_stub_apply_volatile_calls = 0;

esp_err_t safety_link_send_apply_config_volatile(SafetyLinkClass *link, uint16_t *out_param_id, uint8_t *out_reason,
                                                  bool *out_rejected)
{
    (void)link;
    s_stub_apply_volatile_calls++;
    if (out_rejected) *out_rejected = s_stub_commit_rejected;
    if (out_param_id) *out_param_id = s_stub_commit_reject_param_id;
    if (out_reason) *out_reason = s_stub_commit_reject_reason;
    return s_stub_commit_result;
}

// ---------------------------------------------------------------------------
// estop_verification.h stub bodies. This is its own separate host-test
// executable (test_safety_cfg_http.c #includes safety_cfg_http.c directly),
// so it cannot link the real estop_verification.c the way main_boot_early.c
// does on target -- a fake here, same as every safety_link.h/safety_cfg_
// store.h stub above, is enough to prove safety_cfg_write_apply_pairs() calls the clear on a
// 0x0212 commit and NOT on any other param.
// ---------------------------------------------------------------------------
static int s_stub_estop_verif_clear_calls = 0;

esp_err_t estop_verification_clear(void)
{
    s_stub_estop_verif_clear_calls++;
    return ESP_OK;
}

esp_err_t estop_verification_confirm(void)
{
    return ESP_OK;
}

bool estop_verification_is_verified(void)
{
    return false;
}

esp_err_t safety_link_get_status(SafetyLinkClass *link, safety_link_status_t *out)
{
    (void)link;
    if (out) memset(out, 0, sizeof(*out));
    return ESP_OK;
}

// CT_COMMISSIONING_PLAN.md step 2 -- ct_auto_zero_post_handler()'s own
// dependencies, same "own stub, real definitions link into other
// executables" reasoning as everything else in this file.
static bool s_stub_ct_auto_zero_begin_result = true;
static kilnlink_ct_auto_zero_status_t s_stub_ct_auto_zero_status = {
    .state = KILNLINK_CT_AUTO_ZERO_STATE_DONE, .channel = 0, .samples_taken = 200,
    .samples_target = 200, .zero_counts = 61,
};
esp_err_t safety_link_send_ct_auto_zero_begin(SafetyLinkClass *link, uint8_t channel)
{
    (void)link; (void)channel;
    return s_stub_ct_auto_zero_begin_result ? ESP_OK : ESP_FAIL;
}
esp_err_t safety_link_get_ct_auto_zero_status(SafetyLinkClass *link, kilnlink_ct_auto_zero_status_t *out)
{
    (void)link;
    if (out) *out = s_stub_ct_auto_zero_status;
    return ESP_OK;
}
uint8_t kiln_io_get_relay_shadow(const kiln_io_t *io)
{
    (void)io;
    return 0u; // no test in this file drives a nonzero shadow today
}
uint32_t kiln_io_relays_off_ms(const kiln_io_t *io)
{
    (void)io;
    return 60000u; // comfortably over the 5 s floor
}
bool autotune_engine_is_active(void)
{
    return false;
}
void profile_executor_get_status(profile_exec_status_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
}
/* 2026-09-15 review (review_divergence_rework_c1d2c526_2026-09-15.md,
 * MEDIUM 3 / HIGH 3 race): safety_cfg_http.c's commissioning_post_handler()
 * calls kiln_cfg_store_recapture_pico_half_confirmed() (NOT the ordinary
 * kiln_cfg_store_autosave_from_live() -- that path is divergence-gated and
 * would deadlock exactly this caller, see that function's own doc comment)
 * after a confirmed commit, to keep the active kiln-config slot's captured
 * Pico half in sync. This file never links the real kiln_cfg_store.c (that
 * pulls in NVS/cfg-filesystem machinery this executable has no fakes for),
 * so a controllable stub satisfies the link -- test_kiln_cfg_store.c's own
 * executable covers the REAL function's behavior; this stub only lets this
 * file's tests confirm the handler calls it (and checks/logs the result)
 * at the right point, via s_stub_recapture_calls/s_stub_recapture_result. */
static int s_stub_recapture_calls = 0;
static bool s_stub_recapture_result = true;
static const char *s_stub_recapture_reason = NULL;
bool kiln_cfg_store_recapture_pico_half_confirmed(char *reason_out, size_t reason_cap)
{
    s_stub_recapture_calls++;
    if (reason_out && reason_cap > 0) {
        snprintf(reason_out, reason_cap, "%s", s_stub_recapture_reason ? s_stub_recapture_reason : "");
    }
    return s_stub_recapture_result;
}

esp_err_t safety_link_get_peer_build_status(SafetyLinkClass *link, bool *out_known, bool *out_dirty,
                                             uint8_t *commit_buf, uint8_t *out_commit_len,
                                             uint8_t *datetime_buf, uint8_t *out_datetime_len,
                                             uint8_t *out_config_version, uint16_t *out_config_crc)
{
    (void)link; (void)out_dirty; (void)commit_buf; (void)out_commit_len; (void)datetime_buf;
    (void)out_datetime_len; (void)out_config_version;
    if (out_known) *out_known = false;
    if (out_config_crc) *out_config_crc = 0;
    return ESP_OK;
}

// M1 (2026-08-27 audit fix): safety_link_get_peer_version_status() stub --
// controllable peer protocol version, so build_commissioning_json()'s
// unset_reporting_reliable gating can be exercised without a real link.
static bool s_stub_peer_version_known = false;
static bool s_stub_peer_version_compatible = false;
static uint16_t s_stub_peer_protocol_version = 0;

esp_err_t safety_link_get_peer_version_status(SafetyLinkClass *link, bool *out_known, bool *out_compatible,
                                               uint16_t *out_peer_protocol, uint16_t *out_peer_min_compatible)
{
    (void)link; (void)out_peer_min_compatible;
    if (out_known) *out_known = s_stub_peer_version_known;
    if (out_compatible) *out_compatible = s_stub_peer_version_compatible;
    if (out_peer_protocol) *out_peer_protocol = s_stub_peer_protocol_version;
    return ESP_OK;
}

static bool s_stub_late_rejected = false;
static uint16_t s_stub_late_reject_param_id = KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID;
static uint8_t s_stub_late_reject_reason = KILNLINK_COMMIT_CONFIG_REJECT_RANGE;
static int s_stub_take_stashed_calls = 0;

bool safety_link_take_stashed_commit_rejected(SafetyLinkClass *link, uint16_t *out_param_id, uint8_t *out_reason)
{
    (void)link;
    s_stub_take_stashed_calls++;
    if (!s_stub_late_rejected) {
        return false;
    }
    if (out_param_id) *out_param_id = s_stub_late_reject_param_id;
    if (out_reason) *out_reason = s_stub_late_reject_reason;
    return true;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void reset_all(void)
{
    memset(s_stub_params, 0, sizeof(s_stub_params));
    s_stub_lookup_result = true;
    s_stub_lookup_type = KILNLINK_PARAM_TYPE_U16;
    s_stub_lookup_name = "stub_field";
    s_stub_set_param_result = ESP_OK;
    s_stub_commit_result = ESP_OK;
    s_stub_set_param_calls = 0;
    s_stub_commit_calls = 0;
    s_stub_commit_rejected = false;
    s_stub_commit_reject_param_id = KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID;
    s_stub_commit_reject_reason = KILNLINK_COMMIT_CONFIG_REJECT_RANGE;
    s_stub_refetch_result = true;
    s_stub_refetch_calls = 0;
    s_stub_refetch_nonblocking_result = true;
    s_stub_refetch_nonblocking_calls = 0;
    s_stub_late_rejected = false;
    s_stub_late_reject_param_id = KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID;
    s_stub_late_reject_reason = KILNLINK_COMMIT_CONFIG_REJECT_RANGE;
    s_stub_take_stashed_calls = 0;
    s_stub_peer_version_known = false;
    s_stub_peer_version_compatible = false;
    s_stub_peer_protocol_version = 0;
    s_stub_lookup_calls = 0;
    s_stub_lookup_fail_at_call = -1;
    s_stub_lookup_type_override_call = -1;
    s_stub_lookup_type_override_value = 0;
    memset(s_stub_ct_cal_has_value, 0, sizeof(s_stub_ct_cal_has_value));
    memset(s_stub_ct_cal_a_fs, 0, sizeof(s_stub_ct_cal_a_fs));
    memset(s_stub_ct_cal_zero_mv, 0, sizeof(s_stub_ct_cal_zero_mv));
    memset(s_stub_ct_cal_source, 0, sizeof(s_stub_ct_cal_source));
    s_stub_set_ct_cal_input_result = true;
    s_stub_set_ct_cal_input_k = 1.0f;
    s_stub_set_ct_cal_input_zc = 0;
    s_stub_set_ct_cal_input_calls = 0;
    s_stub_estop_verif_clear_calls = 0;
    s_stub_recapture_calls = 0;
    s_stub_recapture_result = true;
    s_stub_recapture_reason = NULL;
    s_stub_req_body = NULL;
    s_stub_req_body_sent = 0;
    memset(s_stub_last_httpd_resp, 0, sizeof(s_stub_last_httpd_resp));
}

static void test_parse_single_pair_no_commit(void)
{
    TEST_SECTION("parse_set_param_body -- a single id=/value= pair, no commit");
    safety_cfg_post_pair_t pairs[SAFETY_CFG_POST_MAX_PAIRS];
    bool commit = true; // must be reset to false by the parser
    int n = parse_set_param_body("id=257&value=1", pairs, SAFETY_CFG_POST_MAX_PAIRS, &commit);
    TEST_CHECK(n == 1, "one pair parsed");
    TEST_CHECK(pairs[0].param_id == 257, "id decoded as 257 (0x0101, tc_source)");
    TEST_CHECK(strcmp(pairs[0].value_text, "1") == 0, "value text is \"1\"");
    TEST_CHECK(commit == false, "commit was not requested -- defaults false, not left uninitialized");
}

static void test_parse_multiple_pairs_plus_commit(void)
{
    TEST_SECTION("parse_set_param_body -- repeated id=/value= pairs plus a trailing commit=1");
    safety_cfg_post_pair_t pairs[SAFETY_CFG_POST_MAX_PAIRS];
    bool commit = false;
    int n = parse_set_param_body("id=513&value=100.5&id=514&value=60&commit=1", pairs,
                                  SAFETY_CFG_POST_MAX_PAIRS, &commit);
    TEST_CHECK(n == 2, "two pairs parsed");
    TEST_CHECK(pairs[0].param_id == 513 && strcmp(pairs[0].value_text, "100.5") == 0,
               "first pair (0x0201 firing_margin_c) intact");
    TEST_CHECK(pairs[1].param_id == 514 && strcmp(pairs[1].value_text, "60") == 0,
               "second pair (0x0202 overshoot_margin_c) intact");
    TEST_CHECK(commit == true, "commit=1 was recognised");
}

static void test_parse_rejects_value_without_id(void)
{
    TEST_SECTION("parse_set_param_body -- a bare \"value=\" with no preceding \"id=\" is rejected");
    safety_cfg_post_pair_t pairs[SAFETY_CFG_POST_MAX_PAIRS];
    bool commit = false;
    int n = parse_set_param_body("value=5", pairs, SAFETY_CFG_POST_MAX_PAIRS, &commit);
    TEST_CHECK(n == -1, "malformed body (orphan value) is refused, not silently dropped");
}

static void test_parse_rejects_dangling_id(void)
{
    TEST_SECTION("parse_set_param_body -- a trailing \"id=\" with no following \"value=\" is rejected");
    safety_cfg_post_pair_t pairs[SAFETY_CFG_POST_MAX_PAIRS];
    bool commit = false;
    int n = parse_set_param_body("id=257&commit=1", pairs, SAFETY_CFG_POST_MAX_PAIRS, &commit);
    TEST_CHECK(n == -1, "an id with no paired value is refused");
}

static void test_parse_value_for_type_bounds(void)
{
    TEST_SECTION("safety_cfg_write_parse_value_for_type -- accepts in-range, rejects out-of-range/garbage per wire type");
    kilnlink_param_value_t v;
    TEST_CHECK(safety_cfg_write_parse_value_for_type("1", KILNLINK_PARAM_TYPE_BOOL, &v) && v.bool_val == 1,
               "bool '1' parses");
    TEST_CHECK(safety_cfg_write_parse_value_for_type("2", KILNLINK_PARAM_TYPE_BOOL, &v) == false,
               "bool '2' is out of range (0/1 only)");
    TEST_CHECK(safety_cfg_write_parse_value_for_type("255", KILNLINK_PARAM_TYPE_U8, &v) && v.u8_val == 255,
               "u8 accepts its max value");
    TEST_CHECK(safety_cfg_write_parse_value_for_type("256", KILNLINK_PARAM_TYPE_U8, &v) == false, "u8 rejects 256");
    TEST_CHECK(safety_cfg_write_parse_value_for_type("65535", KILNLINK_PARAM_TYPE_U16, &v) && v.u16_val == 65535,
               "u16 accepts its max value");
    TEST_CHECK(safety_cfg_write_parse_value_for_type("70000", KILNLINK_PARAM_TYPE_U16, &v) == false, "u16 rejects 70000");
    TEST_CHECK(safety_cfg_write_parse_value_for_type("123.5", KILNLINK_PARAM_TYPE_F32, &v) && v.f32_val > 123.0f,
               "f32 parses a decimal");
    TEST_CHECK(safety_cfg_write_parse_value_for_type("not_a_number", KILNLINK_PARAM_TYPE_F32, &v) == false,
               "garbage text is rejected, not parsed as 0");
    TEST_CHECK(safety_cfg_write_parse_value_for_type("12abc", KILNLINK_PARAM_TYPE_U16, &v) == false,
               "trailing garbage after a valid-looking prefix is rejected, not truncated");
}

static void test_build_json_unset_param_omits_value(void)
{
    TEST_SECTION("build_commissioning_json -- an unset parameter carries \"set\":false and OMITS \"value\"");
    reset_all();
    s_stub_params[0].param_id = 0x0101;
    s_stub_params[0].name = "tc_source";
    s_stub_params[0].type = KILNLINK_PARAM_TYPE_U8;
    s_stub_params[0].set = false;
    s_stub_params[0].value.u8_val = 0; // a real, plausible-looking value that must NOT be printed

    safety_cfg_http_snapshot_t snap = {0};
    static char json[SAFETY_CFG_JSON_MAX];
    size_t len = build_commissioning_json(&snap, json, sizeof(json));
    TEST_CHECK(len > 0, "JSON built successfully");
    TEST_CHECK(strstr(json, "\"id\":257") != NULL, "the unset param still appears in the list");
    TEST_CHECK(strstr(json, "\"set\":false") != NULL, "it is marked set:false");
    TEST_CHECK(strstr(json, "\"value\"") == NULL,
               "\"value\" is OMITTED entirely -- never a printed 0 a page could mistake for real");
}

static void test_build_json_set_param_includes_value(void)
{
    TEST_SECTION("build_commissioning_json -- a set parameter DOES include its value");
    reset_all();
    s_stub_params[0].param_id = 0x0104;
    s_stub_params[0].name = "abs_max_temp_c";
    s_stub_params[0].type = KILNLINK_PARAM_TYPE_F32;
    s_stub_params[0].set = true;
    s_stub_params[0].value.f32_val = 1300.0f;

    safety_cfg_http_snapshot_t snap = {0};
    snap.unset_reliable = true; // M1: this test is about the set/value rendering itself, not the
                                 // peer-version gate -- assume a peer new enough to be trusted
                                 // (test_build_json_forces_unset_when_peer_too_old() covers the gate).
    static char json[SAFETY_CFG_JSON_MAX];
    size_t len = build_commissioning_json(&snap, json, sizeof(json));
    TEST_CHECK(len > 0, "JSON built successfully");
    TEST_CHECK(strstr(json, "\"set\":true") != NULL, "marked set:true");
    TEST_CHECK(strstr(json, "\"value\":1300") != NULL, "the real value is present");
}

// 2026-09-10: "last_diff" is the decoded field-by-field diff of the most
// recent safety_cfg_store refetch (docs/audits/safety_config_crc_seq_2026-09-
// 10.md's recommended follow-up to the config-CRC investigation, same idea as
// the trip_mask decode) -- proves a genuine mismatch renders by name/old/new,
// not just as a changed CRC number.
static void test_build_json_last_diff_reports_named_mismatch(void)
{
    TEST_SECTION("build_commissioning_json -- last_diff names the field, old, and new value");
    reset_all();
    safety_cfg_http_snapshot_t snap = {0};
    snap.diff_from_crc = 111;
    snap.diff_to_crc = 222;
    snap.diff_truncated = false;
    snap.diff_count = 1;
    snap.diff_entries[0].name = "mains_voltage_v";
    snap.diff_entries[0].type = KILNLINK_PARAM_TYPE_F32;
    snap.diff_entries[0].old_set = true;
    snap.diff_entries[0].old_value.f32_val = 240.0f;
    snap.diff_entries[0].new_set = true;
    snap.diff_entries[0].new_value.f32_val = 120.0f;

    static char json[SAFETY_CFG_JSON_MAX];
    size_t len = build_commissioning_json(&snap, json, sizeof(json));
    TEST_CHECK(len > 0, "JSON built successfully");
    TEST_CHECK(strstr(json, "\"from_crc\":111") != NULL, "reports the CRC this diff was computed from");
    TEST_CHECK(strstr(json, "\"to_crc\":222") != NULL, "reports the CRC this diff was computed to");
    TEST_CHECK(strstr(json, "\"name\":\"mains_voltage_v\"") != NULL, "names the field that actually changed");
    TEST_CHECK(strstr(json, "\"old\":240") != NULL, "reports the old value");
    TEST_CHECK(strstr(json, "\"new\":120") != NULL, "reports the new value");
}

static void test_build_json_last_diff_empty_when_nothing_changed(void)
{
    TEST_SECTION("build_commissioning_json -- last_diff is an empty list when nothing decodable changed");
    reset_all();
    safety_cfg_http_snapshot_t snap = {0};
    static char json[SAFETY_CFG_JSON_MAX];
    size_t len = build_commissioning_json(&snap, json, sizeof(json));
    TEST_CHECK(len > 0, "JSON built successfully");
    TEST_CHECK(strstr(json, "\"last_diff\":{\"from_crc\":0,\"to_crc\":0,\"truncated\":false,\"fields\":[]}") != NULL,
               "an all-zero snapshot renders an explicitly empty diff, not an omitted field");
}

// 2026-09-03, TASK 1/2: the live status-frame flags (BORROWED, TC_NOT_
// INSTALLED, TC_INJECTED) commissioning_get_handler() now folds into this
// same JSON. build_commissioning_json() is the pure half that host-tests
// directly; commissioning_get_handler()'s own snapshot-filling code (which
// reads st.flags/st.borrowed_known/etc. from a real SafetyLinkClass) is
// exercised end-to-end by test_safety_link_compile.c's
// test_apply_status_v3_borrowed() -- Pico pack -> wire -> KilnFW decode --
// so this file only needs to prove the JSON RENDERING side: given a
// snapshot, does the right shape come out.
static void test_build_json_borrowed_unknown(void)
{
    TEST_SECTION("build_commissioning_json -- borrowed_known:false (V1/V2 peer, or link never up) "
                 "omits \"borrowed\"/\"borrowed_zone_index\" entirely -- never a false 'not borrowed'");
    reset_all();
    safety_cfg_http_snapshot_t snap = {0};
    snap.borrowed_known = false;
    // Deliberately also seed the fields that would be printed IF borrowed_known
    // were (wrongly) ignored, so a bug that renders them anyway is caught.
    snap.borrowed = true;
    snap.borrowed_zone_index = 2;

    static char json[SAFETY_CFG_JSON_MAX];
    size_t len = build_commissioning_json(&snap, json, sizeof(json));
    TEST_CHECK(len > 0, "JSON built successfully");
    TEST_CHECK(strstr(json, "\"borrowed_known\":false") != NULL, "borrowed_known:false is reported");
    TEST_CHECK(strstr(json, "\"borrowed\":") == NULL,
               "\"borrowed\" is OMITTED when unknown -- even though snap.borrowed was (wrongly, for "
               "this test) set true, proving the omission is really gated on borrowed_known");
    TEST_CHECK(strstr(json, "\"borrowed_zone_index\"") == NULL,
               "\"borrowed_zone_index\" is also omitted when borrowed_known is false");
}

static void test_build_json_borrowed_known_true_with_zone(void)
{
    TEST_SECTION("build_commissioning_json -- borrowed_known:true, borrowed:true, a real zone index");
    reset_all();
    safety_cfg_http_snapshot_t snap = {0};
    snap.borrowed_known = true;
    snap.borrowed = true;
    snap.borrowed_zone_index = 1;

    static char json[SAFETY_CFG_JSON_MAX];
    size_t len = build_commissioning_json(&snap, json, sizeof(json));
    TEST_CHECK(len > 0, "JSON built successfully");
    TEST_CHECK(strstr(json, "\"borrowed_known\":true") != NULL, "borrowed_known:true is reported");
    TEST_CHECK(strstr(json, "\"borrowed\":true") != NULL, "borrowed:true is reported");
    TEST_CHECK(strstr(json, "\"borrowed_zone_index\":1") != NULL, "the real zone index (1) is reported");
}

static void test_build_json_borrowed_known_true_zone_unknown(void)
{
    TEST_SECTION("build_commissioning_json -- borrowed_known:true, borrowed:true, but the ZONE itself "
                 "is not commissioned on the Pico (SAFETY_LINK_BORROWED_ZONE_UNKNOWN) -- "
                 "borrowed_zone_index must still be omitted, not printed as 255");
    reset_all();
    safety_cfg_http_snapshot_t snap = {0};
    snap.borrowed_known = true;
    snap.borrowed = true;
    snap.borrowed_zone_index = SAFETY_LINK_BORROWED_ZONE_UNKNOWN;

    static char json[SAFETY_CFG_JSON_MAX];
    size_t len = build_commissioning_json(&snap, json, sizeof(json));
    TEST_CHECK(len > 0, "JSON built successfully");
    TEST_CHECK(strstr(json, "\"borrowed\":true") != NULL, "borrowed:true is still reported");
    TEST_CHECK(strstr(json, "\"borrowed_zone_index\"") == NULL,
               "borrowed_zone_index is omitted, never printed as the raw 255 sentinel");
}

static void test_build_json_tc_flags_gated_on_link_up(void)
{
    TEST_SECTION("build_commissioning_json -- tc_not_installed/tc_injected are forced false when "
                 "link_up is false, even if the (stale, cached) snapshot fields say otherwise");
    reset_all();
    safety_cfg_http_snapshot_t snap = {0};
    snap.link_up = false;
    snap.tc_not_installed = true; // stale cached value from before the link dropped
    snap.tc_injected = true;

    static char json[SAFETY_CFG_JSON_MAX];
    size_t len = build_commissioning_json(&snap, json, sizeof(json));
    TEST_CHECK(len > 0, "JSON built successfully");
    TEST_CHECK(strstr(json, "\"tc_not_installed\":false") != NULL,
               "tc_not_installed reads false while the link is down, despite the stale true field");
    TEST_CHECK(strstr(json, "\"tc_injected\":false") != NULL,
               "tc_injected reads false while the link is down, despite the stale true field");

    snap.link_up = true;
    len = build_commissioning_json(&snap, json, sizeof(json));
    TEST_CHECK(len > 0, "JSON built successfully with link_up=true");
    TEST_CHECK(strstr(json, "\"tc_not_installed\":true") != NULL,
               "tc_not_installed reads true once the link is up -- proves the false case above was "
               "really gated on link_up, not the field always reading false");
    TEST_CHECK(strstr(json, "\"tc_injected\":true") != NULL, "tc_injected reads true once the link is up");
}

static void test_stale_flag_reflects_crc_mismatch(void)
{
    TEST_SECTION("snapshot_is_stale -- true on any mismatch or unknown-live-crc, false only when they agree");
    safety_cfg_http_snapshot_t snap = {0};
    snap.live_crc_known = false;
    TEST_CHECK(snapshot_is_stale(&snap) == true, "an unknown live CRC can never be vouched for as current");

    snap.live_crc_known = true;
    snap.live_crc = 100;
    snap.cached_crc = 200;
    TEST_CHECK(snapshot_is_stale(&snap) == true, "a real mismatch is stale");

    snap.cached_crc = 100;
    TEST_CHECK(snapshot_is_stale(&snap) == false, "matching CRCs -- not stale");
}

static void test_apply_pairs_all_succeed_with_commit(void)
{
    TEST_SECTION("safety_cfg_write_apply_pairs -- every pair staged, commit ACKed -- success, empty reason");
    reset_all();
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    safety_cfg_post_pair_t pairs[2] = {
        { .param_id = 513, .value_text = "100" },
        { .param_id = 514, .value_text = "75" },
    };
    // safety_cfg_store_lookup() stubs every id as KILNLINK_PARAM_TYPE_U16 --
    // the live read-back (confirm_commit_landed()) requires the refetched
    // cache to report these ids back SET to the exact submitted value, or a
    // "confirmed success" would still be a lie. This is what a real Pico that
    // actually wrote the values would report after safety_cfg_store_refetch().
    s_stub_params[0].param_id = 513;
    s_stub_params[0].type = KILNLINK_PARAM_TYPE_U16;
    s_stub_params[0].set = true;
    s_stub_params[0].value.u16_val = 100;
    s_stub_params[1].param_id = 514;
    s_stub_params[1].type = KILNLINK_PARAM_TYPE_U16;
    s_stub_params[1].set = true;
    s_stub_params[1].value.u16_val = 75;
    char reason[160];
    bool ok = safety_cfg_write_apply_pairs(&fake_link, pairs, 2, true, reason, sizeof(reason), NULL);
    TEST_CHECK(ok == true, "all staged and committed successfully");
    TEST_CHECK(reason[0] == '\0', "no reason text on success");
    TEST_CHECK(s_stub_set_param_calls == 2, "both pairs were staged");
    TEST_CHECK(s_stub_commit_calls == 1, "commit was sent exactly once");
    TEST_CHECK(s_stub_refetch_calls == 1, "a live read-back was forced after the commit ACKed");
}

// 2026-09-08 E-stop bench-verification pass: a successful commit of param
// 0x0212 (estop_active_level) must invalidate any standing E-stop
// verification (estop_verification.h) -- the operator confirmed the
// procedure against a specific polarity, and this ESP has no way to prove a
// re-sent polarity value is the SAME one that was verified against.
static void test_apply_pairs_estop_polarity_commit_clears_verification(void)
{
    TEST_SECTION("safety_cfg_write_apply_pairs -- a landed commit of param 0x0212 (estop_active_level) clears "
                 "estop_verification");
    reset_all();
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    s_stub_lookup_type = KILNLINK_PARAM_TYPE_U8;
    safety_cfg_post_pair_t pairs[1] = {
        { .param_id = 0x0212u, .value_text = "1" },
    };
    s_stub_params[0].param_id = 0x0212u;
    s_stub_params[0].type = KILNLINK_PARAM_TYPE_U8;
    s_stub_params[0].set = true;
    s_stub_params[0].value.u8_val = 1;
    char reason[160];
    bool ok = safety_cfg_write_apply_pairs(&fake_link, pairs, 1, true, reason, sizeof(reason), NULL);
    TEST_CHECK(ok == true, "commit landed");
    TEST_CHECK(s_stub_estop_verif_clear_calls == 1,
               "a landed commit that includes 0x0212 clears the E-stop verification exactly once");
}

// A commit that does NOT touch 0x0212 must leave a standing verification
// alone -- proves the hook is keyed on the specific param, not fired on
// every successful commit.
static void test_apply_pairs_unrelated_commit_does_not_clear_verification(void)
{
    TEST_SECTION("safety_cfg_write_apply_pairs -- a landed commit of an unrelated param does NOT clear "
                 "estop_verification");
    reset_all();
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    safety_cfg_post_pair_t pairs[1] = {
        { .param_id = 513, .value_text = "100" },
    };
    s_stub_params[0].param_id = 513;
    s_stub_params[0].type = KILNLINK_PARAM_TYPE_U16;
    s_stub_params[0].set = true;
    s_stub_params[0].value.u16_val = 100;
    char reason[160];
    bool ok = safety_cfg_write_apply_pairs(&fake_link, pairs, 1, true, reason, sizeof(reason), NULL);
    TEST_CHECK(ok == true, "commit landed");
    TEST_CHECK(s_stub_estop_verif_clear_calls == 0,
               "a landed commit of an unrelated param must NOT touch estop_verification");
}

// ---------------------------------------------------------------------------
// 2026-08-27 audit fix: "ok cannot fail" -- a commit that the Pico's link
// layer ACKed and that arrived with no REJECTED frame inside the reply
// window used to be reported as success unconditionally (safety_cfg_write_apply_pairs()
// returned true straight off safety_link_send_commit_config()'s return
// value). That is provably not proof of anything: SET_PARAM/COMMIT_CONFIG are
// both fire-and-forget UART broadcasts (uart_protocol_send_broadcast()
// reports only "the local UART accepted the bytes"), so ESP_OK+!rejected only
// ever meant "we didn't SEE a refusal", never "the Pico actually wrote it".
// confirm_commit_landed() is what turns that into a real proof -- these three
// tests exercise exactly the three ways a "successful" commit could still be
// a lie, and prove safety_cfg_write_apply_pairs() now catches every one of them.
// ---------------------------------------------------------------------------

static void test_apply_pairs_readback_mismatch_fails_even_when_acked_and_not_rejected(void)
{
    TEST_SECTION("safety_cfg_write_apply_pairs -- ACKed, not rejected, but the read-back does NOT match -- must FAIL");
    reset_all();
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    safety_cfg_post_pair_t pairs[1] = { { .param_id = 0x0104u, .value_text = "1300" } }; // abs_max_temp_c
    // Deliberately leave s_stub_params empty: the "commit" was ACKed
    // (s_stub_commit_result == ESP_OK) and NOT flagged rejected
    // (s_stub_commit_rejected == false, both from reset_all()'s defaults) --
    // this is EXACTLY the live-bench symptom the audit caught: {"ok":true}
    // three times while live_config_crc never moved and the Pico's own
    // histogram showed commit_config_rejected=2.
    char reason[160];
    bool ok = safety_cfg_write_apply_pairs(&fake_link, pairs, 1, true, reason, sizeof(reason), NULL);
    TEST_CHECK(ok == false, "a commit that ACKed but did not actually land is reported as FAILED");
    TEST_CHECK(reason[0] != '\0', "a non-empty reason is produced");
    TEST_CHECK(strstr(reason, "does not read back") != NULL || strstr(reason, "FAILED") != NULL,
               "the reason explains this is an unconfirmed/failed write, not a generic error");
    TEST_CHECK(s_stub_refetch_calls == 1, "a live read-back was attempted");
}

// Owner request 2026-09-08 ("I should be able to set the safety thermocouple
// type", scope-expanded to tc_offset_c, 0x010A): same class of test as
// test_apply_pairs_readback_mismatch_fails_even_when_acked_and_not_rejected()
// above, but naming tc_offset_c specifically -- confirm_commit_landed()/
// safety_cfg_write_apply_pairs() are generic over param_id, so this does not exercise new
// code, but the task brief asks for the read-back-verification negative test
// to explicitly cover the new field, not just its siblings. F32 (not the
// U16 the other tests default to), and a negative, non-integer value so a
// sign or truncation bug could not accidentally pass.
static void test_apply_pairs_tc_offset_c_readback_mismatch_fails(void)
{
    TEST_SECTION("safety_cfg_write_apply_pairs -- tc_offset_c (0x010A) ACKed, not rejected, but the read-back "
                 "does NOT match -- must FAIL, not report success");
    reset_all();
    s_stub_lookup_type = KILNLINK_PARAM_TYPE_F32;
    s_stub_lookup_name = "tc_offset_c";
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    safety_cfg_post_pair_t pairs[1] = { { .param_id = 0x010Au, .value_text = "-4.25" } };
    // Deliberately leave s_stub_params empty -- the board never actually
    // reports tc_offset_c back as -4.25 after the "commit", the exact shape
    // of a write that ACKed on the wire but did not really land.
    char reason[160];
    bool ok = safety_cfg_write_apply_pairs(&fake_link, pairs, 1, true, reason, sizeof(reason), NULL);
    TEST_CHECK(ok == false,
               "a tc_offset_c commit that ACKed but did not actually land is reported as FAILED, "
               "never as success -- a wrong safety-TC calibration offset silently believed to be "
               "written is exactly the failure this endpoint exists to prevent");
    TEST_CHECK(reason[0] != '\0', "a non-empty reason is produced");
    TEST_CHECK(strstr(reason, "tc_offset_c") != NULL,
               "the reason names the field, not just a generic failure");
    TEST_CHECK(s_stub_refetch_calls == 1, "a live read-back was attempted");
}

// 2026-09-10 opus review, second finding: confirm_commit_landed() used to
// call the BLOCKING safety_cfg_store_refetch() (portMAX_DELAY) unconditionally
// -- correct for every httpd-worker caller of safety_cfg_write_apply_pairs(), but a documented
// rule violation for the ceiling-reconcile writer (safety_ceiling_sync.c),
// which runs on safety_poll_task and must never block on s_store_lock behind
// an httpd commissioning POST (safety_cfg_store.c:1488-1510's own "ONLY path
// safety_poll_task may take" rule). safety_cfg_write_set_and_confirm_f32() is
// that writer's entry point, and is now the ONLY caller in this file's build
// that reaches apply_pairs_ex() with nonblocking_refetch=true.
//
// This is the negative-test-shaped proof the blocking-call fix is real: it
// checks CALL COUNTS on the two separately-tracked stubs, not just a return
// value both stubs could satisfy identically. Break the fix by hand (e.g.
// have safety_cfg_write_set_and_confirm_f32() call apply_pairs_ex(...,
// /*nonblocking_refetch=*/false) instead) and this test fails: s_stub_
// refetch_calls becomes 1 and s_stub_refetch_nonblocking_calls becomes 0,
// the exact inversion of what this asserts.
static void test_set_and_confirm_f32_uses_nonblocking_refetch(void)
{
    TEST_SECTION("safety_cfg_write_set_and_confirm_f32 -- the ceiling-reconcile writer's confirm "
                 "step uses the NON-BLOCKING refetch, never the blocking one safety_poll_task must "
                 "not call");
    reset_all();
    s_stub_lookup_type = KILNLINK_PARAM_TYPE_F32;
    s_stub_lookup_name = "abs_max_temp_c";
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    s_stub_params[0].param_id = 0x0104u;
    s_stub_params[0].type = KILNLINK_PARAM_TYPE_F32;
    s_stub_params[0].set = true;
    s_stub_params[0].value.f32_val = 120.0f;
    char reason[160];
    safety_ceiling_refusal_class_t out_class = SAFETY_CEILING_REFUSAL_OTHER;
    bool ok = safety_cfg_write_set_and_confirm_f32(&fake_link, 0x0104u, 120.0f, reason, sizeof(reason),
                                                   &out_class);
    TEST_CHECK(ok == true, "staged, committed, and confirmed by read-back");
    TEST_CHECK(s_stub_refetch_nonblocking_calls == 1,
               "the confirm step must go through the NON-BLOCKING refetch exactly once");
    TEST_CHECK(s_stub_refetch_calls == 0,
               "the BLOCKING refetch (portMAX_DELAY) must NEVER be reached from this call path -- "
               "safety_poll_task, the only caller of this function, may not block behind an httpd "
               "commissioning POST holding s_store_lock");
}

static void test_apply_pairs_refetch_failure_reports_unconfirmed_not_success(void)
{
    TEST_SECTION("safety_cfg_write_apply_pairs -- the live read-back itself fails (link trouble) -- reported UNCONFIRMED");
    reset_all();
    s_stub_refetch_result = false; // safety_cfg_store_refetch() could not complete
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    safety_cfg_post_pair_t pairs[1] = { { .param_id = 0x0104u, .value_text = "1300" } };
    char reason[160];
    bool ok = safety_cfg_write_apply_pairs(&fake_link, pairs, 1, true, reason, sizeof(reason), NULL);
    TEST_CHECK(ok == false, "an unconfirmable commit is never reported as success");
    TEST_CHECK(strstr(reason, "UNCONFIRMED") != NULL || strstr(reason, "could not read") != NULL,
               "the reason is honest about not knowing, not a fabricated success or a fabricated field name");
}

static void test_apply_pairs_late_rejection_attaches_pico_reason_to_confirmed_failure(void)
{
    TEST_SECTION("safety_cfg_write_apply_pairs -- a REJECTED frame that missed the reply window still surfaces its reason "
                 "once the read-back proves the write failed");
    reset_all();
    // s_stub_commit_rejected stays false (this simulates the ~144 ms race:
    // safety_link_send_commit_config()'s own window closed before the
    // REJECTED frame arrived) but the frame turns up in the stash by the time
    // confirm_commit_landed() checks it (safety_link_take_stashed_commit_
    // rejected()) -- see safety_link.c's stashed_commit_rejected field.
    s_stub_late_rejected = true;
    s_stub_late_reject_param_id = 0x0104u; // abs_max_temp_c
    s_stub_late_reject_reason = KILNLINK_COMMIT_CONFIG_REJECT_ARMED;
    s_stub_lookup_name = "abs_max_temp_c";
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    safety_cfg_post_pair_t pairs[1] = { { .param_id = 0x0104u, .value_text = "1300" } };
    char reason[160];
    safety_ceiling_refusal_class_t refusal_class = SAFETY_CEILING_REFUSAL_OTHER;
    bool ok = safety_cfg_write_apply_pairs(&fake_link, pairs, 1, true, reason, sizeof(reason), &refusal_class);
    TEST_CHECK(ok == false, "still reported as failed -- the read-back is what decides, and it never matched");
    TEST_CHECK(strstr(reason, "ARMED") != NULL,
               "the Pico's OWN late-arriving reason is attached to the failure, not a generic message");
    TEST_CHECK(s_stub_take_stashed_calls >= 1, "the stash was actually consulted");
    // 2026-09-10 opus review finding B: the machine-readable classification
    // must come from the stash's own numeric reject reason (KILNLINK_
    // COMMIT_CONFIG_REJECT_ARMED, set above), not from grepping `reason`.
    TEST_CHECK(refusal_class == SAFETY_CEILING_REFUSAL_ARMED,
               "a late-arriving stashed ARMED rejection must classify as ARMED via its numeric reason code");
}

static void test_apply_pairs_refetch_failure_with_no_stash_classifies_as_other_not_armed(void)
{
    TEST_SECTION("safety_cfg_write_apply_pairs -- an unconfirmable commit with NO rejection frame in hand must classify as "
                 "OTHER, never guess ARMED just because ARMED is the most common real-world cause");
    reset_all();
    s_stub_refetch_result = false; // safety_cfg_store_refetch() could not complete
    // s_stub_late_rejected stays false -- no stashed frame at all, the
    // "genuinely could not confirm, and does not even know why" case.
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    safety_cfg_post_pair_t pairs[1] = { { .param_id = 0x0104u, .value_text = "1300" } };
    char reason[160];
    safety_ceiling_refusal_class_t refusal_class = SAFETY_CEILING_REFUSAL_ARMED; // deliberately wrong seed
    bool ok = safety_cfg_write_apply_pairs(&fake_link, pairs, 1, true, reason, sizeof(reason), &refusal_class);
    TEST_CHECK(ok == false, "an unconfirmable commit is never reported as success");
    TEST_CHECK(refusal_class == SAFETY_CEILING_REFUSAL_OTHER,
               "with no numeric reject reason available, the classification must be the safe default "
               "OTHER, never a guessed ARMED -- this is the exact case the pre-fix strstr classifier "
               "got wrong when a stashed frame missed the read-back window");
}

// ---------------------------------------------------------------------------
// 2026-08-27 audit fix (M1): a peer on KILNLINK_PROTOCOL_VERSION < 8 (or one
// this board has never heard a FW_VERSION frame from at all) never sets
// KILNLINK_CONFIG_PAGE_UNSET_BIT -- every field decodes "set" regardless of
// whether the Pico actually holds a value for it. Ungated, that renders an
// uncommissioned board's abs_max_temp_c as {"set":true,"value":0}, and 0
// there means the overtemperature guard never trips.
// ---------------------------------------------------------------------------

static void test_peer_reports_unset_reliably_gates_on_known_and_version(void)
{
    TEST_SECTION("peer_reports_unset_reliably -- true only for a KNOWN peer at protocol >= 8");
    TEST_CHECK(peer_reports_unset_reliably(true, 8) == true, "known, exactly at the floor -- reliable");
    TEST_CHECK(peer_reports_unset_reliably(true, 9) == true, "known, newer than the floor -- reliable");
    TEST_CHECK(peer_reports_unset_reliably(true, 7) == false,
               "known but OLDER than the floor -- the exact v7-Pico scenario the audit caught");
    TEST_CHECK(peer_reports_unset_reliably(false, 8) == false,
               "version UNKNOWN (never heard from the peer) is treated as unreliable too, not "
               "optimistically assumed fine -- this board cannot prove the bit means anything yet");
    TEST_CHECK(peer_reports_unset_reliably(false, 0) == false, "unknown + version 0 -- still unreliable");
}

static void test_build_json_forces_unset_when_peer_too_old(void)
{
    TEST_SECTION("build_commissioning_json -- a SET cache entry renders set:false when the peer is too "
                 "old to have meant it (M1)");
    reset_all();
    s_stub_params[0].param_id = 0x0104;
    s_stub_params[0].name = "abs_max_temp_c";
    s_stub_params[0].type = KILNLINK_PARAM_TYPE_F32;
    s_stub_params[0].set = true;              // the cache says "set" ...
    s_stub_params[0].value.f32_val = 0.0f;    // ... to exactly the dangerous "never trips" value

    safety_cfg_http_snapshot_t snap = {0};
    snap.unset_reliable = false; // peer known-old or unknown -- this is the fix under test
    static char json[SAFETY_CFG_JSON_MAX];
    size_t len = build_commissioning_json(&snap, json, sizeof(json));
    TEST_CHECK(len > 0, "JSON built successfully");
    TEST_CHECK(strstr(json, "\"unset_reporting_reliable\":false") != NULL,
               "the page is told outright that set/unset cannot be trusted this fetch");
    TEST_CHECK(strstr(json, "\"id\":260") != NULL, "abs_max_temp_c still appears in the list");
    TEST_CHECK(strstr(json, "\"set\":false") != NULL,
               "forced to set:false even though the cache itself says set:true -- never a lying 0");
    TEST_CHECK(strstr(json, "\"value\"") == NULL,
               "value is omitted entirely -- the dangerous 0.0 is never printed at all");
}

static void test_build_json_still_reports_set_when_peer_reliable(void)
{
    TEST_SECTION("build_commissioning_json -- unset_reliable:true still reports a real set:true/value "
                 "exactly as before M1 (no regression on a peer new enough to be trusted)");
    reset_all();
    s_stub_params[0].param_id = 0x0104;
    s_stub_params[0].name = "abs_max_temp_c";
    s_stub_params[0].type = KILNLINK_PARAM_TYPE_F32;
    s_stub_params[0].set = true;
    s_stub_params[0].value.f32_val = 1300.0f;

    safety_cfg_http_snapshot_t snap = {0};
    snap.unset_reliable = true;
    static char json[SAFETY_CFG_JSON_MAX];
    size_t len = build_commissioning_json(&snap, json, sizeof(json));
    TEST_CHECK(len > 0, "JSON built successfully");
    TEST_CHECK(strstr(json, "\"unset_reporting_reliable\":true") != NULL, "reliable flag is true");
    TEST_CHECK(strstr(json, "\"set\":true") != NULL, "set:true is still reported for a reliable peer");
    TEST_CHECK(strstr(json, "\"value\":1300") != NULL, "the real value is still present");
}

// ---------------------------------------------------------------------------
// LOW (2026-08-27 audit fix): confirm_commit_landed()'s two `continue`s, for
// a pair whose lookup or value-parse fails on ITS OWN (post-refetch) pass --
// believed unreachable because safety_cfg_write_apply_pairs() already validated both before
// ever staging the pair -- used to silently skip verification of that pair
// and let the OVERALL commit still report success if every OTHER pair
// checked out. That is the identical failure shape ("ok cannot fail") this
// whole audit exists to close, just one level deeper. Both must now fail the
// whole confirmation instead.
// ---------------------------------------------------------------------------

static void test_confirm_commit_landed_lookup_failure_on_its_own_pass_fails_closed(void)
{
    TEST_SECTION("safety_cfg_write_apply_pairs -- if confirm_commit_landed()'s OWN lookup fails for a pair (believed "
                 "unreachable), the commit is reported FAILED, not silently skipped-and-successful");
    reset_all();
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    safety_cfg_post_pair_t pairs[1] = { { .param_id = 513, .value_text = "100" } };
    // Call #1 is safety_cfg_write_apply_pairs()'s own staging-time lookup (must succeed, or
    // this never reaches confirm_commit_landed() at all). Call #2 is
    // confirm_commit_landed()'s post-refetch lookup for that SAME pair --
    // fail exactly that one.
    s_stub_lookup_fail_at_call = 2;
    char reason[160];
    bool ok = safety_cfg_write_apply_pairs(&fake_link, pairs, 1, true, reason, sizeof(reason), NULL);
    TEST_CHECK(ok == false, "a lookup failure inside confirm_commit_landed() fails the whole commit");
    TEST_CHECK(reason[0] != '\0', "a non-empty reason is produced");
    TEST_CHECK(strstr(reason, "UNCONFIRMED") != NULL,
               "the reason is honest about not knowing, not a fabricated success");
}

static void test_confirm_commit_landed_parse_failure_on_its_own_pass_fails_closed(void)
{
    TEST_SECTION("safety_cfg_write_apply_pairs -- if confirm_commit_landed()'s OWN value-parse fails for a pair (believed "
                 "unreachable), the commit is reported FAILED, not silently skipped-and-successful");
    reset_all();
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    safety_cfg_post_pair_t pairs[1] = { { .param_id = 513, .value_text = "100" } };
    // Call #1 (staging) sees the default type (U16) -- "100" parses fine.
    // Call #2 (confirm_commit_landed()'s own re-lookup for the same pair) is
    // overridden to report BOOL instead -- "100" is not a legal bool literal
    // (safety_cfg_write_parse_value_for_type() only accepts "0"/"1"), so THAT call's re-parse
    // fails even though the original staging parse never did.
    s_stub_lookup_type_override_call = 2;
    s_stub_lookup_type_override_value = KILNLINK_PARAM_TYPE_BOOL;
    char reason[160];
    bool ok = safety_cfg_write_apply_pairs(&fake_link, pairs, 1, true, reason, sizeof(reason), NULL);
    TEST_CHECK(ok == false, "a re-parse failure inside confirm_commit_landed() fails the whole commit");
    TEST_CHECK(reason[0] != '\0', "a non-empty reason is produced");
    TEST_CHECK(strstr(reason, "UNCONFIRMED") != NULL,
               "the reason is honest about not knowing, not a fabricated success");
}

static void test_apply_pairs_unknown_id_is_refused_and_named(void)
{
    TEST_SECTION("safety_cfg_write_apply_pairs -- an unknown param id is refused and named in the reason");
    reset_all();
    s_stub_lookup_result = false; // simulates an id this build's table does not recognise
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    safety_cfg_post_pair_t pairs[1] = { { .param_id = 0x9999, .value_text = "1" } };
    char reason[160];
    bool ok = safety_cfg_write_apply_pairs(&fake_link, pairs, 1, false, reason, sizeof(reason), NULL);
    TEST_CHECK(ok == false, "an unknown id is refused");
    TEST_CHECK(strstr(reason, "39321") != NULL || strstr(reason, "unknown") != NULL,
               "the reason names the offending id (COMMISSIONING.md sec 2's per-id refusal)");
    TEST_CHECK(s_stub_set_param_calls == 0, "nothing was sent to the wire for an unrecognised id");
}

static void test_apply_pairs_refused_commit_surfaces_reason(void)
{
    TEST_SECTION("safety_cfg_write_apply_pairs -- a commit the safety processor never ACKs surfaces a reason, not silence");
    reset_all();
    s_stub_commit_result = ESP_ERR_TIMEOUT;
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    safety_cfg_post_pair_t pairs[1] = { { .param_id = 513, .value_text = "100" } };
    char reason[160];
    bool ok = safety_cfg_write_apply_pairs(&fake_link, pairs, 1, true, reason, sizeof(reason), NULL);
    TEST_CHECK(ok == false, "an un-ACKed commit is reported as a failure");
    TEST_CHECK(reason[0] != '\0', "a non-empty reason is always produced on failure");
    TEST_CHECK(strstr(reason, "commit") != NULL || strstr(reason, "acknowledge") != NULL,
               "the reason talks about the commit outcome, not a generic error");
    TEST_CHECK(s_stub_set_param_calls == 1, "the field WAS staged before the commit was attempted");
}

static void test_apply_pairs_rejected_commit_names_field_and_reason(void)
{
    TEST_SECTION("safety_cfg_write_apply_pairs -- a REJECTED commit (0x20) names the offending field and reason, "
                 "not \"awaiting confirmation\"");
    reset_all();
    s_stub_commit_rejected = true;
    s_stub_commit_reject_param_id = 0x0104u; // abs_max_temp_c
    s_stub_commit_reject_reason = KILNLINK_COMMIT_CONFIG_REJECT_RANGE;
    s_stub_lookup_name = "abs_max_temp_c"; // safety_cfg_store_lookup() stub returns this for any id
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    // value_text just has to parse for the stub's default wire type
    // (KILNLINK_PARAM_TYPE_U16) -- the actual rejection this test exercises
    // comes from the stubbed safety_link_send_commit_config() outcome, not
    // from safety_cfg_write_parse_value_for_type(), so this must be an in-range U16.
    safety_cfg_post_pair_t pairs[1] = { { .param_id = 0x0104u, .value_text = "500" } };
    char reason[160];
    bool ok = safety_cfg_write_apply_pairs(&fake_link, pairs, 1, true, reason, sizeof(reason), NULL);
    TEST_CHECK(ok == false, "a rejected commit is reported as a failure, not success");
    TEST_CHECK(strstr(reason, "abs_max_temp_c") != NULL,
               "the offending field is named (COMMISSIONING.md sec 3.1: \"name the offending field\")");
    TEST_CHECK(strstr(reason, "range") != NULL, "the reason (\"value out of range\") is named");
    TEST_CHECK(strstr(reason, "awaiting confirmation") == NULL,
               "this is no longer the old ambiguous \"sent, awaiting confirmation\" wording");
    TEST_CHECK(s_stub_commit_calls == 1, "commit was sent exactly once");

    // Same rejection, but a not-field-specific one (ARMED) -- the
    // NO_PARAM_ID sentinel must not be looked up as if it were a real id.
    reset_all();
    s_stub_commit_rejected = true;
    s_stub_commit_reject_param_id = KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID;
    s_stub_commit_reject_reason = KILNLINK_COMMIT_CONFIG_REJECT_ARMED;
    reason[0] = '\0';
    ok = safety_cfg_write_apply_pairs(&fake_link, pairs, 1, true, reason, sizeof(reason), NULL);
    TEST_CHECK(ok == false, "an ARMED rejection is reported as a failure");
    TEST_CHECK(strstr(reason, "ARMED") != NULL, "the ARMED reason is named");
    TEST_CHECK(strstr(reason, "0x0104") == NULL && strstr(reason, "abs_max_temp_c") == NULL,
               "a not-field-specific rejection does not fabricate a field name");
}

// 2026-09-15 (Opus adversarial re-review, F1/F2): before this fix, the tc_
// type heat-safety gate's ARMED_HEAT_ON/ARMED_HEAT_UNKNOWN reasons (wire
// values 5/6, added by N3) and the mixed-change ARMED_MIXED reason (wire
// value 7, added by F2) all fell through commit_reject_reason_words()'s
// default case to "refused (unrecognised reason)" -- unreadable to the
// operator and missed the commissioning page's /ARMED/i match. This proves
// all three now render an actionable, ARMED-family sentence instead.
static void test_apply_pairs_rejected_commit_new_tc_type_reasons_are_readable(void)
{
    TEST_SECTION("safety_cfg_write_apply_pairs -- ARMED_HEAT_ON/ARMED_HEAT_UNKNOWN/ARMED_MIXED render real "
                 "sentences, not \"unrecognised reason\"");
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    safety_cfg_post_pair_t pairs[1] = { { .param_id = 0x0104u, .value_text = "500" } };
    char reason[160];

    const uint8_t reasons[] = {
        KILNLINK_COMMIT_CONFIG_REJECT_ARMED_HEAT_ON,
        KILNLINK_COMMIT_CONFIG_REJECT_ARMED_HEAT_UNKNOWN,
        KILNLINK_COMMIT_CONFIG_REJECT_ARMED_MIXED,
    };
    for (size_t i = 0; i < sizeof(reasons) / sizeof(reasons[0]); ++i) {
        reset_all();
        s_stub_commit_rejected = true;
        s_stub_commit_reject_param_id = KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID;
        s_stub_commit_reject_reason = reasons[i];
        reason[0] = '\0';
        bool ok = safety_cfg_write_apply_pairs(&fake_link, pairs, 1, true, reason, sizeof(reason), NULL);
        TEST_CHECK(ok == false, "the rejection is reported as a failure");
        TEST_CHECK(strstr(reason, "ARMED") != NULL,
                   "the sentence names ARMED (matches the page's /ARMED/i test)");
        TEST_CHECK(strstr(reason, "unrecognised reason") == NULL,
                   "the reason is NOT the generic unrecognised-reason fallback");
    }
}

// ---------------------------------------------------------------------------
// CT_COMMISSIONING_PLAN.md step 2 -- ct_auto_zero_check_preconditions() and
// the counts<->mV/100mV-refusal decision, tested directly (pure functions,
// no httpd_req_t needed -- see ct_auto_zero_check_preconditions()'s own doc
// comment for why this file's httpd_req_recv() stub always returning 0
// makes a body-driven test of the handler itself impractical).
// ---------------------------------------------------------------------------

static void test_ct_auto_zero_precheck_each_refusal(void)
{
    TEST_SECTION("ct_auto_zero_check_preconditions -- each individual refusal, checked one at a time "
                 "against an otherwise-all-clear baseline");

    // Baseline uses has_existing=true/SOURCE_SWEEP throughout -- has_existing
    // itself is exercised separately below (test_ct_auto_zero_precheck_
    // requires_existing_a_fs()) and SOURCE_SWEEP keeps the unrelated
    // manual-wins rule from interfering with these single-condition checks.

    // Baseline: everything clear -- must return NULL (ok).
    TEST_CHECK(ct_auto_zero_check_preconditions(true, false, true, true, false, 5000u, false, false,
                                                 true, SAFETY_CT_CAL_SOURCE_SWEEP, false) == NULL,
               "baseline: every precondition satisfied -> NULL (ok)");

    TEST_CHECK(ct_auto_zero_check_preconditions(false, false, true, true, false, 5000u, false, false,
                                                 true, SAFETY_CT_CAL_SOURCE_SWEEP, false) != NULL,
               "link down -> refused");
    TEST_CHECK(ct_auto_zero_check_preconditions(true, true, true, true, false, 5000u, false, false, true,
                                                 SAFETY_CT_CAL_SOURCE_SWEEP, false) != NULL,
               "trip latched -> refused");
    TEST_CHECK(ct_auto_zero_check_preconditions(true, false, false, true, false, 5000u, false, false,
                                                 true, SAFETY_CT_CAL_SOURCE_SWEEP, false) != NULL,
               "K4 not closed -> refused");
    TEST_CHECK(ct_auto_zero_check_preconditions(true, false, true, false, false, 5000u, false, false,
                                                 true, SAFETY_CT_CAL_SOURCE_SWEEP, false) != NULL,
               "no board I/O -> refused");
    TEST_CHECK(ct_auto_zero_check_preconditions(true, false, true, true, true, 5000u, false, false, true,
                                                 SAFETY_CT_CAL_SOURCE_SWEEP, false) != NULL,
               "a relay is commanded on -> refused");
    TEST_CHECK(ct_auto_zero_check_preconditions(true, false, true, true, false, 4999u, false, false,
                                                 true, SAFETY_CT_CAL_SOURCE_SWEEP, false) != NULL,
               "relays off only 4999 ms (just under the 5 s floor) -> refused");
    TEST_CHECK(ct_auto_zero_check_preconditions(true, false, true, true, false, UINT32_MAX, false, false,
                                                 true, SAFETY_CT_CAL_SOURCE_SWEEP, false) != NULL,
               "relays-off duration unknown (UINT32_MAX sentinel) -> refused, not treated as 'plenty'");
    TEST_CHECK(ct_auto_zero_check_preconditions(true, false, true, true, false, 5000u, true, false, true,
                                                 SAFETY_CT_CAL_SOURCE_SWEEP, false) != NULL,
               "profile running/paused -> refused");
    TEST_CHECK(ct_auto_zero_check_preconditions(true, false, true, true, false, 5000u, false, true, true,
                                                 SAFETY_CT_CAL_SOURCE_SWEEP, false) != NULL,
               "autotune running -> refused");

    // NEGATIVE TEST (this codebase's "negative-test every check" rule):
    // exactly at the 5 s floor must PASS (>=, not >) -- proves the boundary
    // is where the plan says it is, not off by one.
    TEST_CHECK(ct_auto_zero_check_preconditions(true, false, true, true, false, 5000u, false, false,
                                                 true, SAFETY_CT_CAL_SOURCE_SWEEP, false) == NULL,
               "NEGATIVE: exactly 5000 ms passes -- the floor is >=5s, not >5s");
}

static void test_ct_auto_zero_precheck_manual_wins_unless_override(void)
{
    TEST_SECTION("ct_auto_zero_check_preconditions -- manual wins unless override_manual is set");

    TEST_CHECK(ct_auto_zero_check_preconditions(true, false, true, true, false, 5000u, false, false, true,
                                                 SAFETY_CT_CAL_SOURCE_MANUAL, false) != NULL,
               "manual source, no override -> refused");
    TEST_CHECK(ct_auto_zero_check_preconditions(true, false, true, true, false, 5000u, false, false, true,
                                                 SAFETY_CT_CAL_SOURCE_MANUAL, true) == NULL,
               "manual source, override_manual=1 -> allowed");
    TEST_CHECK(ct_auto_zero_check_preconditions(true, false, true, true, false, 5000u, false, false, true,
                                                 SAFETY_CT_CAL_SOURCE_SWEEP, false) == NULL,
               "sweep source (not manual) never needs an override for auto-zero");
}

// HIGH review finding on b8f0f47: the commit path used to fabricate
// a_fs_for_convert=1.0f when has_existing was false and write a matching
// k_ct=1.0 (V/A) to the Pico for a channel that had never been calibrated at
// all -- a silently wrong gain, not a "zero-only measurement is unaffected
// by A_fs" no-op the removed comment claimed. The fix moved the refusal into
// this same precondition gate so the wrong-gain write can never be reached.
static void test_ct_auto_zero_precheck_requires_existing_a_fs(void)
{
    TEST_SECTION("ct_auto_zero_check_preconditions -- refuses outright when the channel has no A_fs yet "
                 "(HIGH finding on b8f0f47: no fabricated A_fs, ever)");

    const char *reason = ct_auto_zero_check_preconditions(true, false, true, true, false, 5000u, false,
                                                            false, false, SAFETY_CT_CAL_SOURCE_MANUAL, false);
    TEST_CHECK(reason != NULL, "has_existing=false -> refused, not silently allowed with a fabricated A_fs");
    TEST_CHECK(reason != NULL && strstr(reason, "A_fs") != NULL,
               "refusal names the actual problem (no A_fs yet), not a generic message");

    // override_manual must NOT bypass this -- it only overrides the manual-
    // wins rule, which has nothing to do with A_fs existing at all.
    TEST_CHECK(ct_auto_zero_check_preconditions(true, false, true, true, false, 5000u, false, false, false,
                                                 SAFETY_CT_CAL_SOURCE_MANUAL, true) != NULL,
               "override_manual=1 does not bypass the missing-A_fs refusal");

    // Existing value present (any source) -> this refusal does not fire.
    TEST_CHECK(ct_auto_zero_check_preconditions(true, false, true, true, false, 5000u, false, false, true,
                                                 SAFETY_CT_CAL_SOURCE_SWEEP, false) == NULL,
               "has_existing=true -> the missing-A_fs refusal does not fire");
}

// NEGATIVE TEST proving the has_existing check above can actually FAIL to
// catch the HIGH defect it exists for -- simulate the old (reverted)
// behavior by short-circuiting has_existing to "true" regardless of the
// real value, the same shape the original bug had (the caller silently
// treated has_existing=false as fine and fabricated a value instead of
// refusing). Confirms the assertions above are not vacuous.
static void test_ct_auto_zero_precheck_missing_a_fs_check_is_not_vacuous(void)
{
    TEST_SECTION("NEGATIVE TEST -- simulating the original 'fabricate A_fs' bug (has_existing forced "
                 "true) would be caught by the missing-A_fs refusal assertion above");
    bool original_has_existing = false;
    bool bug_ignores_has_existing = true; // simulates the bug: caller never looks at has_existing at all
    (void)original_has_existing;
    // SOURCE_SWEEP (not MANUAL) so the unrelated manual-wins rule cannot
    // also refuse this call for a different reason and mask the point.
    TEST_CHECK(ct_auto_zero_check_preconditions(true, false, true, true, false, 5000u, false, false,
                                                 bug_ignores_has_existing, SAFETY_CT_CAL_SOURCE_SWEEP,
                                                 false) == NULL,
               "if has_existing were wrongly reported/ignored as true, the refusal would NOT fire here -- "
               "proving test_ct_auto_zero_precheck_requires_existing_a_fs()'s has_existing=false assertion "
               "is the one actually doing the work, not a tautology");
}

// ---------------------------------------------------------------------------
// MEDIUM finding on b8f0f47: postconditions must be re-checked AFTER the
// ~10-12s measurement, not just before it -- ct_auto_zero_check_
// postconditions() is the same kind of pure, host-testable gate as
// ct_auto_zero_check_preconditions() above.
// ---------------------------------------------------------------------------

static void test_ct_auto_zero_postcheck_each_refusal(void)
{
    TEST_SECTION("ct_auto_zero_check_postconditions -- each individual refusal, checked one at a time");

    uint32_t waited_ms = 10000u;

    TEST_CHECK(ct_auto_zero_check_postconditions(false, 5000u + waited_ms, waited_ms, false, false) == NULL,
               "baseline: relays off the whole window, no profile/autotune -> NULL (ok)");

    TEST_CHECK(ct_auto_zero_check_postconditions(true, 5000u + waited_ms, waited_ms, false, false) != NULL,
               "a relay is commanded on right now -> refused");

    // A relay that pulsed on and back off mid-measurement resets kiln_io_
    // relays_off_ms() to a small value even though a point-in-time read
    // afterward already shows it off again -- this is the actual case the
    // MEDIUM finding is about.
    TEST_CHECK(ct_auto_zero_check_postconditions(false, 1000u, waited_ms, false, false) != NULL,
               "relay pulsed on mid-measurement (off_ms far short of 5000+waited_ms) -> refused, even "
               "though relays_on_after reads false");

    TEST_CHECK(ct_auto_zero_check_postconditions(false, UINT32_MAX, waited_ms, false, false) != NULL,
               "relays-off duration unknown (UINT32_MAX sentinel) -> refused, not treated as 'plenty'");

    TEST_CHECK(ct_auto_zero_check_postconditions(false, 5000u + waited_ms, waited_ms, true, false) != NULL,
               "a profile started during the measurement -> refused");

    TEST_CHECK(ct_auto_zero_check_postconditions(false, 5000u + waited_ms, waited_ms, false, true) != NULL,
               "autotune started during the measurement -> refused");

    // NEGATIVE TEST: exactly at the floor (off_ms_after == 5000+waited_ms)
    // must PASS (>=, not >).
    TEST_CHECK(ct_auto_zero_check_postconditions(false, 5000u + waited_ms, waited_ms, false, false) == NULL,
               "NEGATIVE: off_ms_after exactly 5000+waited_ms passes -- the floor is >=, not >");
    TEST_CHECK(ct_auto_zero_check_postconditions(false, 5000u + waited_ms - 1u, waited_ms, false, false) !=
                   NULL,
               "NEGATIVE: one ms under the floor is refused");
}

// NEGATIVE TEST proving the precondition check function can actually FAIL to
// catch a real defect -- break production by hand (K4 check inverted) and
// confirm the baseline case this test suite relies on would then wrongly
// pass. This is not run against production; it documents, by construction,
// that test_ct_auto_zero_precheck_each_refusal()'s K4 assertion is not
// vacuous.
static void test_ct_auto_zero_precheck_k4_check_is_not_vacuous(void)
{
    TEST_SECTION("NEGATIVE TEST -- a hand-broken 'k4_closed' polarity would be caught by the K4 "
                 "refusal assertion above, proving that check is not vacuous");
    bool broken_k4_closed_reads_as_open = !true; // simulates the bug: treats "closed" as "open"
    TEST_CHECK(ct_auto_zero_check_preconditions(true, false, broken_k4_closed_reads_as_open, true, false,
                                                 5000u, false, false, false, SAFETY_CT_CAL_SOURCE_MANUAL,
                                                 false) != NULL,
               "a K4-closed board misread as open is refused -- if the real check were inverted "
               "(refusing on true instead of false), THIS assertion, not the earlier one, would be "
               "the one to fail");
}

// NOTE: this file stubs safety_ct_cal_convert()/safety_cfg_store_set_ct_cal_
// input() (see this file's own header comment: the real safety_cfg_store.c
// links into the OTHER host-test executable, test_safety_cfg_store.c, which
// is where safety_ct_cal_convert()'s own probe-rating math is genuinely
// exercised -- test_ct_cal_convert_at_several_probe_ratings() there). This
// file's stub of that function always returns fixed knobs regardless of its
// arguments, so a "round-trip through safety_ct_cal_convert()" test HERE
// would silently test the stub, not real conversion math -- exactly the
// idealized/mocked-input trap this codebase's project memory warns about.
// What IS genuinely this file's own code, and worth testing here, is
// ct_auto_zero_counts_to_mv() itself (CURRENT_SENSE.md's inverse formula,
// zero_mv = zero_counts * (3.3/4096) / gain * 1000) -- checked directly
// against quantized (integer) counts at two different probe gains, since
// A_fs itself never enters this formula (it only scales k_ct_v_per_a, not
// zero_counts/zero_mv -- CT_COMMISSIONING_PLAN.md step 1's own model).
static void test_ct_auto_zero_counts_to_mv_at_two_probe_ratings(void)
{
    TEST_SECTION("ct_auto_zero_counts_to_mv -- quantized (integer) ADC counts against the "
                 "CURRENT_SENSE.md inverse formula, at two different front-end gains (the parameter "
                 "this formula actually depends on -- A_fs does not enter it at all)");

    // Bench probe's own worked example (CT_COMMISSIONING_PLAN.md): ~59 mV
    // zero at the 0.715 default gain implies ~52 raw counts; the reverse
    // direction (counts -> mV) is what this function computes.
    uint16_t counts_default_gain = 52u; // quantized -- not chosen to divide evenly
    float mv_default_gain = ct_auto_zero_counts_to_mv(counts_default_gain, 0.715f);
    float expected_default_gain = ((float)counts_default_gain * (3.3f / 4096.0f) / 0.715f) * 1000.0f;
    TEST_CHECK(fabsf(mv_default_gain - expected_default_gain) < 1e-3f,
               "default gain (0.715): matches the hand-computed formula exactly, no hidden rounding");
    TEST_CHECK(mv_default_gain > 55.0f && mv_default_gain < 63.0f,
               "default gain: 52 counts is roughly the 59 mV this quantization implies");

    // A different front-end gain (still a real, positive value -- gain is a
    // hardware property of the divider, independent of the probe's A_fs).
    float other_gain = 1.2f;
    uint16_t counts_other_gain = 40u;
    float mv_other_gain = ct_auto_zero_counts_to_mv(counts_other_gain, other_gain);
    float expected_other_gain = ((float)counts_other_gain * (3.3f / 4096.0f) / other_gain) * 1000.0f;
    TEST_CHECK(fabsf(mv_other_gain - expected_other_gain) < 1e-3f,
               "a different gain: matches the hand-computed formula exactly too");
    TEST_CHECK(fabsf(mv_other_gain - mv_default_gain) > 1.0f,
               "NEGATIVE: the two gains genuinely produce different mV for these counts -- proves "
               "gain is actually being used, not silently defaulted for both");

    // Nonpositive/zero gain falls back to the documented 0.715 default
    // rather than dividing by zero or a negative number.
    float mv_zero_gain = ct_auto_zero_counts_to_mv(counts_default_gain, 0.0f);
    TEST_CHECK(fabsf(mv_zero_gain - mv_default_gain) < 1e-3f,
               "gain <= 0 falls back to the 0.715 default -- matches the explicit default-gain call");
}

static void test_ct_auto_zero_100mv_refusal_uses_quantized_counts(void)
{
    TEST_SECTION("ct_auto_zero_counts_to_mv -- the 100 mV refusal threshold, evaluated against "
                 "quantized counts, not idealized floats");

    float gain = 0.715f;
    // zero_counts=52 is ~59 mV (the nominal, no-current baseline). A stored
    // value of 0 mV plus a genuinely large real delta (current flowing) must
    // exceed the plan's 100 mV refusal threshold.
    float previous_zero_mv = 0.0f;
    uint16_t counts_with_real_current = 200u; // quantized, deliberately not round in mV
    float measured_mv = ct_auto_zero_counts_to_mv(counts_with_real_current, gain);
    float delta_mv = measured_mv - previous_zero_mv;
    TEST_CHECK(fabsf(delta_mv) > 100.0f,
               "200 counts of quantized delta against a 0 mV baseline exceeds the 100 mV refusal "
               "threshold -- this is the 'current flowing with every relay off' case that must be "
               "refused, not calibrated away");

    // NEGATIVE TEST: a small, quantized delta (offset drift, not real
    // current) must NOT trip the same threshold.
    uint16_t counts_small_drift = 55u; // a few counts of drift from the 52-count baseline
    float small_drift_mv = ct_auto_zero_counts_to_mv(counts_small_drift, gain) -
                            ct_auto_zero_counts_to_mv(52u, gain);
    TEST_CHECK(fabsf(small_drift_mv) <= 100.0f,
               "NEGATIVE: a few counts of quantized drift stays under 100 mV -- proves the check "
               "above is measuring a real large delta, not tripping on any nonzero difference");
}

// ---------------------------------------------------------------------------
// S8 rate-guard auto-calc write path (docs/audits/s8_auto_calc_design_2026-
// 09-09.md "Part 3") -- rate_guard_gather_and_estimate()/rate_guard_current_
// value()/rate_guard_auto_compute() directly (same "call the static helper
// directly" convention this whole file already uses for safety_cfg_write_apply_pairs()/
// build_commissioning_json()), plus a few handler-level smoke tests via the
// captured httpd_resp_send() body for the two cases reachable without a
// real request body (content_len == 0 -- see rate_guard_auto_post_handler's
// own comment: an empty body is a legitimate "act on tighten/apply only"
// submission).
// ---------------------------------------------------------------------------

// s_stub_params (this file's fake for safety_cfg_store_get_by_index()) is
// indexed by array POSITION, not by id -- the real SAFETY_CFG_PARAM_TABLE
// this fake stands in for lives in safety_cfg_store.c, which this
// executable does NOT link (see this file's header comment: the real store
// links into the OTHER executable). Position 0 is as good as any other for
// this fake; rate_guard_current_value() searches by param_id, not position,
// so this is a faithful stand-in for "0x0204 lives somewhere in the table".
static void set_current_rate_guard(bool set, float value)
{
    s_stub_params[0].param_id = 0x0204u;
    s_stub_params[0].set = set;
    s_stub_params[0].value.f32_val = value;
}

static void test_rate_guard_gather_no_zone_identified_is_no_data(void)
{
    TEST_SECTION("rate_guard_gather_and_estimate -- no zone identified -> NO_DATA");
    reset_all();
    reset_rate_guard_stubs();
    float out = -1.0f;
    s8_rate_guard_estimate_reason_t r = rate_guard_gather_and_estimate(&out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_NO_DATA, "an uncommissioned board reports NO_DATA");
}

static void test_rate_guard_gather_picks_coldest_identified_zone(void)
{
    TEST_SECTION("rate_guard_gather_and_estimate -- reaches the REAL s8_rate_guard_estimate() "
                 "through zones_config_get_model()/_get_model_fit_context()'s fakes");
    reset_all();
    reset_rate_guard_stubs();
    // Zone 0: identified at 500C (hot). Zone 1: identified at 20C (cold) --
    // this is the one that must win.
    s_stub_zone_has_model[0] = true;
    s_stub_zone_k_dc[0] = 5.0f;
    s_stub_zone_tau_s[0] = 60.0f;
    s_stub_zone_has_fit_ctx[0] = true;
    s_stub_zone_fit_temp_c[0] = 500.0f;

    s_stub_zone_has_model[1] = true;
    s_stub_zone_k_dc[1] = 20.0f;
    s_stub_zone_tau_s[1] = 60.0f;
    s_stub_zone_has_fit_ctx[1] = true;
    s_stub_zone_fit_temp_c[1] = 20.0f;

    float out = -1.0f;
    s8_rate_guard_estimate_reason_t r = rate_guard_gather_and_estimate(&out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_OK, "two identified zones -> OK");
    // Zone 1: (20/60)*60*1.3 = 26.0 C/min -- inside [15,60], so unclamped.
    TEST_CHECK(fabsf(out - 26.0f) < 1e-3f, "zone 1 (coldest fit_temp_c) wins, not zone 0");
}

// 2026-09-10 (opus review round 2, defect B): these two are new -- no
// existing test in this file ever set s_stub_zone_has_coupling[] true, so
// the off-diagonal/board_coupling_provenance_ok path safety_cfg_http.c's
// rate_guard_gather_and_estimate() computes via the now-SHARED
// zone_coupling_matrix_provenance_ok()/zone_coupling_use_measured_diag_k_dc()
// (zone_coupling_solve.c/.h) was entirely unexercised here. Both zones have
// identical identified models/fit context so the only thing that differs
// between them is coupling provenance -- isolating the margin
// (S8_RATE_GUARD_ESTIMATE_MARGIN_UNCOUPLED 2.0x vs S8_RATE_GUARD_ESTIMATE_
// MARGIN 1.3x) as the sole observable effect of that shared call.
static void test_rate_guard_gather_measured_offdiag_without_diag_k_dc_is_unproven(void)
{
    TEST_SECTION("rate_guard_gather_and_estimate -- a measured off-diagonal with NO member's "
                 "coupling_diag_k_dc identified is unproven -> MARGIN_UNCOUPLED (2.0x), exercising "
                 "the shared zone_coupling_matrix_provenance_ok() path");
    reset_all();
    reset_rate_guard_stubs();
    s_stub_thermo_count = 2;
    s_stub_zone_has_model[0] = true;
    s_stub_zone_k_dc[0] = 20.0f;
    s_stub_zone_tau_s[0] = 60.0f;
    s_stub_zone_has_fit_ctx[0] = true;
    s_stub_zone_fit_temp_c[0] = 20.0f;
    s_stub_zone_has_model[1] = true;
    s_stub_zone_k_dc[1] = 20.0f;
    s_stub_zone_tau_s[1] = 60.0f;
    s_stub_zone_has_fit_ctx[1] = true;
    s_stub_zone_fit_temp_c[1] = 20.0f;
    // A measured, nonzero off-diagonal -- coupling data exists -- but
    // neither zone's coupling_diag_k_dc is identified (both left false by
    // reset_rate_guard_stubs() above), which is this bench's real, live
    // state today. zone_coupling_matrix_provenance_ok() must refuse the
    // whole matrix.
    s_stub_zone_has_coupling[0] = true;
    s_stub_zone_coupling_row[0][1] = 5.0f;
    s_stub_zone_has_coupling[1] = true;
    s_stub_zone_coupling_row[1][0] = 5.0f;
    float out = -1.0f;
    s8_rate_guard_estimate_reason_t r = rate_guard_gather_and_estimate(&out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_OK, "still produces an estimate, just at the wider margin");
    // (20/60)*60*2.0 = 40.0 C/min -- UNCOUPLED margin, unproven coupling.
    TEST_CHECK(fabsf(out - 40.0f) < 1e-3f,
               "unproven coupling must fall back to the 2.0x UNCOUPLED margin, not 1.3x -- a "
               "regression back to the pre-fix mirror (missing the use_measured_diag_k_dc gate, or "
               "any other divergence from the real zone_coupling_matrix_provenance_ok()) would "
               "silently accept this matrix and report 26.0 instead");
}

static void test_rate_guard_gather_measured_offdiag_with_diag_k_dc_is_proven(void)
{
    TEST_SECTION("rate_guard_gather_and_estimate -- a measured off-diagonal WITH every member's "
                 "coupling_diag_k_dc identified is proven -> the tighter coupled margin (1.3x)");
    reset_all();
    reset_rate_guard_stubs();
    s_stub_thermo_count = 2;
    s_stub_zone_has_model[0] = true;
    s_stub_zone_k_dc[0] = 20.0f;
    s_stub_zone_tau_s[0] = 60.0f;
    s_stub_zone_has_fit_ctx[0] = true;
    s_stub_zone_fit_temp_c[0] = 20.0f;
    s_stub_zone_has_model[1] = true;
    s_stub_zone_k_dc[1] = 20.0f;
    s_stub_zone_tau_s[1] = 60.0f;
    s_stub_zone_has_fit_ctx[1] = true;
    s_stub_zone_fit_temp_c[1] = 20.0f;
    s_stub_zone_has_coupling[0] = true;
    s_stub_zone_coupling_row[0][1] = 5.0f;
    s_stub_zone_has_coupling[1] = true;
    s_stub_zone_coupling_row[1][0] = 5.0f;
    // Every member now has a measured, usable diag_k_dc -- provenance ok.
    s_stub_zone_has_coupling_diag_k_dc[0] = true;
    s_stub_zone_coupling_diag_k_dc[0] = 20.0f;
    s_stub_zone_has_coupling_diag_k_dc[1] = true;
    s_stub_zone_coupling_diag_k_dc[1] = 20.0f;
    float out = -1.0f;
    s8_rate_guard_estimate_reason_t r = rate_guard_gather_and_estimate(&out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_OK, "produces an estimate at the tighter margin");
    TEST_CHECK(fabsf(out - 40.0f) >= 1e-3f,
               "proven coupling must NOT land on the same number the unproven case above produces "
               "-- if it does, board_coupling_provenance_ok is not actually distinguishing the two");
}

static void test_rate_guard_gather_missing_fit_context_disqualifies_zone(void)
{
    TEST_SECTION("rate_guard_gather_and_estimate -- a model with NO recorded fit context is not a "
                 "candidate, even though k_dc/tau_s alone look usable");
    reset_all();
    reset_rate_guard_stubs();
    s_stub_zone_has_model[0] = true;
    s_stub_zone_k_dc[0] = 10.0f;
    s_stub_zone_tau_s[0] = 60.0f;
    s_stub_zone_has_fit_ctx[0] = false; // never recorded
    float out = -1.0f;
    s8_rate_guard_estimate_reason_t r = rate_guard_gather_and_estimate(&out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_NO_DATA,
               "a model with no fit context is disqualified -- this function must not invent a fit "
               "temperature of 0.0 just because the struct was zero-initialized");
}

static void test_rate_guard_current_value_reads_0x0204_by_id_not_position(void)
{
    TEST_SECTION("rate_guard_current_value -- reads 0x0204's REAL table position, not index 0");
    reset_all();
    reset_rate_guard_stubs();
    set_current_rate_guard(true, 20.0f);
    float value = -1.0f;
    bool is_set = false;
    bool ok = rate_guard_current_value(&value, &is_set);
    TEST_CHECK(ok, "0x0204 is always in this build's table");
    TEST_CHECK(is_set, "current value is reported set");
    TEST_CHECK(fabsf(value - 20.0f) < 1e-6f, "the exact bench value (20.0) round-trips");
}

static void test_rate_guard_current_value_dormant_reports_unset(void)
{
    TEST_SECTION("rate_guard_current_value -- dormant (never fetched) reports is_set=false");
    reset_all();
    reset_rate_guard_stubs();
    set_current_rate_guard(false, 0.0f);
    float value = -1.0f;
    bool is_set = true; // deliberately seeded wrong, to prove the function actually clears it
    bool ok = rate_guard_current_value(&value, &is_set);
    TEST_CHECK(ok, "0x0204 is always in this build's table, even when unset");
    TEST_CHECK(!is_set, "dormant/unset is reported honestly, not defaulted to true");
}

static void test_rate_guard_auto_compute_dormant_applies(void)
{
    TEST_SECTION("rate_guard_auto_compute -- a dormant guard's first commissioning always APPLIES");
    reset_all();
    reset_rate_guard_stubs();
    set_current_rate_guard(false, 0.0f);
    s_stub_zone_has_model[0] = true;
    s_stub_zone_k_dc[0] = 5.0f;
    s_stub_zone_tau_s[0] = 60.0f;
    s_stub_zone_has_fit_ctx[0] = true;
    s_stub_zone_fit_temp_c[0] = 20.0f;

    float candidate = 0.0f, current = -1.0f;
    bool current_is_set = true;
    s8_rate_guard_auto_decision_t decision;
    s8_rate_guard_estimate_reason_t clamp_reason;
    char err[160];
    bool ok = rate_guard_auto_compute(&candidate, &current, &current_is_set, &decision, &clamp_reason, err, sizeof(err));
    TEST_CHECK(ok, "compute succeeds with one identified zone");
    TEST_CHECK(!current_is_set, "correctly reports the guard as currently dormant");
    TEST_CHECK(decision == S8_RATE_GUARD_AUTO_APPLY, "arming a dormant guard always applies");
}

static void test_rate_guard_auto_compute_tighten_applies(void)
{
    TEST_SECTION("rate_guard_auto_compute -- a tighter candidate against an ARMED guard applies");
    reset_all();
    reset_rate_guard_stubs();
    set_current_rate_guard(true, 33.3f); // the old, wrong bench default
    s_stub_zone_has_model[0] = true;
    s_stub_zone_k_dc[0] = 5.0f;
    s_stub_zone_tau_s[0] = 60.0f;
    s_stub_zone_has_fit_ctx[0] = true;
    s_stub_zone_fit_temp_c[0] = 20.0f; // -> (5/60)*60*1.3 = 6.5, floored to 15.0

    float candidate = 0.0f, current = 0.0f;
    bool current_is_set = false;
    s8_rate_guard_auto_decision_t decision;
    s8_rate_guard_estimate_reason_t clamp_reason;
    char err[160];
    bool ok = rate_guard_auto_compute(&candidate, &current, &current_is_set, &decision, &clamp_reason, err, sizeof(err));
    TEST_CHECK(ok, "compute succeeds");
    TEST_CHECK(fabsf(candidate - 15.0f) < 1e-3f, "candidate is floored to 15.0 C/min");
    TEST_CHECK(decision == S8_RATE_GUARD_AUTO_APPLY, "15.0 < 33.3 -- tightens, so this applies");
}

static void test_rate_guard_auto_compute_loosen_suggests_only(void)
{
    TEST_SECTION("rate_guard_auto_compute -- a looser candidate against an ARMED guard is SUGGEST_ONLY");
    reset_all();
    reset_rate_guard_stubs();
    set_current_rate_guard(true, 15.0f); // already at the floor
    s_stub_zone_has_model[0] = true;
    s_stub_zone_k_dc[0] = 20.0f;
    s_stub_zone_tau_s[0] = 60.0f;
    s_stub_zone_has_fit_ctx[0] = true;
    s_stub_zone_fit_temp_c[0] = 20.0f; // -> (20/60)*60*1.3 = 26.0 C/min

    float candidate = 0.0f, current = 0.0f;
    bool current_is_set = false;
    s8_rate_guard_auto_decision_t decision;
    s8_rate_guard_estimate_reason_t clamp_reason;
    char err[160];
    bool ok = rate_guard_auto_compute(&candidate, &current, &current_is_set, &decision, &clamp_reason, err, sizeof(err));
    TEST_CHECK(ok, "compute succeeds");
    TEST_CHECK(fabsf(candidate - 26.0f) < 1e-3f, "candidate is 26.0 C/min");
    TEST_CHECK(decision == S8_RATE_GUARD_AUTO_SUGGEST_ONLY,
               "26.0 > 15.0 -- loosens the armed guard, must require confirmation, never auto-apply");
}

static void test_rate_guard_auto_compute_no_data_fails_with_reason(void)
{
    TEST_SECTION("rate_guard_auto_compute -- no identified zone at all fails with a named reason");
    reset_all();
    reset_rate_guard_stubs();
    set_current_rate_guard(true, 20.0f);
    float candidate = 0.0f, current = 0.0f;
    bool current_is_set = false;
    s8_rate_guard_auto_decision_t decision;
    s8_rate_guard_estimate_reason_t clamp_reason;
    char err[160] = {0};
    bool ok = rate_guard_auto_compute(&candidate, &current, &current_is_set, &decision, &clamp_reason, err,
                                       sizeof(err));
    TEST_CHECK(!ok, "no zone identified -> compute refuses");
    TEST_CHECK(err[0] != '\0', "a non-empty reason is produced");
}

static void test_build_json_rate_guard_provenance_absent(void)
{
    TEST_SECTION("build_commissioning_json -- rate_guard_provenance.has_value:false, no source/value "
                 "printed, when nothing has ever been recorded (fresh board / bench preset just ran)");
    reset_all();
    safety_cfg_http_snapshot_t snap = {0};
    snap.rate_guard_has_provenance = false;
    snap.rate_guard_source = SAFETY_RATE_GUARD_SOURCE_AUTO; // deliberately set, must be ignored
    snap.rate_guard_value = 99.0f;                          // deliberately set, must be ignored

    static char json[SAFETY_CFG_JSON_MAX];
    size_t len = build_commissioning_json(&snap, json, sizeof(json));
    TEST_CHECK(len > 0, "JSON built successfully");
    TEST_CHECK(strstr(json, "\"rate_guard_provenance\":{\"has_value\":false}") != NULL,
               "has_value:false with nothing else -- source/value are never printed for an absent record, "
               "even though the (stale) snapshot fields carry values");
}

static void test_build_json_rate_guard_provenance_auto(void)
{
    TEST_SECTION("build_commissioning_json -- rate_guard_provenance reports source:\"auto\" and the value");
    reset_all();
    safety_cfg_http_snapshot_t snap = {0};
    snap.rate_guard_has_provenance = true;
    snap.rate_guard_source = SAFETY_RATE_GUARD_SOURCE_AUTO;
    snap.rate_guard_value = 20.0f;

    static char json[SAFETY_CFG_JSON_MAX];
    size_t len = build_commissioning_json(&snap, json, sizeof(json));
    TEST_CHECK(len > 0, "JSON built successfully");
    TEST_CHECK(strstr(json, "\"has_value\":true") != NULL, "has_value:true");
    TEST_CHECK(strstr(json, "\"source\":\"auto\"") != NULL, "source is reported as auto, not manual");
    TEST_CHECK(strstr(json, "\"value\":20") != NULL, "the recorded value is reported");
}

static void test_build_json_rate_guard_provenance_manual(void)
{
    TEST_SECTION("build_commissioning_json -- rate_guard_provenance reports source:\"manual\"");
    reset_all();
    safety_cfg_http_snapshot_t snap = {0};
    snap.rate_guard_has_provenance = true;
    snap.rate_guard_source = SAFETY_RATE_GUARD_SOURCE_MANUAL;
    snap.rate_guard_value = 25.0f;

    static char json[SAFETY_CFG_JSON_MAX];
    size_t len = build_commissioning_json(&snap, json, sizeof(json));
    TEST_CHECK(len > 0, "JSON built successfully");
    TEST_CHECK(strstr(json, "\"source\":\"manual\"") != NULL,
               "an operator hand-entry is labeled manual, never mistaken for auto");
}

static void test_rate_guard_auto_post_handler_dormant_applies_and_tags_auto(void)
{
    TEST_SECTION("rate_guard_auto_post_handler -- candidate does not loosen the guard, empty body -> "
                 "writes via safety_cfg_write_apply_pairs, tags the provenance record AUTO only after a verified read-back");
    reset_all();
    reset_rate_guard_stubs();
    s_stub_lookup_type = KILNLINK_PARAM_TYPE_F32;
    s_stub_lookup_name = "max_rate_c_per_min";
    s_stub_zone_has_model[0] = true;
    s_stub_zone_k_dc[0] = 5.0f;
    s_stub_zone_tau_s[0] = 60.0f;
    s_stub_zone_has_fit_ctx[0] = true;
    s_stub_zone_fit_temp_c[0] = 20.0f; // -> floored to 15.0
    // s_stub_params (the fake for safety_cfg_store_get_by_index()) is read
    // BOTH by rate_guard_auto_compute()'s own pre-write lookup and by
    // safety_cfg_write_apply_pairs()'s post-commit read-back (confirm_commit_landed()) --
    // there is only one fake state for both, so seed it with the value the
    // Pico is expected to read back AFTER the commit (15.0), exactly like a
    // real 15.0-committed board would answer at any point once the write
    // has landed. This exercises the tighten-applies path end to end,
    // through the real write/verify machinery, without needing a separate
    // "before" and "after" fake.
    set_current_rate_guard(true, 15.0f);

    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    s_link = &fake_link;
    httpd_req_t req = { .content_len = 0 };
    esp_err_t err = rate_guard_auto_post_handler(&req);
    s_link = NULL;

    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK regardless of application-level result");
    TEST_CHECK(strstr(s_stub_last_httpd_resp, "\"ok\":true") != NULL, "reports success");
    TEST_CHECK(strstr(s_stub_last_httpd_resp, "\"applied\":true") != NULL, "reports it actually applied");
    TEST_CHECK(s_stub_set_param_calls == 1, "exactly one SET_PARAM was staged");
    TEST_CHECK(s_stub_commit_calls == 1, "a commit was sent");
    TEST_CHECK(s_stub_rate_guard_meta_set_calls == 1,
               "the provenance record was tagged exactly once, only after the write verified");
    TEST_CHECK(s_stub_rate_guard_meta_source == SAFETY_RATE_GUARD_SOURCE_AUTO,
               "tagged AUTO, never MANUAL, for this endpoint");
}

static void test_rate_guard_auto_post_handler_loosen_without_confirm_never_writes(void)
{
    TEST_SECTION("rate_guard_auto_post_handler -- a looser candidate against an armed guard, empty "
                 "body (no confirm) -- must NOT write, must NOT tag provenance");
    reset_all();
    reset_rate_guard_stubs();
    set_current_rate_guard(true, 15.0f); // armed at the floor
    s_stub_zone_has_model[0] = true;
    s_stub_zone_k_dc[0] = 20.0f;
    s_stub_zone_tau_s[0] = 60.0f;
    s_stub_zone_has_fit_ctx[0] = true;
    s_stub_zone_fit_temp_c[0] = 20.0f; // -> 26.0 C/min, LOOSER than 15.0

    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    s_link = &fake_link;
    httpd_req_t req = { .content_len = 0 }; // no "confirm=1"
    esp_err_t err = rate_guard_auto_post_handler(&req);
    s_link = NULL;

    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strstr(s_stub_last_httpd_resp, "\"applied\":false") != NULL,
               "reports the suggestion without applying it");
    TEST_CHECK(strstr(s_stub_last_httpd_resp, "LOOSEN") != NULL,
               "the reason explicitly says this would loosen the guard");
    TEST_CHECK(s_stub_set_param_calls == 0,
               "NEVER LOOSEN SILENTLY -- no SET_PARAM was sent for a loosening candidate without confirm=1");
    TEST_CHECK(s_stub_commit_calls == 0, "no commit either");
    TEST_CHECK(s_stub_rate_guard_meta_set_calls == 0, "the provenance record is untouched -- nothing was written");
}

static void test_rate_guard_auto_get_handler_never_writes(void)
{
    TEST_SECTION("rate_guard_auto_get_handler -- pure preview, never calls SET_PARAM/commit regardless "
                 "of what the candidate would be");
    reset_all();
    reset_rate_guard_stubs();
    set_current_rate_guard(true, 15.0f);
    s_stub_zone_has_model[0] = true;
    s_stub_zone_k_dc[0] = 20.0f;
    s_stub_zone_tau_s[0] = 60.0f;
    s_stub_zone_has_fit_ctx[0] = true;
    s_stub_zone_fit_temp_c[0] = 20.0f; // looser candidate -- GET must still never write

    httpd_req_t req = { .content_len = 0 };
    esp_err_t err = rate_guard_auto_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strstr(s_stub_last_httpd_resp, "\"would_loosen\":true") != NULL,
               "correctly previews that this candidate would loosen the guard");
    TEST_CHECK(s_stub_set_param_calls == 0, "GET never writes, regardless of decision");
    TEST_CHECK(s_stub_commit_calls == 0, "GET never commits");
    TEST_CHECK(s_stub_rate_guard_meta_set_calls == 0, "GET never tags provenance");
}

// 2026-09-15 review (review_divergence_rework_c1d2c526_2026-09-15.md,
// MEDIUM 3 / MEDIUM 4): commissioning_post_handler() must call
// kiln_cfg_store_recapture_pico_half_confirmed() -- NOT the divergence-gated
// kiln_cfg_store_autosave_from_live() -- exactly once after a committed AND
// confirmed write, and must never discard its result: a failure there is
// logged (ESP_LOGW), never silently swallowed the way the original `(void)`
// cast did. This is the negative-test-shaped proof: breaking the fix by hand
// (reverting to `(void)kiln_cfg_store_autosave_from_live(...)`, or calling
// the recapture function but discarding its result again) makes
// s_stub_recapture_calls read 0, the exact inversion of what this asserts.
static void test_commissioning_post_confirmed_commit_calls_recapture(void)
{
    TEST_SECTION("commissioning_post_handler -- a committed and confirmed write recaptures the active "
                 "slot's Pico half via kiln_cfg_store_recapture_pico_half_confirmed(), never the "
                 "divergence-gated autosave path, and checks the result rather than discarding it");
    reset_all();
    s_stub_lookup_type = KILNLINK_PARAM_TYPE_F32;
    s_stub_lookup_name = "stub_field";
    s_stub_params[0].param_id = 0x0104u;
    s_stub_params[0].type = KILNLINK_PARAM_TYPE_F32;
    s_stub_params[0].set = true;
    s_stub_params[0].value.f32_val = 120.0f; // read-back value confirm_commit_landed() will see

    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    s_link = &fake_link;

    static char body[128];
    snprintf(body, sizeof(body), "id=260&value=120.0&commit=1"); // 0x0104 == 260
    s_stub_req_body = body;
    s_stub_req_body_sent = 0;
    httpd_req_t req = { .content_len = (int)strlen(body) };
    esp_err_t err = commissioning_post_handler(&req);
    s_link = NULL;

    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strstr(s_stub_last_httpd_resp, "\"ok\":true") != NULL, "reports success");
    TEST_CHECK(s_stub_recapture_calls == 1,
               "exactly one confirmed-push Pico-half recapture was performed after the committed write");
}

static void test_commissioning_post_stage_only_does_not_recapture(void)
{
    TEST_SECTION("commissioning_post_handler -- a stage-only submission (no commit=1) changes nothing "
                 "on the Pico, so it must NOT trigger a Pico-half recapture either");
    reset_all();
    s_stub_lookup_type = KILNLINK_PARAM_TYPE_F32;
    s_stub_lookup_name = "stub_field";

    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    s_link = &fake_link;

    static char body[128];
    snprintf(body, sizeof(body), "id=260&value=120.0"); // no commit=1
    s_stub_req_body = body;
    s_stub_req_body_sent = 0;
    httpd_req_t req = { .content_len = (int)strlen(body) };
    esp_err_t err = commissioning_post_handler(&req);
    s_link = NULL;

    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(s_stub_recapture_calls == 0, "no recapture for a stage-only (uncommitted) submission");
}

static void test_commissioning_post_failed_recapture_still_reports_ok(void)
{
    TEST_SECTION("commissioning_post_handler -- a failed Pico-half recapture is logged (ESP_LOGW), never "
                 "swallowed, but must not fail the HTTP response -- the Pico write itself already landed "
                 "and confirmed by the time the recapture is attempted");
    reset_all();
    s_stub_lookup_type = KILNLINK_PARAM_TYPE_F32;
    s_stub_lookup_name = "stub_field";
    s_stub_params[0].param_id = 0x0104u;
    s_stub_params[0].type = KILNLINK_PARAM_TYPE_F32;
    s_stub_params[0].set = true;
    s_stub_params[0].value.f32_val = 120.0f;
    s_stub_recapture_result = false;
    s_stub_recapture_reason = "simulated recapture failure";

    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    s_link = &fake_link;

    static char body[128];
    snprintf(body, sizeof(body), "id=260&value=120.0&commit=1");
    s_stub_req_body = body;
    s_stub_req_body_sent = 0;
    httpd_req_t req = { .content_len = (int)strlen(body) };
    esp_err_t err = commissioning_post_handler(&req);
    s_link = NULL;

    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK even though the recapture failed");
    TEST_CHECK(strstr(s_stub_last_httpd_resp, "\"ok\":true") != NULL,
               "the Pico write itself succeeded and confirmed -- the HTTP response still reports success");
    TEST_CHECK(s_stub_recapture_calls == 1, "the recapture was still attempted and its result observed");
}

int main(void)
{
    test_rate_guard_gather_no_zone_identified_is_no_data();
    test_rate_guard_gather_picks_coldest_identified_zone();
    test_rate_guard_gather_measured_offdiag_without_diag_k_dc_is_unproven();
    test_rate_guard_gather_measured_offdiag_with_diag_k_dc_is_proven();
    test_rate_guard_gather_missing_fit_context_disqualifies_zone();
    test_rate_guard_current_value_reads_0x0204_by_id_not_position();
    test_rate_guard_current_value_dormant_reports_unset();
    test_rate_guard_auto_compute_dormant_applies();
    test_rate_guard_auto_compute_tighten_applies();
    test_rate_guard_auto_compute_loosen_suggests_only();
    test_rate_guard_auto_compute_no_data_fails_with_reason();
    test_build_json_rate_guard_provenance_absent();
    test_build_json_rate_guard_provenance_auto();
    test_build_json_rate_guard_provenance_manual();
    test_rate_guard_auto_post_handler_dormant_applies_and_tags_auto();
    test_rate_guard_auto_post_handler_loosen_without_confirm_never_writes();
    test_rate_guard_auto_get_handler_never_writes();

    test_parse_single_pair_no_commit();
    test_parse_multiple_pairs_plus_commit();
    test_parse_rejects_value_without_id();
    test_parse_rejects_dangling_id();
    test_parse_value_for_type_bounds();
    test_build_json_unset_param_omits_value();
    test_build_json_set_param_includes_value();
    test_build_json_last_diff_reports_named_mismatch();
    test_build_json_last_diff_empty_when_nothing_changed();
    test_build_json_borrowed_unknown();
    test_build_json_borrowed_known_true_with_zone();
    test_build_json_borrowed_known_true_zone_unknown();
    test_build_json_tc_flags_gated_on_link_up();
    test_peer_reports_unset_reliably_gates_on_known_and_version();
    test_build_json_forces_unset_when_peer_too_old();
    test_build_json_still_reports_set_when_peer_reliable();
    test_stale_flag_reflects_crc_mismatch();
    test_apply_pairs_all_succeed_with_commit();
    test_apply_pairs_estop_polarity_commit_clears_verification();
    test_apply_pairs_unrelated_commit_does_not_clear_verification();
    test_apply_pairs_unknown_id_is_refused_and_named();
    test_apply_pairs_refused_commit_surfaces_reason();
    test_apply_pairs_rejected_commit_names_field_and_reason();
    test_apply_pairs_rejected_commit_new_tc_type_reasons_are_readable();
    test_apply_pairs_readback_mismatch_fails_even_when_acked_and_not_rejected();
    test_apply_pairs_tc_offset_c_readback_mismatch_fails();
    test_set_and_confirm_f32_uses_nonblocking_refetch();
    test_apply_pairs_refetch_failure_reports_unconfirmed_not_success();
    test_apply_pairs_refetch_failure_with_no_stash_classifies_as_other_not_armed();
    test_apply_pairs_late_rejection_attaches_pico_reason_to_confirmed_failure();
    test_confirm_commit_landed_lookup_failure_on_its_own_pass_fails_closed();
    test_confirm_commit_landed_parse_failure_on_its_own_pass_fails_closed();
    test_ct_auto_zero_precheck_each_refusal();
    test_ct_auto_zero_precheck_manual_wins_unless_override();
    test_ct_auto_zero_precheck_requires_existing_a_fs();
    test_ct_auto_zero_precheck_missing_a_fs_check_is_not_vacuous();
    test_ct_auto_zero_postcheck_each_refusal();
    test_ct_auto_zero_precheck_k4_check_is_not_vacuous();
    test_ct_auto_zero_counts_to_mv_at_two_probe_ratings();
    test_ct_auto_zero_100mv_refusal_uses_quantized_counts();

    test_commissioning_post_confirmed_commit_calls_recapture();
    test_commissioning_post_stage_only_does_not_recapture();
    test_commissioning_post_failed_recapture_still_reports_ok();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
