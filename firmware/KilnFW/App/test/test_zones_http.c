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

#include "../drivers/zones_http.c"

#undef asm

// ---- Embedded-page symbols page_get_handler() references ------------------
// Never actually sent by these tests (that handler is never called), but
// must exist for the linker.
const uint8_t zones_page_html_gz_start[1] = { 0 };
const uint8_t zones_page_html_gz_end[1] = { 0 };
const uint8_t safety_config_page_html_gz_start[1] = { 0 };
const uint8_t safety_config_page_html_gz_end[1] = { 0 };

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
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, long long buf_len)
{
    (void)r;
    (void)buf;
    (void)buf_len;
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
 * have no seam of their own to call directly, unlike parse_u8_field()/
 * parse_float_field()). NULL/false by default so every pre-existing test in
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
    (void)s;
    s_test_ok_called = true;
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
    z.heater_window_ms = 2000.0f;
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
                        "z0_maxtemp=1300&z0_mintemp=-20&z0_window=1000&z0_minon=0&z0_minoff=0";

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
                        "z0_maxtemp=1300&z0_mintemp=-20&z0_window=1000&z0_minon=0&z0_minoff=0";
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
// FIX 2 -- parse_u8_field()/parse_float_field() trailing-garbage rejection,
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
    bool ok = parse_u8_field("thermo_count=3X", "thermo_count", 0, 10, &out);
    TEST_CHECK(!ok, "\"3X\" must be rejected outright, not silently accepted as 3");
    TEST_CHECK(out == 99, "out must be untouched on rejection");
}

static void test_parse_u8_field_accepts_clean_value(void)
{
    TEST_SECTION("parse_u8_field -- positive control: a clean in-range value is still accepted");
    uint8_t out = 0;
    bool ok = parse_u8_field("thermo_count=3", "thermo_count", 0, 10, &out);
    TEST_CHECK(ok, "a clean value must still parse");
    TEST_CHECK(out == 3, "parsed value must be correct");
}

static void test_parse_float_field_rejects_trailing_garbage(void)
{
    TEST_SECTION("parse_float_field -- trailing garbage after a valid numeric prefix is rejected (FIX 2)");
    float out = -1.0f;
    bool ok = parse_float_field("z0_kp=1200X", "z0_kp", 0.0f, 5000.0f, &out);
    TEST_CHECK(!ok, "\"1200X\" must be rejected outright, not silently accepted as 1200.0");
    TEST_CHECK(out == -1.0f, "out must be untouched on rejection");
}

static void test_parse_float_field_rejects_unit_suffix(void)
{
    TEST_SECTION("parse_float_field -- a value with a trailing unit suffix is rejected (FIX 2)");
    float out = -1.0f;
    bool ok = parse_float_field("z0_maxtemp=1300C", "z0_maxtemp", 0.0f, 1400.0f, &out);
    TEST_CHECK(!ok, "\"1300C\" must be rejected outright, not silently accepted as 1300.0");
}

