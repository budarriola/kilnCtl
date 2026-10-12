// Host tests for App/drivers/http/diagnostics_http.c (docs/audits/
// HOST_TEST_COVERAGE_GAPS_2026-10-09.md campaign 9). The handlers are
// `static`, so this file #includes diagnostics_http.c directly (same
// convention as test_kiln_cfg_http.c) and fakes the whole dependency surface.
//
// Note: estop_verify's refusals (safety_trip not ok, missing confirm) live in
// the MCP tool, not this handler; the handler just calls
// estop_verification_confirm(). The v3-crash-record-after-upgrade scenario
// (DEV_FIRMWARE_REVIEW_14 MED-1) lives in crash_report.c's loader, not here.
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

#define asm(x)

// ---------------------------------------------------------------------------
// httpd fakes
// ---------------------------------------------------------------------------
static int s_status;
static char s_body[20000];
static size_t s_body_len;
static int s_chunk_calls;
static int s_chunk_fail_at;
static char s_post_body[9000];
static size_t s_post_len;
static size_t s_post_off;
static bool s_recv_fail;
static char s_query[256];
static bool s_query_present;

static void resp_reset(void)
{
    s_status = 200;
    s_body[0] = '\0';
    s_body_len = 0;
    s_chunk_calls = 0;
    s_chunk_fail_at = 0;
    s_post_off = 0;
    s_recv_fail = false;
}

static void body_append(const char *b, size_t n)
{
    if (s_body_len + n >= sizeof(s_body)) n = sizeof(s_body) - 1 - s_body_len;
    memcpy(s_body + s_body_len, b, n);
    s_body_len += n;
    s_body[s_body_len] = '\0';
}

int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len)
{
    (void)r;
    if (s_recv_fail) return -1;
    size_t left = s_post_len - s_post_off;
    size_t n = left < buf_len ? left : buf_len;
    memcpy(buf, s_post_body + s_post_off, n);
    s_post_off += n;
    return (int)n;
}
esp_err_t httpd_resp_send_err(httpd_req_t *req, httpd_err_code_t error, const char *msg)
{
    (void)req;
    s_status = (int)error;
    s_body_len = 0;
    s_body[0] = '\0';
    body_append(msg ? msg : "", msg ? strlen(msg) : 0);
    return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status)
{
    (void)req;
    s_status = atoi(status);
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type) { (void)req; (void)type; return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *f, const char *v) { (void)req; (void)f; (void)v; return ESP_OK; }
esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *s)
{
    (void)req;
    body_append(s ? s : "", s ? strlen(s) : 0);
    return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t *req, const char *buf, ssize_t len)
{
    (void)req;
    if (buf) body_append(buf, len < 0 ? strlen(buf) : (size_t)len);
    return ESP_OK;
}
esp_err_t httpd_resp_send_chunk(httpd_req_t *req, const char *buf, ssize_t len)
{
    (void)req;
    s_chunk_calls++;
    if (s_chunk_fail_at && s_chunk_calls == s_chunk_fail_at) return ESP_FAIL;
    if (buf) body_append(buf, len < 0 ? strlen(buf) : (size_t)len);
    return ESP_OK;
}
esp_err_t httpd_req_get_url_query_str(httpd_req_t *req, char *buf, size_t buf_len)
{
    (void)req;
    if (!s_query_present) return ESP_ERR_NOT_FOUND;
    snprintf(buf, buf_len, "%s", s_query);
    return ESP_OK;
}
esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t val_size)
{
    size_t kl = strlen(key);
    const char *p = qry;
    while (*p) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t vl = strcspn(v, "&");
            if (vl >= val_size) {
                memcpy(val, v, val_size - 1);
                val[val_size - 1] = '\0';
                return ESP_ERR_HTTPD_RESULT_TRUNC;
            }
            memcpy(val, v, vl);
            val[vl] = '\0';
            return ESP_OK;
        }
        p += strcspn(p, "&");
        if (*p == '&') p++;
    }
    return ESP_ERR_NOT_FOUND;
}

#define MAX_REG 64
static struct { const char *uri; int method; } s_reg[MAX_REG];
static int s_reg_n;
static int s_reg_fail_at;
static httpd_handle_t s_server_handle = (httpd_handle_t)1;
httpd_handle_t wifi_provision_http_get_server(void) { return s_server_handle; }
esp_err_t kiln_http_register(httpd_handle_t server, const httpd_uri_t *uri)
{
    (void)server;
    if (s_reg_n < MAX_REG) { s_reg[s_reg_n].uri = uri->uri; s_reg[s_reg_n].method = (int)uri->method; }
    s_reg_n++;
    if (s_reg_fail_at && s_reg_n == s_reg_fail_at) return ESP_FAIL;
    return ESP_OK;
}
bool web_client_accepts_gzip(httpd_req_t *req) { (void)req; return true; }
esp_err_t web_send_gzip_not_acceptable(httpd_req_t *req, const char *t, const char *p) { (void)req; (void)t; (void)p; return ESP_OK; }
void web_set_asset_cache_headers(httpd_req_t *req) { (void)req; }
const uint8_t diagnostics_page_html_gz_start[4] = {0};
const uint8_t diagnostics_page_html_gz_end[1] = {0};
const uint8_t safety_page_html_gz_start[4] = {0};
const uint8_t safety_page_html_gz_end[1] = {0};

// ---------------------------------------------------------------------------
// module fakes (real headers first, so a signature drift fails to compile)
// ---------------------------------------------------------------------------
#include "nvs.h"
#include "esp_littlefs.h"
#include "MAX31856.h"
#include "cfg_fs.h"
#include "cfg_fs_mount.h"
#include "cfg_fs_status.h"
#include "cfgfs_file_validate.h"
#include "crash_report.h"
#include "estop_verification.h"
#include "danger_mode.h"
#include "dashboard_http.h"
#include "hal_kv.h"
#include "hal_sysinfo.h"
#include "http_async_job.h"
#include "lvgl_port.h"
#include "dualwrite_window.h"
#include "ramp_assist_cfg.h"
#include "relay_cycles.h"
#include "safety_link.h"
#include "thermo_owner.h"
#include "watchdog_cfg.h"
#include "adaptive_tune.h"
#include "profile_executor.h"
#include "zones_config_cfg_fs.h"

// crash_report
static bool g_cr_present;
static crash_report_record_t g_cr_rec;
static bool g_cr_ack_ok;
static esp_err_t g_cr_clear_err;
static int g_cr_clear_calls;
bool crash_report_get(crash_report_record_t *out) { if (g_cr_present && out) *out = g_cr_rec; return g_cr_present; }
bool crash_report_acknowledge(void) { return g_cr_ack_ok; }
bool crash_report_frame_trustworthy(const crash_report_record_t *r) { (void)r; return true; }
esp_err_t crash_report_clear(void) { g_cr_clear_calls++; return g_cr_clear_err; }

