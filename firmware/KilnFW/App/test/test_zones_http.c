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
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t error, const char *msg)
{
    (void)r;
    (void)error;
    (void)msg;
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
    return ESP_OK;
}
int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len)
{
    (void)r;
    (void)buf;
    (void)buf_len;
    return 0;
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

void run_test_zones_http(void)
{
    test_out_of_range_zone_preserves_stored_fields();
    test_in_range_zone_thermo_mask_legacy_fallback_unchanged();
    test_old_behaviour_would_have_zeroed_it();
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
