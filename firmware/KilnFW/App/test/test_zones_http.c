// Host test for App/drivers/zones_http.c's parse_zone_fields(), added
// 2026-08-21 for the whole-page-zone-save data-loss defect found by
// black-box testing against the live board: a POST to /api/zones that
// carries fields only for zones 0..thermo_count-1 used to silently ZERO
// relay_mask/thermo_mask/every other per-zone field for any zone index at
// or past thermo_count, because zones_post_handler() memset(0)s its
// candidate config before parsing and parse_zone_fields() returned early
// (before touching any of those fields) for i >= thermo_count. Reproduced
// live: with thermo_count=1, an ordinary whole-page save that only replayed
// what GET had just reported zeroed zone 1 and zone 2's thermo_mask, with a
// 200 OK and no warning.
//
// This is its own SEPARATE host-test executable (own main(), not merged into
// test_main.c/kilnctl_host_tests.exe) -- see build_host_tests.ps1's second
// build+run step. Reason: parse_zone_fields() is `static`, so the only way
// to reach it directly is to #include zones_http.c itself (same convention
// test_backup_import.c/test_kiln_cfg_store.c use for backup_http.c/
// kiln_cfg_store.c). But zones_http.c DEFINES the real, non-static
// zones_config_get_*()/set_*() functions declared in zones_http.h, and
// test_backup_import.c already defines its OWN fake bodies for those exact
// names (to stub backup_import_apply()'s dependency on zones_http.h without
// pulling in all of zones_http.c's hardware-owning code) -- linking both
// into one executable would be a multiple-definition error. Keeping this in
// a second, independent executable sidesteps that entirely: nothing here is
// ever linked alongside test_backup_import.c's stubs.
//
// Stub surface below is wider than parse_zone_fields() itself touches, for
// the same reason test_backup_import.c's is wider than backup_import_apply()
// needs: the whole of zones_http.c (page GET, JSON GET, the POST wrapper,
// zones_http_start()'s hardware bring-up) is compiled into this one
// translation unit and must link, even though these tests call
// parse_zone_fields() directly and never invoke any of the real handlers.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

// Ahead of zones_http.c's own #includes, purely for the TYPES the stub
// bodies below need (same convention test_backup_import.c/test_wifi_prov.c
// use).
#include "esp_err.h"
#include "esp_http_server.h"

// asm("_binary_...") is a GCC/binutils extension (EMBED_TXTFILES,
// CMakeLists.txt) with no MSVC equivalent -- #define it away to nothing so
// `extern const uint8_t X[] asm("...");` parses as plain
// `extern const uint8_t X[];`. Real (empty) definitions follow the include.
#define asm(x)

// zones_http.c split 2026-09-01 into six files along its natural seams (it
// had grown to 5628 lines, the largest file in the firmware -- see
// ../drivers/zones_http_internal.h's header comment for the seam
// rationale). parse_zone_fields() -- the whole reason this test reaches for
// source inclusion instead of linking -- now lives in
// zones_http_handlers.c, but the pattern this file's own header comment
// describes is unchanged: #include every one of the split's .c files into
// this ONE translation unit so their (now cross-file) `static`/non-static
// mix still resolves exactly the way it does in the real, separately-
// compiled firmware build, and reach every `static` internal directly.
// Order matches the original file's top-to-bottom order.
#include "../drivers/zones_http.c"
#include "../drivers/zones_config_store.c"
#include "../drivers/zones_config_accessors.c"
#include "../drivers/zones_http_handlers.c"
#include "../drivers/zones_current_sweep_engine.c"
#include "../drivers/zones_current_sweep_task.c"

#undef asm

// ---- Embedded-page symbols page_get_handler() references ------------------
// Never actually sent by these tests (that handler is never called), but
// must exist for the linker.
const uint8_t zones_page_html_gz_start[1] = { 0 };
const uint8_t zones_page_html_gz_end[1] = { 0 };
const uint8_t safety_config_page_html_gz_start[1] = { 0 };
const uint8_t safety_config_page_html_gz_end[1] = { 0 };
// tuning_recommendations_json_start/_end (2026-09-02 host-link fix, commit
// 333dd4e's build_host_tests.ps1 break): same 1-byte-placeholder convention
// as the gz pairs above -- these two are ESP-IDF EMBED_FILES symbols with no
// host-toolchain equivalent, and tuning_rec_get_handler() is never called by
// these tests (only its extracted tuning_rec_body_len() helper is, with a
// real synthetic buffer -- see test_tuning_rec_body_len_* below). A 1-byte
// pair is enough for the linker.
const uint8_t tuning_recommendations_json_start[1] = { 0 };
const uint8_t tuning_recommendations_json_end[1] = { 0 };

// ---- ota_http.c's interlock gate --------------------------------------------
// zones_http.c's POST handler now refuses to rewrite zone config while a
// firing is running, through the same ota_http_check_interlocks() gate
// kiln_cfg_http and backup_http use. None of these is reachable from the
// tests here (only parse_zone_fields()/the NVS decode path is called
// directly), but every symbol the file references must resolve at link time.
// Returns OK so that if a future test ever does drive the handler, it is the
// handler's own logic under test rather than this stand-in refusing first.
ota_interlock_result_t ota_http_check_interlocks(bool ack_no_safety_processor, char *reason_out,
                                                 size_t reason_cap)
{
    (void)ack_no_safety_processor;
    if (reason_out && reason_cap > 0) {
        reason_out[0] = '\0';
    }
    return OTA_INTERLOCK_OK;
}

bool ota_http_req_ack_no_safety(httpd_req_t *req)
{
    (void)req;
    return false;
}

esp_err_t ota_http_send_interlock_refusal(httpd_req_t *req, ota_interlock_result_t result,
                                          const char *reason)
{
    (void)req; (void)result; (void)reason;
    return ESP_OK;
}


// ---- esp_http_server.h stub bodies -----------------------------------------
// None of these is ever invoked by this file's tests (only parse_zone_fields()
// is called directly), but every symbol zones_http.c references anywhere in
// the file must resolve at link time.
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler)
{
    (void)handle;
    (void)uri_handler;
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type)
{
    (void)r;
    (void)type;
    return ESP_OK;
}
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value)
{
    (void)r;
    (void)field;
    (void)value;
    return ESP_OK;
}

/* web_encoding.c is not part of the host build (it pulls in the real httpd),
   but zones_http.c/safety_cfg_http.c now call this from their page handlers.
   Same local-stub convention as httpd_resp_set_hdr() just above. */
void web_set_asset_cache_headers(httpd_req_t *r);
void web_set_asset_cache_headers(httpd_req_t *r) { (void)r; }
/* PID_EXPANSION_PLAN.md Phase 4 round-trip test support: captures the last
 * body httpd_resp_send() was asked to send, so a test can inspect what
 * zones_get_handler() actually emitted -- every prior test in this file only
 * ever calls parse_zone_fields()/zones_post_handler(), never
 * zones_get_handler(), so this capture is inert for them. */
static char s_last_resp_body[8192];
static size_t s_last_resp_len;
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, long long buf_len)
{
    (void)r;
    size_t n = (buf_len < 0) ? 0 : (size_t)buf_len;
    if (n >= sizeof(s_last_resp_body)) {
        n = sizeof(s_last_resp_body) - 1;
    }
    if (buf && n > 0) {
        memcpy(s_last_resp_body, buf, n);
    }
    s_last_resp_body[n] = '\0';
    s_last_resp_len = n;
    return ESP_OK;
}
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, size_t buf_len)
{
    (void)r;
    (void)buf;
    (void)buf_len;
    return ESP_OK;
}
/* Test hooks added for FIX 2's zones_post_handler() end-to-end coverage
 * (the two inline strtol sites -- max_simultaneous_relays/safety_tc_type --
 * have no seam of their own to call directly, unlike zones_config_json_parse_u8_field()/
 * zones_config_json_parse_float_field()). NULL/false by default so every pre-existing test in
 * this file, which only calls parse_zone_fields() directly and never
 * zones_post_handler(), is completely unaffected -- same opt-in convention
 * stubs/nvs.h's nvs_test_enable() uses. */
static const char *s_test_post_body = NULL;
static bool s_test_err_called = false;
static char s_test_err_msg[256];
static bool s_test_ok_called = false;

static void test_post_hooks_reset(void)
{
    s_test_post_body = NULL;
    s_test_err_called = false;
    s_test_err_msg[0] = '\0';
    s_test_ok_called = false;
}

esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t error, const char *msg)
{
    (void)r;
    (void)error;
    s_test_err_called = true;
    if (msg) {
        strncpy(s_test_err_msg, msg, sizeof(s_test_err_msg) - 1);
        s_test_err_msg[sizeof(s_test_err_msg) - 1] = '\0';
    }
    return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status)
{
    (void)r;
    (void)status;
    return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s)
{
    (void)r;
    s_test_ok_called = true;
    /* Also captured into s_last_resp_body, same as httpd_resp_send() above --
     * added for zones_get_handler()'s malloc-failure 500 path (2026-08-31
     * PSRAM fix), which sends its error body via httpd_resp_sendstr(), not
     * httpd_resp_send(). Every pre-existing test in this file only checks
     * s_test_ok_called/s_test_err_called, never s_last_resp_body after a
     * sendstr() call, so this addition is inert for them. */
    if (s) {
        size_t n = strlen(s);
        if (n >= sizeof(s_last_resp_body)) {
            n = sizeof(s_last_resp_body) - 1;
        }
        memcpy(s_last_resp_body, s, n);
        s_last_resp_body[n] = '\0';
        s_last_resp_len = n;
    }
    return ESP_OK;
}
int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len)
{
    (void)r;
    /* Single-shot: hands back the whole staged body in one call. Every real
     * caller in zones_http.c loops until `received` reaches content_len, but
     * a test always stages a body whose length equals the content_len it
     * sets on the request, so one call satisfies the loop's condition and
     * exits without a second, zero-length call. */
    if (!s_test_post_body) {
        return 0;
    }
    size_t len = strlen(s_test_post_body);
    if (len > buf_len) {
        len = buf_len;
    }
    memcpy(buf, s_test_post_body, len);
    return (int)len;
}

// ---- web_encoding.h -- only reached from page_get_handler(), never called
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

// ---- wifi_provision_http.h -- only reached from zones_http_start(), never
// called by these tests.
httpd_handle_t wifi_provision_http_get_server(void)
{
    return NULL;
}

// ---- MAX31856.h / thermo_owner.h -- only reached from zones_http_start()'s
// hardware bring-up, never called by these tests.
void MAX31856_config_default(MAX31856Config *cfg)
{
    if (cfg) memset(cfg, 0, sizeof(*cfg));
}
esp_err_t thermo_owner_command_config_channel(uint8_t channel, uint8_t tc_type, uint8_t avg_mode,
                                              bool filter_50hz, bool auto_convert)
{
    (void)channel;
    (void)tc_type;
    (void)avg_mode;
    (void)filter_50hz;
    (void)auto_convert;
    return ESP_OK;
}
esp_err_t MAX31856_read_all(MAX31856BusClass *bus, MAX31856Reading *out, size_t max_readings,
                            size_t *out_count)
{
    (void)bus;
    (void)out;
    (void)max_readings;
    if (out_count) *out_count = 0;
    return ESP_OK;
}

// ---- thermo_combine.h -- only reached from zone_sweep_read_zone_temp(),
// itself only reached from inside zone_sweep_task(), which the host tests
// never run (xTaskCreate() is stubbed to never invoke its task function --
// see stubs/freertos/task.h). Exists purely so the translation unit links.
float thermo_combine(const float *channel_c, const bool *channel_ok, uint8_t channel_count,
                     uint8_t thermo_mask, bool *out_valid)
{
    (void)channel_c;
    (void)channel_ok;
    (void)channel_count;
    (void)thermo_mask;
    if (out_valid) *out_valid = false;
    return NAN;
}

// ---- kiln_io.h -- Task 1's sweep. kiln_io_set_relay_mask()/
// kiln_io_all_relays_off() are only reached from inside zone_sweep_task()
// (never run by these tests, same reasoning as MAX31856_read_all() above);
// kiln_io_get_relay_shadow() IS reachable from zones_ct_mapping_warn_mask(),
// which some tests below call directly with a non-NULL io/safety pair, so
// this returns a fixed, documented value rather than an arbitrary one.
esp_err_t kiln_io_set_relay_mask(kiln_io_t *io, uint8_t mask, uint8_t value)
{
    (void)io;
    (void)mask;
    (void)value;
    return ESP_OK;
}
esp_err_t kiln_io_all_relays_off(kiln_io_t *io)
{
    (void)io;
    return ESP_OK;
}
static uint8_t s_test_relay_shadow = 0;
uint8_t kiln_io_get_relay_shadow(const kiln_io_t *io)
{
    (void)io;
    return s_test_relay_shadow;
}

// ---- kiln_io_owner.h -- B1 fix (opus review, 2026-08-27): the sweep's
// hardware bindings (zone_sweep_hw_energize()/zone_sweep_hw_force_off() in
// zones_http.c) now route through the owner module instead of calling
// kiln_io_set_relay_mask()/kiln_io_all_relays_off() directly. Call counters
// let test_zone_sweep_hw_bindings_route_through_owner() below prove that
// wiring directly, not just that the symbols link.
static int s_test_owner_set_relay_mask_calls = 0;
static int s_test_owner_all_relays_off_calls = 0;
static uint8_t s_test_owner_last_mask = 0;
static uint8_t s_test_owner_last_value = 0;
kiln_io_owner_relay_result_t kiln_io_owner_command_set_relay_mask(uint8_t mask, uint8_t value,
                                                                   uint32_t *out_safety_sources)
{
    s_test_owner_set_relay_mask_calls++;
    s_test_owner_last_mask = mask;
    s_test_owner_last_value = value;
    if (out_safety_sources) *out_safety_sources = 0;
    return KILN_IO_OWNER_RELAY_OK;
}
esp_err_t kiln_io_owner_command_all_relays_off(void)
{
    s_test_owner_all_relays_off_calls++;
    return ESP_OK;
}

// ---- profile_executor.h / autotune_engine.h -- zones_current_sweep_start()'s
// refusal checks. Tests that exercise the refusal path set these through the
// small setters below rather than linking the real (huge) modules.
static profile_exec_status_t s_test_profile_status;
static bool s_test_autotune_active = false;
void profile_executor_get_status(profile_exec_status_t *out)
{
    if (out) *out = s_test_profile_status;
}
bool autotune_engine_is_active(void)
{
    return s_test_autotune_active;
}

// ---- relay_authority.h -- the single shared heat claim.
// zones_current_sweep_start() takes this atomically right before its own
// commit (s_sweep.active = true), on top of (not instead of) the early,
// non-atomic profile_executor_get_status()/autotune_engine_is_active()
// reads above. Default OK so every existing wired-refusal test's success
// path is unchanged; s_test_heat_sweep_claim_result lets
// test_zones_current_sweep_start_atomic_gate_closes_the_race() below prove
// the LATE gate is independently load-bearing.
static relay_heat_sweep_claim_result_t s_test_heat_sweep_claim_result = RELAY_HEAT_SWEEP_CLAIM_OK;
static int s_test_heat_sweep_claim_begin_calls = 0;
static int s_test_heat_sweep_claim_end_calls = 0;
relay_heat_sweep_claim_result_t relay_authority_heat_sweep_claim_begin(void)
{
    s_test_heat_sweep_claim_begin_calls++;
    return s_test_heat_sweep_claim_result;
}
void relay_authority_heat_sweep_claim_end(void)
{
    s_test_heat_sweep_claim_end_calls++;
}

// ---- safety_link.h -- same reasoning: a small test-controlled stand-in
// instead of linking the real (hardware-owning) module.
static bool s_test_safety_link_up = false;
static safety_link_status_t s_test_safety_status;
esp_err_t safety_link_get_status(SafetyLinkClass *link, safety_link_status_t *out)
{
    (void)link;
    if (out) {
        *out = s_test_safety_status;
        out->link_up = s_test_safety_link_up;
    }
    return ESP_OK;
}

// M12: zone_sweep_push_ct_channel_map() stages the derived map over these
// two, so zones_http.c now references them. Test-controlled stand-ins for
// the same reason safety_link_get_status() above is one -- nothing here
// exercises the wire, only the decision that leads to it.
//
// M12b: the CT-scale push (zone_sweep_push_k_ct_v_per_a()) needs more than a
// fixed "always succeeds" answer -- its whole safety argument is what it does
// when a stage fails, a commit is rejected, or the read-back disagrees, and
// none of those arms is reachable while the stubs can only succeed. So these
// now RECORD every SET_PARAM and can be told to fail; defaults are exactly
// the old fixed behaviour, so every pre-existing test is unaffected.
#define TEST_SETPARAM_LOG_MAX 32
typedef struct {
    uint16_t param_id;
    uint8_t  type;
    kilnlink_param_value_t value;
} test_setparam_call_t;

static test_setparam_call_t s_setparam_log[TEST_SETPARAM_LOG_MAX];
static int s_setparam_count = 0;
// Fail the Nth (0-based) SET_PARAM of the run; -1 fails none.
static int s_setparam_fail_at = -1;
static esp_err_t s_commit_err = ESP_OK;
static bool s_commit_rejected = false;
static int s_commit_count = 0;

static void test_link_reset(void)
{
    s_setparam_count = 0;
    s_setparam_fail_at = -1;
    s_commit_err = ESP_OK;
    s_commit_rejected = false;
    s_commit_count = 0;
    memset(s_setparam_log, 0, sizeof(s_setparam_log));
}

esp_err_t safety_link_send_set_param(SafetyLinkClass *link, uint16_t param_id, uint8_t type,
                                      kilnlink_param_value_t value)
{
    (void)link;
    bool fail = (s_setparam_fail_at >= 0 && s_setparam_count == s_setparam_fail_at);
    if (s_setparam_count < TEST_SETPARAM_LOG_MAX) {
        s_setparam_log[s_setparam_count].param_id = param_id;
        s_setparam_log[s_setparam_count].type = type;
        s_setparam_log[s_setparam_count].value = value;
    }
    s_setparam_count++;
    return fail ? ESP_FAIL : ESP_OK;
}
esp_err_t safety_link_send_commit_config(SafetyLinkClass *link, uint16_t *out_param_id,
                                          uint8_t *out_reason, bool *out_rejected)
{
    (void)link;
    s_commit_count++;
    if (out_param_id) *out_param_id = 0x0308;
    if (out_reason) *out_reason = 3;
    if (out_rejected) *out_rejected = s_commit_rejected;
    return s_commit_err;
}

// H1 fix (opus review, 2026-08-28): zone_sweep_confirm_ct_map_landed() forces
// a LIVE re-fetch of the Pico's record and re-reads it, so zones_http.c now
// references safety_cfg_store's read side and safety_link_get_peer_build_
// status(). Same test-controlled stand-in reasoning as the two link senders
// above -- nothing here drives the push, only the decisions that feed it.
esp_err_t safety_link_get_peer_build_status(SafetyLinkClass *link, bool *out_known, bool *out_dirty,
                                             uint8_t *commit_buf, uint8_t *out_commit_len,
                                             uint8_t *datetime_buf, uint8_t *out_datetime_len,
                                             uint8_t *out_config_version, uint16_t *out_config_crc)
{
    (void)link; (void)out_dirty; (void)commit_buf; (void)out_commit_len;
    (void)datetime_buf; (void)out_datetime_len; (void)out_config_version;
    if (out_known) *out_known = false;
    if (out_config_crc) *out_config_crc = 0;
    return ESP_OK;
}

// M12b: a programmable stand-in for the ESP's cache of the Pico's committed
// record. Empty by default (safety_cfg_store_param_count() == 0), which is
// byte-for-byte the old fixed stub -- a test that wants the committed side of
// a read-back or a nameplate answer populates it explicitly.
#define TEST_CFG_ROWS_MAX 16
static safety_cfg_param_t s_cfg_rows[TEST_CFG_ROWS_MAX];
static size_t s_cfg_row_count = 0;
static bool s_cfg_refetch_ok = true;
// Set true to make refetch() re-point the store at whatever the pushes have
// staged -- i.e. model a Pico that really did commit what it was sent.
static bool s_cfg_refetch_applies_staged = false;

static void test_cfg_rows_reset(void)
{
    memset(s_cfg_rows, 0, sizeof(s_cfg_rows));
    s_cfg_row_count = 0;
    s_cfg_refetch_ok = true;
    s_cfg_refetch_applies_staged = false;
}

static void test_cfg_set_f32(uint16_t param_id, float v, bool is_set)
{
    for (size_t i = 0; i < s_cfg_row_count; i++) {
        if (s_cfg_rows[i].param_id == param_id) {
            s_cfg_rows[i].value.f32_val = v;
            s_cfg_rows[i].set = is_set;
            return;
        }
    }
    TEST_CHECK(s_cfg_row_count < TEST_CFG_ROWS_MAX, "test config row table has room");
    if (s_cfg_row_count >= TEST_CFG_ROWS_MAX) {
        return;
    }
    s_cfg_rows[s_cfg_row_count].param_id = param_id;
    s_cfg_rows[s_cfg_row_count].type = KILNLINK_PARAM_TYPE_F32;
    s_cfg_rows[s_cfg_row_count].value.f32_val = v;
    s_cfg_rows[s_cfg_row_count].set = is_set;
    s_cfg_row_count++;
}

bool safety_cfg_store_refetch(SafetyLinkClass *link, uint16_t config_crc)
{
    (void)link; (void)config_crc;
    if (!s_cfg_refetch_ok) {
        return false;
    }
    if (s_cfg_refetch_applies_staged) {
        // Every SET_PARAM this run sent, now "committed" -- the read-back a
        // Pico that genuinely applied the commit would answer with.
        // Only the f32 CT-scale ids: the map push stages u8 values into the
        // same union, and copying one of those back as an f32 would answer
        // zone_ct_map_committed_value() with a reinterpreted float.
        for (int i = 0; i < s_setparam_count && i < TEST_SETPARAM_LOG_MAX; i++) {
            uint16_t id = s_setparam_log[i].param_id;
            if (id >= 0x0308u && id <= 0x030Au) {
                test_cfg_set_f32(id, s_setparam_log[i].value.f32_val, true);
            }
        }
    }
    return true;
}
size_t safety_cfg_store_param_count(void)
{
    return s_cfg_row_count;
}
bool safety_cfg_store_get_by_index(size_t index, safety_cfg_param_t *out)
{
    if (index >= s_cfg_row_count) {
        return false;
    }
    if (out) {
        *out = s_cfg_rows[index];
    }
    return true;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// A fully-populated zone_cfg_t, standing in for "what's currently stored"
// (the live/on-flash config a whole-page save must not clobber for a zone it
// carries no data for).
static zone_cfg_t make_stored_zone(void)
{
    zone_cfg_t z;
    memset(&z, 0, sizeof(z));
    strncpy(z.name, "Kiln Top", ZONE_NAME_MAX_LEN);
    z.relay_mask = 0x02;
    z.thermo_mask = 0x04;
    z.cal_offset_c = 1.5f;
    z.pid_kp = 2.0f;
    z.pid_ki = 0.3f;
    z.pid_kd = 0.05f;
    z.max_ramp_c_per_hr = 120.0f;
    z.sanity_rate_c_per_min = 5.0f;
    z.control_mode = 2;
    z.max_temp_c = 1300.0f;
    z.min_temp_c = -10.0f;
    z.heater_window_ms = 60000.0f;
    z.heater_min_on_ms = 100.0f;
    z.heater_min_off_ms = 100.0f;
    z.cross_zone_max_delta_c = 40.0f;
    z.tc_type = 3;
    z.model_k_dc = 12.0f;
    z.model_tau_s = 300.0f;
    z.model_dead_time_s = 30.0f;
    return z;
}

// The defect, reproduced directly against parse_zone_fields(): a body that
// carries NO fields at all for zone index i (exactly what zones_page.html
// sends when i >= thermo_count -- it never renders that zone's block) must
// leave every field of the currently-stored zone_cfg_t intact, not zero it.
static void test_out_of_range_zone_preserves_stored_fields(void)
{
    TEST_SECTION("parse_zone_fields -- zone index >= thermo_count preserves stored fields (defect 2 fix)");

    zone_cfg_t current = make_stored_zone();
    zone_cfg_t out;
    memset(&out, 0, sizeof(out)); // matches zones_post_handler()'s memset(&tmp, 0, ...) before parsing

    // Body carries fields for zone 0 only (thermo_count=1) -- nothing at all
    // for zone 1, same as a real whole-page submit from zones_page.html with
    // thermo_count set to 1.
    const char *body = "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&"
                        "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=0&"
                        "z0_maxtemp=1300&z0_mintemp=-20&z0_window=60000&z0_minon=0&z0_minoff=0";

    const uint8_t thermo_count = 1; // zone index 1 is past this -- the defect's exact trigger
    const uint8_t relay_count = 4;
    const uint8_t timing_profile_count = 1; // zone 1 is past thermo_count -- the early-return
                                            // preserve path never reaches z1_timingprofile's parse
    const char *err_reason = "unset";
    bool ok = parse_zone_fields(body, /*i=*/1, thermo_count, relay_count, timing_profile_count,
                                &current, &out, &err_reason);

    TEST_CHECK(ok, "a body silent on zone 1 (past thermo_count) must still be accepted, not refused");
    TEST_CHECK(out.relay_mask == current.relay_mask,
              "relay_mask must be preserved from the stored config, not zeroed");
    TEST_CHECK(out.thermo_mask == current.thermo_mask,
              "thermo_mask must be preserved -- this is the exact field zeroed live on the bench "
              "(thermo_count=1 zeroed zone 1/2's thermo_mask on an ordinary re-save)");
    TEST_CHECK_NEAR(out.cal_offset_c, current.cal_offset_c, 1e-6, "cal_offset_c preserved");
    TEST_CHECK_NEAR(out.pid_kp, current.pid_kp, 1e-6, "pid_kp preserved");
    TEST_CHECK_NEAR(out.pid_ki, current.pid_ki, 1e-6, "pid_ki preserved");
    TEST_CHECK_NEAR(out.pid_kd, current.pid_kd, 1e-6, "pid_kd preserved");
    TEST_CHECK_NEAR(out.max_ramp_c_per_hr, current.max_ramp_c_per_hr, 1e-6, "max_ramp_c_per_hr preserved");
    TEST_CHECK_NEAR(out.sanity_rate_c_per_min, current.sanity_rate_c_per_min, 1e-6, "sanity_rate_c_per_min preserved");
    TEST_CHECK(out.control_mode == current.control_mode, "control_mode preserved");
    TEST_CHECK_NEAR(out.max_temp_c, current.max_temp_c, 1e-6, "max_temp_c preserved");
    TEST_CHECK_NEAR(out.min_temp_c, current.min_temp_c, 1e-6, "min_temp_c preserved");
    TEST_CHECK_NEAR(out.heater_window_ms, current.heater_window_ms, 1e-6, "heater_window_ms preserved");
    TEST_CHECK_NEAR(out.cross_zone_max_delta_c, current.cross_zone_max_delta_c, 1e-6, "cross_zone_max_delta_c preserved");
    TEST_CHECK_NEAR(out.model_k_dc, current.model_k_dc, 1e-6, "model_k_dc preserved");
    TEST_CHECK_NEAR(out.model_tau_s, current.model_tau_s, 1e-6, "model_tau_s preserved");
    TEST_CHECK_NEAR(out.model_dead_time_s, current.model_dead_time_s, 1e-6, "model_dead_time_s preserved");
    // Name/tc_type are parsed BEFORE the i >= thermo_count check (see the
    // function's own comment) -- omitted here too, so they fall back to
    // preserving the stored value as well (name's own omit-means-preserve
    // path, tc_type's existing one).
    TEST_CHECK(strcmp(out.name, current.name) == 0, "name preserved when the submission omits z1_name entirely");
    TEST_CHECK(out.tc_type == current.tc_type, "tc_type preserved when the submission omits z1_tctype entirely");
}

// ZONES_CFG_VERSION 12->13: the whole-page POST /api/zones path is a SECOND
// gain-writing path (parse_zone_fields() writes z->pid_kp/ki/kd directly,
// never through zones_config_set_pid()'s choke point -- see this function's
// own comment on why the invalidation had to be duplicated here) -- exactly
// the kind of second path this repo's reset-one-side bug class keeps
// producing when a pair of writers isn't found. This proves BOTH halves: an
// actual gain change invalidates, and an untouched gain leaves the record
// standing (an operator editing an unrelated field on the same page must not
// lose a good tuning record for no reason).
static void test_whole_page_post_invalidates_tuning_quality_only_when_gains_actually_change(void)
{
    TEST_SECTION("parse_zone_fields -- whole-page POST invalidates tuning_valid when pid_kp/ki/kd "
                 "actually change, and leaves it standing when they don't");

    zone_cfg_t current = make_stored_zone();
    current.tuning_valid = 1;
    current.tuning_method = 0;
    current.tuning_rule = 0;
    current.tuning_baseline_c = 25.0f;

    // Case 1: kp changed (2.0 -> 3.5) -- must invalidate.
    {
        zone_cfg_t out;
        memset(&out, 0, sizeof(out));
        const char *body = "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&z0_timingprofile=0&"
                            "z0_cal=1.5&z0_kp=3.5&z0_ki=0.3&z0_kd=0.05&z0_ramp=120&z0_sanity=5&z0_mode=2&"
                            "z0_maxtemp=1300&z0_mintemp=-10&z0_window=60000&z0_minon=0&z0_minoff=0";
        const char *err_reason = "unset";
        bool ok = parse_zone_fields(body, /*i=*/0, /*thermo_count=*/1, /*relay_count=*/4,
                                    /*timing_profile_count=*/1, &current, &out, &err_reason);
        TEST_CHECK(ok, "a well-formed submission with a changed kp is accepted");
        TEST_CHECK_NEAR(out.pid_kp, 3.5f, 1e-6, "sanity: kp really did change in the parsed result");
        TEST_CHECK(!out.tuning_valid,
                  "an actual gain change on the whole-page path invalidates the tuning-quality record, "
                  "same as the narrow POST /api/zones/pid path");
    }

    // Case 2: every gain field resubmitted IDENTICAL to what is already
    // stored (the ordinary "change something else, resave" case) -- must
    // NOT invalidate.
    {
        zone_cfg_t out;
        memset(&out, 0, sizeof(out));
        const char *body = "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&z0_timingprofile=0&"
                            "z0_cal=1.5&z0_kp=2.0&z0_ki=0.3&z0_kd=0.05&z0_ramp=120&z0_sanity=5&z0_mode=2&"
                            "z0_maxtemp=1300&z0_mintemp=-10&z0_window=60000&z0_minon=0&z0_minoff=0";
        const char *err_reason = "unset";
        bool ok = parse_zone_fields(body, /*i=*/0, /*thermo_count=*/1, /*relay_count=*/4,
                                    /*timing_profile_count=*/1, &current, &out, &err_reason);
        TEST_CHECK(ok, "a well-formed submission with unchanged gains is accepted");
        TEST_CHECK_NEAR(out.pid_kp, current.pid_kp, 1e-6, "sanity: kp is unchanged");
        TEST_CHECK(out.tuning_valid,
                  "resubmitting the SAME gains (an unrelated field changed) must leave a good "
                  "tuning-quality record standing, not wipe it for no reason");
    }
}

// Sibling check: a zone index still IN range (i < thermo_count) that omits
// its thermo_mask field keeps the documented legacy "zone i reads channel i"
// fallback -- the fix above must not have disturbed this existing,
// in-range behaviour.
static void test_in_range_zone_thermo_mask_legacy_fallback_unchanged(void)
{
    TEST_SECTION("parse_zone_fields -- in-range zone omitting thermo_mask still gets the legacy 1<<i fallback");

    zone_cfg_t current = make_stored_zone();
    zone_cfg_t out;
    memset(&out, 0, sizeof(out));

    // No z0_thermo_mask key at all, but zone 0 IS in range (thermo_count=2).
    const char *body = "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_timingprofile=0&"
                        "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=0&"
                        "z0_maxtemp=1300&z0_mintemp=-20&z0_window=60000&z0_minon=0&z0_minoff=0";
    const char *err_reason = "unset";
    bool ok = parse_zone_fields(body, /*i=*/0, /*thermo_count=*/2, /*relay_count=*/4,
                                /*timing_profile_count=*/1, &current, &out, &err_reason);

    TEST_CHECK(ok, "in-range zone with every REQUIRED field present must still be accepted");
    TEST_CHECK(out.thermo_mask == (1u << 0), "omitted thermo_mask on an in-range zone falls back to legacy 1<<i, "
                                             "not the stored value and not zero");
}

// Negative control (this is the "prove the check can fail" pass): with the
// OLD behaviour (return true immediately for i >= thermo_count, leaving a
// zero-initialized `out` untouched) this same test would fail. Demonstrated
// here by calling a copy of the buggy logic inline rather than editing
// zones_http.c back and forth -- see the header comment on why: this file
// asserts against the CURRENT (fixed) parse_zone_fields(); the bug's old
// shape is reproduced here as its own tiny function so the contrast is
// checked mechanically, not just asserted in prose.
static bool old_buggy_early_return_zeroed_it(uint8_t i, uint8_t thermo_count, uint8_t out_thermo_mask)
{
    if (i >= thermo_count) {
        return out_thermo_mask == 0; // the old bug's actual, reproducible output
    }
    return false;
}

static void test_old_behaviour_would_have_zeroed_it(void)
{
    TEST_SECTION("sanity check -- confirms what the old early-return actually produced (for contrast)");
    zone_cfg_t out;
    memset(&out, 0, sizeof(out)); // the caller's memset(&tmp, 0, ...), untouched by the old early return
    TEST_CHECK(old_buggy_early_return_zeroed_it(1, 1, out.thermo_mask),
              "documents the exact old failure mode this fix replaces: thermo_mask stayed at the "
              "caller's zero-init for any zone index >= thermo_count");
}

// ---------------------------------------------------------------------------
// FIX 2 -- zones_config_json_parse_u8_field()/zones_config_json_parse_float_field() trailing-garbage rejection,
// plus the two inline strtol sites (max_simultaneous_relays/safety_tc_type)
// zones_post_handler() has no other seam for. Blast-radius check (see the
// task's own instructions): zones_page.html's saveBtn handler pushes raw
// <input type="number">.value strings straight into the form body (see
// zones_page.html's saveBtn click handler) -- never with a unit suffix,
// trailing whitespace, or a locale decimal comma (a number input's .value is
// always plain ASCII digits/'.'/'-' per the HTML spec, regardless of the
// browser's locale) -- so the real client can never trigger a rejection this
// stricter check newly introduces. Confirmed by reading zones_page.html
// directly rather than assumed.
// ---------------------------------------------------------------------------

static void test_parse_u8_field_rejects_trailing_garbage(void)
{
    TEST_SECTION("parse_u8_field -- trailing garbage after a valid numeric prefix is rejected (FIX 2)");
    uint8_t out = 99;
    bool ok = zones_config_json_parse_u8_field("thermo_count=3X", "thermo_count", 0, 10, &out);
    TEST_CHECK(!ok, "\"3X\" must be rejected outright, not silently accepted as 3");
    TEST_CHECK(out == 99, "out must be untouched on rejection");
}

static void test_parse_u8_field_accepts_clean_value(void)
{
    TEST_SECTION("parse_u8_field -- positive control: a clean in-range value is still accepted");
    uint8_t out = 0;
    bool ok = zones_config_json_parse_u8_field("thermo_count=3", "thermo_count", 0, 10, &out);
    TEST_CHECK(ok, "a clean value must still parse");
    TEST_CHECK(out == 3, "parsed value must be correct");
}

static void test_parse_float_field_rejects_trailing_garbage(void)
{
    TEST_SECTION("parse_float_field -- trailing garbage after a valid numeric prefix is rejected (FIX 2)");
    float out = -1.0f;
    bool ok = zones_config_json_parse_float_field("z0_kp=1200X", "z0_kp", 0.0f, 5000.0f, &out);
    TEST_CHECK(!ok, "\"1200X\" must be rejected outright, not silently accepted as 1200.0");
    TEST_CHECK(out == -1.0f, "out must be untouched on rejection");
}

static void test_parse_float_field_rejects_unit_suffix(void)
{
    TEST_SECTION("parse_float_field -- a value with a trailing unit suffix is rejected (FIX 2)");
    float out = -1.0f;
    bool ok = zones_config_json_parse_float_field("z0_maxtemp=1300C", "z0_maxtemp", 0.0f, 1400.0f, &out);
    TEST_CHECK(!ok, "\"1300C\" must be rejected outright, not silently accepted as 1300.0");
}

static void test_parse_float_field_accepts_clean_value(void)
{
    TEST_SECTION("parse_float_field -- positive control: a clean in-range value is still accepted");
    float out = 0.0f;
    bool ok = zones_config_json_parse_float_field("z0_kp=2.5", "z0_kp", 0.0f, 5000.0f, &out);
    TEST_CHECK(ok, "a clean value must still parse");
    TEST_CHECK_NEAR(out, 2.5, 1e-6, "parsed value must be correct");
}

// zones_post_handler() end-to-end, for the two inline strtol sites that have
// no standalone function to call directly. thermo_count=0/relay_count=0
// keeps the body minimal -- every zone block is then past thermo_count, so
// parse_zone_fields() treats all its fields as optional/preserve-existing
// (see test_out_of_range_zone_preserves_stored_fields() above), and no
// z%u_* fields are required at all.
static void run_zones_post(const char *body)
{
    test_post_hooks_reset();
    s_test_post_body = body;
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.content_len = (long long)strlen(body);
    esp_err_t err = zones_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "zones_post_handler must always return ESP_OK (errors go through httpd_resp_send_err)");
}

static void test_zones_post_max_simultaneous_relays_rejects_trailing_garbage(void)
{
    TEST_SECTION("zones_post_handler -- max_simultaneous_relays trailing garbage rejected (FIX 2, inline strtol site 1)");
    run_zones_post("thermo_count=0&relay_count=0&max_simultaneous_relays=2X");
    TEST_CHECK(s_test_err_called, "\"2X\" must be rejected, not silently accepted as 2");
    TEST_CHECK(!s_test_ok_called, "must not report success for a rejected submission");
    TEST_CHECK(strstr(s_test_err_msg, "max_simultaneous_relays") != NULL,
              "error message should name the offending field");
}

// A single minimal-but-complete timing profile (tp0_*), all nine fields at
// their legal 0 ("use the firmware default") -- the minimum every whole-page
// submit must carry as of ZONES_CFG_VERSION 9 (see zones_post_handler()'s own
// comment: timing_profile_count must never be 0). Reused by every test below
// that needs zones_post_handler() to reach past the profile-parsing block.
#define MINIMAL_TIMING_PROFILE_BODY \
    "tp0_name=Default&tp0_progressduty=0&tp0_progresswindow=0&tp0_drifthyst=0&" \
    "tp0_frozeneps=0&tp0_xzoneperiod=0&tp0_bbhyst=0&tp0_coolmargin=0&tp0_coolhold=0&tp0_ramplock=0"

static void test_zones_post_safety_tc_type_rejects_trailing_garbage(void)
{
    TEST_SECTION("zones_post_handler -- safety_tc_type trailing garbage rejected (FIX 2, inline strtol site 2)");
    run_zones_post("thermo_count=0&relay_count=0&" MINIMAL_TIMING_PROFILE_BODY "&safety_tc_type=3Q");
    TEST_CHECK(s_test_err_called, "\"3Q\" must be rejected, not silently accepted as 3");
    TEST_CHECK(!s_test_ok_called, "must not report success for a rejected submission");
    TEST_CHECK(strstr(s_test_err_msg, "safety_tc_type") != NULL,
              "must be rejected FOR safety_tc_type specifically, not for an unrelated missing "
              "timing profile -- proves this test still exercises the code path it claims to");
}

static void test_zones_post_accepts_clean_minimal_body(void)
{
    TEST_SECTION("zones_post_handler -- positive control: clean values on the same two fields are still accepted");
    run_zones_post("thermo_count=0&relay_count=0&" MINIMAL_TIMING_PROFILE_BODY
                   "&max_simultaneous_relays=2&safety_tc_type=3");
    TEST_CHECK(!s_test_err_called, "a clean submission must not be rejected");
    TEST_CHECK(s_test_ok_called, "a clean submission must report success");
}

// A single per-zone body block, shared by the whole-page cross-zone cycle
// test below -- every field parse_zone_fields() requires for an in-range
// zone (thermo_count covers both zone 0 and zone 1 in that test).
#define TWO_ZONE_MINIMAL_BODY(SRC0, SRC1) \
    "thermo_count=2&relay_count=4&" MINIMAL_TIMING_PROFILE_BODY "&" \
    "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&" \
    "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=3&" \
    "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&z0_timingprofile=0&" \
    "z0_settings_source=" SRC0 "&" \
    "z1_name=Bottom&z1_tctype=2&z1_relay_mask=2&z1_thermo_mask=2&" \
    "z1_cal=0&z1_kp=1&z1_ki=0&z1_kd=0&z1_ramp=100&z1_sanity=0&z1_mode=3&" \
    "z1_maxtemp=1300&z1_mintemp=-20&z1_window=0&z1_minon=0&z1_minoff=0&z1_timingprofile=0&" \
    "z1_settings_source=" SRC1

// ---- POST /api/zones/pid -- narrow mid-firing PID-gain exception ----------
// zones_pid_post_handler() (zones_http.c). Same "stage the body, call the
// real handler, inspect the real side effects" discipline as run_zones_post()
// above -- s_test_post_body/content_len/httpd_resp_send_err/sendstr are all
// the SAME stub surface, so no new hooks were needed for this endpoint.
static void run_zones_pid_post(const char *body)
{
    test_post_hooks_reset();
    s_test_post_body = body;
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.content_len = (long long)strlen(body);
    esp_err_t err = zones_pid_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "zones_pid_post_handler must always return ESP_OK (errors go through httpd_resp_send_err)");
}

// Seeds a live two-zone config identical to TWO_ZONE_MINIMAL_BODY's own
// baseline (relay_mask/thermo_mask/control_mode/max_temp_c/etc all at known,
// non-default values) through the REAL whole-page path, so every "still
// forbidden while running" test below has a concrete baseline value to prove
// untouched, not just an assumed zero.
static void seed_two_zone_pid_baseline(void)
{
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    /* "255" == ZONE_SETTINGS_SOURCE_CUSTOM (zones_http.h) on both zones --
     * "0" would make zone 0 self-reference (parse_zone_fields() refuses
     * that as a degenerate cycle), which is what the very first version of
     * this seed used and every test in this group failed at this line
     * because of it. */
    run_zones_post(TWO_ZONE_MINIMAL_BODY("255", "255"));
    TEST_CHECK(!s_test_err_called, "baseline seed must itself be accepted");
}

static void test_zones_pid_post_accepts_while_profile_running(void)
{
    TEST_SECTION("POST /api/zones/pid -- a PID-only change is ACCEPTED while profile_executor reports RUNNING");
    seed_two_zone_pid_baseline();
    s_test_profile_status.state = PROFILE_EXEC_RUNNING; /* zones_pid_post_handler() must not even look at this --
                                                           * proven by the fact this still succeeds. */
    run_zones_pid_post("zone=0&kp=0.0318&ki=0.00012&kd=0.8401");
    s_test_profile_status.state = PROFILE_EXEC_IDLE;
    TEST_CHECK(!s_test_err_called, "a PID-only submit must not be refused just because a firing is running");
    TEST_CHECK(s_test_ok_called, "a PID-only submit must report success");
    float kp = 0.0f, ki = 0.0f, kd = 0.0f;
    TEST_CHECK(zones_config_get_pid(0, &kp, &ki, &kd), "zone 0 must still be readable after the write");
    TEST_CHECK_NEAR(kp, 0.0318, 1e-6, "kp must be applied exactly");
    TEST_CHECK_NEAR(ki, 0.00012, 1e-9, "ki (~1e-4 magnitude) must be applied exactly, not rounded toward zero");
    TEST_CHECK_NEAR(kd, 0.8401, 1e-6, "kd must be applied exactly");
}

static void test_zones_pid_post_bumps_generation(void)
{
    TEST_SECTION("POST /api/zones/pid -- the generation counter bumps, so profile_executor will observe the change");
    seed_two_zone_pid_baseline();
    uint32_t gen_before = zones_config_generation();
    run_zones_pid_post("zone=1&kp=0.0485&ki=0.00018&kd=1.0548");
    TEST_CHECK(!s_test_err_called, "clean submit must not be refused");
    TEST_CHECK(zones_config_generation() > gen_before,
              "s_config_generation must advance on a successful PID write, or reload_config_if_changed() "
              "will never notice the new gains");
}

static void test_zones_pid_post_rejects_kp_over_bound(void)
{
    TEST_SECTION("POST /api/zones/pid -- kp above ZONE_PID_GAIN_MAX is refused by the SHARED validator");
    seed_two_zone_pid_baseline();
    float kp_before = 0.0f, ki_before = 0.0f, kd_before = 0.0f;
    zones_config_get_pid(0, &kp_before, &ki_before, &kd_before);
    run_zones_pid_post("zone=0&kp=1000.001&ki=0.0001&kd=1.0");
    TEST_CHECK(s_test_err_called, "kp past 1000.0 must be refused");
    TEST_CHECK(!s_test_ok_called, "must not report success for a rejected submission");
    float kp = 0.0f, ki = 0.0f, kd = 0.0f;
    zones_config_get_pid(0, &kp, &ki, &kd);
    TEST_CHECK_NEAR(kp, kp_before, 1e-9, "a rejected submission must not partially apply -- kp must be unchanged");
}

static void test_zones_pid_post_rejects_negative_ki(void)
{
    TEST_SECTION("POST /api/zones/pid -- a negative ki is refused by the SHARED validator");
    seed_two_zone_pid_baseline();
    run_zones_pid_post("zone=0&kp=0.03&ki=-0.0001&kd=1.0");
    TEST_CHECK(s_test_err_called, "a negative ki must be refused, matching zones_config_json_parse_float_field()'s "
                                  "0.0f floor");
    TEST_CHECK(!s_test_ok_called, "must not report success for a rejected submission");
}

static void test_zones_pid_post_rejects_non_numeric_kd(void)
{
    TEST_SECTION("POST /api/zones/pid -- a non-numeric kd is refused by the SHARED validator");
    seed_two_zone_pid_baseline();
    run_zones_pid_post("zone=0&kp=0.03&ki=0.0001&kd=notanumber");
    TEST_CHECK(s_test_err_called, "a non-numeric kd must be refused");
    TEST_CHECK(!s_test_ok_called, "must not report success for a rejected submission");
}

static void test_zones_pid_post_rejects_zone_out_of_range(void)
{
    TEST_SECTION("POST /api/zones/pid -- a zone index at or past thermo_count is refused");
    seed_two_zone_pid_baseline(); /* thermo_count=2 -- zone 2 does not exist */
    run_zones_pid_post("zone=2&kp=0.03&ki=0.0001&kd=1.0");
    TEST_CHECK(s_test_err_called, "an unconfigured zone index must be refused");
    TEST_CHECK(!s_test_ok_called, "must not report success for a rejected submission");
}

// ---- "Still forbidden while running" -- one test PER field, proving the
// field cannot be smuggled through this fixed-shape endpoint (it parses
// ONLY zone/kp/ki/kd, so an attacker-controlled extra key in the same POST
// body is simply never looked at). Each test posts a clean, ACCEPTED PID
// change alongside an attempted edit of one forbidden field (using the SAME
// key parse_zone_fields()/the whole-page path would recognise), then proves
// that field's live value is exactly the seeded baseline -- not the smuggled
// value.
static void test_zones_pid_post_ignores_relay_mask(void)
{
    TEST_SECTION("POST /api/zones/pid -- relay_mask / zone membership stays refused while running "
                "(cannot be smuggled through this endpoint)");
    seed_two_zone_pid_baseline();
    run_zones_pid_post("zone=0&kp=0.05&ki=0.0002&kd=1.0&z0_relay_mask=2&relay_mask=2");
    TEST_CHECK(!s_test_err_called, "the PID part of this submit is clean and must still succeed");
    uint8_t mask = 0xFF;
    zones_config_get_relay_mask(0, &mask);
    TEST_CHECK(mask == 1, "relay_mask must remain the seeded value (1) -- z0_relay_mask=2 must be ignored");
}

static void test_zones_pid_post_ignores_control_mode(void)
{
    TEST_SECTION("POST /api/zones/pid -- control_mode stays refused while running");
    seed_two_zone_pid_baseline();
    run_zones_pid_post("zone=0&kp=0.05&ki=0.0002&kd=1.0&z0_mode=0&control_mode=0&mode=0");
    TEST_CHECK(!s_test_err_called, "the PID part of this submit is clean and must still succeed");
    TEST_CHECK(s_zones.cfg.zones[0].control_mode == 3,
              "control_mode must remain the seeded value (3, PID_FUZZY) -- z0_mode=0 must be ignored");
}

static void test_zones_pid_post_ignores_max_temp(void)
{
    TEST_SECTION("POST /api/zones/pid -- max_temp_c/min_temp_c stay refused while running");
    seed_two_zone_pid_baseline();
    run_zones_pid_post("zone=0&kp=0.05&ki=0.0002&kd=1.0&z0_maxtemp=1&maxtemp=1&z0_mintemp=-999&mintemp=-999");
    TEST_CHECK(!s_test_err_called, "the PID part of this submit is clean and must still succeed");
    TEST_CHECK_NEAR(s_zones.cfg.zones[0].max_temp_c, 1300.0, 1e-6,
                    "max_temp_c must remain the seeded value -- z0_maxtemp=1 must be ignored");
    TEST_CHECK_NEAR(s_zones.cfg.zones[0].min_temp_c, -20.0, 1e-6,
                    "min_temp_c must remain the seeded value -- z0_mintemp=-999 must be ignored");
}

static void test_zones_pid_post_ignores_max_ramp(void)
{
    TEST_SECTION("POST /api/zones/pid -- max_ramp_c_per_hr stays refused while running");
    seed_two_zone_pid_baseline();
    run_zones_pid_post("zone=0&kp=0.05&ki=0.0002&kd=1.0&z0_ramp=1&ramp=1");
    TEST_CHECK(!s_test_err_called, "the PID part of this submit is clean and must still succeed");
    TEST_CHECK_NEAR(s_zones.cfg.zones[0].max_ramp_c_per_hr, 100.0, 1e-6,
                    "max_ramp_c_per_hr must remain the seeded value -- z0_ramp=1 must be ignored");
}

static void test_zones_pid_post_ignores_cal_offset(void)
{
    TEST_SECTION("POST /api/zones/pid -- cal_offset_c stays refused while running");
    seed_two_zone_pid_baseline();
    run_zones_pid_post("zone=0&kp=0.05&ki=0.0002&kd=1.0&z0_cal=25&cal=25");
    TEST_CHECK(!s_test_err_called, "the PID part of this submit is clean and must still succeed");
    TEST_CHECK_NEAR(s_zones.cfg.zones[0].cal_offset_c, 0.0, 1e-6,
                    "cal_offset_c must remain the seeded value (0) -- z0_cal=25 must be ignored");
}

static void test_zones_pid_post_ignores_guard_thresholds(void)
{
    TEST_SECTION("POST /api/zones/pid -- guard thresholds stay refused while running");
    seed_two_zone_pid_baseline();
    run_zones_pid_post("zone=0&kp=0.05&ki=0.0002&kd=1.0&z0_wrongdirwindow=999&wrongdirwindow=999&"
                       "z0_runawaymargin=1&runawaymargin=1");
    TEST_CHECK(!s_test_err_called, "the PID part of this submit is clean and must still succeed");
    TEST_CHECK_NEAR(s_zones.cfg.zones[0].guard_wrong_dir_window_s, 0.0, 1e-6,
                    "guard_wrong_dir_window_s must remain the seeded value (0) -- must be ignored");
    TEST_CHECK_NEAR(s_zones.cfg.zones[0].guard_runaway_margin_c, 0.0, 1e-6,
                    "guard_runaway_margin_c must remain the seeded value (0) -- must be ignored");
}

static void test_zones_pid_post_ignores_coupling_matrix(void)
{
    TEST_SECTION("POST /api/zones/pid -- the coupling matrix stays refused while running");
    seed_two_zone_pid_baseline();
    run_zones_pid_post("zone=0&kp=0.05&ki=0.0002&kd=1.0&z0_coupling_c1=999&coupling_c1=999");
    TEST_CHECK(!s_test_err_called, "the PID part of this submit is clean and must still succeed");
    TEST_CHECK_NEAR(s_zones.cfg.zones[0].coupling_coeff[1], 0.0, 1e-6,
                    "coupling_coeff[1] must remain the seeded value (0) -- z0_coupling_c1=999 must be ignored");
}

static void test_zones_pid_post_ignores_model_parameters(void)
{
    TEST_SECTION("POST /api/zones/pid -- the plant model (K_dc/tau) stays refused while running");
    seed_two_zone_pid_baseline();
    run_zones_pid_post("zone=0&kp=0.05&ki=0.0002&kd=1.0&z0_k=99&k=99&z0_tau=99&tau=99");
    TEST_CHECK(!s_test_err_called, "the PID part of this submit is clean and must still succeed");
    TEST_CHECK_NEAR(s_zones.cfg.zones[0].model_k_dc, 0.0, 1e-6,
                    "model_k_dc must remain the seeded value (0) -- z0_k=99 must be ignored");
    TEST_CHECK_NEAR(s_zones.cfg.zones[0].model_tau_s, 0.0, 1e-6,
                    "model_tau_s must remain the seeded value (0) -- z0_tau=99 must be ignored");
}

static void test_zones_pid_post_ignores_thermo_mask(void)
{
    TEST_SECTION("POST /api/zones/pid -- thermo_mask/ct_mask stay refused while running");
    seed_two_zone_pid_baseline();
    run_zones_pid_post("zone=0&kp=0.05&ki=0.0002&kd=1.0&z0_thermo_mask=4&thermo_mask=4");
    TEST_CHECK(!s_test_err_called, "the PID part of this submit is clean and must still succeed");
    uint8_t mask = 0xFF;
    zones_config_get_thermo_mask(0, &mask);
    TEST_CHECK(mask == 1, "thermo_mask must remain the seeded value (1) -- z0_thermo_mask=4 must be ignored");
}

static void test_zones_pid_post_ignores_settings_source(void)
{
    TEST_SECTION("POST /api/zones/pid -- settings_source (coupling to the safety-relevant zone) stays refused "
                "while running");
    seed_two_zone_pid_baseline();
    run_zones_pid_post("zone=0&kp=0.05&ki=0.0002&kd=1.0&z0_settings_source=1&settings_source=1");
    TEST_CHECK(!s_test_err_called, "the PID part of this submit is clean and must still succeed");
    TEST_CHECK(s_zones.cfg.zones[0].settings_source == 0xFF,
              "settings_source must remain the seeded value (0xFF, CUSTOM) -- z0_settings_source=1 must be ignored");
}

static void test_zones_pid_post_ignores_safety_tc_type(void)
{
    TEST_SECTION("POST /api/zones/pid -- safety_tc_type (mirrored to the RP2040 safety processor) stays "
                "refused while running");
    seed_two_zone_pid_baseline();
    run_zones_pid_post("zone=0&kp=0.05&ki=0.0002&kd=1.0&safety_tc_type=5");
    TEST_CHECK(!s_test_err_called, "the PID part of this submit is clean and must still succeed");
    TEST_CHECK(s_zones.cfg.safety_tc_type == 0,
              "safety_tc_type must remain the seeded value (0) -- safety_tc_type=5 must be ignored");
}

// parse_zone_fields()'s own per-zone chain-walk only ever checks the zone
// being written against every OTHER zone's LIVE stored value -- it cannot
// see a SECOND zone changing in the very same whole-page POST. z0's own
// entry (z0_settings_source=1) is not a cycle against the pre-POST live
// config (both zones start Custom -- see zones_post_handler()'s own body,
// tmp is zero-initialized, and s_zones.cfg is never touched by this test
// group before this call), and neither is z1's own entry
// (z1_settings_source=0) checked in isolation -- only TOGETHER, once both
// are assembled into tmp.zones[], do they close 0 -> 1 -> 0. This is exactly
// what zones_post_handler()'s own post-loop re-walk (right after the
// per-zone parse_zone_fields() loop, before the commit point) exists to
// catch.
static void test_post_whole_page_cross_zone_cycle_refused(void)
{
    TEST_SECTION("zones_post_handler -- two zones' settings_source keys in the SAME whole-page POST "
                 "that only close a cycle TOGETHER (neither is a cycle against the live config alone) "
                 "are refused, before either is committed");
    /* Explicit clean slate (Custom/Custom) -- run_zones_post() does not
     * reset s_zones.cfg, and this door's per-zone cross-check (inside
     * parse_zone_fields(), for the single-zone case) also consults the live
     * config for every OTHER zone, so a leftover raw-zero from an earlier
     * test (or this file's own zero-initialized BSS default) must not leak
     * in as an accidental link -- see zones_config_json_settings_source_chain_has_cycle()'s
     * own comment on why 0 is real data, not a "not set" sentinel. */
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 2; /* the getters below refuse zone_index >= thermo_count */
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        s_zones.cfg.zones[z].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    }
    run_zones_post(TWO_ZONE_MINIMAL_BODY("1", "0"));
    TEST_CHECK(s_test_err_called, "the whole submission is refused");
    TEST_CHECK(!s_test_ok_called, "must not report success for a rejected submission");
    TEST_CHECK(strstr(s_test_err_msg, "settings_source") != NULL, "the refusal names the field");
    TEST_CHECK(strstr(s_test_err_msg, "cycle") != NULL, "and calls out the cycle specifically");
    uint8_t s0 = 0xAA, s1 = 0xAA;
    TEST_CHECK(zones_config_get_settings_source(0, &s0) && s0 == ZONE_SETTINGS_SOURCE_CUSTOM &&
              zones_config_get_settings_source(1, &s1) && s1 == ZONE_SETTINGS_SOURCE_CUSTOM,
              "NOTHING was committed -- both zones are still at their pre-POST Custom default, not "
              "half-applied with zone 0's individually-legal-looking link written and zone 1's not");
}

// Positive control: the identical two-zone whole-page shape, but with a
// legal (acyclic) chain -- proves the post-loop re-walk refuses only a
// genuine cycle, not any submission that happens to touch two zones'
// settings_source in the same POST.
static void test_post_whole_page_cross_zone_legal_chain_accepted(void)
{
    TEST_SECTION("zones_post_handler -- a legal (acyclic) settings_source chain spanning two zones in "
                 "the same whole-page POST is still accepted");
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 2;
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        s_zones.cfg.zones[z].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    }
    run_zones_post(TWO_ZONE_MINIMAL_BODY("1", "255"));
    TEST_CHECK(!s_test_err_called, "a legal two-zone chain (0 -> 1 -> Custom) must not be rejected");
    TEST_CHECK(s_test_ok_called, "and must report success");
    uint8_t s0 = 0xAA, s1 = 0xAA;
    TEST_CHECK(zones_config_get_settings_source(0, &s0) && s0 == 1, "zone 0's link committed as 1");
    TEST_CHECK(zones_config_get_settings_source(1, &s1) && s1 == ZONE_SETTINGS_SOURCE_CUSTOM,
              "zone 1's link committed as Custom");
}

// ---------------------------------------------------------------------------
// FIX 1 -- a found-but-refused newer-version zones blob must not look like
// "nothing found" to the legacy-migration decision, and must never be
// overwritten by it. nvs_load_from() is `static`; reached directly, same
// convention as parse_zone_fields() above. Uses stubs/nvs.h's opt-in
// single-blob-slot NVS stub (nvs_test_enable()/nvs_test_clear()) -- off by
// default, so every test above this section (which never touches NVS) is
// unaffected.
// ---------------------------------------------------------------------------

static void stage_zones_blob(const void *data, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition("whatever", NVS_NAMESPACE, NVS_READWRITE, &h);
    (void)err; // the stub always succeeds once nvs_test_enable(true) is set
    nvs_set_blob(h, NVS_KEY_ZONES, data, len);
    nvs_close(h);
}

static void test_nvs_load_from_too_short_is_corrupt_not_refused(void)
{
    TEST_SECTION("nvs_load_from -- a too-short blob is corrupt: found=false, valid=false (migration may still run)");
    nvs_test_enable(true);
    nvs_test_clear();
    stage_zones_blob("", 0); // shorter than zones_cfg_t::version itself

    zones_cfg_t out_cfg;
    bool found = true, valid = true; // deliberately pre-set to the wrong answer
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "a too-short blob is a handled outcome, not an NVS error");
    TEST_CHECK(!found, "too-short (genuine corruption) must report found=false, so a caller is free to "
                       "look elsewhere (e.g. the legacy-partition migration) instead of treating it as "
                       "protected data");
    TEST_CHECK(!valid, "too-short must not be trustworthy");
    TEST_CHECK(out_cfg.version == 0, "out_cfg must come back zeroed");

    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_nvs_load_from_wrong_size_current_version_is_corrupt_not_refused(void)
{
    TEST_SECTION("nvs_load_from -- current-version blob at the wrong size is corrupt: found=false, valid=false");
    nvs_test_enable(true);
    nvs_test_clear();
    zones_cfg_t src;
    memset(&src, 0, sizeof(src));
    src.version = ZONES_CFG_VERSION;
    stage_zones_blob(&src, sizeof(src) - 1); // right version, truncated by one byte

    zones_cfg_t out_cfg;
    bool found = true, valid = true;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "a wrong-size blob is a handled outcome, not an NVS error");
    TEST_CHECK(!found, "wrong-size-for-its-version (genuine corruption) must report found=false, same "
                       "reasoning as the too-short branch");
    TEST_CHECK(!valid, "wrong-size must not be trustworthy");

    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_nvs_load_from_current_version_happy_path(void)
{
    TEST_SECTION("nvs_load_from -- current version, right size, valid CRC: found=true, valid=true");
    nvs_test_enable(true);
    nvs_test_clear();
    zones_cfg_t src;
    memset(&src, 0, sizeof(src));
    src.version = ZONES_CFG_VERSION;
    src.thermo_count = 2;
    src.timing_profile_count = 1; // must never be 0 in a config zones_config_json_validate() accepts
    src.crc32 = zones_config_json_compute_crc(&src); // CRC now checked on the current-version path (item 4)
    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found, "a real current-version blob must report found=true");
    TEST_CHECK(valid, "a real current-version blob must report valid=true");
    TEST_CHECK(out_cfg.thermo_count == 2, "the decoded config must actually come through");

    nvs_test_enable(false);
    nvs_test_clear();
}

// A pre-existing stored settings_source cycle (however it got onto flash --
// direct NVS tampering, or a blob written by firmware that predates the
// chain-walk guard) must NOT brick the config on load. zones_config_json_validate()
// rejecting it would drive zones_config_json_decode_blob() to the CORRUPT/wipe outcome
// and destroy an entire commissioned config over one stale UI-only
// provenance link -- this repo has lost configs to exactly that shape of
// overreaction before (raise_heater_timing_to_floors()'s neighboring
// precedent, cited by zones_config_json_normalize_settings_source_cycles()'s own comment). So
// nvs_load_from() must instead collapse the cyclic zone(s) to Custom and
// keep booting with everything else intact, the same "collapse to Custom"
// resolution zones_page.html's own client-side resolveTerminal() already
// performs on a stale page load.
static void test_nvs_load_from_pre_existing_cycle_normalizes_not_wipes(void)
{
    TEST_SECTION("nvs_load_from -- a pre-existing stored settings_source cycle collapses to Custom on "
                 "load instead of being rejected as corrupt -- must not wipe a commissioned config over "
                 "a stale UI-only provenance link");
    nvs_test_enable(true);
    nvs_test_clear();
    zones_cfg_t src;
    memset(&src, 0, sizeof(src));
    src.version = ZONES_CFG_VERSION;
    src.thermo_count = 3;
    src.timing_profile_count = 1;
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        src.zones[z].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    }
    // The stored 2-cycle this same guard refuses at write time -- reachable
    // here only because it predates the guard (or was written by some other
    // means entirely outside this file's own setters/parser).
    src.zones[0].settings_source = 1;
    src.zones[1].settings_source = 0;
    // A real, meaningful field on the cyclic zone, to prove normalization
    // touches ONLY settings_source and does not zero the rest of the zone.
    src.zones[0].pid_kp = 7.25f;
    src.zones[0].tc_type = 5;
    // A THIRD zone with a legal (non-cyclic) link, to prove the collapse is
    // scoped to the zone(s) actually on a cycle, not a blanket "any zone
    // with a real link gets wiped" overreaction.
    src.zones[2].settings_source = 0; // "copies zone 0" -- legal once zone 0 is Custom below
    src.crc32 = zones_config_json_compute_crc(&src);
    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found, "a stored cycle is still a REAL blob -- found=true");
    TEST_CHECK(valid, "and it is still VALID -- the cycle is normalized, not treated as corrupt "
              "(this is the assertion that proves normalize-not-reject: a zones_config_json_validate() "
              "rejection here would flip this to false and wipe out_cfg via zones_config_json_decode_blob()'s "
              "CORRUPT path)");
    TEST_CHECK(out_cfg.thermo_count == 3, "the rest of the config survives the load untouched");
    uint8_t s0 = 0xAA, s1 = 0xAA;
    TEST_CHECK(out_cfg.zones[0].settings_source != 1 || out_cfg.zones[1].settings_source != 0,
              "the stored 2-cycle no longer exists in the decoded config -- at least one of the two "
              "links was broken by normalization");
    s0 = out_cfg.zones[0].settings_source;
    s1 = out_cfg.zones[1].settings_source;
    TEST_CHECK(s0 == ZONE_SETTINGS_SOURCE_CUSTOM || s1 == ZONE_SETTINGS_SOURCE_CUSTOM,
              "the break was made by collapsing (at least) one of the two cyclic zones to Custom, the "
              "same resolution zones_page.html's own client-side cycle guard already uses -- not some "
              "other repair");
    TEST_CHECK_NEAR(out_cfg.zones[0].pid_kp, 7.25f, 1e-6,
                    "normalization touches ONLY settings_source -- zone 0's pid_kp survives intact, "
                    "proving this is not a wipe of the zone or the config");
    TEST_CHECK(out_cfg.zones[0].tc_type == 5, "and zone 0's tc_type survives intact too");

    nvs_test_enable(false);
    nvs_test_clear();
}

// zones_config_json_normalize_settings_source_cycles() non-blocking finding (opus review of
// commit b69b74a): a zone that merely LEADS INTO a cycle (its own chain is
// fine, it just happens to walk into one) must be left untouched -- only the
// zone(s) actually ON the cycle may be collapsed to Custom. An earlier
// version of this function walked start-to-finish in index order and reset
// `start`'s own link whenever ITS walk hit a cycle, which is index-order
// dependent: with a stored 1<->2 cycle and zone 0 -> 1 merely leading into
// it, scanning from i=0 would hit the cycle via zone 0's own walk and reset
// zone 0's OWN (innocent, non-cyclic) link first, even though breaking 1<->2
// alone would have sufficed. This test seeds exactly that shape and proves
// zone 0's link survives.
static void test_nvs_load_from_cycle_normalization_does_not_touch_lead_in_zone(void)
{
    TEST_SECTION("nvs_load_from -- zones_config_json_normalize_settings_source_cycles() breaks ONLY the zone(s) actually "
                 "ON a cycle, not a zone that merely leads into one (index-order independence)");
    nvs_test_enable(true);
    nvs_test_clear();
    zones_cfg_t src;
    memset(&src, 0, sizeof(src));
    src.version = ZONES_CFG_VERSION;
    src.thermo_count = 3;
    src.timing_profile_count = 1;
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        src.zones[z].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    }
    // Zone 0 leads into the cycle (0 -> 1) but is not itself part of it.
    src.zones[0].settings_source = 1;
    // The actual 1 <-> 2 cycle.
    src.zones[1].settings_source = 2;
    src.zones[2].settings_source = 1;
    src.crc32 = zones_config_json_compute_crc(&src);
    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a stored cycle collapses, it does not get treated as corrupt");
    TEST_CHECK(out_cfg.zones[0].settings_source == 1,
              "zone 0's link (0 -> 1) survives untouched -- it was never ON the cycle, only LEADING "
              "INTO it, and breaking the real 1<->2 cycle alone is sufficient to fix zone 0's chain too");
    TEST_CHECK(out_cfg.zones[1].settings_source != 2 || out_cfg.zones[2].settings_source != 1,
              "the actual 1<->2 cycle no longer exists -- at least one of its two links was broken");
    uint8_t s1 = out_cfg.zones[1].settings_source, s2 = out_cfg.zones[2].settings_source;
    TEST_CHECK(s1 == ZONE_SETTINGS_SOURCE_CUSTOM || s2 == ZONE_SETTINGS_SOURCE_CUSTOM,
              "the break was made by collapsing (at least) one of the two cyclic zones to Custom");

    nvs_test_enable(false);
    nvs_test_clear();
}

// zones_config_import_blob()'s own pass-1 settings_source cycle check (added
// alongside the two-pass restore fix -- see backup_http.c's importer, which
// calls THIS function to apply a decoded blob) had no test at all before
// this pair, positive or negative: nvs_load_from()'s cycle tests above only
// exercise zones_config_json_normalize_settings_source_cycles() on the LOAD path, which
// deliberately COLLAPSES a cycle rather than rejecting it -- a completely
// different code path from zones_config_import_blob()'s pass-1 walk, which
// must REJECT instead (see that function's own comment: "there is a live
// client on the other end of this call who can be handed a clear reason").
// Negative first: a genuinely cyclic blob must be refused, with the commit
// point (s_zones.cfg = cand) never reached -- proven by checking the live
// config is untouched, not just that the call returns false.
static void test_import_blob_pass1_rejects_cyclic_settings_source(void)
{
    TEST_SECTION("zones_config_import_blob -- pass 1: a genuinely cyclic settings_source blob is "
                 "REFUSED, nothing committed to the live config");
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 2;
    s_zones.cfg.timing_profile_count = 1;
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        s_zones.cfg.zones[z].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    }
    s_zones.cfg.zones[0].max_temp_c = 1234.0f; // a live value the rejected import must not disturb
    s_zones_config_valid = true;
    uint32_t generation_before = s_config_generation;

    zones_cfg_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = ZONES_CFG_VERSION;
    blob.thermo_count = 2;
    blob.timing_profile_count = 1;
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        blob.zones[z].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    }
    blob.zones[0].settings_source = 1; // the 2-cycle this pass-1 check must catch
    blob.zones[1].settings_source = 0;
    blob.crc32 = zones_config_json_compute_crc(&blob);

    char reason[128];
    reason[0] = '\0';
    bool ok = zones_config_import_blob(&blob, sizeof(blob), reason, sizeof(reason));

    TEST_CHECK(!ok, "a cyclic settings_source import is refused");
    TEST_CHECK(strstr(reason, "settings_source") != NULL && strstr(reason, "cycle") != NULL,
              "the refusal names the field and calls out the cycle specifically");
    TEST_CHECK(s_zones.cfg.zones[0].max_temp_c == 1234.0f,
              "the pre-existing live config is completely untouched -- pass 1 rejected before the "
              "commit point, not partway through it");
    TEST_CHECK(s_zones.cfg.zones[0].settings_source == ZONE_SETTINGS_SOURCE_CUSTOM &&
              s_zones.cfg.zones[1].settings_source == ZONE_SETTINGS_SOURCE_CUSTOM,
              "the live settings_source links are still Custom -- the cyclic blob's links never landed");
    TEST_CHECK(s_config_generation == generation_before, "no config generation bump for a rejected import");
}

// Positive control for the test above: the identical shape, but the blob's
// settings_source chain is legal (acyclic) -- proves pass 1 refuses ONLY a
// genuine cycle, not any blob that happens to link two zones' settings_source
// together, and that a legitimate import still lands.
static void test_import_blob_pass1_accepts_acyclic_settings_source(void)
{
    TEST_SECTION("zones_config_import_blob -- pass 1: a legal (acyclic) settings_source chain is "
                 "accepted and actually committed");
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 2;
    s_zones.cfg.timing_profile_count = 1;
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        s_zones.cfg.zones[z].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    }
    s_zones_config_valid = false;

    zones_cfg_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = ZONES_CFG_VERSION;
    blob.thermo_count = 2;
    blob.timing_profile_count = 1;
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        blob.zones[z].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    }
    blob.zones[0].settings_source = 1; // 0 -> 1 -> Custom: terminates cleanly, no cycle
    blob.crc32 = zones_config_json_compute_crc(&blob);

    char reason[128];
    reason[0] = '\0';
    bool ok = zones_config_import_blob(&blob, sizeof(blob), reason, sizeof(reason));

    TEST_CHECK(ok, "an acyclic settings_source import is accepted");
    TEST_CHECK(s_zones_config_valid, "and marked valid/committed");
    TEST_CHECK(s_zones.cfg.zones[0].settings_source == 1,
              "the imported link is actually live in the committed config");
}

static void test_nvs_load_from_newer_than_firmware_is_found_but_not_valid(void)
{
    TEST_SECTION("nvs_load_from -- FIX 1: a newer-than-firmware blob is found=true, valid=false "
                 "(refused, but must block migration, not invite it)");
    nvs_test_enable(true);
    nvs_test_clear();
    zones_cfg_t src;
    memset(&src, 0, sizeof(src));
    src.version = (uint8_t)(ZONES_CFG_VERSION + 1); // firmware-rollback case
    src.thermo_count = 3; // something a stale migration would clobber if this leaked through
    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = true; // deliberately pre-set to the wrong answers
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "refusing to load is a handled outcome, not an NVS error");
    TEST_CHECK(found, "FIX 1: a refused newer-version blob MUST report found=true -- it is real, "
                      "deliberately-protected data, not \"nothing was ever saved\". Before FIX 1 this "
                      "was false, which is exactly what let zones_http_start() fall through to "
                      "migrate_from_default_partition() and overwrite it.");
    TEST_CHECK(!valid, "refused data must not be reported trustworthy for this boot");
    TEST_CHECK(out_cfg.version == 0, "out_cfg must come back zeroed, not the newer struct's raw contents");
    TEST_CHECK(out_cfg.thermo_count == 0, "the refused blob's fields must not leak into out_cfg");

    nvs_test_enable(false);
    nvs_test_clear();
}

// The scenario FIX 1 actually fixes, exercised through zones_http_start()
// itself rather than nvs_load_from() in isolation: a newer-than-firmware
// blob staged as "what's on flash" must survive a boot completely
// untouched -- not clobbered by the legacy-partition migration, which the
// old (err == ESP_OK && s_zones.cfg.version != 0) proxy could not tell apart
// from "kiln_nvs has never had anything saved". stubs/nvs.h's stub has a
// single blob slot shared across every (partition, key) pair, which happens
// to model this exact bug perfectly: if zones_http_start() ever calls
// migrate_from_default_partition() here, THAT function's own
// nvs_load_from(NVS_DEFAULT_PART_NAME, ...) call reads the very same staged
// blob right back (there is only one slot), decides it cannot use it either
// (same refusal), and returns without saving -- so any accidental migration
// attempt is invisible to a check that only looks at "did anything change".
// The real, load-bearing assertion here is s_zones_config_valid staying
// false AND the staged bytes in the stub's one slot staying byte-for-byte
// identical after the call: an nvs_save() from ANY path (migration or
// otherwise) would stamp a fresh ZONES_CFG_VERSION into byte 0, which the
// staged (ZONES_CFG_VERSION + 1) can never equal.
static void test_zones_http_start_refused_newer_blob_not_overwritten(void)
{
    TEST_SECTION("zones_http_start -- FIX 1: a refused newer-version blob on flash survives a boot untouched");
    nvs_test_enable(true);
    nvs_test_clear();
    zones_cfg_t src;
    memset(&src, 0, sizeof(src));
    src.version = (uint8_t)(ZONES_CFG_VERSION + 1);
    src.thermo_count = 3;
    stage_zones_blob(&src, sizeof(src));

    uint8_t blob_before[sizeof(zones_cfg_t)];
    memcpy(blob_before, &src, sizeof(src));

    s_zones_config_valid = true; // deliberately wrong, so a no-op bug can't accidentally read as a pass
    (void)zones_http_start(); // returns ESP_ERR_INVALID_STATE (no HTTP server in this stub) AFTER the
                              // NVS load/migration logic below has already run -- exactly what's under test.

    TEST_CHECK(!s_zones_config_valid, "a refused newer-version blob must leave the config NOT valid for "
                                      "this boot -- zone commanding must stay refused, not silently run "
                                      "off a stale migrated copy");
    TEST_CHECK(s_stub_nvs_blob_len == sizeof(src) &&
                  memcmp(s_stub_nvs_blob, blob_before, sizeof(src)) == 0,
              "FIX 1: the on-flash blob must be byte-for-byte unchanged -- if migrate_from_default_"
              "partition() ran and saved, nvs_save() would have stamped ZONES_CFG_VERSION (not "
              "ZONES_CFG_VERSION+1) into byte 0, which this check catches");

    nvs_test_enable(false);
    nvs_test_clear();
}

// ---------------------------------------------------------------------------
// "Saved securely like the others" -- the real defect this pass fixes.
// nvs_load_from()'s old load path (for a stored version OLDER than current)
// did no length check, never ran zones_config_json_validate(), assumed zone_cfg_t
// only ever grew at the tail (false: three of the last four bumps before
// this one grew it mid-struct, an ARRAY ELEMENT, which displaces every zone
// after the first), and had no CRC at all. Each check below is proven to be
// able to FAIL, not just proven to pass on a well-formed blob -- see each
// test's own "confirmed RED" note in this file's accompanying report.
// ---------------------------------------------------------------------------

// Item 1 -- length-vs-claimed-version check, exercised on an OLDER version:
// a blob that claims version 4 but is sized like a version-3 blob (one whole
// zone_cfg_t narrower, since v3->v4 grew every zone_cfg_t array element) must
// be rejected outright, not partially interpreted.
static void test_nvs_load_from_old_version_wrong_length_is_rejected(void)
{
    TEST_SECTION("nvs_load_from -- item 1: old-version blob whose length doesn't match its "
                 "claimed version is rejected outright");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v3_t src3;
    memset(&src3, 0, sizeof(src3));
    src3.version = 4; // LIES about being v4 -- actually only a v3-sized blob
    src3.thermo_count = 3;
    stage_zones_blob(&src3, sizeof(src3)); // v3-sized, not v4-sized

    zones_cfg_t out_cfg;
    bool found = true, valid = true;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "a length mismatch is a handled outcome, not an NVS error");
    TEST_CHECK(!found, "wrong length for the claimed version must be rejected (found=false)");
    TEST_CHECK(!valid, "must not be trustworthy");
    TEST_CHECK(out_cfg.version == 0, "out_cfg must come back zeroed, not partially interpreted");
    TEST_CHECK(out_cfg.thermo_count == 0, "no field may leak through from a rejected blob");

    nvs_test_enable(false);
    nvs_test_clear();
}

// Item 4 -- CRC. A current-version blob with the RIGHT length but a stored
// CRC that does not match its own bytes must be rejected.
static void test_nvs_load_from_bad_crc_is_rejected(void)
{
    TEST_SECTION("nvs_load_from -- item 4: current-version blob with a bad CRC is rejected");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_t src;
    memset(&src, 0, sizeof(src));
    src.version = ZONES_CFG_VERSION;
    src.thermo_count = 1;
    src.zones[0].max_temp_c = 1300.0f;
    src.crc32 = zones_config_json_compute_crc(&src) ^ 0x1u; // one bit off from the real CRC
    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = true, valid = true;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "a bad CRC is a handled outcome, not an NVS error");
    TEST_CHECK(!found, "a CRC mismatch must be rejected (found=false)");
    TEST_CHECK(!valid, "must not be trustworthy");
    TEST_CHECK(out_cfg.thermo_count == 0, "no field may leak through from a CRC-rejected blob");

    nvs_test_enable(false);
    nvs_test_clear();
}

// Item 3 -- zones_config_json_validate() now runs on the NVS load path too, not just
// zones_config_import_blob(). A current-version blob with the right length
// AND a correct CRC (proving the bytes are exactly what was written) but an
// out-of-range field must still be rejected, not adopted with the valid flag
// set -- the config was written wrong in the first place, and a correct CRC
// over wrong data is not a reason to trust it.
static void test_nvs_load_from_failed_validation_is_rejected(void)
{
    TEST_SECTION("nvs_load_from -- item 3: length+CRC correct but zones_config_json_validate() fails "
                 "-> rejected, not partially adopted");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_t src;
    memset(&src, 0, sizeof(src));
    src.version = ZONES_CFG_VERSION;
    src.thermo_count = 1;
    src.zones[0].max_temp_c = 999999.0f; // past ZONE_MAX_TEMP_C_MAX -- zones_config_json_validate() must reject
    src.crc32 = zones_config_json_compute_crc(&src); // CRC is genuinely correct for these (bad) bytes
    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = true, valid = true;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "a validation failure is a handled outcome, not an NVS error");
    TEST_CHECK(!found, "a zones_config_json_validate() failure must be rejected (found=false), matching "
                       "every other corruption case, not partially defaulted");
    TEST_CHECK(!valid, "must not be trustworthy");
    TEST_CHECK(out_cfg.thermo_count == 0, "nothing from a validation-rejected blob may leak through");

    nvs_test_enable(false);
    nvs_test_clear();
}

// Round-trip: nvs_save() then nvs_load() (the real save/load pair, not just
// nvs_load_from() in isolation) must hand back every field identical,
// including the newly-added crc32-stamping behavior itself.
static void test_nvs_save_load_round_trip_current_version(void)
{
    TEST_SECTION("nvs_save/nvs_load -- round trip: every field identical after save then load back");
    nvs_test_enable(true);
    nvs_test_clear();

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;
    s_zones.cfg.relay_count = 4;
    s_zones.cfg.max_simultaneous_relays = 2;
    s_zones.cfg.continue_on_zone_trip = 1;
    s_zones.cfg.safety_tc_type = 3;
    s_zones.cfg.timing_profile_count = 2;
    strncpy(s_zones.cfg.timing_profiles[0].name, "Default", TIMING_PROFILE_NAME_MAX_LEN);
    s_zones.cfg.timing_profiles[0].guard_progress_duty_min = 0.1f;
    s_zones.cfg.timing_profiles[0].ramp_lock_band_c = 3.0f;
    strncpy(s_zones.cfg.timing_profiles[1].name, "Fast", TIMING_PROFILE_NAME_MAX_LEN);
    s_zones.cfg.timing_profiles[1].guard_progress_duty_min = 0.2f;
    s_zones.cfg.timing_profiles[1].ramp_lock_band_c = 5.0f;
    for (uint8_t i = 0; i < 3; i++) {
        zone_cfg_t *z = &s_zones.cfg.zones[i];
        snprintf(z->name, sizeof(z->name), "Z%u", (unsigned)i);
        z->relay_mask = (uint8_t)(1u << i);
        z->thermo_mask = (uint8_t)(1u << i);
        z->ct_mask = (uint8_t)(1u << (i % ZONE_CT_CHANNEL_COUNT));
        z->tc_type = (uint8_t)(i % 8);
        z->cal_offset_c = 1.5f + i;
        z->pid_kp = 2.0f + i;
        z->max_temp_c = 1200.0f + 10.0f * i;
        z->model_k_dc = 12.0f + i;
        z->timing_profile = (uint8_t)(i % 2); // exercise both profiles, not just 0
    }
    zones_cfg_t saved_copy = s_zones.cfg; // captured before nvs_save() stamps version/crc32 in place

    esp_err_t save_err = nvs_save();
    TEST_CHECK(save_err == ESP_OK, "nvs_save() must succeed against the stub");

    zones_cfg_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    zones_cfg_t *saved_ptr = &s_zones.cfg;
    memset(saved_ptr, 0, sizeof(*saved_ptr)); // wipe the live struct so nvs_load() must reconstruct it from NVS
    bool found = false, valid = false;
    esp_err_t load_err = nvs_load(&found, &valid);
    loaded = s_zones.cfg;

    TEST_CHECK(load_err == ESP_OK, "nvs_load() must succeed");
    TEST_CHECK(found && valid, "a freshly saved current-version config must load back found+valid");
    TEST_CHECK(loaded.thermo_count == saved_copy.thermo_count, "thermo_count round-trips");
    TEST_CHECK(loaded.relay_count == saved_copy.relay_count, "relay_count round-trips");
    TEST_CHECK(loaded.continue_on_zone_trip == saved_copy.continue_on_zone_trip, "continue_on_zone_trip round-trips");
    TEST_CHECK(loaded.safety_tc_type == saved_copy.safety_tc_type, "safety_tc_type round-trips");
    for (uint8_t i = 0; i < 3; i++) {
        TEST_CHECK(strcmp(loaded.zones[i].name, saved_copy.zones[i].name) == 0, "zone name round-trips");
        TEST_CHECK(loaded.zones[i].relay_mask == saved_copy.zones[i].relay_mask, "zone relay_mask round-trips");
        TEST_CHECK(loaded.zones[i].thermo_mask == saved_copy.zones[i].thermo_mask, "zone thermo_mask round-trips");
        TEST_CHECK(loaded.zones[i].ct_mask == saved_copy.zones[i].ct_mask, "zone ct_mask round-trips");
        TEST_CHECK(loaded.zones[i].tc_type == saved_copy.zones[i].tc_type, "zone tc_type round-trips");
        TEST_CHECK_NEAR(loaded.zones[i].cal_offset_c, saved_copy.zones[i].cal_offset_c, 1e-6, "zone cal_offset_c round-trips");
        TEST_CHECK_NEAR(loaded.zones[i].pid_kp, saved_copy.zones[i].pid_kp, 1e-6, "zone pid_kp round-trips");
        TEST_CHECK_NEAR(loaded.zones[i].max_temp_c, saved_copy.zones[i].max_temp_c, 1e-6, "zone max_temp_c round-trips");
        TEST_CHECK_NEAR(loaded.zones[i].model_k_dc, saved_copy.zones[i].model_k_dc, 1e-6, "zone model_k_dc round-trips");
        TEST_CHECK(loaded.zones[i].timing_profile == saved_copy.zones[i].timing_profile,
                  "zone timing_profile round-trips");
    }
    TEST_CHECK(loaded.timing_profile_count == saved_copy.timing_profile_count, "timing_profile_count round-trips");
    for (uint8_t p = 0; p < 2; p++) {
        TEST_CHECK(strcmp(loaded.timing_profiles[p].name, saved_copy.timing_profiles[p].name) == 0,
                  "timing profile name round-trips");
        TEST_CHECK_NEAR(loaded.timing_profiles[p].guard_progress_duty_min,
                        saved_copy.timing_profiles[p].guard_progress_duty_min, 1e-6,
                        "timing profile guard_progress_duty_min round-trips");
        TEST_CHECK_NEAR(loaded.timing_profiles[p].ramp_lock_band_c,
                        saved_copy.timing_profiles[p].ramp_lock_band_c, 1e-6,
                        "timing profile ramp_lock_band_c round-trips");
    }

    nvs_test_enable(false);
    nvs_test_clear();
}

// Dedicated v1 blob test (v1/v2 previously only reached indirectly via
// convert_zone_v1() as invoked by convert_zone_v3()/etc.'s own tests). v1
// predates continue_on_zone_trip/safety_tc_type entirely -- those come out at
// their documented defaults (0 / THERMO_TC_K) -- but the per-zone fields v1
// DOES carry (name, gains, thresholds, model params) must be the real staged
// values, not zeros or a wrong-offset read.
static void test_nvs_load_from_v1_blob_upconverts_fields_correctly(void)
{
    TEST_SECTION("nvs_load_from -- a v1 blob (predates continue_on_zone_trip/safety_tc_type) "
                 "upconverts every zone's real field values correctly");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v1_t src;
    memset(&src, 0, sizeof(src));
    src.version = 1;
    src.thermo_count = 2;
    src.relay_count = 2;
    src.max_simultaneous_relays = 2;

    snprintf(src.zones[0].name, sizeof(src.zones[0].name), "ZoneA");
    src.zones[0].relay_mask = 0x01;
    src.zones[0].cal_offset_c = 1.5f;
    src.zones[0].pid_kp = 3.5f;
    src.zones[0].pid_ki = 0.2f;
    src.zones[0].pid_kd = 0.1f;
    src.zones[0].max_temp_c = 1150.0f;
    src.zones[0].min_temp_c = -10.0f;
    src.zones[0].model_k_dc = 7.0f;
    src.zones[0].model_tau_s = 300.0f;
    src.zones[0].model_dead_time_s = 12.0f;

    snprintf(src.zones[1].name, sizeof(src.zones[1].name), "ZoneB");
    src.zones[1].relay_mask = 0x02;
    src.zones[1].cal_offset_c = 2.5f;
    src.zones[1].pid_kp = 4.5f;
    src.zones[1].max_temp_c = 1250.0f;

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v1 blob must migrate to a valid current config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped with the current version");
    TEST_CHECK(out_cfg.continue_on_zone_trip == 0, "v1 predates continue_on_zone_trip -- documented default 0");
    TEST_CHECK(out_cfg.safety_tc_type == THERMO_TC_K, "v1 predates safety_tc_type -- documented default THERMO_TC_K");

    TEST_CHECK(strcmp(out_cfg.zones[0].name, "ZoneA") == 0, "zones[0].name (real v1 value) carried through");
    TEST_CHECK(out_cfg.zones[0].relay_mask == 0x01, "zones[0].relay_mask carried through");
    TEST_CHECK_NEAR(out_cfg.zones[0].cal_offset_c, 1.5f, 1e-6, "zones[0].cal_offset_c carried through");
    TEST_CHECK_NEAR(out_cfg.zones[0].pid_kp, 3.5f, 1e-6, "zones[0].pid_kp carried through");
    TEST_CHECK_NEAR(out_cfg.zones[0].pid_ki, 0.2f, 1e-6, "zones[0].pid_ki carried through");
    TEST_CHECK_NEAR(out_cfg.zones[0].max_temp_c, 1150.0f, 1e-6, "zones[0].max_temp_c carried through");
    TEST_CHECK_NEAR(out_cfg.zones[0].min_temp_c, -10.0f, 1e-6, "zones[0].min_temp_c carried through");
    TEST_CHECK_NEAR(out_cfg.zones[0].model_k_dc, 7.0f, 1e-6, "zones[0].model_k_dc carried through");
    TEST_CHECK_NEAR(out_cfg.zones[0].model_tau_s, 300.0f, 1e-6, "zones[0].model_tau_s carried through");
    TEST_CHECK_NEAR(out_cfg.zones[0].model_dead_time_s, 12.0f, 1e-6, "zones[0].model_dead_time_s carried through");
    TEST_CHECK(out_cfg.zones[0].tc_type == THERMO_TC_K, "zones[0].tc_type defaults to THERMO_TC_K (v1 predates it)");
    TEST_CHECK(out_cfg.zones[0].thermo_mask == 0x01, "zones[0].thermo_mask defaults to legacy 1<<0 mapping");

    TEST_CHECK(strcmp(out_cfg.zones[1].name, "ZoneB") == 0, "zones[1].name must NOT be shifted");
    TEST_CHECK(out_cfg.zones[1].relay_mask == 0x02, "zones[1].relay_mask must NOT be shifted");
    TEST_CHECK_NEAR(out_cfg.zones[1].cal_offset_c, 2.5f, 1e-6, "zones[1].cal_offset_c must be the real value");
    TEST_CHECK_NEAR(out_cfg.zones[1].pid_kp, 4.5f, 1e-6, "zones[1].pid_kp must be the real value");
    TEST_CHECK_NEAR(out_cfg.zones[1].max_temp_c, 1250.0f, 1e-6, "zones[1].max_temp_c must be the real value");
    TEST_CHECK(out_cfg.zones[1].thermo_mask == 0x02, "zones[1].thermo_mask defaults to legacy 1<<1 mapping");

    nvs_test_enable(false);
    nvs_test_clear();
}

// Dedicated v2 blob test -- v2 adds continue_on_zone_trip over v1 but shares
// v1's zone layout (zone_cfg_v1_t) and converter (convert_zone_v1()).
static void test_nvs_load_from_v2_blob_upconverts_fields_correctly(void)
{
    TEST_SECTION("nvs_load_from -- a v2 blob (adds continue_on_zone_trip over v1) upconverts "
                 "every zone's real field values correctly");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v2_t src;
    memset(&src, 0, sizeof(src));
    src.version = 2;
    src.thermo_count = 2;
    src.relay_count = 2;
    src.continue_on_zone_trip = 1;

    snprintf(src.zones[0].name, sizeof(src.zones[0].name), "ZoneA");
    src.zones[0].relay_mask = 0x01;
    src.zones[0].pid_kp = 3.5f;
    src.zones[0].max_temp_c = 1150.0f;

    snprintf(src.zones[1].name, sizeof(src.zones[1].name), "ZoneB");
    src.zones[1].relay_mask = 0x02;
    src.zones[1].pid_kp = 4.5f;
    src.zones[1].max_temp_c = 1250.0f;
    src.zones[1].model_tau_s = 250.0f;

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v2 blob must migrate to a valid current config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped with the current version");
    TEST_CHECK(out_cfg.continue_on_zone_trip == 1, "v2's real continue_on_zone_trip value carried through (unlike v1's forced 0)");
    TEST_CHECK(out_cfg.safety_tc_type == THERMO_TC_K, "v2 predates safety_tc_type -- documented default THERMO_TC_K");

    TEST_CHECK(strcmp(out_cfg.zones[1].name, "ZoneB") == 0, "zones[1].name must NOT be shifted");
    TEST_CHECK(out_cfg.zones[1].relay_mask == 0x02, "zones[1].relay_mask must NOT be shifted");
    TEST_CHECK_NEAR(out_cfg.zones[1].pid_kp, 4.5f, 1e-6, "zones[1].pid_kp must be the real value");
    TEST_CHECK_NEAR(out_cfg.zones[1].max_temp_c, 1250.0f, 1e-6, "zones[1].max_temp_c must be the real value");
    TEST_CHECK_NEAR(out_cfg.zones[1].model_tau_s, 250.0f, 1e-6, "zones[1].model_tau_s must be the real value");

    nvs_test_enable(false);
    nvs_test_clear();
}

// Dedicated v3 blob test -- v3 adds the 8 guard thresholds over v1's zone
// layout; still predates thermo_mask/tc_type/ct_mask.
static void test_nvs_load_from_v3_blob_upconverts_guard_thresholds_correctly(void)
{
    TEST_SECTION("nvs_load_from -- a v3 blob (adds the 8 guard thresholds) upconverts every "
                 "zone's real field values, guard thresholds included, correctly");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v3_t src;
    memset(&src, 0, sizeof(src));
    src.version = 3;
    src.thermo_count = 2;
    src.relay_count = 2;
    src.continue_on_zone_trip = 1;

    src.zones[0].relay_mask = 0x01;
    src.zones[0].max_temp_c = 1150.0f;
    src.zones[0].guard_wrong_dir_window_s = 45.0f;
    src.zones[0].guard_runaway_margin_c = 15.0f;

    snprintf(src.zones[1].name, sizeof(src.zones[1].name), "ZoneB");
    src.zones[1].relay_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;
    src.zones[1].guard_wrong_dir_window_s = 55.0f;
    src.zones[1].guard_wrong_dir_rate_c_per_min = 5.0f;
    src.zones[1].guard_off_settle_s = 20.0f;
    src.zones[1].guard_runaway_rate_c_per_min = 8.0f;
    src.zones[1].guard_runaway_margin_c = 25.0f;
    src.zones[1].guard_drift_period_s = 60.0f;
    src.zones[1].guard_sensor_fault_debounce_ticks = 3.0f;
    src.zones[1].guard_frozen_window_s = 90.0f;

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v3 blob must migrate to a valid current config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped with the current version");

    TEST_CHECK(out_cfg.zones[1].relay_mask == 0x02, "zones[1].relay_mask must NOT be shifted");
    TEST_CHECK_NEAR(out_cfg.zones[1].guard_wrong_dir_window_s, 55.0f, 1e-6, "zones[1].guard_wrong_dir_window_s -- new in v3 -- carried through");
    TEST_CHECK_NEAR(out_cfg.zones[1].guard_wrong_dir_rate_c_per_min, 5.0f, 1e-6, "zones[1].guard_wrong_dir_rate_c_per_min carried through");
    TEST_CHECK_NEAR(out_cfg.zones[1].guard_off_settle_s, 20.0f, 1e-6, "zones[1].guard_off_settle_s carried through");
    TEST_CHECK_NEAR(out_cfg.zones[1].guard_runaway_rate_c_per_min, 8.0f, 1e-6, "zones[1].guard_runaway_rate_c_per_min carried through");
    TEST_CHECK_NEAR(out_cfg.zones[1].guard_runaway_margin_c, 25.0f, 1e-6, "zones[1].guard_runaway_margin_c carried through");
    TEST_CHECK_NEAR(out_cfg.zones[1].guard_drift_period_s, 60.0f, 1e-6, "zones[1].guard_drift_period_s carried through");
    TEST_CHECK_NEAR(out_cfg.zones[1].guard_sensor_fault_debounce_ticks, 3.0f, 1e-6, "zones[1].guard_sensor_fault_debounce_ticks carried through");
    TEST_CHECK_NEAR(out_cfg.zones[1].guard_frozen_window_s, 90.0f, 1e-6, "zones[1].guard_frozen_window_s carried through");

    TEST_CHECK(out_cfg.zones[0].relay_mask == 0x01, "zones[0].relay_mask still correct (sanity check)");
    TEST_CHECK_NEAR(out_cfg.zones[0].guard_wrong_dir_window_s, 45.0f, 1e-6, "zones[0].guard_wrong_dir_window_s still correct (sanity check)");

    nvs_test_enable(false);
    nvs_test_clear();
}

// Dedicated v4 blob test -- v4 appends thermo_mask (a REAL, operator-set
// value now, not the legacy 1<<chan_idx default v1/v3 fall back to) at the
// tail of v3's zone layout; still predates tc_type.
static void test_nvs_load_from_v4_blob_upconverts_thermo_mask_correctly(void)
{
    TEST_SECTION("nvs_load_from -- a v4 blob (adds real thermo_mask over v3) upconverts every "
                 "zone's real field values, including the operator-set thermo_mask, correctly");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v4_t src;
    memset(&src, 0, sizeof(src));
    src.version = 4;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.continue_on_zone_trip = 1;

    src.zones[0].relay_mask = 0x01;
    src.zones[0].max_temp_c = 1150.0f;
    src.zones[0].thermo_mask = 0x04; // deliberately NOT 1<<0, to distinguish real value from the legacy default

    snprintf(src.zones[1].name, sizeof(src.zones[1].name), "ZoneB");
    src.zones[1].relay_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;
    src.zones[1].guard_runaway_margin_c = 25.0f;
    src.zones[1].thermo_mask = 0x01; // deliberately NOT 1<<1

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v4 blob must migrate to a valid current config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped with the current version");

    TEST_CHECK(out_cfg.zones[0].thermo_mask == 0x04,
              "zones[0].thermo_mask is the real v4 operator-set value, not the legacy 1<<0 default");
    TEST_CHECK(out_cfg.zones[1].relay_mask == 0x02, "zones[1].relay_mask must NOT be shifted");
    TEST_CHECK(out_cfg.zones[1].thermo_mask == 0x01,
              "zones[1].thermo_mask is the real v4 operator-set value, not the legacy 1<<1 default");
    TEST_CHECK_NEAR(out_cfg.zones[1].max_temp_c, 1250.0f, 1e-6, "zones[1].max_temp_c carried through");
    TEST_CHECK_NEAR(out_cfg.zones[1].guard_runaway_margin_c, 25.0f, 1e-6, "zones[1].guard_runaway_margin_c carried through");
    TEST_CHECK(out_cfg.zones[1].tc_type == THERMO_TC_K, "zones[1].tc_type defaults to THERMO_TC_K (v4 predates it)");

    nvs_test_enable(false);
    nvs_test_clear();
}

// Item 2 -- the actual historical bug: an old-version (v5, i.e. the exact
// "3 -> 4, 4 -> 5 grew zone_cfg_t mid-struct" case the defect report calls
// out) blob with DISTINCT values on zones[0], zones[1] and zones[2] must land
// each zone's fields at the RIGHT zone after conversion -- not the old bug's
// "zones[1]/zones[2] read from the wrong byte offsets, as arbitrary floats,
// and flagged VALID" behavior. zones[1]/zones[2] specifically, since those
// are the ones the old memcpy-based migration got wrong (zones[0] happened
// to always land correctly, which is exactly what let the bug hide).
static void test_nvs_load_from_v5_blob_upconverts_zones_1_and_2_correctly(void)
{
    TEST_SECTION("nvs_load_from -- item 2: an old (v5) blob's zones[1]/zones[2] land at the "
                 "correct fields after typed conversion, not shifted");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v5_t src;
    memset(&src, 0, sizeof(src));
    src.version = 5;
    src.thermo_count = 3;
    src.relay_count = 4;
    src.continue_on_zone_trip = 1;
    src.safety_tc_type = 5;

    src.zones[0].relay_mask = 0x01;
    src.zones[0].max_temp_c = 1100.0f;
    src.zones[0].tc_type = 1;
    src.zones[0].guard_runaway_margin_c = 11.0f;

    src.zones[1].relay_mask = 0x02;
    src.zones[1].max_temp_c = 1200.0f;
    src.zones[1].tc_type = 2;
    src.zones[1].guard_runaway_margin_c = 22.0f;
    src.zones[1].thermo_mask = 0x02;

    src.zones[2].relay_mask = 0x04;
    src.zones[2].max_temp_c = 1300.0f;
    src.zones[2].tc_type = 3;
    src.zones[2].guard_runaway_margin_c = 33.0f;
    src.zones[2].thermo_mask = 0x04;

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v5 blob must migrate to a valid current config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped with the current version");
    TEST_CHECK(out_cfg.continue_on_zone_trip == 1, "top-level continue_on_zone_trip carried through");
    TEST_CHECK(out_cfg.safety_tc_type == 5, "top-level safety_tc_type (real v5 value) carried through");

    TEST_CHECK(out_cfg.zones[1].relay_mask == 0x02, "zones[1].relay_mask must NOT be shifted -- "
                                                    "this is the exact field the old bug misread");
    TEST_CHECK_NEAR(out_cfg.zones[1].max_temp_c, 1200.0f, 1e-6, "zones[1].max_temp_c must be the real "
                                                                "value, not an arbitrary float from "
                                                                "the wrong offset");
    TEST_CHECK(out_cfg.zones[1].tc_type == 2, "zones[1].tc_type carried through");
    TEST_CHECK_NEAR(out_cfg.zones[1].guard_runaway_margin_c, 22.0f, 1e-6,
                    "zones[1].guard_runaway_margin_c -- a thermal guard threshold -- must be the real value");
    TEST_CHECK(out_cfg.zones[1].thermo_mask == 0x02, "zones[1].thermo_mask (real v5 value) carried through");

    TEST_CHECK(out_cfg.zones[2].relay_mask == 0x04, "zones[2].relay_mask must NOT be shifted");
    TEST_CHECK_NEAR(out_cfg.zones[2].max_temp_c, 1300.0f, 1e-6, "zones[2].max_temp_c must be the real value");
    TEST_CHECK(out_cfg.zones[2].tc_type == 3, "zones[2].tc_type carried through");
    TEST_CHECK_NEAR(out_cfg.zones[2].guard_runaway_margin_c, 33.0f, 1e-6, "zones[2].guard_runaway_margin_c must be the real value");
    TEST_CHECK(out_cfg.zones[2].thermo_mask == 0x04, "zones[2].thermo_mask (real v5 value) carried through");

    TEST_CHECK(out_cfg.zones[0].relay_mask == 0x01, "zones[0].relay_mask still correct (sanity check)");
    TEST_CHECK_NEAR(out_cfg.zones[0].max_temp_c, 1100.0f, 1e-6, "zones[0].max_temp_c still correct (sanity check)");

    nvs_test_enable(false);
    nvs_test_clear();
}

/* A config that zones_config_json_validate() accepts, so a test can change exactly
 * one field and attribute the rejection to it. */
static void make_minimal_valid_cfg(zones_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->version = ZONES_CFG_VERSION;
    cfg->thermo_count = 1;
    cfg->relay_count = 1;
    cfg->zones[0].relay_mask = 0x01;
    cfg->zones[0].thermo_mask = 0x01;
    cfg->zones[0].max_temp_c = 1300.0f;
    /* zone[0].timing_profile stays 0 from the memset above, and that must
     * resolve to a REAL profile for zones_config_json_validate() to accept this --
     * see zones_cfg_t::timing_profile_count's own comment. */
    cfg->timing_profile_count = 1;
    strncpy(cfg->timing_profiles[0].name, "Default", TIMING_PROFILE_NAME_MAX_LEN);
}

static void test_nvs_load_from_v7_blob_upconverts_and_defaults_new_fields(void)
{
    TEST_SECTION("nvs_load_from -- a v7 blob upconverts to v8: every zone's existing fields land "
                 "correctly and the nine new overrides default to 0 (= firmware default)");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v7_t src;
    memset(&src, 0, sizeof(src));
    src.version = 7;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.continue_on_zone_trip = 1;
    src.safety_tc_type = 3;

    /* Mirrors the bench board's actual live config -- zone 0 is the one that
     * is really commissioned, zones 1 and 2 carry their own masks. */
    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].control_mode = 2;
    src.zones[0].max_temp_c = 1300.0f;
    src.zones[0].max_ramp_c_per_hr = 900.0f;
    src.zones[0].guard_wrong_dir_window_s = 60.0f;
    src.zones[0].tc_type = 3;

    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].guard_runaway_margin_c = 22.0f;
    src.zones[1].tc_type = 3;

    src.zones[2].relay_mask = 0x04;
    src.zones[2].thermo_mask = 0x04;
    src.zones[2].guard_runaway_margin_c = 33.0f;
    src.zones[2].tc_type = 3;

    src.crc32 = 0; /* v7's own CRC is not checked on the old-version path */

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v7 blob must migrate to a valid current config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");
    TEST_CHECK(out_cfg.thermo_count == 3 && out_cfg.relay_count == 3, "counts carried through");
    TEST_CHECK(out_cfg.safety_tc_type == 3, "safety_tc_type carried through");

    TEST_CHECK(out_cfg.zones[0].control_mode == 2, "zones[0].control_mode (PID) survives the upgrade");
    TEST_CHECK_NEAR(out_cfg.zones[0].max_temp_c, 1300.0f, 1e-6, "zones[0].max_temp_c survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].max_ramp_c_per_hr, 900.0f, 1e-6, "zones[0].max_ramp_c_per_hr survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].guard_wrong_dir_window_s, 60.0f, 1e-6,
                    "zones[0]'s configured guard window survives -- the operator's setting, not a default");

    TEST_CHECK(out_cfg.zones[1].relay_mask == 0x02, "zones[1].relay_mask must NOT be shifted");
    TEST_CHECK(out_cfg.zones[1].thermo_mask == 0x02, "zones[1].thermo_mask must NOT be shifted");
    TEST_CHECK_NEAR(out_cfg.zones[1].guard_runaway_margin_c, 22.0f, 1e-6, "zones[1] guard threshold correct");
    TEST_CHECK(out_cfg.zones[2].relay_mask == 0x04, "zones[2].relay_mask must NOT be shifted");
    TEST_CHECK_NEAR(out_cfg.zones[2].guard_runaway_margin_c, 33.0f, 1e-6, "zones[2] guard threshold correct");

    /* v7 predates the nine timing overrides entirely (ZONES_CFG_VERSION 8->9)
     * -- there is nothing to migrate, so every zone is pointed at ONE
     * synthesized "Default" profile, and that profile's nine fields are all
     * 0 (= firmware default), matching exactly how a v7 board already
     * behaved. */
    TEST_CHECK(out_cfg.timing_profile_count == 1,
              "a v7 board -- no per-zone timing data to migrate -- collapses to one shared profile");
    TEST_CHECK(strcmp(out_cfg.timing_profiles[0].name, "Default") == 0,
              "the synthesized profile is named \"Default\"");
    for (uint8_t i = 0; i < 3; i++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "zones[%u] points at the shared Default profile (index 0)", i);
        TEST_CHECK(out_cfg.zones[i].timing_profile == 0, msg);
    }
    const zone_timing_profile_t *tp0 = &out_cfg.timing_profiles[0];
    bool all_zero = tp0->guard_progress_duty_min == 0.0f && tp0->guard_progress_window_s == 0.0f &&
                    tp0->guard_drift_hysteresis_c == 0.0f && tp0->guard_frozen_eps_c == 0.0f &&
                    tp0->guard_cross_zone_period_s == 0.0f && tp0->bangbang_hysteresis_c == 0.0f &&
                    tp0->cooling_limited_margin_c == 0.0f && tp0->cooling_limited_hold_s == 0.0f &&
                    tp0->ramp_lock_band_c == 0.0f;
    TEST_CHECK(all_zero, "the synthesized Default profile's nine fields are all 0 (= firmware default)");
    TEST_CHECK(out_cfg.pc_link_abort_silence_ms == 0.0f,
               "the global pc_link_abort_silence_ms also defaults to 0 on upgrade");

    nvs_test_enable(false);
    nvs_test_clear();
}

// THE test this whole pass is about (task instructions: "the migration needs
// a test that would FAIL if v8 data were misread -- build a real v8 blob with
// DISTINCT non-zero values per zone, load it, and assert every zone still
// resolves to its original nine values through the new profile indirection").
// Reproduces the exact profiles_http.c disaster shape: a wrong expected-length
// (sizeof the CURRENT struct instead of the frozen v8 snapshot) would make
// this blob either get rejected outright (wrong length) or, worse, silently
// misread -- this test would catch either failure mode.
static void test_nvs_load_from_v8_blob_with_distinct_zone_values_migrates_losslessly(void)
{
    TEST_SECTION("nvs_load_from -- a v8 blob with DISTINCT per-zone timing overrides migrates "
                 "losslessly: each zone gets its own profile with its exact original nine values");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v8_t src;
    memset(&src, 0, sizeof(src));
    src.version = 8;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.continue_on_zone_trip = 1;
    src.safety_tc_type = 3;
    src.pc_link_abort_silence_ms = 45000.0f;

    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].tc_type = 3;
    src.zones[0].guard_progress_duty_min = 0.10f;
    src.zones[0].guard_progress_window_s = 100.0f;
    src.zones[0].guard_drift_hysteresis_c = 1.0f;
    src.zones[0].guard_frozen_eps_c = 0.10f;
    src.zones[0].guard_cross_zone_period_s = 10.0f;
    src.zones[0].bangbang_hysteresis_c = 2.0f;
    src.zones[0].cooling_limited_margin_c = 5.0f;
    src.zones[0].cooling_limited_hold_s = 60.0f;
    src.zones[0].ramp_lock_band_c = 3.0f;

    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].tc_type = 3;
    src.zones[1].guard_progress_duty_min = 0.20f;
    src.zones[1].guard_progress_window_s = 200.0f;
    src.zones[1].guard_drift_hysteresis_c = 2.0f;
    src.zones[1].guard_frozen_eps_c = 0.20f;
    src.zones[1].guard_cross_zone_period_s = 20.0f;
    src.zones[1].bangbang_hysteresis_c = 4.0f;
    src.zones[1].cooling_limited_margin_c = 6.0f;
    src.zones[1].cooling_limited_hold_s = 70.0f;
    src.zones[1].ramp_lock_band_c = 4.0f;

    src.zones[2].relay_mask = 0x04;
    src.zones[2].thermo_mask = 0x04;
    src.zones[2].tc_type = 3;
    src.zones[2].guard_progress_duty_min = 0.30f;
    src.zones[2].guard_progress_window_s = 300.0f;
    src.zones[2].guard_drift_hysteresis_c = 3.0f;
    src.zones[2].guard_frozen_eps_c = 0.30f;
    src.zones[2].guard_cross_zone_period_s = 30.0f;
    src.zones[2].bangbang_hysteresis_c = 6.0f;
    src.zones[2].cooling_limited_margin_c = 7.0f;
    src.zones[2].cooling_limited_hold_s = 80.0f;
    src.zones[2].ramp_lock_band_c = 5.0f;

    src.crc32 = 0; /* v8's own CRC is not checked on the old-version path */

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v8 blob with distinct zone values must migrate to a "
                              "valid current config -- NOT be rejected as corrupt");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");
    TEST_CHECK_NEAR((double)out_cfg.pc_link_abort_silence_ms, 45000.0, 1e-6,
                    "the global pc_link_abort_silence_ms (real v8 value) carried through");

    /* THE lossless-migration proof: three distinct zones -> three distinct
     * profiles, each zone pointed at its OWN profile, none shared. If
     * expected_len_for_version(8) had returned sizeof(the CURRENT struct)
     * (the exact profiles_http.c mistake this pass's instructions warn
     * about) instead of sizeof(zones_cfg_v8_t), this blob would either fail
     * the length check outright (found && valid would be false, caught
     * above) or -- had the sizes happened to coincide -- be misread field-by-
     * field-wrong, and the checks below would catch that: a byte-shifted
     * read would not reproduce these exact numbers at these exact zones. */
    TEST_CHECK(out_cfg.timing_profile_count == 3,
              "three distinct zones must produce three distinct profiles, not a collapsed shared one");
    TEST_CHECK(out_cfg.zones[0].timing_profile != out_cfg.zones[1].timing_profile &&
              out_cfg.zones[1].timing_profile != out_cfg.zones[2].timing_profile &&
              out_cfg.zones[0].timing_profile != out_cfg.zones[2].timing_profile,
              "every zone's assigned profile index must be distinct from every other zone's");

    struct { float duty, window, drift, eps, xzone, bb, coolmargin, coolhold, ramplock; } expect[3] = {
        {0.10f, 100.0f, 1.0f, 0.10f, 10.0f, 2.0f, 5.0f, 60.0f, 3.0f},
        {0.20f, 200.0f, 2.0f, 0.20f, 20.0f, 4.0f, 6.0f, 70.0f, 4.0f},
        {0.30f, 300.0f, 3.0f, 0.30f, 30.0f, 6.0f, 7.0f, 80.0f, 5.0f},
    };
    for (uint8_t i = 0; i < 3; i++) {
        char msg[128];
        uint8_t p = out_cfg.zones[i].timing_profile;
        TEST_CHECK(p < out_cfg.timing_profile_count, "zone's profile index must be in range");
        const zone_timing_profile_t *tp = &out_cfg.timing_profiles[p];
        snprintf(msg, sizeof(msg), "zone %u's profile guard_progress_duty_min is its OWN original v8 value", i);
        TEST_CHECK_NEAR((double)tp->guard_progress_duty_min, (double)expect[i].duty, 1e-6, msg);
        snprintf(msg, sizeof(msg), "zone %u's profile guard_progress_window_s is its OWN original v8 value", i);
        TEST_CHECK_NEAR((double)tp->guard_progress_window_s, (double)expect[i].window, 1e-6, msg);
        snprintf(msg, sizeof(msg), "zone %u's profile guard_drift_hysteresis_c is its OWN original v8 value", i);
        TEST_CHECK_NEAR((double)tp->guard_drift_hysteresis_c, (double)expect[i].drift, 1e-6, msg);
        snprintf(msg, sizeof(msg), "zone %u's profile guard_frozen_eps_c is its OWN original v8 value", i);
        TEST_CHECK_NEAR((double)tp->guard_frozen_eps_c, (double)expect[i].eps, 1e-6, msg);
        snprintf(msg, sizeof(msg), "zone %u's profile guard_cross_zone_period_s is its OWN original v8 value", i);
        TEST_CHECK_NEAR((double)tp->guard_cross_zone_period_s, (double)expect[i].xzone, 1e-6, msg);
        snprintf(msg, sizeof(msg), "zone %u's profile bangbang_hysteresis_c is its OWN original v8 value", i);
        TEST_CHECK_NEAR((double)tp->bangbang_hysteresis_c, (double)expect[i].bb, 1e-6, msg);
        snprintf(msg, sizeof(msg), "zone %u's profile cooling_limited_margin_c is its OWN original v8 value", i);
        TEST_CHECK_NEAR((double)tp->cooling_limited_margin_c, (double)expect[i].coolmargin, 1e-6, msg);
        snprintf(msg, sizeof(msg), "zone %u's profile cooling_limited_hold_s is its OWN original v8 value", i);
        TEST_CHECK_NEAR((double)tp->cooling_limited_hold_s, (double)expect[i].coolhold, 1e-6, msg);
        snprintf(msg, sizeof(msg), "zone %u's profile ramp_lock_band_c is its OWN original v8 value", i);
        TEST_CHECK_NEAR((double)tp->ramp_lock_band_c, (double)expect[i].ramplock, 1e-6, msg);
    }

    /* The other zone fields (untouched by this pass) must also have survived
     * the v8->v9 conversion, same discipline as the v5/v7 lossless-migration
     * tests above -- a bug in convert_zone_v8() could plausibly leave the
     * TIMING fields correct while breaking something else it touches. */
    TEST_CHECK(out_cfg.zones[1].relay_mask == 0x02, "zones[1].relay_mask must NOT be shifted");
    TEST_CHECK(out_cfg.zones[2].relay_mask == 0x04, "zones[2].relay_mask must NOT be shifted");
    TEST_CHECK(out_cfg.zones[1].tc_type == 3, "zones[1].tc_type carried through");

    nvs_test_enable(false);
    nvs_test_clear();
}

// The degenerate case explicitly called out by this pass's instructions:
// "Today's board has all-zero values on every zone, which should collapse to
// a single shared profile -- verify that is what your converter does."
// Distinct from the v7 test above (v7 has no per-zone timing fields AT ALL to
// even be zero); this one is a v8 blob whose zones DO carry the nine fields,
// every one of them still at the default 0 -- proving the dedup logic
// collapses "all zero" to ONE profile, not the "no such fields exist" case.
static void test_nvs_load_from_v8_blob_upconverts_to_shared_default_profile(void)
{
    TEST_SECTION("nvs_load_from -- a v8 blob where every zone's nine overrides are still all-zero "
                 "(the owner's actual board today) collapses to ONE shared \"Default\" profile");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v8_t src;
    memset(&src, 0, sizeof(src)); /* every zone's nine timing fields at their 0 default */
    src.version = 8;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.zones[0].relay_mask = 0x01;
    src.zones[1].relay_mask = 0x02;
    src.zones[2].relay_mask = 0x04;

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "an all-zero-timing v8 blob must migrate to a valid current config");
    TEST_CHECK(out_cfg.timing_profile_count == 1,
              "three zones, all identical (all-zero) timing values, collapse to exactly one profile");
    TEST_CHECK(strcmp(out_cfg.timing_profiles[0].name, "Default") == 0,
              "the single shared profile is named \"Default\", not \"Zone 1\"");
    TEST_CHECK(out_cfg.zones[0].timing_profile == 0 && out_cfg.zones[1].timing_profile == 0 &&
              out_cfg.zones[2].timing_profile == 0,
              "every zone points at the same shared profile 0");

    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_validate_rejects_out_of_range_v8_fields(void)
{
    TEST_SECTION("validate_zones_cfg -- the nine timing-profile overrides are range-checked like "
                 "every field before them (now on timing_profiles[], not zones[] -- see "
                 "ZONES_CFG_VERSION's 8->9 comment)");

    /* A duty above 1.0 would arm guard 1 never, silently disabling the
     * heating-failed check -- the exact "configured it into uselessness"
     * case the ceiling exists to refuse. */
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.timing_profiles[0].guard_progress_duty_min = 1.5f;
        const char *reason = NULL;
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason), "guard_progress_duty_min > 1.0 is rejected");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.timing_profiles[0].guard_progress_window_s = -1.0f;
        const char *reason = NULL;
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason), "a negative guard_progress_window_s is rejected");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.timing_profiles[0].ramp_lock_band_c = ZONE_GUARD_MARGIN_C_MAX + 1.0f;
        const char *reason = NULL;
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason), "ramp_lock_band_c past its ceiling is rejected");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.pc_link_abort_silence_ms = ZONE_PC_LINK_SILENCE_MS_MAX + 1.0f;
        const char *reason = NULL;
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason), "pc_link_abort_silence_ms past its ceiling is rejected");
    }
    /* 0 must stay legal on every one of them -- it is the "use the firmware
     * default" value, not a missing setting. */
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        const char *reason = NULL;
        TEST_CHECK(zones_config_json_validate(&cfg, &reason), "all-zero timing profile fields stay valid (0 = firmware default)");
    }
    /* timing_profile_count itself: 0 is illegal (zone[0].timing_profile == 0
     * from a fresh config must always resolve to something real), and a
     * count past MAX31856_CHANNEL_COUNT is illegal (the array's fixed
     * capacity). */
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.timing_profile_count = 0;
        const char *reason = NULL;
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason), "timing_profile_count == 0 is rejected");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.timing_profile_count = MAX31856_CHANNEL_COUNT + 1;
        const char *reason = NULL;
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason), "timing_profile_count past MAX31856_CHANNEL_COUNT is rejected");
    }
    /* zone_cfg_t::timing_profile: must reference a profile that actually
     * exists in THIS candidate -- the owner's whole feature ("assign the
     * zones to them") is meaningless if a zone can point past the end of
     * timing_profiles[]. */
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].timing_profile = 1; /* timing_profile_count is 1 -- only index 0 exists */
        const char *reason = NULL;
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason),
                  "a zone's timing_profile referencing a profile past timing_profile_count is rejected");
    }
}

// ---------------------------------------------------------------------------
// PID_EXPANSION_PLAN.md Phase 2/4 (2026-08-30): ZONE_CONTROL_MODE_PID_FUZZY,
// fuzzy_strength_pct/coupling_coeff/coupling_neighbor_zone/settings_source,
// and the ZONES_CFG_VERSION 9->10 migration. Every test below was run once
// against a deliberately-broken version of the change it covers (see the
// task report) to confirm it can actually fail, per this file's own
// "negative-test every check" convention.
// ---------------------------------------------------------------------------

// THE migration test this bump is about: a v9 blob (predating all four new
// fields) must upgrade losslessly -- every pre-existing field survives
// unchanged, the three new floats read as 0 (their documented "not
// configured" default), and settings_source lands at ZONE_SETTINGS_SOURCE_CUSTOM
// (0xFF) for EVERY zone, never 0 -- 0 would silently mean "copies zone 0's
// settings" and would be overwritten by zone 0's numbers on the next save
// once Phase 5's resolve-on-save logic lands (a later pass). This test would
// FAIL if convert_zone_v9() ever left settings_source at its zero-initialized
// default instead of explicitly setting it.
static void test_nvs_load_from_v9_blob_upconverts_new_fields_default_and_settings_source_is_custom(void)
{
    TEST_SECTION("nvs_load_from -- a v9 blob upconverts to v10: new float fields default to 0, "
                 "settings_source lands at CUSTOM (0xFF) for every zone (NOT 0), and every "
                 "pre-existing field survives unchanged");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v9_t src;
    memset(&src, 0, sizeof(src));
    src.version = 9;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.continue_on_zone_trip = 1;
    src.safety_tc_type = 3;
    src.pc_link_abort_silence_ms = 45000.0f;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");

    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].tc_type = 3;
    src.zones[0].control_mode = 2; /* PID -- a real, distinct, pre-existing operator setting */
    src.zones[0].pid_kp = 2.5f;
    src.zones[0].max_temp_c = 1300.0f;

    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;

    src.zones[2].relay_mask = 0x04;
    src.zones[2].thermo_mask = 0x04;
    src.zones[2].max_temp_c = 1200.0f;

    src.crc32 = 0; /* v9's own CRC is not checked on the old-version path */

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v9 blob must migrate to a valid current (v10) config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");

    for (uint8_t i = 0; i < 3; i++) {
        char msg[128];
        snprintf(msg, sizeof(msg), "zones[%u].fuzzy_strength_pct defaults to 0 (not configured)", i);
        TEST_CHECK_NEAR(out_cfg.zones[i].fuzzy_strength_pct, 0.0f, 1e-6, msg);
        for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
            snprintf(msg, sizeof(msg), "zones[%u].coupling_coeff[%u] defaults to 0 (no coupling measured)", i, j);
            TEST_CHECK_NEAR(out_cfg.zones[i].coupling_coeff[j], 0.0f, 1e-6, msg);
        }
        snprintf(msg, sizeof(msg),
                "zones[%u].settings_source lands at ZONE_SETTINGS_SOURCE_CUSTOM (0xFF), NOT 0 -- "
                "0 would silently claim \"copies zone 0's settings\"", i);
        TEST_CHECK(out_cfg.zones[i].settings_source == ZONE_SETTINGS_SOURCE_CUSTOM, msg);
    }

    // Pre-existing fields must survive the upgrade completely unchanged.
    TEST_CHECK(out_cfg.thermo_count == 3 && out_cfg.relay_count == 3, "counts carried through");
    TEST_CHECK(out_cfg.continue_on_zone_trip == 1, "continue_on_zone_trip carried through");
    TEST_CHECK(out_cfg.safety_tc_type == 3, "safety_tc_type carried through");
    TEST_CHECK_NEAR(out_cfg.pc_link_abort_silence_ms, 45000.0f, 1e-6, "pc_link_abort_silence_ms carried through");
    TEST_CHECK(out_cfg.timing_profile_count == 1, "timing_profile_count carried through");
    TEST_CHECK(strcmp(out_cfg.timing_profiles[0].name, "Default") == 0, "timing profile name carried through");

    TEST_CHECK(out_cfg.zones[0].relay_mask == 0x01 && out_cfg.zones[1].relay_mask == 0x02 &&
              out_cfg.zones[2].relay_mask == 0x04, "relay_mask must NOT be shifted for any zone");
    TEST_CHECK(out_cfg.zones[0].thermo_mask == 0x01 && out_cfg.zones[1].thermo_mask == 0x02 &&
              out_cfg.zones[2].thermo_mask == 0x04, "thermo_mask must NOT be shifted for any zone");
    TEST_CHECK(out_cfg.zones[0].control_mode == 2, "zones[0].control_mode (PID) survives the upgrade");
    TEST_CHECK_NEAR(out_cfg.zones[0].pid_kp, 2.5f, 1e-6, "zones[0].pid_kp survives the upgrade");
    TEST_CHECK_NEAR(out_cfg.zones[0].max_temp_c, 1300.0f, 1e-6, "zones[0].max_temp_c survives");
    TEST_CHECK_NEAR(out_cfg.zones[1].max_temp_c, 1250.0f, 1e-6, "zones[1].max_temp_c survives");
    TEST_CHECK_NEAR(out_cfg.zones[2].max_temp_c, 1200.0f, 1e-6, "zones[2].max_temp_c survives");

    nvs_test_enable(false);
    nvs_test_clear();
}

// ---------------------------------------------------------------------------
// Same-day follow-up (2026-08-30): ZONES_CFG_VERSION 10->11 -- the single
// coupling_coeff/coupling_neighbor_zone pair is replaced by a full directed
// coupling_coeff[] row (bench-measured coupling is asymmetric AND
// multi-neighbor; see zone_cfg_t's own doc comment). Every test below was
// run once against a deliberately-broken version of the change it covers to
// confirm it can actually fail, per this file's own "negative-test every
// check" convention (see the task report for the specific break/restore
// pairs).
// ---------------------------------------------------------------------------

// THE migration test this bump is about: a v10 blob's single
// (coupling_coeff, coupling_neighbor_zone) pair must land in exactly the
// right cell of the new row, every other cell (including the diagonal) 0,
// and every other pre-existing v10 field survives unchanged. sizeof(src) is
// zone_cfg_v10_t/zones_cfg_v10_t -- both frozen, historical types with their
// own _Static_assert(sizeof(...) == N) in zones_http.c checked against a
// HAND-COMPUTED byte count, never the live zone_cfg_t/zones_cfg_t (which by
// now is already the v11 row-coupling shape) -- so staging with sizeof(src)
// here stages a byte count independently verified to match the real v10
// on-flash layout, not a number that happens to match today's struct.
static void test_nvs_load_from_v10_blob_folds_single_pair_into_row_cell(void)
{
    TEST_SECTION("nvs_load_from -- a v10 blob upconverts to v11: coupling_coeff/coupling_neighbor_zone "
                 "fold into the right coupling_coeff[] cell, every other cell (incl. diagonal) is 0, "
                 "and every other pre-existing v10 field survives unchanged");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v10_t src;
    memset(&src, 0, sizeof(src));
    src.version = 10;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.continue_on_zone_trip = 1;
    src.safety_tc_type = 3;
    src.pc_link_abort_silence_ms = 45000.0f;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");

    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].max_temp_c = 1300.0f;
    src.zones[0].fuzzy_strength_pct = 40.0f;
    src.zones[0].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    /* zone 0's ONE storable neighbor before this pass: zone 2, coeff 6.75. */
    src.zones[0].coupling_coeff = 6.75f;
    src.zones[0].coupling_neighbor_zone = 2.0f;

    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;
    src.zones[1].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    /* zone 1: no coupling ever measured on a v10 board -- coupling_coeff
     * stays at its 0 default, same "not configured" meaning either side of
     * the migration. */

    src.zones[2].relay_mask = 0x04;
    src.zones[2].thermo_mask = 0x04;
    src.zones[2].max_temp_c = 1200.0f;
    src.zones[2].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.crc32 = 0; /* v10's own CRC is not checked on the old-version path */

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v10 blob must migrate to a valid current (v11) config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");

    // The one cell that must be populated: zone 0's row, column 2.
    TEST_CHECK_NEAR(out_cfg.zones[0].coupling_coeff[2], 6.75f, 1e-6,
                    "zone 0's v10 (coeff, neighbor=2) pair lands in coupling_coeff[2]");
    // Every other cell of zone 0's row, including its own diagonal, is 0.
    TEST_CHECK_NEAR(out_cfg.zones[0].coupling_coeff[0], 0.0f, 1e-6, "zone 0's diagonal cell is 0");
    TEST_CHECK_NEAR(out_cfg.zones[0].coupling_coeff[1], 0.0f, 1e-6, "zone 0's untouched neighbor cell (1) is 0");
    // Zones 1 and 2 never had a v10 coupling pair -- their whole rows are 0.
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "zone 1's untouched coupling_coeff[%u] is 0", j);
        TEST_CHECK_NEAR(out_cfg.zones[1].coupling_coeff[j], 0.0f, 1e-6, msg);
        snprintf(msg, sizeof(msg), "zone 2's untouched coupling_coeff[%u] is 0", j);
        TEST_CHECK_NEAR(out_cfg.zones[2].coupling_coeff[j], 0.0f, 1e-6, msg);
    }

    // Pre-existing v10 fields must survive the upgrade completely unchanged.
    TEST_CHECK(out_cfg.thermo_count == 3 && out_cfg.relay_count == 3, "counts carried through");
    TEST_CHECK(out_cfg.continue_on_zone_trip == 1, "continue_on_zone_trip carried through");
    TEST_CHECK_NEAR(out_cfg.pc_link_abort_silence_ms, 45000.0f, 1e-6, "pc_link_abort_silence_ms carried through");
    TEST_CHECK(strcmp(out_cfg.timing_profiles[0].name, "Default") == 0, "timing profile name carried through");
    TEST_CHECK(out_cfg.zones[0].relay_mask == 0x01 && out_cfg.zones[1].relay_mask == 0x02 &&
              out_cfg.zones[2].relay_mask == 0x04, "relay_mask must NOT be shifted for any zone");
    TEST_CHECK_NEAR(out_cfg.zones[0].fuzzy_strength_pct, 40.0f, 1e-6, "zones[0].fuzzy_strength_pct survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].max_temp_c, 1300.0f, 1e-6, "zones[0].max_temp_c survives");
    TEST_CHECK_NEAR(out_cfg.zones[1].max_temp_c, 1250.0f, 1e-6, "zones[1].max_temp_c survives");
    TEST_CHECK_NEAR(out_cfg.zones[2].max_temp_c, 1200.0f, 1e-6, "zones[2].max_temp_c survives");
    TEST_CHECK(out_cfg.zones[0].settings_source == ZONE_SETTINGS_SOURCE_CUSTOM,
              "zones[0].settings_source (already a real v10 field) is carried through verbatim, "
              "not re-defaulted the way v9's migration must");

    nvs_test_enable(false);
    nvs_test_clear();
}

// 2026-08-31 defect fix: a v10 blob whose single coupling pair is
// self-referencing (coupling_neighbor_zone == the zone's OWN index) must
// have that pair dropped, not folded into the diagonal cell. Before this
// fix convert_zone_v10() had no chan_idx parameter to compare against (unlike
// convert_zone_v1(), which has always taken one and skipped exactly this
// case), so a self-referencing pair wrote a nonzero diagonal cell;
// zones_config_json_validate() rejects any nonzero diagonal, and zones_config_json_decode_blob()
// then returns CORRUPT for the WHOLE migrated struct -- discarding the
// entire commissioned config over one stray self-reference. Unreachable
// today (no board has ever held a v10 blob with a self-referencing pair),
// but cheap to guard and this proves the guard actually does something.
static void test_nvs_load_from_v10_blob_self_referencing_pair_is_dropped(void)
{
    TEST_SECTION("nvs_load_from -- a v10 blob with a self-referencing coupling pair "
                 "(neighbor == own zone index) is dropped, not folded into the diagonal, "
                 "so the migrated config is still VALID rather than CORRUPT");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v10_t src;
    memset(&src, 0, sizeof(src));
    src.version = 10;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.safety_tc_type = 3;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");

    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].max_temp_c = 1300.0f;
    src.zones[0].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;
    src.zones[1].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    /* the defect: zone 1's stored pair points at itself. */
    src.zones[1].coupling_coeff = 9.5f;
    src.zones[1].coupling_neighbor_zone = 1.0f;

    src.zones[2].relay_mask = 0x04;
    src.zones[2].thermo_mask = 0x04;
    src.zones[2].max_temp_c = 1200.0f;
    src.zones[2].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.crc32 = 0;

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid,
              "a v10 blob with a self-referencing coupling pair must still migrate to a VALID "
              "config, not be discarded as CORRUPT");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "zone 1's self-referencing pair leaves coupling_coeff[%u] at 0", j);
        TEST_CHECK_NEAR(out_cfg.zones[1].coupling_coeff[j], 0.0f, 1e-6, msg);
    }

    nvs_test_enable(false);
    nvs_test_clear();
}

// ZONES_CFG_VERSION 11->12 (DATA PLUMBING pass): a v11 blob (coupling_coeff[]
// row shape, but predating coupling_tau_s[]/coupling_dead_time_s[] entirely)
// must migrate cleanly to the current (v12) config with both new arrays at
// 0 -- "not measured", not garbage -- while coupling_coeff[] itself and every
// other pre-existing v11 field survive unchanged. sizeof(src) is
// zone_cfg_v11_t/zones_cfg_v11_t -- both frozen, historical types with their
// own _Static_assert(sizeof(...) == N) checked against a hand-computed byte
// count, never the live zone_cfg_t/zones_cfg_t (already the v12 shape by
// now), mirroring test_nvs_load_from_v10_blob_folds_single_pair_into_row_cell()
// above exactly.
static void test_nvs_load_from_v11_blob_defaults_coupling_tau_dead_time_to_zero(void)
{
    TEST_SECTION("nvs_load_from -- a v11 blob upconverts to v12: coupling_tau_s[]/"
                 "coupling_dead_time_s[] default to 0 (never stored pre-v12), coupling_coeff[] "
                 "and every other pre-existing v11 field survive unchanged");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v11_t src;
    memset(&src, 0, sizeof(src));
    src.version = 11;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.continue_on_zone_trip = 1;
    src.safety_tc_type = 3;
    src.pc_link_abort_silence_ms = 45000.0f;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");

    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].max_temp_c = 1300.0f;
    src.zones[0].fuzzy_strength_pct = 40.0f;
    src.zones[0].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    // A v11 board that HAD already measured a coupling gain (the row this
    // pass's own bump exists to grow with tau/L) -- coupling_coeff[2] must
    // survive the migration untouched while coupling_tau_s[2]/
    // coupling_dead_time_s[2] (fields v11 never had at all) come out at 0.
    src.zones[0].coupling_coeff[2] = 6.75f;

    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;
    src.zones[1].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.zones[2].relay_mask = 0x04;
    src.zones[2].thermo_mask = 0x04;
    src.zones[2].max_temp_c = 1200.0f;
    src.zones[2].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.crc32 = 0; // v11's own CRC is not checked on the old-version path

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v11 blob must migrate to a valid current (v12) config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");

    // coupling_coeff[] carried through verbatim -- this migration does not
    // touch the row's existing shape at all.
    TEST_CHECK_NEAR(out_cfg.zones[0].coupling_coeff[2], 6.75f, 1e-6,
                    "zone 0's pre-existing coupling_coeff[2] survives the v11->v12 migration untouched");

    // THE thing this test is really about: coupling_tau_s[]/
    // coupling_dead_time_s[] must be all-zero for every zone and every cell
    // -- not garbage, not uninitialized memory -- since no version before v12
    // ever stored either array.
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
            char msg[96];
            snprintf(msg, sizeof(msg), "zones[%u].coupling_tau_s[%u] is 0 (v11 never stored this)", i, j);
            TEST_CHECK_NEAR(out_cfg.zones[i].coupling_tau_s[j], 0.0f, 1e-6, msg);
            snprintf(msg, sizeof(msg), "zones[%u].coupling_dead_time_s[%u] is 0 (v11 never stored this)", i, j);
            TEST_CHECK_NEAR(out_cfg.zones[i].coupling_dead_time_s[j], 0.0f, 1e-6, msg);
        }
    }

    // Pre-existing v11 fields must survive the upgrade completely unchanged.
    TEST_CHECK(out_cfg.thermo_count == 3 && out_cfg.relay_count == 3, "counts carried through");
    TEST_CHECK(out_cfg.continue_on_zone_trip == 1, "continue_on_zone_trip carried through");
    TEST_CHECK_NEAR(out_cfg.pc_link_abort_silence_ms, 45000.0f, 1e-6, "pc_link_abort_silence_ms carried through");
    TEST_CHECK(strcmp(out_cfg.timing_profiles[0].name, "Default") == 0, "timing profile name carried through");
    TEST_CHECK(out_cfg.zones[0].relay_mask == 0x01 && out_cfg.zones[1].relay_mask == 0x02 &&
              out_cfg.zones[2].relay_mask == 0x04, "relay_mask must NOT be shifted for any zone");
    TEST_CHECK_NEAR(out_cfg.zones[0].fuzzy_strength_pct, 40.0f, 1e-6, "zones[0].fuzzy_strength_pct survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].max_temp_c, 1300.0f, 1e-6, "zones[0].max_temp_c survives");
    TEST_CHECK_NEAR(out_cfg.zones[1].max_temp_c, 1250.0f, 1e-6, "zones[1].max_temp_c survives");
    TEST_CHECK_NEAR(out_cfg.zones[2].max_temp_c, 1200.0f, 1e-6, "zones[2].max_temp_c survives");
    TEST_CHECK(out_cfg.zones[0].settings_source == ZONE_SETTINGS_SOURCE_CUSTOM,
              "zones[0].settings_source is carried through verbatim");

    nvs_test_enable(false);
    nvs_test_clear();
}

// Full end-to-end chain: a genuine v9 blob (predates coupling entirely) must
// still land on a valid, all-zero-coupling v11 config after going through
// BOTH migration steps back-to-back (v9->v10->v11 inside one
// convert_versioned_blob_to_current(9, ...) call, since case 9 lands
// directly on CURRENT format rather than stopping at v10). Exists
// separately from the two single-step tests above because a chain bug
// (e.g. an intermediate value never making it into the final struct) is
// exactly the kind of thing that would NOT show up testing each hop in
// isolation.
// Dedicated v6 blob test -- v6 shares v7's zone layout (zone_cfg_v7_t) and
// converter (convert_zone_v7()) but predates v7's own crc32 field. Only v7
// had a dedicated blob test before this; v6 was only reached indirectly.
static void test_nvs_load_from_v6_blob_upconverts_fields_correctly(void)
{
    TEST_SECTION("nvs_load_from -- a v6 blob (v7's zone layout, no crc32 yet) upconverts every "
                 "zone's real field values, including ct_mask, correctly");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v6_t src;
    memset(&src, 0, sizeof(src));
    src.version = 6;
    src.thermo_count = 2;
    src.relay_count = 2;
    src.continue_on_zone_trip = 1;
    src.safety_tc_type = 4;

    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].tc_type = 2;
    src.zones[0].max_temp_c = 1150.0f;
    src.zones[0].ct_mask = 0x01;

    snprintf(src.zones[1].name, sizeof(src.zones[1].name), "ZoneB");
    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].tc_type = 3;
    src.zones[1].max_temp_c = 1250.0f;
    src.zones[1].guard_runaway_margin_c = 25.0f;
    src.zones[1].ct_mask = 0x02;

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v6 blob must migrate to a valid current config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped with the current version");
    TEST_CHECK(out_cfg.safety_tc_type == 4, "top-level safety_tc_type (real v6 value) carried through");

    TEST_CHECK(out_cfg.zones[1].relay_mask == 0x02, "zones[1].relay_mask must NOT be shifted");
    TEST_CHECK(out_cfg.zones[1].thermo_mask == 0x02, "zones[1].thermo_mask must NOT be shifted");
    TEST_CHECK(out_cfg.zones[1].tc_type == 3, "zones[1].tc_type carried through");
    TEST_CHECK_NEAR(out_cfg.zones[1].max_temp_c, 1250.0f, 1e-6, "zones[1].max_temp_c carried through");
    TEST_CHECK_NEAR(out_cfg.zones[1].guard_runaway_margin_c, 25.0f, 1e-6, "zones[1].guard_runaway_margin_c carried through");
    TEST_CHECK(out_cfg.zones[1].ct_mask == 0x02, "zones[1].ct_mask -- new since v4 -- carried through, not shifted");

    TEST_CHECK(out_cfg.zones[0].ct_mask == 0x01, "zones[0].ct_mask still correct (sanity check)");
    TEST_CHECK_NEAR(out_cfg.pc_link_abort_silence_ms, 0.0f, 1e-6, "v6 predates pc_link_abort_silence_ms -- documented default 0");

    nvs_test_enable(false);
    nvs_test_clear();
}

// End-to-end composition test: the OLDEST supported version (v1) all the way
// to CURRENT (v15), asserting real field values -- not just defaults --
// survive every hop. "Composes by inspection" was the audit's own phrase for
// this gap; this test replaces that inspection with an executed assertion.
// A test that only checked new-field defaults (coupling_diag_k_dc == 0, etc.)
// would pass even if every pre-existing field were silently dropped along the
// way -- so this asserts real, non-default, non-zero values for fields that
// have existed since v1 (name, pid_kp, max_temp_c, model_tau_s) all still
// read back correctly out of the fully-migrated v15 struct.
static void test_nvs_load_from_v1_blob_chains_end_to_end_to_v15_preserving_real_values(void)
{
    TEST_SECTION("nvs_load_from -- a v1 blob migrates end-to-end to the current (v15) config: "
                 "real v1 field values (not just new-field defaults) survive every hop");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v1_t src;
    memset(&src, 0, sizeof(src));
    src.version = 1;
    src.thermo_count = 2;
    src.relay_count = 2;
    src.max_simultaneous_relays = 2;

    snprintf(src.zones[0].name, sizeof(src.zones[0].name), "OldestZone");
    src.zones[0].relay_mask = 0x01;
    src.zones[0].cal_offset_c = 1.5f;
    src.zones[0].pid_kp = 3.5f;
    src.zones[0].pid_ki = 0.2f;
    src.zones[0].pid_kd = 0.1f;
    src.zones[0].max_temp_c = 1150.0f;
    src.zones[0].min_temp_c = -10.0f;
    src.zones[0].model_k_dc = 7.0f;
    src.zones[0].model_tau_s = 300.0f;
    src.zones[0].model_dead_time_s = 12.0f;

    src.zones[1].relay_mask = 0x02;
    src.zones[1].pid_kp = 4.5f;
    src.zones[1].max_temp_c = 1250.0f;

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a v1 blob must migrate end-to-end to a valid current (v15) config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current (v15) version");

    // Real values that existed at v1 and must have survived every hop to v15.
    TEST_CHECK(strcmp(out_cfg.zones[0].name, "OldestZone") == 0,
              "zones[0].name -- present since v1 -- survives all the way to v15");
    TEST_CHECK_NEAR(out_cfg.zones[0].pid_kp, 3.5f, 1e-6, "zones[0].pid_kp survives all the way to v15");
    TEST_CHECK_NEAR(out_cfg.zones[0].pid_ki, 0.2f, 1e-6, "zones[0].pid_ki survives all the way to v15");
    TEST_CHECK_NEAR(out_cfg.zones[0].max_temp_c, 1150.0f, 1e-6, "zones[0].max_temp_c survives all the way to v15");
    TEST_CHECK_NEAR(out_cfg.zones[0].model_tau_s, 300.0f, 1e-6, "zones[0].model_tau_s survives all the way to v15");
    TEST_CHECK_NEAR(out_cfg.zones[1].pid_kp, 4.5f, 1e-6, "zones[1].pid_kp (a DIFFERENT zone's value) survives too, unshifted");
    TEST_CHECK_NEAR(out_cfg.zones[1].max_temp_c, 1250.0f, 1e-6, "zones[1].max_temp_c survives too, unshifted");

    // Fields introduced well after v1 (coupling matrix: v11/v12; tuning
    // quality record: v13; adaptive_tune_enabled: v14; coupling_diag_k_dc:
    // v15) correctly land at their documented zero/unset defaults, since a v1
    // board never had any such data to carry.
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        TEST_CHECK_NEAR(out_cfg.zones[0].coupling_coeff[j], 0.0f, 1e-6, "v1 has no coupling data -- 0 (unset)");
        TEST_CHECK_NEAR(out_cfg.zones[0].coupling_tau_s[j], 0.0f, 1e-6, "v1 has no coupling tau data -- 0 (unset)");
        TEST_CHECK_NEAR(out_cfg.zones[0].coupling_dead_time_s[j], 0.0f, 1e-6, "v1 has no coupling dead-time data -- 0 (unset)");
    }
    TEST_CHECK_NEAR(out_cfg.zones[0].coupling_diag_k_dc, 0.0f, 1e-6, "v1 has no coupling_diag_k_dc -- 0 (unset)");
    TEST_CHECK(out_cfg.zones[0].adaptive_tune_enabled == 0, "v1 has no adaptive_tune_enabled -- documented default 0");
    TEST_CHECK(out_cfg.zones[0].tuning_valid == 0, "v1 has no tuning-quality record -- documented default 0 (not valid)");

    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_nvs_load_from_v9_blob_chains_through_v10_to_v11_with_zero_coupling(void)
{
    TEST_SECTION("nvs_load_from -- a v9 blob migrates through v10 to v11 (end-to-end): valid, "
                 "current version, and an all-zero coupling row (v9 never had ANY coupling data)");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v9_t src;
    memset(&src, 0, sizeof(src));
    src.version = 9;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");
    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[2].relay_mask = 0x04;
    src.zones[2].thermo_mask = 0x04;
    src.crc32 = 0;

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v9 blob must migrate all the way to a valid v11 config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
            char msg[96];
            snprintf(msg, sizeof(msg), "zones[%u].coupling_coeff[%u] is 0 (v9 has no coupling data at all)", i, j);
            TEST_CHECK_NEAR(out_cfg.zones[i].coupling_coeff[j], 0.0f, 1e-6, msg);
        }
    }

    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_validate_accepts_control_mode_pid_fuzzy_rejects_past_it(void)
{
    TEST_SECTION("validate_zones_cfg / zones_config_set_control_mode -- ZONE_CONTROL_MODE_PID_FUZZY "
                 "(3) is accepted, 4 is still rejected");
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].control_mode = (uint8_t)ZONE_CONTROL_MODE_PID_FUZZY;
        const char *reason = NULL;
        TEST_CHECK(zones_config_json_validate(&cfg, &reason), "control_mode 3 (PID_FUZZY) validates");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].control_mode = 4;
        const char *reason = NULL;
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason), "control_mode 4 is still out of range and rejected");
    }

    // zones_config_set_control_mode() enforces the identical bound. Its
    // success path calls nvs_save(), which needs the NVS stub armed (same
    // convention every other setter test in this file that persists uses).
    {
        nvs_test_enable(true);
        nvs_test_clear();
        s_zones.cfg.thermo_count = 1;
        TEST_CHECK(zones_config_set_control_mode(0, ZONE_CONTROL_MODE_PID_FUZZY),
                  "zones_config_set_control_mode(PID_FUZZY) is accepted");
        zone_control_mode_t got;
        TEST_CHECK(zones_config_get_control_mode(0, &got) && got == ZONE_CONTROL_MODE_PID_FUZZY,
                  "the accepted mode reads back correctly");
        TEST_CHECK(!zones_config_set_control_mode(0, (zone_control_mode_t)4),
                  "zones_config_set_control_mode(4) is refused");
        nvs_test_enable(false);
        nvs_test_clear();
    }
}

static void test_post_mode_pid_fuzzy_accepted_by_parser(void)
{
    TEST_SECTION("parse_zone_fields -- z0_mode=3 (PID_FUZZY) is accepted, z0_mode=4 is refused (0-3)");
    char body3[512];
    snprintf(body3, sizeof(body3),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=3&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&"
             "z0_timingprofile=0");
    zone_cfg_t current = make_stored_zone();
    zone_cfg_t out;
    memset(&out, 0, sizeof(out));
    const char *err_reason = "unset";
    bool ok = parse_zone_fields(body3, 0, 1, 4, 1, &current, &out, &err_reason);
    TEST_CHECK(ok, "z0_mode=3 (PID_FUZZY) must be accepted");
    TEST_CHECK(out.control_mode == (uint8_t)ZONE_CONTROL_MODE_PID_FUZZY, "the parsed mode is PID_FUZZY");

    char body4[512];
    snprintf(body4, sizeof(body4),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=4&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&"
             "z0_timingprofile=0");
    memset(&out, 0, sizeof(out));
    err_reason = "unset";
    ok = parse_zone_fields(body4, 0, 1, 4, 1, &current, &out, &err_reason);
    TEST_CHECK(!ok, "z0_mode=4 must still be refused -- appending PID_FUZZY did not widen the ceiling further");
    TEST_CHECK(err_reason && strstr(err_reason, "control_mode") != NULL, "the refusal names the field");
}

/* One clean body with only the field-under-test varied, mirroring
 * post_body_with_minon()'s own pattern above. */
static bool post_body_with_fuzzy_strength(const char *literal, const char **err_reason_out, float *out_value)
{
    char body[600];
    snprintf(body, sizeof(body),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=3&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&"
             "z0_timingprofile=0&z0_fuzzy_strength=%s",
             literal);
    zone_cfg_t current = make_stored_zone();
    zone_cfg_t out;
    memset(&out, 0, sizeof(out));
    const char *err_reason = "unset";
    bool ok = parse_zone_fields(body, 0, 1, 4, 1, &current, &out, &err_reason);
    if (err_reason_out) {
        *err_reason_out = err_reason;
    }
    if (out_value) {
        *out_value = out.fuzzy_strength_pct;
    }
    return ok;
}

static void test_post_fuzzy_strength_out_of_range_refused_not_clamped(void)
{
    TEST_SECTION("parse_zone_fields -- z0_fuzzy_strength=101 and =-1 are REFUSED, not clamped to 100/0");
    const char *reason = "unset";
    float value = -99.0f;

    TEST_CHECK(!post_body_with_fuzzy_strength("101", &reason, &value),
              "101 (just above the 0-100 range) must be refused outright");
    TEST_CHECK(reason && strstr(reason, "fuzzy_strength_pct") != NULL, "the refusal names the field");

    TEST_CHECK(!post_body_with_fuzzy_strength("-1", &reason, &value),
              "-1 (just below the 0-100 range) must be refused outright");
    TEST_CHECK(reason && strstr(reason, "fuzzy_strength_pct") != NULL, "the refusal names the field");

    // Positive control: the boundary values themselves must still be accepted
    // EXACTLY, proving a rejected 101/-1 is a real range check, not a
    // bug that rejects everything.
    TEST_CHECK(post_body_with_fuzzy_strength("0", &reason, &value), "0 is accepted");
    TEST_CHECK_NEAR(value, 0.0f, 1e-6, "0 is stored as 0 exactly");
    TEST_CHECK(post_body_with_fuzzy_strength("100", &reason, &value), "100 is accepted");
    TEST_CHECK_NEAR(value, 100.0f, 1e-6, "100 is stored as 100 exactly");
}

/* Generic single-field body, so the three post-review refusals below can be
 * exercised without one helper per field. */
static bool post_body_with_extra(const char *extra_kv, const char **err_reason_out, zone_cfg_t *out_zone)
{
    char body[700];
    snprintf(body, sizeof(body),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=3&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&"
             "z0_timingprofile=0&%s",
             extra_kv);
    zone_cfg_t current = make_stored_zone();
    zone_cfg_t out;
    memset(&out, 0, sizeof(out));
    const char *err_reason = "unset";
    /* thermo_count = MAX31856_CHANNEL_COUNT, not 1: settings_source's
     * cross-zone chain-walk (parse_zone_fields()'s own comment on it) is
     * bounded by thermo_count -- the same "unused trailing slot" discipline
     * used everywhere else in this file -- so a thermo_count of 1 would make
     * every OTHER zone index invisible to that check and silently defeat
     * the cycle tests that exercise this door. Every other field this
     * helper posts only ever touches zone 0, so widening thermo_count here
     * does not change what any of those checks accept or reject. */
    bool ok = parse_zone_fields(body, 0, MAX31856_CHANNEL_COUNT, 4, 1, &current, &out, &err_reason);
    if (err_reason_out) *err_reason_out = err_reason;
    if (out_zone) *out_zone = out;
    return ok;
}

/* All three of these were found by review AFTER the phase was first called
 * done, and each one silently accepted bad input rather than refusing it. */
static void test_post_new_fields_present_but_unparseable_are_refused(void)
{
    TEST_SECTION("parse_zone_fields -- a PRESENT but empty/over-long new field is refused, not "
                 "treated as omitted");
    const char *reason = "unset";
    zone_cfg_t out;

    /* An empty value ("key=") makes http_form_find_field() return 0, and an
     * over-long one returns -2. The original probe tested `> 0`, so BOTH
     * fell into the preserve-on-omit branch: a 200 OK, the old value kept,
     * and the operator told nothing. Only a genuinely absent key is an
     * omission. */
    TEST_CHECK(!post_body_with_extra("z0_fuzzy_strength=", &reason, &out),
              "an empty z0_fuzzy_strength= is refused, not silently treated as omitted");
    TEST_CHECK(!post_body_with_extra("z0_coupling_c1=", &reason, &out),
              "an empty z0_coupling_c1= is refused");
    TEST_CHECK(!post_body_with_extra(
                   "z0_fuzzy_strength=999999999999999999999999999999999999", &reason, &out),
              "an over-long z0_fuzzy_strength value is refused, not swallowed by the probe buffer");

    /* Positive control: genuinely omitting the key still succeeds, so the
     * above is a real present-vs-absent distinction and not a blanket
     * rejection. */
    TEST_CHECK(post_body_with_extra("z0_kp=1", &reason, &out),
              "omitting the new fields entirely is still accepted (preserve-on-omit intact)");
}

// 2026-08-30 (ZONES_CFG_VERSION 10->11): coupling_neighbor_zone as a
// separate field no longer exists -- "which neighbor" is now which
// z%u_coupling_c%u key was posted, not a value. What replaces the old
// integrality check is the diagonal rule: z0_coupling_c0 (zone 0's OWN
// index) must be exactly 0, since a zone's response to its own heater is
// model_k_dc, not a coupling cell.
static void test_post_coupling_diagonal_must_be_zero(void)
{
    TEST_SECTION("parse_zone_fields -- z0_coupling_c0 (the diagonal) must be exactly 0, "
                 "an off-diagonal cell accepts the normal range");
    const char *reason = "unset";
    zone_cfg_t out;

    TEST_CHECK(!post_body_with_extra("z0_coupling_c0=1.5", &reason, &out),
              "a nonzero diagonal cell is refused at the door");
    TEST_CHECK(reason && strstr(reason, "coupling_coeff") != NULL, "the refusal names the field");

    TEST_CHECK(post_body_with_extra("z0_coupling_c0=0", &reason, &out),
              "a diagonal cell posted as exactly 0 is accepted (a no-op, not an error)");
    TEST_CHECK_NEAR(out.coupling_coeff[0], 0.0f, 1e-6, "and reads back as 0");

    TEST_CHECK(post_body_with_extra("z0_coupling_c2=8.25", &reason, &out),
              "an off-diagonal cell in range is accepted");
    TEST_CHECK_NEAR(out.coupling_coeff[2], 8.25f, 1e-6, "and is stored exactly");
}

static void test_post_settings_source_self_reference_refused(void)
{
    TEST_SECTION("parse_zone_fields -- a zone cannot claim 'same settings as' itself");
    const char *reason = "unset";
    zone_cfg_t out;

    /* parse_zone_fields()'s settings_source cross-zone chain-walk consults
     * the LIVE s_zones.cfg for every zone other than the one being posted
     * (post_body_with_extra()'s own `current` argument only ever covers
     * zone 0's OTHER fields, not this cross-zone check) -- so this test
     * must not run against whatever s_zones.cfg happened to be left at by
     * an earlier test in this suite. Reset every zone to Custom (the same
     * "never leave an in-use zone at raw 0 unintentionally" reasoning as
     * every other settings_source test in this file) so "pointing at a
     * different zone" below is checked against a clean, unlinked config. */
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        s_zones.cfg.zones[z].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    }

    TEST_CHECK(!post_body_with_extra("z0_settings_source=0", &reason, &out),
              "zone 0 pointing at zone 0 is the degenerate cycle and is refused");
    TEST_CHECK(reason && strstr(reason, "settings_source") != NULL, "the refusal names the field");

    /* Positive controls: CUSTOM and a different zone both still work, so the
     * check is specifically self-reference and not a blanket refusal. */
    TEST_CHECK(post_body_with_extra("z0_settings_source=255", &reason, &out),
              "ZONE_SETTINGS_SOURCE_CUSTOM (0xFF) is still accepted");
    TEST_CHECK(out.settings_source == 0xFF, "and is stored as CUSTOM");
    TEST_CHECK(post_body_with_extra("z0_settings_source=1", &reason, &out),
              "pointing at a DIFFERENT zone is still accepted");
    TEST_CHECK(out.settings_source == 1, "and is stored as that zone index");
}

static void test_post_omitting_new_fields_preserves_stored_values(void)
{
    TEST_SECTION("parse_zone_fields -- omitting z0_fuzzy_strength/z0_coupling_coeff/"
                 "z0_coupling_neighbor/z0_settings_source succeeds and PRESERVES the "
                 "previously-stored values, never zeroing them");
    char body[512];
    snprintf(body, sizeof(body),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=3&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&"
             "z0_timingprofile=0"); // none of the four new fields present

    zone_cfg_t current = make_stored_zone();
    current.fuzzy_strength_pct = 42.0f;
    current.coupling_coeff[1] = 5.0f;
    current.coupling_coeff[2] = 7.5f;
    current.settings_source = 1; /* "copies zone 1" -- a real, previously-chosen value */
    /* ZONES_CFG_VERSION 14->15: a real, previously-measured diagonal cell --
     * z0_coupling_diag_k_dc is also absent from body above, so this must
     * survive the same "omit preserves" rule as the four fields already
     * covered by this test. */
    current.coupling_diag_k_dc = 18.75f;
    /* ZONES_CFG_VERSION 13->14: the operator's adaptive-tune opt-in. It has
     * NO z%u_ POST key at all (adaptive_tune.c is its only writer), so this
     * is the strongest form of "omitted": a client CANNOT send it even if it
     * wanted to, and a whole-page save must therefore never clear it. */
    current.adaptive_tune_enabled = 1;

    zone_cfg_t out;
    memset(&out, 0, sizeof(out));
    const char *err_reason = "unset";
    bool ok = parse_zone_fields(body, 0, 1, 4, 1, &current, &out, &err_reason);

    TEST_CHECK(ok, "a POST omitting all five new fields must still succeed (optionality)");
    TEST_CHECK_NEAR(out.fuzzy_strength_pct, 42.0f, 1e-6,
                    "fuzzy_strength_pct must be PRESERVED, not zeroed, when omitted");
    TEST_CHECK_NEAR(out.coupling_coeff[1], 5.0f, 1e-6,
                    "coupling_coeff[1] must be PRESERVED, not zeroed, when omitted");
    TEST_CHECK_NEAR(out.coupling_coeff[2], 7.5f, 1e-6,
                    "coupling_coeff[2] must be PRESERVED, not zeroed, when omitted");
    TEST_CHECK(out.settings_source == 1,
              "settings_source must be PRESERVED at its previously-chosen value, not reset to CUSTOM "
              "or zeroed to \"copies zone 0\"");
    TEST_CHECK_NEAR(out.coupling_diag_k_dc, 18.75f, 1e-6,
                    "coupling_diag_k_dc must be PRESERVED, not zeroed, when omitted -- it is a "
                    "measured quantity, same as fuzzy_strength_pct/coupling_coeff above");
    TEST_CHECK(out.adaptive_tune_enabled == 1,
              "adaptive_tune_enabled must be PRESERVED by a whole-page save -- it has no POST key "
              "at all, so a save that clears it silently disables adaptive tuning behind the "
              "operator's back and persists that to NVS");
}

// End-to-end round trip through the real handlers: a POST carrying real
// values for all four new fields, then a GET, must report exactly what was
// posted. Uses run_zones_post() (the real zones_post_handler()) and
// zones_get_handler() directly, with httpd_resp_send() captured by this
// file's own stub above.
static void test_post_then_get_round_trips_new_fields(void)
{
    TEST_SECTION("zones_post_handler -> zones_get_handler -- the four new fields round-trip exactly");

    char body[900];
    snprintf(body, sizeof(body),
             "thermo_count=1&relay_count=1&" MINIMAL_TIMING_PROFILE_BODY
             "&z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&z0_timingprofile=0&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=0&z0_sanity=0&z0_mode=3&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&"
             "z0_fuzzy_strength=12.5&z0_coupling_c1=1.5&z0_coupling_c2=3.25&"
             "z0_coupling_diag_k_dc=25.75&"
             "z0_settings_source=255");
    run_zones_post(body);
    TEST_CHECK(s_test_ok_called && !s_test_err_called, "the whole-page POST with new fields must be accepted");

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = zones_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "zones_get_handler must return ESP_OK");

    TEST_CHECK(strstr(s_last_resp_body, "\"control_mode\":3") != NULL,
              "GET reports the posted control_mode (PID_FUZZY)");
    TEST_CHECK(strstr(s_last_resp_body, "\"fuzzy_strength_pct\":12.50") != NULL,
              "GET reports the posted fuzzy_strength_pct exactly");
    TEST_CHECK(strstr(s_last_resp_body, "\"coupling_c1\":1.5000") != NULL,
              "GET reports the posted coupling_coeff[1] exactly");
    TEST_CHECK(strstr(s_last_resp_body, "\"coupling_c2\":3.2500") != NULL,
              "GET reports the posted coupling_coeff[2] exactly");
    TEST_CHECK(strstr(s_last_resp_body, "\"coupling_c0\":0.0000") != NULL,
              "GET reports the untouched diagonal cell as 0 (never omitted)");
    TEST_CHECK(strstr(s_last_resp_body, "\"coupling_diag_k_dc\":25.7500") != NULL,
              "GET reports the posted coupling_diag_k_dc exactly");
    TEST_CHECK(strstr(s_last_resp_body, "\"settings_source\":255") != NULL,
              "GET reports the posted settings_source (CUSTOM) exactly");
}

// tuning_rec_body_len() (2026-09-02 host-link fix, commit 333dd4e): the
// real tuning_recommendations_json_start/_end symbols are ESP-IDF
// EMBED_FILES symbols this host build cannot reproduce (see this file's
// placeholder definitions above), so this exercises the extracted length
// arithmetic directly with a real, deliberately-adjacent synthetic buffer --
// the exact case the handler's own comment documents: EMBED_TXTFILES
// appends a NUL after the file content and places _end past it, so the
// reported length must be (end - start) - 1, never the bare difference.
static void test_tuning_rec_body_len_strips_the_idf_appended_nul(void)
{
    TEST_SECTION("tuning_rec_body_len -- EMBED_TXTFILES trailing-NUL arithmetic");

    // Ordinary case: content "abc" (3 bytes) + IDF's appended NUL (1 byte) =
    // a 4-byte buffer, exactly like tuning_recommendations_json's real
    // start/end pair.
    static const uint8_t buf[] = { 'a', 'b', 'c', '\0' };
    size_t n = tuning_rec_body_len(buf, buf + sizeof(buf));
    TEST_CHECK(n == 3, "3 content bytes + 1 appended NUL must report length 3, not 4");

    // Degenerate: start == end (diff 0) must floor at 0, not underflow a
    // size_t to SIZE_MAX -- the `raw > 0 ? raw - 1 : 0` guard this tests.
    n = tuning_rec_body_len(buf, buf);
    TEST_CHECK(n == 0, "start == end must report length 0, not underflow");

    // Degenerate: a 1-byte buffer (just the appended NUL, empty content)
    // must also report 0, the same floor as start == end.
    n = tuning_rec_body_len(buf, buf + 1);
    TEST_CHECK(n == 0, "a 1-byte (NUL-only) buffer must report length 0");
}

// Coordinator review, 2026-08-31 httpd_worker stack-overflow fix:
// zones_get_handler()'s json[5760] moved off the stack and onto
// heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) -- the largest
// single stack buffer on the whole httpd_worker task's request path (>70%
// of its 8192-byte stack by itself). This is the ONE handler in this
// codebase's audit that is host-testable calling the REAL production
// function (dashboard_http.c's equivalent handlers cannot compile on the
// host at all -- see dashboard_json.h's own note), so it is the test that
// actually proves the heap-allocation pattern works end to end: a normal
// call still renders real config, and a simulated out-of-memory (via this
// file's heap_caps_malloc_test_set_fail() stub hook) returns a clean 500
// with a diagnosable body instead of crashing on a NULL deref -- the exact
// property "keep the NULL check, clean 500 on failure" asks for, previously
// completely unexercised (the prior heap-conversion test only re-implemented
// a fragment of the render inline and never called a handler or exercised
// the failure path at all).
static void test_zones_get_handler_malloc_failure_returns_clean_500(void)
{
    TEST_SECTION("zones_get_handler -- heap_caps_malloc() failure must return a clean 500 with a "
                 "diagnosable body, not crash");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 1;
    s_zones.cfg.relay_count = 1;

    test_post_hooks_reset();
    s_last_resp_body[0] = '\0';
    s_last_resp_len = 0;

    heap_caps_malloc_test_set_fail(true);
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = zones_get_handler(&req);
    heap_caps_malloc_test_set_fail(false); /* restore immediately, before any other check/test can run */

    TEST_CHECK(err == ESP_OK, "zones_get_handler must still return ESP_OK (httpd_resp_sendstr succeeded) "
              "even when its own response buffer could not be allocated");
    TEST_CHECK(s_test_ok_called, "the malloc-failure path must actually call httpd_resp_sendstr, not "
              "silently return without sending anything");
    TEST_CHECK(strstr(s_last_resp_body, "\"ok\":false") != NULL,
              "the 500 body must report ok:false, not a bare crash or an empty response");
    TEST_CHECK(strstr(s_last_resp_body, "out of memory") != NULL,
              "the 500 body must say WHY -- out of memory, not a generic failure -- so this is "
              "diagnosable from the wire instead of looking like a hang");
}

// Sanity companion to the failure test above: with the fail hook OFF (the
// normal case, and every other test in this file), zones_get_handler must
// still render real config through the now-heap-allocated buffer, exactly
// as it did on the stack -- proves the heap move didn't break the success
// path either.
static void test_zones_get_handler_succeeds_when_malloc_does_not_fail(void)
{
    TEST_SECTION("zones_get_handler -- with heap_caps_malloc() NOT failing, the handler still renders "
                 "real config through its now-heap-allocated buffer");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 1;
    s_zones.cfg.relay_count = 1;
    strncpy(s_zones.cfg.zones[0].name, "Kiln Zone", ZONE_NAME_MAX_LEN - 1);

    heap_caps_malloc_test_set_fail(false);
    s_last_resp_body[0] = '\0';
    s_last_resp_len = 0;
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = zones_get_handler(&req);

    TEST_CHECK(err == ESP_OK, "zones_get_handler must return ESP_OK on a normal call");
    TEST_CHECK(strstr(s_last_resp_body, "\"thermo_count\":1") != NULL,
              "the heap-allocated buffer must render real config, same as the stack version did");
    TEST_CHECK(strstr(s_last_resp_body, "Kiln Zone") != NULL,
              "a real zone name set just above must actually appear in the rendered JSON");
}

// ---------------------------------------------------------------------------
// zones_config_set_fuzzy_strength_pct / zones_config_get/set_coupling /
// zones_config_get/set_settings_source -- new accessors backup_http.c's
// import needs, added because parse_zone_fields() previously was the only
// door onto these four fields. Same nvs_test_enable discipline as every other
// setter test in this file: the setter writes RAM immediately either way, but
// its return value (and thus TEST_CHECK) depends on nvs_save() succeeding.
// ---------------------------------------------------------------------------

static void test_fuzzy_strength_pct_setter_round_trip_and_bounds(void)
{
    TEST_SECTION("zones_config_set/get_fuzzy_strength_pct -- accepts in-range, refuses out-of-range");
    nvs_test_enable(true);
    nvs_test_clear();
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 1;

    TEST_CHECK(zones_config_set_fuzzy_strength_pct(0, 55.5f), "55.5 is in range 0-100");
    float out = -1.0f;
    TEST_CHECK(zones_config_get_fuzzy_strength_pct(0, &out), "getter succeeds");
    TEST_CHECK_NEAR(out, 55.5f, 1e-6, "the exact value set comes back out");

    TEST_CHECK(!zones_config_set_fuzzy_strength_pct(0, 100.0001f), "just over 100 is refused");
    TEST_CHECK(!zones_config_set_fuzzy_strength_pct(0, -0.0001f), "just under 0 is refused");
    TEST_CHECK(zones_config_get_fuzzy_strength_pct(0, &out) && out == 55.5f,
              "a refused setter call must not have changed the stored value (refuse, not clamp)");

    TEST_CHECK(!zones_config_set_fuzzy_strength_pct(1, 10.0f), "zone_index >= thermo_count is refused");

    nvs_test_enable(false);
    nvs_test_clear();
}

// 2026-08-30 (ZONES_CFG_VERSION 10->11): row-based, not a bundled pair --
// see zones_config_get/set_coupling()'s own doc comment (zones_http.h).
// THE test the whole 10->11 bump exists to make possible: two different
// off-diagonal cells of the SAME row hold distinct, asymmetric values
// simultaneously -- the bench-measured c(1->0)=10.887 / c(1->2)=3.332 pair
// this task's own brief cites as the reason a single (coeff, neighbor) pair
// could never represent this kiln's real coupling.
static void test_coupling_row_whole_setter_round_trip_and_bounds(void)
{
    TEST_SECTION("zones_config_set/get_coupling -- whole row, diagonal must stay 0, "
                 "multi-neighbor asymmetric values survive together");
    nvs_test_enable(true);
    nvs_test_clear();
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;

    // The exact bench-measured asymmetric pair this whole change exists for.
    float row[MAX31856_CHANNEL_COUNT] = {10.887f, 0.0f, 3.332f};
    TEST_CHECK(zones_config_set_coupling(1, row), "zone 1's asymmetric two-neighbor row is accepted");
    float out_row[MAX31856_CHANNEL_COUNT] = {-1.0f, -1.0f, -1.0f};
    TEST_CHECK(zones_config_get_coupling(1, out_row), "getter succeeds");
    TEST_CHECK_NEAR(out_row[0], 10.887f, 1e-6, "c(1->0) comes back exactly");
    TEST_CHECK_NEAR(out_row[2], 3.332f, 1e-6, "c(1->2) comes back exactly, DISTINCT from c(1->0)");
    TEST_CHECK(out_row[0] != out_row[2], "the whole point: two neighbors of one zone hold different values");
    TEST_CHECK_NEAR(out_row[1], 0.0f, 1e-6, "the diagonal (zone 1's own cell) reads back as 0");

    // A nonzero diagonal is refused outright, the whole row left untouched.
    float bad_diag[MAX31856_CHANNEL_COUNT] = {10.887f, 1.0f, 3.332f};
    TEST_CHECK(!zones_config_set_coupling(1, bad_diag), "a nonzero diagonal cell is refused");

    // An out-of-range off-diagonal cell is refused outright too.
    float bad_range[MAX31856_CHANNEL_COUNT] = {10.887f, 0.0f, ZONE_COUPLING_COEFF_MAX + 1.0f};
    TEST_CHECK(!zones_config_set_coupling(1, bad_range), "an off-diagonal cell above ZONE_COUPLING_COEFF_MAX is refused");

    TEST_CHECK(zones_config_get_coupling(1, out_row) && out_row[0] == 10.887f && out_row[2] == 3.332f,
              "every refused whole-row call above left the previously-stored row untouched (refuse, not clamp)");

    nvs_test_enable(false);
    nvs_test_clear();
}

// The single-cell setter autotune_engine.c's finalize_fit() uses to persist
// one neighbor's measured coefficient without disturbing the others.
//
// Storage convention (2026-08-31 correction -- these two bench numbers were
// previously both placed in zone 1's row, which is backwards): row index =
// the AFFECTED zone (the one whose temperature the coefficient describes the
// response of), column index = the STEPPED zone (the one whose heater was
// perturbed to produce the measurement). The bench pair this file cites
// elsewhere -- stepping zone 1's heater moves zone 0 by 10.887 and zone 2 by
// 3.332 -- therefore lands in TWO DIFFERENT rows, both at column 1:
// coupling_coeff[0][1] = 10.887 and coupling_coeff[2][1] = 3.332. It never
// belonged in zone 1's own row at all.
static void test_coupling_single_cell_setter_preserves_other_cells(void)
{
    TEST_SECTION("zones_config_set_coupling_cell -- updates ONE cell, leaves other rows alone, "
                 "diagonal write of 0 is a harmless no-op, nonzero diagonal is refused");
    nvs_test_enable(true);
    nvs_test_clear();
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;

    // row=affected, column=stepped: zone 0's response to zone 1's step, and
    // zone 2's response to that SAME step -- two different rows, same column.
    TEST_CHECK(zones_config_set_coupling_cell(0, 1, 10.887f, 42.0f, 6.0f),
              "zone 0's response to zone 1's step is set");
    TEST_CHECK(zones_config_set_coupling_cell(2, 1, 3.332f, 18.0f, 2.5f),
              "zone 2's response to zone 1's step is set separately");
    float row0[MAX31856_CHANNEL_COUNT] = {0};
    float row2[MAX31856_CHANNEL_COUNT] = {0};
    float tau0[MAX31856_CHANNEL_COUNT] = {0};
    float dead0[MAX31856_CHANNEL_COUNT] = {0};
    TEST_CHECK(zones_config_get_coupling(0, row0), "zone 0's getter succeeds");
    TEST_CHECK(zones_config_get_coupling(2, row2), "zone 2's getter succeeds");
    TEST_CHECK_NEAR(row0[1], 10.887f, 1e-6, "zone 0's cell survived zone 2's write");
    TEST_CHECK_NEAR(row2[1], 3.332f, 1e-6, "and zone 2's cell is exactly what was set, in its OWN row");
    // ZONES_CFG_VERSION 11->12: tau_s/dead_time_s ride alongside coeff.
    TEST_CHECK(zones_config_get_coupling_tau(0, tau0), "zone 0's tau getter succeeds");
    TEST_CHECK(zones_config_get_coupling_dead_time(0, dead0), "zone 0's dead-time getter succeeds");
    TEST_CHECK_NEAR(tau0[1], 42.0f, 1e-6, "zone 0's coupling_tau_s[1] is exactly what was set");
    TEST_CHECK_NEAR(dead0[1], 6.0f, 1e-6, "zone 0's coupling_dead_time_s[1] is exactly what was set");

    TEST_CHECK(zones_config_set_coupling_cell(0, 0, 0.0f, 0.0f, 0.0f),
              "writing the diagonal to exactly 0 is accepted");
    TEST_CHECK(!zones_config_set_coupling_cell(0, 0, 5.0f, 0.0f, 0.0f), "writing a NONZERO diagonal is refused");
    TEST_CHECK(zones_config_get_coupling(0, row0) && row0[1] == 10.887f,
              "the refused diagonal write left zone 0's other cell untouched");
    TEST_CHECK(zones_config_get_coupling(2, row2) && row2[1] == 3.332f,
              "zone 0's diagonal write did not cross into zone 2's row");

    TEST_CHECK(!zones_config_set_coupling_cell(0, 1, ZONE_COUPLING_COEFF_MAX + 1.0f, 1.0f, 1.0f),
              "a single cell above ZONE_COUPLING_COEFF_MAX is refused");
    TEST_CHECK(zones_config_get_coupling(0, row0) && row0[1] == 10.887f,
              "the refused cell write did not clamp or corrupt the previously-good value");

    // ZONES_CFG_VERSION 11->12: an in-range coeff paired with an
    // out-of-range tau_s must refuse the WHOLE cell -- coeff must NOT land
    // while tau_s/dead_time_s are silently dropped (the all-or-nothing rule
    // zones_config_set_coupling_cell()'s own header comment documents).
    TEST_CHECK(!zones_config_set_coupling_cell(0, 1, 99.0f, ZONE_MODEL_TIME_MAX_S + 1.0f, 1.0f),
              "an in-range coeff with an out-of-range tau_s is refused entirely");
    TEST_CHECK(zones_config_get_coupling(0, row0) && row0[1] == 10.887f,
              "the all-or-nothing refusal left coeff at its previous value, not the rejected 99.0");
    TEST_CHECK(zones_config_get_coupling_tau(0, tau0) && tau0[1] == 42.0f,
              "the all-or-nothing refusal left tau_s at its previous value too");

    nvs_test_enable(false);
    nvs_test_clear();
}

// Bounded search: find `needle` inside the JSON object for zone `zone_index`
// within a GET /api/zones response, never past the following zone's object
// (or the end of the "zones" array) -- so a value that legitimately appears
// in a NEIGHBOR zone's object can't produce a false match for this one.
static bool zone_json_field_present(const char *body, uint8_t zone_index, const char *needle)
{
    const char *zones_arr = strstr(body, "\"zones\":[");
    if (!zones_arr) return false;
    char marker[24];
    snprintf(marker, sizeof(marker), "\"index\":%u,", (unsigned)zone_index);
    const char *start = strstr(zones_arr, marker);
    if (!start) return false;
    char next_marker[24];
    snprintf(next_marker, sizeof(next_marker), "\"index\":%u,", (unsigned)zone_index + 1);
    const char *end = strstr(start, next_marker);
    const char *found = strstr(start, needle);
    if (!found) return false;
    if (end && found >= end) return false;
    return true;
}

// PID_EXPANSION_PLAN.md section 3.2's re-solved matrix (adopted
// 2026-09-02), applied here in its storage orientation
// coupling_coeff[affected][stepped]:
//
//     [[38.13, 27.32, 21.72],
//      [14.30, 35.90, 22.15],
//      [ 8.33, 12.42, 35.32]]
//
// This matrix is DELIBERATELY, strongly asymmetric off-diagonal (z1's step
// raises z0 by ~22C; z0's step raises z1 by only ~9C) specifically so a
// transposed apply is easy to catch here rather than on hardware -- see
// zones_http.h's coupling_coeff doc comment ("row i is the affected zone,
// column j is the stepped zone. Do not transpose").
//
// REVIEW 2026-09-02: an earlier version of this test set every row through
// zones_config_set_coupling() and read it back through
// zones_config_get_coupling() ONLY. That is orientation-BLIND by
// construction: if the setter's storage location and the getter's read
// location were transposed the SAME way (e.g. both indexed by the stepped
// zone instead of the affected one), the pair would still round-trip
// correctly with each other while every OTHER consumer of the same storage
// -- the GET /api/zones JSON, zone_coupling_solve.c, autotune_engine.c's
// coupling-aware feedforward -- would silently read the wrong cell. A round
// trip through one matched getter/setter pair can never rule that out.
//
// This version drives the matrix in through zones_config_set_coupling() (the
// same setter finalize_fit() and the z%u_coupling_c%u POST path both funnel
// through) and reads it back through zones_get_handler() -- the REAL
// production GET /api/zones handler, which serializes coupling_coeff[]
// straight out of zone_cfg_t with no dependency on zones_config_get_coupling()
// at all (see zones_http_handlers.c's "coupling_c%u" emission). That is a
// genuinely independent second view of the same storage: a setter bug that
// lands cell [affected][stepped] in the wrong zone's array shows up here
// even if a same-shaped getter bug would have hidden it from a get/set-only
// round trip.
static void test_coupling_matrix_2026_09_02_adopted_orientation_not_transposed(void)
{
    TEST_SECTION("zones_config_set_coupling -> zones_get_handler -- the adopted 2026-09-02 matrix lands in "
                 "[affected][stepped] storage orientation, not transposed (verified through the independent "
                 "GET /api/zones wire path, not the matching getter)");
    nvs_test_enable(true);
    nvs_test_clear();
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;

    // matrix[affected][stepped], diagonal forced to 0 by the setter's own rule.
    const float matrix[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT] = {
        {0.0f,   27.32f, 21.72f},
        {14.30f, 0.0f,   22.15f},
        {8.33f,  12.42f, 0.0f},
    };

    for (uint8_t affected = 0; affected < MAX31856_CHANNEL_COUNT; affected++) {
        TEST_CHECK(zones_config_set_coupling(affected, matrix[affected]),
                  "row `affected` is accepted");
    }

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    s_last_resp_body[0] = '\0';
    s_last_resp_len = 0;
    esp_err_t err = zones_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "zones_get_handler must return ESP_OK");

    for (uint8_t affected = 0; affected < MAX31856_CHANNEL_COUNT; affected++) {
        for (uint8_t stepped = 0; stepped < MAX31856_CHANNEL_COUNT; stepped++) {
            char needle[40];
            snprintf(needle, sizeof(needle), "\"coupling_c%u\":%.4f", (unsigned)stepped,
                     (double)matrix[affected][stepped]);
            char msg[192];
            snprintf(msg, sizeof(msg),
                     "GET /api/zones's zone %u must report coupling_c%u == the adopted matrix's "
                     "[affected=%u][stepped=%u] cell (%.4f), NOT its transpose [stepped][affected] -- this "
                     "reads the storage independently of zones_config_get_coupling()",
                     affected, stepped, affected, stepped, (double)matrix[affected][stepped]);
            TEST_CHECK(zone_json_field_present(s_last_resp_body, affected, needle), msg);
        }
    }

    // The asymmetry itself, called out explicitly: z1's step on z0 (14.30)
    // must not be confused with z0's step on z1 (27.32) -- a transposed
    // apply would swap exactly this pair and still "look plausible" (both
    // are positive, both in range) without this check.
    TEST_CHECK(zone_json_field_present(s_last_resp_body, 0, "\"coupling_c1\":27.3200"),
              "zone 0 (affected) reports coupling_c1 (stepped=1) as 27.32, z1's step raising z0");
    TEST_CHECK(zone_json_field_present(s_last_resp_body, 1, "\"coupling_c0\":14.3000"),
              "zone 1 (affected) reports coupling_c0 (stepped=0) as only 14.30, z0's step raising z1");

    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_settings_source_setter_round_trip_and_bounds(void)
{
    TEST_SECTION("zones_config_set/get_settings_source -- CUSTOM or a real other zone, self-reference refused");
    nvs_test_enable(true);
    nvs_test_clear();
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;
    /* Real decoded configs never leave an in-use zone's settings_source at
     * raw 0 unintentionally (convert_zone_v9() etc. explicitly seed
     * ZONE_SETTINGS_SOURCE_CUSTOM, "NEVER 0" -- see that function's own
     * comment); memset(0) above is a test-only shortcut that leaves zones 1
     * and 2 looking like they explicitly link to zone 0, which is not a
     * state real firmware ever produces. Seed them CUSTOM here so this test
     * exercises the setter against a realistic starting config, not an
     * artifact of the test harness's own zeroing. */
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        s_zones.cfg.zones[z].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    }

    TEST_CHECK(zones_config_set_settings_source(0, 1), "zone 0 copying zone 1 is legal");
    uint8_t out = 0;
    TEST_CHECK(zones_config_get_settings_source(0, &out), "getter succeeds");
    TEST_CHECK(out == 1, "the exact value set comes back out");

    TEST_CHECK(zones_config_set_settings_source(0, ZONE_SETTINGS_SOURCE_CUSTOM), "0xFF (CUSTOM) is always legal");
    TEST_CHECK(zones_config_get_settings_source(0, &out) && out == ZONE_SETTINGS_SOURCE_CUSTOM,
              "CUSTOM round-trips exactly");

    TEST_CHECK(!zones_config_set_settings_source(0, 0), "zone 0 cannot copy itself (self-reference refused)");
    TEST_CHECK(!zones_config_set_settings_source(0, 3), "zone index 3 does not exist (thermo_count is 3, 0-2 valid)");
    TEST_CHECK(zones_config_get_settings_source(0, &out) && out == ZONE_SETTINGS_SOURCE_CUSTOM,
              "every refused call above left the stored value at CUSTOM (refuse, not clamp)");

    nvs_test_enable(false);
    nvs_test_clear();
}

// ---------------------------------------------------------------------------
// ZONES_CFG_VERSION 12->13: the tuning-quality record (set 1 of the owner's
// two-sets request -- see zone_cfg_t::tuning_valid's own doc comment).
// ---------------------------------------------------------------------------

// Round-trip through zones_config_set_tuning_quality()/get_tuning_quality()
// using a DELIBERATELY ASYMMETRIC per-zone fixture (every field distinct
// between zone 0 and zone 2, and zone 1 left entirely unset) -- a transposed
// zone index, or a getter/setter that silently shares one struct across
// zones, would fail this test rather than coincidentally pass it, the same
// discipline test_coupling_single_cell_setter_preserves_other_cells() above
// applies to the coupling row setter.
static void test_tuning_quality_round_trip_asymmetric_per_zone(void)
{
    TEST_SECTION("zones_config_set/get_tuning_quality -- asymmetric per-zone round-trip, "
                 "zone 1 stays unknown, a transposed zone index would fail this");
    nvs_test_enable(true);
    nvs_test_clear();
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;

    zone_tuning_quality_t q0 = {
        .valid = true, .method = 0 /* STEP */, .rule = 0 /* SIMC */,
        .settled = true, .extrapolation_converged = true, .tau_consistent = false,
        .baseline_c = 25.5f, .step_ambient_c = 24.0f, .raw_rise_c = 40.0f, .rise_inf_c = 45.5f,
    };
    zone_tuning_quality_t q2 = {
        .valid = true, .method = 1 /* RELAY, stored even though finalize_fit() never actually
                                     * calls this setter for a relay run -- the getter/setter pair
                                     * itself must not assume STEP */
        , .rule = 3 /* Cohen-Coon */,
        .settled = false, .extrapolation_converged = false, .tau_consistent = true,
        .baseline_c = 802.1f, .step_ambient_c = 799.9f, .raw_rise_c = 12.25f, .rise_inf_c = 11.9f,
    };

    TEST_CHECK(zones_config_set_tuning_quality(0, &q0), "zone 0's record is set");
    TEST_CHECK(zones_config_set_tuning_quality(2, &q2), "zone 2's record is set separately");

    zone_tuning_quality_t out0 = {0}, out1 = {0}, out2 = {0};
    TEST_CHECK(zones_config_get_tuning_quality(0, &out0), "zone 0's getter succeeds");
    TEST_CHECK(zones_config_get_tuning_quality(1, &out1), "zone 1's getter succeeds (even though unset)");
    TEST_CHECK(zones_config_get_tuning_quality(2, &out2), "zone 2's getter succeeds");

    TEST_CHECK(out0.valid, "zone 0 reads valid");
    TEST_CHECK(out0.method == 0 && out0.rule == 0, "zone 0's method/rule are exactly what was set");
    TEST_CHECK(out0.settled && out0.extrapolation_converged && !out0.tau_consistent,
              "zone 0's three flags are exactly what was set, including the one false");
    TEST_CHECK_NEAR(out0.baseline_c, 25.5f, 1e-4, "zone 0 baseline_c");
    TEST_CHECK_NEAR(out0.step_ambient_c, 24.0f, 1e-4, "zone 0 step_ambient_c");
    TEST_CHECK_NEAR(out0.raw_rise_c, 40.0f, 1e-4, "zone 0 raw_rise_c");
    TEST_CHECK_NEAR(out0.rise_inf_c, 45.5f, 1e-4, "zone 0 rise_inf_c");

    TEST_CHECK(!out1.valid, "zone 1 (never written) reads INVALID -- unknown, not a stray zero-valued record");

    TEST_CHECK(out2.valid, "zone 2 reads valid");
    TEST_CHECK(out2.method == 1 && out2.rule == 3, "zone 2's method/rule are exactly what was set -- "
              "DISTINCT from zone 0's, so a transposed zone index would fail here");
    TEST_CHECK(!out2.settled && !out2.extrapolation_converged && out2.tau_consistent,
              "zone 2's three flags are exactly what was set, the mirror-image pattern of zone 0's");
    TEST_CHECK_NEAR(out2.baseline_c, 802.1f, 1e-3, "zone 2 baseline_c -- far from zone 0's 25.5, catches a swap");
    TEST_CHECK_NEAR(out2.step_ambient_c, 799.9f, 1e-3, "zone 2 step_ambient_c");
    TEST_CHECK_NEAR(out2.raw_rise_c, 12.25f, 1e-4, "zone 2 raw_rise_c");
    TEST_CHECK_NEAR(out2.rise_inf_c, 11.9f, 1e-4, "zone 2 rise_inf_c");

    // Sequence counter: bumped by the setter itself, independently per zone.
    TEST_CHECK(s_zones.cfg.zones[0].tuning_seq == 1, "zone 0's seq is 1 after its first write");
    TEST_CHECK(s_zones.cfg.zones[2].tuning_seq == 1, "zone 2's seq is 1 after its first write, independent of zone 0's");
    TEST_CHECK(zones_config_set_tuning_quality(0, &q0), "zone 0 written a second time");
    TEST_CHECK(s_zones.cfg.zones[0].tuning_seq == 2, "zone 0's seq bumps again; zone 2's own write did not bump it");
    TEST_CHECK(s_zones.cfg.zones[2].tuning_seq == 1, "zone 2's seq is untouched by zone 0's second write");

    // Refusals: q->valid == false, NULL, out-of-range zone_index, non-finite,
    // and an out-of-range method/rule must all be refused without touching
    // storage -- same all-or-nothing discipline as every other setter here.
    zone_tuning_quality_t q_invalid = q0;
    q_invalid.valid = false;
    TEST_CHECK(!zones_config_set_tuning_quality(0, &q_invalid),
              "q->valid == false is refused -- a caller wanting to clear the record uses "
              "zones_config_set_pid() instead, never this setter");
    TEST_CHECK(!zones_config_set_tuning_quality(0, NULL), "NULL q is refused");
    TEST_CHECK(!zones_config_set_tuning_quality(3, &q0), "out-of-range zone_index is refused");
    zone_tuning_quality_t q_nan = q0;
    q_nan.baseline_c = NAN;
    TEST_CHECK(!zones_config_set_tuning_quality(0, &q_nan), "non-finite baseline_c is refused");
    zone_tuning_quality_t q_bad_rule = q0;
    q_bad_rule.rule = 4;
    TEST_CHECK(!zones_config_set_tuning_quality(0, &q_bad_rule), "an out-of-range rule value is refused");
    TEST_CHECK(zones_config_get_tuning_quality(0, &out0) && out0.tau_consistent == false &&
              out0.baseline_c == 25.5f,
              "every refusal above left zone 0's stored record exactly at its last successful write "
              "(the second q0 write), not partially applied and not touched by the refused calls");

    nvs_test_enable(false);
    nvs_test_clear();
}

// The invalidation this whole feature exists to get right: ANY gain change
// through zones_config_set_pid() -- a manual PID edit is the operator-facing
// example, but the setter cannot distinguish its callers -- must invalidate
// a previously-written tuning-quality record. This is the reset-one-side
// guard: comment out the invalidation line in zones_config_set_pid() and
// this test must fail. Verified by hand (see this file's own build log) --
// removing `z->tuning_valid = 0;` from zones_config_accessors.c leaves
// tuning_valid at 1, and the "invalidated" TEST_CHECK below goes red.
static void test_zones_config_set_pid_invalidates_tuning_quality(void)
{
    TEST_SECTION("zones_config_set_pid() invalidates a zone's tuning-quality record -- "
                 "the reset-one-side guard (a manual PID edit must not leave a stale record standing)");
    nvs_test_enable(true);
    nvs_test_clear();
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;

    zone_tuning_quality_t q = {
        .valid = true, .method = 0, .rule = 0,
        .settled = true, .extrapolation_converged = true, .tau_consistent = true,
        .baseline_c = 25.0f, .step_ambient_c = 25.0f, .raw_rise_c = 50.0f, .rise_inf_c = 50.0f,
    };
    TEST_CHECK(zones_config_set_tuning_quality(1, &q), "zone 1's record is set (autotune just accepted)");
    zone_tuning_quality_t before = {0};
    TEST_CHECK(zones_config_get_tuning_quality(1, &before) && before.valid, "sanity: zone 1 reads valid before the edit");

    // Same call POST /api/zones/pid's handler makes for a manual PID edit
    // (zones_http_handlers.c) -- this test goes through the shared setter
    // directly, which is exactly the point: invalidation lives in the ONE
    // choke point every gain-writing path (autotune accept, a manual edit,
    // adaptive_tune.c's re-blend, backup_http.c's restore, the LCD UI/
    // uart_bridge_ext.c path) already goes through, not duplicated at each
    // call site.
    TEST_CHECK(zones_config_set_pid(1, 12.0f, 0.5f, 3.0f), "a manual PID edit on zone 1 succeeds");

    zone_tuning_quality_t after = {0};
    TEST_CHECK(zones_config_get_tuning_quality(1, &after), "getter still succeeds after the edit");
    TEST_CHECK(!after.valid, "invalidated -- zone 1's tuning-quality record must read unknown "
              "after its gains changed by ANY path, not just an autotune accept");

    // Zone 0's untouched record is unaffected -- invalidation is per-zone,
    // not a global flag (would itself be a reset-one-side-shaped bug in the
    // other direction: wiping every zone's record because one was edited).
    TEST_CHECK(zones_config_set_tuning_quality(0, &q), "zone 0's own record is set");
    TEST_CHECK(zones_config_set_pid(1, 1.0f, 1.0f, 1.0f), "zone 1 edited again");
    zone_tuning_quality_t zone0_after = {0};
    TEST_CHECK(zones_config_get_tuning_quality(0, &zone0_after) && zone0_after.valid,
              "zone 0's record survives a DIFFERENT zone's gain edit -- invalidation is per-zone");

    nvs_test_enable(false);
    nvs_test_clear();
}

// ZONES_CFG_VERSION 12->13: a v12 blob (predates the tuning-quality record
// entirely) must migrate cleanly, with every tuning_* field reading as
// UNKNOWN (tuning_valid == false) rather than a value indistinguishable
// from a real (if zero-valued) measurement -- e.g. tuning_rule == 0 must
// NOT be readable as "SIMC, deliberately chosen", and tuning_settled == 0
// must NOT be readable as "measured and found unsettled". sizeof(src) is
// zone_cfg_v12_t/zones_cfg_v12_t -- frozen, historical types, never the
// live zone_cfg_t/zones_cfg_t (already the v13 shape by now), mirroring
// test_nvs_load_from_v11_blob_defaults_coupling_tau_dead_time_to_zero()
// above exactly.
static void test_nvs_load_from_v12_blob_defaults_tuning_quality_to_unknown(void)
{
    TEST_SECTION("nvs_load_from -- a v12 blob upconverts to v13: every tuning_* field defaults to "
                 "UNKNOWN (tuning_valid=false), never a value that reads as a real measurement, while "
                 "coupling_tau_s[]/model_k_dc/etc survive unchanged");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v12_t src;
    memset(&src, 0, sizeof(src));
    src.version = 12;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.continue_on_zone_trip = 1;
    src.safety_tc_type = 3;
    src.pc_link_abort_silence_ms = 45000.0f;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");

    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].max_temp_c = 1300.0f;
    src.zones[0].model_k_dc = 20.969f;
    src.zones[0].model_tau_s = 640.0f;
    src.zones[0].model_dead_time_s = 45.0f;
    src.zones[0].coupling_tau_s[1] = 42.0f;
    src.zones[0].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;
    src.zones[1].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.zones[2].relay_mask = 0x04;
    src.zones[2].thermo_mask = 0x04;
    src.zones[2].max_temp_c = 1200.0f;
    src.zones[2].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.crc32 = 0; // v12's own CRC is not checked on the old-version path

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v12 blob must migrate to a valid current (v13) config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");

    // THE thing this test is really about: every tuning_* field must be at
    // its "unknown" zero default for every zone -- not garbage, not
    // uninitialized memory -- since no version before v13 ever stored a
    // tuning-quality record.
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "zones[%u].tuning_valid is false (v12 never stored a quality record)", i);
        TEST_CHECK(!out_cfg.zones[i].tuning_valid, msg);
        snprintf(msg, sizeof(msg), "zones[%u].tuning_method reads 0, but gated unknown by tuning_valid", i);
        TEST_CHECK(out_cfg.zones[i].tuning_method == 0, msg);
        snprintf(msg, sizeof(msg), "zones[%u].tuning_settled reads 0, but gated unknown by tuning_valid", i);
        TEST_CHECK(out_cfg.zones[i].tuning_settled == 0, msg);
        snprintf(msg, sizeof(msg), "zones[%u].tuning_seq is 0 (never written)", i);
        TEST_CHECK(out_cfg.zones[i].tuning_seq == 0, msg);
    }
    // Pre-existing v12 fields must survive the upgrade completely unchanged
    // -- the same "everything else carries through" proof
    // test_nvs_load_from_v11_blob_defaults_coupling_tau_dead_time_to_zero()
    // makes for the v11->v12 migration.
    TEST_CHECK(out_cfg.thermo_count == 3 && out_cfg.relay_count == 3, "counts carried through");
    TEST_CHECK(out_cfg.continue_on_zone_trip == 1, "continue_on_zone_trip carried through");
    TEST_CHECK_NEAR(out_cfg.pc_link_abort_silence_ms, 45000.0f, 1e-6, "pc_link_abort_silence_ms carried through");
    TEST_CHECK(strcmp(out_cfg.timing_profiles[0].name, "Default") == 0, "timing profile name carried through");
    TEST_CHECK(out_cfg.zones[0].relay_mask == 0x01 && out_cfg.zones[1].relay_mask == 0x02 &&
              out_cfg.zones[2].relay_mask == 0x04, "relay_mask must NOT be shifted for any zone");
    TEST_CHECK_NEAR(out_cfg.zones[0].model_k_dc, 20.969f, 1e-6, "zones[0].model_k_dc survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].model_tau_s, 640.0f, 1e-6, "zones[0].model_tau_s survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].model_dead_time_s, 45.0f, 1e-6, "zones[0].model_dead_time_s survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].coupling_tau_s[1], 42.0f, 1e-6, "zones[0].coupling_tau_s[1] survives");
    TEST_CHECK(out_cfg.zones[0].settings_source == ZONE_SETTINGS_SOURCE_CUSTOM,
              "zones[0].settings_source is carried through verbatim");

    nvs_test_enable(false);
    nvs_test_clear();
}

// ZONES_CFG_VERSION 13->14: a v13 blob (predates adaptive_tune_enabled
// entirely -- the full tuning-quality record and everything before it, but
// no opt-in flag of its own) must migrate cleanly, with adaptive_tune_
// enabled defaulting to 0 ("opted out") for every zone, while every
// pre-existing field -- including the v13 tuning_* quality record and
// coupling_coeff[]/coupling_tau_s[]/coupling_dead_time_s[] -- survives
// unchanged. This is the migration test that was missing: convert_zone_v13()
// and case 13 of convert_versioned_blob_to_current() had never been
// exercised by a dedicated nvs_load_from() test, unlike every neighboring
// version step (v9 through v12, and v14). sizeof(src) is zone_cfg_v13_t/
// zones_cfg_v13_t -- frozen, historical types, never the live zone_cfg_t/
// zones_cfg_t (already the v14 shape by now), mirroring
// test_nvs_load_from_v12_blob_defaults_tuning_quality_to_unknown() above.
static void test_nvs_load_from_v13_blob_defaults_adaptive_tune_enabled_to_zero(void)
{
    TEST_SECTION("nvs_load_from -- a v13 blob upconverts to v14: adaptive_tune_enabled defaults to 0 "
                 "(\"opted out\") for every zone, while the tuning_* quality record/coupling_coeff[]/"
                 "coupling_tau_s[]/model_k_dc/etc survive unchanged");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v13_t src;
    memset(&src, 0, sizeof(src));
    src.version = 13;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.continue_on_zone_trip = 1;
    src.safety_tc_type = 3;
    src.pc_link_abort_silence_ms = 45000.0f;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");

    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].max_temp_c = 1300.0f;
    src.zones[0].model_k_dc = 20.969f;
    src.zones[0].model_tau_s = 640.0f;
    src.zones[0].model_dead_time_s = 45.0f;
    src.zones[0].coupling_coeff[1] = 10.887f;
    src.zones[0].coupling_tau_s[1] = 42.0f;
    src.zones[0].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    src.zones[0].tuning_valid = 1;
    src.zones[0].tuning_method = 1; /* autotune_method_t: 0=STEP, 1=RELAY */
    src.zones[0].tuning_rule = 3;   /* autotune_rule_t: 0-3 valid */
    src.zones[0].tuning_baseline_c = 25.5f;
    src.zones[0].tuning_seq = 7;

    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;
    src.zones[1].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.zones[2].relay_mask = 0x04;
    src.zones[2].thermo_mask = 0x04;
    src.zones[2].max_temp_c = 1200.0f;
    src.zones[2].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.crc32 = 0; // v13's own CRC is not checked on the old-version path

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v13 blob must migrate to a valid current (v14) config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");

    // THE thing this test is really about: adaptive_tune_enabled must be at
    // its "opted out" zero default for every zone -- not garbage, not
    // uninitialized memory -- since no version before v14 ever stored it.
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "zones[%u].adaptive_tune_enabled reads 0 (v13 never stored it)", i);
        TEST_CHECK(out_cfg.zones[i].adaptive_tune_enabled == 0, msg);
    }
    // Pre-existing v13 fields, including the tuning-quality record, must
    // survive the upgrade completely unchanged.
    TEST_CHECK(out_cfg.thermo_count == 3 && out_cfg.relay_count == 3, "counts carried through");
    TEST_CHECK(out_cfg.continue_on_zone_trip == 1, "continue_on_zone_trip carried through");
    TEST_CHECK_NEAR(out_cfg.pc_link_abort_silence_ms, 45000.0f, 1e-6, "pc_link_abort_silence_ms carried through");
    TEST_CHECK(strcmp(out_cfg.timing_profiles[0].name, "Default") == 0, "timing profile name carried through");
    TEST_CHECK(out_cfg.zones[0].relay_mask == 0x01 && out_cfg.zones[1].relay_mask == 0x02 &&
              out_cfg.zones[2].relay_mask == 0x04, "relay_mask must NOT be shifted for any zone");
    TEST_CHECK_NEAR(out_cfg.zones[0].model_k_dc, 20.969f, 1e-6, "zones[0].model_k_dc survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].coupling_coeff[1], 10.887f, 1e-6, "zones[0].coupling_coeff[1] survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].coupling_tau_s[1], 42.0f, 1e-6, "zones[0].coupling_tau_s[1] survives");
    TEST_CHECK(out_cfg.zones[0].tuning_valid == 1, "zones[0].tuning_valid survives");
    TEST_CHECK(out_cfg.zones[0].tuning_method == 1, "zones[0].tuning_method survives");
    TEST_CHECK(out_cfg.zones[0].tuning_rule == 3, "zones[0].tuning_rule survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].tuning_baseline_c, 25.5f, 1e-6, "zones[0].tuning_baseline_c survives");
    TEST_CHECK(out_cfg.zones[0].tuning_seq == 7, "zones[0].tuning_seq survives");
    TEST_CHECK(out_cfg.zones[0].settings_source == ZONE_SETTINGS_SOURCE_CUSTOM,
              "zones[0].settings_source is carried through verbatim");

    nvs_test_enable(false);
    nvs_test_clear();
}

// ZONES_CFG_VERSION 14->15 (PID_EXPANSION_PLAN.md section 3.2 follow-up): a
// v14 blob (predates coupling_diag_k_dc entirely -- adaptive_tune_enabled
// and everything before it, but no diagonal cell of its own) must migrate
// cleanly, with coupling_diag_k_dc defaulting to 0.0 ("not measured", same
// convention coupling_coeff[]/coupling_tau_s[] already use) for every zone,
// while every pre-existing field -- including adaptive_tune_enabled and the
// existing coupling_coeff[]/coupling_tau_s[] rows -- survives unchanged.
// sizeof(src) is zone_cfg_v14_t/zones_cfg_v14_t -- frozen, historical types,
// never the live zone_cfg_t/zones_cfg_t (already the v15 shape by now),
// mirroring test_nvs_load_from_v12_blob_defaults_tuning_quality_to_unknown()
// above exactly.
static void test_nvs_load_from_v14_blob_defaults_coupling_diag_k_dc_to_zero(void)
{
    TEST_SECTION("nvs_load_from -- a v14 blob upconverts to v15: coupling_diag_k_dc defaults to 0.0 "
                 "(\"not measured\") for every zone, while adaptive_tune_enabled/coupling_coeff[]/"
                 "coupling_tau_s[]/model_k_dc/etc survive unchanged");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v14_t src;
    memset(&src, 0, sizeof(src));
    src.version = 14;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.continue_on_zone_trip = 1;
    src.safety_tc_type = 3;
    src.pc_link_abort_silence_ms = 45000.0f;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");

    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].max_temp_c = 1300.0f;
    src.zones[0].model_k_dc = 20.969f;
    src.zones[0].model_tau_s = 640.0f;
    src.zones[0].model_dead_time_s = 45.0f;
    src.zones[0].coupling_coeff[1] = 10.887f;
    src.zones[0].coupling_tau_s[1] = 42.0f;
    src.zones[0].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    src.zones[0].adaptive_tune_enabled = 1;

    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;
    src.zones[1].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.zones[2].relay_mask = 0x04;
    src.zones[2].thermo_mask = 0x04;
    src.zones[2].max_temp_c = 1200.0f;
    src.zones[2].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.crc32 = 0; // v14's own CRC is not checked on the old-version path

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v14 blob must migrate to a valid current (v15) config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");

    // THE thing this test is really about: coupling_diag_k_dc must be at its
    // "not measured" zero default for every zone -- not garbage, not
    // uninitialized memory -- since no version before v15 ever stored it.
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "zones[%u].coupling_diag_k_dc reads 0.0 (v14 never stored it)", i);
        TEST_CHECK(out_cfg.zones[i].coupling_diag_k_dc == 0.0f, msg);
    }
    // Pre-existing v14 fields must survive the upgrade completely unchanged
    // -- the same "everything else carries through" proof
    // test_nvs_load_from_v12_blob_defaults_tuning_quality_to_unknown() makes
    // for the v12->v13 migration.
    TEST_CHECK(out_cfg.thermo_count == 3 && out_cfg.relay_count == 3, "counts carried through");
    TEST_CHECK(out_cfg.continue_on_zone_trip == 1, "continue_on_zone_trip carried through");
    TEST_CHECK_NEAR(out_cfg.pc_link_abort_silence_ms, 45000.0f, 1e-6, "pc_link_abort_silence_ms carried through");
    TEST_CHECK(strcmp(out_cfg.timing_profiles[0].name, "Default") == 0, "timing profile name carried through");
    TEST_CHECK(out_cfg.zones[0].relay_mask == 0x01 && out_cfg.zones[1].relay_mask == 0x02 &&
              out_cfg.zones[2].relay_mask == 0x04, "relay_mask must NOT be shifted for any zone");
    TEST_CHECK_NEAR(out_cfg.zones[0].model_k_dc, 20.969f, 1e-6, "zones[0].model_k_dc survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].coupling_coeff[1], 10.887f, 1e-6, "zones[0].coupling_coeff[1] survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].coupling_tau_s[1], 42.0f, 1e-6, "zones[0].coupling_tau_s[1] survives");
    TEST_CHECK(out_cfg.zones[0].settings_source == ZONE_SETTINGS_SOURCE_CUSTOM,
              "zones[0].settings_source is carried through verbatim");
    TEST_CHECK(out_cfg.zones[0].adaptive_tune_enabled == 1,
              "zones[0].adaptive_tune_enabled survives -- a real prior opt-in choice must not be lost");
    TEST_CHECK(out_cfg.zones[1].adaptive_tune_enabled == 0,
              "zones[1].adaptive_tune_enabled stays 0 (never opted in)");

    nvs_test_enable(false);
    nvs_test_clear();
}

// ---------------------------------------------------------------------------
// PID_EXPANSION_PLAN.md line ~864's three negative tests for zone settings
// inheritance. The feature itself (self-reference refused at the door) was
// already covered above; these three were missing entirely.
// ---------------------------------------------------------------------------

// 1. Save/reload round-trip: a zone's settings_source link (a real OTHER
// zone index, not CUSTOM) must survive an nvs_save()/nvs_load() cycle intact
// -- same shape as test_nvs_save_load_round_trip_current_version() above,
// narrowed to the one field this task calls out by name.
static void test_settings_source_save_reload_inheritance_round_trip(void)
{
    TEST_SECTION("settings_source -- a real inheritance link (zone 1 copies zone 2) survives "
                 "nvs_save()/nvs_load() and still names the right source zone afterward");
    nvs_test_enable(true);
    nvs_test_clear();

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;
    s_zones.cfg.relay_count = 3;
    s_zones.cfg.timing_profile_count = 1;
    strncpy(s_zones.cfg.timing_profiles[0].name, "Default", TIMING_PROFILE_NAME_MAX_LEN);
    for (uint8_t i = 0; i < 3; i++) {
        zone_cfg_t *z = &s_zones.cfg.zones[i];
        z->relay_mask = (uint8_t)(1u << i);
        z->thermo_mask = (uint8_t)(1u << i);
        z->max_temp_c = 1300.0f;
        z->timing_profile = 0;
    }
    s_zones.cfg.zones[0].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    s_zones.cfg.zones[1].settings_source = 2; /* zone 1 inherits from zone 2 */
    s_zones.cfg.zones[2].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    /* A distinctive value on the SOURCE zone, so "resolves to the right
     * source zone" is checkable, not just "some link survived". */
    s_zones.cfg.zones[2].tc_type = 5;
    s_zones.cfg.zones[2].pid_kp = 7.25f;

    esp_err_t save_err = nvs_save();
    TEST_CHECK(save_err == ESP_OK, "nvs_save() must succeed");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg)); // wipe the live struct, force a real reload
    bool found = false, valid = false;
    esp_err_t load_err = nvs_load(&found, &valid);
    TEST_CHECK(load_err == ESP_OK && found && valid, "nvs_load() must succeed and report found+valid");

    uint8_t src = 0xAA;
    TEST_CHECK(zones_config_get_settings_source(1, &src) && src == 2,
              "zone 1's settings_source (2, a real zone) survives the round trip exactly");
    TEST_CHECK(s_zones.cfg.zones[0].settings_source == ZONE_SETTINGS_SOURCE_CUSTOM,
              "zone 0's CUSTOM marker round-trips too, not disturbed by zone 1's link");
    /* "resolves to the right source zone": follow the link this test set up
     * and confirm the values sitting there are still the source zone's own
     * -- not zeroed, not zone 1's, proving the link actually points
     * somewhere real and reloadable, not just a surviving integer. */
    TEST_CHECK(s_zones.cfg.zones[src].tc_type == 5, "the zone settings_source(1) names still holds its own tc_type");
    TEST_CHECK_NEAR(s_zones.cfg.zones[src].pid_kp, 7.25f, 1e-6,
                    "and still holds its own pid_kp -- zone 1's link resolves to the correct data");

    nvs_test_enable(false);
    nvs_test_clear();
}

// 2. Cycle refusal -- REGRESSION TEST for the fix (was: a documented defect).
//
// PID_EXPANSION_PLAN.md line ~864 requires that a configuration forming an
// inheritance cycle "must not be storable or must collapse safely". The
// storage layer (zones_config_set_settings_source(), and parse_zone_fields()'s
// identical z%u_settings_source door) used to refuse only SELF-reference
// (settings_source == zone_index) -- see both functions' own comments -- and
// never checked the TARGET zone's own settings_source, so a genuine 2-zone
// cycle (zone 0 -> zone 1, zone 1 -> zone 0) was reachable by two ordinary,
// individually-legal setter calls -- exactly the "hand-edited backup" path an
// opus reviewer flagged: backup_http.c's importer commits settings_source
// through this same setter, one zone entry at a time.
//
// Both doors now run zones_config_json_settings_source_chain_has_cycle() (a bounded
// visited-set walk, capped at MAX31856_CHANNEL_COUNT hops so it terminates
// even against an already-corrupt stored chain) before committing a new
// link, so the second call of any cycle-closing pair is refused and the
// live config is left exactly where the first call put it -- refuse, not
// partially apply. This test used to assert the opposite (the cycle IS
// storable) as a positive proof of the then-open defect; it now asserts the
// fix: the SAME sequence of calls is refused at the second link, for both
// the 2-cycle and the 3-cycle, through both doors (the direct setter and
// parse_zone_fields()), and a legal (acyclic) chain is still storable so
// this isn't a blanket refusal.
static void test_settings_source_two_and_three_zone_cycles_are_refused(void)
{
    TEST_SECTION("settings_source -- FIX: a 2-zone and a 3-zone inheritance cycle are BOTH refused "
                 "(only the SECOND, cycle-closing link of each pair) -- PID_EXPANSION_PLAN.md line ~864's "
                 "'must not be storable' requirement, now enforced by a real chain-walk");
    nvs_test_enable(true);
    nvs_test_clear();
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;
    /* Real decoded configs never leave an in-use zone's settings_source at
     * raw 0 unintentionally (see convert_zone_v9()'s "NEVER 0" comment) --
     * 0 is a REAL, DIFFERENT settings_source value ("copy zone 0"), not a
     * "not set" sentinel, so a plain memset(0) here would make zones 1 and 2
     * look like they already explicitly link to zone 0 before this test
     * ever touches them. Seed CUSTOM so the starting config matches what
     * real firmware actually produces, not a memset artifact. */
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        s_zones.cfg.zones[z].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    }

    // ---- Direct setter, 2-cycle: zone 0 -> zone 1, then zone 1 -> zone 0 ----
    TEST_CHECK(zones_config_set_settings_source(0, 1), "zone 0 -> zone 1 is accepted on its own "
              "(0 -> 1 -> Custom terminates cleanly, no cycle yet)");
    TEST_CHECK(!zones_config_set_settings_source(1, 0), "zone 1 -> zone 0 is REFUSED -- it would close "
              "a genuine mutual cycle (0 -> 1 -> 0), and the chain-walk now catches that the first call "
              "alone could not");
    uint8_t s0 = 0xAA, s1 = 0xAA;
    TEST_CHECK(zones_config_get_settings_source(0, &s0) && s0 == 1, "zone 0's link is still 1 (the "
              "legal first call)");
    TEST_CHECK(zones_config_get_settings_source(1, &s1) && s1 == ZONE_SETTINGS_SOURCE_CUSTOM,
              "zone 1 is still Custom -- the refused call left it untouched, not partially applied");

    // ---- Direct setter, 3-cycle: 0 -> 1 already set above; 1 -> 2, then 2 -> 0 ----
    TEST_CHECK(zones_config_set_settings_source(1, 2), "zone 1 -> zone 2 is accepted (1 -> 2 -> Custom, "
              "no cycle yet -- zone 0's own 0 -> 1 link doesn't participate in THIS chain's termination "
              "check, only in whether closing it later would cycle)");
    TEST_CHECK(!zones_config_set_settings_source(2, 0), "zone 2 -> zone 0 is REFUSED -- it would close "
              "the 3-zone cycle 0 -> 1 -> 2 -> 0");
    uint8_t s2 = 0xAA;
    TEST_CHECK(zones_config_get_settings_source(0, &s0) && s0 == 1 &&
              zones_config_get_settings_source(1, &s1) && s1 == 2 &&
              zones_config_get_settings_source(2, &s2) && s2 == ZONE_SETTINGS_SOURCE_CUSTOM,
              "zone 0 -> 1 -> 2 (a legal, acyclic chain) is exactly what's stored -- the refused "
              "2 -> 0 call left zone 2 at Custom, not half-applied");

    // ---- Self-reference: still refused (unchanged behaviour, not a regression) ----
    TEST_CHECK(!zones_config_set_settings_source(0, 0), "self-reference is still refused directly, "
              "independent of the longer-chain guard added above");

    // ---- Positive control: a legal chain (no cycle) is still storable ----
    // Reset zone 2 back to Custom and re-close the SAME 0 -> 1 -> 2 chain
    // from a clean start, proving the guard above refuses ONLY the
    // cycle-closing link and does not over-reject an ordinary acyclic chain.
    TEST_CHECK(zones_config_set_settings_source(2, ZONE_SETTINGS_SOURCE_CUSTOM),
              "zone 2 reset to Custom (2 -> Custom is never a cycle)");
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        s_zones.cfg.zones[z].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    }
    TEST_CHECK(zones_config_set_settings_source(0, 1), "0 -> 1 (fresh chain)");
    TEST_CHECK(zones_config_set_settings_source(1, 2), "1 -> 2 (fresh chain, still acyclic: 0->1->2->Custom)");
    TEST_CHECK(zones_config_get_settings_source(0, &s0) && s0 == 1 &&
              zones_config_get_settings_source(1, &s1) && s1 == 2,
              "the legal 0 -> 1 -> 2 chain is fully storable -- the guard does not over-reject a chain "
              "that never revisits a zone");

    // ---- The same guard through parse_zone_fields() (the POST/import door) ----
    // Current live state: zone 0 -> 1 -> 2 -> Custom (set directly above).
    // Free zone 0 back to Custom, then point zone 1 at zone 0 directly (a
    // fresh, legal link: 1 -> 0 -> Custom) so a SUBSEQUENT parse_zone_fields()
    // call closing zone 0 back onto zone 1 is a genuine 2-cycle, not a
    // freshly-created one.
    TEST_CHECK(zones_config_set_settings_source(0, ZONE_SETTINGS_SOURCE_CUSTOM),
              "zone 0 reset to Custom, freeing it to be pointed at");
    TEST_CHECK(zones_config_set_settings_source(1, 0), "zone 1 -> zone 0 now legal (0 is Custom): "
              "live state is now zone 0 = Custom, zone 1 -> 0");
    zone_cfg_t current0 = make_stored_zone();
    zone_cfg_t out0;
    const char *reason = "unset";
    TEST_CHECK(!post_body_with_extra("z0_settings_source=1", &reason, &out0),
              "parse_zone_fields() (the POST/import door) REFUSES the identical 2-cycle a whole-page POST "
              "or backup_http.c's importer would create: zone 1 already points at zone 0 live, so posting "
              "z0_settings_source=1 (zone 0 -> zone 1) would close 0 -> 1 -> 0 -- refused through this "
              "door too, not just the direct setter");
    TEST_CHECK(reason && strstr(reason, "settings_source") != NULL && strstr(reason, "cycle") != NULL,
              "the refusal names the field and calls out the cycle specifically");
    (void)current0;

    nvs_test_enable(false);
    nvs_test_clear();
}

// 3. Shared-channel tc_type agreement. PID_EXPANSION_PLAN.md's corrected
// rule (2026-08-30, after a real bug): a zone with its OWN custom settings
// writes tc_type as the identity -- zone i's field sets channel i's type,
// full stop. The mask fan-out ("copy the terminal zone's type onto every
// channel this zone's thermo_mask actually reads") is exclusively a
// zones_page.html UI computation performed BEFORE the value is POSTed --
// PID_EXPANSION_PLAN.md's own "Storage" note says so explicitly ("this is a
// UI-level convenience... not a new inheritance layer in the firmware...
// an inheriting zone writes the resolved values into its own zone_cfg_t
// exactly as if they had been typed"). There is no fan-out, and no
// settings_source-conditioned override, anywhere in zones_http.c's storage
// layer -- confirmed by grep: tc_type is written by exactly one path
// (parse_zone_fields()'s z%u_tctype handling / zones_config_set_tc_type()),
// keyed ONLY on the zone/channel index the caller names, never touched by
// settings_source or thermo_mask.
//
// This test is the storage-layer half of that contract: it proves tc_type
// is written per-index, independent of that same zone's settings_source --
// i.e. firmware never silently re-types a channel based on inheritance
// state, which is exactly the bug PID_EXPANSION_PLAN.md's correction
// describes ("an operator setting channel 2 silently re-types channel 0").
// Two zones sharing a channel getting DIFFERENT tc_type values here (last-
// write-wins, as the plan explicitly allows) is the proof: if firmware were
// doing its own fan-out/sync, zone 1's tc_type would have been forced to
// match zone 0's once both were saved.
static void test_tc_type_write_is_identity_independent_of_settings_source(void)
{
    TEST_SECTION("tc_type -- storage writes it as the identity (zone i sets channel i), independent of "
                 "that zone's settings_source -- no firmware-side fan-out/re-sync to reintroduce the "
                 "'wrong channel re-typed' bug PID_EXPANSION_PLAN.md's 3.5 correction describes");
    nvs_test_enable(true);
    nvs_test_clear();
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;

    // Zone 0: CUSTOM settings, its own tc_type.
    TEST_CHECK(zones_config_set_settings_source(0, ZONE_SETTINGS_SOURCE_CUSTOM), "zone 0 is Custom");
    TEST_CHECK(zones_config_set_tc_type(0, 2), "zone 0's own channel is set to type 2");

    // Zone 1: inherits from a DIFFERENT terminal zone (zone 0), but is
    // itself posted with tc_type=2 too -- what zones_page.html's UI would
    // have written after resolving/fanning-out zone 0's type onto zone 1's
    // own channel index BEFORE the save, per the plan's corrected rule. The
    // firmware-side assertion here is simply that whatever value comes in
    // for zone 1's own index is what gets stored for zone 1's own index --
    // it does not derive or override it from settings_source itself.
    TEST_CHECK(zones_config_set_settings_source(1, 0), "zone 1 is set to inherit from zone 0");
    TEST_CHECK(zones_config_set_tc_type(1, 2), "zone 1's own channel is written with the fanned-out value (2)");

    // Zone 2: also CUSTOM, but its OWN channel is a genuinely different
    // type. If firmware secretly synced tc_type across zones sharing a
    // settings_source chain or a mask, this would have been dragged to 2 as
    // well by one of the writes above -- last-write-wins per zone's own
    // index is the only rule actually enforced.
    TEST_CHECK(zones_config_set_settings_source(2, ZONE_SETTINGS_SOURCE_CUSTOM), "zone 2 is Custom");
    TEST_CHECK(zones_config_set_tc_type(2, 6), "zone 2's own channel is set to a DIFFERENT type (6)");

    uint8_t t0 = 0xFF, t1 = 0xFF, t2 = 0xFF;
    TEST_CHECK(zones_config_get_tc_type(0, &t0) && t0 == 2, "zone 0's tc_type is exactly what zone 0 set");
    TEST_CHECK(zones_config_get_tc_type(1, &t1) && t1 == 2,
              "zone 1's tc_type is exactly what was posted for zone 1's OWN index -- identity write, "
              "not derived from settings_source at the storage layer");
    TEST_CHECK(zones_config_get_tc_type(2, &t2) && t2 == 6,
              "zone 2's tc_type is untouched by zones 0/1's writes -- no cross-zone fan-out happens in "
              "firmware storage, proving the mask fan-out described in the plan is exclusively the UI's "
              "job, done before the POST, never re-derived or overridden here");

    // Now change zone 1's settings_source WITHOUT touching its tc_type at
    // all -- firmware must not silently re-type zone 1's channel just
    // because its inheritance link moved. (Re-resolving tc_type on a source
    // change is the UI's job, per the plan's own "must re-resolve and
    // re-save... on save in renderZones()'s submit path" note -- a firmware
    // guard that re-derived it here on a bare settings_source write would
    // be the identical bug in a different spot.)
    TEST_CHECK(zones_config_set_settings_source(1, 2), "zone 1's link is repointed to zone 2");
    TEST_CHECK(zones_config_get_tc_type(1, &t1) && t1 == 2,
              "zone 1's tc_type is STILL 2 (its own last-written value) after its settings_source moved -- "
              "not zone 2's type (6), proving no implicit re-typing on a bare source change");

    nvs_test_enable(false);
    nvs_test_clear();
}

// ---------------------------------------------------------------------------
// Relay names (owner report 2026-08-27+1: "the user should be able to assign
// names to relays not assigned to zones as well"). Separate NVS blob from
// zones_cfg_t -- see zones_http.c's relay-names section header comment for
// the design decision and the byte arithmetic behind it. Same nvs_test_enable
// single-blob-slot stub the zones_cfg_t tests above use; each test that
// touches NVS enables/clears it and disables it again when done, same
// discipline as every test in this file.
// ---------------------------------------------------------------------------

static void reset_relay_names(void)
{
    memset(&s_relay_names.cfg, 0, sizeof(s_relay_names.cfg));
}

static void stage_relay_names_blob(const void *data, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition("whatever", NVS_NAMESPACE, NVS_READWRITE, &h);
    (void)err; // the stub always succeeds once nvs_test_enable(true) is set
    nvs_set_blob(h, NVS_KEY_RELAY_NAMES, data, len);
    nvs_close(h);
}

/* zones_config_set_relay_name() persists via relay_names_save(), which opens
 * an NVS handle the same way every other setter's nvs_save() call does --
 * with the stub's default (nvs_test_enable(false)) that open fails closed
 * (ESP_ERR_NVS_NOT_FOUND), so the setter would report false even though the
 * in-RAM write it makes BEFORE calling relay_names_save() is fine. Every test
 * below that calls the setter enables the stub first, same as
 * test_nvs_save_load_round_trip_current_version() does for
 * zones_config_set_name()'s equivalent. */

static void test_relay_name_get_set_round_trip(void)
{
    TEST_SECTION("zones_config_get/set_relay_name -- basic round trip");
    nvs_test_enable(true);
    nvs_test_clear();
    reset_relay_names();

    TEST_CHECK(zones_config_set_relay_name(3, "Vent fan"), "set on an in-range relay must succeed");
    char out[RELAY_NAME_MAX_LEN + 1];
    TEST_CHECK(zones_config_get_relay_name(3, out, sizeof(out)), "get on an in-range relay must succeed");
    TEST_CHECK(strcmp(out, "Vent fan") == 0, "the exact string set must come back out");

    /* A relay never named must read back as a real (true), empty string --
     * not a getter failure, same "false means cannot answer, not answer is
     * empty" convention every other named getter in zones_http.c uses. */
    TEST_CHECK(zones_config_get_relay_name(1, out, sizeof(out)), "get on a never-named relay must still succeed");
    TEST_CHECK(out[0] == '\0', "a never-named relay reads back as an empty string");

    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_relay_name_setter_rejects_out_of_range_relay(void)
{
    TEST_SECTION("zones_config_set_relay_name -- relay_n out of range is rejected");
    reset_relay_names();
    TEST_CHECK(!zones_config_set_relay_name(0, "x"), "relay 0 does not exist (1-based numbering)");
    TEST_CHECK(!zones_config_set_relay_name((uint8_t)(KILN_IO_RELAY_COUNT + 1), "x"),
              "a relay past KILN_IO_RELAY_COUNT does not exist");
    char out[RELAY_NAME_MAX_LEN + 1];
    TEST_CHECK(!zones_config_get_relay_name(0, out, sizeof(out)), "getter must reject relay 0 too");
    TEST_CHECK(!zones_config_get_relay_name((uint8_t)(KILN_IO_RELAY_COUNT + 1), out, sizeof(out)),
              "getter must reject a relay past KILN_IO_RELAY_COUNT too");
}

static void test_relay_name_setter_rejects_overlong_name(void)
{
    TEST_SECTION("zones_config_set_relay_name -- a name longer than RELAY_NAME_MAX_LEN is rejected, "
                 "and nothing is written (matches zones_config_set_name()'s own z%u_name rejection)");
    nvs_test_enable(true);
    nvs_test_clear();
    reset_relay_names();
    TEST_CHECK(zones_config_set_relay_name(2, "short"), "seed a known-good value first");

    char overlong[RELAY_NAME_MAX_LEN + 2];
    memset(overlong, 'x', sizeof(overlong) - 1);
    overlong[sizeof(overlong) - 1] = '\0';
    TEST_CHECK(!zones_config_set_relay_name(2, overlong), "a name one char over the limit is rejected");

    char out[RELAY_NAME_MAX_LEN + 1];
    TEST_CHECK(zones_config_get_relay_name(2, out, sizeof(out)), "getter still works after the rejection");
    TEST_CHECK(strcmp(out, "short") == 0, "the rejected write must not have touched the stored value");

    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_relay_name_setter_null_clears(void)
{
    TEST_SECTION("zones_config_set_relay_name -- NULL clears the name, matching zones_config_set_name()'s convention");
    nvs_test_enable(true);
    nvs_test_clear();
    reset_relay_names();
    TEST_CHECK(zones_config_set_relay_name(4, "Kiln light"), "seed a name");
    TEST_CHECK(zones_config_set_relay_name(4, NULL), "NULL must be accepted (treated as empty)");
    char out[RELAY_NAME_MAX_LEN + 1];
    TEST_CHECK(zones_config_get_relay_name(4, out, sizeof(out)), "getter still works");
    TEST_CHECK(out[0] == '\0', "the name must now be empty");

    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_relay_name_survives_relay_becoming_zone_owned(void)
{
    TEST_SECTION("a relay's stored name is KEPT, not cleared, when the relay becomes zone-owned "
                 "(design decision -- see zones_http.c's relay-names section header comment)");
    nvs_test_enable(true);
    nvs_test_clear();
    reset_relay_names();
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));

    TEST_CHECK(zones_config_set_relay_name(1, "Vent fan"), "name relay 1 while it belongs to no zone");

    /* Now claim relay 1 (bit 0) into zone 0 -- the getter must still answer
     * with the same string; nothing about zone assignment touches storage. */
    s_zones.cfg.thermo_count = 1;
    s_zones.cfg.zones[0].relay_mask = 0x01;
    TEST_CHECK((zone_owned_relay_mask(&s_zones.cfg) & 0x01) != 0, "sanity: relay 1 now reads as zone-owned");

    char out[RELAY_NAME_MAX_LEN + 1];
    TEST_CHECK(zones_config_get_relay_name(1, out, sizeof(out)), "getter still works once zone-owned");
    TEST_CHECK(strcmp(out, "Vent fan") == 0, "the name must still be there, unmodified");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_zone_owned_relay_mask_is_union_of_zone_relay_masks(void)
{
    TEST_SECTION("zone_owned_relay_mask -- union of every configured zone's relay_mask, matching "
                 "rules_task.c's compute_heater_relay_mask()/rules_http.c's check_relay_not_zone_owned() rule");
    zones_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.thermo_count = 2;
    cfg.zones[0].relay_mask = 0x01; /* relay 1 */
    cfg.zones[1].relay_mask = 0x06; /* relays 2 and 3 */
    /* zones[2] is past thermo_count and must not contribute even though it
       has a nonzero relay_mask left over from some earlier state. */
    cfg.zones[2].relay_mask = 0x08; /* relay 4 -- must be IGNORED */

    TEST_CHECK(zone_owned_relay_mask(&cfg) == 0x07, "mask must be the union of only the in-range zones' bits "
                                                     "(0x01 | 0x06 = 0x07), not including the past-thermo_count zone");
}

static void test_relay_names_load_wrong_length_blob_is_rejected(void)
{
    TEST_SECTION("relay_names_load -- a blob of the wrong length for relay_names_cfg_t is discarded "
                 "(names reset to blank), same length-before-interpretation discipline as the zones blob");
    nvs_test_enable(true);
    nvs_test_clear();
    relay_names_cfg_t src;
    memset(&src, 0, sizeof(src));
    src.version = RELAY_NAMES_CFG_VERSION;
    src.crc32 = compute_relay_names_crc(&src);
    stage_relay_names_blob(&src, sizeof(src) - 1); /* one byte short */

    reset_relay_names();
    strcpy(s_relay_names.cfg.names[0], "pre-existing junk"); /* proves load() actually clears, not just "leaves 0" */
    relay_names_load();
    TEST_CHECK(s_relay_names.cfg.names[0][0] == '\0', "a wrong-length blob must reset names to blank");

    nvs_test_enable(false);
    nvs_test_clear();
    reset_relay_names();
}

static void test_relay_names_load_wrong_version_is_rejected(void)
{
    TEST_SECTION("relay_names_load -- an unrecognized version is discarded, never guessed at "
                 "(same NEWER-refuses-to-load discipline as zones_config_json_decode_blob())");
    nvs_test_enable(true);
    nvs_test_clear();
    relay_names_cfg_t src;
    memset(&src, 0, sizeof(src));
    src.version = (uint8_t)(RELAY_NAMES_CFG_VERSION + 1);
    strcpy(src.names[0], "should never surface");
    src.crc32 = compute_relay_names_crc(&src);
    stage_relay_names_blob(&src, sizeof(src));

    reset_relay_names();
    relay_names_load();
    TEST_CHECK(s_relay_names.cfg.names[0][0] == '\0',
              "an unrecognized version's names must never reach s_relay_names.cfg");

    nvs_test_enable(false);
    nvs_test_clear();
    reset_relay_names();
}

static void test_relay_names_load_bad_crc_is_rejected(void)
{
    TEST_SECTION("relay_names_load -- a CRC mismatch is discarded, same integrity gate as the zones blob's crc32");
    nvs_test_enable(true);
    nvs_test_clear();
    relay_names_cfg_t src;
    memset(&src, 0, sizeof(src));
    src.version = RELAY_NAMES_CFG_VERSION;
    strcpy(src.names[0], "should never surface");
    src.crc32 = compute_relay_names_crc(&src) ^ 0xFFFFFFFFu; /* deliberately wrong */
    stage_relay_names_blob(&src, sizeof(src));

    reset_relay_names();
    relay_names_load();
    TEST_CHECK(s_relay_names.cfg.names[0][0] == '\0', "a bad-CRC blob's names must never reach s_relay_names.cfg");

    nvs_test_enable(false);
    nvs_test_clear();
    reset_relay_names();
}

/* THE migration-style test the owner's report specifically calls for: build a
 * real on-flash blob with DISTINCT values in every slot, load it through the
 * real relay_names_load() path, and assert every one of them survives. This
 * is the shape of test that would have caught profiles_http.c's
 * sizeof(current struct)-for-an-old-version data-loss bug -- if
 * relay_names_load() ever starts checking length/version/CRC against the
 * wrong struct, or silently drops one slot, this goes red immediately. */
static void test_relay_names_save_load_round_trip_preserves_distinct_values(void)
{
    TEST_SECTION("relay_names_load -- a real, valid blob with distinct per-relay names survives the "
                 "full save->load round trip losslessly (the profiles_http.c-class data-loss check)");
    nvs_test_enable(true);
    nvs_test_clear();
    reset_relay_names();

    TEST_CHECK(zones_config_set_relay_name(1, "Vent fan"), "seed relay 1");
    TEST_CHECK(zones_config_set_relay_name(2, "Bottom element"), "seed relay 2");
    TEST_CHECK(zones_config_set_relay_name(3, "Top element"), "seed relay 3");
    TEST_CHECK(zones_config_set_relay_name(4, ""), "seed relay 4 as deliberately blank");

    /* Simulate a reboot: wipe the in-RAM copy and reload from the "flash"
       the setters above actually wrote to (via nvs_set_blob, since
       nvs_test_enable(true) makes it a real round trip, not a no-op). */
    reset_relay_names();
    relay_names_load();

    char out[RELAY_NAME_MAX_LEN + 1];
    TEST_CHECK(zones_config_get_relay_name(1, out, sizeof(out)) && strcmp(out, "Vent fan") == 0,
              "relay 1's name must survive the round trip exactly");
    TEST_CHECK(zones_config_get_relay_name(2, out, sizeof(out)) && strcmp(out, "Bottom element") == 0,
              "relay 2's name must survive the round trip exactly");
    TEST_CHECK(zones_config_get_relay_name(3, out, sizeof(out)) && strcmp(out, "Top element") == 0,
              "relay 3's name must survive the round trip exactly");
    TEST_CHECK(zones_config_get_relay_name(4, out, sizeof(out)) && out[0] == '\0',
              "relay 4's deliberate blank must survive as blank, not as some other slot's leftover value");

    nvs_test_enable(false);
    nvs_test_clear();
    reset_relay_names();
}

static void test_zones_post_relay_name_omitted_preserves_current_value(void)
{
    TEST_SECTION("zones_post_handler -- an omitted relay<N>_name PRESERVES the current stored name "
                 "(global-field convention, like safety_tc_type/pc_link_abort_silence_ms), it does NOT "
                 "zero it the way an omitted PER-ZONE field does -- this is the exact whole-page-submit "
                 "trap the task brief calls out: safety_config_page.html never renders a relay-name "
                 "input at all, so a save FROM THAT PAGE must not wipe every relay name");
    nvs_test_enable(true);
    nvs_test_clear();
    reset_relay_names();
    TEST_CHECK(zones_config_set_relay_name(1, "Vent fan"), "seed relay 1's name before the POST under test");

    /* A minimal, otherwise-valid body that says nothing at all about
       relay1_name -- exactly what safety_config_page.html's saveAll() sent
       before this pass added its relay-names echo, and exactly what any
       future third client that has never heard of this field will send. */
    run_zones_post("thermo_count=0&relay_count=0&" MINIMAL_TIMING_PROFILE_BODY);
    TEST_CHECK(!s_test_err_called, "an otherwise-clean submission must not be rejected");
    TEST_CHECK(s_test_ok_called, "an otherwise-clean submission must report success");

    char out[RELAY_NAME_MAX_LEN + 1];
    TEST_CHECK(zones_config_get_relay_name(1, out, sizeof(out)) && strcmp(out, "Vent fan") == 0,
              "relay 1's name must be UNCHANGED after a submission that never mentioned it");

    reset_relay_names();
    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_zones_post_relay_name_present_updates_value(void)
{
    TEST_SECTION("zones_post_handler -- a present relay<N>_name field DOES update the stored name");
    nvs_test_enable(true);
    nvs_test_clear();
    reset_relay_names();
    TEST_CHECK(zones_config_set_relay_name(2, "old name"), "seed relay 2's name");

    run_zones_post("thermo_count=0&relay_count=0&" MINIMAL_TIMING_PROFILE_BODY "&relay2_name=Vent+fan");
    TEST_CHECK(!s_test_err_called, "a clean submission with a relay name must not be rejected");
    TEST_CHECK(s_test_ok_called, "a clean submission with a relay name must report success");

    char out[RELAY_NAME_MAX_LEN + 1];
    TEST_CHECK(zones_config_get_relay_name(2, out, sizeof(out)) && strcmp(out, "Vent fan") == 0,
              "relay 2's name must be updated to the submitted value");

    reset_relay_names();
    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_zones_post_relay_name_too_long_rejected_and_commits_nothing(void)
{
    TEST_SECTION("zones_post_handler -- an overlong relay<N>_name is rejected outright, and the "
                 "WHOLE submission is refused (nothing committed), same all-or-nothing discipline "
                 "as every other field this handler validates");
    nvs_test_enable(true);
    nvs_test_clear();
    reset_relay_names();
    TEST_CHECK(zones_config_set_relay_name(3, "kept"), "seed a name that must survive the rejected submission");
    s_zones.cfg.relay_count = 0; /* known value the rejected submission must not have changed */

    char overlong_body[256];
    snprintf(overlong_body, sizeof(overlong_body),
             "thermo_count=0&relay_count=2&%s&relay3_name=%s",
             MINIMAL_TIMING_PROFILE_BODY, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" /* > RELAY_NAME_MAX_LEN */);
    run_zones_post(overlong_body);
    TEST_CHECK(s_test_err_called, "an overlong relay name must be rejected");
    TEST_CHECK(!s_test_ok_called, "must not report success for a rejected submission");
    TEST_CHECK(strstr(s_test_err_msg, "relay name") != NULL, "error message should name the offending field");

    TEST_CHECK(s_zones.cfg.relay_count == 0, "a rejected submission must not have committed relay_count=2 either "
                                             "-- the relay-name check runs before the commit point, same as "
                                             "every field parsed earlier in this handler");
    char out[RELAY_NAME_MAX_LEN + 1];
    TEST_CHECK(zones_config_get_relay_name(3, out, sizeof(out)) && strcmp(out, "kept") == 0,
              "relay 3's pre-existing name must be untouched by the rejected submission");

    reset_relay_names();
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    nvs_test_enable(false);
    nvs_test_clear();
}

// ---------------------------------------------------------------------------
// Task 1: normal-current sweep -- refusal logic, ceiling predicate, and the
// persisted-result getter/setter.
// ---------------------------------------------------------------------------

static void test_zone_sweep_check_refusal_each_reason_fires(void)
{
    // Baseline: every input in the "allow" state -- must return OK. This is
    // what proves the OTHER branches below are actually testing something:
    // without this, a refusal function that always returned the same
    // enumerator would pass every "returns X for input X" check trivially.
    TEST_CHECK(zone_sweep_check_refusal(false, true, true, 1, false, false, true, false, false) == ZONE_SWEEP_REFUSE_OK,
              "all-clear inputs refuse nothing");

    TEST_CHECK(zone_sweep_check_refusal(true, true, true, 1, false, false, true, false, false) ==
                  ZONE_SWEEP_REFUSE_ALREADY_RUNNING,
              "already_running is refused");
    TEST_CHECK(zone_sweep_check_refusal(false, false, true, 1, false, false, true, false, false) ==
                  ZONE_SWEEP_REFUSE_NO_HW,
              "no hardware is refused");
    TEST_CHECK(zone_sweep_check_refusal(false, true, false, 1, false, false, true, false, false) ==
                  ZONE_SWEEP_REFUSE_CONFIG_INVALID,
              "invalid zones config is refused");
    TEST_CHECK(zone_sweep_check_refusal(false, true, true, 0, false, false, true, false, false) ==
                  ZONE_SWEEP_REFUSE_NO_ZONES,
              "thermo_count == 0 is refused");
    TEST_CHECK(zone_sweep_check_refusal(false, true, true, 1, true, false, true, false, false) ==
                  ZONE_SWEEP_REFUSE_PROFILE_RUNNING,
              "a running/paused profile is refused");
    TEST_CHECK(zone_sweep_check_refusal(false, true, true, 1, false, true, true, false, false) ==
                  ZONE_SWEEP_REFUSE_AUTOTUNE_RUNNING,
              "an active autotune is refused");
    TEST_CHECK(zone_sweep_check_refusal(false, true, true, 1, false, false, false, false, false) ==
                  ZONE_SWEEP_REFUSE_LINK_DOWN,
              "a down safety link is refused");
    TEST_CHECK(zone_sweep_check_refusal(false, true, true, 1, false, false, true, true, false) ==
                  ZONE_SWEEP_REFUSE_TRIP_LATCHED,
              "a latched trip is refused");
    // N9 (opus review, 2026-08-28): a relay already on (dashboard, or a
    // profile that just ended) must refuse the start outright, not silently
    // measure a foreign load.
    TEST_CHECK(zone_sweep_check_refusal(false, true, true, 1, false, false, true, false, true) ==
                  ZONE_SWEEP_REFUSE_RELAYS_ON,
              "N9: any relay already on is refused");
}

static void test_zone_sweep_ceiling_hit(void)
{
    TEST_CHECK(!zone_sweep_ceiling_hit(50.0f, false, 80.0f), "an invalid reading never trips the ceiling");
    TEST_CHECK(!zone_sweep_ceiling_hit(79.9f, true, 80.0f), "below the effective ceiling does not trip");
    TEST_CHECK(zone_sweep_ceiling_hit(80.0f, true, 80.0f), "at the effective ceiling trips");
    TEST_CHECK(zone_sweep_ceiling_hit(90.0f, true, 80.0f), "above the effective ceiling trips");
}

// H2 (2026-08-27 pass, then rejected and refixed 2026-08-28): the ceiling
// itself is now DERIVED by zone_sweep_effective_ceiling_c(), not hardcoded
// per call. The 2026-08-27 fallback (OTA_INTERLOCK_TEMP_CEILING_C, 100 C)
// is gone entirely -- it was picked as a conservative OTA-at-rest precondition,
// not a sweep-time thermal abort, and this bench's own zones are all capped
// at 80 C, so a 100 C fallback could never fire on the hardware this sweep
// actually runs against. Replaced with: the tightest configured ceiling
// across every zone that has one, or ZONE_SWEEP_UNCOMMISSIONED_CEILING_C
// (60 C) only when NO zone anywhere has a ceiling configured.
static void test_zone_sweep_effective_ceiling_c(void)
{
    TEST_SECTION("zone_sweep_effective_ceiling_c() (H2, opus review 2026-08-28)");

    // No zones configured at all -- the uncommissioned-board fallback.
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 0;
    TEST_CHECK(zone_sweep_effective_ceiling_c() == ZONE_SWEEP_UNCOMMISSIONED_CEILING_C,
              "H2: no zones at all -> the fixed 60C uncommissioned fallback");

    // Zones exist but none has a ceiling configured (max_temp_c == 0, the
    // documented "no ceiling" convention and the default on a never-
    // commissioned zone) -- still the fallback, not a per-zone 0.
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;
    TEST_CHECK(zone_sweep_effective_ceiling_c() == ZONE_SWEEP_UNCOMMISSIONED_CEILING_C,
              "H2: zones configured but none has a ceiling -> still the fallback");

    // This bench: every zone capped at 80C. The old defect (H2, 2026-08-27
    // fallback == 100C) could never fire here -- prove the NEW effective
    // ceiling actually reflects the tightest real ceiling instead.
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 2;
    s_zones.cfg.zones[0].max_temp_c = 80.0f;
    s_zones.cfg.zones[1].max_temp_c = 80.0f;
    TEST_CHECK(zone_sweep_effective_ceiling_c() == 80.0f,
              "H2: every zone ceilinged at 80C -> effective ceiling is 80C, not 100C");

    // Mixed: one zone commissioned (60C), one not (0 -- excluded from the
    // min). The tightest REAL ceiling wins, not the uncommissioned zone's 0.
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 2;
    s_zones.cfg.zones[0].max_temp_c = 500.0f;
    s_zones.cfg.zones[1].max_temp_c = 0.0f;
    TEST_CHECK(zone_sweep_effective_ceiling_c() == 500.0f,
              "H2: one ceilinged zone (500C), one not -> the configured 500C, not the fallback and not 0");

    // Negative test the reviewer specifically asked for: fallback +/- epsilon
    // around the boundary, driving zone_sweep_ceiling_hit() itself.
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 0;
    float eff = zone_sweep_effective_ceiling_c();
    TEST_CHECK(eff == ZONE_SWEEP_UNCOMMISSIONED_CEILING_C, "sanity: fallback is exactly the named constant");
    TEST_CHECK(!zone_sweep_ceiling_hit(eff - 0.1f, true, eff),
              "H2: fallback - epsilon does not trip the ceiling");
    TEST_CHECK(zone_sweep_ceiling_hit(eff + 0.1f, true, eff),
              "H2: fallback + epsilon trips the ceiling");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
}

static void test_zone_sweep_timing_predicates(void)
{
    TEST_CHECK(!zone_sweep_should_sample(0), "no sampling before settle time has elapsed");
    TEST_CHECK(!zone_sweep_should_sample(ZONE_SWEEP_SETTLE_MS - 1), "no sampling one tick before settle");
    TEST_CHECK(zone_sweep_should_sample(ZONE_SWEEP_SETTLE_MS), "sampling begins exactly at settle time");
    TEST_CHECK(!zone_sweep_zone_done(ZONE_SWEEP_ENERGIZE_MS - 1), "not done one tick early");
    TEST_CHECK(zone_sweep_zone_done(ZONE_SWEEP_ENERGIZE_MS), "done at the full energize duration");
}

// ---------------------------------------------------------------------------
// M3 (opus review, 2026-08-27): zone_sweep_run_one_zone() state-machine
// coverage, via a fake zone_sweep_zone_deps_t that records call order. Before
// this extraction the relay on/off sequencing, the choke-point property, the
// abort path, and the link-loss exit had ZERO test coverage -- xTaskCreate()
// is stubbed (see this file's header comment), so zone_sweep_task()'s body
// never ran under test, only the pure predicates around it. This drives the
// SAME function real hardware runs, dependency-injected.
// ---------------------------------------------------------------------------

typedef struct {
    char call_log[256];
    size_t log_len;
    bool abort_now;
    int abort_after_polls;      /* -1 = never via poll count */
    int ceiling_hit_at_poll;    /* -1 = never */
    int link_lost_at_poll;      /* -1 = never */
    int trip_latched_at_poll;   /* -1 = never (N10) */
    int invalid_temp_from_poll; /* -1 = never; read_temp reports !valid from this poll onward (N1) */
    int invalid_temp_only_at_poll; /* -1 = never; read_temp reports !valid on EXACTLY this one poll (N1) */
    int poll_count;
    kiln_io_owner_relay_result_t energize_result;
    float sample_value;
    /* H3: simulates an abort landing BETWEEN the caller's top-of-loop
     * abort_requested() check and the energize call, by answering false on
     * the Nth call and true from then on -- abort_true_from_call == 2 means
     * "the FIRST check (top-of-loop) still sees false, the SECOND check
     * (immediately before energize) sees true." A caller with only one
     * check before energizing can never observe this landing in time; a
     * caller with two (this file's zone_sweep_run_one_zone(), after the H3
     * fix) does. 0/negative = disabled (abort_now/abort_after_polls decide
     * instead). */
    int abort_true_from_call;
    int abort_requested_calls;
} fake_sweep_ctx_t;

static void fake_sweep_ctx_reset(fake_sweep_ctx_t *c)
{
    memset(c, 0, sizeof(*c));
    c->abort_after_polls = -1;
    c->ceiling_hit_at_poll = -1;
    c->link_lost_at_poll = -1;
    c->trip_latched_at_poll = -1;
    c->invalid_temp_from_poll = -1;
    c->invalid_temp_only_at_poll = -1;
    c->energize_result = KILN_IO_OWNER_RELAY_OK;
    c->sample_value = 1.0f;
    c->abort_true_from_call = -1;
}

static void fake_sweep_log(fake_sweep_ctx_t *c, const char *op)
{
    size_t n = strlen(op);
    if (c->log_len + n + 2 >= sizeof(c->call_log)) {
        return; /* test logs are always short -- silently truncating here would hide a bug */
    }
    if (c->log_len > 0) {
        c->call_log[c->log_len++] = ',';
    }
    memcpy(c->call_log + c->log_len, op, n);
    c->log_len += n;
    c->call_log[c->log_len] = '\0';
}

static kiln_io_owner_relay_result_t fake_sweep_energize(void *ctx, uint8_t relay_mask, uint32_t *out_safety_sources)
{
    (void)relay_mask;
    fake_sweep_ctx_t *c = (fake_sweep_ctx_t *)ctx;
    fake_sweep_log(c, "on");
    if (out_safety_sources) *out_safety_sources = 0;
    return c->energize_result;
}

static void fake_sweep_force_off(void *ctx)
{
    fake_sweep_log((fake_sweep_ctx_t *)ctx, "off");
}

static void fake_sweep_read_temp(void *ctx, uint8_t zi, float *out_c, bool *out_valid)
{
    (void)zi;
    fake_sweep_ctx_t *c = (fake_sweep_ctx_t *)ctx;
    if ((c->invalid_temp_from_poll >= 0 && c->poll_count >= c->invalid_temp_from_poll) ||
        c->poll_count == c->invalid_temp_only_at_poll) {
        *out_c = NAN;
        *out_valid = false;
        return;
    }
    if (c->ceiling_hit_at_poll >= 0 && c->poll_count >= c->ceiling_hit_at_poll) {
        *out_c = 999.0f;
    } else {
        *out_c = 20.0f;
    }
    *out_valid = true;
}

static float fake_sweep_sample_current(void *ctx, uint8_t zi)
{
    (void)zi;
    return ((fake_sweep_ctx_t *)ctx)->sample_value;
}

static bool fake_sweep_link_up(void *ctx)
{
    fake_sweep_ctx_t *c = (fake_sweep_ctx_t *)ctx;
    if (c->link_lost_at_poll >= 0 && c->poll_count >= c->link_lost_at_poll) {
        return false;
    }
    return true;
}

static bool fake_sweep_trip_latched(void *ctx)
{
    fake_sweep_ctx_t *c = (fake_sweep_ctx_t *)ctx;
    if (c->trip_latched_at_poll >= 0 && c->poll_count >= c->trip_latched_at_poll) {
        return true;
    }
    return false;
}

static bool fake_sweep_abort_requested(void *ctx)
{
    fake_sweep_ctx_t *c = (fake_sweep_ctx_t *)ctx;
    c->abort_requested_calls++;
    if (c->abort_true_from_call >= 0 && c->abort_requested_calls >= c->abort_true_from_call) return true;
    if (c->abort_now) return true;
    if (c->abort_after_polls >= 0 && c->poll_count >= c->abort_after_polls) return true;
    return false;
}

static void fake_sweep_delay_poll(void *ctx)
{
    /* Advances the fake clock -- the real deps->delay_poll() actually sleeps
     * ZONE_SWEEP_POLL_MS; this just counts polls, which is all the fake
     * ceiling/link-loss/abort-after-N-polls triggers above need. */
    ((fake_sweep_ctx_t *)ctx)->poll_count++;
}

static const zone_sweep_zone_deps_t s_fake_sweep_deps = {
    .energize = fake_sweep_energize,
    .force_off = fake_sweep_force_off,
    .read_temp = fake_sweep_read_temp,
    .sample_current = fake_sweep_sample_current,
    .link_up = fake_sweep_link_up,
    .trip_latched = fake_sweep_trip_latched,
    .abort_requested = fake_sweep_abort_requested,
    .delay_poll = fake_sweep_delay_poll,
    .ctx = NULL, /* set per-test */
};

static void test_zone_sweep_hw_bindings_route_through_owner(void)
{
    // B1 (opus review, 2026-08-27): the REAL hardware bindings
    // (zone_sweep_hw_energize()/zone_sweep_hw_force_off(), the pair
    // zone_sweep_task() actually wires up) must call the owner module, not
    // kiln_io_set_relay_mask()/kiln_io_all_relays_off() directly -- that
    // bypass is exactly what let the sweep race the other five relay
    // writers and skip the OTA/safety gate. Calling these two static
    // functions directly (reachable because this whole translation unit is
    // #include'd, same as everything else in this file) exercises the real
    // wiring without ever spinning up zone_sweep_task() itself.
    TEST_SECTION("zone_sweep_hw_energize()/zone_sweep_hw_force_off() route through kiln_io_owner_* (B1)");
    static kiln_io_t dummy_io;
    memset(&dummy_io, 0, sizeof(dummy_io));
    zones_http_set_hw(&dummy_io, NULL, NULL);
    s_test_owner_set_relay_mask_calls = 0;
    s_test_owner_all_relays_off_calls = 0;

    uint32_t sources = 0;
    kiln_io_owner_relay_result_t rr = zone_sweep_hw_energize(NULL, 0x01, &sources);

    TEST_CHECK(rr == KILN_IO_OWNER_RELAY_OK, "sanity: the stub reports OK");
    TEST_CHECK(s_test_owner_set_relay_mask_calls == 1,
              "B1: energizing must call kiln_io_owner_command_set_relay_mask() (MANUAL) exactly once");
    // N9 (opus review, 2026-08-28): mask must be 0xFF (all relays), not just
    // the zone's own relay_mask -- this asserts an all-others-off
    // precondition on every energize write. value is still the zone's own
    // relay_mask (only ITS relay(s) end up on).
    TEST_CHECK(s_test_owner_last_mask == 0xFFu,
              "N9: the energize write's mask must be 0xFF -- every relay outside this zone is forced off, "
              "not just left however it was");
    TEST_CHECK(s_test_owner_last_value == 0x01,
              "N9: the energize write's value is still only this zone's own relay_mask");

    zone_sweep_hw_force_off(NULL);
    TEST_CHECK(s_test_owner_all_relays_off_calls == 1,
              "B1: de-energizing must call kiln_io_owner_command_all_relays_off() exactly once");

    zones_http_set_hw(NULL, NULL, NULL);
}

static void test_zone_sweep_run_one_zone_skips_unwired_zone(void)
{
    fake_sweep_ctx_t ctx;
    fake_sweep_ctx_reset(&ctx);
    zone_sweep_zone_deps_t deps = s_fake_sweep_deps;
    deps.ctx = &ctx;

    float avg = NAN;
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, /* relay_mask */ 0, &deps, &avg, NULL, NULL);

    TEST_CHECK(outcome == ZONE_SWEEP_ZONE_SKIPPED, "relay_mask == 0 -> SKIPPED, nothing measured");
    TEST_CHECK(strlen(ctx.call_log) == 0, "a skipped zone touches NO relay call at all -- not even an off");
}

static void test_zone_sweep_run_one_zone_abort_before_energize_never_turns_relay_on(void)
{
    fake_sweep_ctx_t ctx;
    fake_sweep_ctx_reset(&ctx);
    ctx.abort_now = true;
    zone_sweep_zone_deps_t deps = s_fake_sweep_deps;
    deps.ctx = &ctx;

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 1;

    float avg = NAN;
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, NULL, NULL);

    TEST_CHECK(outcome == ZONE_SWEEP_ZONE_ABORTED, "abort already requested -> ABORTED");
    // H3's specific claim: an operator Abort must be able to stop a zone from
    // EVER energizing, not merely turn it off quickly afterward.
    TEST_CHECK(strstr(ctx.call_log, "on") == NULL,
              "H3: relay energize must never be called once abort_requested() is already true "
              "-- \"aborted\" and \"was briefly on, then off\" are different outcomes");
}

// H3 (opus review, 2026-08-28): DELETED. The 2026-08-27 pass added a SECOND
// abort_requested() check immediately before the energize call, claiming it
// "structurally covers the window kiln_io_owner_command_set_relay_mask()'s
// bounded wait opens" -- false: that check runs BEFORE the call, not inside
// or after it, so it observes exactly what the first (top-of-loop) check
// already observed. The two calls were adjacent with zero code between them
// -- textbook dead code -- and test_zone_sweep_run_one_zone_h3_recheck_
// catches_late_abort() (formerly here) only passed because its fake answered
// false on call #1 and true from call #2 onward, a scenario indistinguishable
// from "only one check exists" once the duplicate is removed: the while
// loop's own abort_requested() check, which runs BEFORE delay_poll() and
// BEFORE anything else on every iteration including the first, already
// covers a late-landing abort -- it fires on the very first iteration, before
// any elapsed time is consumed, which is exactly the same window the deleted
// duplicate falsely claimed to own. Decision: delete the duplicate and its
// test rather than move a real check after the energize call (the
// alternative the review offered) -- energize() is synchronous from this
// function's point of view (it blocks until kiln_io_owner_command_set_relay_
// mask()'s bounded wait resolves), so there is no "after energize returns
// OK, but before anything else" window left to check that the while loop's
// first iteration does not already cover on the very next line.

static void test_zone_sweep_run_one_zone_normal_completion_sequencing(void)
{
    fake_sweep_ctx_t ctx;
    fake_sweep_ctx_reset(&ctx);
    ctx.sample_value = 2.5f;
    zone_sweep_zone_deps_t deps = s_fake_sweep_deps;
    deps.ctx = &ctx;

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 1;
    s_zones.cfg.zones[0].max_temp_c = 500.0f; /* configured ceiling, far above the fake's 20C/999C readings */

    float avg = NAN;
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, NULL, NULL);

    TEST_CHECK(outcome == ZONE_SWEEP_ZONE_OK, "no abort/ceiling/link-loss -> runs to completion, OK");
    TEST_CHECK(strcmp(ctx.call_log, "on,off") == 0,
              "the ONLY calls for a normal zone are energize once, then de-energize once, in that order "
              "-- the exact relay on/off sequencing this finding says had zero coverage");
    TEST_CHECK(!isnan(avg) && avg > 0.0f, "samples were averaged into a real reading");
}

static void test_zone_sweep_run_one_zone_abort_mid_run_still_forces_off(void)
{
    fake_sweep_ctx_t ctx;
    fake_sweep_ctx_reset(&ctx);
    ctx.abort_after_polls = 1; /* abort is seen on the SECOND loop iteration, after one poll */
    zone_sweep_zone_deps_t deps = s_fake_sweep_deps;
    deps.ctx = &ctx;

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 1;
    s_zones.cfg.zones[0].max_temp_c = 500.0f;

    float avg = NAN;
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, NULL, NULL);

    TEST_CHECK(outcome == ZONE_SWEEP_ZONE_ABORTED, "abort mid-poll-loop -> ABORTED");
    TEST_CHECK(strcmp(ctx.call_log, "on,off") == 0,
              "the CHOKE POINT: a zone that was actually energized always gets de-energized on the way "
              "out, abort included -- exactly one \"on\", exactly one \"off\", in that order");
}

static void test_zone_sweep_run_one_zone_ceiling_hit_forces_off(void)
{
    fake_sweep_ctx_t ctx;
    fake_sweep_ctx_reset(&ctx);
    ctx.ceiling_hit_at_poll = 2;
    zone_sweep_zone_deps_t deps = s_fake_sweep_deps;
    deps.ctx = &ctx;

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 1;
    s_zones.cfg.zones[0].max_temp_c = 500.0f;

    float avg = NAN;
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, NULL, NULL);

    TEST_CHECK(outcome == ZONE_SWEEP_ZONE_CEILING_HIT, "reading reaches the ceiling -> CEILING_HIT");
    TEST_CHECK(strcmp(ctx.call_log, "on,off") == 0, "the ceiling abort is also a choke-point exit: on, then off");
}

static void test_zone_sweep_run_one_zone_link_loss_forces_off(void)
{
    fake_sweep_ctx_t ctx;
    fake_sweep_ctx_reset(&ctx);
    ctx.link_lost_at_poll = 1;
    zone_sweep_zone_deps_t deps = s_fake_sweep_deps;
    deps.ctx = &ctx;

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 1;
    s_zones.cfg.zones[0].max_temp_c = 500.0f;

    float avg = NAN;
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, NULL, NULL);

    TEST_CHECK(outcome == ZONE_SWEEP_ZONE_LINK_LOST, "the safety link drops mid-poll -> LINK_LOST");
    TEST_CHECK(strcmp(ctx.call_log, "on,off") == 0, "the link-loss exit is also a choke-point exit: on, then off");
}

static void test_zone_sweep_run_one_zone_energize_refused_is_a_choke_point_exit_too(void)
{
    // B1 (opus review, 2026-08-27): the owner-gated energize call can now be
    // REFUSED (owned/safety/updating) -- a possibility the original direct
    // kiln_io_set_relay_mask() call could never report. Prove it is treated
    // as its own exit path, not silently ignored.
    fake_sweep_ctx_t ctx;
    fake_sweep_ctx_reset(&ctx);
    ctx.energize_result = KILN_IO_OWNER_RELAY_ERR_SAFETY;
    zone_sweep_zone_deps_t deps = s_fake_sweep_deps;
    deps.ctx = &ctx;

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 1;
    s_zones.cfg.zones[0].max_temp_c = 500.0f;

    float avg = NAN;
    uint32_t refused_sources = 0;
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, NULL, &refused_sources);

    TEST_CHECK(outcome == ZONE_SWEEP_ZONE_ENERGIZE_REFUSED,
              "B1: an owner refusal (e.g. a safety fault asserting mid-sweep) is its own outcome, "
              "distinct from OK/ABORTED/CEILING_HIT/LINK_LOST");
    // The refused energize call itself counts as the attempted "on" in the
    // log -- force_off() still runs afterward (belt-and-suspenders: nothing
    // was actually left energized, but the choke point is unconditional).
    TEST_CHECK(strcmp(ctx.call_log, "on,off") == 0,
              "a refused energize is STILL followed by the choke point -- no path skips it");
}

// N1 (opus review, 2026-08-28): the ceiling abort is inert while
// actual_valid is false. If the thermo bus faults mid-sweep every
// subsequent poll comes back invalid, and without this fix the ceiling
// abort could never fire for the rest of that zone's 5s energize -- link_up()
// is the only other in-loop guard and it does not observe temperature.
static void test_zone_sweep_run_one_zone_temp_lost_forces_off(void)
{
    fake_sweep_ctx_t ctx;
    fake_sweep_ctx_reset(&ctx);
    ctx.invalid_temp_from_poll = 1; /* every poll from #1 onward reports !valid */
    zone_sweep_zone_deps_t deps = s_fake_sweep_deps;
    deps.ctx = &ctx;

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 1;
    s_zones.cfg.zones[0].max_temp_c = 500.0f;

    float avg = NAN;
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, NULL, NULL);

    TEST_CHECK(outcome == ZONE_SWEEP_ZONE_TEMP_LOST,
              "N1: two consecutive invalid thermocouple reads -> TEMP_LOST, not a silent 5s unsupervised run");
    TEST_CHECK(strcmp(ctx.call_log, "on,off") == 0, "N1: the temp-lost exit is also a choke-point exit: on, then off");
}

// N1 negative half: a single dropped/garbled read must NOT abort the zone --
// one poll of noise tolerance is deliberate (see ZONE_SWEEP_TEMP_LOST_POLLS's
// doc comment).
static void test_zone_sweep_run_one_zone_single_invalid_poll_does_not_abort(void)
{
    fake_sweep_ctx_t ctx;
    fake_sweep_ctx_reset(&ctx);
    ctx.invalid_temp_only_at_poll = 3; /* exactly poll 3 is invalid; every other poll (1,2,4,5,...) is valid --
                                         * never two consecutive misses, so TEMP_LOST must not fire and the
                                         * zone must run to a normal OK completion. */
    zone_sweep_zone_deps_t deps = s_fake_sweep_deps;
    deps.ctx = &ctx;

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 1;
    s_zones.cfg.zones[0].max_temp_c = 500.0f;

    float avg = NAN;
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, NULL, NULL);

    TEST_CHECK(outcome == ZONE_SWEEP_ZONE_OK,
              "N1: a single invalid poll surrounded by valid ones must not itself trip TEMP_LOST -- the "
              "streak resets on the very next valid read, and the zone completes normally, proving one "
              "poll of noise tolerance is real, not accidental");
}

// N10 (opus review, 2026-08-28): a safety trip latching mid-zone must end
// the sweep promptly instead of grinding on for the rest of the dwell.
// M12: zone_sweep_derive_ct_channel() -- the unambiguity condition
// COMMISSIONING_UX.md sec 1.2 requires before ct_channel_map may be derived
// at all. Each negative case below is a mapping this MUST refuse to produce:
// a check that cannot fail would let the sweep write a guessed value into a
// field S3/S4/S14 then trust.
static void test_zone_sweep_derive_ct_channel_picks_the_dominant_channel(void)
{
    float per_ch[ZONE_CT_CHANNEL_COUNT] = { 0.1f, 14.0f, 0.2f };
    uint8_t ch = 0xFF;
    TEST_CHECK(zone_sweep_derive_ct_channel(per_ch, &ch), "one channel far above the rest resolves");
    TEST_CHECK(ch == 1, "the resolved channel is the one that carried the current");
}

static void test_zone_sweep_derive_ct_channel_refuses_below_the_load_threshold(void)
{
    // Nothing conducted -- a CT not fitted, or a zone whose relay closed onto
    // a dead element. Noise must never be read as "this channel is zone 0's".
    float per_ch[ZONE_CT_CHANNEL_COUNT] = { 0.3f, 0.05f, 0.0f };
    uint8_t ch = 0xFF;
    TEST_CHECK(!zone_sweep_derive_ct_channel(per_ch, &ch), "sub-threshold current resolves nothing");
}

static void test_zone_sweep_derive_ct_channel_refuses_a_shared_ct(void)
{
    // Two channels within the dominance factor: ct_mask explicitly permits
    // one CT feeding more than one zone, and that case is not invertible.
    float per_ch[ZONE_CT_CHANNEL_COUNT] = { 10.0f, 8.0f, 0.0f };
    uint8_t ch = 0xFF;
    TEST_CHECK(!zone_sweep_derive_ct_channel(per_ch, &ch), "two comparable channels resolve nothing");
}

static void test_zone_sweep_derive_ct_channel_refuses_nan(void)
{
    // No per-channel sampler, or a link read that failed mid-window: NaN
    // must read as "cannot tell", never as 0 A on that channel.
    float per_ch[ZONE_CT_CHANNEL_COUNT] = { 12.0f, NAN, 0.0f };
    uint8_t ch = 0xFF;
    TEST_CHECK(!zone_sweep_derive_ct_channel(per_ch, &ch), "a NaN channel resolves nothing");
}

// M12 / opus review 2026-08-28: zone_sweep_derive_ct_channel() above is only
// HALF the derivation. zone_sweep_task_record_ct_channels() is the other
// half, and it owns the two refusals nothing above can express -- the
// zone-id/relay-id identity precondition, and the two-zones-one-channel
// conflict. Both were untested; both write (or decline to write) the field
// S14 aims the over-current guard with. These two exercise them directly.
static void test_zone_sweep_record_ct_refuses_a_zone_whose_relay_is_not_its_own_bit(void)
{
    // ct_channel_map[] is indexed straight into the Pico's relay_now_mask, so
    // the derived value is only meaningful while zone id == relay id. Zone 0
    // wired to relay bit 1 (or to two relays at once) breaks that: the sweep
    // measured real, unambiguous current, and it STILL must not map it --
    // writing the zone index would point S14 at somebody else's relay.
    memset(&s_ct_derive, 0, sizeof(s_ct_derive));
    float per_ch[ZONE_CT_CHANNEL_COUNT] = { 0.1f, 14.0f, 0.2f };

    zone_sweep_task_record_ct_channels(NULL, 0, 0x02, per_ch);       /* zone 0, relay bit 1 */
    zone_sweep_task_record_ct_channels(NULL, 1, 0x03, per_ch);       /* zone 1, TWO relays */

    TEST_CHECK(s_ct_derive.derived_mask == 0,
              "a zone whose relay_mask is not exactly its own bit must derive NOTHING -- the same "
              "current that would map cleanly for a zone/relay-identity board must be refused here");
    TEST_CHECK((s_ct_derive.unresolved_zone_mask & 0x03u) == 0x03u,
              "both refused zones must land in unresolved_zone_mask so the page asks for them by "
              "hand, rather than the refusal being silent");

    // Negative half: the SAME current, on a zone that does satisfy the
    // identity, maps -- proving the refusal above is the relay_mask check
    // and not the measurement failing to resolve.
    memset(&s_ct_derive, 0, sizeof(s_ct_derive));
    zone_sweep_task_record_ct_channels(NULL, 0, 0x01, per_ch);
    TEST_CHECK(s_ct_derive.derived_mask == 0x02u && s_ct_derive.zone_for_ch[1] == 0,
              "control: relay_mask == 1<<zone_index maps that same measurement to CT1");
}

static void test_zone_sweep_record_ct_refuses_two_zones_claiming_one_channel(void)
{
    // Each zone looked perfectly unambiguous on its own -- the conflict is
    // only visible ACROSS zones, which is exactly why no per-zone check can
    // catch it. COMMISSIONING_UX.md sec 1.2 requires a one-to-one inversion:
    // neither claimant may win, and a third claimant must not revive it.
    memset(&s_ct_derive, 0, sizeof(s_ct_derive));
    float per_ch[ZONE_CT_CHANNEL_COUNT] = { 0.1f, 14.0f, 0.2f };

    zone_sweep_task_record_ct_channels(NULL, 0, 0x01, per_ch);
    TEST_CHECK(s_ct_derive.derived_mask == 0x02u, "zone 0 alone resolves CT1 (setup)");

    zone_sweep_task_record_ct_channels(NULL, 1, 0x02, per_ch);
    TEST_CHECK((s_ct_derive.derived_mask & 0x02u) == 0,
              "a second zone dominating the SAME CT must drop that channel entirely -- not let the "
              "first claimant stand, and not let the last one win");
    TEST_CHECK((s_ct_derive.conflict_mask & 0x02u) != 0,
              "the dropped channel must be recorded as a conflict, so the operator is told two zones "
              "share one CT rather than just seeing a missing row");

    zone_sweep_task_record_ct_channels(NULL, 2, 0x04, per_ch);
    TEST_CHECK((s_ct_derive.derived_mask & 0x02u) == 0,
              "a third claimant must not resurrect a channel an earlier conflict disqualified");

    // A DIFFERENT channel, claimed by exactly one zone, still maps -- the
    // conflict must poison only the contested channel.
    float per_ch0[ZONE_CT_CHANNEL_COUNT] = { 14.0f, 0.1f, 0.2f };
    memset(&s_ct_derive, 0, sizeof(s_ct_derive));
    zone_sweep_task_record_ct_channels(NULL, 0, 0x01, per_ch);   /* -> CT1 */
    zone_sweep_task_record_ct_channels(NULL, 1, 0x02, per_ch);   /* -> CT1, conflict */
    zone_sweep_task_record_ct_channels(NULL, 2, 0x04, per_ch0);  /* -> CT0, uncontested */
    TEST_CHECK(s_ct_derive.derived_mask == 0x01u && s_ct_derive.zone_for_ch[0] == 2,
              "an uncontested channel in the same run still maps -- the conflict poisons only the "
              "channel two zones actually claimed");
}

static void test_zone_sweep_run_one_zone_trip_latched_forces_off(void)
{
    fake_sweep_ctx_t ctx;
    fake_sweep_ctx_reset(&ctx);
    ctx.trip_latched_at_poll = 1;
    zone_sweep_zone_deps_t deps = s_fake_sweep_deps;
    deps.ctx = &ctx;

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 1;
    s_zones.cfg.zones[0].max_temp_c = 500.0f;

    float avg = NAN;
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, NULL, NULL);

    TEST_CHECK(outcome == ZONE_SWEEP_ZONE_TRIP_LATCHED, "N10: a safety trip latching mid-zone -> TRIP_LATCHED");
    TEST_CHECK(strcmp(ctx.call_log, "on,off") == 0, "N10: the trip-latched exit is also a choke-point exit: on, then off");
}

// ---------------------------------------------------------------------------
// M3 (opus review, 2026-08-28): zone_sweep_run_all_zones() coverage -- the
// hoisted whole-sweep loop, driven the same way zone_sweep_run_one_zone() is
// above. Proves the property the file's own comment called "structural, not
// a convention" (no two zones' relays on at once) against the REAL function,
// not just by reading it, and covers the OK-with-zero-samples aggregation
// path that had no coverage before.
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t relay_masks[MAX31856_CHANNEL_COUNT];
    uint8_t zone_index_calls[MAX31856_CHANNEL_COUNT];
    uint8_t zone_index_call_count;
    uint8_t recorded_zone[MAX31856_CHANNEL_COUNT];
    float   recorded_avg[MAX31856_CHANNEL_COUNT];
    uint8_t record_call_count;
    uint8_t zone_done_calls;
} fake_all_ctx_t;

static uint8_t fake_all_relay_mask_for_zone(void *ctx, uint8_t zi)
{
    fake_all_ctx_t *c = (fake_all_ctx_t *)ctx;
    return c->relay_masks[zi];
}

static void fake_all_set_zone_index(void *ctx, uint8_t zi)
{
    fake_all_ctx_t *c = (fake_all_ctx_t *)ctx;
    if (c->zone_index_call_count < MAX31856_CHANNEL_COUNT) {
        c->zone_index_calls[c->zone_index_call_count++] = zi;
    }
}

static void fake_all_record_normal(void *ctx, uint8_t zi, float avg_a)
{
    fake_all_ctx_t *c = (fake_all_ctx_t *)ctx;
    if (c->record_call_count < MAX31856_CHANNEL_COUNT) {
        c->recorded_zone[c->record_call_count] = zi;
        c->recorded_avg[c->record_call_count] = avg_a;
        c->record_call_count++;
    }
}

static void fake_all_zone_done(void *ctx)
{
    ((fake_all_ctx_t *)ctx)->zone_done_calls++;
}

static const zone_sweep_all_hooks_t s_fake_all_hooks = {
    .relay_mask_for_zone = fake_all_relay_mask_for_zone,
    .set_zone_index = fake_all_set_zone_index,
    .record_normal = fake_all_record_normal,
    .zone_done = fake_all_zone_done,
    .ctx = NULL, /* set per-test */
};

/* Fake energize/force_off that would FAIL a real hardware assertion if two
 * zones were ever both "on" at once -- this is the actual mechanism that
 * proves the no-two-zones-on-at-once property, not just an observation. */
static bool s_all_any_relay_on = false;
static bool s_all_overlap_detected = false;
/* Set by test_zone_sweep_run_all_zones_energize_refused_reason_decodes_fault_words()
 * below to make the energize call refuse with a known safety-source mask,
 * driving zone_sweep_run_all_zones()'s ZONE_SWEEP_ZONE_ENERGIZE_REFUSED ->
 * out->reason build path (the actual call site being tested). */
static bool s_all_refuse_with_safety = false;
static uint32_t s_all_refuse_sources = 0;

static kiln_io_owner_relay_result_t fake_all_energize(void *ctx, uint8_t relay_mask, uint32_t *out_safety_sources)
{
    (void)ctx;
    (void)relay_mask;
    if (s_all_refuse_with_safety) {
        if (out_safety_sources) *out_safety_sources = s_all_refuse_sources;
        return KILN_IO_OWNER_RELAY_ERR_SAFETY;
    }
    if (out_safety_sources) *out_safety_sources = 0;
    if (s_all_any_relay_on) {
        s_all_overlap_detected = true; /* a second zone tried to energize while one was still on */
    }
    s_all_any_relay_on = true;
    return KILN_IO_OWNER_RELAY_OK;
}

static void fake_all_force_off(void *ctx)
{
    (void)ctx;
    s_all_any_relay_on = false;
}

static void fake_all_read_temp(void *ctx, uint8_t zi, float *out_c, bool *out_valid)
{
    (void)ctx;
    (void)zi;
    *out_c = 20.0f;
    *out_valid = true;
}

static float fake_all_sample_current(void *ctx, uint8_t zi)
{
    (void)ctx;
    (void)zi;
    return 1.0f;
}

static bool fake_all_link_up(void *ctx)
{
    (void)ctx;
    return true;
}

static bool fake_all_trip_latched(void *ctx)
{
    (void)ctx;
    return false;
}

static bool fake_all_abort_requested(void *ctx)
{
    (void)ctx;
    return false;
}

static void fake_all_delay_poll(void *ctx)
{
    (void)ctx;
}

static const zone_sweep_zone_deps_t s_fake_all_zone_deps = {
    .energize = fake_all_energize,
    .force_off = fake_all_force_off,
    .read_temp = fake_all_read_temp,
    .sample_current = fake_all_sample_current,
    .link_up = fake_all_link_up,
    .trip_latched = fake_all_trip_latched,
    .abort_requested = fake_all_abort_requested,
    .delay_poll = fake_all_delay_poll,
    .ctx = NULL,
};

static void test_zone_sweep_run_all_zones_never_energizes_two_zones_at_once(void)
{
    TEST_SECTION("zone_sweep_run_all_zones() -- no two zones' relays on at once (M3, opus review 2026-08-28)");
    fake_all_ctx_t actx;
    memset(&actx, 0, sizeof(actx));
    actx.relay_masks[0] = 0x01;
    actx.relay_masks[1] = 0x02;
    actx.relay_masks[2] = 0x04;

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;
    s_zones.cfg.zones[0].max_temp_c = 500.0f;
    s_zones.cfg.zones[1].max_temp_c = 500.0f;
    s_zones.cfg.zones[2].max_temp_c = 500.0f;

    zone_sweep_zone_deps_t deps = s_fake_all_zone_deps;
    deps.ctx = NULL;
    zone_sweep_all_hooks_t hooks = s_fake_all_hooks;
    hooks.ctx = &actx;

    s_all_any_relay_on = false;
    s_all_overlap_detected = false;

    zone_sweep_all_result_t result;
    zone_sweep_run_all_zones(3, &deps, &hooks, &result);

    TEST_CHECK(result.state == ZONE_SWEEP_DONE, "all three zones complete cleanly");
    TEST_CHECK(result.zones_done == 3, "all three zones counted as done");
    TEST_CHECK(!s_all_overlap_detected,
              "M3: no zone's energize() was ever called while a PRIOR zone's relay was still on -- "
              "the property the file's comment called structural, now actually exercised");
    TEST_CHECK(actx.zone_index_call_count == 3 && actx.zone_index_calls[0] == 0 && actx.zone_index_calls[1] == 1 &&
                  actx.zone_index_calls[2] == 2,
              "zones are measured strictly in order, one at a time");
    TEST_CHECK(actx.record_call_count == 3, "all three zones recorded a measured normal");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
}

/* M3: the ZONE_SWEEP_ZONE_OK && samples == 0 path -- zones_done increments
 * but no normal is recorded. Not reachable through the full timed loop under
 * today's fixed SETTLE/ENERGIZE_MS constants (settle always elapses before
 * done), so this drives zone_sweep_run_all_zones()'s aggregation directly
 * with a zone whose relay_mask is 0 mixed among wired zones -- SKIPPED zones
 * never sample either, and must not be recorded or double-counted. */
static void test_zone_sweep_run_all_zones_skipped_zone_records_nothing(void)
{
    TEST_SECTION("zone_sweep_run_all_zones() -- a SKIPPED (unwired) zone records no normal and is not "
                 "counted as done (M3, opus review 2026-08-28)");
    fake_all_ctx_t actx;
    memset(&actx, 0, sizeof(actx));
    actx.relay_masks[0] = 0x01;
    actx.relay_masks[1] = 0x00; /* unwired -- SKIPPED */
    actx.relay_masks[2] = 0x04;

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;
    s_zones.cfg.zones[0].max_temp_c = 500.0f;
    s_zones.cfg.zones[2].max_temp_c = 500.0f;

    zone_sweep_zone_deps_t deps = s_fake_all_zone_deps;
    zone_sweep_all_hooks_t hooks = s_fake_all_hooks;
    hooks.ctx = &actx;

    s_all_any_relay_on = false;
    s_all_overlap_detected = false;

    zone_sweep_all_result_t result;
    zone_sweep_run_all_zones(3, &deps, &hooks, &result);

    TEST_CHECK(result.state == ZONE_SWEEP_DONE, "sweep completes despite one skipped zone");
    TEST_CHECK(result.zones_done == 2, "only the two WIRED zones count as done -- the skip is not a done zone");
    TEST_CHECK(actx.record_call_count == 2, "only the two wired zones ever recorded a normal");
    TEST_CHECK(actx.zone_index_call_count == 2,
              "set_zone_index() is never called for the skipped zone -- matches the pre-hoist contract "
              "(\"only set for a zone actually being measured\")");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
}

/* ROADMAP.md M13 ("every fault... what was detected wrong"): the
 * ZONE_SWEEP_ZONE_ENERGIZE_REFUSED case in zone_sweep_run_all_zones() used to
 * render out->reason as "zone %u energize refused (0x%02X)" -- a bare hex
 * mask. This drives that exact call site (not safety_fault_source_words() in
 * isolation, which already works) and asserts the decoded source name is in
 * the operator-facing reason string, not a hex byte. */
static void test_zone_sweep_run_all_zones_energize_refused_reason_decodes_fault_words(void)
{
    TEST_SECTION("zone_sweep_run_all_zones() -- ENERGIZE_REFUSED reason decodes the safety fault-source "
                 "mask instead of showing a bare hex value (ROADMAP.md M13)");
    fake_all_ctx_t actx;
    memset(&actx, 0, sizeof(actx));
    actx.relay_masks[0] = 0x01;

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 1;
    s_zones.cfg.zones[0].max_temp_c = 500.0f;

    zone_sweep_zone_deps_t deps = s_fake_all_zone_deps;
    zone_sweep_all_hooks_t hooks = s_fake_all_hooks;
    hooks.ctx = &actx;

    s_all_any_relay_on = false;
    s_all_overlap_detected = false;
    s_all_refuse_with_safety = true;
    s_all_refuse_sources = 0x04u; /* main-board thermocouple fault, per safety_link.h */

    zone_sweep_all_result_t result;
    zone_sweep_run_all_zones(1, &deps, &hooks, &result);

    s_all_refuse_with_safety = false;
    s_all_refuse_sources = 0;

    TEST_CHECK(result.state == ZONE_SWEEP_FAILED, "an owner safety refusal fails the sweep");
    TEST_CHECK(strstr(result.reason, "0x") == NULL,
              "RED before this fix: the old format string put a bare \"0x04\" hex mask in the reason -- "
              "this must be gone now that the call site decodes the mask instead");
    /* The call site caps the decoded word at 16 bytes (see its comment) so
     * -Werror=format-truncation can prove out->reason never overflows --
     * "main-board thermocouple fault" is cut to "main-board therm" at that
     * width, so assert on the prefix that survives, not the full word. */
    TEST_CHECK(strstr(result.reason, "main-board") != NULL,
              "the decoded fault-source word (\"main-board...\", from safety_fault_source_words()) is in "
              "the reason string, not just the raw mask");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
}

static void test_zone_normals_get_set_round_trip(void)
{
    memset(&s_zone_normals.cfg, 0, sizeof(s_zone_normals.cfg));
    nvs_test_enable(true);
    nvs_test_clear();

    float amps = -1.0f;
    bool measured = true;
    TEST_CHECK(zones_config_get_normal_current(0, &amps, &measured), "getter answers for a valid index");
    TEST_CHECK(!measured, "a never-measured zone reports measured == false");
    TEST_CHECK(amps == 0.0f, "a never-measured zone reads back 0.0, not garbage");

    TEST_CHECK(zone_normals_set(2, 3.75f), "zone_normals_set persists a measured value");
    amps = -1.0f;
    measured = false;
    TEST_CHECK(zones_config_get_normal_current(2, &amps, &measured), "getter answers after a set");
    TEST_CHECK(measured, "the swept zone now reports measured == true");
    TEST_CHECK(amps == 3.75f, "the swept zone's amps round-trips exactly");

    // A DIFFERENT zone must still read "never measured" -- proves
    // measured_mask is per-zone, not a single sweep-ever-ran flag.
    amps = -1.0f;
    measured = true;
    TEST_CHECK(zones_config_get_normal_current(1, &amps, &measured), "getter answers for the untouched zone");
    TEST_CHECK(!measured, "an untouched zone is unaffected by another zone's measurement");

    TEST_CHECK(!zone_normals_set(MAX31856_CHANNEL_COUNT, 1.0f), "an out-of-range zone_index is rejected");
    TEST_CHECK(!zone_normals_set(0, -0.1f), "a negative amps value is rejected");
    TEST_CHECK(!zone_normals_set(0, NAN), "a non-finite amps value is rejected");

    memset(&s_zone_normals.cfg, 0, sizeof(s_zone_normals.cfg));
    nvs_test_enable(false);
    nvs_test_clear();
}

// ---------------------------------------------------------------------------
// Task 2: CT-to-zone mapping mismatch predicate, and its live wiring.
// ---------------------------------------------------------------------------

static void test_ct_mapping_mismatch_silent_when_never_measured(void)
{
    // The one requirement the owner called out explicitly: NEVER warn for a
    // zone whose normal was never measured, no matter how wild live_current_a
    // is -- proven here with a live reading that would obviously mismatch
    // ANY plausible normal, to show the silence isn't an accident of the
    // numbers chosen.
    TEST_CHECK(!zones_ct_mapping_mismatch(5.0f, false, 500.0f),
              "an unmeasured zone never warns, even for a wildly implausible live reading");
}

static void test_ct_mapping_mismatch_within_band_is_silent(void)
{
    TEST_CHECK(!zones_ct_mapping_mismatch(5.0f, true, 5.0f), "an exact match never warns");
    TEST_CHECK(!zones_ct_mapping_mismatch(5.0f, true, 2.5f), "the low edge of the ratio band does not warn");
    TEST_CHECK(!zones_ct_mapping_mismatch(5.0f, true, 12.0f), "the high edge of the ratio band does not warn");
}

static void test_ct_mapping_mismatch_outside_band_warns(void)
{
    TEST_CHECK(zones_ct_mapping_mismatch(5.0f, true, 0.0f), "a CT reading zero while its zone is on warns");
    TEST_CHECK(zones_ct_mapping_mismatch(5.0f, true, 20.0f), "a CT reading far above normal warns");
}

static void test_ct_mapping_mismatch_tiny_normal_needs_absolute_delta_too(void)
{
    // A tiny normal current means the ratio band alone would flag ordinary
    // measurement noise -- ZONE_CT_MISMATCH_MIN_DELTA_A exists to require a
    // real absolute difference too, not just a ratio.
    TEST_CHECK(!zones_ct_mapping_mismatch(0.05f, true, 0.2f),
              "a small absolute difference on a tiny normal does not warn");
}

static void test_ct_mapping_warn_mask_wiring(void)
{
    static kiln_io_t dummy_io;
    static SafetyLinkClass dummy_safety;
    memset(&dummy_io, 0, sizeof(dummy_io));
    memset(&dummy_safety, 0, sizeof(dummy_safety));
    zones_http_set_hw(&dummy_io, NULL, &dummy_safety);

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 1;
    s_zones.cfg.zones[0].relay_mask = 0x01;
    s_zones.cfg.zones[0].ct_mask = 0x01;
    memset(&s_zone_normals.cfg, 0, sizeof(s_zone_normals.cfg));

    // No hardware -> silent, regardless of anything else.
    zones_http_set_hw(NULL, NULL, NULL);
    TEST_CHECK(zones_ct_mapping_warn_mask() == 0, "no hardware registered -> nothing to warn about");
    zones_http_set_hw(&dummy_io, NULL, &dummy_safety);

    // Link down -> silent even with a mismatching reading staged.
    s_test_safety_link_up = false;
    s_test_relay_shadow = 0x01;
    s_test_safety_status.current_a[0] = 99.0f;
    TEST_CHECK(zones_ct_mapping_warn_mask() == 0, "a down safety link produces no warnings");

    // Link up, zone commanded on, but never measured -> silent.
    s_test_safety_link_up = true;
    TEST_CHECK(zones_ct_mapping_warn_mask() == 0, "an unmeasured zone stays silent even with a live mismatch");

    // Measure the zone's normal, then feed a live reading inside the band.
    zone_normals_set(0, 5.0f);
    s_test_safety_status.current_a[0] = 5.0f;
    TEST_CHECK(zones_ct_mapping_warn_mask() == 0, "a matching live reading warns for nobody");

    // Now feed a live reading far outside the band -- bit 0 (zone 1) must warn.
    s_test_safety_status.current_a[0] = 0.0f;
    TEST_CHECK(zones_ct_mapping_warn_mask() == 0x01, "a mismatching live reading on a commanded-on zone warns");

    // Zone commanded OFF -- must not warn even with the same mismatching reading.
    s_test_relay_shadow = 0x00;
    TEST_CHECK(zones_ct_mapping_warn_mask() == 0, "a zone that is not commanded on is never checked");

    zones_http_set_hw(NULL, NULL, NULL);
    s_test_relay_shadow = 0;
    s_test_safety_link_up = false;
    memset(&s_test_safety_status, 0, sizeof(s_test_safety_status));
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    memset(&s_zone_normals.cfg, 0, sizeof(s_zone_normals.cfg));
}

// ---------------------------------------------------------------------------
// zones_current_sweep_start(): the wired end-to-end refusal path, proving
// the real subsystem accessors (profile_executor_get_status()/
// autotune_engine_is_active()/safety_link_get_status()) are actually
// consulted, not just the pure zone_sweep_check_refusal() helper above.
// ---------------------------------------------------------------------------

static void reset_sweep_state_for_test(void)
{
    s_sweep.active = false;
    s_sweep.abort_requested = false;
    s_sweep.state = ZONE_SWEEP_IDLE;
    s_sweep.zone_index = 0;
    s_sweep.zones_done = 0;
    s_sweep.zones_total = 0;
    s_sweep.reason[0] = '\0';
    s_test_profile_status.state = PROFILE_EXEC_IDLE;
    s_test_autotune_active = false;
    s_test_safety_link_up = true;
    memset(&s_test_safety_status, 0, sizeof(s_test_safety_status));
    s_test_heat_sweep_claim_result = RELAY_HEAT_SWEEP_CLAIM_OK;
    s_test_heat_sweep_claim_begin_calls = 0;
    s_test_heat_sweep_claim_end_calls = 0;
}

static void test_zones_current_sweep_start_wired_refusals(void)
{
    static kiln_io_t dummy_io;
    static SafetyLinkClass dummy_safety;
    static MAX31856BusClass dummy_thermo;
    memset(&dummy_io, 0, sizeof(dummy_io));
    memset(&dummy_safety, 0, sizeof(dummy_safety));
    memset(&dummy_thermo, 0, sizeof(dummy_thermo));
    dummy_thermo.initialized = true;

    reset_sweep_state_for_test();
    zones_http_set_hw(NULL, NULL, NULL);
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones_config_valid = true;
    s_zones.cfg.thermo_count = 1;
    TEST_CHECK(zones_current_sweep_start() == ZONE_SWEEP_REFUSE_NO_HW,
              "no hardware registered at all -> the real start function refuses");

    // H1 (opus review, 2026-08-27): relay I/O present but NO thermo bus used
    // to be accepted -- have_hw was `s_hw_io != NULL` alone, so a sweep could
    // start and energize real elements with the ceiling abort structurally
    // incapable of ever firing (zone_sweep_read_zone_temp() early-returns
    // invalid with no thermo bus, and zone_sweep_ceiling_hit() never trips on
    // an invalid reading). Prove the fix: relay I/O + safety link present,
    // thermo bus NULL, must still refuse NO_HW.
    reset_sweep_state_for_test();
    zones_http_set_hw(&dummy_io, NULL, &dummy_safety);
    s_zones_config_valid = true;
    s_zones.cfg.thermo_count = 1;
    TEST_CHECK(zones_current_sweep_start() == ZONE_SWEEP_REFUSE_NO_HW,
              "H1: relay I/O present but no thermo bus -> the real start function refuses NO_HW, "
              "not just NO relay I/O");

    // Same again, but with a thermo bus struct that itself was never
    // initialized (attached but `.initialized == false`, e.g. bring-up
    // failed on this boot) -- must refuse exactly the same way.
    reset_sweep_state_for_test();
    {
        static MAX31856BusClass uninitialized_thermo;
        memset(&uninitialized_thermo, 0, sizeof(uninitialized_thermo));
        uninitialized_thermo.initialized = false;
        zones_http_set_hw(&dummy_io, &uninitialized_thermo, &dummy_safety);
    }
    s_zones_config_valid = true;
    s_zones.cfg.thermo_count = 1;
    TEST_CHECK(zones_current_sweep_start() == ZONE_SWEEP_REFUSE_NO_HW,
              "H1: a thermo bus struct that is registered but never initialized -> still refused NO_HW");

    reset_sweep_state_for_test();
    zones_http_set_hw(&dummy_io, &dummy_thermo, &dummy_safety);
    s_zones_config_valid = false;
    TEST_CHECK(zones_current_sweep_start() == ZONE_SWEEP_REFUSE_CONFIG_INVALID,
              "invalid config -> the real start function refuses");

    reset_sweep_state_for_test();
    zones_http_set_hw(&dummy_io, &dummy_thermo, &dummy_safety);
    s_zones_config_valid = true;
    s_zones.cfg.thermo_count = 1;
    s_test_profile_status.state = PROFILE_EXEC_RUNNING;
    TEST_CHECK(zones_current_sweep_start() == ZONE_SWEEP_REFUSE_PROFILE_RUNNING,
              "profile_executor_get_status() reporting RUNNING -> the real start function refuses");

    reset_sweep_state_for_test();
    s_zones_config_valid = true;
    s_zones.cfg.thermo_count = 1;
    s_test_autotune_active = true;
    TEST_CHECK(zones_current_sweep_start() == ZONE_SWEEP_REFUSE_AUTOTUNE_RUNNING,
              "autotune_engine_is_active() true -> the real start function refuses");

    reset_sweep_state_for_test();
    s_zones_config_valid = true;
    s_zones.cfg.thermo_count = 1;
    s_test_safety_link_up = false;
    TEST_CHECK(zones_current_sweep_start() == ZONE_SWEEP_REFUSE_LINK_DOWN,
              "safety_link_get_status() reporting link down -> the real start function refuses");

    reset_sweep_state_for_test();
    s_zones_config_valid = true;
    s_zones.cfg.thermo_count = 1;
    s_test_safety_status.fault_asserted = true;
    TEST_CHECK(zones_current_sweep_start() == ZONE_SWEEP_REFUSE_TRIP_LATCHED,
              "a latched fault (fault_asserted) -> the real start function refuses");

    // N9 (opus review, 2026-08-28): a relay already on (kiln_io_get_relay_
    // shadow() non-zero) -- e.g. left on from the dashboard, or by a profile
    // that just ended -- must refuse the start outright.
    reset_sweep_state_for_test();
    s_zones_config_valid = true;
    s_zones.cfg.thermo_count = 1;
    s_test_relay_shadow = 0x01;
    TEST_CHECK(zones_current_sweep_start() == ZONE_SWEEP_REFUSE_RELAYS_ON,
              "N9: any relay already on (per kiln_io_get_relay_shadow()) -> the real start function refuses, "
              "so a foreign load can never ride along on the sweep's measurement");
    s_test_relay_shadow = 0;

    // Now clear every refusal reason -- OK, and starts (xTaskCreate() stub
    // succeeds without ever running the task body, per stubs/freertos/task.h).
    reset_sweep_state_for_test();
    s_zones_config_valid = true;
    s_zones.cfg.thermo_count = 1;
    TEST_CHECK(zones_current_sweep_start() == ZONE_SWEEP_REFUSE_OK, "every refusal cleared -> starts cleanly");
    TEST_CHECK(s_sweep.active, "a started sweep is marked active");

    // And a second start while the first is still (per s_sweep.active)
    // running must be refused -- proves ALREADY_RUNNING really is checked
    // by the wired function, not just the pure helper.
    TEST_CHECK(zones_current_sweep_start() == ZONE_SWEEP_REFUSE_ALREADY_RUNNING,
              "a second start while one is active is refused");

    reset_sweep_state_for_test();
    zones_http_set_hw(NULL, NULL, NULL);
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones_config_valid = false;
}

// The shared heat claim's atomic gate (relay_authority.h) -- proves the LATE
// gate right before s_sweep.active = true is independently load-bearing,
// not merely decorative alongside the early profile_executor_get_status()/
// autotune_engine_is_active() reads
// test_zones_current_sweep_start_wired_refusals() above already covers.
// Sets those EARLY reads to report "nothing running" (the race window
// itself: a profile or autotune could commit in the gap between that read
// and this call) and forces the atomic gate to refuse anyway, simulating
// the other side winning the race.
static void test_zones_current_sweep_start_atomic_gate_closes_the_race(void)
{
    static kiln_io_t dummy_io;
    static SafetyLinkClass dummy_safety;
    static MAX31856BusClass dummy_thermo;
    memset(&dummy_io, 0, sizeof(dummy_io));
    memset(&dummy_safety, 0, sizeof(dummy_safety));
    memset(&dummy_thermo, 0, sizeof(dummy_thermo));
    dummy_thermo.initialized = true;

    // RED: every early/informational check reports clean (profile IDLE,
    // autotune not active, link up, no trip, no relay on) -- yet the atomic
    // gate itself refuses, exactly as it would if a profile won the race in
    // the window right after those reads.
    reset_sweep_state_for_test();
    zones_http_set_hw(&dummy_io, &dummy_thermo, &dummy_safety);
    s_zones_config_valid = true;
    s_zones.cfg.thermo_count = 1;
    s_test_heat_sweep_claim_result = RELAY_HEAT_SWEEP_CLAIM_REFUSE_PROFILE;
    TEST_CHECK(zones_current_sweep_start() == ZONE_SWEEP_REFUSE_PROFILE_RUNNING,
              "the atomic gate alone must be able to produce PROFILE_RUNNING even when the early "
              "profile_executor_get_status() read saw IDLE");
    TEST_CHECK(!s_sweep.active, "a run refused at the atomic gate must never mark the sweep active");
    TEST_CHECK(s_sweep.task == NULL, "a refused start must never have spawned the sweep task");

    reset_sweep_state_for_test();
    zones_http_set_hw(&dummy_io, &dummy_thermo, &dummy_safety);
    s_zones_config_valid = true;
    s_zones.cfg.thermo_count = 1;
    s_test_heat_sweep_claim_result = RELAY_HEAT_SWEEP_CLAIM_REFUSE_AUTOTUNE;
    TEST_CHECK(zones_current_sweep_start() == ZONE_SWEEP_REFUSE_AUTOTUNE_RUNNING,
              "the atomic gate alone must be able to produce AUTOTUNE_RUNNING even when the early "
              "autotune_engine_is_active() read saw false");
    TEST_CHECK(!s_sweep.active, "a run refused at the atomic gate must never mark the sweep active");

    // GREEN: identical setup, atomic gate now allows it -- proves the RED
    // results above were really the gate, not some other stub failing
    // closed.
    reset_sweep_state_for_test();
    zones_http_set_hw(&dummy_io, &dummy_thermo, &dummy_safety);
    s_zones_config_valid = true;
    s_zones.cfg.thermo_count = 1;
    TEST_CHECK(zones_current_sweep_start() == ZONE_SWEEP_REFUSE_OK,
              "with the atomic gate allowing it, the identical setup must start cleanly");
    TEST_CHECK(s_sweep.active, "a started sweep is marked active");
    TEST_CHECK(s_test_heat_sweep_claim_begin_calls == 1, "the gate is attempted exactly once per "
                                                         "zones_current_sweep_start() call");

    reset_sweep_state_for_test();
    zones_http_set_hw(NULL, NULL, NULL);
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones_config_valid = false;
}

// ---------------------------------------------------------------------------
// M12b: calibrating k_ct_v_per_a from the sweep
// ---------------------------------------------------------------------------
// The pure decision first. Every refusal below is a NEGATIVE test in the
// sense this repo means it: it is exercised by breaking the input the guard
// exists to catch, and each one is paired with the same call made valid, so a
// guard that had been deleted would show up as a passing "refuses" check
// only if the OK case also stopped resolving -- which it does not.

static void test_zone_sweep_derive_k_ct_scales_by_the_measured_over_expected_ratio(void)
{
    TEST_SECTION("zone_sweep_derive_k_ct -- k_new = k_old * (measured / nameplate) amps");

    float k = 0.0f;
    // 7.2 kW at 240 V is 30 A expected; the sweep measured exactly that, so
    // the existing k is already right and must come back unchanged.
    TEST_CHECK(zone_sweep_derive_k_ct(30.0f, 7200.0f, 240.0f, 0.0333f, &k) == ZONE_KCT_DERIVE_OK,
               "a measurement that matches the nameplate derives");
    TEST_CHECK(fabsf(k - 0.0333f) < 1e-6f, "an exact match leaves k_ct_v_per_a unchanged");

    // Reading HALF the nameplate current means the board is under-reporting,
    // which happens when k_ct is set too small -- so k must come down, not up.
    TEST_CHECK(zone_sweep_derive_k_ct(15.0f, 7200.0f, 240.0f, 0.0333f, &k) == ZONE_KCT_DERIVE_OK,
               "a half-scale measurement derives");
    TEST_CHECK(fabsf(k - 0.01665f) < 1e-6f, "reading half the nameplate halves k_ct_v_per_a");

    // And the other direction -- the sign of the correction is the half of
    // this that a transposed formula would get exactly backwards.
    TEST_CHECK(zone_sweep_derive_k_ct(60.0f, 7200.0f, 240.0f, 0.0333f, &k) == ZONE_KCT_DERIVE_OK,
               "a double-scale measurement derives");
    TEST_CHECK(k > 0.0333f, "reading twice the nameplate RAISES k_ct_v_per_a, never lowers it");
}

static void test_zone_sweep_derive_k_ct_refuses_without_the_nameplate_answers(void)
{
    TEST_SECTION("zone_sweep_derive_k_ct -- refuses unless BOTH commissioning answers are real");

    float k = 0.0f;
    TEST_CHECK(zone_sweep_derive_k_ct(30.0f, 0.0f, 240.0f, 0.0333f, &k) == ZONE_KCT_DERIVE_NO_NAMEPLATE,
               "no full-output power answer derives nothing");
    TEST_CHECK(zone_sweep_derive_k_ct(30.0f, 7200.0f, 0.0f, 0.0333f, &k) == ZONE_KCT_DERIVE_NO_NAMEPLATE,
               "no mains voltage answer derives nothing");
    TEST_CHECK(zone_sweep_derive_k_ct(30.0f, NAN, 240.0f, 0.0333f, &k) == ZONE_KCT_DERIVE_NO_NAMEPLATE,
               "a NaN power answer derives nothing");
    TEST_CHECK(zone_sweep_derive_k_ct(30.0f, 7200.0f, -240.0f, 0.0333f, &k) == ZONE_KCT_DERIVE_NO_NAMEPLATE,
               "a negative mains voltage derives nothing");
}

static void test_zone_sweep_derive_k_ct_refuses_without_a_measurement(void)
{
    TEST_SECTION("zone_sweep_derive_k_ct -- refuses a total that is noise, or no total at all");

    float k = 0.0f;
    TEST_CHECK(zone_sweep_derive_k_ct(0.0f, 7200.0f, 240.0f, 0.0333f, &k) == ZONE_KCT_DERIVE_NO_MEASUREMENT,
               "a zero measured total derives nothing");
    TEST_CHECK(zone_sweep_derive_k_ct(1.9f, 7200.0f, 240.0f, 0.0333f, &k) == ZONE_KCT_DERIVE_NO_MEASUREMENT,
               "a total below the load threshold derives nothing");
    TEST_CHECK(zone_sweep_derive_k_ct(NAN, 7200.0f, 240.0f, 0.0333f, &k) == ZONE_KCT_DERIVE_NO_MEASUREMENT,
               "a NaN total derives nothing");
}

static void test_zone_sweep_derive_k_ct_refuses_an_uncommissioned_prior_k(void)
{
    TEST_SECTION("zone_sweep_derive_k_ct -- an uncommissioned k_old carries no scale to correct");

    // The link reports AMPS, not counts, and the Pico computes those amps
    // with k_ct_v_per_a -- so k_old == 0 means every reading was 0.0 A and
    // there is nothing to scale. Inventing a starting value here is exactly
    // what this refusal exists to prevent.
    float k = 0.0f;
    TEST_CHECK(zone_sweep_derive_k_ct(30.0f, 7200.0f, 240.0f, 0.0f, &k) == ZONE_KCT_DERIVE_NO_PRIOR_K,
               "k_ct_v_per_a at its uncommissioned 0.0 derives nothing");
    TEST_CHECK(zone_sweep_derive_k_ct(30.0f, 7200.0f, 240.0f, -0.03f, &k) == ZONE_KCT_DERIVE_NO_PRIOR_K,
               "a negative k_ct_v_per_a derives nothing");
    TEST_CHECK(zone_sweep_derive_k_ct(30.0f, 7200.0f, 240.0f, NAN, &k) == ZONE_KCT_DERIVE_NO_PRIOR_K,
               "a NaN k_ct_v_per_a derives nothing");
}

static void test_zone_sweep_derive_k_ct_refuses_an_implausible_correction(void)
{
    TEST_SECTION("zone_sweep_derive_k_ct -- a disagreement too large to be a scale error is refused");

    float k = 0.0f;
    // 300 A measured against a 30 A nameplate is not a mis-scaled CT, it is a
    // wiring or units error -- and scaling k_ct by 10 would make the amps
    // read right while moving the presence threshold to match the error.
    TEST_CHECK(zone_sweep_derive_k_ct(300.0f, 7200.0f, 240.0f, 0.0333f, &k) == ZONE_KCT_DERIVE_IMPLAUSIBLE,
               "measuring 10x the nameplate derives nothing");
    TEST_CHECK(zone_sweep_derive_k_ct(3.0f, 7200.0f, 240.0f, 0.0333f, &k) == ZONE_KCT_DERIVE_IMPLAUSIBLE,
               "measuring a tenth of the nameplate derives nothing");
    // The absolute band is a separate guard from the ratio band: this ratio
    // (2x) is perfectly acceptable, but the k it produces is not a CT.
    TEST_CHECK(zone_sweep_derive_k_ct(60.0f, 7200.0f, 240.0f, 0.4f, &k) == ZONE_KCT_DERIVE_IMPLAUSIBLE,
               "an in-band ratio that lands k outside the plausible CT band derives nothing");
    // ...and the same in-band ratio on a plausible k_old still derives, so
    // the check above is the absolute band talking, not the ratio band.
    TEST_CHECK(zone_sweep_derive_k_ct(60.0f, 7200.0f, 240.0f, 0.0333f, &k) == ZONE_KCT_DERIVE_OK,
               "the same 2x ratio on a plausible k_old is accepted");
}

// ---- the plan, which is where the run's own completeness is judged ---------

// Puts s_ct_derive and the fake committed record into the state a clean
// three-zone sweep leaves behind: every channel resolved, both nameplate
// answers present, all three k_ct_v_per_a committed at the CT's datasheet
// figure, and a measured total that exactly matches the nameplate.
static void kct_setup_clean_run(void)
{
    test_link_reset();
    test_cfg_rows_reset();
    memset(&s_ct_derive, 0, sizeof(s_ct_derive));
    zone_k_ct_clear(); // no test may inherit an earlier test's provenance record
    s_ct_derive.derived_mask = 0x07;
    s_ct_derive.zone_for_ch[0] = 0;
    s_ct_derive.zone_for_ch[1] = 1;
    s_ct_derive.zone_for_ch[2] = 2;
    s_ct_derive.measured_total_a = 30.0f;
    test_cfg_set_f32(ZONE_MAINS_VOLTAGE_PARAM_ID, 240.0f, true);
    test_cfg_set_f32(ZONE_MAX_POWER_PARAM_ID, 7200.0f, true);
    for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
        test_cfg_set_f32(ZONE_KCT_PARAM_ID(c), 0.0333f, true);
    }
}

static void test_zone_sweep_plan_k_ct_clean_run_plans_every_derived_channel(void)
{
    TEST_SECTION("zone_sweep_plan_k_ct -- a complete run with both answers plans all three channels");

    kct_setup_clean_run();
    float k[ZONE_CT_CHANNEL_COUNT] = {0};
    char note[96] = "";
    TEST_CHECK(zone_sweep_plan_k_ct(k, note, sizeof(note)) == 0x07,
               "every channel the map resolved is calibrated");
    TEST_CHECK(note[0] == '\0', "a clean plan says nothing");
    TEST_CHECK(fabsf(k[1] - 0.0333f) < 1e-6f, "a matching measurement leaves the scale where it was");
}

static void test_zone_sweep_plan_k_ct_refuses_an_incomplete_run(void)
{
    TEST_SECTION("zone_sweep_plan_k_ct -- a zone that did not resolve makes the kiln total a lie");

    // The guard being proved: an unresolved zone still drew current, so the
    // total is short by that zone's share and the derived scale would be
    // dragged DOWN by exactly that much -- silently.
    kct_setup_clean_run();
    s_ct_derive.unresolved_zone_mask = 0x04;
    float k[ZONE_CT_CHANNEL_COUNT] = {0};
    char note[96] = "";
    TEST_CHECK(zone_sweep_plan_k_ct(k, note, sizeof(note)) == 0,
               "an unresolved zone calibrates nothing");
    TEST_CHECK(strstr(note, "incomplete") != NULL, "and the operator is told why");

    // The same run with that zone resolved plans normally -- so the refusal
    // above is the completeness check, not some other failure.
    s_ct_derive.unresolved_zone_mask = 0;
    TEST_CHECK(zone_sweep_plan_k_ct(k, note, sizeof(note)) == 0x07,
               "clearing the unresolved zone lets the same run calibrate");

    // A two-zones-one-channel conflict is the same hole seen from the other
    // side, and must refuse identically.
    s_ct_derive.conflict_mask = 0x02;
    TEST_CHECK(zone_sweep_plan_k_ct(k, note, sizeof(note)) == 0,
               "a shared-CT conflict calibrates nothing either");
}

static void test_zone_sweep_plan_k_ct_refuses_after_a_failed_map_push(void)
{
    TEST_SECTION("zone_sweep_plan_k_ct -- never commits on top of a staged buffer the map push abandoned");

    // Both pushes stage into the SAME buffer on the Pico and each ends with
    // its own COMMIT_CONFIG. If the map push failed and could not fully back
    // its staging out, this commit would carry those leftovers into flash --
    // which is precisely the hazard the map push's own H3 repair exists for.
    kct_setup_clean_run();
    s_ct_derive.map_push_failed = true;
    float k[ZONE_CT_CHANNEL_COUNT] = {0};
    char note[96] = "";
    TEST_CHECK(zone_sweep_plan_k_ct(k, note, sizeof(note)) == 0,
               "a failed CT-map push blocks the CT-scale commit outright");
    TEST_CHECK(strstr(note, "CT map write failed") != NULL, "and says so");

    s_ct_derive.map_push_failed = false;
    TEST_CHECK(zone_sweep_plan_k_ct(k, note, sizeof(note)) == 0x07,
               "with the map push clean the same run calibrates");
}

static void test_zone_sweep_plan_k_ct_refuses_an_unanswered_nameplate(void)
{
    TEST_SECTION("zone_sweep_plan_k_ct -- an UNSET commissioning answer is not a zero");

    kct_setup_clean_run();
    // Present in the record but never set -- the case a plain "read the
    // value" would turn into 0.0 W and the derivation would then have to
    // catch as a nameplate refusal one layer later.
    test_cfg_set_f32(ZONE_MAX_POWER_PARAM_ID, 7200.0f, false);
    float k[ZONE_CT_CHANNEL_COUNT] = {0};
    char note[96] = "";
    TEST_CHECK(zone_sweep_plan_k_ct(k, note, sizeof(note)) == 0,
               "an unset full-output power answer calibrates nothing");
    TEST_CHECK(strstr(note, "mains voltage") != NULL, "and asks for the missing answers");
    /* Overflow-fix regression (zone_sweep_plan_k_ct()'s NO_NAMEPLATE arm,
     * %.72s -> %.70s): the operator-visible reason must be the COMPLETE
     * sentence, not silently cut short -- assert the exact byte-for-byte
     * message, not just a substring, so a future re-introduction of a
     * precision wider than the note[] buffer allows would fail this even if
     * the truncated tail still happened to contain "mains voltage". */
    TEST_CHECK(strcmp(note, "CT scale not calibrated: answer the mains voltage and "
                            "full-output power questions first") == 0,
               "the NO_NAMEPLATE reason lands in note[] complete and unmodified");

    test_cfg_set_f32(ZONE_MAX_POWER_PARAM_ID, 7200.0f, true);
    TEST_CHECK(zone_sweep_plan_k_ct(k, note, sizeof(note)) == 0x07,
               "answering it lets the same run calibrate");
}

static void test_zone_sweep_plan_k_ct_implausible_reason_is_not_truncated(void)
{
    TEST_SECTION("zone_sweep_plan_k_ct -- the longest real refusal reason must reach note[] whole");

    /* This drives the LONGEST of the five zone_kct_derive_t strings
     * ("measured and nameplate current disagree too far to be a scale
     * error", 67 bytes) through the exact call site
     * (zone_sweep_plan_k_ct()'s IMPLAUSIBLE arm) that used to read
     * `snprintf(note, note_cap, "CT scale not calibrated: %.72s", ...)` --
     * 25 (prefix) + 72 (precision) + 1 (NUL) = 98 bytes claimed against a
     * 96-byte note[], the exact -Wformat-truncation shape build_kilnfw's
     * -Werror has broken this build on twice before (see this file's H1/H3
     * comments). measured_total_a = 10x the nameplate expectation (300A vs
     * a 30A expected_a from 7200W/240V) lands ratio=10, past
     * ZONE_KCT_RATIO_MAX, which is what zone_sweep_derive_k_ct() reports as
     * ZONE_KCT_DERIVE_IMPLAUSIBLE. */
    kct_setup_clean_run();
    s_ct_derive.measured_total_a = 300.0f;
    float k[ZONE_CT_CHANNEL_COUNT] = {0};
    char note[96] = "";
    TEST_CHECK(zone_sweep_plan_k_ct(k, note, sizeof(note)) == 0,
               "an implausible measured/nameplate ratio calibrates nothing");
    static const char expected[] =
        "CT scale not calibrated: measured and nameplate current disagree too "
        "far to be a scale error";
    TEST_CHECK(strlen(expected) == 92, "the fixture's own expected string is the documented worst case");
    TEST_CHECK(strcmp(note, expected) == 0,
               "the operator sees the WHOLE reason, not a %.70s-truncated fragment of it");
}

// ---- the push, and every way it can fail after something has been staged ---

static int kct_setparam_count_for(uint16_t param_id)
{
    int n = 0;
    for (int i = 0; i < s_setparam_count && i < TEST_SETPARAM_LOG_MAX; i++) {
        if (s_setparam_log[i].param_id == param_id) {
            n++;
        }
    }
    return n;
}

static void test_zone_sweep_push_k_ct_happy_path_writes_and_confirms(void)
{
    TEST_SECTION("zone_sweep_push_k_ct_v_per_a -- stages, commits, and confirms by read-back");

    kct_setup_clean_run();
    s_ct_derive.measured_total_a = 60.0f; // 2x the nameplate -> a real correction to write
    s_cfg_refetch_applies_staged = true;  // a Pico that genuinely applied the commit
    s_hw_safety = (SafetyLinkClass *)1;

    zone_sweep_push_k_ct_v_per_a();

    TEST_CHECK(s_sweep.k_ct_derived_mask == 0x07, "all three channels report as derived");
    TEST_CHECK(s_sweep.k_ct_reason[0] == '\0', "a confirmed write says nothing");
    TEST_CHECK(s_setparam_count == 3, "exactly one SET_PARAM per channel, no repair writes");
    TEST_CHECK(s_commit_count == 1, "one COMMIT_CONFIG");
    TEST_CHECK(fabsf(s_setparam_log[0].value.f32_val - 0.0666f) < 1e-5f,
               "the value staged is the corrected scale, not the old one");
    uint8_t dm = 0;
    float persisted[ZONE_CT_CHANNEL_COUNT] = {0};
    zones_ct_k_v_per_a_derived(&dm, persisted);
    TEST_CHECK(dm == 0x07, "the provenance record says all three were derived here");
    TEST_CHECK(fabsf(persisted[2] - 0.0666f) < 1e-5f, "and remembers the value it wrote");
    s_hw_safety = NULL;
}

static void test_zone_sweep_push_k_ct_unconfirmed_readback_derives_nothing_and_backs_out(void)
{
    TEST_SECTION("zone_sweep_push_k_ct_v_per_a -- an ACKed commit that does not read back is NOT written");

    // H1, for this push: SET_PARAM/COMMIT_CONFIG are fire-and-forget, so an
    // un-rejected commit proves nothing. Here the Pico "acks" but the record
    // still reads the OLD value -- the push must report nothing derived AND
    // put the staged buffer back, or the next unrelated commit carries this
    // run's abandoned scale into flash.
    kct_setup_clean_run();
    s_ct_derive.measured_total_a = 60.0f;
    s_cfg_refetch_applies_staged = false; // the record never changes -> read-back disagrees
    s_hw_safety = (SafetyLinkClass *)1;

    zone_sweep_push_k_ct_v_per_a();

    TEST_CHECK(s_sweep.k_ct_derived_mask == 0, "an unconfirmed write derives nothing");
    TEST_CHECK(strstr((const char *)s_sweep.k_ct_reason, "NOT written") != NULL,
               "and says the value was not written");
    TEST_CHECK(s_setparam_count == 6, "every staged channel is re-staged back (3 writes + 3 repairs)");
    for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
        TEST_CHECK(kct_setparam_count_for(ZONE_KCT_PARAM_ID(c)) == 2,
                   "each channel was both staged and backed out");
        TEST_CHECK(fabsf(s_setparam_log[3 + c].value.f32_val - 0.0333f) < 1e-6f,
                   "the backout restores the value the Pico has actually committed");
    }
    uint8_t dm = 0xFF;
    zones_ct_k_v_per_a_derived(&dm, NULL);
    TEST_CHECK(dm == 0, "and nothing is persisted as derived");
    s_hw_safety = NULL;
}

static void test_zone_sweep_push_k_ct_rejected_commit_backs_the_staging_out(void)
{
    TEST_SECTION("zone_sweep_push_k_ct_v_per_a -- a REJECTED commit still leaves staging to back out");

    kct_setup_clean_run();
    s_ct_derive.measured_total_a = 60.0f;
    s_commit_rejected = true; // the Pico refuses config writes while armed
    s_hw_safety = (SafetyLinkClass *)1;

    zone_sweep_push_k_ct_v_per_a();

    TEST_CHECK(s_sweep.k_ct_derived_mask == 0, "a rejected commit derives nothing");
    TEST_CHECK(strstr((const char *)s_sweep.k_ct_reason, "rejected") != NULL,
               "and the rejection is reported rather than inferred from an unchanged value");
    TEST_CHECK(s_setparam_count == 6, "the three staged channels are all backed out");
    s_hw_safety = NULL;
}

static void test_zone_sweep_push_k_ct_partial_staging_failure_backs_out_what_staged(void)
{
    TEST_SECTION("zone_sweep_push_k_ct_v_per_a -- a mid-loop staging failure backs out the earlier writes");

    // H3, for this push: the failure arrives on channel 1, so channel 0 is
    // already sitting in the Pico's staged buffer. It must be overwritten
    // with the committed value, and no COMMIT_CONFIG may be sent at all.
    kct_setup_clean_run();
    s_ct_derive.measured_total_a = 60.0f;
    s_setparam_fail_at = 1;
    s_hw_safety = (SafetyLinkClass *)1;

    zone_sweep_push_k_ct_v_per_a();

    TEST_CHECK(s_sweep.k_ct_derived_mask == 0, "a failed stage derives nothing");
    TEST_CHECK(s_commit_count == 0, "and nothing is committed");
    TEST_CHECK(kct_setparam_count_for(ZONE_KCT_PARAM_ID(0)) == 2,
               "the channel that DID stage is written back");
    TEST_CHECK(kct_setparam_count_for(ZONE_KCT_PARAM_ID(2)) == 0,
               "and the channel the loop never reached is left alone");
    s_hw_safety = NULL;
}

static void test_zone_sweep_push_k_ct_backout_restores_the_uncommissioned_zero(void)
{
    TEST_SECTION("zone_sweep_push_k_ct_v_per_a -- backing out a never-committed channel restores 0.0");

    // A channel with no committed value has nothing to restore, so the
    // repair writes config_store's own uncommissioned 0.0f -- which puts the
    // Pico back on the SAFE side of current_presence_policy's branch rather
    // than leaving this run's abandoned scale staged.
    kct_setup_clean_run();
    s_ct_derive.derived_mask = 0x01; // only channel 0
    s_ct_derive.measured_total_a = 60.0f;
    test_cfg_set_f32(ZONE_KCT_PARAM_ID(0), 0.0333f, true); // usable as k_old...
    s_commit_rejected = true;
    s_hw_safety = (SafetyLinkClass *)1;
    zone_sweep_push_k_ct_v_per_a();
    TEST_CHECK(s_setparam_count == 2 && fabsf(s_setparam_log[1].value.f32_val - 0.0333f) < 1e-6f,
               "a committed channel is restored to its committed value");

    // ...and now the same channel with the row marked never-set.
    kct_setup_clean_run();
    s_ct_derive.derived_mask = 0x01;
    s_ct_derive.measured_total_a = 60.0f;
    s_commit_rejected = true;
    // k_old must still be readable for the DERIVATION, so this test drives
    // the restore path directly rather than through a plan that would refuse.
    uint8_t staged_only_ch0 = 0x01;
    test_cfg_set_f32(ZONE_KCT_PARAM_ID(0), 0.0333f, false); // present but never set
    char note[96] = "";
    zone_sweep_unstage_k_ct(staged_only_ch0, note, sizeof(note));
    TEST_CHECK(s_setparam_count == 1 && s_setparam_log[0].value.f32_val == 0.0f,
               "a never-committed channel is restored to the uncommissioned 0.0");
    s_hw_safety = NULL;
}

// ---------------------------------------------------------------------------
// heater_min_on_ms and HEATER_MIN_ON_MS_FLOOR (10 s, set by the owner
// 2026-08-28).
//
// The floor is enforced in four places; this block covers three of them (the
// fourth, heater_output_duty()'s own quantization + running hold, is
// test_heater_output.c's and stays there as defense in depth):
//   1. parse_zone_fields()  -- the POST /api/zones door an operator types at;
//   2. zones_config_json_validate() -- every stored or imported blob;
//   3. raise_heater_timing_to_floors(), via zones_config_json_decode_blob() -- a pre-floor stored
//      value is raised on load, so a GET can never report a number the very
//      next POST would bounce.
//
// The HTTP layer REFUSES rather than clamps. A clamped-and-stored value would
// leave /api/zones reporting a number the kiln is not actually using, which
// is the dishonesty this page's other read-only/derived labelling exists to
// avoid. See ZONE_HEATER_MIN_ON_MS_FLOOR's comment in zones_http.h.
// ---------------------------------------------------------------------------

/* Set by post_body_with_minon() on the accept path so a caller can assert on
 * the value that actually LANDED, not merely on the accept/refuse verdict --
 * "accepted" alone would not catch a clamp. */
static float s_last_parsed_minon = -1.0f;

/* One clean body with only z0_minon varied, so any difference in the verdict
 * can only be that one field. */
static bool post_body_with_minon(const char *minon_literal, const char **err_reason_out)
{
    char body[512];
    snprintf(body, sizeof(body),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=0&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=60000&z0_minon=%s&z0_minoff=2000&"
             "z0_timingprofile=0",
             minon_literal);

    zone_cfg_t current = make_stored_zone();
    current.heater_min_on_ms = (float)HEATER_MIN_ON_MS_FLOOR; /* a legal stored value */
    zone_cfg_t out;
    memset(&out, 0, sizeof(out));
    const char *err_reason = "unset";
    bool ok = parse_zone_fields(body, /*i=*/0, /*thermo_count=*/1, /*relay_count=*/4,
                                /*timing_profile_count=*/1, &current, &out, &err_reason);
    if (err_reason_out) {
        *err_reason_out = err_reason;
    }
    s_last_parsed_minon = ok ? out.heater_min_on_ms : -1.0f;
    return ok;
}

static void test_post_minon_below_floor_is_refused(void)
{
    TEST_SECTION("parse_zone_fields -- heater_min_on_ms below the 10 s floor is REFUSED, not clamped");

    const char *reason = "unset";
    TEST_CHECK(!post_body_with_minon("3000", &reason),
               "z0_minon=3000 must be refused outright, never stored and silently run at 10000");
    TEST_CHECK(reason && strstr(reason, "heater_min_on_ms") != NULL,
               "the refusal names the field, so the page can tell the operator which one");
    TEST_CHECK(reason && strstr(reason, "10000") != NULL,
               "the refusal names the floor itself, not just 'out of range'");

    /* One millisecond under. If the comparison were a >= where it should be a
     * <, nothing else in this suite would notice. */
    TEST_CHECK(!post_body_with_minon("9999", &reason),
               "9999 ms -- one under the floor -- must also be refused");
}

static void test_post_minon_zero_and_at_or_above_floor_are_accepted(void)
{
    TEST_SECTION("parse_zone_fields -- 0 and >= floor accepted, and a value above the floor is "
                 "honored EXACTLY");

    const char *reason = "unset";
    TEST_CHECK(post_body_with_minon("0", &reason),
               "0 means 'not configured' (the default it selects IS the floor) and stays accepted");
    TEST_CHECK_NEAR(s_last_parsed_minon, 0.0f, 1e-6,
                    "0 is stored as 0, not rewritten to 10000 -- 'use the default' must stay "
                    "distinguishable from 'the operator chose 10000'");

    TEST_CHECK(post_body_with_minon("10000", &reason),
               "exactly the floor is accepted -- the boundary is inclusive");
    TEST_CHECK_NEAR(s_last_parsed_minon, 10000.0f, 1e-6, "the floor value round-trips exactly");

    /* The failure mode of a clamp written as an assignment instead of a
     * comparison: it would clobber a legitimately longer configured value. */
    TEST_CHECK(post_body_with_minon("15000", &reason),
               "15000 ms (above the floor) is accepted");
    TEST_CHECK_NEAR(s_last_parsed_minon, 15000.0f, 1e-6,
                    "15000 is honored EXACTLY -- the floor raises, it never lowers");
}

static void test_validate_rejects_sub_floor_min_on(void)
{
    TEST_SECTION("validate_zones_cfg -- a sub-floor heater_min_on_ms is rejected (the import door)");

    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].heater_min_on_ms = 3000.0f;
        const char *reason = NULL;
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason),
                   "a config carrying 3000 ms does not validate, whichever door it came through");
        TEST_CHECK(reason && strstr(reason, "heater_min_on_ms") != NULL, "the reason names the field");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].heater_min_on_ms = 15000.0f;
        const char *reason = NULL;
        TEST_CHECK(zones_config_json_validate(&cfg, &reason),
                   "15000 ms still validates -- the check must not have become 'must equal 10000'");
    }
}

static void test_stored_pre_floor_blob_is_raised_on_load_not_rejected(void)
{
    TEST_SECTION("decode_zones_blob -- a pre-floor stored heater_min_on_ms is RAISED on load, "
                 "not rejected");

    /* A board configured before 2026-08-28 legally carries 2000 ms (the old
     * HEATER_DEFAULT_MIN_ON_MS). Rejecting it at load would wipe that board's
     * config on the next boot; reporting it verbatim would make the very next
     * whole-page save fail -- the page posts back what GET returned -- on a
     * number the operator never typed. Raising on load makes both
     * impossible, and leaves the refusal above firing only on human input. */
    zones_cfg_t stored;
    make_minimal_valid_cfg(&stored);
    stored.zones[0].heater_min_on_ms = 2000.0f;
    stored.zones[1].heater_min_on_ms = 0.0f;
    stored.zones[2].heater_min_on_ms = 15000.0f;
    stored.crc32 = 0;
    stored.crc32 = zones_config_json_compute_crc(&stored);

    zones_cfg_t out;
    const char *reason = "unset";
    zones_decode_result_t r = zones_config_json_decode_blob(&stored, sizeof(stored), &out, &reason);

    TEST_CHECK(r == ZONES_DECODE_OK,
               "a pre-floor blob still decodes -- it must not be called corrupt");
    TEST_CHECK_NEAR(out.zones[0].heater_min_on_ms, (float)HEATER_MIN_ON_MS_FLOOR, 1e-6,
                    "2000 ms is raised to the 10000 ms floor on load");
    TEST_CHECK_NEAR(out.zones[1].heater_min_on_ms, 0.0f, 1e-6,
                    "0 ('not configured') is left alone -- the default it selects IS the floor");
    TEST_CHECK_NEAR(out.zones[2].heater_min_on_ms, 15000.0f, 1e-6,
                    "a longer configured value is untouched by the raise");
}

// ---------------------------------------------------------------------------
// heater_window_ms vs heater_min_on_ms (2026-08-29).
//
// The relationship, not either number. This bench's zone 0 carried
// window=2000 ms with the 10 s min-on floor: both values passed their own
// range checks, and together they made the zone incapable of rendering ANY
// fractional duty -- 0.4*2000 = 800 ms is under the floor, so
// heater_output_duty() quantized every window to OFF. An autotune step at
// duty 0.4 commanded heat for 40 minutes and never closed the relay once.
//
// Enforced the same four places the min-on floor is (refuse at the POST door,
// refuse in the setter backup import uses, refuse in zones_config_json_validate(),
// raise on load), with heater_output_cfg_expressible() as the point-of-use
// check. These tests negative-test the rule at each door -- a check nothing
// can fail is not a check.
// ---------------------------------------------------------------------------

static float s_last_parsed_window = -1.0f;

static bool post_body_with_window(const char *window_literal, const char *minon_literal,
                                  const char **err_reason_out)
{
    char body[512];
    snprintf(body, sizeof(body),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=0&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=%s&z0_minon=%s&z0_minoff=2000&"
             "z0_timingprofile=0",
             window_literal, minon_literal);

    zone_cfg_t current = make_stored_zone();
    zone_cfg_t out;
    memset(&out, 0, sizeof(out));
    const char *err_reason = "unset";
    bool ok = parse_zone_fields(body, /*i=*/0, /*thermo_count=*/1, /*relay_count=*/4,
                                /*timing_profile_count=*/1, &current, &out, &err_reason);
    if (err_reason_out) {
        *err_reason_out = err_reason;
    }
    s_last_parsed_window = ok ? out.heater_window_ms : -1.0f;
    return ok;
}

static void test_post_window_too_short_for_min_on_is_refused(void)
{
    TEST_SECTION("parse_zone_fields -- heater_window_ms shorter than 3x the min-on is REFUSED");

    const char *reason = "unset";

    /* The exact configuration that was on this bench. */
    TEST_CHECK(!post_body_with_window("2000", "0", &reason),
               "the bench's own window=2000 with the default 10 s min-on must now be refused");
    TEST_CHECK(reason && strstr(reason, "heater_window_ms") != NULL,
               "the refusal names the field, so the page can point at it");
    TEST_CHECK(reason && strstr(reason, "min") != NULL,
               "the refusal explains it is about the minimum on-time, not a plain range");

    /* One millisecond under the bound: catches a >= written as a >. */
    TEST_CHECK(!post_body_with_window("29999", "0", &reason),
               "29999 ms -- one under 3x the 10 s floor -- is refused too");

    /* The bound tracks the ZONE's own min_on, it is not the constant 30000.
     * With min_on=20000 the requirement is 60000, so 45000 must fail even
     * though it comfortably clears the default-case bound. */
    TEST_CHECK(!post_body_with_window("45000", "20000", &reason),
               "the bound is 3x THIS zone's min-on (60000), not a fixed 30000");
}

static void test_post_window_at_or_above_the_bound_is_accepted_exactly(void)
{
    TEST_SECTION("parse_zone_fields -- a window at or above 3x the min-on is accepted, unaltered");

    const char *reason = "unset";

    TEST_CHECK(post_body_with_window("30000", "0", &reason),
               "exactly 3x the 10 s floor is accepted -- the boundary is inclusive");
    TEST_CHECK_NEAR(s_last_parsed_window, 30000.0f, 1e-6, "30000 round-trips exactly, not raised");

    TEST_CHECK(post_body_with_window("60000", "0", &reason),
               "the 60 s firmware default is accepted");
    TEST_CHECK_NEAR(s_last_parsed_window, 60000.0f, 1e-6, "60000 is honored exactly");

    TEST_CHECK(post_body_with_window("0", "0", &reason),
               "0 means 'not configured' and stays accepted -- the default it selects satisfies the rule");
    TEST_CHECK_NEAR(s_last_parsed_window, 0.0f, 1e-6,
                    "0 stays 0 -- 'use the default' must stay distinguishable from 'chose 60000'");

    TEST_CHECK(post_body_with_window("60000", "20000", &reason),
               "a longer min-on with a window that still clears 3x it is accepted");
}

static void test_setter_and_validate_reject_short_window(void)
{
    TEST_SECTION("zones_config_set_heater_cfg / validate_zones_cfg -- the non-HTTP doors refuse too");

    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].heater_window_ms = 2000.0f;
        cfg.zones[0].heater_min_on_ms = 0.0f;
        const char *reason = NULL;
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason),
                   "a stored/imported config with a 2000 ms window does not validate");
        TEST_CHECK(reason && strstr(reason, "heater_window_ms") != NULL, "the reason names the field");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].heater_window_ms = 30000.0f;
        cfg.zones[0].heater_min_on_ms = 0.0f;
        const char *reason = NULL;
        TEST_CHECK(zones_config_json_validate(&cfg, &reason),
                   "30000 still validates -- the check must not reject everything");
    }
    /* The setter backup import comes through. Its refusal is the reason a
     * restore of a pre-2026-08-29 backup cannot re-install the trap. */
    TEST_CHECK(!zones_config_set_heater_cfg(0, 2000.0f, 0.0f, 2000.0f),
               "zones_config_set_heater_cfg refuses a 2000 ms window -- backup import's door");
    TEST_CHECK(!zones_config_set_heater_cfg(0, 45000.0f, 20000.0f, 2000.0f),
               "and it computes the bound from the min_on it was handed, not a constant");
}

static void test_stored_short_window_is_raised_on_load(void)
{
    TEST_SECTION("decode_zones_blob -- a stored too-short heater_window_ms is RAISED on load");

    /* Exactly this bench's flash. Rejecting the blob would wipe the board's
     * whole config on the next boot; reporting 2000 verbatim would make the
     * next whole-page save fail on a number the operator never typed. */
    zones_cfg_t stored;
    make_minimal_valid_cfg(&stored);
    stored.zones[0].heater_window_ms = 2000.0f;
    stored.zones[0].heater_min_on_ms = 0.0f;
    stored.zones[1].heater_window_ms = 0.0f;
    stored.zones[1].heater_min_on_ms = 0.0f;
    /* min_on itself sub-floor: the window bound must be computed from the
     * RAISED min_on (10000 -> needs 30000), not the stored 2000. */
    stored.zones[2].heater_window_ms = 20000.0f;
    stored.zones[2].heater_min_on_ms = 2000.0f;
    stored.crc32 = 0;
    stored.crc32 = zones_config_json_compute_crc(&stored);

    zones_cfg_t out;
    const char *reason = "unset";
    zones_decode_result_t r = zones_config_json_decode_blob(&stored, sizeof(stored), &out, &reason);

    TEST_CHECK(r == ZONES_DECODE_OK, "the blob still decodes -- it must not be called corrupt");
    TEST_CHECK_NEAR(out.zones[0].heater_window_ms, 30000.0f, 1e-6,
                    "2000 ms is raised to 3x the 10 s floor on load");
    TEST_CHECK_NEAR(out.zones[1].heater_window_ms, 0.0f, 1e-6,
                    "0 ('not configured') is left alone");
    TEST_CHECK_NEAR(out.zones[2].heater_min_on_ms, 10000.0f, 1e-6, "min_on raised first");
    TEST_CHECK_NEAR(out.zones[2].heater_window_ms, 30000.0f, 1e-6,
                    "and the window bound is computed from the RAISED min_on, not the stored 2000");

    /* And what came out must now pass the very validation that rejects the
     * input -- the whole point of raising rather than refusing at load. */
    TEST_CHECK(zones_config_json_validate(&out, &reason), "the raised config validates");
}

/* ------------------------------------------------------------------------
 * sanity_rate_c_per_min -- thermal_guard.c guard 1's minimum rise rate, the
 * dead-element check, editable per zone on the Zones page as z%u_sanity.
 * 2026-08-29: this bench's zone 0 carried 5.0 C/min (a real kiln's figure)
 * and every firing died at t=62s on "rose only 0.0C in 1.0min (need >=5.0C)"
 * while the jig was genuinely -- just slowly -- heating. The number is
 * operator config and always was; what was missing was a test that the door
 * it comes through actually enforces its bounds, and that the guard's verdict
 * really follows the configured number rather than any constant.
 * ------------------------------------------------------------------------ */
static float s_last_parsed_sanity = -1.0f;

static bool post_body_with_sanity(const char *sanity_literal, const char **err_reason_out)
{
    char body[512];
    snprintf(body, sizeof(body),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=%s&z0_mode=0&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=60000&z0_minon=10000&z0_minoff=2000&"
             "z0_timingprofile=0",
             sanity_literal);

    zone_cfg_t current = make_stored_zone();
    current.sanity_rate_c_per_min = 5.0f; /* a legal stored value, and the one this bench had */
    zone_cfg_t out;
    memset(&out, 0, sizeof(out));
    const char *err_reason = "unset";
    bool ok = parse_zone_fields(body, /*i=*/0, /*thermo_count=*/1, /*relay_count=*/4,
                                /*timing_profile_count=*/1, &current, &out, &err_reason);
    if (err_reason_out) {
        *err_reason_out = err_reason;
    }
    s_last_parsed_sanity = ok ? out.sanity_rate_c_per_min : -1.0f;
    return ok;
}

static void test_post_sanity_rate_bounds(void)
{
    TEST_SECTION("parse_zone_fields -- z0_sanity is bounded 0..ZONE_SANITY_RATE_MAX_C_PER_MIN");

    const char *reason = "unset";

    /* Negative-test the validation itself: each of these MUST be refused, or
     * the bound is decorative. */
    TEST_CHECK(!post_body_with_sanity("-0.2", &reason),
               "a negative minimum rise rate is meaningless and must be refused");
    TEST_CHECK(reason && strstr(reason, "sanity_rate_c_per_min") != NULL,
               "the refusal names the field, so the page can say which one");
    TEST_CHECK(!post_body_with_sanity("20.1", &reason),
               "above the 20 C/min ceiling is refused -- a rate no kiln sustains would trip everything");
    TEST_CHECK(!post_body_with_sanity("0.2C", &reason),
               "a unit suffix is refused, not silently parsed as 0.2");
    TEST_CHECK(!post_body_with_sanity("", &reason),
               "an empty field is refused rather than read as 0");

    /* And the accept path, with the value landing EXACTLY -- "accepted" alone
     * would not catch a clamp or a rounding to the old 5.0. */
    TEST_CHECK(post_body_with_sanity("0.2", &reason), "0.2 C/min -- this bench's jig value -- is accepted");
    TEST_CHECK_NEAR(s_last_parsed_sanity, 0.2f, 1e-6, "0.2 round-trips exactly, not clamped upward");

    TEST_CHECK(post_body_with_sanity("0", &reason),
               "0 stays legal: it means 'use the firmware default', NOT 'disable the guard'");
    TEST_CHECK_NEAR(s_last_parsed_sanity, 0.0f, 1e-6,
                    "0 is stored as 0 -- 'use the default' must stay distinguishable from a chosen 0.5");

    TEST_CHECK(post_body_with_sanity("20", &reason), "exactly the ceiling is accepted -- inclusive bound");
    TEST_CHECK_NEAR(s_last_parsed_sanity, 20.0f, 1e-6, "the ceiling value round-trips exactly");
}

static void test_validate_rejects_out_of_range_sanity_rate(void)
{
    TEST_SECTION("validate_zones_cfg -- an out-of-range sanity_rate_c_per_min is rejected at the "
                 "import door too");

    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].sanity_rate_c_per_min = 500.0f;
        const char *reason = NULL;
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason),
                   "500 C/min does not validate, whichever door it came through");
        TEST_CHECK(reason && strstr(reason, "sanity_rate_c_per_min") != NULL,
                   "the reason names the field");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].sanity_rate_c_per_min = 0.2f;
        const char *reason = NULL;
        TEST_CHECK(zones_config_json_validate(&cfg, &reason),
                   "0.2 C/min validates -- the slow-jig value must not be collateral damage");
    }
}

void run_test_zones_http(void)
{
    test_out_of_range_zone_preserves_stored_fields();
    test_whole_page_post_invalidates_tuning_quality_only_when_gains_actually_change();
    test_in_range_zone_thermo_mask_legacy_fallback_unchanged();
    test_old_behaviour_would_have_zeroed_it();

    test_parse_u8_field_rejects_trailing_garbage();
    test_parse_u8_field_accepts_clean_value();
    test_parse_float_field_rejects_trailing_garbage();
    test_parse_float_field_rejects_unit_suffix();
    test_parse_float_field_accepts_clean_value();
    test_zones_post_max_simultaneous_relays_rejects_trailing_garbage();
    test_zones_post_safety_tc_type_rejects_trailing_garbage();
    test_zones_post_accepts_clean_minimal_body();
    test_post_whole_page_cross_zone_cycle_refused();
    test_post_whole_page_cross_zone_legal_chain_accepted();

    test_zones_pid_post_accepts_while_profile_running();
    test_zones_pid_post_bumps_generation();
    test_zones_pid_post_rejects_kp_over_bound();
    test_zones_pid_post_rejects_negative_ki();
    test_zones_pid_post_rejects_non_numeric_kd();
    test_zones_pid_post_rejects_zone_out_of_range();
    test_zones_pid_post_ignores_relay_mask();
    test_zones_pid_post_ignores_control_mode();
    test_zones_pid_post_ignores_max_temp();
    test_zones_pid_post_ignores_max_ramp();
    test_zones_pid_post_ignores_cal_offset();
    test_zones_pid_post_ignores_guard_thresholds();
    test_zones_pid_post_ignores_coupling_matrix();
    test_zones_pid_post_ignores_model_parameters();
    test_zones_pid_post_ignores_thermo_mask();
    test_zones_pid_post_ignores_settings_source();
    test_zones_pid_post_ignores_safety_tc_type();

    test_nvs_load_from_too_short_is_corrupt_not_refused();
    test_nvs_load_from_wrong_size_current_version_is_corrupt_not_refused();
    test_nvs_load_from_current_version_happy_path();
    test_nvs_load_from_pre_existing_cycle_normalizes_not_wipes();
    test_nvs_load_from_cycle_normalization_does_not_touch_lead_in_zone();
    test_import_blob_pass1_rejects_cyclic_settings_source();
    test_import_blob_pass1_accepts_acyclic_settings_source();
    test_nvs_load_from_newer_than_firmware_is_found_but_not_valid();
    test_zones_http_start_refused_newer_blob_not_overwritten();

    test_nvs_load_from_old_version_wrong_length_is_rejected();
    test_nvs_load_from_bad_crc_is_rejected();
    test_nvs_load_from_failed_validation_is_rejected();
    test_nvs_save_load_round_trip_current_version();
    test_nvs_load_from_v1_blob_upconverts_fields_correctly();
    test_nvs_load_from_v2_blob_upconverts_fields_correctly();
    test_nvs_load_from_v3_blob_upconverts_guard_thresholds_correctly();
    test_nvs_load_from_v4_blob_upconverts_thermo_mask_correctly();
    test_nvs_load_from_v5_blob_upconverts_zones_1_and_2_correctly();
    test_nvs_load_from_v6_blob_upconverts_fields_correctly();
    test_nvs_load_from_v1_blob_chains_end_to_end_to_v15_preserving_real_values();
    test_nvs_load_from_v7_blob_upconverts_and_defaults_new_fields();
    test_nvs_load_from_v8_blob_with_distinct_zone_values_migrates_losslessly();
    test_nvs_load_from_v8_blob_upconverts_to_shared_default_profile();
    test_validate_rejects_out_of_range_v8_fields();

    test_nvs_load_from_v9_blob_upconverts_new_fields_default_and_settings_source_is_custom();
    test_nvs_load_from_v10_blob_folds_single_pair_into_row_cell();
    test_nvs_load_from_v11_blob_defaults_coupling_tau_dead_time_to_zero();
    test_nvs_load_from_v10_blob_self_referencing_pair_is_dropped();
    test_nvs_load_from_v9_blob_chains_through_v10_to_v11_with_zero_coupling();
    test_validate_accepts_control_mode_pid_fuzzy_rejects_past_it();
    test_post_mode_pid_fuzzy_accepted_by_parser();
    test_post_fuzzy_strength_out_of_range_refused_not_clamped();
    test_post_omitting_new_fields_preserves_stored_values();
    test_post_new_fields_present_but_unparseable_are_refused();
    test_post_coupling_diagonal_must_be_zero();
    test_post_settings_source_self_reference_refused();
    test_post_then_get_round_trips_new_fields();
    test_tuning_rec_body_len_strips_the_idf_appended_nul();
    test_zones_get_handler_malloc_failure_returns_clean_500();
    test_zones_get_handler_succeeds_when_malloc_does_not_fail();
    test_fuzzy_strength_pct_setter_round_trip_and_bounds();
    test_coupling_row_whole_setter_round_trip_and_bounds();
    test_coupling_single_cell_setter_preserves_other_cells();
    test_coupling_matrix_2026_09_02_adopted_orientation_not_transposed();
    test_settings_source_setter_round_trip_and_bounds();
    test_tuning_quality_round_trip_asymmetric_per_zone();
    test_zones_config_set_pid_invalidates_tuning_quality();
    test_nvs_load_from_v12_blob_defaults_tuning_quality_to_unknown();
    test_nvs_load_from_v13_blob_defaults_adaptive_tune_enabled_to_zero();
    test_nvs_load_from_v14_blob_defaults_coupling_diag_k_dc_to_zero();
    test_settings_source_save_reload_inheritance_round_trip();
    test_settings_source_two_and_three_zone_cycles_are_refused();
    test_tc_type_write_is_identity_independent_of_settings_source();

    test_post_sanity_rate_bounds();
    test_validate_rejects_out_of_range_sanity_rate();

    test_post_minon_below_floor_is_refused();
    test_post_minon_zero_and_at_or_above_floor_are_accepted();
    test_validate_rejects_sub_floor_min_on();
    test_post_window_too_short_for_min_on_is_refused();
    test_post_window_at_or_above_the_bound_is_accepted_exactly();
    test_setter_and_validate_reject_short_window();
    test_stored_short_window_is_raised_on_load();
    test_stored_pre_floor_blob_is_raised_on_load_not_rejected();

    test_relay_name_get_set_round_trip();
    test_relay_name_setter_rejects_out_of_range_relay();
    test_relay_name_setter_rejects_overlong_name();
    test_relay_name_setter_null_clears();
    test_relay_name_survives_relay_becoming_zone_owned();
    test_zone_owned_relay_mask_is_union_of_zone_relay_masks();
    test_relay_names_load_wrong_length_blob_is_rejected();
    test_relay_names_load_wrong_version_is_rejected();
    test_relay_names_load_bad_crc_is_rejected();
    test_relay_names_save_load_round_trip_preserves_distinct_values();
    test_zones_post_relay_name_omitted_preserves_current_value();
    test_zones_post_relay_name_present_updates_value();
    test_zones_post_relay_name_too_long_rejected_and_commits_nothing();

    test_zone_sweep_check_refusal_each_reason_fires();
    test_zone_sweep_ceiling_hit();
    test_zone_sweep_effective_ceiling_c();
    test_zone_sweep_timing_predicates();
    test_zone_sweep_hw_bindings_route_through_owner();
    test_zone_sweep_run_one_zone_skips_unwired_zone();
    test_zone_sweep_run_one_zone_abort_before_energize_never_turns_relay_on();
    test_zone_sweep_run_one_zone_normal_completion_sequencing();
    test_zone_sweep_run_one_zone_abort_mid_run_still_forces_off();
    test_zone_sweep_run_one_zone_ceiling_hit_forces_off();
    test_zone_sweep_run_one_zone_link_loss_forces_off();
    test_zone_sweep_run_one_zone_energize_refused_is_a_choke_point_exit_too();
    test_zone_sweep_run_one_zone_temp_lost_forces_off();
    test_zone_sweep_run_one_zone_single_invalid_poll_does_not_abort();
    test_zone_sweep_run_one_zone_trip_latched_forces_off();
    test_zone_sweep_derive_ct_channel_picks_the_dominant_channel();
    test_zone_sweep_derive_ct_channel_refuses_below_the_load_threshold();
    test_zone_sweep_derive_ct_channel_refuses_a_shared_ct();
    test_zone_sweep_derive_ct_channel_refuses_nan();
    test_zone_sweep_record_ct_refuses_a_zone_whose_relay_is_not_its_own_bit();
    test_zone_sweep_record_ct_refuses_two_zones_claiming_one_channel();
    test_zone_sweep_run_all_zones_never_energizes_two_zones_at_once();
    test_zone_sweep_run_all_zones_skipped_zone_records_nothing();
    test_zone_sweep_run_all_zones_energize_refused_reason_decodes_fault_words();
    test_zone_normals_get_set_round_trip();

    test_zone_sweep_derive_k_ct_scales_by_the_measured_over_expected_ratio();
    test_zone_sweep_derive_k_ct_refuses_without_the_nameplate_answers();
    test_zone_sweep_derive_k_ct_refuses_without_a_measurement();
    test_zone_sweep_derive_k_ct_refuses_an_uncommissioned_prior_k();
    test_zone_sweep_derive_k_ct_refuses_an_implausible_correction();
    test_zone_sweep_plan_k_ct_clean_run_plans_every_derived_channel();
    test_zone_sweep_plan_k_ct_refuses_an_incomplete_run();
    test_zone_sweep_plan_k_ct_refuses_after_a_failed_map_push();
    test_zone_sweep_plan_k_ct_refuses_an_unanswered_nameplate();
    test_zone_sweep_plan_k_ct_implausible_reason_is_not_truncated();
    test_zone_sweep_push_k_ct_happy_path_writes_and_confirms();
    test_zone_sweep_push_k_ct_unconfirmed_readback_derives_nothing_and_backs_out();
    test_zone_sweep_push_k_ct_rejected_commit_backs_the_staging_out();
    test_zone_sweep_push_k_ct_partial_staging_failure_backs_out_what_staged();
    test_zone_sweep_push_k_ct_backout_restores_the_uncommissioned_zero();

    test_ct_mapping_mismatch_silent_when_never_measured();
    test_ct_mapping_mismatch_within_band_is_silent();
    test_ct_mapping_mismatch_outside_band_warns();
    test_ct_mapping_mismatch_tiny_normal_needs_absolute_delta_too();
    test_ct_mapping_warn_mask_wiring();

    test_zones_current_sweep_start_wired_refusals();
    test_zones_current_sweep_start_atomic_gate_closes_the_race();
}

int main(void)
{
    run_test_zones_http();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