// http_async_job
static bool g_job_busy;
static http_async_job_start_result_t g_job_result = HTTP_ASYNC_JOB_STARTED;
bool http_async_job_busy(void) { return g_job_busy; }
http_async_job_start_result_t http_async_job_try_start(httpd_req_t *req, const char *name, uint32_t stack,
                                                       http_async_job_fn_t fn, void *ctx)
{
    (void)name; (void)stack;
    if (g_job_result == HTTP_ASYNC_JOB_STARTED) fn(req, ctx);
    return g_job_result;
}

// estop
static esp_err_t g_estop_err;
static int g_estop_calls;
esp_err_t estop_verification_confirm(void) { g_estop_calls++; return g_estop_err; }

// coredump
static hal_status_t g_cd_info_st;
static hal_sysinfo_coredump_info_t g_cd_info;
static hal_status_t g_cd_read_st;
static uint32_t g_cd_read_off, g_cd_read_len;
static int g_cd_read_calls;
hal_status_t hal_sysinfo_coredump_get_info(hal_sysinfo_coredump_info_t *out) { if (out) *out = g_cd_info; return g_cd_info_st; }
hal_status_t hal_sysinfo_coredump_read(uint32_t off, void *buf, uint32_t len)
{
    g_cd_read_calls++;
    g_cd_read_off = off;
    g_cd_read_len = len;
    if (g_cd_read_st == HAL_OK) memset(buf, 0xAB, len);
    return g_cd_read_st;
}

// watchdog
static bool g_wd_disabled;
static esp_err_t g_wd_set_err;
static int g_wd_set_calls;
bool watchdog_cfg_panic_disabled(void) { return g_wd_disabled; }
esp_err_t watchdog_cfg_set_panic_disabled(bool d, const char *why)
{
    (void)why;
    g_wd_set_calls++;
    if (g_wd_set_err == ESP_OK) g_wd_disabled = d;
    return g_wd_set_err;
}

// relay cycles
static bool g_rc_reset_ok;
static unsigned g_rc_reset_arg = 99;
static int g_rc_reset_calls;
static bool g_rc_restore_ok;
static uint32_t g_rc_restore_counts[RELAY_CYCLES_COUNT];
static uint8_t g_rc_restore_mask;
static int g_rc_restore_calls;
static relay_cycles_restore_result_t g_rc_restore_result;
void relay_cycles_budget(uint8_t r, relay_cycles_budget_t *out) { (void)r; memset(out, 0, sizeof(*out)); }
bool relay_cycles_reset(unsigned r) { g_rc_reset_calls++; g_rc_reset_arg = r; return g_rc_reset_ok; }
bool relay_cycles_restore_all(const uint32_t counts[RELAY_CYCLES_COUNT], uint8_t mask, relay_cycles_restore_result_t *res)
{
    g_rc_restore_calls++;
    memcpy(g_rc_restore_counts, counts, sizeof(g_rc_restore_counts));
    g_rc_restore_mask = mask;
    if (res) *res = g_rc_restore_result;
    return g_rc_restore_ok;
}
bool relay_cycles_migration_worker_wait_deferred(void) { return false; }
bool adaptive_tune_kibase_migration_worker_wait_deferred(void) { return false; }
static esp_err_t g_worker_err;
static int g_worker_calls;
bool uart_bridge_ext_is_on_flash_worker(void) { return false; }
esp_err_t uart_bridge_ext_run_on_flash_worker(void (*fn)(void *), void *arg)
{
    g_worker_calls++;
    if (g_worker_err != ESP_OK) return g_worker_err;
    fn(arg);
    return ESP_OK;
}

// ramp assist
static bool g_ra_enabled;
static esp_err_t g_ra_set_err;
bool ramp_assist_cfg_enabled(void) { return g_ra_enabled; }
esp_err_t ramp_assist_cfg_set_enabled(bool e) { if (g_ra_set_err == ESP_OK) g_ra_enabled = e; return g_ra_set_err; }

// cfg_fs
static bool g_cfg_avail = true;
static bool g_cfg_recovery;
static esp_err_t g_cfg_read_err;
static size_t g_cfg_read_len = 5;
static char g_cfg_read_name[64];
static esp_err_t g_cfg_write_err;
static char g_cfg_write_name[64];
static size_t g_cfg_write_len;
static int g_cfg_write_calls;
bool cfg_fs_is_available(void) { return g_cfg_avail; }
bool cfg_fs_skipped_for_recovery(void) { return g_cfg_recovery; }
esp_err_t cfg_fs_read(const char *rel, void *buf, size_t cap, size_t *out_len)
{
    snprintf(g_cfg_read_name, sizeof(g_cfg_read_name), "%s", rel);
    if (g_cfg_read_err != ESP_OK) return g_cfg_read_err;
    size_t n = g_cfg_read_len < cap ? g_cfg_read_len : cap;
    memset(buf, 0x5A, n);
    *out_len = n;
    return ESP_OK;
}
esp_err_t cfg_fs_write_atomic(const char *rel, const void *data, size_t len)
{
    (void)data;
    g_cfg_write_calls++;
    snprintf(g_cfg_write_name, sizeof(g_cfg_write_name), "%s", rel);
    g_cfg_write_len = len;
    return g_cfg_write_err;
}
static cfgfs_file_check_t g_chk_result = CFGFS_FILE_CHECK_OK;
static bool g_chk_raw;
static int g_chk_calls;
cfgfs_file_check_t cfgfs_file_check_write(const char *name, bool raw, const void *body, size_t len, const char **reason)
{
    (void)name; (void)body; (void)len;
    g_chk_calls++;
    g_chk_raw = raw;
    *reason = "invalid body";
    return g_chk_result;
}
bool cfg_fs_mount_format_ever_started(void) { return false; }
bool cfg_fs_mount_format_in_progress(void) { return false; }
bool cfg_fs_mount_format_completed(void) { return false; }
esp_err_t cfg_fs_mount_format_result(void) { return ESP_OK; }
uint32_t cfg_fs_mount_format_elapsed_ms(void) { return 0; }
bool cfg_fs_status_item_nvs_stale(bool d, uint32_t f, uint32_t n) { (void)d; (void)f; (void)n; return false; }
static esp_err_t g_status_build_err;
esp_err_t cfg_fs_status_build_json_ex(const char *base, const cfg_fs_capacity_info_t *cap, const cfg_fs_dualwrite_item_t *items,
                                      size_t n, const cfg_fs_format_progress_t *fmt, const dualwrite_window_status_t *win,
                                      char *buf, size_t cap_len, size_t *out_len)
{
    (void)base; (void)cap; (void)items; (void)fmt; (void)win;
    if (g_status_build_err != ESP_OK) return g_status_build_err;
    int w = snprintf(buf, cap_len, "{\"items\":%u}", (unsigned)n);
    *out_len = (size_t)w;
    return ESP_OK;
}
bool dualwrite_window_get_status(dualwrite_window_status_t *out) { if (out) memset(out, 0, sizeof(*out)); return true; }
esp_err_t esp_littlefs_info(const char *p, size_t *total, size_t *used) { (void)p; *total = 1000; *used = 10; return ESP_OK; }
void zones_config_cfg_fs_load_raw(zones_cfg_t *c, uint32_t *rev, bool *valid) { (void)c; *rev = 0; *valid = false; }

