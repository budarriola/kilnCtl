// Host tests for App/drivers/backup_http.c's import validation pass
// (backup_import_apply()), added 2026-08-21 alongside backup_http.c itself.
//
// backup_import_apply() is `static` with no public seam -- like
// wifi_prov.c's do_ev_got_ip()/do_confirm_static_reachable(), the only way
// to exercise it directly is to #include backup_http.c into this test file
// (see test_wifi_prov.c's header comment for the same convention). Doing so
// pulls in backup_http.c's OTHER handlers too (the page GET, the export GET,
// the import POST wrapper) even though this file only ever calls
// backup_import_apply() -- they still have to COMPILE and LINK, which is
// why this file's stub surface below is wider than what backup_import_apply()
// itself touches.
//
// backup_import_apply()'s only real dependency is a pile of read-only
// getters and validating setters on zones_http.h/profiles_http.h -- pure
// config storage, no I/O -- so, same as test_profile_feasibility.c, those are
// stubbed with plain C functions the tests drive directly, not mocked.
//
// Two things this pulls in that no other host test has needed before today:
//   - backup_http.c's `extern ... asm("_binary_...")` embedded-page symbols
//     (EMBED_TXTFILES is a CMake/GCC convention with no MSVC equivalent) --
//     `asm` is #define'd away to nothing before including backup_http.c, and
//     the two symbols it would otherwise declare are given real (empty)
//     definitions below instead.
//   - ota_http.h's transitive drag-in of kiln_io.h/safety_link.h/MAX31856.h,
//     which name real ESP-IDF I2C/UART driver types no other host test's
//     include chain reaches. App/test/stubs/driver/i2c_master.h,
//     stubs/i2c_owner.h, stubs/uart_owner.h, and stubs/uart_protocol.h are
//     new, minimal (opaque-enough-to-compile, never instantiated) type
//     stand-ins added for exactly this -- see each file's own header comment.
//
// Collision note: test_profile_feasibility.c already defines
// zones_config_get_thermo_count()/zones_config_get_max_ramp()/
// zones_config_get_model() as plain (non-static) C functions linked into
// this same host-test executable -- redefining any of them here would be a
// multiple-definition link error. This file does not call
// zones_config_get_model() (backup_import_apply() never does; only the
// export path, which these tests don't exercise, does), and drives the
// other two through test_stub_zones_set_thermo_count()/
// test_stub_zones_set_max_ramp(), two small hooks added to
// test_profile_feasibility.c for exactly this cross-file sharing.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "test_common.h"

// Ahead of backup_http.c's own #includes below, purely for the TYPES these
// stub bodies need (same convention test_wifi_prov.c uses for esp_wifi.h/
// esp_http_server.h).
#include "esp_err.h"
#include "esp_http_server.h"

// asm("_binary_...") is a GCC/binutils extension (EMBED_TXTFILES,
// CMakeLists.txt) with no MSVC equivalent -- #define it away to nothing so
// `extern const uint8_t X[] asm("...");` parses as plain
// `extern const uint8_t X[];`. Real (empty) definitions follow.
#define asm(x)

// Defined in test_profile_feasibility.c -- see this file's header comment
// for why the shared zones_config_get_thermo_count()/get_max_ramp() state
// is driven through these two hooks instead of being redefined here.
void test_stub_zones_set_thermo_count(uint8_t n);
void test_stub_zones_set_max_ramp(uint8_t zone_index, bool answers, float c_per_hr);

#include "../drivers/backup_http.c"

#undef asm

// ---- Embedded-page symbols backup_page_get_handler() references ----------
// Never actually sent by these tests (that handler is never called), but
// must exist for the linker.
const uint8_t backup_page_html_gz_start[1] = { 0 };
const uint8_t backup_page_html_gz_end[1] = { 0 };

// ---- esp_http_server.h stub bodies ----------------------------------------
// Declared in stubs/esp_http_server.h, defined here (see that header's
// comment) -- none of these is ever actually invoked by this file's tests,
// since only backup_import_apply() is called, not the httpd handlers around
// it, but every symbol backup_http.c references must resolve at link time.
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
int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len)
{
    (void)r;
    (void)buf;
    (void)buf_len;
    return 0;
}

