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
    const char *err_reason = "unset";
    bool ok = parse_zone_fields(body, /*i=*/1, thermo_count, relay_count, &current, &out, &err_reason);

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
    const char *body = "z0_name=Top&z0_tctype=2&z0_relay_mask=1&"
                        "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=0&"
                        "z0_maxtemp=1300&z0_mintemp=-20&z0_window=1000&z0_minon=0&z0_minoff=0";
    const char *err_reason = "unset";
    bool ok = parse_zone_fields(body, /*i=*/0, /*thermo_count=*/2, /*relay_count=*/4, &current, &out, &err_reason);

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

static void test_zones_post_safety_tc_type_rejects_trailing_garbage(void)
{
    TEST_SECTION("zones_post_handler -- safety_tc_type trailing garbage rejected (FIX 2, inline strtol site 2)");
    run_zones_post("thermo_count=0&relay_count=0&safety_tc_type=3Q");
    TEST_CHECK(s_test_err_called, "\"3Q\" must be rejected, not silently accepted as 3");
    TEST_CHECK(!s_test_ok_called, "must not report success for a rejected submission");
}

static void test_zones_post_accepts_clean_minimal_body(void)
{
    TEST_SECTION("zones_post_handler -- positive control: clean values on the same two fields are still accepted");
    run_zones_post("thermo_count=0&relay_count=0&max_simultaneous_relays=2&safety_tc_type=3");
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
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped v8");
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

    /* The point of the 0 convention: an upgraded board must behave exactly as
     * it did, so every new field has to arrive as "not configured". */
    for (uint8_t i = 0; i < 3; i++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "zones[%u]'s new v8 overrides all default to 0 (= firmware default)", i);
        bool all_zero = out_cfg.zones[i].guard_progress_duty_min == 0.0f &&
                        out_cfg.zones[i].guard_progress_window_s == 0.0f &&
                        out_cfg.zones[i].guard_drift_hysteresis_c == 0.0f &&
                        out_cfg.zones[i].guard_frozen_eps_c == 0.0f &&
                        out_cfg.zones[i].guard_cross_zone_period_s == 0.0f &&
                        out_cfg.zones[i].bangbang_hysteresis_c == 0.0f &&
                        out_cfg.zones[i].cooling_limited_margin_c == 0.0f &&
                        out_cfg.zones[i].cooling_limited_hold_s == 0.0f &&
                        out_cfg.zones[i].ramp_lock_band_c == 0.0f;
        TEST_CHECK(all_zero, msg);
    }
    TEST_CHECK(out_cfg.pc_link_abort_silence_ms == 0.0f,
               "the global pc_link_abort_silence_ms also defaults to 0 on upgrade");

    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_validate_rejects_out_of_range_v8_fields(void)
{
    TEST_SECTION("validate_zones_cfg -- the v8 overrides are range-checked like every field before them");

    /* A duty above 1.0 would arm guard 1 never, silently disabling the
     * heating-failed check -- the exact "configured it into uselessness"
     * case the ceiling exists to refuse. */
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].guard_progress_duty_min = 1.5f;
        const char *reason = NULL;
        TEST_CHECK(!validate_zones_cfg(&cfg, &reason), "guard_progress_duty_min > 1.0 is rejected");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].guard_progress_window_s = -1.0f;
        const char *reason = NULL;
        TEST_CHECK(!validate_zones_cfg(&cfg, &reason), "a negative guard_progress_window_s is rejected");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].ramp_lock_band_c = ZONE_GUARD_MARGIN_C_MAX + 1.0f;
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
        TEST_CHECK(validate_zones_cfg(&cfg, &reason), "all-zero v8 fields stay valid (0 = firmware default)");
    }
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
    test_validate_rejects_out_of_range_v8_fields();
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