#define DW_FAKE(fn) \
    void fn(bool *fv, uint32_t *fr, bool *nv, uint32_t *nr, bool *dv) { *fv = false; *fr = 0; *nv = false; *nr = 0; *dv = false; }
DW_FAKE(adaptive_tune_get_kibase_dualwrite_status)
DW_FAKE(aux_outputs_cfg_get_dualwrite_status)
DW_FAKE(ct_verify_store_get_dualwrite_status)
DW_FAKE(display_power_cfg_get_dualwrite_status)
DW_FAKE(iter_tune_store_get_dualwrite_status)
DW_FAKE(kiln_cfg_store_get_dualwrite_status)
DW_FAKE(live_profile_get_dualwrite_status)
DW_FAKE(profiles_builtin_get_dualwrite_status)
DW_FAKE(profiles_favorites_get_dualwrite_status)
DW_FAKE(ramp_assist_cfg_get_dualwrite_status)
DW_FAKE(relay_cycles_get_dualwrite_status)
DW_FAKE(relay_names_get_dualwrite_status)
DW_FAKE(setup_wizard_progress_get_dualwrite_status)
DW_FAKE(time_sync_get_tz_dualwrite_status)
DW_FAKE(unit_pref_get_dualwrite_status)
DW_FAKE(update_settings_get_dualwrite_status)
DW_FAKE(zone_normals_get_dualwrite_status)
void firing_stats_get_dualwrite_status_ex(bool *fv, uint32_t *fr, bool *nv, uint32_t *nr, bool *dv, bool *stale)
{
    *fv = false; *fr = 0; *nv = false; *nr = 0; *dv = false; *stale = false;
}
void profiles_http_get_dualwrite_status(uint8_t id, bool *fv, uint32_t *fr, bool *nv, uint32_t *nr, bool *dv)
{
    (void)id; *fv = false; *fr = 0; *nv = false; *nr = 0; *dv = false;
}

// danger mode
static bool g_dm_active;
static bool g_dm_start_ok = true;
static int g_dm_start_calls, g_dm_stop_calls, g_dm_touch_calls;
static bool g_dm_enable_ok = true;
static bool g_dm_enable_arg;
static int g_dm_enable_calls;
static uint32_t g_dm_remaining = 12345;
static dashboard_relay_result_t g_dm_relay_rr = DASHBOARD_RELAY_OK;
static uint8_t g_dm_relay_arg;
static bool g_dm_relay_on;
static int g_dm_relay_calls;
bool danger_mode_active(void) { return g_dm_active; }
bool danger_mode_get_heat_requested(void) { return false; }
bool danger_mode_get_relay_status(bool *e, bool *h) { *e = true; *h = false; return true; }
uint32_t danger_mode_remaining_ms(void) { return g_dm_remaining; }
bool danger_mode_request_start(void) { g_dm_start_calls++; return g_dm_start_ok; }
bool danger_mode_set_heat_enable_request(bool on) { g_dm_enable_calls++; g_dm_enable_arg = on; return g_dm_enable_ok; }
void danger_mode_stop(const char *why) { (void)why; g_dm_stop_calls++; }
bool danger_mode_touch(void) { g_dm_touch_calls++; return true; }
dashboard_relay_result_t dashboard_set_relay(uint8_t r, bool on, uint32_t *src)
{
    g_dm_relay_calls++;
    g_dm_relay_arg = r;
    g_dm_relay_on = on;
    if (src) *src = 0;
    return g_dm_relay_rr;
}

// timing sources
void lvgl_port_get_flush_stats_ex(uint32_t *l, uint32_t *mn, uint32_t *mx, uint32_t *c, uint32_t *me)
{
    *l = 11; *mn = 5; *mx = 99; *c = 3; *me = 22;
}
void MAX31856_get_read_all_stats(uint32_t *l, uint32_t *mn, uint32_t *mx, uint32_t *c, uint32_t *me)
{
    *l = 1; *mn = 2; *mx = 3; *c = 4; *me = 5;
}
static esp_err_t g_sl_stats_err = ESP_FAIL;
esp_err_t safety_link_get_stats(SafetyLinkClass *l, safety_link_stats_t *out)
{
    (void)l;
    if (g_sl_stats_err != ESP_OK) return g_sl_stats_err;
    memset(out, 0, sizeof(*out));
    out->link_reply_us_last = 70;
    out->link_reply_us_min = 60;
    out->link_reply_us_max = 80;
    out->link_reply_us_count = 9;
    out->link_reply_us_mean = 71;
    out->timeouts = 4;
    return ESP_OK;
}
esp_err_t safety_link_get_status(SafetyLinkClass *l, safety_link_status_t *out) { (void)l; memset(out, 0, sizeof(*out)); return ESP_FAIL; }
esp_err_t thermo_owner_command_read(uint8_t ch, MAX31856Reading *out) { (void)ch; memset(out, 0, sizeof(*out)); return ESP_FAIL; }

// nvs iteration (scriptable)
static struct { const char *key; nvs_type_t type; } s_nvs_keys[8];
static int s_nvs_n;
static int s_nvs_pos;
static esp_err_t s_nvs_find_err;
static int s_nvs_find_calls;
static char s_nvs_find_part[32], s_nvs_find_ns[32];
static int s_nvs_released;
esp_err_t nvs_entry_find(const char *part, const char *ns, nvs_type_t type, nvs_iterator_t *it)
{
    (void)type;
    s_nvs_find_calls++;
    snprintf(s_nvs_find_part, sizeof(s_nvs_find_part), "%s", part);
    snprintf(s_nvs_find_ns, sizeof(s_nvs_find_ns), "%s", ns);
    s_nvs_pos = 0;
    if (s_nvs_find_err != ESP_OK) { *it = NULL; return s_nvs_find_err; }
    if (s_nvs_n == 0) { *it = NULL; return ESP_ERR_NVS_NOT_FOUND; }
    *it = (nvs_iterator_t)1;
    return ESP_OK;
}
esp_err_t nvs_entry_next(nvs_iterator_t *it)
{
    s_nvs_pos++;
    if (s_nvs_pos >= s_nvs_n) { *it = NULL; return ESP_ERR_NVS_NOT_FOUND; }
    return ESP_OK;
}
esp_err_t nvs_entry_info(const nvs_iterator_t it, nvs_entry_info_t *info)
{
    (void)it;
    memset(info, 0, sizeof(*info));
    snprintf(info->key, sizeof(info->key), "%s", s_nvs_keys[s_nvs_pos].key);
    info->type = s_nvs_keys[s_nvs_pos].type;
    return ESP_OK;
}
void nvs_release_iterator(nvs_iterator_t it) { (void)it; s_nvs_released++; }

#include "../drivers/http/diagnostics_http.c"

#undef asm

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static httpd_req_t s_req;