// wifi_provision_http_get_server() is already defined by test_wifi_prov.c
// (also linked into this executable) -- do NOT redefine it here.

// web_encoding.h's two functions -- only reached from backup_page_get_handler(),
// never called by these tests, but must resolve.
bool web_client_accepts_gzip(httpd_req_t *req)
{
    (void)req;
    return true;
}
esp_err_t web_send_gzip_not_acceptable(httpd_req_t *req, const char *tag, const char *page_name)
{
    (void)req;
    (void)tag;
    (void)page_name;
    return ESP_OK;
}

// ota_http.h's interlock check -- only reached from backup_import_post_handler(),
// which these tests never call (they call backup_import_apply() directly).
// Controllable via the two globals below (default OK/"", unchanged from this
// function's original hardcoded body) rather than a second, colliding
// definition, because test_kiln_cfg_store.c (linked into the same host-test
// executable, 2026-08-21) needs this same symbol to be able to report
// OTA_INTERLOCK_REFUSED for its own interlock-backstop regression test --
// exactly one definition of ota_http_check_interlocks() may exist across the
// whole link, so it lives here and is driven by state, not duplicated.
ota_interlock_result_t g_stub_ota_interlock_result = OTA_INTERLOCK_OK;
char g_stub_ota_interlock_reason[OTA_INTERLOCK_REASON_MAX] = "";

// g_stub_ota_interlock_saw_ack records the ack argument the code under test
// passed, so a test can prove the acknowledgement is actually threaded
// through rather than dropped on the floor somewhere between the handler and
// the store.
bool g_stub_ota_interlock_saw_ack = false;

ota_interlock_result_t ota_http_check_interlocks(bool ack_no_safety_processor, char *reason_out,
                                                 size_t reason_cap)
{
    g_stub_ota_interlock_saw_ack = ack_no_safety_processor;
    if (reason_out && reason_cap) {
        strncpy(reason_out, g_stub_ota_interlock_reason, reason_cap - 1);
        reason_out[reason_cap - 1] = '\0';
    }
    return g_stub_ota_interlock_result;
}

// The other two ota_http.h symbols backup_http.c's handler now references.
// Neither is reachable from these tests -- they live in
// backup_import_post_handler(), the HTTP entry point, while every test here
// calls backup_import_apply() directly -- but the linker still needs a body
// for each. Deliberately trivial: if a future test ever does exercise the
// handler, these firing would be the signal that this stub needs real
// behaviour rather than silently passing.
bool ota_http_req_ack_no_safety(httpd_req_t *req)
{
    (void)req;
    return false;
}