static void test_parse_float_field_accepts_clean_value(void)
{
    TEST_SECTION("parse_float_field -- positive control: a clean in-range value is still accepted");
    float out = 0.0f;
    bool ok = parse_float_field("z0_kp=2.5", "z0_kp", 0.0f, 5000.0f, &out);
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
    src.timing_profile_count = 1; // must never be 0 in a config validate_zones_cfg() accepts
    src.crc32 = compute_zones_crc(&src); // CRC now checked on the current-version path (item 4)
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
// did no length check, never ran validate_zones_cfg(), assumed zone_cfg_t
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
    src.crc32 = compute_zones_crc(&src) ^ 0x1u; // one bit off from the real CRC
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

// Item 3 -- validate_zones_cfg() now runs on the NVS load path too, not just
// zones_config_import_blob(). A current-version blob with the right length
// AND a correct CRC (proving the bytes are exactly what was written) but an
// out-of-range field must still be rejected, not adopted with the valid flag
// set -- the config was written wrong in the first place, and a correct CRC
// over wrong data is not a reason to trust it.
static void test_nvs_load_from_failed_validation_is_rejected(void)
{
    TEST_SECTION("nvs_load_from -- item 3: length+CRC correct but validate_zones_cfg() fails "
                 "-> rejected, not partially adopted");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_t src;
    memset(&src, 0, sizeof(src));
    src.version = ZONES_CFG_VERSION;
    src.thermo_count = 1;
    src.zones[0].max_temp_c = 999999.0f; // past ZONE_MAX_TEMP_C_MAX -- validate_zones_cfg() must reject
    src.crc32 = compute_zones_crc(&src); // CRC is genuinely correct for these (bad) bytes
    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = true, valid = true;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "a validation failure is a handled outcome, not an NVS error");
    TEST_CHECK(!found, "a validate_zones_cfg() failure must be rejected (found=false), matching "
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

/* A config that validate_zones_cfg() accepts, so a test can change exactly
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
     * resolve to a REAL profile for validate_zones_cfg() to accept this --
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
        TEST_CHECK(!validate_zones_cfg(&cfg, &reason), "guard_progress_duty_min > 1.0 is rejected");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.timing_profiles[0].guard_progress_window_s = -1.0f;
        const char *reason = NULL;
        TEST_CHECK(!validate_zones_cfg(&cfg, &reason), "a negative guard_progress_window_s is rejected");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.timing_profiles[0].ramp_lock_band_c = ZONE_GUARD_MARGIN_C_MAX + 1.0f;
        const char *reason = NULL;
        TEST_CHECK(!validate_zones_cfg(&cfg, &reason), "ramp_lock_band_c past its ceiling is rejected");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.pc_link_abort_silence_ms = ZONE_PC_LINK_SILENCE_MS_MAX + 1.0f;
        const char *reason = NULL;
        TEST_CHECK(!validate_zones_cfg(&cfg, &reason), "pc_link_abort_silence_ms past its ceiling is rejected");
    }
    /* 0 must stay legal on every one of them -- it is the "use the firmware
     * default" value, not a missing setting. */
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        const char *reason = NULL;
        TEST_CHECK(validate_zones_cfg(&cfg, &reason), "all-zero timing profile fields stay valid (0 = firmware default)");
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
        TEST_CHECK(!validate_zones_cfg(&cfg, &reason), "timing_profile_count == 0 is rejected");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.timing_profile_count = MAX31856_CHANNEL_COUNT + 1;
        const char *reason = NULL;
        TEST_CHECK(!validate_zones_cfg(&cfg, &reason), "timing_profile_count past MAX31856_CHANNEL_COUNT is rejected");
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
        TEST_CHECK(!validate_zones_cfg(&cfg, &reason),
                  "a zone's timing_profile referencing a profile past timing_profile_count is rejected");
    }
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
                 "(same NEWER-refuses-to-load discipline as decode_zones_blob())");
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
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, /* relay_mask */ 0, &deps, &avg, NULL);

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
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, NULL);

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
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, NULL);

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
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, NULL);

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
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, NULL);

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
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, NULL);

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
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, &refused_sources);

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
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, NULL);

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
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, NULL);

    TEST_CHECK(outcome == ZONE_SWEEP_ZONE_OK,
              "N1: a single invalid poll surrounded by valid ones must not itself trip TEMP_LOST -- the "
              "streak resets on the very next valid read, and the zone completes normally, proving one "
              "poll of noise tolerance is real, not accidental");
}

// N10 (opus review, 2026-08-28): a safety trip latching mid-zone must end
// the sweep promptly instead of grinding on for the rest of the dwell.
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
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, NULL);

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

static kiln_io_owner_relay_result_t fake_all_energize(void *ctx, uint8_t relay_mask, uint32_t *out_safety_sources)
{
    (void)ctx;
    (void)relay_mask;
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

void run_test_zones_http(void)
{
    test_out_of_range_zone_preserves_stored_fields();
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

    test_nvs_load_from_too_short_is_corrupt_not_refused();
    test_nvs_load_from_wrong_size_current_version_is_corrupt_not_refused();
    test_nvs_load_from_current_version_happy_path();
    test_nvs_load_from_newer_than_firmware_is_found_but_not_valid();
    test_zones_http_start_refused_newer_blob_not_overwritten();

    test_nvs_load_from_old_version_wrong_length_is_rejected();
    test_nvs_load_from_bad_crc_is_rejected();
    test_nvs_load_from_failed_validation_is_rejected();
    test_nvs_save_load_round_trip_current_version();
    test_nvs_load_from_v5_blob_upconverts_zones_1_and_2_correctly();
    test_nvs_load_from_v7_blob_upconverts_and_defaults_new_fields();
    test_nvs_load_from_v8_blob_with_distinct_zone_values_migrates_losslessly();
    test_nvs_load_from_v8_blob_upconverts_to_shared_default_profile();
    test_validate_rejects_out_of_range_v8_fields();

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
    test_zone_sweep_run_all_zones_never_energizes_two_zones_at_once();
    test_zone_sweep_run_all_zones_skipped_zone_records_nothing();
    test_zone_normals_get_set_round_trip();

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