static void world_reset(void)
{
    resp_reset();
    s_post_len = 0;
    s_post_body[0] = '\0';
    s_query_present = false;
    s_query[0] = '\0';
    memset(&g_cr_rec, 0, sizeof(g_cr_rec));
    g_cr_present = false; g_cr_ack_ok = false; g_cr_clear_err = ESP_OK; g_cr_clear_calls = 0;
    g_job_busy = false; g_job_result = HTTP_ASYNC_JOB_STARTED;
    s_crash_clear_in_progress = false;
    g_estop_err = ESP_OK; g_estop_calls = 0;
    g_cd_info_st = HAL_OK; memset(&g_cd_info, 0, sizeof(g_cd_info));
    g_cd_read_st = HAL_OK; g_cd_read_calls = 0; g_cd_read_off = 0; g_cd_read_len = 0;
    g_wd_disabled = false; g_wd_set_err = ESP_OK; g_wd_set_calls = 0;
    g_rc_reset_ok = true; g_rc_reset_arg = 99; g_rc_reset_calls = 0;
    g_rc_restore_ok = true; g_rc_restore_calls = 0; g_rc_restore_mask = 0xFF;
    memset(&g_rc_restore_result, 0, sizeof(g_rc_restore_result));
    memset(g_rc_restore_counts, 0, sizeof(g_rc_restore_counts));
    g_worker_err = ESP_OK; g_worker_calls = 0;
    g_ra_enabled = false; g_ra_set_err = ESP_OK;
    g_cfg_avail = true; g_cfg_recovery = false; g_cfg_read_err = ESP_OK; g_cfg_read_len = 5;
    g_cfg_read_name[0] = '\0'; g_cfg_write_err = ESP_OK; g_cfg_write_name[0] = '\0'; g_cfg_write_len = 0; g_cfg_write_calls = 0;
    g_chk_result = CFGFS_FILE_CHECK_OK; g_chk_raw = false; g_chk_calls = 0;
    g_status_build_err = ESP_OK;
    g_dm_active = false; g_dm_start_ok = true; g_dm_start_calls = 0; g_dm_stop_calls = 0; g_dm_touch_calls = 0;
    g_dm_enable_ok = true; g_dm_enable_calls = 0; g_dm_enable_arg = false;
    g_dm_relay_rr = DASHBOARD_RELAY_OK; g_dm_relay_calls = 0; g_dm_relay_arg = 0; g_dm_relay_on = false;
    g_sl_stats_err = ESP_FAIL;
    s_nvs_n = 0; s_nvs_pos = 0; s_nvs_find_err = ESP_OK;
    s_nvs_find_calls = 0; s_nvs_find_part[0] = '\0'; s_nvs_find_ns[0] = '\0'; s_nvs_released = 0;
    s_reg_n = 0; s_reg_fail_at = 0; s_server_handle = (httpd_handle_t)1;
    memset(&s_req, 0, sizeof(s_req));
}

static void set_body(const char *b)
{
    snprintf(s_post_body, sizeof(s_post_body), "%s", b);
    s_post_len = strlen(s_post_body);
    s_req.content_len = (long long)s_post_len;
}
static void set_query(const char *q)
{
    snprintf(s_query, sizeof(s_query), "%s", q);
    s_query_present = true;
}
static bool body_has(const char *needle) { return strstr(s_body, needle) != NULL; }

// ---------------------------------------------------------------------------
// crash_report / estop
// ---------------------------------------------------------------------------
static void test_crash_report(void)
{
    world_reset();
    crash_report_get_handler(&s_req);
    TEST_CHECK(s_status == 200 && body_has("\"present\":false"), "crash_report GET absent: present:false");

    world_reset();
    g_cr_present = true;
    g_cr_rec.acknowledged = 1;
    g_cr_rec.exc_pc = 0x400d1234u;
    g_cr_rec.bt_count = 2;
    g_cr_rec.backtrace_pc[0] = 0x400d0001u;
    g_cr_rec.backtrace_pc[1] = 0x400d0002u;
    g_cr_rec.dump_id = 777;
    crash_report_get_handler(&s_req);
    TEST_CHECK(s_status == 200 && body_has("\"present\":true") && body_has("\"acknowledged\":true"),
               "crash_report GET present: acknowledged flag reflected");
    TEST_CHECK(body_has("400d1234") && body_has("400d0001") && body_has("400d0002"), "crash_report GET: pc and backtrace emitted");

    // oversized bt_count must never read past backtrace_pc[]
    world_reset();
    g_cr_present = true;
    g_cr_rec.bt_count = 255;
    crash_report_get_handler(&s_req);
    {
        int n = 0;
        for (const char *p = s_body; (p = strstr(p, "0x")) != NULL; p++) n++;
        TEST_CHECK(n <= CRASH_REPORT_BT_MAX + 6, "crash_report GET: oversized bt_count capped at CRASH_REPORT_BT_MAX entries");
    }

    // worst-case record with quote-heavy strings must still encode
    world_reset();
    g_cr_present = true;
    g_cr_rec.bt_count = CRASH_REPORT_BT_MAX;
    for (int i = 0; i < CRASH_REPORT_BT_MAX; i++) g_cr_rec.backtrace_pc[i] = 0xFFFFFFFFu;
    memset(g_cr_rec.exc_task, '"', sizeof(g_cr_rec.exc_task) - 1);
    memset(g_cr_rec.exc_cause_str, '"', sizeof(g_cr_rec.exc_cause_str) - 1);
    memset(g_cr_rec.reset_reason, '"', sizeof(g_cr_rec.reset_reason) - 1);
    memset(g_cr_rec.fw_build, '"', sizeof(g_cr_rec.fw_build) - 1);
    memset(g_cr_rec.dump_elf_sha, '"', sizeof(g_cr_rec.dump_elf_sha) - 1);
    crash_report_get_handler(&s_req);
    TEST_CHECK(s_status == 200 && body_has("\"present\":true"), "crash_report GET: worst-case full record still encodes");
}

static void test_crash_report_ack_clear(void)
{
    world_reset();
    g_cr_present = true; g_cr_ack_ok = true;
    crash_report_ack_post_handler(&s_req);
    TEST_CHECK(s_status == 200 && body_has("\"ok\":true"), "ack: success answers 200 ok:true");

    world_reset();
    crash_report_ack_post_handler(&s_req);
    TEST_CHECK(s_status >= 400 && !body_has("\"ok\":true"), "ack: absent record is a refusal, never ok:true");

    world_reset();
    g_cr_present = true; g_cr_ack_ok = false;
    crash_report_ack_post_handler(&s_req);
    TEST_CHECK(s_status >= 400 && !body_has("\"ok\":true"), "ack: persist failure on a present record is a refusal");

    world_reset();
    g_job_busy = true;
    crash_report_clear_post_handler(&s_req);
    TEST_CHECK(s_status == 503 && g_cr_clear_calls == 0, "clear: busy job answers 503 without erasing");

    world_reset();
    g_job_result = HTTP_ASYNC_JOB_BUSY;
    crash_report_clear_post_handler(&s_req);
    TEST_CHECK(s_status == 503 && g_cr_clear_calls == 0 && !s_crash_clear_in_progress,
               "clear: try_start BUSY answers 503 and releases clear_in_progress");

    world_reset();
    g_job_result = HTTP_ASYNC_JOB_RESOURCE_FAILURE;
    crash_report_clear_post_handler(&s_req);
    TEST_CHECK(s_status == 500 && g_cr_clear_calls == 0 && !s_crash_clear_in_progress,
               "clear: resource failure answers 500 and releases clear_in_progress");

    world_reset();
    crash_report_clear_post_handler(&s_req);
    TEST_CHECK(s_status == 200 && body_has("\"ok\":true") && g_cr_clear_calls == 1 && !s_crash_clear_in_progress,
               "clear: success runs crash_report_clear once and clears the in-progress flag");

    world_reset();
    g_cr_clear_err = ESP_FAIL;
    crash_report_clear_post_handler(&s_req);
    TEST_CHECK(s_status == 500 && !body_has("\"ok\":true"), "clear: coredump erase failure is reported as failure, not success");
    TEST_CHECK(!s_crash_clear_in_progress, "clear: in-progress flag released even when the erase failed");
}