esp_err_t ota_http_send_interlock_refusal(httpd_req_t *req, ota_interlock_result_t r,
                                          const char *reason)
{
    (void)req;
    (void)r;
    (void)reason;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// zones_http.h stub state -- one entry per MAX31856_CHANNEL_COUNT zone.
// ---------------------------------------------------------------------------

#define STUB_ZONE_COUNT MAX31856_CHANNEL_COUNT

typedef struct {
    bool set_pid_called;
    float kp, ki, kd;
    bool set_model_called;
    float k_dc, tau_s, dead_time_s;
    bool set_tc_called;
    uint8_t tc_type;
    bool set_name_called;
    char name[64];
    bool set_relay_mask_called;
    uint8_t relay_mask;
    bool set_thermo_mask_called;
    uint8_t thermo_mask;
    bool set_ct_mask_called;
    uint8_t ct_mask;
    bool set_cal_called;
    float cal_offset_c;
    bool set_ramp_called;
    float max_ramp_c_per_hr;
    bool set_sanity_called;
    float sanity_rate_c_per_min;
    bool set_mode_called;
    uint8_t control_mode;
    bool set_temp_limits_called;
    float max_temp_c, min_temp_c;
    bool set_heater_called;
    float heater_window_ms, heater_min_on_ms, heater_min_off_ms;
    bool set_guard_called;
    float guard[8];
    bool set_xzone_called;
    float cross_zone_max_delta_c;
} zone_write_t;

static zone_write_t s_writes[STUB_ZONE_COUNT];
static uint8_t s_relay_count = 4;
static bool s_safety_tc_set;
static uint8_t s_safety_tc_type;
static int g_total_write_calls;
static int g_profile_save_calls;
static uint8_t g_last_saved_profile_id;
static profile_t g_last_saved_profile;

static void reset_stub_state(void)
{
    memset(s_writes, 0, sizeof(s_writes));
    s_relay_count = 4;
    s_safety_tc_set = false;
    s_safety_tc_type = 0;
    g_total_write_calls = 0;
    g_profile_save_calls = 0;
    g_last_saved_profile_id = 0;
    memset(&g_last_saved_profile, 0, sizeof(g_last_saved_profile));
    test_stub_zones_set_thermo_count(3);
    for (uint8_t i = 0; i < STUB_ZONE_COUNT; i++) {
        test_stub_zones_set_max_ramp(i, true, 1000.0f);
    }
}

// ---- zones_http.h getters this TU needs (not already supplied elsewhere) --

uint8_t zones_config_get_relay_count(void)
{
    return s_relay_count;
}

bool zones_config_get_pid(uint8_t zone_index, float *out_kp, float *out_ki, float *out_kd)
{
    if (zone_index >= STUB_ZONE_COUNT) {
        return false;
    }
    if (out_kp) *out_kp = s_writes[zone_index].kp;
    if (out_ki) *out_ki = s_writes[zone_index].ki;
    if (out_kd) *out_kd = s_writes[zone_index].kd;
    return s_writes[zone_index].set_pid_called;
}

bool zones_config_get_tc_type(uint8_t zone_index, uint8_t *out_tc_type)
{
    if (zone_index >= STUB_ZONE_COUNT || !s_writes[zone_index].set_tc_called) {
        return false;
    }
    *out_tc_type = s_writes[zone_index].tc_type;
    return true;
}

bool zones_config_get_name(uint8_t zone_index, char *out, size_t out_cap)
{
    if (!out || out_cap == 0 || zone_index >= STUB_ZONE_COUNT) {
        return false;
    }
    strncpy(out, s_writes[zone_index].name, out_cap - 1);
    out[out_cap - 1] = '\0';
    return true;
}

bool zones_config_get_relay_mask(uint8_t zone_index, uint8_t *out_mask)
{
    if (!out_mask || zone_index >= STUB_ZONE_COUNT) {
        return false;
    }
    *out_mask = s_writes[zone_index].relay_mask;
    return true;
}

bool zones_config_get_thermo_mask(uint8_t zone_index, uint8_t *out_mask)
{
    if (!out_mask || zone_index >= STUB_ZONE_COUNT) {
        return false;
    }
    *out_mask = s_writes[zone_index].thermo_mask;
    return true;
}

bool zones_config_get_ct_mask(uint8_t zone_index, uint8_t *out_mask)
{
    if (!out_mask || zone_index >= STUB_ZONE_COUNT) {
        return false;
    }
    *out_mask = s_writes[zone_index].ct_mask;
    return true;
}

bool zones_config_get_cal_offset(uint8_t zone_index, float *out_cal_offset_c)
{
    if (!out_cal_offset_c || zone_index >= STUB_ZONE_COUNT) {
        return false;
    }
    *out_cal_offset_c = s_writes[zone_index].cal_offset_c;
    return true;
}

bool zones_config_get_sanity_rate(uint8_t zone_index, float *out_c_per_min)
{
    if (!out_c_per_min || zone_index >= STUB_ZONE_COUNT) {
        return false;
    }
    *out_c_per_min = s_writes[zone_index].sanity_rate_c_per_min;
    return true;
}

bool zones_config_get_control_mode(uint8_t zone_index, zone_control_mode_t *out_mode)
{
    if (!out_mode || zone_index >= STUB_ZONE_COUNT) {
        return false;
    }
    *out_mode = (zone_control_mode_t)s_writes[zone_index].control_mode;
    return true;
}

bool zones_config_get_temp_limits(uint8_t zone_index, float *out_max_temp_c, float *out_min_temp_c)
{
    if (!out_max_temp_c || !out_min_temp_c || zone_index >= STUB_ZONE_COUNT) {
        return false;
    }
    *out_max_temp_c = s_writes[zone_index].max_temp_c;
    *out_min_temp_c = s_writes[zone_index].min_temp_c;
    return true;
}

/* v8 overrides. Settable so a test can prove a configured value actually
 * reaches the control path; defaults to "nothing configured", which is what
 * every pre-existing test in this file assumes. */
float g_stub_guard_extra[5] = {0, 0, 0, 0, 0};
bool g_stub_guard_extra_present = false;
float g_stub_exec_thresholds[4] = {0, 0, 0, 0};
bool g_stub_exec_thresholds_present = false;
float g_stub_pc_link_silence_ms = 0.0f;

bool zones_config_get_guard_extra(uint8_t zone_index, float *o1, float *o2, float *o3, float *o4, float *o5)
{
    (void)zone_index;
    if (o1) *o1 = g_stub_guard_extra[0];
    if (o2) *o2 = g_stub_guard_extra[1];
    if (o3) *o3 = g_stub_guard_extra[2];
    if (o4) *o4 = g_stub_guard_extra[3];
    if (o5) *o5 = g_stub_guard_extra[4];
    return g_stub_guard_extra_present;
}

bool zones_config_get_executor_thresholds(uint8_t zone_index, float *o1, float *o2, float *o3, float *o4)
{
    (void)zone_index;
    if (o1) *o1 = g_stub_exec_thresholds[0];
    if (o2) *o2 = g_stub_exec_thresholds[1];
    if (o3) *o3 = g_stub_exec_thresholds[2];
    if (o4) *o4 = g_stub_exec_thresholds[3];
    return g_stub_exec_thresholds_present;
}

bool zones_config_get_pc_link_abort_silence_ms(float *out_ms)
{
    if (out_ms) *out_ms = g_stub_pc_link_silence_ms;
    return true;
}

bool zones_config_get_heater_cfg(uint8_t zone_index, float *out_window_ms, float *out_min_on_ms,
                                 float *out_min_off_ms)
{
    if (!out_window_ms || !out_min_on_ms || !out_min_off_ms || zone_index >= STUB_ZONE_COUNT) {
        return false;
    }
    *out_window_ms = s_writes[zone_index].heater_window_ms;
    *out_min_on_ms = s_writes[zone_index].heater_min_on_ms;
    *out_min_off_ms = s_writes[zone_index].heater_min_off_ms;
    return true;
}

bool zones_config_get_guard_thresholds(uint8_t zone_index, float *o1, float *o2, float *o3, float *o4, float *o5,
                                       float *o6, float *o7, float *o8)
{
    if (!o1 || !o2 || !o3 || !o4 || !o5 || !o6 || !o7 || !o8 || zone_index >= STUB_ZONE_COUNT) {
        return false;
    }
    const float *g = s_writes[zone_index].guard;
    *o1 = g[0]; *o2 = g[1]; *o3 = g[2]; *o4 = g[3]; *o5 = g[4]; *o6 = g[5]; *o7 = g[6]; *o8 = g[7];
    return true;
}

bool zones_config_get_cross_zone_delta(uint8_t zone_index, float *out_max_delta_c)
{
    if (!out_max_delta_c || zone_index >= STUB_ZONE_COUNT) {
        return false;
    }
    *out_max_delta_c = s_writes[zone_index].cross_zone_max_delta_c;
    return true;
}

bool zones_config_get_safety_tc_type(uint8_t *out_tc_type)
{
    if (!out_tc_type) {
        return false;
    }
    *out_tc_type = s_safety_tc_type;
    return true;
}

// ---- zones_http.h setters -- every one records the call and bumps the
// shared write counter, matching the real setters' "false means rejected,
// nothing written" shape (always true here since backup_import_apply()'s
// own pass 1 has already validated everything by the time pass 2 calls
// these). ----

bool zones_config_set_pid(uint8_t zone_index, float kp, float ki, float kd)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_pid_called = true;
    s_writes[zone_index].kp = kp;
    s_writes[zone_index].ki = ki;
    s_writes[zone_index].kd = kd;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_model(uint8_t zone_index, float k_dc, float tau_s, float dead_time_s)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_model_called = true;
    s_writes[zone_index].k_dc = k_dc;
    s_writes[zone_index].tau_s = tau_s;
    s_writes[zone_index].dead_time_s = dead_time_s;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_tc_type(uint8_t zone_index, uint8_t tc_type)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_tc_called = true;
    s_writes[zone_index].tc_type = tc_type;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_name(uint8_t zone_index, const char *name)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_name_called = true;
    strncpy(s_writes[zone_index].name, name ? name : "", sizeof(s_writes[zone_index].name) - 1);
    g_total_write_calls++;
    return true;
}
bool zones_config_set_relay_mask(uint8_t zone_index, uint8_t relay_mask)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_relay_mask_called = true;
    s_writes[zone_index].relay_mask = relay_mask;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_thermo_mask(uint8_t zone_index, uint8_t thermo_mask)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_thermo_mask_called = true;
    s_writes[zone_index].thermo_mask = thermo_mask;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_ct_mask(uint8_t zone_index, uint8_t ct_mask)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_ct_mask_called = true;
    s_writes[zone_index].ct_mask = ct_mask;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_cal_offset(uint8_t zone_index, float cal_offset_c)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_cal_called = true;
    s_writes[zone_index].cal_offset_c = cal_offset_c;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_max_ramp(uint8_t zone_index, float c_per_hr)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_ramp_called = true;
    s_writes[zone_index].max_ramp_c_per_hr = c_per_hr;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_sanity_rate(uint8_t zone_index, float c_per_min)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_sanity_called = true;
    s_writes[zone_index].sanity_rate_c_per_min = c_per_min;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_control_mode(uint8_t zone_index, zone_control_mode_t mode)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_mode_called = true;
    s_writes[zone_index].control_mode = (uint8_t)mode;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_temp_limits(uint8_t zone_index, float max_temp_c, float min_temp_c)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_temp_limits_called = true;
    s_writes[zone_index].max_temp_c = max_temp_c;
    s_writes[zone_index].min_temp_c = min_temp_c;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_heater_cfg(uint8_t zone_index, float window_ms, float min_on_ms, float min_off_ms)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_heater_called = true;
    s_writes[zone_index].heater_window_ms = window_ms;
    s_writes[zone_index].heater_min_on_ms = min_on_ms;
    s_writes[zone_index].heater_min_off_ms = min_off_ms;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_guard_thresholds(uint8_t zone_index, float f1, float f2, float f3, float f4, float f5,
                                       float f6, float f7, float f8)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_guard_called = true;
    float *g = s_writes[zone_index].guard;
    g[0] = f1; g[1] = f2; g[2] = f3; g[3] = f4; g[4] = f5; g[5] = f6; g[6] = f7; g[7] = f8;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_cross_zone_delta(uint8_t zone_index, float max_delta_c)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_xzone_called = true;
    s_writes[zone_index].cross_zone_max_delta_c = max_delta_c;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_safety_tc_type(uint8_t tc_type)
{
    s_safety_tc_set = true;
    s_safety_tc_type = tc_type;
    g_total_write_calls++;
    return true;
}