static void test_estop_verify(void)
{
    world_reset();
    estop_verify_post_handler(&s_req);
    TEST_CHECK(s_status == 200 && body_has("\"ok\":true") && g_estop_calls == 1, "estop_verify: success calls confirm once");

    world_reset();
    g_estop_err = ESP_ERR_INVALID_STATE;
    estop_verify_post_handler(&s_req);
    TEST_CHECK(s_status >= 400 && !body_has("\"ok\":true"), "estop_verify: confirm failure is an error, never ok:true");
}

// ---------------------------------------------------------------------------
// coredump
// ---------------------------------------------------------------------------
static void test_coredump(void)
{
    world_reset();
    g_cd_info.present = true; g_cd_info.data_len = 1234; g_cd_info.partition_size = 65536;
    coredump_info_get_handler(&s_req);
    TEST_CHECK(s_status == 200 && body_has("1234") && body_has("65536"), "coredump info: fields reported");

    world_reset();
    g_cd_info_st = HAL_IO;
    coredump_info_get_handler(&s_req);
    TEST_CHECK(s_status >= 400, "coredump info: HAL failure is an error");

    world_reset();
    coredump_chunk_get_handler(&s_req);
    TEST_CHECK(s_status == 400 && g_cd_read_calls == 0, "coredump chunk: no query -> 400");

    const char *bad[] = {"offset=0", "len=10", "offset=abc&len=10", "offset=0&len=abc", "offset=0&len=0", "offset=0&len=-5",
                         "offset=-1&len=5", "offset=1x&len=5", NULL};
    for (int i = 0; bad[i]; i++) {
        world_reset();
        set_query(bad[i]);
        coredump_chunk_get_handler(&s_req);
        TEST_CHECK(s_status == 400 && g_cd_read_calls == 0, "coredump chunk: bad offset/len refused before any read");
    }

    world_reset();
    set_query("offset=16&len=64");
    coredump_chunk_get_handler(&s_req);
    TEST_CHECK(s_status == 200 && s_body_len == 64 && g_cd_read_off == 16 && g_cd_read_len == 64,
               "coredump chunk: in-range request passes offset/len through");

    world_reset();
    set_query("offset=0&len=99999");
    coredump_chunk_get_handler(&s_req);
    TEST_CHECK(g_cd_read_len <= 4096, "coredump chunk: oversized len never reads more than 4096");

    world_reset();
    set_query("offset=0&len=8");
    g_cd_read_st = HAL_IO;
    coredump_chunk_get_handler(&s_req);
    TEST_CHECK(s_status >= 400, "coredump chunk: read failure is an error");
}

// ---------------------------------------------------------------------------
// watchdog / ramp assist
// ---------------------------------------------------------------------------
static void test_watchdog_ramp(void)
{
    world_reset();
    s_req.content_len = 0;
    watchdog_cfg_post_handler(&s_req);
    TEST_CHECK(s_status == 400 && g_wd_set_calls == 0, "watchdog POST: empty body -> 400");

    world_reset();
    set_body("disabled=1");
    s_req.content_len = 33;
    watchdog_cfg_post_handler(&s_req);
    TEST_CHECK(s_status == 400 && g_wd_set_calls == 0, "watchdog POST: oversized body -> 400");

    world_reset();
    set_body("disabled=1");
    s_recv_fail = true;
    watchdog_cfg_post_handler(&s_req);
    TEST_CHECK(s_status == 400 && g_wd_set_calls == 0, "watchdog POST: recv failure -> 400");

    const char *bad[] = {"x=1", "disabled=", "disabled=2", "disabled=true", "disabled=01", "disabled=-1", NULL};
    for (int i = 0; bad[i]; i++) {
        world_reset();
        set_body(bad[i]);
        watchdog_cfg_post_handler(&s_req);
        TEST_CHECK(s_status == 400 && g_wd_set_calls == 0, "watchdog POST: anything but 0/1 refused without writing");
    }

    world_reset();
    set_body("disabled=1");
    watchdog_cfg_post_handler(&s_req);
    TEST_CHECK(s_status == 200 && g_wd_disabled && g_wd_set_calls == 1, "watchdog POST disabled=1 applied");

    world_reset();
    set_body("disabled=1");
    g_wd_set_err = ESP_FAIL;
    watchdog_cfg_post_handler(&s_req);
    TEST_CHECK(s_status == 500 && !g_wd_disabled, "watchdog POST: set failure -> 500");

    world_reset();
    g_cfg_avail = false;
    set_body("enabled=1");
    ramp_assist_post_handler(&s_req);
    TEST_CHECK(s_status >= 400 && !g_ra_enabled, "ramp_assist POST: cfg unmounted refused before touching state");

    const char *rbad[] = {"enabled=", "enabled=2", "enabled=yes", "y=1", NULL};
    for (int i = 0; rbad[i]; i++) {
        world_reset();
        set_body(rbad[i]);
        ramp_assist_post_handler(&s_req);
        TEST_CHECK(s_status == 400 && !g_ra_enabled, "ramp_assist POST: anything but 0/1 refused");
    }

    world_reset();
    set_body("enabled=1");
    ramp_assist_post_handler(&s_req);
    TEST_CHECK(s_status == 200 && g_ra_enabled, "ramp_assist POST enabled=1 applied");

    world_reset();
    set_body("enabled=1");
    g_ra_set_err = ESP_FAIL;
    ramp_assist_post_handler(&s_req);
    TEST_CHECK(s_status == 500, "ramp_assist POST: persist failure -> 500");
}

// ---------------------------------------------------------------------------
// relay cycles
// ---------------------------------------------------------------------------
static void test_relay_cycles(void)
{
    world_reset();
    s_req.content_len = 0;
    relay_cycles_reset_post_handler(&s_req);
    TEST_CHECK(s_status == 400 && g_rc_reset_calls == 0, "relay_cycles reset: empty body -> 400");

    const char *bad[] = {"x=1", "relay=", "relay=abc", "relay=-1", "relay=99", "relay=1x", "relay=1.5", NULL};
    for (int i = 0; bad[i]; i++) {
        world_reset();
        set_body(bad[i]);
        relay_cycles_reset_post_handler(&s_req);
        TEST_CHECK(s_status == 400 && g_rc_reset_calls == 0, "relay_cycles reset: bad index refused without reset");
    }

    char b[32];
    snprintf(b, sizeof(b), "relay=%u", (unsigned)RELAY_CYCLES_COUNT);
    world_reset();
    set_body(b);
    relay_cycles_reset_post_handler(&s_req);
    TEST_CHECK(s_status == 400 && g_rc_reset_calls == 0, "relay_cycles reset: index == RELAY_CYCLES_COUNT refused");

    snprintf(b, sizeof(b), "relay=%u", (unsigned)(RELAY_CYCLES_COUNT - 1));
    world_reset();
    set_body(b);
    relay_cycles_reset_post_handler(&s_req);
    TEST_CHECK(s_status == 200 && g_rc_reset_calls == 1 && g_rc_reset_arg == RELAY_CYCLES_COUNT - 1,
               "relay_cycles reset: top valid index resets exactly that slot");

    world_reset();
    set_body("relay=0");
    relay_cycles_reset_post_handler(&s_req);
    TEST_CHECK(s_status == 200 && g_rc_reset_arg == 0, "relay_cycles reset: index 0 accepted");

    world_reset();
    set_body("relay=2");
    g_rc_reset_ok = false;
    relay_cycles_reset_post_handler(&s_req);
    TEST_CHECK(s_status >= 500 && !body_has("\"ok\":true"), "relay_cycles reset: persist failure is not success");

    world_reset();
    s_req.content_len = 0;
    relay_cycles_restore_post_handler(&s_req);
    TEST_CHECK(s_status == 400 && g_rc_restore_calls == 0, "relay_cycles restore: empty body -> 400");

    world_reset();
    set_body("c0=1&c1=2&c2=3");
    relay_cycles_restore_post_handler(&s_req);
    TEST_CHECK(s_status == 400 && g_rc_restore_calls == 0, "relay_cycles restore: a missing count refuses the whole request");

    world_reset();
    set_body("c0=1&c1=x&c2=3&c3=4&c4=5&c5=6");
    relay_cycles_restore_post_handler(&s_req);
    TEST_CHECK(s_status == 400 && g_rc_restore_calls == 0, "relay_cycles restore: non-numeric count refused");

    world_reset();
    set_body("c0=1&c1=-2&c2=3&c3=4&c4=5&c5=6");
    relay_cycles_restore_post_handler(&s_req);
    TEST_CHECK(s_status == 400 && g_rc_restore_calls == 0, "relay_cycles restore: negative count refused");
}

// ---------------------------------------------------------------------------
// danger mode
// ---------------------------------------------------------------------------
static void test_danger(void)
{
    world_reset();
    s_req.content_len = 0;
    danger_start_post_handler(&s_req);
    TEST_CHECK(s_status == 400 && g_dm_start_calls == 0, "danger start: empty body -> 400");

    const char *bad[] = {"accept=0", "accept=", "x=1", "accept=2", NULL};
    for (int i = 0; bad[i]; i++) {
        world_reset();
        set_body(bad[i]);
        danger_start_post_handler(&s_req);
        TEST_CHECK(s_status == 400 && g_dm_start_calls == 0, "danger start: without accept=1 never starts");
    }

    world_reset();
    set_body("accept=1");
    danger_start_post_handler(&s_req);
    TEST_CHECK(s_status == 200 && g_dm_start_calls == 1, "danger start: accept=1 starts the window");

    world_reset();
    set_body("accept=1");
    g_dm_start_ok = false;
    danger_start_post_handler(&s_req);
    TEST_CHECK(s_status == 409, "danger start: refused while a firing runs -> 409");

    world_reset();
    danger_stop_post_handler(&s_req);
    TEST_CHECK(s_status == 200 && g_dm_stop_calls == 1, "danger stop: stops the window");

    // relay
    world_reset();
    set_body("relay=1&on=1");
    danger_relay_post_handler(&s_req);
    TEST_CHECK(s_status == 409 && g_dm_relay_calls == 0, "danger relay: inactive window -> 409 before any relay write");

    world_reset();
    g_dm_active = true;
    s_req.content_len = 0;
    danger_relay_post_handler(&s_req);
    TEST_CHECK(s_status == 400 && g_dm_relay_calls == 0, "danger relay: empty body -> 400");

    const char *rbad[] = {"on=1", "relay=1", "relay=1&on=2", "relay=99&on=1", "relay=abc&on=1", "relay=-1&on=1", "relay=1&on=", NULL};
    for (int i = 0; rbad[i]; i++) {
        world_reset();
        g_dm_active = true;
        set_body(rbad[i]);
        danger_relay_post_handler(&s_req);
        TEST_CHECK(s_status == 400 && g_dm_relay_calls == 0 && g_dm_touch_calls == 0, "danger relay: bad relay/on refused with no write");
    }

    world_reset();
    g_dm_active = true;
    set_body("relay=2&on=1");
    danger_relay_post_handler(&s_req);
    TEST_CHECK(s_status == 200 && g_dm_relay_calls == 1 && g_dm_relay_arg == 2 && g_dm_relay_on && g_dm_touch_calls == 1,
               "danger relay: valid write goes through dashboard_set_relay and extends the window");

    dashboard_relay_result_t r409[] = {DASHBOARD_RELAY_ERR_RUNNING, DASHBOARD_RELAY_ERR_OWNED};
    for (int i = 0; i < 2; i++) {
        world_reset();
        g_dm_active = true;
        g_dm_relay_rr = r409[i];
        set_body("relay=1&on=1");
        danger_relay_post_handler(&s_req);
        TEST_CHECK(s_status == 409 && g_dm_touch_calls == 0, "danger relay: RUNNING/OWNED -> 409 and no window extension");
    }

    dashboard_relay_result_t r400[] = {DASHBOARD_RELAY_ERR_NO_BOARD, DASHBOARD_RELAY_ERR_SAFETY, DASHBOARD_RELAY_ERR_IO_FAIL,
                                       DASHBOARD_RELAY_ERR_RANGE};
    for (int i = 0; i < 4; i++) {
        world_reset();
        g_dm_active = true;
        g_dm_relay_rr = r400[i];
        set_body("relay=1&on=1");
        danger_relay_post_handler(&s_req);
        TEST_CHECK(s_status >= 400 && g_dm_touch_calls == 0, "danger relay: other refusals are errors with no window extension");
    }

    // enable
    world_reset();
    set_body("on=1");
    danger_enable_post_handler(&s_req);
    TEST_CHECK(s_status == 409 && g_dm_enable_calls == 0, "danger enable: inactive window -> 409");

    world_reset();
    g_dm_active = true;
    s_req.content_len = 0;
    danger_enable_post_handler(&s_req);
    TEST_CHECK(s_status == 400 && g_dm_enable_calls == 0, "danger enable: empty body -> 400");

    const char *ebad[] = {"on=2", "on=", "x=1", "on=yes", NULL};
    for (int i = 0; ebad[i]; i++) {
        world_reset();
        g_dm_active = true;
        set_body(ebad[i]);
        danger_enable_post_handler(&s_req);
        TEST_CHECK(s_status == 400 && g_dm_enable_calls == 0, "danger enable: anything but 0/1 refused");
    }

    world_reset();
    g_dm_active = true;
    set_body("on=1");
    danger_enable_post_handler(&s_req);
    TEST_CHECK(s_status == 200 && g_dm_enable_calls == 1 && g_dm_enable_arg, "danger enable: on=1 requests heat enable");

    world_reset();
    g_dm_active = true;
    g_dm_enable_ok = false;
    set_body("on=0");
    danger_enable_post_handler(&s_req);
    TEST_CHECK(s_status == 409, "danger enable: window closed in the race -> 409");
}