// ---- profiles_http.h stubs -------------------------------------------------

bool profiles_http_get(uint8_t id, profile_t *out)
{
    (void)id;
    (void)out;
    return false; // export path only, never exercised by these tests
}

bool profiles_http_save(uint8_t requested_id, const profile_t *candidate, uint8_t *out_id,
                        uint8_t *out_warning_count, char *err_msg, size_t err_cap)
{
    (void)err_msg;
    (void)err_cap;
    g_profile_save_calls++;
    g_total_write_calls++;
    uint8_t id = (requested_id < PROFILES_MAX_COUNT) ? requested_id : 0;
    g_last_saved_profile_id = id;
    g_last_saved_profile = *candidate;
    if (out_id) *out_id = id;
    if (out_warning_count) *out_warning_count = 0;
    return true;
}

bool profiles_http_delete(uint8_t id)
{
    (void)id;
    return false;
}

void profiles_http_get_bounds(float *out_target_c_min, float *out_target_c_max, float *out_ramp_c_per_hr_min,
                              float *out_ramp_c_per_hr_max, uint32_t *out_dwell_min_max)
{
    if (out_target_c_min) *out_target_c_min = 0.0f;
    if (out_target_c_max) *out_target_c_max = 1400.0f;
    if (out_ramp_c_per_hr_min) *out_ramp_c_per_hr_min = 0.0f;
    if (out_ramp_c_per_hr_max) *out_ramp_c_per_hr_max = 1000.0f;
    if (out_dwell_min_max) *out_dwell_min_max = 600;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_malformed_body_writes_nothing(void)
{
    TEST_SECTION("backup_import_apply -- malformed/truncated body writes nothing");
    reset_stub_state();

    char err[160];
    bool ok = backup_import_apply("{", err, sizeof(err));

    TEST_CHECK(!ok, "a truncated/malformed body must be refused, not crash or silently accept");
    TEST_CHECK(g_total_write_calls == 0, "no setter may run when the body cannot even be parsed for \"kind\"");
    TEST_CHECK(g_profile_save_calls == 0, "no profile may be saved either");
}

static void test_wrong_kind_refused(void)
{
    TEST_SECTION("backup_import_apply -- wrong \"kind\" is refused");
    reset_stub_state();

    const char *body = "{\"kind\":\"something_else\",\"version\":2,\"profiles\":[],\"zones\":[]}";
    char err[160];
    bool ok = backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(!ok, "a body whose \"kind\" isn't kilnctl_backup must be refused");
    TEST_CHECK(strstr(err, "kilnCtl backup") != NULL, "error names the actual problem (wrong kind)");
    TEST_CHECK(g_total_write_calls == 0, "nothing written for a wrong-kind body");
}

static void test_unknown_version_refused(void)
{
    TEST_SECTION("backup_import_apply -- unknown (too new) version is refused");
    reset_stub_state();

    const char *body = "{\"kind\":\"kilnctl_backup\",\"version\":3,\"profiles\":[],\"zones\":[]}";
    char err[160];
    bool ok = backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(!ok, "version 3 is newer than this firmware's BACKUP_FORMAT_VERSION (2) -- must be refused");
    TEST_CHECK(g_total_write_calls == 0, "nothing written for an unsupported version");
}

static void test_version1_body_imports_under_v2_reader(void)
{
    TEST_SECTION("backup_import_apply -- a version 1 body still imports under the v2 reader");
    reset_stub_state();

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":1,"
        "\"profiles\":[{\"id\":0,\"name\":\"P1\",\"zone_mask\":1,"
        "\"segments\":[{\"target_c\":100,\"ramp_c_per_hr\":50,\"dwell_min\":30}]}],"
        "\"zones\":[{\"index\":0,\"pid_kp\":1.5,\"pid_ki\":0.2,\"pid_kd\":0.05}]}";
    char err[160];
    bool ok = backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(ok, "a version-1-shaped body (no v2-only keys) must import cleanly");
    TEST_CHECK(s_writes[0].set_pid_called, "zone 0's PID gains were committed");
    TEST_CHECK_NEAR(s_writes[0].kp, 1.5, 1e-6, "kp round-trips");
    TEST_CHECK_NEAR(s_writes[0].ki, 0.2, 1e-6, "ki round-trips");
    TEST_CHECK_NEAR(s_writes[0].kd, 0.05, 1e-6, "kd round-trips");
    TEST_CHECK(!s_writes[0].set_model_called, "no model keys were present -- setter must not run");
    TEST_CHECK(!s_writes[0].set_name_called, "no v2-only \"name\" key was present -- setter must not run");
    TEST_CHECK(g_profile_save_calls == 1, "the one profile entry was committed");
}

static void test_out_of_range_model_rejected(void)
{
    TEST_SECTION("backup_import_apply -- plant model over ZONE_MODEL_K_MAX is rejected, nothing written");
    reset_stub_state();

    char body[512];
    snprintf(body, sizeof(body),
             "{\"kind\":\"kilnctl_backup\",\"version\":2,\"profiles\":[],"
             "\"zones\":[{\"index\":0,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,"
             "\"model_k_dc\":%.1f,\"model_tau_s\":100,\"model_dead_time_s\":30}]}",
             (double)(ZONE_MODEL_K_MAX + 1000.0f));
    char err[160];
    bool ok = backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(!ok, "model_k_dc over ZONE_MODEL_K_MAX must be rejected in validation");
    TEST_CHECK(g_total_write_calls == 0,
              "pass 1 validates the WHOLE entry before pass 2 commits ANY of it -- even pid_kp/ki/kd, "
              "which were themselves in range, must not have been written");
}

static void test_overlong_zone_name_rejected(void)
{
    TEST_SECTION("backup_import_apply -- an overlong zone name is rejected, nothing written");
    reset_stub_state();

    // ZONE_NAME_MAX_LEN is 15; this name is well past it.
    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":2,\"profiles\":[],"
        "\"zones\":[{\"index\":0,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,"
        "\"name\":\"ThisNameIsWayTooLongForOneZone\"}]}";
    char err[160];
    bool ok = backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(!ok, "a zone name over ZONE_NAME_MAX_LEN must be rejected in validation");
    TEST_CHECK(g_total_write_calls == 0, "nothing written -- not even pid_kp/ki/kd from the same entry");
}

// 2026-08-21, item 2 of the owner-report pass: backup_http.c's export used to
// emit a junk "_":0 trailing-comma-guard key as the last key of every zone
// object (removed now that the comma is emitted before each field after the
// first instead). Two directions must both still import cleanly under THIS
// build: a v2 body that never had the sentinel (what this build now
// exports), and a v2 body that DOES still carry it (what firmware already
// live on the bench exported before this fix, and may still be handed to
// this build as a restore file).
static void test_v2_body_without_sentinel_imports(void)
{
    TEST_SECTION("backup_import_apply -- v2 body with no \"_\" sentinel key imports cleanly (new export shape)");
    reset_stub_state();

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":2,\"profiles\":[],"
        "\"zones\":[{\"index\":0,\"pid_kp\":1.5,\"pid_ki\":0.2,\"pid_kd\":0.05,"
        "\"name\":\"Top\",\"relay_mask\":1,\"thermo_mask\":1,"
        "\"cal_offset_c\":0,\"max_ramp_c_per_hr\":100,\"sanity_rate_c_per_min\":0,\"control_mode\":0,"
        "\"max_temp_c\":1300,\"min_temp_c\":-20,\"heater_window_ms\":1000,"
        "\"heater_min_on_ms\":0,\"heater_min_off_ms\":0,"
        "\"guard_wrong_dir_window_s\":0,\"guard_wrong_dir_rate_c_per_min\":0,\"guard_off_settle_s\":0,"
        "\"guard_runaway_rate_c_per_min\":0,\"guard_runaway_margin_c\":0,\"guard_drift_period_s\":0,"
        "\"guard_sensor_fault_debounce_ticks\":0,\"guard_frozen_window_s\":0,"
        "\"cross_zone_max_delta_c\":0}]}";
    char err[160];
    bool ok = backup_import_apply(body, err, sizeof(err));

    (void)err;
    TEST_CHECK(ok, "a well-formed v2 zone entry with no sentinel key must import");
    TEST_CHECK(s_writes[0].set_pid_called, "pid gains committed");
    TEST_CHECK(s_writes[0].set_xzone_called, "the real last key, cross_zone_max_delta_c, was reached and committed");
}

static void test_v2_body_with_stale_sentinel_still_imports(void)
{
    TEST_SECTION("backup_import_apply -- v2 body WITH the old \"_\":0 sentinel still imports (bench firmware's export)");
    reset_stub_state();

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":2,\"profiles\":[],"
        "\"zones\":[{\"index\":0,\"pid_kp\":1.5,\"pid_ki\":0.2,\"pid_kd\":0.05,"
        "\"name\":\"Top\",\"relay_mask\":1,\"thermo_mask\":1,"
        "\"cal_offset_c\":0,\"max_ramp_c_per_hr\":100,\"sanity_rate_c_per_min\":0,\"control_mode\":0,"
        "\"max_temp_c\":1300,\"min_temp_c\":-20,\"heater_window_ms\":1000,"
        "\"heater_min_on_ms\":0,\"heater_min_off_ms\":0,"
        "\"guard_wrong_dir_window_s\":0,\"guard_wrong_dir_rate_c_per_min\":0,\"guard_off_settle_s\":0,"
        "\"guard_runaway_rate_c_per_min\":0,\"guard_runaway_margin_c\":0,\"guard_drift_period_s\":0,"
        "\"guard_sensor_fault_debounce_ticks\":0,\"guard_frozen_window_s\":0,"
        "\"cross_zone_max_delta_c\":0,\"_\":0}]}";
    char err[160];
    bool ok = backup_import_apply(body, err, sizeof(err));

    (void)err;
    TEST_CHECK(ok, "an old-shaped v2 zone entry carrying the now-removed \"_\":0 sentinel must still import "
                  "under this build");
    TEST_CHECK(s_writes[0].set_pid_called, "pid gains committed even with the stale sentinel key present");
    TEST_CHECK(s_writes[0].set_xzone_called, "cross_zone_max_delta_c still committed despite the trailing junk key");
}

// FIX 3: json_field_str() silently truncates to cap-1 bytes with no way to
// tell the caller it did so -- before this fix, the profile-name buffer was
// sized exactly PROFILE_NAME_MAX_LEN+1, so an overlong name came back
// pre-truncated to a fit and there was no length check at all to catch it
// (the interactive POST /api/profile path rejects the identical input via
// http_form_find_field()'s -2 return -- import silently accepted what that
// path refuses). Modeled directly on test_overlong_zone_name_rejected()
// above, which solves the identical problem for zone names.
static void test_overlong_profile_name_rejected(void)
{
    TEST_SECTION("backup_import_apply -- an overlong profile name is rejected, nothing written (FIX 3)");
    reset_stub_state();

    // PROFILE_NAME_MAX_LEN is 15; this name is well past it.
    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":2,"
        "\"profiles\":[{\"id\":0,\"name\":\"ThisNameIsWayTooLongForOneProfile\",\"zone_mask\":1,"
        "\"segments\":[{\"target_c\":100,\"ramp_c_per_hr\":50,\"dwell_min\":30}]}],"
        "\"zones\":[]}";
    char err[160];
    bool ok = backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(!ok, "a profile name over PROFILE_NAME_MAX_LEN must be rejected in validation");
    TEST_CHECK(strstr(err, "name too long") != NULL, "error message should say what's wrong");
    TEST_CHECK(g_profile_save_calls == 0, "nothing written -- not even a profile whose other fields were in range");
}

// Positive control, sibling to the rejection test above: a name whose length
// is EXACTLY PROFILE_NAME_MAX_LEN (the boundary, not one over it) must still
// import -- proves the fix's ">" check, not ">=", and that it isn't
// over-rejecting valid input at the limit.
static void test_profile_name_at_limit_accepted(void)
{
    TEST_SECTION("backup_import_apply -- a profile name exactly at PROFILE_NAME_MAX_LEN is accepted (FIX 3)");
    reset_stub_state();

    // PROFILE_NAME_MAX_LEN is 15 -- this name is exactly 15 characters.
    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":2,"
        "\"profiles\":[{\"id\":0,\"name\":\"ExactlyFifteenC\",\"zone_mask\":1,"
        "\"segments\":[{\"target_c\":100,\"ramp_c_per_hr\":50,\"dwell_min\":30}]}],"
        "\"zones\":[]}";
    char err[160];
    bool ok = backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(strlen("ExactlyFifteenC") == PROFILE_NAME_MAX_LEN, "test setup sanity: name is exactly at the limit");
    TEST_CHECK(ok, "a name exactly at the limit must be accepted, not rejected as \"too long\"");
    TEST_CHECK(g_profile_save_calls == 1, "the profile was committed");
    TEST_CHECK(strcmp(g_last_saved_profile.name, "ExactlyFifteenC") == 0, "the full, untruncated name was written");
}

void run_test_backup_import(void)
{
    test_malformed_body_writes_nothing();
    test_wrong_kind_refused();
    test_unknown_version_refused();
    test_version1_body_imports_under_v2_reader();
    test_out_of_range_model_rejected();
    test_overlong_zone_name_rejected();
    test_v2_body_without_sentinel_imports();
    test_v2_body_with_stale_sentinel_still_imports();
    test_overlong_profile_name_rejected();
    test_profile_name_at_limit_accepted();
}