// ---------------------------------------------------------------------------
// cfgfs
// ---------------------------------------------------------------------------
static void test_cfgfs(void)
{
    world_reset();
    cfgfs_status_get_handler(&s_req);
    TEST_CHECK(s_status == 200 && body_has("\"items\":"), "cfgfs status: 200 with the built json");

    world_reset();
    g_status_build_err = ESP_ERR_NO_MEM;
    cfgfs_status_get_handler(&s_req);
    TEST_CHECK(s_status == 500, "cfgfs status: json build failure -> 500");

    world_reset();
    cfgfs_file_get_handler(&s_req);
    TEST_CHECK(s_status == 400, "cfgfs file GET: no query -> 400");

    const char *bad[] = {"x=1", "name=", "name=../etc", "name=a/b", "name=a\\b", "name=..", NULL};
    for (int i = 0; bad[i]; i++) {
        world_reset();
        set_query(bad[i]);
        cfgfs_file_get_handler(&s_req);
        TEST_CHECK(s_status == 400 && g_cfg_read_name[0] == '\0', "cfgfs file GET: invalid or traversing name refused before any read");
    }

    world_reset();
    g_cfg_avail = false;
    set_query("name=zones.json");
    cfgfs_file_get_handler(&s_req);
    TEST_CHECK(s_status == 404 && g_cfg_read_name[0] == '\0', "cfgfs file GET: unmounted -> 404");

    world_reset();
    set_query("name=zones.json");
    g_cfg_read_len = 7;
    cfgfs_file_get_handler(&s_req);
    TEST_CHECK(s_status == 200 && s_body_len == 7 && strcmp(g_cfg_read_name, "zones.json") == 0, "cfgfs file GET: returns the raw bytes");

    world_reset();
    set_query("name=zones.json");
    g_cfg_read_err = ESP_ERR_NOT_FOUND;
    cfgfs_file_get_handler(&s_req);
    TEST_CHECK(s_status == 404, "cfgfs file GET: read failure -> 404");

    // POST
    world_reset();
    set_body("data");
    cfgfs_file_post_handler(&s_req);
    TEST_CHECK(s_status == 400 && g_cfg_write_calls == 0, "cfgfs file POST: no name -> 400");

    const char *pbad[] = {"name=../x", "name=a/b", "name=a\\b", "name=", NULL};
    for (int i = 0; pbad[i]; i++) {
        world_reset();
        set_query(pbad[i]);
        set_body("data");
        cfgfs_file_post_handler(&s_req);
        TEST_CHECK(s_status == 400 && g_cfg_write_calls == 0 && g_chk_calls == 0, "cfgfs file POST: traversing name refused before validation");
    }

    world_reset();
    set_query("name=zones.json");
    s_req.content_len = 0;
    cfgfs_file_post_handler(&s_req);
    TEST_CHECK(s_status == 400 && g_cfg_write_calls == 0, "cfgfs file POST: empty body -> 400");

    world_reset();
    set_query("name=zones.json");
    s_req.content_len = 8193;
    cfgfs_file_post_handler(&s_req);
    TEST_CHECK(s_status == 400 && g_cfg_write_calls == 0, "cfgfs file POST: body over 8192 -> 400");

    world_reset();
    set_query("name=zones.json");
    set_body("data");
    g_cfg_avail = false;
    cfgfs_file_post_handler(&s_req);
    TEST_CHECK(s_status == 409 && g_cfg_write_calls == 0, "cfgfs file POST: unmounted -> 409");

    world_reset();
    set_query("name=zones.json");
    set_body("data");
    s_recv_fail = true;
    cfgfs_file_post_handler(&s_req);
    TEST_CHECK(s_status == 400 && g_cfg_write_calls == 0, "cfgfs file POST: recv failure -> 400");

    world_reset();
    set_query("name=zones.json");
    set_body("data");
    g_chk_result = CFGFS_FILE_CHECK_INVALID;
    cfgfs_file_post_handler(&s_req);
    TEST_CHECK(s_status == 400 && g_cfg_write_calls == 0, "cfgfs file POST: validator INVALID -> 400, nothing written");

    world_reset();
    set_query("name=mystery.bin");
    set_body("data");
    g_chk_result = CFGFS_FILE_CHECK_NO_VALIDATOR;
    cfgfs_file_post_handler(&s_req);
    TEST_CHECK(s_status == 400 && g_cfg_write_calls == 0, "cfgfs file POST: no validator and no raw=1 -> 400, nothing written");

    world_reset();
    set_query("name=zones.json");
    set_body("data");
    g_chk_result = CFGFS_FILE_CHECK_OOM;
    cfgfs_file_post_handler(&s_req);
    TEST_CHECK(s_status == 500 && g_cfg_write_calls == 0, "cfgfs file POST: validator OOM -> 500, nothing written");

    world_reset();
    set_query("name=zones.json&raw=1");
    set_body("data");
    cfgfs_file_post_handler(&s_req);
    TEST_CHECK(s_status == 200 && g_chk_raw && g_cfg_write_calls == 1 && g_cfg_write_len == 4 &&
               strcmp(g_cfg_write_name, "zones.json") == 0, "cfgfs file POST: raw=1 reaches the validator and the write lands");
    TEST_CHECK(g_worker_calls == 1, "cfgfs file POST: write dispatched through the flash worker");

    world_reset();
    set_query("name=zones.json&raw=10");
    set_body("data");
    cfgfs_file_post_handler(&s_req);
    TEST_CHECK(!g_chk_raw, "cfgfs file POST: raw=10 is not raw=1");

    world_reset();
    set_query("name=zones.json");
    set_body("data");
    g_cfg_write_err = ESP_FAIL;
    cfgfs_file_post_handler(&s_req);
    TEST_CHECK(s_status == 500 && !body_has("\"ok\":true"), "cfgfs file POST: write failure -> 500");

    world_reset();
    set_query("name=zones.json");
    set_body("data");
    g_worker_err = ESP_ERR_TIMEOUT;
    cfgfs_file_post_handler(&s_req);
    TEST_CHECK(s_status == 500 && g_cfg_write_calls == 0, "cfgfs file POST: flash worker unreachable -> 500, nothing written");
}

// ---------------------------------------------------------------------------
// nvs keys
// ---------------------------------------------------------------------------
static void test_nvs_keys(void)
{
    world_reset();
    nvs_keys_get_handler(&s_req);
    TEST_CHECK(s_status == 400 && s_nvs_find_calls == 0, "nvs keys: no query -> 400");

    world_reset();
    set_query("partition=nvs");
    nvs_keys_get_handler(&s_req);
    TEST_CHECK(s_status == 400 && s_nvs_find_calls == 0, "nvs keys: missing namespace -> 400");

    world_reset();
    set_query("namespace=net80211");
    nvs_keys_get_handler(&s_req);
    TEST_CHECK(s_status == 400 && s_nvs_find_calls == 0, "nvs keys: missing partition -> 400");

    world_reset();
    set_query("partition=nvs&namespace=");
    nvs_keys_get_handler(&s_req);
    TEST_CHECK(s_status == 400 && s_nvs_find_calls == 0, "nvs keys: empty namespace -> 400");

    world_reset();
    set_query("partition=nvs&namespace=kiln_auth");
    s_nvs_n = 1; s_nvs_keys[0].key = "secret_hash"; s_nvs_keys[0].type = NVS_TYPE_BLOB;
    nvs_keys_get_handler(&s_req);
    TEST_CHECK(s_status == 403 && s_nvs_find_calls == 0, "nvs keys: kiln_auth refused 403 and NVS never queried");
    TEST_CHECK(!body_has("secret_hash"), "nvs keys: kiln_auth refusal leaks no key name");

    world_reset();
    set_query("partition=kiln_nvs&namespace=kiln_auth");
    nvs_keys_get_handler(&s_req);
    TEST_CHECK(s_status == 403 && s_nvs_find_calls == 0, "nvs keys: kiln_auth refused in every partition");

    world_reset();
    set_query("partition=nvs&namespace=kiln_auth2");
    s_nvs_n = 1; s_nvs_keys[0].key = "k"; s_nvs_keys[0].type = NVS_TYPE_U8;
    nvs_keys_get_handler(&s_req);
    TEST_CHECK(s_status == 200 && s_nvs_find_calls == 1, "nvs keys: only the exact name kiln_auth is refused");

    world_reset();
    set_query("partition=nvs&namespace=net80211");
    s_nvs_find_err = ESP_ERR_NVS_PART_NOT_FOUND;
    nvs_keys_get_handler(&s_req);
    TEST_CHECK(s_status == 404, "nvs keys: unknown partition -> 404");

    world_reset();
    set_query("partition=nvs&namespace=net80211");
    nvs_keys_get_handler(&s_req);
    TEST_CHECK(s_status == 200 && body_has("\"keys\":[]"), "nvs keys: empty namespace -> 200 with an empty key list");

    world_reset();
    set_query("partition=nvs&namespace=net80211");
    s_nvs_n = 3;
    s_nvs_keys[0].key = "sta.ssid"; s_nvs_keys[0].type = NVS_TYPE_BLOB;
    s_nvs_keys[1].key = "sta.pswd"; s_nvs_keys[1].type = NVS_TYPE_STR;
    s_nvs_keys[2].key = "opmode"; s_nvs_keys[2].type = NVS_TYPE_U8;
    nvs_keys_get_handler(&s_req);
    TEST_CHECK(s_status == 200 && strcmp(s_nvs_find_part, "nvs") == 0 && strcmp(s_nvs_find_ns, "net80211") == 0,
               "nvs keys: lists the requested partition/namespace");
    TEST_CHECK(strcmp(s_body, "{\"partition\":\"nvs\",\"namespace\":\"net80211\",\"keys\":["
                              "{\"key\":\"sta.ssid\",\"type\":\"blob\"},{\"key\":\"sta.pswd\",\"type\":\"str\"},"
                              "{\"key\":\"opmode\",\"type\":\"u8\"}]}") == 0,
               "nvs keys: exact JSON, names and types only");
    TEST_CHECK(s_nvs_released == 1, "nvs keys: iterator released exactly once");

    world_reset();
    set_query("partition=nvs&namespace=net80211");
    s_nvs_n = 1; s_nvs_keys[0].key = "x"; s_nvs_keys[0].type = (nvs_type_t)0x77;
    nvs_keys_get_handler(&s_req);
    TEST_CHECK(body_has("\"type\":\"unknown\""), "nvs keys: unknown type maps to \"unknown\"");

    world_reset();
    set_query("partition=nvs&namespace=net80211");
    s_nvs_n = 3;
    s_nvs_keys[0].key = "a"; s_nvs_keys[0].type = NVS_TYPE_U8;
    s_nvs_keys[1].key = "b"; s_nvs_keys[1].type = NVS_TYPE_U8;
    s_nvs_keys[2].key = "c"; s_nvs_keys[2].type = NVS_TYPE_U8;
    s_chunk_fail_at = 3;
    esp_err_t e = nvs_keys_get_handler(&s_req);
    TEST_CHECK(e != ESP_OK && s_nvs_released == 1 && !body_has("]}"),
               "nvs keys: failed chunk send aborts the stream, releases the iterator, sends no closing tail");
}

// ---------------------------------------------------------------------------
// timing / registration
// ---------------------------------------------------------------------------
static void test_timing_registration(void)
{
    world_reset();
    diagnostics_timing_get_handler(&s_req);
    TEST_CHECK(s_status == 200 && body_has("\"count\":3,\"last\":11,\"min\":5,\"max\":99,\"mean\":22"), "timing: display block reads lvgl stats");
    TEST_CHECK(body_has("\"count\":4,\"last\":1,\"min\":2,\"max\":3,\"mean\":5"), "timing: thermo block reads MAX31856 stats");

    world_reset();
    TEST_CHECK(diagnostics_http_start(NULL) == ESP_OK, "start: success");
    TEST_CHECK(s_reg_n == 26, "start: 26 routes registered (update the uri-handler-cap check if this changes)");
    int dup = 0;
    for (int i = 0; i < s_reg_n && i < MAX_REG; i++)
        for (int j = 0; j < i; j++)
            if (strcmp(s_reg[i].uri, s_reg[j].uri) == 0 && s_reg[i].method == s_reg[j].method) dup++;
    TEST_CHECK(dup == 0, "start: no duplicate (uri, method) registration");

    world_reset();
    s_server_handle = NULL;
    TEST_CHECK(diagnostics_http_start(NULL) != ESP_OK && s_reg_n == 0, "start: no HTTP server -> error, nothing registered");

    world_reset();
    s_reg_fail_at = 5;
    TEST_CHECK(diagnostics_http_start(NULL) != ESP_OK && s_reg_n == 5, "start: a failing registration stops the sequence and propagates");
}

int main(void)
{
    test_crash_report();
    test_crash_report_ack_clear();
    test_estop_verify();
    test_coredump();
    test_watchdog_ramp();
    test_relay_cycles();
    test_danger();
    test_cfgfs();
    test_nvs_keys();
    test_timing_registration();
    printf("test_diagnostics_http: %d checks, %d failures\n", g_test_count, g_test_failures);
    return g_test_failures ? 1 : 0;
}
