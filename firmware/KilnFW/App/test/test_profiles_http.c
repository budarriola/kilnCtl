// Host test for App/drivers/http/profiles_http.c's on-flash blob decoding
// (decode_profile_blob()/nvs_load_all_from(), both `static`) -- added
// 2026-08-27 for the exact data-loss defect PROFILE_VERSION's own header
// comment in profiles_http.c documents: an earlier draft of the 1->2 crc32
// hardening computed expected_len_for_version(1) as sizeof(the CURRENT
// struct, crc32 included) instead of the historical pre-crc32 size, which
// made every already-saved version-1 blob "the wrong length for its claimed
// version" and silently wiped both of a bench board's saved profiles on the
// very firmware meant to protect them. test_v1_blob_loads_and_preserves_
// all_fields() below is that regression test.
//
// This is its own SEPARATE host-test executable (own main(), not merged into
// test_main.c/kilnctl_host_tests.exe) -- see build_host_tests.ps1's seventh
// build+run step, added alongside this file. Same reason test_zones_http.c
// is its own executable: decode_profile_blob()/nvs_load_all_from() are
// `static`, so the only way to reach them directly is to #include
// profiles_http.c itself, and that pulls in the whole file's handler surface
// (page GET, JSON GET/POST, builtin hide/restore) which must link even
// though these tests never call any of it.
//
// NVS STUB: stubs/nvs.h's shared single-blob-slot stub (used by
// test_zones_http.c) cannot model this file's needs -- nvs_load_all_from()
// reads ONE "prof_used" bitmap key plus up to PROFILES_MAX_COUNT separate
// "profN" blob keys from the SAME open handle, and the "one bad slot must
// not take out the others" test (TODO.md 8.1) genuinely needs two distinct
// keys holding two distinct blobs at once. So this file defines its own
// small multi-key (partition,namespace,key) -> blob map, built the same way
// stubs/nvs.h's is (real semantics: NOT_FOUND when absent, INVALID_SIZE when
// the caller's buffer is too small), and #define's TEST_STUB_NVS_H before
// profiles_http.c's own `#include "nvs.h"` reaches the real stub header, so
// that header's include guard makes it a no-op include and this file's
// definitions are the only ones in play. Everything else (esp_err.h,
// esp_http_server.h, esp_log.h, esp_crc.h, nvs_flash.h, MAX31856.h's
// driver/spi_master.h etc.) still comes from stubs/ via /I, unchanged.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

// Ahead of profiles_http.c's own #includes, purely for the TYPES the stub
// bodies below need (same convention test_zones_http.c/test_backup_import.c
// use).
#include "esp_err.h"
#include "esp_http_server.h"
#include "psa/crypto.h"

// psa/crypto.h's host stub declares this `extern` (only ONE definition per
// executable) -- this executable now links web_auth_store.c
// (docs/WEB_AUTH_PLAN.md section 5/9 route rewiring pulling in
// http_auth_policy_iface.c) so it needs its own copy.
psa_status_t g_stub_psa_import_key_result = PSA_SUCCESS;

// stubs/esp_http_server.h (added for wifi_prov.c/zones_http.c/backup_http.c)
// has never needed these two query-string helpers before -- profiles_http.c
// is the first host-tested file to call them (builtin_list_get_handler()'s
// ?all=1 and profile_detail_get_handler()'s ?id=). Declared here, ahead of
// profiles_http.c's own #include, so the compiler has a real prototype
// rather than silently assuming an implicit `int` return for an
// esp_err_t-returning function; bodies are defined further down, same
// per-executable "declared once here, defined once per test file" split the
// rest of this stub surface already uses.
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t buf_len);
esp_err_t httpd_query_key_value(const char *qs, const char *key, char *val, size_t val_size);

// stubs/esp_http_server.h's httpd_err_code_t enum only lists the two codes
// prior host-tested files needed (400/500) -- profiles_http.c is the first
// to send a 404 (no such profile / no such built-in schedule). Enums are
// int-compatible in C, so passing this literal where an httpd_err_code_t is
// expected is legal without editing that shared enum.
#define HTTPD_404_NOT_FOUND 404

// ---------------------------------------------------------------------------
// HAL_KV shim -- HW_ABSTRACTION.md Phase 3 item 3 migrated
// profiles_http.c off nvs.h onto hal_kv.h, so this file no longer needs a
// hand-rolled nvs_* stub: it links the REAL host hal_kv backend (fake_kv.c,
// see build_host_tests.ps1's cmd7) and keeps only thin nvs_*()-named
// wrappers so every existing test body below (stage_profile_blob(),
// nvs_stub_find(), etc.) keeps compiling unchanged. fake_kv.c already models
// exactly what this file used to hand-roll -- multiple (partition,
// namespace, key) slots live at once, NOT_FOUND when absent, INVALID_SIZE
// when the caller's buffer is too small -- so nothing here duplicates that
// logic anymore.
// ---------------------------------------------------------------------------
#include "hal_kv.h"
#include "fake_kv.h"

typedef hal_kv_handle_t *nvs_handle_t;

typedef enum {
    NVS_READONLY = 0,
    NVS_READWRITE,
} nvs_open_mode_t;

#define NVS_STUB_BLOB_CAP FAKE_KV_MAX_VALUE_BYTES
#define NVS_STUB_HANDLE_POOL 8

static hal_kv_handle_t s_kv_handle_pool[NVS_STUB_HANDLE_POOL];
static int s_kv_handle_next = 0;

static esp_err_t hal_to_esp(hal_status_t st)
{
    switch (st) {
    case HAL_OK:          return ESP_OK;
    case HAL_NOT_FOUND:   return ESP_ERR_NVS_NOT_FOUND;
    case HAL_INVALID_SIZE: return ESP_ERR_INVALID_SIZE;
    case HAL_INVALID_ARG: return ESP_ERR_INVALID_ARG;
    case HAL_NO_MEM:      return ESP_ERR_NO_MEM;
    case HAL_NOT_READY:   return ESP_ERR_INVALID_STATE;
    default:              return ESP_FAIL;
    }
}

// nvs_stub_reset() now also (re)creates both partitions this file's tests
// stage into -- fake_kv_reset_all() wipes hal_kv_init_partition()'s
// bookkeeping along with every key, so each test must redo that init, same
// as profiles_http.c's own nvs_partition_init() would on a real boot.
/* flash_worker_wait.c (linked for pref_cfg_fs) asks whether the flash worker
 * started; on the host there is no worker, so answer yes at once. */
bool uart_bridge_ext_flash_worker_started(void)
{
    return true;
}

static void pcfg_mount_fresh(void); /* defined with the cfg_fs section below */
esp_err_t profiles_favorites_start(void); /* real persist/profiles_favorites.c is linked; reset its RAM masks */
static void nvs_stub_reset(void)
{
    pcfg_mount_fresh(); /* profile saves are cfg-file-only: every reset starts from a clean, mounted cfg */
    fake_kv_reset_all();
    hal_kv_init_partition("profiles_nvs"); /* PROFILES_NVS_PARTITION's literal -- that macro isn't
                                             * defined until profiles_http.c's own #include below */
    hal_kv_init_partition(NULL); /* the default partition, for the pre-split migration tests */
    s_kv_handle_next = 0;
    (void)profiles_favorites_start(); /* RAM masks back to the (empty) mounted-cfg + empty-NVS state */
}

static esp_err_t nvs_open_from_partition(const char *partition, const char *ns, int mode, nvs_handle_t *out)
{
    hal_kv_mode_t m = (mode == NVS_READWRITE) ? HAL_KV_MODE_READ_WRITE : HAL_KV_MODE_READ_ONLY;
    hal_kv_handle_t *h = &s_kv_handle_pool[s_kv_handle_next % NVS_STUB_HANDLE_POOL];
    s_kv_handle_next++;
    hal_status_t err = hal_kv_open(h, ns, m, partition);
    if (err != HAL_OK) {
        return hal_to_esp(err);
    }
    if (out) {
        *out = h;
    }
    return ESP_OK;
}

static void nvs_close(nvs_handle_t h) { hal_kv_close(h); }

static esp_err_t nvs_commit(nvs_handle_t h) { return hal_to_esp(hal_kv_commit(h)); }

static esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out)
{
    return hal_to_esp(hal_kv_get_u8(h, key, out));
}

static esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t val)
{
    return hal_to_esp(hal_kv_set_u8(h, key, val));
}

static esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    return hal_to_esp(hal_kv_get_blob(h, key, out, len));
}

static esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *val, size_t len)
{
    return hal_to_esp(hal_kv_set_blob(h, key, val, len));
}

static esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    return hal_to_esp(hal_kv_erase_key(h, key));
}

// Read-only peek used by test 5 (test_newer_version_refused_not_wiped) to
// prove a refused blob's on-flash bytes survive byte-for-byte -- same
// (partition, namespace, key) -> blob lookup the old hand-rolled
// nvs_stub_entry_t offered, now backed by a real hal_kv_get_blob() call.
typedef struct {
    bool   has_blob;
    uint8_t blob[NVS_STUB_BLOB_CAP];
    size_t blob_len;
} nvs_stub_entry_t;
static nvs_stub_entry_t s_nvs_stub_peek;

static nvs_stub_entry_t *nvs_stub_find(const char *partition, const char *ns, const char *key, bool create)
{
    (void)create; /* peek-only: every caller in this file passes false */
    hal_kv_handle_t h;
    if (hal_kv_open(&h, ns, HAL_KV_MODE_READ_ONLY, partition) != HAL_OK) {
        return NULL;
    }
    memset(&s_nvs_stub_peek, 0, sizeof(s_nvs_stub_peek));
    size_t len = sizeof(s_nvs_stub_peek.blob);
    hal_status_t err = hal_kv_get_blob(&h, key, s_nvs_stub_peek.blob, &len);
    hal_kv_close(&h);
    if (err != HAL_OK) {
        return NULL;
    }
    s_nvs_stub_peek.has_blob = true;
    s_nvs_stub_peek.blob_len = len;
    return &s_nvs_stub_peek;
}

// asm("_binary_...") is a GCC/binutils extension with no MSVC equivalent --
// #define it away, same convention test_zones_http.c uses.
#define asm(x)

#include "../drivers/http/profiles_http.c"
// profiles_http.c split 2026-09-04 (ROADMAP.md M15, the 1500-line rule) --
// #include the sibling files alongside it, same convention
// test_wifi_prov.c/test_autotune_engine_prestart.c use for their own splits.
#include "../drivers/http/profiles_catalog_http.c"
#include "../drivers/http/profiles_edit_http.c"
#include "save_section_probe.h"

#undef asm

// docs/PROFILE_SLOTS_100.md section 7 task 6's bench-slot exclusion
// test needs PROFILE_BENCH_SLOT_ID and the real LCD-picker deletability check.
#include "../drivers/persist/profiles_bench_slot.h"
#include "../drivers/ui/ui_page_profile_picker_format.h"

// ---------------------------------------------------------------------------

// ---- Embedded-page symbol page_get_handler() references -------------------
const uint8_t profiles_page_html_gz_start[1] = { 0 };
const uint8_t profiles_page_html_gz_end[1] = { 0 };

// ---- esp_http_server.h stub bodies -----------------------------------------
// None of these is ever invoked by this file's tests directly (only
// decode_profile_blob()/nvs_load_all_from() and, for a couple of tests,
// profiles_http_start()/profiles_list_get_handler() are called), but every
// symbol profiles_http.c references anywhere in the file must resolve at
// link time.
static char s_chunk_capture[32768];
static size_t s_chunk_capture_len = 0;
static bool s_chunk_capture_on = false;

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
// 2026-09-29: http_auth_http.c now calls wifi_prov_request_arrived_on_ap(httpd_req_to_sockfd(req))
// to tag each session touch with whether it arrived over the SoftAP interface.
int httpd_req_to_sockfd(httpd_req_t *r) { (void)r; return -1; }
static bool s_floors_unknown_hdr_set;
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value)
{
    (void)r;
    if (field && value && strcmp(field, "X-Profile-Floors-Unknown") == 0 && strcmp(value, "1") == 0) {
        s_floors_unknown_hdr_set = true;
    }
    return ESP_OK;
}
void web_set_asset_cache_headers(httpd_req_t *r);
void web_set_asset_cache_headers(httpd_req_t *r) { (void)r; }
// Same convention as s_chunk_capture above, for the (much rarer) handlers
// that reply via a single httpd_resp_send() rather than chunked -- notably
// profile_detail_get_handler(), which test_profile_detail_json_valid_at_
// max_capacity() below drives directly.
static char s_send_capture[16384];
static size_t s_send_capture_len = 0;
static bool s_send_capture_on = false;

// ---- POST-body/response capture for profile_post_handler() -- Opus review
// finding A (malformed-JSON-on-collision): these tests drive
// profile_post_handler() through a real (fake) httpd_req_t rather than only
// profiles_http_save()/live_edit_name_collides() directly, so they exercise
// the ACTUAL bytes the handler sends, catching an unescaped '"' the same way
// profiles_page.html's r.json() would (by trying to parse it). s_post_body/
// s_post_body_left let httpd_req_recv() serve a caller-supplied body instead
// of always reporting EOF; s_resp_capture/-_on mirror s_chunk_capture's
// pattern above for httpd_resp_send() (not httpd_resp_send_chunk() -- the
// non-chunked path profile_post_handler() actually uses for every response).
static const char *s_post_body;
static size_t s_post_body_left;
// Fuzz hooks: after s_fail_after bytes were served recv returns s_fail_ret (0 EOF, <0 error).
static size_t s_post_served;
static size_t s_fail_after = (size_t)-1;
static int s_fail_ret = 0;
static int s_fake_hide_calls = 0;
static int s_fake_restore_calls = 0;
static char s_resp_capture[2048];
static size_t s_resp_capture_len;
static bool s_resp_capture_on;

esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, long long buf_len)
{
    (void)r;
    if (s_send_capture_on && buf) {
        long long n = buf_len < 0 ? (long long)strlen(buf) : buf_len;
        if (n >= (long long)sizeof(s_send_capture)) {
            n = (long long)sizeof(s_send_capture) - 1;
        }
        memcpy(s_send_capture, buf, (size_t)n);
        s_send_capture[n] = '\0';
        s_send_capture_len = (size_t)n;
    }
    if (s_resp_capture_on && buf && buf_len > 0) {
        size_t n = (size_t)buf_len;
        if (n >= sizeof(s_resp_capture)) {
            n = sizeof(s_resp_capture) - 1;
        }
        memcpy(s_resp_capture, buf, n);
        s_resp_capture[n] = '\0';
        s_resp_capture_len = n;
    }
    return ESP_OK;
}
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, size_t buf_len)
{
    (void)r;
    if (s_chunk_capture_on && buf && buf_len > 0) {
        if (s_chunk_capture_len + buf_len < sizeof(s_chunk_capture)) {
            memcpy(s_chunk_capture + s_chunk_capture_len, buf, buf_len);
            s_chunk_capture_len += buf_len;
            s_chunk_capture[s_chunk_capture_len] = '\0';
        }
    }
    return ESP_OK;
}
static int s_last_err_code;
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t error, const char *msg)
{
    (void)r;
    s_last_err_code = (int)error;
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
    if (s_fail_after != (size_t)-1 && s_post_served >= s_fail_after) {
        return s_fail_ret;
    }
    if (s_post_body && s_post_body_left > 0) {
        size_t n = buf_len < s_post_body_left ? buf_len : s_post_body_left;
        if (s_fail_after != (size_t)-1 && s_post_served + n > s_fail_after) {
            n = s_fail_after - s_post_served;
        }
        s_post_served += n;
        memcpy(buf, s_post_body, n);
        s_post_body += n;
        s_post_body_left -= n;
        return (int)n;
    }
    return 0;
}
// Controllable per test (default NULL, matching the always-ESP_FAIL behavior
// every existing test in this file relies on) -- test_profile_detail_json_
// valid_at_max_capacity() below is the first test in this file that needs
// profile_detail_get_handler()'s ?id= query to actually resolve.
static const char *s_stub_query_str = NULL;
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t buf_len)
{
    (void)r;
    if (!s_stub_query_str) {
        return ESP_FAIL;
    }
    snprintf(buf, buf_len, "%s", s_stub_query_str);
    return strlen(s_stub_query_str) >= buf_len ? ESP_ERR_HTTPD_RESULT_TRUNC : ESP_OK;
}
esp_err_t httpd_query_key_value(const char *qs, const char *key, char *val, size_t val_size)
{
    if (!qs) {
        return ESP_FAIL;
    }
    size_t klen = strlen(key);
    const char *p = qs;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *amp = strchr(v, '&');
            size_t vlen = amp ? (size_t)(amp - v) : strlen(v);
            if (vlen >= val_size) {
                vlen = val_size - 1;
            }
            memcpy(val, v, vlen);
            val[vlen] = '\0';
            return ESP_OK;
        }
        p = strchr(p, '&');
        if (p) {
            p++;
        }
    }
    return ESP_FAIL;
}

// ---- web_encoding.h -- only reached from page_get_handler(), never called.
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

// ---- wifi_provision_http.h -- only reached from profiles_http_start(),
// never called directly by these tests.
httpd_handle_t wifi_provision_http_get_server(void)
{
    return NULL;
}

// ---- zones_http.h -- profiles_http.c reads zone config through these
// getters. Fixed, generous stand-ins: thermo_count=8 (every zone_mask bit
// legal) and a high ramp ceiling (so the feasibility gate never blocks a
// test unrelated to it). None of this file's tests exercises feasibility
// rejection -- that is profiles_parse_profile_fields()/profile_post_handler()
// territory, already covered indirectly by zones_http.c's own tests using
// the same pattern for zones config.
// Overridable so the builtin-catalogue emitter test below can pin a
// realistic 3-zone board and assert the exact zone_mask that produces (0x7),
// rather than only the all-ones 0xFF an 8-zone stand-in gives -- 0xFF is the
// one value that would still look right if the resolution helper were wrong
// in the "n >= 8" direction.
static uint8_t g_stub_thermo_count = 8;
uint8_t zones_config_get_thermo_count(void)
{
    return g_stub_thermo_count;
}
// 1000.0f by default (never a feasibility constraint unless a test opts in),
// overridable per-zone for the live-edit HARD-mode ramp-ceiling refusal
// tests below.
static float g_stub_zone_max_ramp_c_per_hr[8] = {1000.0f, 1000.0f, 1000.0f, 1000.0f,
                                                  1000.0f, 1000.0f, 1000.0f, 1000.0f};
bool zones_config_get_max_ramp(uint8_t zone_index, float *out_c_per_hr)
{
    if (out_c_per_hr) *out_c_per_hr = (zone_index < 8) ? g_stub_zone_max_ramp_c_per_hr[zone_index] : 1000.0f;
    return true;
}

// Controllable per-zone max_temp_c (guard 5 ceiling) -- 0.0f by default for
// every zone, same "uncommissioned" convention zones_http.c itself uses.
// profiles_http_save()'s profile_exceeds_zone_ceiling() check (2026-09-02
// owner correction: save-time is now advisory-only, never a refusal --
// see that function's own comment in profiles_http.c) reads this getter,
// same as profile_executor_run()'s separate run-time re-check reads its own
// copy of this stub in test_profile_executor_prestart.c.
static float g_stub_zone_max_temp_c[8];
bool zones_config_get_temp_limits(uint8_t zone_index, float *out_max_temp_c, float *out_min_temp_c)
{
    if (out_max_temp_c) {
        *out_max_temp_c = (zone_index < 8) ? g_stub_zone_max_temp_c[zone_index] : 0.0f;
    }
    if (out_min_temp_c) {
        *out_min_temp_c = 0.0f;
    }
    return true;
}

// ---- zones_config_accessors.h -- validate_on_off_rules() (plan step 5)
// reads a candidate rule's zone_index's type. Controllable per test, default
// ZONE_TYPE_HEATER for every zone (same "generous stand-in until a test
// says otherwise" convention as the stubs above) -- a test that needs an
// on/off zone (or, for the negative test, a HEATER one on purpose) sets
// g_stub_zone_type[N] first.
static zone_type_t g_stub_zone_type[8];
bool zones_config_get_zone_type(uint8_t zone_index, zone_type_t *out_type)
{
    if (out_type) {
        *out_type = (zone_index < 8) ? g_stub_zone_type[zone_index] : ZONE_TYPE_HEATER;
    }
    return zone_index < 8;
}

// ---- aux_outputs_cfg.h -- spare-relay aux outputs (WP-4). Controllable per test;
// default: every aux disabled (today's behaviour). g_stub_aux[i] = relay i+1.
static aux_output_t g_stub_aux[AUX_OUTPUTS_COUNT];
/* Seam: when armed, the first aux read made while the save lock is held disables
 * the aux first, as a convert commit landing between pre-validate and lock would. */
static bool g_aux_flip_under_lock_armed;
static uint8_t g_aux_flip_relay;
static void aux_flip_under_lock_hook(void)
{
    if (g_aux_flip_under_lock_armed && g_test_stub_lock_depth > 0) {
        g_aux_flip_under_lock_armed = false;
        g_stub_aux[g_aux_flip_relay - 1].enabled = false;
    }
}
bool aux_outputs_cfg_get(uint8_t relay, aux_output_t *out)
{
    aux_flip_under_lock_hook();
    if (relay < 1 || relay > AUX_OUTPUTS_COUNT || !out) return false;
    *out = g_stub_aux[relay - 1];
    return true;
}
uint8_t aux_outputs_cfg_enabled_mask(void)
{
    aux_flip_under_lock_hook();
    uint8_t m = 0;
    for (uint8_t i = 0; i < AUX_OUTPUTS_COUNT; i++) {
        if (g_stub_aux[i].enabled && !g_stub_aux[i].conflicted) m |= (uint8_t)(1u << i);
    }
    return m;
}

// Controllable by test_validate_io_segment_zone_ownership() -- bit N-1 of
// this mask set means "zone 0 owns relay N", matching zone_cfg_t::relay_mask's
// own bit convention. Every other zone (1-7) always reports "no mask", same
// as the fixed thermo_count=8 above being a "generous stand-in, not exercised
// further" choice for every test that isn't specifically about relay
// ownership.
static uint8_t g_zone_relay_mask_zone0 = 0x00;
bool zones_config_get_relay_mask(uint8_t zone_index, uint8_t *out_mask)
{
    if (out_mask) *out_mask = (zone_index == 0) ? g_zone_relay_mask_zone0 : 0x00;
    return true;
}

// ---- profile_feasibility.h -- only reached from the JSON GET handlers'
// per-entry feasibility annotation, never asserted on by these tests.
static uint8_t g_last_feasibility_zone_mask = 0xAA; /* sentinel: never a real mask here */
profile_seg_verdict_t profile_feasibility_profile_mask(uint8_t zone_mask, const profile_t *p,
                                                       profile_seg_verdict_t *out_segments, size_t out_cap)
{
    g_last_feasibility_zone_mask = zone_mask;
    (void)p;
    for (size_t i = 0; i < out_cap; i++) {
        out_segments[i] = PROFILE_SEG_UNKNOWN;
    }
    return PROFILE_SEG_UNKNOWN;
}
const char *profile_feasibility_verdict_str(profile_seg_verdict_t v)
{
    (void)v;
    return "unknown";
}

// ---- profiles_builtin.h -- empty catalogue: these tests only care about
// user-slot behaviour, so the builtin loop in profiles_list_get_handler()/
// builtin_list_get_handler() simply iterates zero times.
const builtin_profile_t g_builtin_profiles[1] = { { { 0 }, NULL, NULL, NULL, PROFILE_FIRING_BISQUE, 0, 0, { { 0 } } } };
const size_t g_builtin_profile_count = 0;

// One optional fake catalogue entry, off by default so every test above is
// untouched. g_builtin_profiles/g_builtin_profile_count deliberately STAY
// empty: the list handlers' loops walk those, and the emitter test below
// calls send_builtin_summary()/send_builtin_full() directly (both are
// `static` in profiles_catalog_http.c, which this file #includes), so it
// drives the real production emitters without perturbing any existing
// list-response assertion.
static bool              g_fake_builtin_on = false;
static builtin_profile_t g_fake_builtin;
static profile_t         g_fake_builtin_profile;

bool profiles_builtin_id_valid(uint8_t id)
{
    return g_fake_builtin_on && id == PROFILE_BUILTIN_ID_BASE;
}
bool profiles_builtin_get(uint8_t id, profile_t *out)
{
    if (!profiles_builtin_id_valid(id) || !out) {
        return false;
    }
    *out = g_fake_builtin_profile;
    return true;
}
const builtin_profile_t *profiles_builtin_entry(uint8_t id)
{
    return profiles_builtin_id_valid(id) ? &g_fake_builtin : NULL;
}
bool profiles_builtin_is_hidden(uint8_t id)
{
    (void)id;
    return false;
}
esp_err_t profiles_builtin_set_hidden(uint8_t id, bool hidden)
{
    (void)id;
    (void)hidden;
    s_fake_hide_calls++;
    return ESP_OK;
}
esp_err_t profiles_builtin_restore_all(void)
{
    s_fake_restore_calls++;
    return ESP_OK;
}

// PROFILE_SLOTS_100.md section 7 task 8: fake for the narrow accessor
// profiles_catalog_http.c forward-declares (rather than #including
// profile_executor.h -- see that declaration's comment). Defaults to 0
// ("never fired") so every existing list/exceeds_ceiling assertion in this
// file keeps passing unchanged; g_fake_last_run_unix_s lets a test opt into
// a nonzero value to verify the new "last_run_started_unix_s" JSON field.
static uint32_t g_fake_last_run_unix_s = 0;
uint32_t profile_executor_last_run_started_unix_s(uint8_t profile_id)
{
    (void)profile_id;
    return g_fake_last_run_unix_s;
}

// ---- profile_executor.h -- fake firing_stats_erase(): profiles_http.c
// (#included above) now calls this from nvs_erase_slot() (docs/
// PROFILE_SLOTS_100.md section 7 task 10). The real definition lives in
// profile_executor_firing_stats.c, a control-tier file with its own heavy
// dependency set (esp_heap_caps, zones_config_accessors.h, the executor's
// internal state) this HTTP-tier executable has no other reason to link --
// same reasoning as the profiles_builtin.h/profile_feasibility.h fakes
// above. This fake only records the call (last id + count) so tests below
// can assert nvs_erase_slot() reaches it with the right id, exactly once,
// without needing a real firing-stats store here.
static esp_err_t g_firing_stats_erase_result = ESP_OK;
static int     g_firing_stats_erase_calls = 0;
static uint8_t g_firing_stats_erase_last_id = 0xFF;
esp_err_t firing_stats_erase(uint8_t profile_id)
{
    g_firing_stats_erase_calls++;
    g_firing_stats_erase_last_id = profile_id;
    return g_firing_stats_erase_result;
}

// ---- profile_executor.h -- fake profile_executor_get_status(): Opus review
// item 2 (PROFILE_SLOTS_100.md section 7) has profiles_http_delete()
// refuse to delete the slot the executor is currently running/paused on.
// Defaults to IDLE (nothing running); tests that need a "delete refused"
// case set g_fake_exec_state/g_fake_exec_profile_id first.
_Static_assert(sizeof(profile_exec_status_t) == 1512, "profile_exec_status_t size: comments across drivers/ cite 1512 B -- update them");
static profile_exec_state_t g_fake_exec_state = PROFILE_EXEC_IDLE;
static uint8_t              g_fake_exec_profile_id = 0xFF;
void profile_executor_get_status(profile_exec_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = g_fake_exec_state;
    out->profile_id = g_fake_exec_profile_id;
}

/* profiles_http_delete() now calls this narrow accessor instead of the
 * full-struct getter (2026-09-22 exec-status stack-local audit) -- fake
 * derives the same RUNNING-or-PAUSED answer from the same g_fake_exec_*
 * state the profile_executor_get_status() fake above uses, so every
 * existing "delete refused because it's the running/paused slot" test
 * keeps exercising the identical scenario through the new call path. */
static uint8_t s_l23_id;                    /* L23 race test below */
static int s_l23_runnable_at_exec_check = -1; /* runnable seen by the running check */
static bool s_l23_probe_exec;
bool profile_executor_get_active_id(uint8_t *out_id)
{
    /* First call only: the delete's running check. Later calls (from the
     * erase path) must not overwrite what that check saw. */
    if (s_l23_probe_exec && s_l23_runnable_at_exec_check < 0) {
        s_l23_runnable_at_exec_check = profiles_http_slot_runnable(s_l23_id) ? 1 : 0;
    }
    bool active = (g_fake_exec_state == PROFILE_EXEC_RUNNING || g_fake_exec_state == PROFILE_EXEC_PAUSED);
    if (out_id) {
        *out_id = g_fake_exec_profile_id;
    }
    return active;
}

// NOTE: profiles_favorites_set()/profiles_favorites_is() are NOT faked here
// -- this executable links persist/profiles_favorites.c for REAL (see
// build_host_tests.ps1's exe7 comment: "a fake would not exercise the
// delete-clears-the-favorite path"). Opus review item 1's test below marks
// a slot favorite via the real profiles_favorites_set() and asserts through
// the real profiles_favorites_is() that profiles_http_delete() clears it.

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

// Source-text-scan helpers, same convention as test_zone_sweep_relay_off_
// wiring.c / test_display_power_wiring.c: this codebase's own functions
// close at column 0, so the first "\n}" after the opening brace is the real
// end of the function.
static const char *find_function_body(const char *text, const char *sig, size_t *out_len)
{
    const char *s = strstr(text, sig);
    if (!s) {
        return NULL;
    }
    const char *open = strchr(s, '{');
    if (!open) {
        return NULL;
    }
    const char *close = strstr(open, "\n}");
    if (!close) {
        return NULL;
    }
    *out_len = (size_t)(close - open);
    return open;
}

static char *dup_range(const char *start, size_t len)
{
    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        return NULL;
    }
    memcpy(buf, start, len);
    buf[len] = '\0';
    return buf;
}

static void stage_bitmap(uint8_t bitmap)
{
    nvs_handle_t h;
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    nvs_set_u8(h, NVS_KEY_USED, bitmap);
    nvs_close(h);
}

static void stage_profile_blob(uint8_t id, const void *data, size_t len)
{
    nvs_handle_t h;
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    char key[8];
    profile_nvs_key(id, key, sizeof(key));
    nvs_set_blob(h, key, data, len);
    nvs_close(h);
}

// A fully-populated profile_t with distinct, checkable field values -- what
// a real, already-saved profile actually looks like.
static profile_t make_stored_profile(void)
{
    profile_t p;
    memset(&p, 0, sizeof(p));
    strncpy(p.name, "ConeSix", PROFILE_NAME_MAX_LEN);
    p.zone_mask = 0x05;
    p.segment_count = 3;
    p.segments[0].target_c = 200.0f;
    p.segments[0].ramp_c_per_hr = 100.0f;
    p.segments[0].dwell_min = 10;
    p.segments[1].target_c = 1000.0f;
    p.segments[1].ramp_c_per_hr = 150.0f;
    p.segments[1].dwell_min = 0;
    p.segments[2].target_c = 1220.0f;
    p.segments[2].ramp_c_per_hr = 60.0f;
    p.segments[2].dwell_min = 20;
    return p;
}

// profile_persisted_v1_t/profile_persisted_v2_t hold the OLD (pre-relay/IO,
// 2026-08-27) profile_t_v2 payload, not today's profile_t -- see
// profiles_http.c's PROFILE_VERSION comment for exactly why a bare struct
// assignment across that shape change is the bug this whole file exists to
// catch. This is the field-by-field down-converter these tests need to build
// a historical blob at all; note it deliberately does NOT round-trip the new
// seg_kind/io_* fields (there is nothing on the v1/v2 side to carry them).
static profile_t_v2 to_v2(const profile_t *src)
{
    profile_t_v2 v2;
    memset(&v2, 0, sizeof(v2));
    strncpy(v2.name, src->name, sizeof(v2.name) - 1);
    v2.zone_mask = src->zone_mask;
    v2.segment_count = src->segment_count;
    for (uint8_t i = 0; i < src->segment_count && i < PROFILE_MAX_SEGMENTS; i++) {
        v2.segments[i].target_c = src->segments[i].target_c;
        v2.segments[i].ramp_c_per_hr = src->segments[i].ramp_c_per_hr;
        v2.segments[i].dwell_min = src->segments[i].dwell_min;
    }
    return v2;
}

// profile_persisted_v3_t holds the CURRENT (post-relay/IO) segment shape but
// no on/off-rule tail -- this is the down-converter for building a real v3
// blob to feed nvs_load_all_from()/profile_decode_blob() with.
static profile_t_v3 to_v3(const profile_t *src)
{
    profile_t_v3 v3;
    memset(&v3, 0, sizeof(v3));
    strncpy(v3.name, src->name, sizeof(v3.name) - 1);
    v3.zone_mask = src->zone_mask;
    v3.segment_count = src->segment_count;
    for (uint8_t i = 0; i < src->segment_count && i < PROFILE_MAX_SEGMENTS; i++) {
        v3.segments[i] = src->segments[i];
    }
    return v3;
}

static void assert_profiles_equal(const profile_t *a, const profile_t *b, const char *ctx)
{
    char msg[128];
    snprintf(msg, sizeof(msg), "%s: name preserved", ctx);
    TEST_CHECK(strcmp(a->name, b->name) == 0, msg);
    snprintf(msg, sizeof(msg), "%s: zone_mask preserved", ctx);
    TEST_CHECK(a->zone_mask == b->zone_mask, msg);
    snprintf(msg, sizeof(msg), "%s: segment_count preserved", ctx);
    TEST_CHECK(a->segment_count == b->segment_count, msg);
    for (uint8_t i = 0; i < a->segment_count && i < PROFILE_MAX_SEGMENTS; i++) {
        snprintf(msg, sizeof(msg), "%s: segment %u target_c preserved", ctx, i);
        TEST_CHECK_NEAR(a->segments[i].target_c, b->segments[i].target_c, 1e-6, msg);
        snprintf(msg, sizeof(msg), "%s: segment %u ramp_c_per_hr preserved", ctx, i);
        TEST_CHECK_NEAR(a->segments[i].ramp_c_per_hr, b->segments[i].ramp_c_per_hr, 1e-6, msg);
        snprintf(msg, sizeof(msg), "%s: segment %u dwell_min preserved", ctx, i);
        TEST_CHECK(a->segments[i].dwell_min == b->segments[i].dwell_min, msg);
    }
    // docs/ON_OFF_ZONE.md plan step 5 -- every existing caller of this
    // helper now also proves the on/off rule tail round-trips (or, for a
    // profile that never had any, stays at 0 -- this is what makes every
    // v1/v2/v3 migration test in this file double as a "rules-free profile
    // is unaffected" proof without needing a separate assertion helper).
    snprintf(msg, sizeof(msg), "%s: on_off_rule_count preserved", ctx);
    TEST_CHECK(a->on_off_rule_count == b->on_off_rule_count, msg);
    for (uint8_t i = 0; i < a->on_off_rule_count && i < PROFILE_MAX_ON_OFF_RULES; i++) {
        const profile_on_off_rule_t *ra = &a->on_off_rules[i];
        const profile_on_off_rule_t *rb = &b->on_off_rules[i];
        snprintf(msg, sizeof(msg), "%s: rule %u segment_index preserved", ctx, i);
        TEST_CHECK(ra->segment_index == rb->segment_index, msg);
        snprintf(msg, sizeof(msg), "%s: rule %u zone_index preserved", ctx, i);
        TEST_CHECK(ra->zone_index == rb->zone_index, msg);
        snprintf(msg, sizeof(msg), "%s: rule %u enable preserved", ctx, i);
        TEST_CHECK(ra->enable == rb->enable, msg);
        snprintf(msg, sizeof(msg), "%s: rule %u phase_mask preserved", ctx, i);
        TEST_CHECK(ra->phase_mask == rb->phase_mask, msg);
        snprintf(msg, sizeof(msg), "%s: rule %u direction_mask preserved", ctx, i);
        TEST_CHECK(ra->direction_mask == rb->direction_mask, msg);
        snprintf(msg, sizeof(msg), "%s: rule %u temp_cmp preserved", ctx, i);
        TEST_CHECK(ra->temp_cmp == rb->temp_cmp, msg);
        snprintf(msg, sizeof(msg), "%s: rule %u temp_threshold_c preserved", ctx, i);
        TEST_CHECK_NEAR(ra->temp_threshold_c, rb->temp_threshold_c, 1e-6, msg);
        snprintf(msg, sizeof(msg), "%s: rule %u time window preserved", ctx, i);
        TEST_CHECK(ra->time_start_s == rb->time_start_s && ra->time_stop_s == rb->time_stop_s, msg);
        snprintf(msg, sizeof(msg), "%s: rule %u invert preserved", ctx, i);
        TEST_CHECK(ra->invert == rb->invert, msg);
    }
}

// ---------------------------------------------------------------------------
// docs/FILESYSTEM_USER_DATA.md section 5 step 4 (user-profiles
// filesystem move) -- tests for profiles_cfg_fs.c's per-slot read-through/
// dual-write bridge, added here (rather than a separate TU) because
// nvs_load_all_from() is `static` -- the exact reason this whole file is its
// own executable in the first place (see this file's header comment). Real
// cfg_fs.c runs against a temp directory on disk, same convention
// test_zones_config_cfg_fs.c uses for the zones-config equivalent.
// ---------------------------------------------------------------------------
#ifdef _WIN32
#include <direct.h>
#define TPCF_MKDIR(p) _mkdir(p)
#define TPCF_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define TPCF_MKDIR(p) mkdir((p), 0755)
#define TPCF_RMDIR(p) rmdir(p)
#endif

static const char *PCFG_SCRATCH_BASE = "cfg_fs_test_profiles";

// Removes every file this test suite could have left behind in a prior run
// (a leftover file defeats a bare rmdir(), which only succeeds against an
// empty directory -- same fix class as project_concurrent_agent_stub_collision,
// applied to this test's own scratch directory), then re-creates a clean
// base directory and resets every piece of shared state a test in this
// section could have touched.
static void pcfg_mount_fresh(void)
{
    char path[600];
    for (uint8_t id = 0; id < PROFILES_MAX_COUNT; id++) {
        char rel[40];
        profiles_cfg_fs_path(id, rel, sizeof(rel));
        snprintf(path, sizeof(path), "%s/.tmp/%s", PCFG_SCRATCH_BASE, rel);
        remove(path);
        snprintf(path, sizeof(path), "%s/%s", PCFG_SCRATCH_BASE, rel);
        remove(path);
    }
    snprintf(path, sizeof(path), "%s/.tmp/prof_fav.bin", PCFG_SCRATCH_BASE);
    remove(path);
    snprintf(path, sizeof(path), "%s/prof_fav.bin", PCFG_SCRATCH_BASE);
    remove(path);
    char tmpdir[600], profdir[600];
    snprintf(tmpdir, sizeof(tmpdir), "%s/.tmp", PCFG_SCRATCH_BASE);
    snprintf(profdir, sizeof(profdir), "%s/profiles", PCFG_SCRATCH_BASE);
    TPCF_RMDIR(tmpdir);
    TPCF_RMDIR(profdir);
    TPCF_RMDIR(PCFG_SCRATCH_BASE);
    TPCF_MKDIR(PCFG_SCRATCH_BASE);

    cfg_fs_deinit();
    profiles_cfg_fs_reset_write_fn_for_test();
    profiles_cfg_fs_reset_delete_fn_for_test();
    memset(&s_profiles, 0, sizeof(s_profiles));
    memset(s_profile_rev, 0, sizeof(s_profile_rev));
    (void)cfg_fs_init(PCFG_SCRATCH_BASE, NULL);
}

// Fresh NVS and a freshly MOUNTED, empty cfg scratch. Tests that need the
// partition absent call cfg_fs_deinit() afterwards.
static void pcfg_reset_all(void)
{
    nvs_stub_reset();
}

static esp_err_t pcfg_failing_write_fn(const char *rel_path, const void *data, size_t len)
{
    (void)rel_path;
    (void)data;
    (void)len;
    return ESP_FAIL;
}

static esp_err_t pcfg_failing_delete_fn(const char *rel_path)
{
    (void)rel_path;
    return ESP_FAIL;
}

// Same "load a slot straight from the file, no NVS involved" helper used
// throughout below to assert what actually landed on disk.
static bool pcfg_file_profile(uint8_t id, profile_t *out)
{
    uint32_t rev = 0;
    bool valid = false;
    profiles_cfg_fs_load_raw(id, out, &rev, &valid);
    return valid;
}

// Stages what a LEGACY (pre dual-write-close) firmware left in profiles_nvs
// for one slot: the decodable blob, the used bitmap and the rev array. Nothing
// in production writes these keys any more.
static void stage_legacy_slot(uint8_t id, const profile_t *p, uint32_t rev)
{
    profile_persisted_t persisted = { .version = PROFILE_VERSION, .profile = *p, .crc32 = 0 };
    persisted.crc32 = compute_profile_crc(&persisted);
    stage_profile_blob(id, &persisted, sizeof(persisted));
    profiles_slot_bitmap_t bm;
    memset(&bm, 0, sizeof(bm));
    profiles_slot_bitmap_set(&bm, id);
    nvs_handle_t h;
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    nvs_set_blob(h, NVS_KEY_USED, &bm, sizeof(bm));
    uint32_t rev_arr[PROFILES_MAX_COUNT] = {0};
    rev_arr[id] = rev;
    nvs_set_blob(h, NVS_KEY_PROFILE_REV, rev_arr, sizeof(rev_arr));
    nvs_commit(h);
    nvs_close(h);
}

static bool pcfg_nvs_slot_blob_present(uint8_t id)
{
    nvs_handle_t h;
    if (nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    char key[8];
    profile_nvs_key(id, key, sizeof(key));
    profile_persisted_t tmp;
    size_t len = sizeof(tmp);
    esp_err_t e = nvs_get_blob(h, key, &tmp, &len);
    nvs_close(h);
    return e == ESP_OK;
}

static void test_pcfg_mounted_migrates_nvs_only_slot_to_file(void)
{
    TEST_SECTION("profiles cfg_fs -- a legacy NVS-only slot is lazily migrated to a file on load");
    pcfg_reset_all();
    size_t reaped = 0;
    TEST_CHECK(cfg_fs_init(PCFG_SCRATCH_BASE, &reaped) == ESP_OK, "cfg_fs mounts against the scratch dir");

    profile_t src = make_stored_profile();
    stage_legacy_slot(0, &src, 1);

    // Simulate a reboot: wipe the RAM/file view of what a fresh load
    // produces are not wiped (cfg_fs itself is real on-disk state), but the
    // in-RAM catalogue must be, so nvs_load_all_from()'s own decode is what
    // is actually exercised, not stale RAM.
    memset(&s_profiles, 0, sizeof(s_profiles));

    profiles_state_t out;
    bool any_found = false;
    TEST_CHECK(nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found) == ESP_OK, "reload succeeds");
    TEST_CHECK(profiles_slot_bitmap_test(&out.used_bitmap, 0), "slot 0 still reported used after reload");
    assert_profiles_equal(&out.profiles[0], &src, "reloaded slot 0 (file migrated from NVS)");

    profile_t file_p;
    TEST_CHECK(pcfg_file_profile(0, &file_p), "a file now exists for slot 0 (lazy migration wrote it)");
    assert_profiles_equal(&file_p, &src, "the migrated file's content matches what NVS had");
}

static void test_pcfg_file_wins_when_it_has_the_higher_rev(void)
{
    TEST_SECTION("profiles cfg_fs -- DIVERGENCE: file with the higher rev wins over NVS");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);

    profile_t nvs_side = make_stored_profile();
    strncpy(nvs_side.name, "NvsSide", PROFILE_NAME_MAX_LEN);
    profile_t file_side = make_stored_profile();
    strncpy(file_side.name, "FileSide", PROFILE_NAME_MAX_LEN);

    stage_legacy_slot(0, &nvs_side, 1);
    // The file holds different content at a HIGHER rev: a save made after the
    // dual-write close, over a stale legacy NVS copy.
    TEST_CHECK(profiles_cfg_fs_save(0, &file_side, 5) == ESP_OK, "file written at rev 5");

    memset(&s_profiles, 0, sizeof(s_profiles));
    profiles_state_t out;
    bool any_found = false;
    TEST_CHECK(nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found) == ESP_OK, "reload succeeds");
    TEST_CHECK(profiles_slot_bitmap_test(&out.used_bitmap, 0), "slot 0 still used");
    assert_profiles_equal(&out.profiles[0], &file_side, "FILE content wins (higher rev)");
}

static void test_pcfg_adopted_file_retires_legacy_nvs_blob(void)
{
    TEST_SECTION("profiles cfg_fs -- adopted higher-rev file retires the legacy NVS blob; floor and file intact");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    profile_t nvs_side = make_stored_profile();
    strncpy(nvs_side.name, "NvsSide", PROFILE_NAME_MAX_LEN);
    profile_t file_side = make_stored_profile();
    strncpy(file_side.name, "FileSide", PROFILE_NAME_MAX_LEN);
    stage_legacy_slot(0, &nvs_side, 40);
    TEST_CHECK(profiles_cfg_fs_save(0, &file_side, 41) == ESP_OK, "file at rev 41");
    TEST_CHECK(pcfg_nvs_slot_blob_present(0), "legacy blob present before load");

    for (int boot = 0; boot < 2; boot++) {
        memset(&s_profiles, 0, sizeof(s_profiles));
        profiles_state_t out;
        bool any_found = false;
        TEST_CHECK(nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found) == ESP_OK, "load succeeds");
        assert_profiles_equal(&out.profiles[0], &file_side, "file content adopted");
        TEST_CHECK(profiles_slot_bitmap_test(&out.used_bitmap, 0), "slot still used");
        TEST_CHECK(!pcfg_nvs_slot_blob_present(0), "legacy NVS blob retired");
        TEST_CHECK(s_profile_rev[0] == 41, "rev floor is the file rev, unchanged by the erase");
        TEST_CHECK(!s_profile_rev_unknown[0], "floor known");
    }
    nvs_handle_t h;
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h);
    uint32_t back[PROFILES_MAX_COUNT];
    size_t blen = sizeof(back);
    nvs_get_blob(h, NVS_KEY_PROFILE_REV, back, &blen);
    nvs_close(h);
    TEST_CHECK(blen == sizeof(back) && back[0] == 40, "persisted prof_rev array untouched");
    /* Review 12 LOW-5: the persisted used bitmap must have slot 0 cleared (no dangling bit). */
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h);
    profiles_slot_bitmap_t persisted_used;
    memset(&persisted_used, 0xFF, sizeof(persisted_used));
    size_t ulen = sizeof(persisted_used);
    TEST_CHECK(nvs_get_blob(h, NVS_KEY_USED, &persisted_used, &ulen) == ESP_OK, "persisted used bitmap readable");
    nvs_close(h);
    TEST_CHECK(!profiles_slot_bitmap_test(&persisted_used, 0), "retire cleared the persisted used bit for slot 0");
    profile_t fp;
    TEST_CHECK(pcfg_file_profile(0, &fp), "file still present");
}

static void test_pcfg_retire_keeps_blob_when_file_not_adopted(void)
{
    TEST_SECTION("profiles cfg_fs -- cfg unmounted or file not adopted keeps the NVS blob");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    profile_t nvs_side = make_stored_profile();
    strncpy(nvs_side.name, "NvsSide", PROFILE_NAME_MAX_LEN);
    profile_t file_side = make_stored_profile();
    strncpy(file_side.name, "FileSide", PROFILE_NAME_MAX_LEN);
    stage_legacy_slot(0, &nvs_side, 40);
    TEST_CHECK(profiles_cfg_fs_save(0, &file_side, 41) == ESP_OK, "file at rev 41");
    cfg_fs_deinit();
    memset(&s_profiles, 0, sizeof(s_profiles));
    profiles_state_t out;
    bool any_found = false;
    nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found);
    TEST_CHECK(pcfg_nvs_slot_blob_present(0), "cfg unmounted: NVS blob kept");

    pcfg_reset_all();
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    stage_legacy_slot(0, &nvs_side, 41);
    TEST_CHECK(profiles_cfg_fs_save(0, &file_side, 41) == ESP_OK, "file at EQUAL rev 41");
    memset(&s_profiles, 0, sizeof(s_profiles));
    nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found);
    TEST_CHECK(pcfg_nvs_slot_blob_present(0), "equal rev (NVS adopted): NVS blob kept");

    /* Review 12 LOW-5: equal rev AND identical bytes -- the file may be adopted, the blob must still stay. */
    pcfg_reset_all();
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    stage_legacy_slot(0, &file_side, 41);
    TEST_CHECK(profiles_cfg_fs_save(0, &file_side, 41) == ESP_OK, "identical file at EQUAL rev 41");
    memset(&s_profiles, 0, sizeof(s_profiles));
    nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found);
    TEST_CHECK(pcfg_nvs_slot_blob_present(0), "equal rev, identical bytes: NVS blob kept (strict >)");
}

static void test_pcfg_unused_slot_keeps_nvs_rev_floor(void)
{
    TEST_SECTION("profiles cfg_fs -- an unused slot keeps its persisted NVS rev as the next-save floor");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);

    profile_t p0 = make_stored_profile();
    stage_legacy_slot(0, &p0, 1);
    // Slot 2 was deleted in an earlier boot: no blob, no file, but its rev counter was bumped.
    nvs_handle_t h;
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    uint32_t rev_arr[PROFILES_MAX_COUNT] = {0};
    rev_arr[0] = 1;
    rev_arr[2] = 9;
    nvs_set_blob(h, NVS_KEY_PROFILE_REV, rev_arr, sizeof(rev_arr));
    nvs_commit(h);
    nvs_close(h);

    memset(&s_profiles, 0, sizeof(s_profiles));
    memset(s_profile_rev, 0, sizeof(s_profile_rev));
    profiles_state_t out;
    bool any_found = false;
    TEST_CHECK(nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found) == ESP_OK, "reload succeeds");
    TEST_CHECK(!profiles_slot_bitmap_test(&out.used_bitmap, 2), "slot 2 is unused");
    TEST_CHECK(s_profile_rev[2] == 9, "unused slot 2 keeps rev floor 9 (not 0)");
}

static void test_pcfg_boot_load_failure_still_resolves_files(void)
{
    TEST_SECTION("profiles boot load -- F8: an NVS load failure still resolves cfg files and seeds their rev floors");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);

    profile_t p0 = make_stored_profile();
    TEST_CHECK(profiles_cfg_fs_save(0, &p0, 5) == ESP_OK, "file-backed slot 0 at rev 5");
    // Corrupt the used-bitmap key (5-byte blob: neither the 16-byte shape nor the legacy u8) so the NVS load errors.
    nvs_handle_t h;
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    uint8_t junk[5] = {1, 2, 3, 4, 5};
    nvs_set_blob(h, NVS_KEY_USED, junk, sizeof(junk));
    nvs_commit(h);
    nvs_close(h);

    memset(&s_profiles, 0, sizeof(s_profiles));
    memset(s_profile_rev, 0, sizeof(s_profile_rev));
    TEST_CHECK(profiles_boot_load() != ESP_OK, "the NVS load reports its failure");
    TEST_CHECK(profiles_slot_bitmap_test(&s_profiles.used_bitmap, 0), "file-backed slot 0 is NOT free after the NVS failure");
    TEST_CHECK(s_profile_rev[0] == 5, "slot 0's rev floor is seeded from its file (5), not 0");
    profile_t file_p;
    TEST_CHECK(pcfg_file_profile(0, &file_p), "slot 0's file is untouched");
}

static void pcfg_corrupt_used_bitmap_and_set_revs(const uint32_t *revs, bool short_rev_blob)
{
    nvs_handle_t h;
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    uint8_t junk[5] = {1, 2, 3, 4, 5};
    nvs_set_blob(h, NVS_KEY_USED, junk, sizeof(junk));
    if (short_rev_blob) {
        nvs_set_blob(h, NVS_KEY_PROFILE_REV, junk, sizeof(junk));
    } else {
        nvs_set_blob(h, NVS_KEY_PROFILE_REV, revs, sizeof(uint32_t) * PROFILES_MAX_COUNT);
    }
    nvs_commit(h);
    nvs_close(h);
}

static void test_pcfg_files_only_seeds_floor_from_persisted_revs(void)
{
    TEST_SECTION("profiles boot load -- F8b: NVS load failure with a readable rev array keeps a deleted slot's floor");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    uint32_t revs[PROFILES_MAX_COUNT];
    memset(revs, 0, sizeof(revs));
    revs[3] = 7; /* slot 3 was deleted earlier: no file, persisted rev 7 */
    pcfg_corrupt_used_bitmap_and_set_revs(revs, false);
    memset(&s_profiles, 0, sizeof(s_profiles));
    memset(s_profile_rev, 0, sizeof(s_profile_rev));
    TEST_CHECK(profiles_boot_load() != ESP_OK, "the NVS load reports its failure");
    TEST_CHECK(s_profile_rev[3] == 7, "deleted slot 3 keeps its rev floor 7 (not 0)");
    TEST_CHECK(!s_profile_rev_unknown[3], "slot 3's floor is known");
    s_profiles.profiles[3] = make_stored_profile();
    profiles_slot_bitmap_set(&s_profiles.used_bitmap, 3);
    TEST_CHECK(nvs_save_slot(3) == ESP_OK, "save into slot 3 succeeds");
    uint32_t file_rev = 0;
    bool valid = false;
    profile_t fp;
    profiles_cfg_fs_load_raw(3, &fp, &file_rev, &valid);
    TEST_CHECK(valid && file_rev == 8, "the file is written at rev 8 (floor + 1), never rev 1");
}

static void test_pcfg_files_only_unknown_floor_refuses_save(void)
{
    TEST_SECTION("profiles boot load -- F8b: NVS load failure with an unreadable rev array refuses saves to file-less slots");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    profile_t p0 = make_stored_profile();
    TEST_CHECK(profiles_cfg_fs_save(0, &p0, 5) == ESP_OK, "file-backed slot 0 at rev 5");
    pcfg_corrupt_used_bitmap_and_set_revs(NULL, true);
    memset(&s_profiles, 0, sizeof(s_profiles));
    memset(s_profile_rev, 0, sizeof(s_profile_rev));
    fake_kv_script_next_write_status(HAL_IO); /* the one-shot prof_rev repair write fails -> floors stay unknown */
    TEST_CHECK(profiles_boot_load() != ESP_OK, "the NVS load reports its failure");
    TEST_CHECK(s_profile_rev_unknown[3], "file-less slot 3 has an unknown floor");
    TEST_CHECK(s_profile_rev_unknown[0], "file-backed slot 0 is flagged too (floors unknown)");
    s_profiles.profiles[3] = make_stored_profile();
    profiles_slot_bitmap_set(&s_profiles.used_bitmap, 3);
    TEST_CHECK(nvs_save_slot(3) != ESP_OK, "save into the unknown-floor slot is refused");
    uint32_t file_rev = 0;
    bool valid = true;
    profile_t fp;
    profiles_cfg_fs_load_raw(3, &fp, &file_rev, &valid);
    TEST_CHECK(!valid, "no file was written for slot 3");
    TEST_CHECK(nvs_save_slot(0) != ESP_OK, "slot 0 save is refused too (floors unknown)");
}

static void test_pcfg_files_only_keeps_files_when_rev_array_equals_file_rev(void)
{
    TEST_SECTION("profiles boot load -- corrupt bitmap + rev array == file rev must KEEP and adopt the file, next save rev 6");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    profile_t p0 = make_stored_profile();
    TEST_CHECK(profiles_cfg_fs_save(0, &p0, 5) == ESP_OK, "file at rev 5");
    uint32_t revs[PROFILES_MAX_COUNT];
    memset(revs, 0, sizeof(revs));
    revs[0] = 5;
    pcfg_corrupt_used_bitmap_and_set_revs(revs, false);
    memset(&s_profiles, 0, sizeof(s_profiles));
    memset(s_profile_rev, 0, sizeof(s_profile_rev));
    (void)profiles_boot_load();
    profile_t fp;
    TEST_CHECK(pcfg_file_profile(0, &fp), "the file was NOT deleted");
    TEST_CHECK(profiles_slot_bitmap_test(&s_profiles.used_bitmap, 0), "slot 0 adopted");
    TEST_CHECK(s_profile_rev[0] == 5, "rev seeded at 5");
    TEST_CHECK(nvs_save_slot(0) == ESP_OK, "save slot 0");
    uint32_t file_rev = 0;
    bool valid = false;
    profiles_cfg_fs_load_raw(0, &fp, &file_rev, &valid);
    TEST_CHECK(valid && file_rev == 6, "next save is rev 6");
}

static void test_pcfg_full_load_short_rev_blob_marks_fileless_slots_unknown(void)
{
    TEST_SECTION("nvs_load_all_from -- short rev blob (valid bitmap) marks file-less slots rev-unknown");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    profile_t p0 = make_stored_profile();
    TEST_CHECK(profiles_cfg_fs_save(0, &p0, 5) == ESP_OK, "file at rev 5");
    nvs_handle_t h;
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    uint8_t junk[5] = {1, 2, 3, 4, 5};
    nvs_set_blob(h, NVS_KEY_PROFILE_REV, junk, sizeof(junk));
    nvs_commit(h);
    nvs_close(h);
    fake_kv_script_next_write_status(HAL_IO); /* the one-shot prof_rev repair write fails -> floors stay unknown */
    memset(&s_profiles, 0, sizeof(s_profiles));
    profiles_state_t out;
    bool any_found = false;
    TEST_CHECK(nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found) == ESP_OK, "load succeeds");
    TEST_CHECK(s_profile_rev_unknown[3], "file-less slot 3 is rev-unknown");
    TEST_CHECK(s_profile_rev_unknown[0], "file-backed slot 0 is flagged too");
    memset(s_profile_rev_unknown, 0, sizeof(s_profile_rev_unknown));
}

static void test_pcfg_legacy_32_byte_rev_array_is_known(void)
{
    TEST_SECTION("nvs_load_all_from -- legacy 32-byte rev array is KNOWN: save and delete work");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    profile_t p0 = make_stored_profile();
    TEST_CHECK(profiles_cfg_fs_save(0, &p0, 5) == ESP_OK, "file at rev 5");
    nvs_handle_t h;
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    uint32_t legacy[8] = {5, 0, 0, 0, 0, 0, 0, 0};
    nvs_set_blob(h, NVS_KEY_PROFILE_REV, legacy, sizeof(legacy));
    nvs_commit(h);
    nvs_close(h);
    memset(&s_profiles, 0, sizeof(s_profiles));
    profiles_state_t out;
    bool any_found = false;
    TEST_CHECK(nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found) == ESP_OK, "load succeeds");
    TEST_CHECK(!s_profile_rev_unknown[3] && !s_profile_rev_unknown[0], "no slot is rev-unknown");
    s_profiles.profiles[3] = make_stored_profile();
    profiles_slot_bitmap_set(&s_profiles.used_bitmap, 3);
    TEST_CHECK(nvs_save_slot(3) == ESP_OK, "save into file-less slot 3 works");
    TEST_CHECK(nvs_erase_slot(3) == ESP_OK, "delete of slot 3 works with the legacy-width array");
}

static void test_pcfg_non_multiple_of_4_rev_blob_stays_unknown(void)
{
    TEST_SECTION("nvs_load_all_from -- 30-byte rev blob stays unknown");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    nvs_handle_t h;
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    uint8_t odd[30] = {0};
    nvs_set_blob(h, NVS_KEY_PROFILE_REV, odd, sizeof(odd));
    nvs_commit(h);
    nvs_close(h);
    fake_kv_script_next_write_status(HAL_IO); /* the one-shot prof_rev repair write fails -> floors stay unknown */
    memset(&s_profiles, 0, sizeof(s_profiles));
    profiles_state_t out;
    bool any_found = false;
    TEST_CHECK(nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found) == ESP_OK, "load succeeds");
    TEST_CHECK(s_profile_rev_unknown[3], "slot 3 is rev-unknown");
    memset(s_profile_rev_unknown, 0, sizeof(s_profile_rev_unknown));
}

static void test_pcfg_unknown_floors_flag_file_backed_slots_too(void)
{
    TEST_SECTION("nvs_load_all_from -- unknown floors flag EVERY slot, file-backed included");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    profile_t p0 = make_stored_profile();
    TEST_CHECK(profiles_cfg_fs_save(0, &p0, 5) == ESP_OK, "file at rev 5");
    nvs_handle_t h;
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    uint8_t junk[5] = {1, 2, 3, 4, 5};
    nvs_set_blob(h, NVS_KEY_PROFILE_REV, junk, sizeof(junk));
    nvs_commit(h);
    nvs_close(h);
    fake_kv_script_next_write_status(HAL_IO); /* the one-shot prof_rev repair write fails -> floors stay unknown */
    memset(&s_profiles, 0, sizeof(s_profiles));
    profiles_state_t out;
    bool any_found = false;
    TEST_CHECK(nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found) == ESP_OK, "load succeeds");
    TEST_CHECK(s_profile_rev_unknown[0], "file-backed slot 0 is rev-unknown too");
    TEST_CHECK(s_profile_rev_unknown[3], "file-less slot 3 is rev-unknown");
    memset(s_profile_rev_unknown, 0, sizeof(s_profile_rev_unknown));
}

static void test_pcfg_corrupt_nvs_blob_keeps_live_file(void)
{
    TEST_SECTION("nvs_load_all_from -- corrupt profN blob, file rev == nvs rev: file survives and loads");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    profile_t p0 = make_stored_profile();
    TEST_CHECK(profiles_cfg_fs_save(0, &p0, 5) == ESP_OK, "file at rev 5");
    stage_legacy_slot(0, &p0, 5);
    uint8_t junk[7] = {9, 9, 9, 9, 9, 9, 9};
    stage_profile_blob(0, junk, sizeof(junk));
    memset(&s_profiles, 0, sizeof(s_profiles));
    profiles_state_t out;
    bool any_found = false;
    TEST_CHECK(nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found) == ESP_OK, "load succeeds");
    profile_t fp;
    TEST_CHECK(pcfg_file_profile(0, &fp), "the live file was NOT deleted");
    TEST_CHECK(profiles_slot_bitmap_test(&out.used_bitmap, 0), "slot 0 loads from its file");
    TEST_CHECK(s_profile_rev[0] >= 5, "rev floor kept");
}

static void pcfg_set_rev_blob(const void *buf, size_t len)
{
    nvs_handle_t h;
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    nvs_set_blob(h, NVS_KEY_PROFILE_REV, buf, len);
    nvs_commit(h);
    nvs_close(h);
}

static void test_pcfg_longer_rev_array_is_known_tail_ignored(void)
{
    TEST_SECTION("nvs_load_all_from -- rev array longer than this build's (newer firmware) is KNOWN, tail ignored");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    profile_t p0 = make_stored_profile();
    TEST_CHECK(profiles_cfg_fs_save(0, &p0, 5) == ESP_OK, "file at rev 5");
    uint32_t big[PROFILES_MAX_COUNT + 8];
    memset(big, 0, sizeof(big));
    big[0] = 5;
    big[3] = 9;
    big[PROFILES_MAX_COUNT + 2] = 77;
    pcfg_set_rev_blob(big, sizeof(big));
    memset(&s_profiles, 0, sizeof(s_profiles));
    profiles_state_t out;
    bool any_found = false;
    TEST_CHECK(nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found) == ESP_OK, "load succeeds");
    TEST_CHECK(!s_profile_rev_unknown[0] && !s_profile_rev_unknown[3], "no slot is rev-unknown");
    TEST_CHECK(s_profile_rev[3] == 9, "slot 3 floor read from the first PROFILES_MAX_COUNT entries");
    s_profiles.profiles[3] = make_stored_profile();
    profiles_slot_bitmap_set(&s_profiles.used_bitmap, 3);
    TEST_CHECK(nvs_save_slot(3) == ESP_OK, "save into slot 3 works");
    TEST_CHECK(nvs_erase_slot(3) == ESP_OK, "delete of slot 3 works");
    {
        nvs_handle_t rh;
        nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &rh);
        uint32_t back[PROFILES_MAX_COUNT + 8];
        memset(back, 0xEE, sizeof(back));
        size_t bl = sizeof(back);
        TEST_CHECK(nvs_get_blob(rh, NVS_KEY_PROFILE_REV, back, &bl) == ESP_OK && bl == sizeof(big),
                   "rev array keeps its full (longer) length after delete");
        nvs_close(rh);
        TEST_CHECK(memcmp(&back[PROFILES_MAX_COUNT], &big[PROFILES_MAX_COUNT], 8 * sizeof(uint32_t)) == 0,
                   "tail floors from newer firmware preserved verbatim after delete");
        TEST_CHECK(back[3] > 9, "slot 3 floor bumped");
    }
}

static void test_pcfg_truncated_rev_blob_not_known_lengths(void)
{
    TEST_SECTION("rev blob lengths 36 and 200 (never written) are NOT known as-is; repair rebuilds them");
    size_t lens[2] = {36, 200};
    for (int k = 0; k < 2; k++) {
        pcfg_reset_all();
        size_t reaped = 0;
        cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
        profile_t p0 = make_stored_profile();
        TEST_CHECK(profiles_cfg_fs_save(0, &p0, 5) == ESP_OK, "file at rev 5");
        uint32_t buf[100];
        memset(buf, 0, sizeof(buf));
        pcfg_set_rev_blob(buf, lens[k]);
        fake_kv_script_next_write_status(HAL_IO);
        memset(&s_profiles, 0, sizeof(s_profiles));
        profiles_state_t out;
        bool any_found = false;
        TEST_CHECK(nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found) == ESP_OK, "load succeeds");
        TEST_CHECK(s_profile_rev_unknown[3], "with the repair write failing the slots stay unknown (not floor 0)");
        memset(s_profile_rev_unknown, 0, sizeof(s_profile_rev_unknown));
    }
}

static void test_pcfg_junk_rev_blob_is_repaired_once(void)
{
    TEST_SECTION("nvs_load_all_from -- junk rev blob: floors rebuilt from file revs, prof_rev rewritten, saves work");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    profile_t p0 = make_stored_profile();
    TEST_CHECK(profiles_cfg_fs_save(0, &p0, 5) == ESP_OK, "file at rev 5");
    uint8_t junk[5] = {1, 2, 3, 4, 5};
    pcfg_set_rev_blob(junk, sizeof(junk));
    memset(&s_profiles, 0, sizeof(s_profiles));
    profiles_state_t out;
    bool any_found = false;
    TEST_CHECK(nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found) == ESP_OK, "load succeeds");
    TEST_CHECK(!s_profile_rev_unknown[0] && !s_profile_rev_unknown[3], "repair verified: nothing is unknown");
    TEST_CHECK(s_profile_rev[0] == 5, "file-backed slot 0 keeps its file rev");
    TEST_CHECK(s_profile_rev[3] == 5, "file-less slot 3 is raised to the max observed rev");
    nvs_handle_t h;
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h);
    uint32_t back[PROFILES_MAX_COUNT];
    size_t blen = sizeof(back);
    nvs_get_blob(h, NVS_KEY_PROFILE_REV, back, &blen);
    nvs_close(h);
    TEST_CHECK(blen == sizeof(back) && back[3] == 5 && back[0] == 5, "prof_rev rewritten full-width");
    s_profiles.profiles[3] = make_stored_profile();
    profiles_slot_bitmap_set(&s_profiles.used_bitmap, 3);
    TEST_CHECK(nvs_save_slot(3) == ESP_OK, "save into file-less slot 3 now works");
    uint32_t file_rev = 0;
    bool valid = false;
    profile_t fp;
    profiles_cfg_fs_load_raw(3, &fp, &file_rev, &valid);
    TEST_CHECK(valid && file_rev == 6, "slot 3 file written at max+1, above any stale value");
    TEST_CHECK(nvs_erase_slot(3) == ESP_OK, "delete works (also exercises firing_stats_erase after the change)");
}

static void test_pcfg_junk_rev_repair_raises_fileless_to_max(void)
{
    TEST_SECTION("junk rev blob, cfg mounted, files at revs 5 and 9: file-less slots raised to max (9), file slots keep own rev");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    profile_t p0 = make_stored_profile();
    TEST_CHECK(profiles_cfg_fs_save(0, &p0, 5) == ESP_OK, "file at rev 5 (slot 0)");
    TEST_CHECK(profiles_cfg_fs_save(1, &p0, 9) == ESP_OK, "file at rev 9 (slot 1)");
    uint8_t junk[5] = {1, 2, 3, 4, 5};
    pcfg_set_rev_blob(junk, sizeof(junk));
    memset(&s_profiles, 0, sizeof(s_profiles));
    profiles_state_t out;
    bool any_found = false;
    TEST_CHECK(nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found) == ESP_OK, "load succeeds");
    TEST_CHECK(s_profile_rev[0] == 5, "slot 0 (file) keeps its own rev 5");
    TEST_CHECK(s_profile_rev[1] == 9, "slot 1 (file) keeps its own rev 9");
    TEST_CHECK(s_profile_rev[3] == 9, "file-less slot 3 raised to max observed rev 9");
    TEST_CHECK(s_profile_rev[4] == 9, "file-less slot 4 raised to max observed rev 9");
    TEST_CHECK(!s_profile_rev_unknown[3] && !s_profile_rev_unknown[4], "repair verified");
    nvs_handle_t h;
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h);
    uint32_t back[PROFILES_MAX_COUNT];
    size_t blen = sizeof(back);
    nvs_get_blob(h, NVS_KEY_PROFILE_REV, back, &blen);
    nvs_close(h);
    TEST_CHECK(blen == sizeof(back) && back[0] == 5 && back[1] == 9 && back[3] == 9, "persisted floors match");
}

/* OOM injection for persist_scratch_alloc() (KILNCTL_PERSIST_SCRATCH_TEST_HOOK, review 5 L4). */
size_t persist_scratch_test_fail_size = 0;
int persist_scratch_test_fail_nth = 0;
int persist_scratch_test_seen = 0;

static void test_pcfg_resolve_scratch_oom_leaves_file_untouched(void)
{
    TEST_SECTION("resolve: load scratch allocation failure never overwrites the file or frees the slot (review 7 L3)");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    profile_t file_p = make_stored_profile();
    strncpy(file_p.name, "CurrentFile", PROFILE_NAME_MAX_LEN);
    profile_t stale_nvs = make_stored_profile();
    strncpy(stale_nvs.name, "StaleNvs", PROFILE_NAME_MAX_LEN);
    TEST_CHECK(profiles_cfg_fs_save(2, &file_p, 7) == ESP_OK, "file at rev 7");

    /* same layout as load_raw's local scratch struct, so the sizes match */
    struct { uint8_t raw[4 + PROFILE_BLOB_MAX_SIZE]; profile_t cand; } probe;
    persist_scratch_test_fail_size = sizeof(probe);
    persist_scratch_test_fail_nth = 1;
    persist_scratch_test_seen = 0;
    profile_t out;
    uint32_t out_rev = 99;
    bool used_file = true, err = false;
    bool have = profiles_cfg_fs_resolve_ex(2, &stale_nvs, true, 3, &out, &out_rev, &used_file, &err);
    persist_scratch_test_fail_nth = 0;
    TEST_CHECK(persist_scratch_test_seen == 1, "the load scratch allocation was reached");
    TEST_CHECK(err, "allocation failure reported as an error");
    TEST_CHECK(!have && !used_file && out_rev == 0, "slot is not adopted");
    profile_t after;
    TEST_CHECK(pcfg_file_profile(2, &after), "file still present");
    TEST_CHECK(strcmp(after.name, "CurrentFile") == 0, "file NOT overwritten with the stale NVS copy");

    /* nvs_valid == false: must be an error, not 'genuinely unused' */
    persist_scratch_test_fail_nth = 1;
    persist_scratch_test_seen = 0;
    err = false;
    have = profiles_cfg_fs_resolve_ex(2, &stale_nvs, false, 3, &out, &out_rev, &used_file, &err);
    persist_scratch_test_fail_nth = 0;
    TEST_CHECK(err && !have, "nvs-absent case also reports error");
    TEST_CHECK(pcfg_file_profile(2, &after) && strcmp(after.name, "CurrentFile") == 0, "file still intact (no stale delete)");

    /* boot loader: error propagates and the slot is refused for saves, not free */
    persist_scratch_test_fail_nth = 1;
    persist_scratch_test_seen = 0;
    profiles_state_t st;
    bool any_found = false;
    esp_err_t lerr = nvs_load_all_from(PROFILES_NVS_PARTITION, &st, &any_found);
    persist_scratch_test_fail_nth = 0;
    TEST_CHECK(lerr == ESP_ERR_NO_MEM, "boot load reports the failure");
    TEST_CHECK(s_profile_rev_unknown[0], "the unexamined slot is marked unknown (saves/deletes refused)");
    TEST_CHECK(pcfg_file_profile(2, &after) && strcmp(after.name, "CurrentFile") == 0, "file intact after boot load");
}

static void test_pcfg_boot_profile_scratch_oom_fails_closed(void)
{
    TEST_SECTION("boot load: sizeof(profile_t) / 2*sizeof(profile_t) scratch OOM is an error and marks slots unknown (review M2, 794fce57)");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    profile_t file_p = make_stored_profile();
    strncpy(file_p.name, "CurrentFile", PROFILE_NAME_MAX_LEN);
    TEST_CHECK(profiles_cfg_fs_save(2, &file_p, 7) == ESP_OK, "file at rev 7");

    /* nvs_load_all_from: resolve scratch (one profile_t); a rev blob creates the kiln_cfg namespace so the
     * function does not delegate to the files-only path */
    uint32_t revs0[PROFILES_MAX_COUNT] = {0};
    pcfg_set_rev_blob(revs0, sizeof(revs0));
    memset(s_profile_rev_unknown, 0, sizeof(s_profile_rev_unknown));
    persist_scratch_test_fail_size = sizeof(profile_t);
    persist_scratch_test_fail_nth = 1;
    persist_scratch_test_seen = 0;
    profiles_state_t st;
    bool any_found = false;
    esp_err_t lerr = nvs_load_all_from(PROFILES_NVS_PARTITION, &st, &any_found);
    persist_scratch_test_fail_nth = 0;
    TEST_CHECK(persist_scratch_test_seen == 1, "the sizeof(profile_t) allocation was reached");
    TEST_CHECK(lerr == ESP_ERR_NO_MEM, "OOM reported as an error, never absent/OK");
    bool all_unknown = true;
    for (uint8_t id = 0; id < PROFILES_MAX_COUNT; id++) {
        all_unknown = all_unknown && s_profile_rev_unknown[id];
    }
    TEST_CHECK(all_unknown, "every slot marked rev-unknown (resolve-error flags set)");
    profile_t after;
    TEST_CHECK(pcfg_file_profile(2, &after) && strcmp(after.name, "CurrentFile") == 0, "file intact");

    /* nvs_load_files_only: two-profile scratch */
    memset(s_profile_rev_unknown, 0, sizeof(s_profile_rev_unknown));
    persist_scratch_test_fail_size = 2 * sizeof(profile_t);
    persist_scratch_test_fail_nth = 1;
    persist_scratch_test_seen = 0;
    memset(&st, 0, sizeof(st));
    any_found = false;
    lerr = nvs_load_files_only(PROFILES_NVS_PARTITION, &st, &any_found);
    persist_scratch_test_fail_nth = 0;
    TEST_CHECK(persist_scratch_test_seen == 1, "the 2*sizeof(profile_t) allocation was reached");
    TEST_CHECK(lerr == ESP_ERR_NO_MEM, "files-only OOM reported as an error");
    all_unknown = true;
    for (uint8_t id = 0; id < PROFILES_MAX_COUNT; id++) {
        all_unknown = all_unknown && s_profile_rev_unknown[id];
    }
    TEST_CHECK(all_unknown, "every slot marked rev-unknown, none absent");
    TEST_CHECK(!any_found, "nothing reported found");
    TEST_CHECK(pcfg_file_profile(2, &after) && strcmp(after.name, "CurrentFile") == 0, "file intact");
}

static void test_pcfg_junk_rev_repair_scratch_oom_fails_closed(void)
{
    TEST_SECTION("junk rev repair: scratch allocation failure fails closed (review 5 L4)");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    profile_t p0 = make_stored_profile();
    TEST_CHECK(profiles_cfg_fs_save(1, &p0, 9) == ESP_OK, "file at rev 9 (slot 1)");
    uint8_t junk[5] = {1, 2, 3, 4, 5};
    pcfg_set_rev_blob(junk, sizeof(junk));
    memset(&s_profiles, 0, sizeof(s_profiles));
    persist_scratch_test_fail_size = sizeof(struct rev_repair_scratch);
    persist_scratch_test_fail_nth = 1;
    persist_scratch_test_seen = 0;
    profiles_state_t out;
    bool any_found = false;
    nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found);
    persist_scratch_test_fail_nth = 0;
    TEST_CHECK(persist_scratch_test_seen == 1, "the repair scratch allocation was reached");
    TEST_CHECK(s_profile_rev_unknown[3], "slot 3 stays rev-unknown (fail closed)");
    nvs_handle_t h;
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h);
    uint32_t back[PROFILES_MAX_COUNT];
    size_t blen = sizeof(back);
    nvs_get_blob(h, NVS_KEY_PROFILE_REV, back, &blen);
    nvs_close(h);
    TEST_CHECK(blen == sizeof(junk), "rev blob not rewritten");
}

static void test_pcfg_junk_rev_repair_refuses_on_external_ram_stack(void)
{
    TEST_SECTION("junk rev repair: PSRAM-stack caller is refused, fails closed, prof_rev untouched "
                 "(HOST_TEST_GAP_AUDIT gap 7, commit 7d155f5a)");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    profile_t p0 = make_stored_profile();
    TEST_CHECK(profiles_cfg_fs_save(1, &p0, 9) == ESP_OK, "file at rev 9 (slot 1)");
    uint8_t junk[5] = {1, 2, 3, 4, 5};
    pcfg_set_rev_blob(junk, sizeof(junk));
    memset(&s_profiles, 0, sizeof(s_profiles));
    memset(s_profile_rev, 0, sizeof(s_profile_rev));
    fake_kv_set_write_safe_here(false);
    profiles_state_t out;
    bool any_found = false;
    (void)nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found);
    fake_kv_set_write_safe_here(true);
    TEST_CHECK(s_profile_rev_unknown[3] && s_profile_rev_unknown[1], "slots stay rev-unknown (fail closed)");
    TEST_CHECK(s_profile_rev[3] == 0, "no floor raise when the repair is refused");
    nvs_handle_t h;
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h);
    uint32_t back[PROFILES_MAX_COUNT];
    size_t blen = sizeof(back);
    nvs_get_blob(h, NVS_KEY_PROFILE_REV, back, &blen);
    nvs_close(h);
    TEST_CHECK(blen == sizeof(junk), "rev blob not rewritten");
}

static void test_pcfg_boot_fallback_keeps_rev_unknown_marks(void)
{
    TEST_SECTION("boot fallback after OOM keeps rev-unknown marks of the failed pass (review 8 L3)");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    profile_t src = make_stored_profile();
    stage_legacy_slot(0, &src, 1); /* legacy NVS-only slot, no file */
    memset(&s_profiles, 0, sizeof(s_profiles));
    struct { uint8_t raw[4 + PROFILE_BLOB_MAX_SIZE]; profile_t cand; } probe;
    persist_scratch_test_fail_size = sizeof(probe);
    persist_scratch_test_fail_nth = 1;
    persist_scratch_test_seen = 0;
    esp_err_t e = profiles_boot_load();
    persist_scratch_test_fail_nth = 0;
    TEST_CHECK(persist_scratch_test_seen >= 1, "the load scratch allocation was reached");
    TEST_CHECK(e == ESP_ERR_NO_MEM, "first pass failed with NO_MEM and the files-only fallback ran");
    TEST_CHECK(s_profile_rev_unknown[0], "slot 0 stays rev-unknown after the fallback (save/erase refused)");
}

static void test_pcfg_junk_repair_load_error_fails_closed(void)
{
    TEST_SECTION("junk rev repair: cfg read error fails closed, no floor raise (review 8 L4)");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    profile_t p0 = make_stored_profile();
    TEST_CHECK(profiles_cfg_fs_save(1, &p0, 9) == ESP_OK, "file at rev 9 (slot 1)");
    uint8_t junk[5] = {1, 2, 3, 4, 5};
    pcfg_set_rev_blob(junk, sizeof(junk));
    memset(&s_profiles, 0, sizeof(s_profiles));
    memset(s_profile_rev, 0, sizeof(s_profile_rev));
    struct { uint8_t raw[4 + PROFILE_BLOB_MAX_SIZE]; profile_t cand; } probe;
    persist_scratch_test_fail_size = sizeof(probe);
    persist_scratch_test_fail_nth = PROFILES_MAX_COUNT + 1; /* the repair's first load, after the resolve pass */
    persist_scratch_test_seen = 0;
    profiles_state_t out;
    bool any_found = false;
    (void)nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found);
    persist_scratch_test_fail_nth = 0;
    TEST_CHECK(persist_scratch_test_seen > PROFILES_MAX_COUNT, "the repair load allocation was reached");
    TEST_CHECK(s_profile_rev_unknown[3] && s_profile_rev_unknown[1], "slots stay rev-unknown");
    TEST_CHECK(s_profile_rev[3] == 0, "no floor raise on a failed load");
}

static void test_pcfg_junk_rev_repair_deferred_without_cfg(void)
{
    TEST_SECTION("junk rev blob with cfg NOT mounted: repair deferred, stays fail-closed, prof_rev untouched");
    pcfg_reset_all();
    cfg_fs_deinit();
    TEST_CHECK(!cfg_fs_is_available(), "cfg not mounted");
    uint8_t junk[5] = {1, 2, 3, 4, 5};
    pcfg_set_rev_blob(junk, sizeof(junk));
    memset(&s_profiles, 0, sizeof(s_profiles));
    profiles_state_t out;
    bool any_found = false;
    nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found);
    TEST_CHECK(s_profile_rev_unknown[3], "slot 3 still rev-unknown (fail closed)");
    nvs_handle_t h;
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h);
    uint32_t back[PROFILES_MAX_COUNT];
    size_t blen = sizeof(back);
    nvs_get_blob(h, NVS_KEY_PROFILE_REV, back, &blen);
    nvs_close(h);
    TEST_CHECK(blen == sizeof(junk), "rev blob not rewritten with zeros");
}

static void test_pcfg_files_only_junk_rev_is_repaired(void)
{
    TEST_SECTION("files-only path (bitmap corrupt) -- junk rev blob repaired too");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    profile_t p0 = make_stored_profile();
    TEST_CHECK(profiles_cfg_fs_save(0, &p0, 5) == ESP_OK, "file at rev 5");
    pcfg_corrupt_used_bitmap_and_set_revs(NULL, true);
    memset(&s_profiles, 0, sizeof(s_profiles));
    memset(s_profile_rev, 0, sizeof(s_profile_rev));
    TEST_CHECK(profiles_boot_load() != ESP_OK, "the NVS load reports its failure");
    TEST_CHECK(!s_profile_rev_unknown[3] && !s_profile_rev_unknown[0], "repaired: not unknown");
    TEST_CHECK(s_profile_rev[3] == 5, "slot 3 floor = max observed file rev");
}

/* Save mutex (coordinator 2026-10-09): the rev read, file write and rev bump
 * happen under profiles_save_lock(). The host semaphore stub is
 * single-threaded, so this proves lock ownership at the write seam
 * (g_test_stub_lock_depth > 0); it does not exercise a real race. */
static int s_sm_depth_at_write[16];
static unsigned s_sm_writes;
static esp_err_t sm_write_fn(const char *rel_path, const void *data, size_t len)
{
    if (s_sm_writes < 16) {
        s_sm_depth_at_write[s_sm_writes] = g_test_stub_lock_depth;
    }
    s_sm_writes++;
    return cfg_fs_write_atomic(rel_path, data, len);
}

static void test_save_mutex_serializes_rev_write_bump(void)
{
    TEST_SECTION("nvs_save_slot -- the cfg file write runs with the save mutex held");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    s_profiles.profiles[3] = make_stored_profile();
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0x08);
    s_sm_writes = 0;
    memset(s_sm_depth_at_write, 0, sizeof(s_sm_depth_at_write));
    profiles_cfg_fs_set_write_fn(sm_write_fn);
    for (int i = 0; i < 3; i++) {
        TEST_CHECK(nvs_save_slot(3) == ESP_OK, "save slot 3");
    }
    TEST_CHECK(s_sm_writes == 3, "three file writes");
    TEST_CHECK(s_sm_depth_at_write[0] > 0 && s_sm_depth_at_write[1] > 0 && s_sm_depth_at_write[2] > 0,
               "the file write ran with the save mutex held");
    TEST_CHECK(g_test_stub_lock_depth == 0, "mutex released after the saves");
    profiles_cfg_fs_reset_write_fn_for_test();
}
/* "Save mutex vs. flash worker" (docs/audits/CFG_STORE_SAVE_RACE_2026-10-09.md):
 * profiles_save_lock() reserves the flash worker BEFORE taking the mutex and
 * releases it AFTER giving it, and the cfg write / delete happen inside the
 * reservation. This is the deadlock on main and in v1.0.0-pre.2: an httpd save
 * holding the mutex waited for the worker while the worker ran a PROFILES
 * SAVE/DELETE job blocked on the mutex. */
static unsigned s_ps_writes_outside;
static esp_err_t ps_write_fn(const char *rel_path, const void *data, size_t len)
{
    g_ssp.writes++;
    if (g_ssp.depth <= 0) {
        s_ps_writes_outside++;
    }
    return cfg_fs_write_atomic(rel_path, data, len);
}
static esp_err_t ps_delete_fn(const char *rel_path)
{
    g_ssp.writes++;
    if (g_ssp.depth <= 0) {
        s_ps_writes_outside++;
    }
    return cfg_fs_delete(rel_path);
}
static void test_profiles_save_reserves_flash_worker(void)
{
    TEST_SECTION("profiles save/delete -- worker reserved before the save mutex, released after; file I/O inside");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    s_profiles.profiles[5] = make_stored_profile();
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0x20);
    ssp_install();
    s_ps_writes_outside = 0;
    profiles_cfg_fs_set_write_fn(ps_write_fn);
    profiles_cfg_fs_set_delete_fn(ps_delete_fn);
    TEST_CHECK(nvs_save_slot(5) == ESP_OK, "save slot 5");
    TEST_CHECK(ssp_shape_ok(1) && s_ps_writes_outside == 0, "save: reservation wraps the mutex and the file write");
    int enters_after_save = g_ssp.enters;
    TEST_CHECK(profiles_http_delete(5), "delete slot 5");
    TEST_CHECK(g_ssp.enters > enters_after_save, "delete opened a section too");
    TEST_CHECK(ssp_shape_ok(2) && s_ps_writes_outside == 0, "delete: reservation wraps the mutex and the file delete");
    TEST_CHECK(g_test_stub_lock_depth == 0, "everything released");
    profiles_cfg_fs_reset_write_fn_for_test();
    profiles_cfg_fs_reset_delete_fn_for_test();
    ssp_uninstall();
}

/* Delete and retarget seams (Opus review of 0880162b): the erase / rewrite must
 * run with the save mutex held. */
static int s_dl_depth_at_delete;
static unsigned s_dl_deletes;
static esp_err_t dl_delete_fn(const char *rel_path)
{
    s_dl_depth_at_delete = g_test_stub_lock_depth;
    s_dl_deletes++;
    return cfg_fs_delete(rel_path);
}

static void test_delete_paths_hold_save_lock_at_erase_seam(void)
{
    TEST_SECTION("profiles_http_delete / nvs_erase_slot_locked -- the cfg file delete runs with the save mutex held");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    s_profiles.profiles[8] = make_stored_profile();
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0x100);
    TEST_CHECK(nvs_save_slot(8) == ESP_OK, "save slot 8");
    s_dl_deletes = 0;
    s_dl_depth_at_delete = 0;
    profiles_cfg_fs_set_delete_fn(dl_delete_fn);
    TEST_CHECK(profiles_http_delete(8), "delete slot 8");
    TEST_CHECK(s_dl_deletes >= 1 && s_dl_depth_at_delete > 0, "profiles_http_delete: file delete under the lock");
    TEST_CHECK(!profiles_slot_used(8), "slot cleared");
    TEST_CHECK(g_test_stub_lock_depth == 0, "lock released");

    s_profiles.profiles[9] = make_stored_profile();
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0x200);
    TEST_CHECK(nvs_save_slot(9) == ESP_OK, "save slot 9");
    s_dl_deletes = 0;
    s_dl_depth_at_delete = 0;
    profiles_save_lock();
    TEST_CHECK(nvs_erase_slot_locked(9) == ESP_OK, "erase_locked slot 9");
    profiles_save_unlock();
    TEST_CHECK(s_dl_deletes >= 1 && s_dl_depth_at_delete > 0, "nvs_erase_slot_locked: delete under the lock");
    profiles_cfg_fs_reset_delete_fn_for_test();
}

static void test_nvs_erase_slot_refuses_when_rev_array_unreadable(void)
{
    TEST_SECTION("nvs_erase_slot -- unreadable rev array is refused, other slots' floors not zeroed");
    pcfg_reset_all();
    profile_t p = make_stored_profile();
    s_profiles.profiles[3] = p;
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0x08);
    TEST_CHECK(nvs_save_slot(3) == ESP_OK, "save slot 3");
    nvs_handle_t h;
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    uint8_t junk[5] = {1, 2, 3, 4, 5};
    nvs_set_blob(h, NVS_KEY_PROFILE_REV, junk, sizeof(junk));
    nvs_commit(h);
    nvs_close(h);
    TEST_CHECK(nvs_erase_slot(3) != ESP_OK, "erase refused");
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h);
    uint8_t back[16] = {0};
    size_t blen = sizeof(back);
    nvs_get_blob(h, NVS_KEY_PROFILE_REV, back, &blen);
    nvs_close(h);
    TEST_CHECK(blen == sizeof(junk), "rev blob not overwritten");
}

static void test_nvs_erase_slot_repairs_wrong_size_used_bitmap(void)
{
    TEST_SECTION("nvs_erase_slot -- review LOW-2: a wrong-size used-bitmap blob is rebuilt, not a permanent outage");
    pcfg_reset_all();
    s_profiles.profiles[3] = make_stored_profile();
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0x08);
    TEST_CHECK(nvs_save_slot(3) == ESP_OK, "save slot 3");
    nvs_handle_t h;
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    uint8_t junk[5] = {1, 2, 3, 4, 5};
    nvs_set_blob(h, NVS_KEY_USED, junk, sizeof(junk));
    nvs_commit(h);
    nvs_close(h);
    TEST_CHECK(nvs_erase_slot(3) == ESP_OK, "delete succeeds despite the junk-size bitmap");
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h);
    uint8_t back[64] = {0};
    size_t blen = sizeof(back);
    TEST_CHECK(nvs_get_blob(h, NVS_KEY_USED, back, &blen) == ESP_OK && blen == sizeof(profiles_slot_bitmap_t),
               "the bad blob was overwritten with a 16-byte bitmap");
    nvs_close(h);
}

static void test_nvs_erase_slot_keeps_longer_used_bitmap_tail(void)
{
    TEST_SECTION("nvs_erase_slot -- review LOW-2: a longer newer-firmware used bitmap keeps its tail");
    pcfg_reset_all();
    s_profiles.profiles[3] = make_stored_profile();
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0x08);
    TEST_CHECK(nvs_save_slot(3) == ESP_OK, "save slot 3");
    nvs_handle_t h;
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    uint8_t longer[24];
    memset(longer, 0, sizeof(longer));
    longer[0] = 0x08; /* slot 3 used */
    for (int i = 16; i < 24; i++) {
        longer[i] = (uint8_t)(0xA0 + i);
    }
    nvs_set_blob(h, NVS_KEY_USED, longer, sizeof(longer));
    nvs_commit(h);
    nvs_close(h);
    TEST_CHECK(nvs_erase_slot(3) == ESP_OK, "delete succeeds with a longer bitmap");
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h);
    uint8_t back[64] = {0};
    size_t blen = sizeof(back);
    TEST_CHECK(nvs_get_blob(h, NVS_KEY_USED, back, &blen) == ESP_OK && blen == sizeof(longer),
               "rewritten at its full original length");
    TEST_CHECK(blen == sizeof(longer) && memcmp(back + 16, longer + 16, 8) == 0, "tail preserved verbatim");
    TEST_CHECK((back[0] & 0x08) == 0, "slot 3's bit cleared in the head");
    nvs_close(h);
}

static void test_favorites_wrong_size_blob_is_an_error(void)
{
    TEST_SECTION("profiles_favorites_start -- review LOW-3: a wrong-size favorites blob is not 'nothing favorited'");
    pcfg_reset_all();
    nvs_handle_t h;
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    uint8_t junk[5] = {1, 2, 3, 4, 5};
    nvs_set_blob(h, "prof_favusr", junk, sizeof(junk));
    nvs_commit(h);
    nvs_close(h);
    TEST_CHECK(profiles_favorites_start() != ESP_OK, "start() reports the unusable blob");
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    nvs_erase_key(h, "prof_favusr");
    nvs_commit(h);
    nvs_close(h);
    (void)profiles_favorites_start();
}

static void set_used_blob(const void *b, size_t n)
{
    nvs_handle_t h;
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    nvs_set_blob(h, NVS_KEY_USED, b, n);
    nvs_commit(h);
    nvs_close(h);
}

static void test_used_bitmap_rebuild_keeps_present_slot_bit(void)
{
    TEST_SECTION("nvs_erase_slot -- review LOW-2a test gap: a rebuild keeps the bit of a slot whose key is present");
    pcfg_reset_all();
    profile_t src = make_stored_profile();
    stage_legacy_slot(3, &src, 1); /* legacy profN keys: the rebuild indexes these */
    stage_legacy_slot(5, &src, 1);
    {
        nvs_handle_t kh;
        nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &kh);
        char k[8];
        uint8_t blob[8] = {1};
        profile_nvs_key(3, k, sizeof(k));
        nvs_set_blob(kh, k, blob, sizeof(blob));
        profile_nvs_key(5, k, sizeof(k));
        nvs_set_blob(kh, k, blob, sizeof(blob));
        nvs_commit(kh);
        nvs_close(kh);
    }
    s_profiles.profiles[3] = src;
    s_profiles.profiles[5] = src;
    uint8_t junk[5] = {1, 2, 3, 4, 5};
    set_used_blob(junk, sizeof(junk));
    TEST_CHECK(nvs_erase_slot(3) == ESP_OK, "delete slot 3 despite junk bitmap");
    nvs_handle_t h;
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h);
    uint8_t back[64] = {0};
    size_t blen = sizeof(back);
    TEST_CHECK(nvs_get_blob(h, NVS_KEY_USED, back, &blen) == ESP_OK && blen == 16, "16-byte bitmap");
    nvs_close(h);
    TEST_CHECK((back[0] & 0x20) != 0, "slot 5 (key present) keeps its bit");
    TEST_CHECK((back[0] & 0x08) == 0, "slot 3 cleared");
}

static void test_used_bitmap_boot_load_longer_and_junk_length(void)
{
    TEST_SECTION("nvs_load_all_from -- review LOW-2b: a 24-byte used blob loads; an 18-byte one is refused");
    pcfg_reset_all();
    profile_t src = make_stored_profile();
    stage_legacy_slot(4, &src, 1);
    uint8_t longer[24] = {0};
    longer[0] = 0x10;
    set_used_blob(longer, sizeof(longer));
    memset(&s_profiles, 0, sizeof(s_profiles));
    profiles_state_t out;
    bool any_found = false;
    esp_err_t e = nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found);
    TEST_CHECK(e == ESP_OK && profiles_slot_bitmap_test(&out.used_bitmap, 4), "24-byte bitmap loads at boot, slot 4 used");
    uint8_t junk20[18] = {0};
    junk20[0] = 0x10;
    set_used_blob(junk20, sizeof(junk20));
    memset(&s_profiles, 0, sizeof(s_profiles));
    memset(&out, 0, sizeof(out));
    e = nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found);
    TEST_CHECK(e != ESP_OK || !profiles_slot_bitmap_test(&out.used_bitmap, 4), "18-byte (non word multiple) bitmap is refused");
}

static size_t used_blob_len(void)
{
    nvs_handle_t h;
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h);
    uint8_t b[64];
    size_t n = sizeof(b);
    esp_err_t e = nvs_get_blob(h, NVS_KEY_USED, b, &n);
    nvs_close(h);
    return e == ESP_OK ? n : (size_t)-1;
}

static void test_used_bitmap_genuine_read_error_does_not_rebuild(void)
{
    TEST_SECTION("nvs_erase_slot -- review LOW-2a: a genuine read error on the bitmap is not rebuilt over");
    pcfg_reset_all();
    profile_t src = make_stored_profile();
    stage_legacy_slot(3, &src, 1);
    TEST_CHECK(fake_kv_script_corrupt_key(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_KEY_USED), "corrupt used key");
    s_profiles.profiles[3] = src;
    TEST_CHECK(nvs_erase_slot(3) != ESP_OK, "erase fails instead of rebuilding from a transient error");
    TEST_CHECK(used_blob_len() == (size_t)-1, "the unreadable bitmap was not overwritten");
}

static void test_used_bitmap_rebuild_fails_closed_on_probe_error(void)
{
    TEST_SECTION("nvs_erase_slot -- review LOW-2a: a profN probe error during rebuild saves nothing");
    pcfg_reset_all();
    profile_t src = make_stored_profile();
    stage_legacy_slot(3, &src, 1);
    stage_legacy_slot(5, &src, 1);
    uint8_t junk[5] = {1, 2, 3, 4, 5};
    set_used_blob(junk, sizeof(junk));
    TEST_CHECK(fake_kv_script_corrupt_key(PROFILES_NVS_PARTITION, NVS_NAMESPACE, "prof5"), "corrupt prof5");
    s_profiles.profiles[3] = src;
    TEST_CHECK(nvs_erase_slot(3) != ESP_OK, "erase fails closed");
    TEST_CHECK(used_blob_len() == sizeof(junk), "no bitmap was saved");
}

static void test_favorites_set_refuses_while_user_mask_unresolved(void)
{
    TEST_SECTION("profiles_favorites_set -- review LOW-3a: refuses after a failed load with no cfg file");
    pcfg_reset_all();
    nvs_handle_t h;
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    uint8_t junk[5] = {1, 2, 3, 4, 5};
    nvs_set_blob(h, "prof_favusr", junk, sizeof(junk));
    nvs_commit(h);
    nvs_close(h);
    TEST_CHECK(profiles_favorites_start() != ESP_OK, "start reports the unusable blob");
    TEST_CHECK(profiles_favorites_set(2, true) != ESP_OK, "set refused while the user mask is unresolved");
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    nvs_erase_key(h, "prof_favusr");
    nvs_commit(h);
    nvs_close(h);
    (void)profiles_favorites_start();
}

static void test_nvs_erase_slot_propagates_firing_stats_error(void)
{
    TEST_SECTION("nvs_erase_slot -- firing_stats_erase failure is propagated");
    pcfg_reset_all();
    profile_t p = make_stored_profile();
    s_profiles.profiles[3] = p;
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0x08);
    TEST_CHECK(nvs_save_slot(3) == ESP_OK, "save slot 3");
    g_firing_stats_erase_result = ESP_FAIL;
    esp_err_t e = nvs_erase_slot(3);
    g_firing_stats_erase_result = ESP_OK;
    TEST_CHECK(e == ESP_FAIL, "error propagated");
}

static void test_pcfg_rev0_file_with_invalid_nvs_is_adopted_not_deleted(void)
{
    TEST_SECTION("profiles cfg_fs -- F9: nvs_rev 0 with a valid file adopts the file; legacy migration never writes rev 0");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);

    profile_t p = make_stored_profile();
    TEST_CHECK(profiles_cfg_fs_save(0, &p, 0) == ESP_OK, "file at rev 0");
    profile_t out;
    uint32_t out_rev = 99;
    bool used_file = false;
    TEST_CHECK(profiles_cfg_fs_resolve(0, &p, false, 0, &out, &out_rev, &used_file), "resolve returns a profile");
    TEST_CHECK(used_file, "the rev-0 file is adopted");
    TEST_CHECK(pcfg_file_profile(0, &out), "the file was NOT deleted as stale");

    pcfg_reset_all();
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    out_rev = 0;
    TEST_CHECK(profiles_cfg_fs_resolve(1, &p, true, 0, &out, &out_rev, &used_file), "NVS-only slot resolves");
    TEST_CHECK(out_rev == 1, "migration reports rev max(nvs_rev, 1)");
    uint32_t file_rev = 0;
    bool valid = false;
    profile_t fp;
    profiles_cfg_fs_load_raw(1, &fp, &file_rev, &valid);
    TEST_CHECK(valid && file_rev == 1, "the migrated file carries rev 1, never 0");
}

static void test_pcfg_nvs_wins_when_it_has_the_higher_rev_and_resyncs_file(void)
{
    TEST_SECTION("profiles cfg_fs -- DIVERGENCE: NVS with the higher rev wins and resyncs the file");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);

    profile_t stale_file = make_stored_profile();
    strncpy(stale_file.name, "StaleFile", PROFILE_NAME_MAX_LEN);
    profile_t fresh_nvs = make_stored_profile();
    strncpy(fresh_nvs.name, "FreshNVS", PROFILE_NAME_MAX_LEN);

    // File at a LOW rev...
    TEST_CHECK(profiles_cfg_fs_save(0, &stale_file, 1) == ESP_OK, "stale file written at rev 1");
    // ...NVS at a HIGHER rev (two real saves happened; only the SECOND one's
    // file write is simulated as having failed by not calling
    // profiles_cfg_fs_save() for it -- the direct NVS stage below plays that
    // role).
    stage_legacy_slot(0, &fresh_nvs, 9);

    memset(&s_profiles, 0, sizeof(s_profiles));
    profiles_state_t out;
    bool any_found = false;
    TEST_CHECK(nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found) == ESP_OK, "reload succeeds");
    assert_profiles_equal(&out.profiles[0], &fresh_nvs, "NVS content wins (higher rev)");

    profile_t file_p;
    TEST_CHECK(pcfg_file_profile(0, &file_p), "the file still decodes (resync wrote it)");
    assert_profiles_equal(&file_p, &fresh_nvs, "the file was resynced to NVS's winning content");
}

static void test_nvs_erase_slot_prunes_firing_stats(void)
{
    TEST_SECTION("nvs_erase_slot() prunes that id's firing history (task 10, PROFILE_SLOTS_100.md sec 7)");
    pcfg_reset_all();

    profile_t p = make_stored_profile();
    s_profiles.profiles[3] = p;
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0x08);
    TEST_CHECK(nvs_save_slot(3) == ESP_OK, "save slot 3");

    g_firing_stats_erase_calls = 0;
    g_firing_stats_erase_last_id = 0xFF;
    TEST_CHECK(nvs_erase_slot(3) == ESP_OK, "erase slot 3 succeeds");
    TEST_CHECK(g_firing_stats_erase_calls == 1, "firing_stats_erase() called exactly once");
    TEST_CHECK(g_firing_stats_erase_last_id == 3, "firing_stats_erase() called with the erased slot's id");
}

static void test_nvs_erase_slot_prunes_firing_stats_for_never_fired_slot(void)
{
    TEST_SECTION("nvs_erase_slot() still calls firing_stats_erase() for a slot that never fired -- must be a "
                 "safe no-op on the real (unfaked) side, task 10's part (a)");
    pcfg_reset_all();

    profile_t p = make_stored_profile();
    s_profiles.profiles[5] = p;
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0x20);
    TEST_CHECK(nvs_save_slot(5) == ESP_OK, "save slot 5 -- never fired, no fs_5/fsr_5 key exists anywhere");

    g_firing_stats_erase_calls = 0;
    g_firing_stats_erase_last_id = 0xFF;
    TEST_CHECK(nvs_erase_slot(5) == ESP_OK, "erase still succeeds for a profile that was never fired");
    TEST_CHECK(g_firing_stats_erase_calls == 1, "firing_stats_erase() is still called -- it is this "
                                                 "function's job (real implementation), not this handler's, to "
                                                 "treat a missing key as a no-op");
    TEST_CHECK(g_firing_stats_erase_last_id == 5, "called with the right id");
}

static void test_profiles_http_delete_stats_and_erase_failures(void)
{
    TEST_SECTION("profiles_http_delete() -- a failed firing-stats prune keeps the slot (retryable); a "
                 "failed nvs_erase_slot() is reported, not swallowed (review L1)");
    pcfg_reset_all();
    profile_t p = make_stored_profile();
    s_profiles.profiles[8] = p;
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0x100);
    TEST_CHECK(nvs_save_slot(8) == ESP_OK, "save slot 8");
    g_fake_exec_state = PROFILE_EXEC_IDLE;
    g_fake_exec_profile_id = 0xFF;

    g_firing_stats_erase_result = ESP_FAIL;
    TEST_CHECK(!profiles_http_delete(8), "stats prune failure fails the delete");
    g_firing_stats_erase_result = ESP_OK;
    TEST_CHECK(profiles_slot_used(8), "slot stays used so the delete can be retried");
    TEST_CHECK(s_profiles.profiles[8].segment_count == p.segment_count, "slot content untouched");

    s_profile_rev_unknown[8] = true; /* nvs_erase_slot() REFUSES -> ESP_ERR_INVALID_STATE */
    TEST_CHECK(!profiles_http_delete(8), "nvs_erase_slot() failure is propagated as false, not true");
    s_profile_rev_unknown[8] = false;
    TEST_CHECK(profiles_slot_used(8) && s_profiles.profiles[8].segment_count == p.segment_count,
               "a failed persistent erase leaves the RAM slot live (consistent, retryable)");

    s_profiles.profiles[8] = p;
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0x100);
    TEST_CHECK(profiles_http_delete(8), "with nothing failing the delete succeeds");
    TEST_CHECK(!profiles_slot_used(8), "and the slot is gone");
}

static void test_profiles_http_delete_clears_favorite(void)
{
    TEST_SECTION("profiles_http_delete() clears the deleted slot's favorite mark (Opus review item 1, "
                 "PROFILE_SLOTS_100.md sec 7) -- profiles_edit_http.c's web delete handler already "
                 "does this; the benchproto path must not leave it undone");
    pcfg_reset_all();

    profile_t p = make_stored_profile();
    s_profiles.profiles[7] = p;
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0x80);
    TEST_CHECK(nvs_save_slot(7) == ESP_OK, "save slot 7");

    (void)profiles_favorites_set(7, false); // start from a known-clear state regardless of test order
    TEST_CHECK(profiles_favorites_set(7, true) == ESP_OK, "mark slot 7 favorite");
    TEST_CHECK(profiles_favorites_is(7), "slot 7 is favorite before delete");

    g_fake_exec_state = PROFILE_EXEC_IDLE;
    g_fake_exec_profile_id = 0xFF;
    TEST_CHECK(profiles_http_delete(7), "profiles_http_delete succeeds");
    TEST_CHECK(!profiles_favorites_is(7), "profiles_http_delete() cleared slot 7's favorite mark");
}

static bool fav_file_exists(void)
{
    bool e = false;
    return cfg_fs_exists(PROFILES_FAVORITES_FILE_PATH, &e) == ESP_OK && e;
}

static bool fav_nvs_has_keys(void)
{
    hal_kv_handle_t h;
    if (hal_kv_open(&h, "kiln_cfg", HAL_KV_MODE_READ_ONLY, "profiles_nvs") != HAL_OK) {
        return false;
    }
    uint32_t v = 0;
    size_t len = 0;
    bool any = hal_kv_get_u32(&h, "prof_favbi", &v) == HAL_OK || hal_kv_get_blob(&h, "prof_favusr", NULL, &len) == HAL_OK ||
               hal_kv_get_u32(&h, "prof_favusr", &v) == HAL_OK;
    hal_kv_close(&h);
    return any;
}

static bool s_prf_mark = false;
static bool prf_hook(void) { return s_prf_mark; }
static void test_profiles_saves_refuse_under_reset_mark(void)
{
    TEST_SECTION("nvs_save_slot and profiles_favorites_set -- refused under the save lock while the reset mark is set");
    nvs_stub_reset();
    s_profiles.profiles[3] = make_stored_profile();
    profiles_slot_bitmap_set(&s_profiles.used_bitmap, 3);
    pref_cfg_fs_set_reset_refuse_hook(prf_hook);
    s_prf_mark = true;
    TEST_CHECK(nvs_save_slot(3) == ESP_ERR_INVALID_STATE, "nvs_save_slot refuses");
    s_prf_mark = false;
    (void)profiles_favorites_set(5, false);
    s_prf_mark = true;
    TEST_CHECK(profiles_favorites_set(5, true) == ESP_ERR_INVALID_STATE, "profiles_favorites_set refuses");
    TEST_CHECK(!profiles_favorites_is(5), "refused favorite leaves RAM untouched");
    TEST_CHECK(!fav_file_exists(), "no favorites file written");
    s_prf_mark = false;
    pref_cfg_fs_set_reset_refuse_hook(NULL);
}

static void test_favorites_cfg_only_storage(void)
{
    TEST_SECTION("profiles_favorites -- cfg file only; a save never writes the legacy NVS keys");
    nvs_stub_reset();
    TEST_CHECK(profiles_favorites_set(3, true) == ESP_OK, "set favorite succeeds with cfg mounted");
    TEST_CHECK(fav_file_exists(), "favorites file written");
    TEST_CHECK(!fav_nvs_has_keys(), "no NVS favorites key was written");
    bool fv = false, nv = true, dv = true;
    uint32_t fr = 0, nr = 0;
    profiles_favorites_get_dualwrite_status(&fv, &fr, &nv, &nr, &dv);
    TEST_CHECK(fv && fr == 1 && !nv && !dv, "status: file valid rev 1, no NVS side, not diverged");
    g_fake_builtin_on = true; /* the fake catalogue exposes exactly one builtin id */
    TEST_CHECK(profiles_favorites_set(PROFILE_BUILTIN_ID_BASE, true) == ESP_OK, "builtin favorite saves too");
    // A restart (start()) reads the file back.
    TEST_CHECK(profiles_favorites_start() == ESP_OK, "start() succeeds");
    TEST_CHECK(profiles_favorites_is(3) && profiles_favorites_is(PROFILE_BUILTIN_ID_BASE),
               "both marks survive a restart from the file alone");
    g_fake_builtin_on = false;
}

static void test_favorites_refused_when_unmounted(void)
{
    TEST_SECTION("profiles_favorites -- with cfg unmounted a save is REFUSED, not masked");
    nvs_stub_reset();
    cfg_fs_deinit();
    esp_err_t e = profiles_favorites_set(2, true);
    TEST_CHECK(e == ESP_ERR_INVALID_STATE, "set returns the unmounted error");
    TEST_CHECK(!fav_nvs_has_keys(), "and nothing was written to NVS as a fallback");
    TEST_CHECK(profiles_favorites_is(2), "the change still applies live for this boot");
    // Audit L1: RAM already holds the change, so a retry sees "no change" -- it must still try the write.
    TEST_CHECK(profiles_favorites_set(2, true) == ESP_ERR_INVALID_STATE,
               "retry after a failed save is not a silent ESP_OK no-op");
    nvs_stub_reset();
    TEST_CHECK(!profiles_favorites_is(2), "and is gone after a restart (never persisted)");
}

static void test_favorites_legacy_nvs_migrates(void)
{
    TEST_SECTION("profiles_favorites -- legacy NVS masks are read at boot and migrated into the cfg file");
    nvs_stub_reset();
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, "kiln_cfg", HAL_KV_MODE_READ_WRITE, "profiles_nvs") == HAL_OK, "open legacy namespace");
    TEST_CHECK(hal_kv_set_u32(&h, "prof_favusr", 0x05u) == HAL_OK, "legacy user mask");
    TEST_CHECK(hal_kv_set_u32(&h, "prof_favbi", 0x01u) == HAL_OK, "legacy builtin mask");
    hal_kv_commit(&h);
    hal_kv_close(&h);
    TEST_CHECK(!fav_file_exists(), "precondition: no cfg file yet");
    TEST_CHECK(profiles_favorites_start() == ESP_OK, "start() succeeds");
    TEST_CHECK(profiles_favorites_is(0) && profiles_favorites_is(2) && !profiles_favorites_is(1),
               "legacy user marks are live");
    g_fake_builtin_on = true;
    TEST_CHECK(profiles_favorites_is(PROFILE_BUILTIN_ID_BASE), "legacy builtin mark is live");
    g_fake_builtin_on = false;
    TEST_CHECK(fav_file_exists(), "start() migrated the NVS copy into the cfg file");
    TEST_CHECK(fav_nvs_has_keys(), "the NVS copy is left in place (read fallback, never erased by start)");
    TEST_CHECK(profiles_favorites_set(0, false) == ESP_OK, "a later save lands in the file");
    // NVS still says slot 0 is a favorite; the file (higher rev) must win on the next boot.
    TEST_CHECK(profiles_favorites_start() == ESP_OK, "restart");
    TEST_CHECK(!profiles_favorites_is(0), "the file beats the stale NVS copy once it carries a newer rev");
}

static void test_profiles_http_delete_refuses_running_slot(void)
{
    TEST_SECTION("profiles_http_delete() refuses a slot the executor is currently running or has paused "
                 "(Opus review item 2, PROFILE_SLOTS_100.md sec 7)");
    pcfg_reset_all();

    profile_t p = make_stored_profile();
    s_profiles.profiles[6] = p;
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0x40);
    TEST_CHECK(nvs_save_slot(6) == ESP_OK, "save slot 6");

    g_fake_exec_state = PROFILE_EXEC_RUNNING;
    g_fake_exec_profile_id = 6;
    TEST_CHECK(!profiles_http_delete(6), "delete refused while the executor is RUNNING this slot");
    profile_t still_there;
    TEST_CHECK(profiles_http_get(6, &still_there), "slot 6 is still present after the refused delete");

    g_fake_exec_state = PROFILE_EXEC_PAUSED;
    g_fake_exec_profile_id = 6;
    TEST_CHECK(!profiles_http_delete(6), "delete refused while the executor is PAUSED on this slot");

    // A different slot running does not block deleting slot 6.
    g_fake_exec_state = PROFILE_EXEC_RUNNING;
    g_fake_exec_profile_id = 3;
    TEST_CHECK(profiles_http_delete(6), "delete succeeds when the executor is running a DIFFERENT slot");

    g_fake_exec_state = PROFILE_EXEC_IDLE;
    g_fake_exec_profile_id = 0xFF;
}

// ---------------------------------------------------------------------------
// HTTP input parsing audit L23: delete vs profile start. The delete marks the
// slot in flight BEFORE its running check (which takes s_exec.lock); the start
// re-checks profiles_http_slot_runnable() under s_exec.lock before committing
// RUNNING. These pin the delete side of that protocol (the start side is
// test_profile_executor_prestart.c's test_run_refuses_slot_being_deleted()).
static int s_l23_runnable_at_erase = -1; /* runnable seen mid-erase */
static int s_l23_nested_delete = -1;     /* second delete's result mid-erase */

static esp_err_t l23_delete_fn(const char *rel_path)
{
    s_l23_runnable_at_erase = profiles_http_slot_runnable(s_l23_id) ? 1 : 0;
    s_l23_nested_delete = (int)profiles_delete_slot(s_l23_id);
    return cfg_fs_delete(rel_path);
}

// Seqlock generation: a start that captured the slot generation before its copy
// is refused after ANY RAM assign, including a save whose persist FAILED (RAM
// changed, published rev unchanged), and while an assign is in flight (odd).
static uint32_t s_gen_seen_in_write = 0;
static esp_err_t gen_probe_write_fn(const char *rel_path, const void *data, size_t len)
{
    s_gen_seen_in_write = profiles_http_slot_rev(4);
    return cfg_fs_write_atomic(rel_path, data, len);
}

static esp_err_t failing_write_fn(const char *rel_path, const void *data, size_t len)
{
    (void)rel_path; (void)data; (void)len;
    return ESP_FAIL;
}

/* Models cfg_fs_write_atomic() failing AFTER its rename: the file is in place, the caller sees ESP_FAIL. */
static esp_err_t write_then_fail_fn(const char *rel_path, const void *data, size_t len)
{
    (void)cfg_fs_write_atomic(rel_path, data, len);
    return ESP_FAIL;
}

static void test_save_ex_rollback_unlinks_written_file(void)
{
    TEST_SECTION("misc8 LOW-1: rolled-back fresh slot whose file landed on flash is unlinked");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    g_fake_exec_state = PROFILE_EXEC_IDLE;
    g_fake_exec_profile_id = 0xFF;
    profile_t p = make_stored_profile();
    uint8_t out_id = 0xFF, warn = 0;
    bool persisted = true;
    char err[128];
    profiles_cfg_fs_set_write_fn(write_then_fail_fn);
    bool ok = profiles_http_save_ex(PROFILES_MAX_COUNT, &p, &out_id, &warn, &persisted, err, sizeof(err));
    profiles_cfg_fs_reset_write_fn_for_test();
    TEST_CHECK(ok && !persisted && out_id < PROFILES_MAX_COUNT, "save reports persisted=false");
    profile_t loaded;
    uint32_t rev = 0;
    bool valid = true;
    profiles_cfg_fs_load_raw(out_id, &loaded, &rev, &valid);
    TEST_CHECK(!valid, "rolled-back slot's file is gone (would resurrect at next boot)");
}

static void rvfx_prepare_free_slot0_with_file(uint32_t file_rev)
{
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    g_fake_exec_state = PROFILE_EXEC_IDLE;
    g_fake_exec_profile_id = 0xFF;
    memset(&s_profiles, 0, sizeof(s_profiles));
    memset(s_profile_rev_unknown, 0, sizeof(s_profile_rev_unknown));
    memset(s_profile_rev, 0, sizeof(s_profile_rev));
    profile_t old = make_stored_profile();
    profiles_cfg_fs_save(0, &old, file_rev);
}

static bool rvfx_slot0_file_valid(void)
{
    profile_t loaded;
    uint32_t rev = 0;
    bool valid = false;
    profiles_cfg_fs_load_raw(0, &loaded, &rev, &valid);
    return valid;
}

static void test_rvfx_rollback_keeps_unexamined_file(void)
{
    TEST_SECTION("misc8fx MED-1: a refused SAVE_AS (rev floor unknown) must not delete the slot's unexamined file");
    rvfx_prepare_free_slot0_with_file(1);
    s_profile_rev_unknown[0] = true;
    profile_t p = make_stored_profile();
    uint8_t out_id = 0xFF, warn = 0;
    bool persisted = true;
    char err[128];
    bool ok = profiles_http_save_ex(PROFILES_MAX_COUNT, &p, &out_id, &warn, &persisted, err, sizeof(err));
    TEST_CHECK(ok && !persisted && out_id == 0, "save landed on slot 0 and was refused");
    TEST_CHECK(rvfx_slot0_file_valid(), "slot 0's unexamined file survived the rollback");
    s_profile_rev_unknown[0] = false;
}

static void test_rvfx_rollback_keeps_file_with_other_rev(void)
{
    TEST_SECTION("misc8fx LOW-1: a failed write before the rename keeps an existing file (rev differs from the attempt)");
    rvfx_prepare_free_slot0_with_file(5);
    profile_t p = make_stored_profile();
    uint8_t out_id = 0xFF, warn = 0;
    bool persisted = true;
    char err[128];
    profiles_cfg_fs_set_write_fn(failing_write_fn);
    bool ok = profiles_http_save_ex(PROFILES_MAX_COUNT, &p, &out_id, &warn, &persisted, err, sizeof(err));
    profiles_cfg_fs_reset_write_fn_for_test();
    TEST_CHECK(ok && !persisted && out_id == 0, "save refused on slot 0");
    TEST_CHECK(rvfx_slot0_file_valid(), "pre-existing file (rev 5 != attempted rev 1) survived");
}

static void test_save_ex_fresh_slot_rolled_back_in_lock(void)
{
    TEST_SECTION("fwlow16 LOW-1: failed persist of a fresh slot rolls back inside save_ex; overwrite stays applied");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    g_fake_exec_state = PROFILE_EXEC_IDLE;
    g_fake_exec_profile_id = 0xFF;

    profile_t p = make_stored_profile();
    uint8_t out_id = 0xFF;
    uint8_t warn = 0;
    bool persisted = true;
    char err[128];
    profiles_cfg_fs_set_write_fn(failing_write_fn);
    bool ok = profiles_http_save_ex(PROFILES_MAX_COUNT, &p, &out_id, &warn, &persisted, err, sizeof(err));
    profiles_cfg_fs_reset_write_fn_for_test();
    TEST_CHECK(ok && !persisted && out_id < PROFILES_MAX_COUNT, "fresh save reports persisted=false");
    TEST_CHECK(!profiles_slot_used(out_id), "fresh slot rolled back (no phantom RAM-only profile)");
    uint8_t id1 = out_id;
    persisted = false;
    ok = profiles_http_save_ex(PROFILES_MAX_COUNT, &p, &out_id, &warn, &persisted, err, sizeof(err));
    TEST_CHECK(ok && persisted && out_id == id1, "same-name retry succeeds in the freed slot");

    /* Overwrite of an existing slot keeps the applied-live convention. */
    profiles_cfg_fs_set_write_fn(failing_write_fn);
    persisted = true;
    ok = profiles_http_save_ex(id1, &p, &out_id, &warn, &persisted, err, sizeof(err));
    profiles_cfg_fs_reset_write_fn_for_test();
    TEST_CHECK(ok && !persisted && profiles_slot_used(id1), "overwrite stays applied when persist fails");
}

static void test_profiles_slot_gen_seqlock(void)
{
    TEST_SECTION("slot generation: failed save and in-flight assign refuse a captured start");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    g_fake_exec_state = PROFILE_EXEC_IDLE;
    g_fake_exec_profile_id = 0xFF;

    profile_t p = make_stored_profile();
    uint8_t out_id = 0xFF;
    char err[128];
    TEST_CHECK(profiles_http_save(4, &p, &out_id, NULL, err, sizeof(err)), "save slot 4");
    uint32_t cap = profiles_http_slot_rev(4);
    TEST_CHECK(profiles_http_slot_runnable_rev(4, cap), "captured generation runnable");

    /* The generation is odd for the whole RAM-assign + file-write window. */
    s_gen_seen_in_write = 0;
    profiles_cfg_fs_set_write_fn(gen_probe_write_fn);
    TEST_CHECK(profiles_http_save(4, &p, &out_id, NULL, err, sizeof(err)), "save slot 4 (probe)");
    profiles_cfg_fs_reset_write_fn_for_test();
    TEST_CHECK((s_gen_seen_in_write & 1u) == 1u, "generation is odd while the assign/persist is in flight");
    TEST_CHECK(!profiles_http_slot_runnable_rev(4, cap), "successful save after capture: old copy refused");
    cap = profiles_http_slot_rev(4);

    /* Failed save: persist refused, RAM still takes the new content. */
    s_profile_rev_unknown[4] = true;
    (void)profiles_http_save(4, &p, &out_id, NULL, err, sizeof(err));
    s_profile_rev_unknown[4] = false;
    TEST_CHECK(!profiles_http_slot_runnable_rev(4, cap), "failed save after capture: old copy refused");
    TEST_CHECK(profiles_http_slot_runnable_rev(4, profiles_http_slot_rev(4)), "fresh capture runnable after failed save");

    /* Assign in flight. */
    uint32_t cap2 = profiles_http_slot_rev(4);
    profiles_slot_gen_begin(4);
    TEST_CHECK(!profiles_http_slot_runnable_rev(4, cap2), "assign in progress: refused");
    uint32_t odd = profiles_http_slot_rev(4);
    TEST_CHECK(!profiles_http_slot_runnable_rev(4, odd), "capture taken mid-assign (odd): refused");
    profiles_slot_gen_end(4);
    TEST_CHECK(!profiles_http_slot_runnable_rev(4, odd), "odd capture stays refused after the assign ends");
    TEST_CHECK(!profiles_http_slot_runnable_rev(4, cap2), "pre-assign capture refused after the assign ends");
}

static void test_profiles_delete_start_race_l23(void)
{
    TEST_SECTION("L23: a start is refused while a delete of the slot is in flight; delete refused while running");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    g_fake_exec_state = PROFILE_EXEC_IDLE;
    g_fake_exec_profile_id = 0xFF;

    s_l23_id = 4;
    s_profiles.profiles[4] = make_stored_profile();
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0x10);
    TEST_CHECK(nvs_save_slot(4) == ESP_OK, "save slot 4");
    TEST_CHECK(profiles_http_slot_runnable(4), "an idle used slot is runnable");
    TEST_CHECK(!profiles_http_slot_runnable(5), "an unused slot is not runnable");

    s_l23_runnable_at_exec_check = -1;
    s_l23_runnable_at_erase = -1;
    s_l23_nested_delete = -1;
    s_l23_probe_exec = true;
    profiles_cfg_fs_set_delete_fn(l23_delete_fn);
    TEST_CHECK(profiles_delete_slot(4) == PROFILES_DELETE_OK, "delete slot 4");
    profiles_cfg_fs_reset_delete_fn_for_test();
    s_l23_probe_exec = false;
    TEST_CHECK(s_l23_runnable_at_exec_check == 0,
               "the in-flight mark is set BEFORE the running check (a start in that window is refused)");
    TEST_CHECK(s_l23_runnable_at_erase == 0, "a start during the erase is refused");
    TEST_CHECK(s_l23_nested_delete == (int)PROFILES_DELETE_BUSY, "a second delete of the same slot mid-erase is BUSY");
    TEST_CHECK(!profiles_http_slot_runnable(4), "after the delete the slot is not runnable");

    /* Residual edge: delete + re-save under the same id changes the published rev. */
    {
        s_profiles.profiles[4] = make_stored_profile();
        profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0x10);
        TEST_CHECK(nvs_save_slot(4) == ESP_OK, "save slot 4 (rev capture)");
        uint32_t cap = profiles_http_slot_rev(4);
        TEST_CHECK(profiles_http_slot_runnable_rev(4, cap), "unchanged rev: runnable");
        TEST_CHECK(profiles_delete_slot(4) == PROFILES_DELETE_OK, "delete slot 4 (rev capture)");
        s_profiles.profiles[4] = make_stored_profile();
        profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0x10);
        TEST_CHECK(nvs_save_slot(4) == ESP_OK, "re-save slot 4 under the same id");
        TEST_CHECK(profiles_slot_used(4) && profiles_http_slot_runnable(4), "slot used and runnable again");
        TEST_CHECK(profiles_http_slot_rev(4) != cap, "rev moved on delete + re-save");
        TEST_CHECK(!profiles_http_slot_runnable_rev(4, cap), "stale captured rev: refused");
        TEST_CHECK(profiles_http_slot_runnable_rev(4, profiles_http_slot_rev(4)), "fresh rev: runnable");
    }

    /* Delete refused while running releases the mark: the slot stays runnable. */
    s_profiles.profiles[4] = make_stored_profile();
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0x10);
    TEST_CHECK(nvs_save_slot(4) == ESP_OK, "re-save slot 4");
    g_fake_exec_state = PROFILE_EXEC_RUNNING;
    g_fake_exec_profile_id = 4;
    TEST_CHECK(profiles_delete_slot(4) == PROFILES_DELETE_RUNNING, "delete refused while the executor runs slot 4");
    TEST_CHECK(profiles_slot_used(4) && profiles_http_slot_runnable(4), "refused delete leaves the slot and no mark");
    g_fake_exec_state = PROFILE_EXEC_IDLE;
    g_fake_exec_profile_id = 0xFF;

    /* A failed erase releases the mark too. */
    s_profile_rev_unknown[4] = true;
    TEST_CHECK(profiles_delete_slot(4) == PROFILES_DELETE_PERSIST_FAILED, "erase refusal reported");
    s_profile_rev_unknown[4] = false;
    TEST_CHECK(profiles_http_slot_runnable(4), "failed delete leaves the slot runnable");
    TEST_CHECK(profiles_delete_slot(4) == PROFILES_DELETE_OK, "retry succeeds");
    TEST_CHECK(profiles_delete_slot(4) == PROFILES_DELETE_NOT_FOUND, "deleting it again is NOT_FOUND");
}

// ---------------------------------------------------------------------------
// Review fold-in (PROFILE_SLOTS_100.md section 7): favorite-clear must
// run BEFORE slot erase, not after. Runtime behavior is identical either way
// on the happy path -- the bug this guards against is a power cut landing
// BETWEEN the two steps, which no host test can simulate by actually
// interrupting execution. Source-text scan is the established precedent for
// this class of ordering property in this suite (test_zone_sweep_relay_off_
// wiring.c, test_display_power_wiring.c, test_safety_core_s8_wiring.c):
// extract each function's body and assert the favorite-clear call's source
// offset precedes the erase call's.
static void assert_favorite_clear_precedes_erase(const char *fn, const char *fn_name)
{
    const char *fav = strstr(fn, "profiles_favorites_set((uint8_t)id, false)");
    TEST_CHECK(fav != NULL, "expected a profiles_favorites_set((uint8_t)id, false) call inside this function");
    const char *erase_slot_call = strstr(fn, "profiles_slot_clear(id)");
    TEST_CHECK(erase_slot_call != NULL, "expected a profiles_slot_clear(id) call inside this function");
    if (fav && erase_slot_call) {
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "%s: favorite must be cleared BEFORE the slot is erased -- a power cut between the "
                 "two must never leave the slot erased with its favorite bit still set (an import over "
                 "that id would inherit the orphaned favorite)",
                 fn_name);
        TEST_CHECK(fav < erase_slot_call, msg);
    }
}

static void test_delete_clears_favorite_before_erase_wiring(void)
{
    TEST_SECTION("profiles_http_delete()/profile_delete_post_handler() -- favorite-clear precedes "
                 "slot-erase in source order (review fold-in, PROFILE_SLOTS_100.md section 7)");

    static const char *HTTP_C_CANDIDATES[] = {
        "../drivers/http/profiles_http.c",
        "App/drivers/http/profiles_http.c",
        "firmware/KilnFW/App/drivers/http/profiles_http.c",
    };
    char *http_text = test_read_source_anchored(__FILE__, "../drivers/http/profiles_http.c",
                                                 HTTP_C_CANDIDATES, 3);
    TEST_CHECK(http_text != NULL, "could not locate profiles_http.c from the host test's working directory");
    if (http_text) {
        size_t len = 0;
        /* Both delete doors share profiles_delete_slot() since the L23 fix. */
        const char *body = find_function_body(http_text, "profiles_delete_result_t profiles_delete_slot(uint8_t id)", &len);
        TEST_CHECK(body != NULL, "could not find profiles_delete_slot()'s function body -- update this test "
                                  "if it was renamed/restructured");
        if (body) {
            char *fn = dup_range(body, len);
            if (fn) {
                assert_favorite_clear_precedes_erase(fn, "profiles_delete_slot()");
                free(fn);
            }
        }
        free(http_text);
    }

    static const char *EDIT_HTTP_C_CANDIDATES[] = {
        "../drivers/http/profiles_edit_http.c",
        "App/drivers/http/profiles_edit_http.c",
        "firmware/KilnFW/App/drivers/http/profiles_edit_http.c",
    };
    char *edit_text = test_read_source_anchored(__FILE__, "../drivers/http/profiles_edit_http.c",
                                                 EDIT_HTTP_C_CANDIDATES, 3);
    TEST_CHECK(edit_text != NULL, "could not locate profiles_edit_http.c from the host test's working directory");
    if (edit_text) {
        size_t len = 0;
        const char *body = find_function_body(edit_text, "profile_delete_post_handler(httpd_req_t *req)", &len);
        TEST_CHECK(body != NULL, "could not find the web delete handler's function body -- update this test "
                                  "if it was renamed/restructured");
        if (body) {
            char *fn = dup_range(body, len);
            if (fn) {
                /* The web door must go through the shared sequence, never a
                 * private erase that skips the delete-in-flight mark (L23). */
                TEST_CHECK(strstr(fn, "profiles_delete_slot(") != NULL,
                           "profile_delete_post_handler() delegates to profiles_delete_slot()");
                TEST_CHECK(strstr(fn, "nvs_erase_slot") == NULL && strstr(fn, "profiles_slot_clear") == NULL,
                           "profile_delete_post_handler() has no private erase path");
                free(fn);
            }
        }
        free(edit_text);
    }
}

static void test_pcfg_stale_file_after_delete_is_not_resurrected(void)
{
    TEST_SECTION("profiles cfg_fs -- a file left behind by an interrupted delete is not resurrected");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);

    profile_t p = make_stored_profile();
    s_profiles.profiles[0] = p;
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0x01);
    TEST_CHECK(nvs_save_slot(0) == ESP_OK, "save lands in the file at rev 1");
    stage_legacy_slot(0, &p, 1); // a stale legacy NVS copy from before the dual-write close

    // The cfg file delete fails: erase must fail loud and leave the slot
    // intact (erase-first order: legacy NVS keys are gone, file kept).
    profiles_cfg_fs_set_delete_fn(pcfg_failing_delete_fn);
    esp_err_t erase_err = nvs_erase_slot(0);
    profiles_cfg_fs_reset_delete_fn_for_test();
    TEST_CHECK(erase_err != ESP_OK, "erase reports the failed file delete instead of claiming success");
    TEST_CHECK(!pcfg_nvs_slot_blob_present(0), "the legacy NVS blob was erased first, so it cannot resurrect");

    profile_t leftover;
    TEST_CHECK(pcfg_file_profile(0, &leftover), "the file is still present (delete failed)");

    // A retry with a working delete finishes the job and the slot stays gone.
    TEST_CHECK(nvs_erase_slot(0) == ESP_OK, "retry erase succeeds");
    TEST_CHECK(!pcfg_file_profile(0, &leftover), "the file is gone after the retry");
    memset(&s_profiles, 0, sizeof(s_profiles));
    profiles_state_t out;
    bool any_found = false;
    TEST_CHECK(nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found) == ESP_OK, "reload succeeds");
    TEST_CHECK(!profiles_slot_bitmap_test(&out.used_bitmap, 0), "slot 0 stays unused after delete: no NVS or file copy remains");
}

// Regression for the dual-write close: nvs_erase_slot() used to persist the
// WHOLE in-RAM used bitmap and rev array into the legacy NVS keys. Once saves
// are file-only, that copied a post-close slot's file rev into NVS next to a
// stale legacy blob (equal revs, differing bytes -- the resolve adopts NVS and
// silently reverts the edit) and set the NVS bit of a slot that only ever
// existed as a file (no "profN" key, so the resolve deleted its file as a
// stale leftover). Deleting ANY slot must leave every other slot intact.
static void test_pcfg_delete_does_not_revert_or_drop_other_file_only_slots(void)
{
    TEST_SECTION("profiles cfg_fs -- deleting one slot neither reverts a post-close edit nor drops a file-only slot");
    pcfg_reset_all();
    size_t reaped = 0;
    TEST_CHECK(cfg_fs_init(PCFG_SCRATCH_BASE, &reaped) == ESP_OK, "cfg_fs mounts against the scratch dir");

    profile_t old_p = make_stored_profile();
    strncpy(old_p.name, "LegacyOld", PROFILE_NAME_MAX_LEN);
    stage_legacy_slot(0, &old_p, 1); // pre-close board: slot 0 in NVS at rev 1

    profiles_state_t out;
    bool any_found = false;
    memset(&s_profiles, 0, sizeof(s_profiles));
    TEST_CHECK(nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found) == ESP_OK, "first boot load succeeds");
    s_profiles = out;

    // Post-close edit of slot 0 (file rev 2, legacy NVS blob still "LegacyOld").
    strncpy(s_profiles.profiles[0].name, "EditedNew", PROFILE_NAME_MAX_LEN);
    TEST_CHECK(nvs_save_slot(0) == ESP_OK, "post-close edit of slot 0 saves to the file");
    // Post-close new slot 2, file only.
    profile_t fresh = make_stored_profile();
    strncpy(fresh.name, "FileOnly", PROFILE_NAME_MAX_LEN);
    s_profiles.profiles[2] = fresh;
    profiles_slot_bitmap_set(&s_profiles.used_bitmap, 2);
    TEST_CHECK(nvs_save_slot(2) == ESP_OK, "post-close new slot 2 saves to the file");
    // Post-close slot 4, then delete it.
    profile_t doomed = make_stored_profile();
    strncpy(doomed.name, "Doomed", PROFILE_NAME_MAX_LEN);
    s_profiles.profiles[4] = doomed;
    profiles_slot_bitmap_set(&s_profiles.used_bitmap, 4);
    TEST_CHECK(nvs_save_slot(4) == ESP_OK, "slot 4 saves to the file");
    profiles_slot_bitmap_clear(&s_profiles.used_bitmap, 4);
    TEST_CHECK(nvs_erase_slot(4) == ESP_OK, "delete of slot 4 succeeds");

    // Reboot.
    memset(&s_profiles, 0, sizeof(s_profiles));
    memset(&out, 0, sizeof(out));
    TEST_CHECK(nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found) == ESP_OK, "reload succeeds");
    TEST_CHECK(profiles_slot_bitmap_test(&out.used_bitmap, 0), "slot 0 still used");
    TEST_CHECK(strcmp(out.profiles[0].name, "EditedNew") == 0,
               "slot 0 keeps its post-close edit (not reverted to the stale legacy NVS blob)");
    TEST_CHECK(profiles_slot_bitmap_test(&out.used_bitmap, 2), "file-only slot 2 survives another slot's delete");
    TEST_CHECK(strcmp(out.profiles[2].name, "FileOnly") == 0, "slot 2 content intact");
    profile_t file_p;
    TEST_CHECK(pcfg_file_profile(2, &file_p), "slot 2's file was not deleted as a stale leftover");
    TEST_CHECK(!profiles_slot_bitmap_test(&out.used_bitmap, 4), "deleted slot 4 stays deleted");
}

static void test_pcfg_partition_absent_behaves_exactly_like_before(void)
{
    TEST_SECTION("profiles cfg_fs -- partition never mounted: a legacy NVS slot still loads, a save fails loud");
    pcfg_reset_all();
    cfg_fs_deinit(); // status UNMOUNTED, cfg_fs_is_available() is false

    profile_t src = make_stored_profile();
    stage_legacy_slot(0, &src, 1);

    s_profiles.profiles[1] = src;
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0x02);
    TEST_CHECK(nvs_save_slot(1) != ESP_OK, "save fails loud with no cfg partition at all");
    TEST_CHECK(!pcfg_nvs_slot_blob_present(1), "the failed save did not fall back to NVS");

    memset(&s_profiles, 0, sizeof(s_profiles));
    profiles_state_t out;
    bool any_found = false;
    TEST_CHECK(nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found) == ESP_OK, "reload succeeds");
    TEST_CHECK(profiles_slot_bitmap_test(&out.used_bitmap, 0), "legacy slot 0 still used");
    assert_profiles_equal(&out.profiles[0], &src, "the legacy NVS copy loads when cfg_fs is unavailable");
    TEST_CHECK(!profiles_slot_bitmap_test(&out.used_bitmap, 1), "the refused save left no slot 1");
}

static void test_pcfg_mount_failed_behaves_like_absent(void)
{
    TEST_SECTION("profiles cfg_fs -- mount attempted and failed: same legacy-load / fail-loud-save behavior");
    pcfg_reset_all();
    size_t reaped = 0;
    // A path that cannot be listed at all makes cfg_fs_init() report
    // UNAVAILABLE (cfg_fs.h's own documented contract), not MOUNTED.
    esp_err_t err = cfg_fs_init("Z:/definitely/does/not/exist/kilnctl_cfg_fs_test", &reaped);
    TEST_CHECK(err != ESP_OK, "cfg_fs_init against an unlistable path reports failure");
    TEST_CHECK(!cfg_fs_is_available(), "cfg_fs_is_available() is false after a failed mount");

    profile_t src = make_stored_profile();
    stage_legacy_slot(0, &src, 1);
    s_profiles.profiles[1] = src;
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0x02);
    TEST_CHECK(nvs_save_slot(1) != ESP_OK, "save fails loud despite NVS being available");

    memset(&s_profiles, 0, sizeof(s_profiles));
    profiles_state_t out;
    bool any_found = false;
    TEST_CHECK(nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found) == ESP_OK, "reload succeeds");
    assert_profiles_equal(&out.profiles[0], &src, "the legacy NVS copy still loads when the mount failed");
}

static void test_pcfg_interrupted_write_leaves_old_file_intact(void)
{
    TEST_SECTION("profiles cfg_fs -- a failed file write leaves the OLD file byte-for-byte untouched");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);

    profile_t original = make_stored_profile();
    TEST_CHECK(profiles_cfg_fs_save(0, &original, 1) == ESP_OK, "initial save succeeds");

    profile_t attempted_update = make_stored_profile();
    strncpy(attempted_update.name, "ShouldNotLand", PROFILE_NAME_MAX_LEN);
    profiles_cfg_fs_set_write_fn(pcfg_failing_write_fn);
    esp_err_t err = profiles_cfg_fs_save(0, &attempted_update, 2);
    profiles_cfg_fs_reset_write_fn_for_test();
    TEST_CHECK(err != ESP_OK, "the forced-failing write is reported as a failure, not swallowed");

    profile_t file_p;
    uint32_t rev = 0;
    bool valid = false;
    profiles_cfg_fs_load_raw(0, &file_p, &rev, &valid);
    TEST_CHECK(valid, "the OLD file is still readable after the failed write attempt");
    TEST_CHECK(rev == 1, "the OLD file's rev is unchanged (rev 2 never landed)");
    assert_profiles_equal(&file_p, &original, "the OLD file's content is byte-for-byte the original");
}

static void test_pcfg_save_load_delete_round_trip_through_real_api(void)
{
    TEST_SECTION("profiles cfg_fs -- save/load/delete round trip through the real profiles_http_* API");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);

    profile_t candidate = make_stored_profile();
    uint8_t out_id = 0xFF;
    char err_msg[128];
    TEST_CHECK(profiles_http_save(0, &candidate, &out_id, NULL, err_msg, sizeof(err_msg)),
               "profiles_http_save succeeds through the unchanged public API");
    TEST_CHECK(out_id == 0, "slot 0 assigned as requested");

    profile_t file_p;
    TEST_CHECK(pcfg_file_profile(0, &file_p), "a file now backs slot 0");
    assert_profiles_equal(&file_p, &candidate, "the file's content matches what was saved");

    profile_t got;
    TEST_CHECK(profiles_http_get(0, &got), "profiles_http_get still finds it");
    assert_profiles_equal(&got, &candidate, "get returns what was saved");

    TEST_CHECK(profiles_http_delete(0), "profiles_http_delete succeeds through the unchanged public API");
    TEST_CHECK(!profiles_http_get(0, &got), "get no longer finds a deleted slot");
    bool still_there = pcfg_file_profile(0, &file_p);
    TEST_CHECK(!still_there, "the file is gone after delete");
}

// Test 1 -- THE regression test: a version-1 blob (the historical,
// pre-crc32 layout every already-saved profile on a real board actually is)
// loads successfully and every field survives. See this file's header
// comment and profiles_http.c's PROFILE_VERSION comment for the exact
// data-loss bug this reproduces and guards against.
// ---------------------------------------------------------------------------
static void test_v1_blob_loads_and_preserves_all_fields(void)
{
    TEST_SECTION("nvs_load_all_from -- REGRESSION: a v1 (pre-crc32) blob loads and keeps every field");

    nvs_stub_reset();
    profile_t src = make_stored_profile();
    profile_persisted_v1_t v1;
    v1.version = 1;
    v1.profile = to_v2(&src);
    stage_profile_blob(0, &v1, sizeof(v1));
    stage_bitmap(0x01);

    profiles_state_t out;
    bool any_found = false;
    esp_err_t err = nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(any_found, "the prof_used bitmap key was present");
    TEST_CHECK(profiles_slot_bitmap_test(&out.used_bitmap, 0),
              "REGRESSION: slot 0 must still be reported used -- a v1 blob rejected as \"wrong length for "
              "its claimed version\" is exactly the bug that wiped real saved profiles on the bench board");
    assert_profiles_equal(&out.profiles[0], &src, "v1 regression");
}

// rf4 follow-up: on target a blob read of a legacy u8 `used` key is NOT_FOUND, not INVALID_ARG.
static void test_legacy_u8_used_key_with_target_not_found(void)
{
    TEST_SECTION("used_bitmap_load -- legacy u8 prof_used key whose blob read is NOT_FOUND (target) still loads");

    nvs_stub_reset();
    profile_t src = make_stored_profile();
    profile_persisted_v1_t v1;
    v1.version = 1;
    v1.profile = to_v2(&src);
    stage_profile_blob(0, &v1, sizeof(v1));
    stage_bitmap(0x01);
    fake_kv_script_blob_get_misses_size1(true);

    profiles_state_t out;
    bool any_found = false;
    esp_err_t err = nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found);
    fake_kv_script_blob_get_misses_size1(false);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(any_found, "the legacy u8 used key counts as present");
    TEST_CHECK(profiles_slot_bitmap_test(&out.used_bitmap, 0), "slot 0 still used via the get_u8 fallback");
}

// ---------------------------------------------------------------------------
// Test 2 -- version == 0 is rejected, not installed as a used slot.
// ---------------------------------------------------------------------------
static void test_version_zero_rejected(void)
{
    TEST_SECTION("nvs_load_all_from -- version 0 is never installed as a used slot");

    nvs_stub_reset();
    profile_t src = make_stored_profile();
    profile_persisted_v1_t blob;
    blob.version = 0; /* the exact "version 0 accepted at any length" historical bug */
    blob.profile = to_v2(&src);
    stage_profile_blob(0, &blob, sizeof(blob));
    stage_bitmap(0x01);

    profiles_state_t out;
    bool any_found = false;
    esp_err_t err = nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(!profiles_slot_bitmap_test(&out.used_bitmap, 0), "version 0 must never be installed as a used slot");
    profile_t zero;
    memset(&zero, 0, sizeof(zero));
    TEST_CHECK(memcmp(&out.profiles[0], &zero, sizeof(zero)) == 0,
              "no field of a version-0 blob may leak into the decoded profile_t");
}

// ---------------------------------------------------------------------------
// Test 3 -- a blob whose length does not match its claimed version is
// rejected outright (using the v1/no-CRC path, so the length check is the
// only gate in play -- a CRC mismatch on the current-version path is covered
// separately by test 4).
// ---------------------------------------------------------------------------
static void test_length_mismatch_rejected(void)
{
    TEST_SECTION("nvs_load_all_from -- length not matching the claimed (v1) version is rejected");

    nvs_stub_reset();
    profile_t src = make_stored_profile();
    profile_persisted_v1_t v1;
    v1.version = 1;
    v1.profile = to_v2(&src);
    stage_profile_blob(0, &v1, sizeof(v1) - 1); /* one byte short */
    stage_bitmap(0x01);

    profiles_state_t out;
    bool any_found = false;
    esp_err_t err = nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(!profiles_slot_bitmap_test(&out.used_bitmap, 0), "a length mismatch for the claimed version must be rejected");
    profile_t zero;
    memset(&zero, 0, sizeof(zero));
    TEST_CHECK(memcmp(&out.profiles[0], &zero, sizeof(zero)) == 0, "nothing may leak through from a rejected blob");
}

// ---------------------------------------------------------------------------
// Test 4 -- a current-version blob with a bad CRC is rejected.
// ---------------------------------------------------------------------------
static void test_bad_crc_rejected(void)
{
    TEST_SECTION("nvs_load_all_from -- current-version blob with a bad CRC is rejected");

    nvs_stub_reset();
    profile_t src = make_stored_profile();
    profile_persisted_t p;
    p.version = PROFILE_VERSION;
    p.profile = src;
    p.crc32 = compute_profile_crc(&p) ^ 0x1u; /* one bit off */
    stage_profile_blob(0, &p, sizeof(p));
    stage_bitmap(0x01);

    profiles_state_t out;
    bool any_found = false;
    esp_err_t err = nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(!profiles_slot_bitmap_test(&out.used_bitmap, 0), "a CRC mismatch must be rejected");
    profile_t zero;
    memset(&zero, 0, sizeof(zero));
    TEST_CHECK(memcmp(&out.profiles[0], &zero, sizeof(zero)) == 0, "nothing may leak through from a CRC-rejected blob");
}

// ---------------------------------------------------------------------------
// Test 5 -- a blob claiming a NEWER version than this build is refused
// without wiping it. Exercised through profiles_http_start() itself (not
// just nvs_load_all_from() in isolation), same convention
// test_zones_http.c's FIX-1 test uses for zones_http_start(): the raw bytes
// staged in the NVS stub must survive byte-for-byte, proving neither the
// load path nor any migration path wrote to that slot.
// ---------------------------------------------------------------------------
static void test_newer_version_refused_not_wiped(void)
{
    TEST_SECTION("profiles_http_start -- a newer-than-firmware blob is refused, and its bytes survive untouched");

    nvs_stub_reset();
    profile_t src = make_stored_profile();
    profile_persisted_t p;
    p.version = (uint8_t)(PROFILE_VERSION + 1);
    p.profile = src;
    p.crc32 = 0xDEADBEEFu; /* irrelevant -- must be refused on version alone, before CRC is even checked */
    stage_profile_blob(0, &p, sizeof(p));
    stage_bitmap(0x01);

    uint8_t blob_before[sizeof(p)];
    memcpy(blob_before, &p, sizeof(p));

    memset(&s_profiles, 0, sizeof(s_profiles));
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0xFF); /* deliberately wrong, so a no-op bug can't accidentally read as a pass */

    (void)profiles_http_start(); /* returns ESP_ERR_INVALID_STATE (no HTTP server in this stub) AFTER the
                                  * NVS load logic below has already run -- exactly what's under test. */

    TEST_CHECK(!profiles_slot_bitmap_test(&s_profiles.used_bitmap, 0), "a refused newer-version blob must not be reported used");

    nvs_stub_entry_t *e = nvs_stub_find(PROFILES_NVS_PARTITION, NVS_NAMESPACE, "prof0", false);
    TEST_CHECK(e != NULL && e->has_blob, "the staged slot must still exist in NVS");
    if (e && e->has_blob) {
        TEST_CHECK(e->blob_len == sizeof(p) && memcmp(e->blob, blob_before, sizeof(p)) == 0,
                  "the on-flash bytes for a refused newer-version blob must be byte-for-byte unchanged -- "
                  "refusing to load must never mean wiping");
    }
}

// ---------------------------------------------------------------------------
// Test 6 -- one bad slot does not take out the others (TODO.md 8.1).
// ---------------------------------------------------------------------------
static void test_one_bad_slot_does_not_affect_others(void)
{
    TEST_SECTION("nvs_load_all_from -- one corrupt slot does not take any other slot down with it");

    nvs_stub_reset();
    profile_t good0 = make_stored_profile();
    profile_persisted_t p0;
    p0.version = PROFILE_VERSION;
    p0.profile = good0;
    p0.crc32 = compute_profile_crc(&p0);
    stage_profile_blob(0, &p0, sizeof(p0));

    profile_t good2;
    memset(&good2, 0, sizeof(good2));
    strncpy(good2.name, "Bisque", PROFILE_NAME_MAX_LEN);
    good2.zone_mask = 0x01;
    good2.segment_count = 1;
    good2.segments[0].target_c = 999.0f;
    good2.segments[0].ramp_c_per_hr = 80.0f;
    good2.segments[0].dwell_min = 15;
    profile_persisted_t p2;
    p2.version = PROFILE_VERSION;
    p2.profile = good2;
    p2.crc32 = compute_profile_crc(&p2);
    stage_profile_blob(2, &p2, sizeof(p2));

    /* Slot 1: bad CRC -- the one that must fail without affecting 0 or 2. */
    profile_t bad1 = make_stored_profile();
    profile_persisted_t p1;
    p1.version = PROFILE_VERSION;
    p1.profile = bad1;
    p1.crc32 = compute_profile_crc(&p1) ^ 0xFFu;
    stage_profile_blob(1, &p1, sizeof(p1));

    stage_bitmap(0x01 | 0x02 | 0x04); /* slots 0, 1, 2 */

    profiles_state_t out;
    bool any_found = false;
    esp_err_t err = nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(profiles_slot_bitmap_test(&out.used_bitmap, 0), "slot 0 (good) must still be used");
    TEST_CHECK(!profiles_slot_bitmap_test(&out.used_bitmap, 1), "slot 1 (bad CRC) must be marked unused");
    TEST_CHECK(profiles_slot_bitmap_test(&out.used_bitmap, 2), "slot 2 (good) must still be used, unaffected by slot 1's corruption");
    assert_profiles_equal(&out.profiles[0], &good0, "slot 0");
    assert_profiles_equal(&out.profiles[2], &good2, "slot 2");
}

// ---------------------------------------------------------------------------
// Test 7 -- GET /api/profiles JSON stays valid with worst-case escape-heavy
// names at a full slot count (all 8 slots used, every name built entirely
// from '"' and '\\' -- profiles_http_json_escape()'s two double-cost characters).
// ---------------------------------------------------------------------------

/* Very small brace/bracket/string balance checker -- enough to catch a
 * malformed or truncated JSON document (mismatched braces, an unterminated
 * string, a stray unescaped quote) without pulling in a JSON library. */
static bool json_is_well_formed(const char *s)
{
    int depth = 0;
    bool in_string = false;
    bool escape = false;
    for (const char *p = s; *p; p++) {
        if (in_string) {
            if (escape) {
                escape = false;
            } else if (*p == '\\') {
                escape = true;
            } else if (*p == '"') {
                in_string = false;
            }
            continue;
        }
        if (*p == '"') {
            in_string = true;
        } else if (*p == '{' || *p == '[') {
            depth++;
        } else if (*p == '}' || *p == ']') {
            depth--;
            if (depth < 0) return false;
        }
    }
    return depth == 0 && !in_string;
}

static int count_occurrences(const char *haystack, const char *needle)
{
    int n = 0;
    const char *p = haystack;
    size_t nlen = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += nlen;
    }
    return n;
}

static void test_profiles_list_json_valid_with_escape_heavy_names(void)
{
    TEST_SECTION("profiles_list_get_handler -- worst-case escape-heavy names at 8/8 slots stay valid JSON");

    nvs_stub_reset();
    memset(&s_profiles, 0, sizeof(s_profiles));
    for (uint8_t id = 0; id < PROFILES_MAX_COUNT; id++) {
        profile_t *p = &s_profiles.profiles[id];
        memset(p, 0, sizeof(*p));
        /* PROFILE_NAME_MAX_LEN (15) characters, alternating '"' and '\\' --
         * profiles_http_json_escape() doubles every one of them, the worst case
         * PROFILE_LIST_ENTRY_MAX's own comment sizes against. */
        for (int i = 0; i < PROFILE_NAME_MAX_LEN; i++) {
            p->name[i] = (i % 2 == 0) ? '"' : '\\';
        }
        p->name[PROFILE_NAME_MAX_LEN] = '\0';
        p->zone_mask = 0xFF;
        p->segment_count = 1;
        p->segments[0].target_c = 100.0f;
        p->segments[0].ramp_c_per_hr = 50.0f;
        p->segments[0].dwell_min = 5;
        profiles_slot_bitmap_set(&s_profiles.used_bitmap, id);
    }

    s_chunk_capture_len = 0;
    s_chunk_capture[0] = '\0';
    s_chunk_capture_on = true;
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = profiles_list_get_handler(&req);
    s_chunk_capture_on = false;

    TEST_CHECK(err == ESP_OK, "handler must not report an error for a full, escape-heavy slot set");
    TEST_CHECK(s_chunk_capture_len > 0, "some JSON must have been sent");
    TEST_CHECK(json_is_well_formed(s_chunk_capture), "the response must be syntactically well-formed JSON "
                                                      "(balanced braces/brackets, no unterminated string) "
                                                      "even with every name fully escaping");
    TEST_CHECK(strstr(s_chunk_capture, "omitted -- listing too large") == NULL,
              "8 slots of worst-case names must fit PROFILE_LIST_ENTRY_MAX's budget -- if this fires, the "
              "budget in profiles_http.c is undersized again");
    TEST_CHECK(count_occurrences(s_chunk_capture, "\"builtin\":false") == PROFILES_MAX_COUNT,
              "all 8 user-slot entries must be present in the listing, none dropped");
}

static void test_profiles_list_floors_unknown_header_covers_free_slots(void)
{
    TEST_SECTION("profiles_list_get_handler -- unknown rev floors on a FREE slot raise X-Profile-Floors-Unknown");

    nvs_stub_reset();
    memset(&s_profiles, 0, sizeof(s_profiles));
    memset(s_profile_rev_unknown, 0, sizeof(s_profile_rev_unknown));
    profile_t *p = &s_profiles.profiles[0];
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "One");
    p->zone_mask = 1;
    p->segment_count = 1;
    profiles_slot_bitmap_set(&s_profiles.used_bitmap, 0);
    httpd_req_t req;
    memset(&req, 0, sizeof(req));

    s_floors_unknown_hdr_set = false;
    TEST_CHECK(profiles_list_get_handler(&req) == ESP_OK, "list ok");
    TEST_CHECK(!s_floors_unknown_hdr_set, "known floors: no header");

    s_profile_rev_unknown[0] = true; /* only the USED slot is unknown: the per-entry flag already covers it */
    s_floors_unknown_hdr_set = false;
    (void)profiles_list_get_handler(&req);
    TEST_CHECK(!s_floors_unknown_hdr_set, "a used-only unknown slot does not raise the header");

    s_profile_rev_unknown[3] = true; /* a free slot: nothing listed shows it, so the header must */
    s_floors_unknown_hdr_set = false;
    (void)profiles_list_get_handler(&req);
    TEST_CHECK(s_floors_unknown_hdr_set, "a free rev-unknown slot raises X-Profile-Floors-Unknown");
    memset(s_profile_rev_unknown, 0, sizeof(s_profile_rev_unknown));
}

// Opus review pass (docs/ON_OFF_ZONE.md step 5b) -- profile_detail_
// get_handler()'s PROFILE_DETAIL_JSON_CAP budgeted PROFILE_MAX_ON_OFF_RULES
// at 128 bytes/rule, but a rule object with a real temp_source key measures
// 182 bytes worst case; combined with 12 worst-case segments and a 512-byte
// escaped ceiling_note this could overflow the 128-byte-per-rule budget and
// silently truncate the response -- profiles_catalog_http.c's APPEND macro
// `goto send;`s on overflow rather than erroring, so the handler would still
// return ESP_OK/200 with an incomplete JSON body. Renders one profile at
// every worst-case dimension at once (12 segments, 8 rules with maximal
// field values, a triggered 512-byte-worst-case ceiling_note) and asserts
// the response is complete, syntactically valid JSON with every segment and
// rule present -- the fix (128 -> 224) is verified here, not just at the
// #define.
static void test_profile_detail_json_valid_at_max_capacity(void)
{
    TEST_SECTION("profile_detail_get_handler -- a max-capacity profile (12 segments, 8 on/off "
                 "rules, a triggered ceiling_note) renders complete, valid JSON");

    nvs_stub_reset();
    memset(&s_profiles, 0, sizeof(s_profiles));
    memset(g_stub_zone_max_temp_c, 0, sizeof(g_stub_zone_max_temp_c));

    const uint8_t id = 0;
    profile_t *p = &s_profiles.profiles[id];
    memset(p, 0, sizeof(*p));
    for (int i = 0; i < PROFILE_NAME_MAX_LEN; i++) {
        p->name[i] = (i % 2 == 0) ? '"' : '\\'; /* worst-case escape load, same as the list test */
    }
    p->name[PROFILE_NAME_MAX_LEN] = '\0';
    p->zone_mask = 0xFF;
    p->segment_count = PROFILE_MAX_SEGMENTS;
    for (uint8_t i = 0; i < PROFILE_MAX_SEGMENTS; i++) {
        profile_segment_t *seg = &p->segments[i];
        seg->seg_kind = PROFILE_SEG_KIND_ZONE_RAMP; /* must stay ZONE_RAMP so
                                 profile_exceeds_zone_ceiling() below still evaluates this
                                 segment -- io_target/io_state/io_blocking/io_leave_on_at_end
                                 are still maxed below even though they're not meaningful for
                                 this seg_kind, since the JSON renderer emits them regardless
                                 of validity and this test is after worst-case byte width. */
        seg->target_c = 99999.99f; /* zone 0's max_temp_c below (10C) makes segment 0 trip the
                                     ceiling_note check (must stay POSITIVE and > 10.0 for
                                     profile_exceeds_zone_ceiling()'s target > zone_max_c test
                                     to fire); every segment shares this large-magnitude value
                                     so none of them are cheap to render either. */
        seg->ramp_c_per_hr = -99999.99f;
        seg->dwell_min = 4294967295u; /* UINT32_MAX: 10 digits, the true worst case for %lu */
        seg->io_target = 255;
        seg->io_state = 255;
        seg->io_blocking = 255;
        seg->io_leave_on_at_end = 255;
    }
    g_stub_zone_max_temp_c[0] = 10.0f; /* triggers profile_exceeds_zone_ceiling()'s note */

    p->on_off_rule_count = PROFILE_MAX_ON_OFF_RULES;
    for (uint8_t i = 0; i < PROFILE_MAX_ON_OFF_RULES; i++) {
        profile_on_off_rule_t *r = &p->on_off_rules[i];
        r->segment_index = PROFILE_MAX_SEGMENTS - 1;
        r->zone_index = 255;
        r->enable = 1;
        r->phase_mask = 0xFF;
        r->direction_mask = 0xFF;
        r->temp_source = 255; /* JSON renderer prints this field unconditionally, so byte-width
                                  worst case uses the uint8_t's full range, not just the one
                                  value (1) that means "live" per profile_executor.c */
        r->temp_ref_zone = 255;
        r->temp_cmp = 255;
        r->temp_threshold_c = -99999.99f;
        r->time_start_s = 65535;
        r->time_stop_s = 65535;
        r->invert = 1;
    }
    profiles_slot_bitmap_set(&s_profiles.used_bitmap, id);

    s_stub_query_str = "id=0";
    s_send_capture_len = 0;
    s_send_capture[0] = '\0';
    s_send_capture_on = true;
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = profile_detail_get_handler(&req);
    s_send_capture_on = false;
    s_stub_query_str = NULL;

    TEST_CHECK(err == ESP_OK, "the handler must not report a transport error");
    TEST_CHECK(s_send_capture_len > 0, "some JSON must have been sent");
    TEST_CHECK(json_is_well_formed(s_send_capture),
              "the response must be syntactically well-formed JSON (balanced braces/brackets, "
              "no unterminated string) at every worst-case dimension at once -- if this fires, "
              "PROFILE_DETAIL_JSON_CAP is undersized again");
    TEST_CHECK(count_occurrences(s_send_capture, "\"seg_kind\":") == PROFILE_MAX_SEGMENTS,
              "all 12 segments must be present, not silently dropped by a truncation that "
              "still parses as valid JSON up to the cut point");
    TEST_CHECK(count_occurrences(s_send_capture, "\"zone\":255") == PROFILE_MAX_ON_OFF_RULES,
              "all 8 on/off rules must be present");
    TEST_CHECK(strstr(s_send_capture, "\"exceeds_ceiling\":true") != NULL,
              "the triggered ceiling_note path must have actually run (otherwise this test "
              "isn't exercising the 512-byte escaped ceiling_note worst case at all)");
    size_t len = strlen(s_send_capture);
    TEST_CHECK(len >= 2 && s_send_capture[len - 1] == '}' && s_send_capture[len - 2] == ']',
              "the response must end with the closing \"]}\" of on_off_rules/the outer object, "
              "not be cut off mid-array/mid-object");
}

static void test_profiles_list_reports_rev_unknown(void)
{
    TEST_SECTION("profiles_list_get_handler -- rev_unknown per slot surfaces the degraded state (stack review S2)");
    nvs_stub_reset();
    memset(&s_profiles, 0, sizeof(s_profiles));
    for (int i = 0; i < 2; i++) {
        profile_t *p = &s_profiles.profiles[i];
        memset(p, 0, sizeof(*p));
        strncpy(p->name, i ? "bad" : "good", PROFILE_NAME_MAX_LEN);
        p->zone_mask = 0x01;
        p->segment_count = 1;
        p->segments[0].target_c = 100.0f;
        p->segments[0].ramp_c_per_hr = 50.0f;
        p->segments[0].dwell_min = 5;
        profiles_slot_bitmap_set(&s_profiles.used_bitmap, (uint8_t)i);
    }
    memset(s_profile_rev_unknown, 0, sizeof(s_profile_rev_unknown));
    s_profile_rev_unknown[1] = true;
    s_chunk_capture_len = 0;
    s_chunk_capture[0] = '\0';
    s_chunk_capture_on = true;
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = profiles_list_get_handler(&req);
    s_chunk_capture_on = false;
    memset(s_profile_rev_unknown, 0, sizeof(s_profile_rev_unknown));
    TEST_CHECK(err == ESP_OK, "handler ok");
    const char *g = strstr(s_chunk_capture, "\"name\":\"good\"");
    const char *b = strstr(s_chunk_capture, "\"name\":\"bad\"");
    TEST_CHECK(g && b, "both entries listed");
    TEST_CHECK(g && strstr(g, "\"rev_unknown\":false") != NULL && (!b || strstr(g, "\"rev_unknown\":false") < b),
               "slot 0 rev_unknown:false");
    TEST_CHECK(b && strstr(b, "\"rev_unknown\":true") != NULL, "slot 1 rev_unknown:true");
}

static void test_profiles_list_carries_last_run_started_unix_s(void)
{
    TEST_SECTION("profiles_list_get_handler -- carries last_run_started_unix_s from "
                 "profile_executor_last_run_started_unix_s() (PROFILE_SLOTS_100.md section 7 task 8)");

    nvs_stub_reset();
    memset(&s_profiles, 0, sizeof(s_profiles));
    profile_t *p = &s_profiles.profiles[0];
    memset(p, 0, sizeof(*p));
    strncpy(p->name, "test", PROFILE_NAME_MAX_LEN);
    p->zone_mask = 0x01;
    p->segment_count = 1;
    p->segments[0].target_c = 100.0f;
    p->segments[0].ramp_c_per_hr = 50.0f;
    p->segments[0].dwell_min = 5;
    profiles_slot_bitmap_set(&s_profiles.used_bitmap, 0);

    g_fake_last_run_unix_s = 1726700000u;

    s_chunk_capture_len = 0;
    s_chunk_capture[0] = '\0';
    s_chunk_capture_on = true;
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = profiles_list_get_handler(&req);
    s_chunk_capture_on = false;

    g_fake_last_run_unix_s = 0; /* restore the "never fired" default for later tests */

    TEST_CHECK(err == ESP_OK, "handler must not report an error");
    TEST_CHECK(strstr(s_chunk_capture, "\"last_run_started_unix_s\":1726700000") != NULL,
              "the fake nonzero last-run timestamp must appear verbatim in the list JSON");
}

// ---------------------------------------------------------------------------
// Test 4b -- THE mandatory regression test for THIS pass's own shape change
// (PROFILE_VERSION 2->3, profile_segment_t growing the relay/IO fields): a
// real v2 blob with DISTINCT values in SEVERAL segments -- not just segment
// 0, since profile_t embeds segments[] BY VALUE and the displacement bug
// this guards against only shows from element 1 onward -- loads and every
// segment survives intact, with the four new fields landing on the safe
// "always was a temperature segment" defaults. This is the test that would
// have caught the v1->v2 data-loss bug one version earlier, applied to the
// identical hazard one version later.
// ---------------------------------------------------------------------------
static void test_v2_blob_migrates_distinct_multi_segment_values(void)
{
    TEST_SECTION("nvs_load_all_from -- v2->v3: a v2 blob with distinct values in segments 0-3 migrates losslessly");

    nvs_stub_reset();
    profile_t src = make_stored_profile(); /* already gives segments 0-2 distinct values */
    src.segment_count = 4;
    src.segments[3].target_c = 999.5f;
    src.segments[3].ramp_c_per_hr = 42.0f;
    src.segments[3].dwell_min = 7;

    profile_persisted_v2_t v2;
    memset(&v2, 0, sizeof(v2));
    v2.version = 2;
    v2.profile = to_v2(&src);
    v2.crc32 = esp_crc32_le(0, (const uint8_t *)&v2, sizeof(v2)); /* crc32 field is still 0 here */
    stage_profile_blob(0, &v2, sizeof(v2));
    stage_bitmap(0x01);

    profiles_state_t out;
    bool any_found = false;
    esp_err_t err = nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(profiles_slot_bitmap_test(&out.used_bitmap, 0), "a v2 blob must migrate to a used slot, not be dropped");
    assert_profiles_equal(&out.profiles[0], &src, "v2->v3 migration");
    for (uint8_t i = 0; i < out.profiles[0].segment_count; i++) {
        char msg[112];
        snprintf(msg, sizeof(msg), "segment %u: migrated seg_kind defaults to ZONE_RAMP (never RELAY_IO)", i);
        TEST_CHECK(out.profiles[0].segments[i].seg_kind == PROFILE_SEG_KIND_ZONE_RAMP, msg);
        snprintf(msg, sizeof(msg), "segment %u: migrated io_leave_on_at_end defaults to 0 (fail-off)", i);
        TEST_CHECK(out.profiles[0].segments[i].io_leave_on_at_end == 0, msg);
    }
}

// ---------------------------------------------------------------------------
// Test 4c -- docs/ON_OFF_ZONE.md plan step 5, PROFILE_VERSION 3->4: a
// real v3 blob (current segment shape, crc32 tail, NO on/off rules -- what
// every board saved between the relay/IO pass and this one) migrates and
// gets the documented migration default: on_off_rule_count == 0, every rule
// slot zeroed. This is the "rules-free profile is byte-identical" proof for
// the migration path specifically (assert_profiles_equal's own rule-tail
// assertions, extended by this pass, do the rest for every OTHER migration
// test in this file for free).
// ---------------------------------------------------------------------------
static void test_v3_blob_migrates_with_no_rules(void)
{
    TEST_SECTION("nvs_load_all_from -- v3->v4: a real v3 blob (no rules yet) migrates with on_off_rule_count 0");

    nvs_stub_reset();
    profile_t src = make_stored_profile();

    profile_persisted_v3_t v3;
    memset(&v3, 0, sizeof(v3));
    v3.version = 3;
    v3.profile = to_v3(&src);
    v3.crc32 = esp_crc32_le(0, (const uint8_t *)&v3, sizeof(v3)); /* crc32 field is still 0 here */
    stage_profile_blob(0, &v3, sizeof(v3));
    stage_bitmap(0x01);

    profiles_state_t out;
    bool any_found = false;
    esp_err_t err = nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(profiles_slot_bitmap_test(&out.used_bitmap, 0), "a v3 blob must migrate to a used slot, not be dropped");
    assert_profiles_equal(&out.profiles[0], &src, "v3->v4 migration");
    TEST_CHECK(out.profiles[0].on_off_rule_count == 0,
              "migration default: a v3 blob never had a rule, so on_off_rule_count must land at 0");
}

// ---------------------------------------------------------------------------
// Test 4d -- on/off rule storage round-trips through the REAL production
// save/load path (profiles_http_save() -> nvs_save_slot() ->
// nvs_load_all_from(), the same NVS path plan step 5's task names). Zone 2
// is set to ZONE_TYPE_ON_OFF via the zones_config_get_zone_type() stub so
// validate_on_off_rules() (profiles_http.c) accepts the rule.
// ---------------------------------------------------------------------------
static void test_on_off_rule_round_trip_through_real_save_and_load(void)
{
    TEST_SECTION("profiles_http_save/nvs_load_all_from -- an on/off rule round-trips byte-for-byte through the cfg file");

    nvs_stub_reset();
    memset(g_stub_zone_type, 0, sizeof(g_stub_zone_type)); /* every zone HEATER by default */
    g_stub_zone_type[2] = ZONE_TYPE_ON_OFF;

    profile_t p = make_stored_profile();
    p.on_off_rule_count = 1;
    p.on_off_rules[0].segment_index = 1;
    p.on_off_rules[0].zone_index = 2;
    p.on_off_rules[0].enable = 1;
    p.on_off_rules[0].phase_mask = ON_OFF_PHASE_DWELL;
    p.on_off_rules[0].direction_mask = ON_OFF_DIR_COOLING;
    p.on_off_rules[0].temp_source = 1;
    p.on_off_rules[0].temp_cmp = ON_OFF_TEMP_CMP_ABOVE;
    p.on_off_rules[0].temp_threshold_c = 650.0f;
    p.on_off_rules[0].time_start_s = 5;
    p.on_off_rules[0].time_stop_s = 0;
    p.on_off_rules[0].invert = 0;

    uint8_t out_id = 0, warn_count = 0;
    char err_msg[160] = "";
    bool ok = profiles_http_save(PROFILES_MAX_COUNT, &p, &out_id, &warn_count, err_msg, sizeof(err_msg));
    TEST_CHECK(ok, err_msg[0] ? err_msg : "save with a valid on/off rule must succeed");

    profiles_state_t out;
    bool any_found = false;
    esp_err_t err = nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found);
    TEST_CHECK(err == ESP_OK, "no NVS error reloading the saved slot");
    assert_profiles_equal(&out.profiles[out_id], &p, "on/off rule cfg round-trip");
}

// ---------------------------------------------------------------------------
// Test 4e -- validate_on_off_rules(): reject a rule pointing at a segment
// index that does not exist in the profile.
// ---------------------------------------------------------------------------
static void test_validate_on_off_rules_rejects_bad_segment_index(void)
{
    TEST_SECTION("validate_on_off_rules -- a rule's segment_index must exist in the profile");
    memset(g_stub_zone_type, 0, sizeof(g_stub_zone_type));
    g_stub_zone_type[0] = ZONE_TYPE_ON_OFF;

    profile_t p = make_stored_profile(); /* segment_count == 3, indices 0-2 legal */
    p.on_off_rule_count = 1;
    p.on_off_rules[0].segment_index = 3; /* one past the end */
    p.on_off_rules[0].zone_index = 0;
    p.on_off_rules[0].enable = 1;

    char err_msg[160] = "";
    bool ok = validate_on_off_rules(&p, err_msg, sizeof(err_msg));
    TEST_CHECK(!ok, "a rule referencing a nonexistent segment must be refused");
    TEST_CHECK(strstr(err_msg, "does not exist") != NULL, "the refusal names the reason");
}

// ---------------------------------------------------------------------------
// Test 4f -- validate_on_off_rules(): THE DANGEROUS DIRECTION. A rule
// pointing at a zone that is NOT typed ZONE_TYPE_ON_OFF (default:
// ZONE_TYPE_HEATER, the fixture's default state) must be refused -- letting
// this through would let on/off (bang-bang, no PID, no guards 1-4/9) logic
// drive a real heating element. See this task's report for the negative
// test performed by hand against this exact check.
// ---------------------------------------------------------------------------
static void test_validate_on_off_rules_rejects_heater_zone(void)
{
    TEST_SECTION("validate_on_off_rules -- THE DANGEROUS DIRECTION: a rule aimed at a HEATER zone is refused");
    memset(g_stub_zone_type, 0, sizeof(g_stub_zone_type)); /* zone 0 stays ZONE_TYPE_HEATER (default) */

    profile_t p = make_stored_profile();
    p.on_off_rule_count = 1;
    p.on_off_rules[0].segment_index = 0;
    p.on_off_rules[0].zone_index = 0; /* a HEATER zone */
    p.on_off_rules[0].enable = 1;

    char err_msg[160] = "";
    bool ok = validate_on_off_rules(&p, err_msg, sizeof(err_msg));
    TEST_CHECK(!ok, "a rule aimed at a HEATER zone must be refused -- on/off logic must never drive a heater");
    TEST_CHECK(strstr(err_msg, "not configured as an on/off device") != NULL,
              "the refusal names the reason");
}

// ---------------------------------------------------------------------------
// WP-4 (SPARE_RELAY_ONOFF_PLAN sec 6/14): rule targets 8..11 address aux relay
// 1..4. Valid/invalid matrix, plus the RELAY_IO-on-aux refusal and the
// unchanged zone path.
// ---------------------------------------------------------------------------
static bool aux_rule_ok(uint8_t target, uint8_t temp_source, uint8_t temp_cmp, char *err, size_t cap)
{
    profile_t p = make_stored_profile();
    p.on_off_rule_count = 1;
    memset(&p.on_off_rules[0], 0, sizeof(p.on_off_rules[0]));
    p.on_off_rules[0].segment_index = 0;
    p.on_off_rules[0].zone_index = target;
    p.on_off_rules[0].enable = 1;
    p.on_off_rules[0].temp_source = temp_source;
    p.on_off_rules[0].temp_cmp = temp_cmp;
    p.on_off_rules[0].temp_threshold_c = 100.0f;
    return validate_on_off_rules(&p, err, cap);
}

static void test_validate_on_off_rules_aux_targets(void)
{
    TEST_SECTION("validate_on_off_rules -- aux targets 8..11");
    char err[200] = "";
    memset(g_stub_zone_type, 0, sizeof(g_stub_zone_type));
    memset(g_stub_aux, 0, sizeof(g_stub_aux));

    TEST_CHECK(!aux_rule_ok(11, 0, 0, err, sizeof(err)) && strstr(err, "not an enabled aux output"),
              "target 11 refused while aux relay 4 is disabled");

    g_stub_aux[3].enabled = true; /* relay 4 */
    g_stub_aux[3].tc_zone = AUX_TC_ZONE_NONE;
    TEST_CHECK(aux_rule_ok(11, 0, 0, err, sizeof(err)), "target 11 accepted with aux relay 4 enabled (no temp axis)");
    TEST_CHECK(!aux_rule_ok(8, 0, 0, err, sizeof(err)), "target 8 (relay 1) refused: that aux is disabled");

    TEST_CHECK(!aux_rule_ok(11, 1, ON_OFF_TEMP_CMP_ABOVE, err, sizeof(err)) && strstr(err, "thermocouple zone"),
              "temperature axis refused when the aux has no tc_zone");
    g_stub_aux[3].tc_zone = 1;
    TEST_CHECK(aux_rule_ok(11, 1, ON_OFF_TEMP_CMP_ABOVE, err, sizeof(err)),
              "temperature axis accepted with temp_source 1 and a tc_zone");
    TEST_CHECK(!aux_rule_ok(11, 0, ON_OFF_TEMP_CMP_ABOVE, err, sizeof(err)) && strstr(err, "temp_source 1"),
              "temperature axis with temp_source 0 refused with the temp_source 1 text (would be silently ignored)");
    TEST_CHECK(!aux_rule_ok(11, 2, 0, err, sizeof(err)) && strstr(err, "reserved"),
              "temp_source 2 reserved for aux targets");

    g_stub_aux[3].conflicted = true;
    g_stub_aux[3].enabled = false; /* forced disabled by a zone conflict */
    TEST_CHECK(!aux_rule_ok(11, 0, 0, err, sizeof(err)) && strstr(err, "conflicted") &&
                   !strstr(err, "not an enabled"),
              "a conflicted (forced-disabled) aux is refused with its own 'conflicted' text");
    g_stub_aux[3].conflicted = false;
    TEST_CHECK(!aux_rule_ok(11, 0, 0, err, sizeof(err)) && strstr(err, "not an enabled") &&
                   !strstr(err, "conflicted"),
              "a plainly disabled aux is refused with the 'not an enabled' text, distinct from conflicted");

    for (unsigned t = 3; t <= 7; t++) {
        TEST_CHECK(!aux_rule_ok((uint8_t)t, 0, 0, err, sizeof(err)) && strstr(err, "out of range"),
                  "targets 3..7 are refused as out of range");
    }
    TEST_CHECK(!aux_rule_ok(12, 0, 0, err, sizeof(err)) && strstr(err, "out of range"), "target 12 refused");
    TEST_CHECK(!aux_rule_ok(255, 0, 0, err, sizeof(err)), "target 255 refused");

    /* zone path unchanged: heater refused, on/off zone accepted */
    TEST_CHECK(!aux_rule_ok(1, 0, 0, err, sizeof(err)), "zone 1 (HEATER) still refused");
    g_stub_zone_type[1] = ZONE_TYPE_ON_OFF;
    TEST_CHECK(aux_rule_ok(1, 0, 0, err, sizeof(err)), "zone 1 typed ON_OFF still accepted");
    memset(g_stub_zone_type, 0, sizeof(g_stub_zone_type));
    memset(g_stub_aux, 0, sizeof(g_stub_aux));
}

// ---------------------------------------------------------------------------
// Zone -> aux rule retarget (docs/SPARE_RELAY_ONOFF_PLAN.md section 10):
// profiles_retarget_zone_to_aux_{plan,commit,revert}.
// ---------------------------------------------------------------------------
#define RT_ZONE 1
#define RT_RELAY 3
#define RT_DEST 10 /* 8 + (relay - 1) */

static void rt_rule(profile_t *p, uint8_t idx, uint8_t zone, uint8_t seg, uint8_t temp_source, uint8_t temp_cmp)
{
    memset(&p->on_off_rules[idx], 0, sizeof(p->on_off_rules[idx]));
    p->on_off_rules[idx].segment_index = seg;
    p->on_off_rules[idx].zone_index = zone;
    p->on_off_rules[idx].enable = 1;
    p->on_off_rules[idx].temp_source = temp_source;
    p->on_off_rules[idx].temp_cmp = temp_cmp;
    p->on_off_rules[idx].temp_threshold_c = 300.0f;
    if (idx + 1u > p->on_off_rule_count) {
        p->on_off_rule_count = (uint8_t)(idx + 1u);
    }
}

/* Fresh store: slot 0 = two rules on zone 1 (one with a temperature compare), slot 1 = a rule on zone 2 only,
 * slot 2 = one rule on zone 1, slot 3 = no rules. Zones 1 and 2 are ON_OFF while saving. */
static void rt_seed(void)
{
    nvs_stub_reset();
    memset(&s_profiles, 0, sizeof(s_profiles));
    memset(g_stub_zone_type, 0, sizeof(g_stub_zone_type));
    memset(g_stub_aux, 0, sizeof(g_stub_aux));
    g_stub_zone_type[1] = ZONE_TYPE_ON_OFF;
    g_stub_zone_type[2] = ZONE_TYPE_ON_OFF;
    g_fake_exec_state = PROFILE_EXEC_IDLE;
    g_fake_exec_profile_id = 0xFF;
    char err[160];
    uint8_t out_id = 0, warn = 0;
    for (uint8_t id = 0; id < 4; id++) {
        profile_t p = make_stored_profile();
        snprintf(p.name, sizeof(p.name), "RtProf%u", id);
        if (id == 0) {
            rt_rule(&p, 0, RT_ZONE, 0, 0, 0);
            rt_rule(&p, 1, RT_ZONE, 1, 1, ON_OFF_TEMP_CMP_ABOVE);
        } else if (id == 1) {
            rt_rule(&p, 0, 2, 0, 0, 0);
        } else if (id == 2) {
            rt_rule(&p, 0, RT_ZONE, 2, 0, 0);
        }
        err[0] = '\0';
        bool ok = profiles_http_save(id, &p, &out_id, &warn, err, sizeof(err));
        TEST_CHECK(ok && out_id == id, err[0] ? err : "retarget fixture profile saves");
    }
    /* The caller frees the zone and enables the aux before commit; mimic that. */
    g_stub_zone_type[1] = ZONE_TYPE_HEATER;
    g_stub_aux[RT_RELAY - 1].enabled = true;
    g_stub_aux[RT_RELAY - 1].tc_zone = RT_ZONE;
}

static void rt_snapshot(profile_t out[4])
{
    for (uint8_t id = 0; id < 4; id++) {
        out[id] = s_profiles.profiles[id];
    }
}

static bool rt_persisted_matches_ram(void)
{
    static profiles_state_t loaded;
    bool any = false;
    if (nvs_load_all_from(PROFILES_NVS_PARTITION, &loaded, &any) != ESP_OK || !any) {
        return false;
    }
    for (uint8_t id = 0; id < 4; id++) {
        const profile_t *a = &loaded.profiles[id];
        const profile_t *b = &s_profiles.profiles[id];
        if (a->on_off_rule_count != b->on_off_rule_count) {
            return false;
        }
        for (uint8_t i = 0; i < b->on_off_rule_count; i++) {
            if (a->on_off_rules[i].zone_index != b->on_off_rules[i].zone_index ||
                a->on_off_rules[i].segment_index != b->on_off_rules[i].segment_index) {
                return false;
            }
        }
    }
    return true;
}

static bool rt_unchanged_from(const profile_t snap[4])
{
    for (uint8_t id = 0; id < 4; id++) {
        if (snap[id].on_off_rule_count != s_profiles.profiles[id].on_off_rule_count ||
            memcmp(snap[id].on_off_rules, s_profiles.profiles[id].on_off_rules, sizeof(snap[id].on_off_rules)) != 0) {
            return false;
        }
    }
    return true;
}

static void test_retarget_commit_success(void)
{
    TEST_SECTION("profiles_retarget_zone_to_aux_commit -- rewrites every rule for the zone, nothing else");
    rt_seed();
    profile_t before[4];
    rt_snapshot(before);
    profiles_retarget_counts_t c;
    char err[160] = "";
    TEST_CHECK(profiles_retarget_zone_to_aux_commit(RT_ZONE, RT_RELAY, true, &c, err, sizeof(err)), err);
    TEST_CHECK(c.profiles_scanned == 4 && c.profiles_affected == 2 && c.rules_retargeted == 3,
               "counts: 4 scanned, 2 affected, 3 rules");
    const profile_t *p0 = &s_profiles.profiles[0];
    TEST_CHECK(p0->on_off_rules[0].zone_index == RT_DEST && p0->on_off_rules[1].zone_index == RT_DEST,
               "slot 0: both rules now target the aux");
    TEST_CHECK(p0->on_off_rules[1].segment_index == 1 && p0->on_off_rules[1].temp_source == 1 &&
                   p0->on_off_rules[1].temp_cmp == ON_OFF_TEMP_CMP_ABOVE &&
                   p0->on_off_rules[1].temp_threshold_c == 300.0f,
               "slot 0: every other rule field untouched");
    TEST_CHECK(s_profiles.profiles[2].on_off_rules[0].zone_index == RT_DEST, "slot 2 retargeted");
    TEST_CHECK(s_profiles.profiles[1].on_off_rules[0].zone_index == 2 &&
                   memcmp(&s_profiles.profiles[1], &before[1], sizeof(profile_t)) == 0,
               "slot 1 (other zone) byte-identical");
    TEST_CHECK(s_profiles.profiles[3].on_off_rule_count == 0, "slot 3 (no rules) untouched");
    TEST_CHECK(rt_persisted_matches_ram(), "persisted profiles match RAM after the commit");

    TEST_CHECK(profiles_retarget_zone_to_aux_revert(RT_ZONE, RT_RELAY), "revert reports clean");
    TEST_CHECK(rt_unchanged_from(before) && rt_persisted_matches_ram(), "revert restores RAM and the persisted profiles to the originals");
}

static void test_retarget_plan_refusals(void)
{
    TEST_SECTION("profiles_retarget_zone_to_aux_plan -- refusals change nothing");
    profiles_retarget_counts_t c;
    char err[160];
    profile_t before[4];

    rt_seed();
    rt_snapshot(before);
    TEST_CHECK(profiles_retarget_zone_to_aux_plan(RT_ZONE, RT_RELAY, true, &c, err, sizeof(err)) &&
                   c.profiles_affected == 2 && c.rules_retargeted == 3 && rt_unchanged_from(before),
               "plan is read-only and counts match");

    TEST_CHECK(!profiles_retarget_zone_to_aux_plan(RT_ZONE, 5, true, &c, err, sizeof(err)), "relay out of aux range refused");
    TEST_CHECK(!profiles_retarget_zone_to_aux_plan(3, RT_RELAY, true, &c, err, sizeof(err)), "zone out of range refused");

    rt_seed();
    rt_rule(&s_profiles.profiles[3], 0, RT_DEST, 0, 0, 0);
    TEST_CHECK(!profiles_retarget_zone_to_aux_plan(RT_ZONE, RT_RELAY, true, &c, err, sizeof(err)) &&
                   strstr(err, "already has a rule"),
               "a rule already at the aux target refuses");

    rt_seed();
    s_profiles.profiles[0].on_off_rules[0].temp_source = 2;
    TEST_CHECK(!profiles_retarget_zone_to_aux_plan(RT_ZONE, RT_RELAY, true, &c, err, sizeof(err)) &&
                   strstr(err, "temp_source"),
               "temp_source 2 refuses");

    rt_seed();
    TEST_CHECK(!profiles_retarget_zone_to_aux_plan(RT_ZONE, RT_RELAY, false, &c, err, sizeof(err)) &&
                   strstr(err, "thermocouple"),
               "temperature compare with no zone TC refuses");

    rt_seed();
    g_fake_exec_state = PROFILE_EXEC_RUNNING;
    g_fake_exec_profile_id = 2;
    TEST_CHECK(!profiles_retarget_zone_to_aux_plan(RT_ZONE, RT_RELAY, true, &c, err, sizeof(err)) &&
                   strstr(err, "running or paused"),
               "running slot that uses the zone refuses");
    g_fake_exec_state = PROFILE_EXEC_PAUSED;
    TEST_CHECK(!profiles_retarget_zone_to_aux_plan(RT_ZONE, RT_RELAY, true, &c, err, sizeof(err)), "paused slot refuses too");
    g_fake_exec_profile_id = 1; /* active slot does not use the zone */
    TEST_CHECK(profiles_retarget_zone_to_aux_plan(RT_ZONE, RT_RELAY, true, &c, err, sizeof(err)),
               "an active slot that does not use the zone does not block");
    g_fake_exec_state = PROFILE_EXEC_IDLE;
    g_fake_exec_profile_id = 0xFF;

    /* commit repeats the plan, so a refusal there writes nothing */
    rt_seed();
    s_profiles.profiles[0].on_off_rules[0].temp_source = 2;
    rt_snapshot(before);
    TEST_CHECK(!profiles_retarget_zone_to_aux_commit(RT_ZONE, RT_RELAY, true, &c, err, sizeof(err)) &&
                   rt_unchanged_from(before),
               "commit refused by the plan leaves every slot as it was");
}

/* Lets the first s_rt_writes_left cfg writes through and fails the one after,
 * once (fake_kv_script_write_status_after()'s semantics), so the revert's own
 * saves run for real. Profile saves are cfg-file-only since the NVS
 * dual-write close, so the failure is injected at the cfg write seam: an NVS
 * (fake_kv) script would never be consumed by a save. */
static unsigned s_rt_writes_left;
static bool s_rt_fail_armed;
static esp_err_t rt_fail_after_cfg_write_fn(const char *rel_path, const void *data, size_t len)
{
    if (s_rt_fail_armed) {
        if (s_rt_writes_left == 0) {
            s_rt_fail_armed = false;
            return ESP_FAIL;
        }
        s_rt_writes_left--;
    }
    return cfg_fs_write_atomic(rel_path, data, len);
}

static void test_retarget_commit_rollback_at_every_write(void)
{
    TEST_SECTION("profiles_retarget_zone_to_aux_commit -- a write failure at ANY point leaves nothing half-done");
    int failures_seen = 0;
    bool succeeded = false;
    for (unsigned skip = 0; skip < 24 && !succeeded; skip++) {
        rt_seed();
        profile_t before[4];
        rt_snapshot(before);
        profiles_retarget_counts_t c;
        char err[160] = "";
        s_rt_writes_left = skip;
        s_rt_fail_armed = true;
        profiles_cfg_fs_set_write_fn(rt_fail_after_cfg_write_fn);
        bool ok = profiles_retarget_zone_to_aux_commit(RT_ZONE, RT_RELAY, true, &c, err, sizeof(err));
        profiles_cfg_fs_reset_write_fn_for_test();
        s_rt_fail_armed = false;
        if (ok) {
            succeeded = true;
            TEST_CHECK(rt_persisted_matches_ram(), "a commit that outlived the injected failure point is fully persisted");
            continue;
        }
        failures_seen++;
        char label[96];
        snprintf(label, sizeof(label), "failure after %u writes: RAM rules restored", skip);
        TEST_CHECK(rt_unchanged_from(before), label);
        snprintf(label, sizeof(label), "failure after %u writes: persisted profiles match RAM, reported restored", skip);
        TEST_CHECK(rt_persisted_matches_ram() && strstr(err, "all profiles restored") != NULL, label);
    }
    TEST_CHECK(failures_seen >= 2, "failure injection actually hit the commit at several points");
    TEST_CHECK(succeeded, "the sweep reached a point past every write");
}

static int s_rv_min_depth;
static unsigned s_rv_writes;
static unsigned s_rv_fail_at;
static esp_err_t rv_write_fn(const char *rel_path, const void *data, size_t len)
{
    if (s_rv_writes == 0 || g_test_stub_lock_depth < s_rv_min_depth) {
        s_rv_min_depth = g_test_stub_lock_depth;
    }
    unsigned i = s_rv_writes++;
    if (s_rv_fail_at != 0 && i + 1 == s_rv_fail_at) {
        return ESP_FAIL;
    }
    return cfg_fs_write_atomic(rel_path, data, len);
}

static void test_retarget_commit_and_revert_hold_save_lock(void)
{
    TEST_SECTION("retarget commit and revert -- every cfg write runs with the save mutex held");
    int pass;
    for (pass = 0; pass < 2; pass++) {
        rt_seed();
        profiles_retarget_counts_t c;
        char err[160] = "";
        s_rv_writes = 0;
        s_rv_min_depth = 0;
        /* pass 0: clean commit. pass 1: 2nd write fails, so the revert writes too. */
        s_rv_fail_at = (pass == 0) ? 0 : 2;
        profiles_cfg_fs_set_write_fn(rv_write_fn);
        bool ok = profiles_retarget_zone_to_aux_commit(RT_ZONE, RT_RELAY, true, &c, err, sizeof(err));
        profiles_cfg_fs_reset_write_fn_for_test();
        TEST_CHECK(ok == (pass == 0), pass == 0 ? "clean commit succeeds" : "injected failure fails the commit");
        TEST_CHECK(s_rv_writes >= (pass == 0 ? 2u : 3u), pass == 0 ? "commit wrote slots" : "revert wrote after the failure");
        TEST_CHECK(s_rv_min_depth > 0, pass == 0 ? "commit writes ran under the lock" : "commit and revert writes ran under the lock");
        TEST_CHECK(g_test_stub_lock_depth == 0, "lock released");
    }
}

static void test_retarget_commit_rechecks_plan_refusals(void)
{
    TEST_SECTION("retarget commit -- plan refusals are re-checked at commit: a slot changed after a passing plan makes the commit refuse");
    rt_seed();
    profiles_retarget_counts_t c;
    char err[160] = "";
    TEST_CHECK(profiles_retarget_zone_to_aux_plan(RT_ZONE, RT_RELAY, true, &c, err, sizeof(err)), "plan passes");
    rt_rule(&s_profiles.profiles[3], 0, RT_DEST, 0, 0, 0); /* changed between plan and commit */
    profile_t before[4];
    rt_snapshot(before);
    TEST_CHECK(!profiles_retarget_zone_to_aux_commit(RT_ZONE, RT_RELAY, true, &c, err, sizeof(err)) &&
                   strstr(err, "already has a rule") != NULL,
               "commit refuses on the changed slot");
    TEST_CHECK(rt_unchanged_from(before), "refused commit changed no slot");
    TEST_CHECK(g_test_stub_lock_depth == 0, "lock released after the refusal");
}

static void test_save_revalidates_under_lock(void)
{
    TEST_SECTION("profiles_http_save -- a rule target that became invalid between pre-validate and the lock is refused; busy flag refuses at once");
    rt_seed();
    profile_t p = make_stored_profile();
    snprintf(p.name, sizeof(p.name), "RevalProf");
    rt_rule(&p, 0, RT_DEST, 0, 0, 0); /* aux relay is enabled by rt_seed */
    char err[160] = "";
    uint8_t out_id = 0, warn = 0;
    g_aux_flip_relay = RT_RELAY;
    g_aux_flip_under_lock_armed = true;
    bool ok = profiles_http_save(0xFF, &p, &out_id, &warn, err, sizeof(err));
    g_aux_flip_under_lock_armed = false;
    TEST_CHECK(!ok, "save refused when the aux turned invalid before the assign");
    TEST_CHECK(err[0] != '\0', "a validation error is reported");
    int used = 0;
    for (uint8_t i = 0; i < PROFILES_MAX_COUNT; i++) {
        if (profiles_slot_used(i) && strcmp(s_profiles.profiles[i].name, "RevalProf") == 0) used++;
    }
    TEST_CHECK(used == 0, "nothing was assigned");
    TEST_CHECK(g_test_stub_lock_depth == 0, "lock released");

    g_stub_aux[RT_RELAY - 1].enabled = true;
    profiles_http_set_convert_busy(true);
    err[0] = '\0';
    ok = profiles_http_save(0xFF, &p, &out_id, &warn, err, sizeof(err));
    profiles_http_set_convert_busy(false);
    TEST_CHECK(!ok && strstr(err, "busy") != NULL, "save refuses with a busy error while a convert is running");
    TEST_CHECK(profiles_http_save(0xFF, &p, &out_id, &warn, err, sizeof(err)), "save works again once the flag is lowered");
}

static void test_retarget_resume_and_whole_blob_verify(void)
{
    TEST_SECTION("profiles_retarget_zone_to_aux_resume -- finishes a half-done rewrite; slot verify compares the whole blob");
    rt_seed();
    profiles_retarget_counts_t c;
    char err[160] = "";
    TEST_CHECK(profiles_retarget_zone_to_aux_commit(RT_ZONE, RT_RELAY, true, &c, err, sizeof(err)), err);
    TEST_CHECK(retarget_verify_slot(0), "whole-slot verify passes right after a commit");
    s_profiles.profiles[0].name[0] = (char)(s_profiles.profiles[0].name[0] ^ 0x01);
    TEST_CHECK(!retarget_verify_slot(0), "a difference OUTSIDE the rules (the name) now fails the verify");
    s_profiles.profiles[0].name[0] = (char)(s_profiles.profiles[0].name[0] ^ 0x01);
    TEST_CHECK(retarget_verify_slot(0), "restoring the byte passes again");

    /* Simulate a power loss after slot 0 was rewritten but before slot 2 was: put slot 2 back. */
    (void)retarget_swap_rules(&s_profiles.profiles[2], RT_DEST, RT_ZONE);
    TEST_CHECK(nvs_save_slot(2) == ESP_OK, "slot 2 put back to the original");
    TEST_CHECK(!profiles_retarget_zone_to_aux_plan(RT_ZONE, RT_RELAY, true, &c, err, sizeof(err)),
               "a fresh plan refuses: slot 0 already has a rule at the destination");
    TEST_CHECK(!profiles_retarget_zone_to_aux_commit(RT_ZONE, RT_RELAY, true, &c, err, sizeof(err)),
               "a plain commit refuses for the same reason");
    err[0] = '\0';
    TEST_CHECK(profiles_retarget_zone_to_aux_resume(RT_ZONE, RT_RELAY, true, &c, err, sizeof(err)), err);
    TEST_CHECK(c.profiles_affected == 1 && c.rules_retargeted == 1, "resume rewrote only the missing slot");
    TEST_CHECK(s_profiles.profiles[0].on_off_rules[0].zone_index == RT_DEST &&
                   s_profiles.profiles[2].on_off_rules[0].zone_index == RT_DEST &&
                   s_profiles.profiles[1].on_off_rules[0].zone_index == 2,
               "every slot is at the destination, the other zone is untouched");
    TEST_CHECK(rt_persisted_matches_ram() && retarget_verify_slot(2), "persisted profiles match RAM after the resume");
    TEST_CHECK(profiles_retarget_zone_to_aux_resume(RT_ZONE, RT_RELAY, true, &c, err, sizeof(err)) &&
                   c.profiles_affected == 0 && c.rules_retargeted == 0,
               "resuming a finished conversion is a no-op success");
}

static void test_aux_rule_survives_real_save_and_load(void)
{
    TEST_SECTION("aux rule target 10 round-trips through the real save/load");
    memset(g_stub_aux, 0, sizeof(g_stub_aux));
    g_stub_aux[2].enabled = true; /* relay 3 -> target 10 */
    g_stub_aux[2].tc_zone = 0;
    profile_t p = make_stored_profile();
    p.on_off_rule_count = 1;
    memset(&p.on_off_rules[0], 0, sizeof(p.on_off_rules[0]));
    p.on_off_rules[0].zone_index = 10;
    p.on_off_rules[0].enable = 1;
    p.on_off_rules[0].temp_source = 1;
    p.on_off_rules[0].temp_cmp = ON_OFF_TEMP_CMP_BELOW;
    p.on_off_rules[0].temp_threshold_c = 300.0f;
    char err[160] = "";
    uint8_t out_id = 0;
    uint8_t warn = 0;
    bool ok = profiles_http_save(PROFILES_MAX_COUNT, &p, &out_id, &warn, err, sizeof(err));
    TEST_CHECK(ok, "save of a profile with an aux rule succeeds while that aux is enabled");
    profile_t back;
    TEST_CHECK(ok && profiles_http_get(out_id, &back) && back.on_off_rule_count == 1 &&
                   back.on_off_rules[0].zone_index == 10 && back.on_off_rules[0].temp_cmp == ON_OFF_TEMP_CMP_BELOW,
              "the stored rule keeps target byte 10 and its fields");
    g_stub_aux[2].enabled = false;
    ok = profiles_http_save(PROFILES_MAX_COUNT, &p, &out_id, &warn, err, sizeof(err));
    TEST_CHECK(!ok, "the same save is refused once the aux is disabled");
    memset(g_stub_aux, 0, sizeof(g_stub_aux));
}

static void test_validate_io_segment_refuses_aux_bound_relay(void)
{
    TEST_SECTION("validate_io_segment -- a relay bound to an enabled aux output is refused");
    profile_segment_t seg;
    char err[160];
    memset(&seg, 0, sizeof(seg));
    seg.seg_kind = PROFILE_SEG_KIND_RELAY_IO;
    seg.io_state = 1;
    seg.io_blocking = 1;
    seg.io_target = 4;
    g_zone_relay_mask_zone0 = 0x00;
    memset(g_stub_aux, 0, sizeof(g_stub_aux));
    TEST_CHECK(validate_io_segment(&seg, 1, err, sizeof(err)), "relay 4 accepted while no aux binds it");
    g_stub_aux[3].enabled = true;
    TEST_CHECK(!validate_io_segment(&seg, 1, err, sizeof(err)) && strstr(err, "aux output"),
              "relay 4 refused while aux relay 4 is enabled");
    seg.io_target = 3;
    TEST_CHECK(validate_io_segment(&seg, 1, err, sizeof(err)), "relay 3 still accepted (only relay 4 is aux)");
    memset(g_stub_aux, 0, sizeof(g_stub_aux));
}

// ---------------------------------------------------------------------------
// Test 5 -- validate_io_segment(): a zone-assigned relay is REFUSED as a
// segment target, and a genuinely unassigned one is ACCEPTED. Both
// directions on purpose -- a gate that refuses everything is not a fix.
// ---------------------------------------------------------------------------
static void test_validate_io_segment_zone_ownership(void)
{
    TEST_SECTION("validate_io_segment -- zone-owned relay refused, unassigned relay accepted");

    profile_segment_t seg;
    memset(&seg, 0, sizeof(seg));
    seg.seg_kind = PROFILE_SEG_KIND_RELAY_IO;
    seg.io_target = 1; /* relay 1 */
    seg.io_state = 1;
    seg.io_blocking = 1;
    char err_msg[128];

    g_zone_relay_mask_zone0 = 0x01; /* zone 0 owns relay 1 */
    bool ok_owned = validate_io_segment(&seg, 1, err_msg, sizeof(err_msg));
    TEST_CHECK(!ok_owned, "a relay assigned to a zone must be refused as a segment target");
    TEST_CHECK(strstr(err_msg, "assigned to zone") != NULL, "the refusal names the owning zone");

    g_zone_relay_mask_zone0 = 0x00; /* nothing owns relay 1 now */
    bool ok_free = validate_io_segment(&seg, 1, err_msg, sizeof(err_msg));
    TEST_CHECK(ok_free, "a relay owned by no zone must be ACCEPTED as a segment target "
                        "(a gate that refuses everything is not a fix)");
}

// ---------------------------------------------------------------------------
// Test 6 -- validate_io_segment(): the encoding gap between the relay range
// (1-4) and the IO range (11-17) -- where a ~DRDY or LCD pin would have to
// be encoded if this gate did not exist -- is refused, and the two legal
// ranges' own boundary values are accepted.
// ---------------------------------------------------------------------------
static void test_validate_io_segment_drdy_lcd_gap_refused(void)
{
    TEST_SECTION("validate_io_segment -- the dead encoding gap (would-be ~DRDY/LCD) is refused");

    profile_segment_t seg;
    char err_msg[128];

    memset(&seg, 0, sizeof(seg));
    seg.seg_kind = PROFILE_SEG_KIND_RELAY_IO;
    seg.io_state = 1;
    seg.io_blocking = 1;

    seg.io_target = 8; /* squarely inside the 5..10 dead gap */
    TEST_CHECK(!validate_io_segment(&seg, 1, err_msg, sizeof(err_msg)),
              "a target in the dead gap between the relay and IO ranges must be refused");

    seg.io_target = 0; /* PROFILE_IO_TARGET_NONE */
    TEST_CHECK(!validate_io_segment(&seg, 1, err_msg, sizeof(err_msg)), "io_target 0 (NONE) must be refused");

    seg.io_target = 18; /* one past the IO range's top (17 = IO_7) */
    TEST_CHECK(!validate_io_segment(&seg, 1, err_msg, sizeof(err_msg)),
              "one past the top of the legal IO range must be refused");

    seg.io_target = 4; /* Relay4, top of the legal relay range -- positive control */
    TEST_CHECK(validate_io_segment(&seg, 1, err_msg, sizeof(err_msg)),
              "the relay range's own top boundary (4) must be ACCEPTED -- the gate must not overreach "
              "into refusing valid targets");

    seg.io_target = 17; /* IO_7, top of the legal IO range -- positive control */
    TEST_CHECK(validate_io_segment(&seg, 1, err_msg, sizeof(err_msg)),
              "the IO range's own top boundary (17 = IO_7) must be ACCEPTED");
}

// ---------------------------------------------------------------------------
// OWNER CORRECTION (2026-09-02): a99bc15 made profiles_http_save() REFUSE a
// segment target_c above the participating zone's configured max_temp_c.
// The owner overturned that: "you should be able to put in fireing profiles
// that exceed kiln max but not run them if they go beyond kiln max ... some
// one may want to use this for a gas fired kiln up to 2,015C". Profiles are
// portable between kilns -- authoring a cone-10 or gas-kiln profile on a
// low-temperature bench rig is legitimate, and the kiln's ceiling is a
// property of the INSTALLATION, not of the profile. Save must now ACCEPT
// every one of these; the single enforcement point is
// profile_executor_run()'s run-start re-check (test_profile_executor_
// prestart.c's test_run_refuses_cone10_profile_on_80c_zone() and its 2015C
// sibling below), unchanged by this pass and proven to still fire by the
// mutation described in the task report.
// ---------------------------------------------------------------------------

static void test_profiles_http_save_accepts_cone10_profile_on_80c_zone(void)
{
    TEST_SECTION("profiles_http_save -- a cone-10-scale target (1285C) on an 80C zone is now ACCEPTED "
                 "(save is portable-across-kilns; only run start enforces the ceiling)");
    memset(&s_profiles, 0, sizeof(s_profiles));
    g_stub_zone_max_temp_c[0] = 80.0f;

    profile_t p;
    memset(&p, 0, sizeof(p));
    strcpy(p.name, "Cone 10 Gas Kiln");
    p.zone_mask = 0x01;
    p.segment_count = 1;
    p.segments[0].target_c = 1285.0f;
    p.segments[0].ramp_c_per_hr = 0.0f; /* isolate the ceiling check from the ramp-rate one */
    p.segments[0].dwell_min = 10;

    uint8_t out_id = 0, warn_count = 0;
    char err_msg[160] = "";
    bool ok = profiles_http_save(PROFILES_MAX_COUNT, &p, &out_id, &warn_count, err_msg, sizeof(err_msg));

    TEST_CHECK(ok, "a 1285C target on an 80C zone must be ACCEPTED at save time");
    TEST_CHECK(profiles_slot_bitmap_test(&s_profiles.used_bitmap, out_id), "the profile must actually be written to storage");
    TEST_CHECK(warn_count >= 1, "the over-ceiling condition must still be surfaced as a warning, not silently "
                                "dropped");
}

static void test_profiles_http_save_accepts_in_range_profile(void)
{
    TEST_SECTION("profiles_http_save -- a normal in-range target (55C) on an 80C zone is accepted, no warning");
    memset(&s_profiles, 0, sizeof(s_profiles));
    g_stub_zone_max_temp_c[0] = 80.0f;

    profile_t p;
    memset(&p, 0, sizeof(p));
    strcpy(p.name, "Bisque 55");
    p.zone_mask = 0x01;
    p.segment_count = 1;
    p.segments[0].target_c = 55.0f;
    p.segments[0].ramp_c_per_hr = 0.0f;
    p.segments[0].dwell_min = 5;

    uint8_t out_id = 0, warn_count = 0;
    char err_msg[160] = "";
    bool ok = profiles_http_save(PROFILES_MAX_COUNT, &p, &out_id, &warn_count, err_msg, sizeof(err_msg));

    TEST_CHECK(ok, "55C on an 80C zone must be accepted");
    TEST_CHECK(warn_count == 0, "an in-range target must not raise the ceiling warning");
}

static void test_profiles_http_save_accepts_target_exactly_at_zone_limit(void)
{
    TEST_SECTION("profiles_http_save -- a target EXACTLY at the zone's 80C limit is accepted, no warning "
                 "(the boundary itself, not one degree over it)");
    memset(&s_profiles, 0, sizeof(s_profiles));
    g_stub_zone_max_temp_c[0] = 80.0f;

    profile_t p;
    memset(&p, 0, sizeof(p));
    strcpy(p.name, "Exactly 80");
    p.zone_mask = 0x01;
    p.segment_count = 1;
    p.segments[0].target_c = 80.0f;
    p.segments[0].ramp_c_per_hr = 0.0f;
    p.segments[0].dwell_min = 5;

    uint8_t out_id = 0, warn_count = 0;
    char err_msg[160] = "";
    bool ok = profiles_http_save(PROFILES_MAX_COUNT, &p, &out_id, &warn_count, err_msg, sizeof(err_msg));

    TEST_CHECK(ok, "exactly 80.0C on an 80C-limit zone must be accepted, not treated as 'over'");
    TEST_CHECK(warn_count == 0, "the exact boundary must not raise the ceiling warning either");
}

static void test_profiles_http_save_accepts_one_degree_over_zone_limit(void)
{
    TEST_SECTION("profiles_http_save -- one degree over the zone's 80C limit (80.1C) is accepted, WITH a warning "
                 "(was refused before the owner correction)");
    memset(&s_profiles, 0, sizeof(s_profiles));
    g_stub_zone_max_temp_c[0] = 80.0f;

    profile_t p;
    memset(&p, 0, sizeof(p));
    strcpy(p.name, "One Over");
    p.zone_mask = 0x01;
    p.segment_count = 1;
    p.segments[0].target_c = 80.1f;
    p.segments[0].ramp_c_per_hr = 0.0f;
    p.segments[0].dwell_min = 5;

    uint8_t out_id = 0, warn_count = 0;
    char err_msg[160] = "";
    bool ok = profiles_http_save(PROFILES_MAX_COUNT, &p, &out_id, &warn_count, err_msg, sizeof(err_msg));

    TEST_CHECK(ok, "80.1C on an 80C-limit zone must be accepted -- save never refuses on the ceiling any more");
    TEST_CHECK(warn_count >= 1, "80.1C is over the ceiling -- the warning must fire");
}

static void test_profiles_http_save_accepts_2015c_gas_kiln_profile_on_80c_zone(void)
{
    TEST_SECTION("profiles_http_save -- the owner's exact scenario: a 2015C (cone 42) gas-kiln profile "
                 "saves successfully on an 80C bench-rig zone");
    memset(&s_profiles, 0, sizeof(s_profiles));
    g_stub_zone_max_temp_c[0] = 80.0f;

    profile_t p;
    memset(&p, 0, sizeof(p));
    strcpy(p.name, "Gas Kiln Cone 42");
    p.zone_mask = 0x01;
    p.segment_count = 1;
    p.segments[0].target_c = 2015.0f; /* PROFILE_TARGET_C_MAX exactly */
    p.segments[0].ramp_c_per_hr = 0.0f;
    p.segments[0].dwell_min = 30;

    uint8_t out_id = 0, warn_count = 0;
    char err_msg[160] = "";
    bool ok = profiles_http_save(PROFILES_MAX_COUNT, &p, &out_id, &warn_count, err_msg, sizeof(err_msg));

    TEST_CHECK(ok, "2015C must be storable -- it is PROFILE_TARGET_C_MAX exactly, not past it");
    TEST_CHECK(profiles_slot_bitmap_test(&s_profiles.used_bitmap, out_id), "the 2015C profile must actually be written");
    TEST_CHECK(warn_count >= 1, "2015C on an 80C zone must still be flagged as exceeding this kiln's ceiling");

    /* Positive control on the input-sanity bound itself, now that it moved:
     * one degree past PROFILE_TARGET_C_MAX must still be refused (this is
     * the garbage/typo bound, not the per-kiln ceiling, and it did not
     * change meaning in this pass -- only its value moved from 1400 to
     * 2015). */
    profile_t over;
    memset(&over, 0, sizeof(over));
    strcpy(over.name, "Past Sanity Bound");
    over.zone_mask = 0x01;
    over.segment_count = 1;
    over.segments[0].target_c = 2015.1f;
    over.segments[0].ramp_c_per_hr = 0.0f;
    over.segments[0].dwell_min = 5;
    uint8_t out_id2 = 0, warn_count2 = 0;
    char err_msg2[160] = "";
    bool ok2 = profiles_http_save(PROFILES_MAX_COUNT, &over, &out_id2, &warn_count2, err_msg2, sizeof(err_msg2));
    TEST_CHECK(!ok2, "PROFILE_TARGET_C_MAX (2015C) is still a real input-sanity bound -- 2015.1C must be refused");
    /* Opus review finding 7 (2026-09-02): the refusal message must state the
     * REAL bound, derived from PROFILE_TARGET_C_MAX itself via %.0f rather
     * than a hand-typed literal that can go stale the next time the bound
     * moves (it already did once: 1400 -> 2015). This would go red if either
     * the message format or PROFILE_TARGET_C_MAX changed without the other
     * following -- proved by hand: reverting the message to the old
     * hardcoded "(0-1400)" text makes this fail with the actual "(0-2015)"
     * string, and bumping PROFILE_TARGET_C_MAX without touching the message
     * (impossible here since the format string reads the macro directly, but
     * would be a stale-literal regression on any prior hardcoded version)
     * fails the same way. */
    char expected_bound[32];
    snprintf(expected_bound, sizeof(expected_bound), "(%.0f-%.0f)", (double)PROFILE_TARGET_C_MIN,
             (double)PROFILE_TARGET_C_MAX);
    TEST_CHECK(strstr(err_msg2, expected_bound) != NULL,
              "the refusal message must state the bound matching PROFILE_TARGET_C_MIN/MAX exactly, not a "
              "stale hand-typed literal");
}

// ---------------------------------------------------------------------------
// Owner request 2026-09-19: saving a profile must NEVER be allowed with a
// name that already exists. profiles_http_save() now runs
// live_edit_name_collides() (live_profile.c, already used by the live-edit
// SAVE_AS path) right before writing the slot.
// ---------------------------------------------------------------------------

static void test_profiles_http_save_rejects_exact_duplicate_name(void)
{
    TEST_SECTION("profiles_http_save -- a name that already exists in another slot is refused");
    memset(&s_profiles, 0, sizeof(s_profiles));

    profile_t p = make_stored_profile();
    strcpy(p.name, "Bisque Fast");
    uint8_t out_id = 0, warn_count = 0;
    char err_msg[160] = "";
    TEST_CHECK(profiles_http_save(PROFILES_MAX_COUNT, &p, &out_id, &warn_count, err_msg, sizeof(err_msg)),
              "setup: the first save of this name must succeed");

    profile_t p2 = make_stored_profile();
    strcpy(p2.name, "Bisque Fast");
    uint8_t out_id2 = 0, warn_count2 = 0;
    char err_msg2[160] = "";
    bool ok2 = profiles_http_save(PROFILES_MAX_COUNT, &p2, &out_id2, &warn_count2, err_msg2, sizeof(err_msg2));

    TEST_CHECK(!ok2, "saving a second profile with the exact same name must be refused");
    TEST_CHECK(strstr(err_msg2, "Bisque Fast") != NULL, "the refusal must name the offending profile");
    TEST_CHECK(!profiles_slot_bitmap_test(&s_profiles.used_bitmap, out_id2) || out_id2 == out_id,
              "the refused save must not land in a second slot");
}

static void test_profiles_http_save_rejects_case_and_whitespace_variant_name(void)
{
    TEST_SECTION("profiles_http_save -- a case/whitespace variant of an existing name is also refused "
                 "(live_edit_name_collides() normalizes both before comparing)");
    memset(&s_profiles, 0, sizeof(s_profiles));

    profile_t p = make_stored_profile();
    strcpy(p.name, "Bisque Fast");
    uint8_t out_id = 0, warn_count = 0;
    char err_msg[160] = "";
    TEST_CHECK(profiles_http_save(PROFILES_MAX_COUNT, &p, &out_id, &warn_count, err_msg, sizeof(err_msg)),
              "setup: the first save must succeed");

    profile_t p2 = make_stored_profile();
    strcpy(p2.name, "  BISQUE fast  ");
    uint8_t out_id2 = 0, warn_count2 = 0;
    char err_msg2[160] = "";
    bool ok2 = profiles_http_save(PROFILES_MAX_COUNT, &p2, &out_id2, &warn_count2, err_msg2, sizeof(err_msg2));

    TEST_CHECK(!ok2, "a case/whitespace-only variant of an existing name must still be refused");
}

static void test_profiles_http_save_allows_overwriting_a_slot_with_its_own_name(void)
{
    TEST_SECTION("profiles_http_save -- overwriting an existing slot with its OWN unchanged name stays legal "
                 "(exclude_id must exempt the slot being written)");
    memset(&s_profiles, 0, sizeof(s_profiles));

    profile_t p = make_stored_profile();
    strcpy(p.name, "Bisque Fast");
    uint8_t out_id = 0, warn_count = 0;
    char err_msg[160] = "";
    TEST_CHECK(profiles_http_save(PROFILES_MAX_COUNT, &p, &out_id, &warn_count, err_msg, sizeof(err_msg)),
              "setup: the first save must succeed");

    /* Re-save targeting the SAME slot, same name, some other field changed
     * (dwell_min) -- must be accepted, not refused as colliding with itself. */
    profile_t p2 = p;
    p2.segments[0].dwell_min = p2.segments[0].dwell_min + 1;
    uint8_t out_id2 = 0, warn_count2 = 0;
    char err_msg2[160] = "";
    bool ok2 = profiles_http_save(out_id, &p2, &out_id2, &warn_count2, err_msg2, sizeof(err_msg2));

    TEST_CHECK(ok2, "overwriting a slot with its own name must be accepted, not refused as a collision");
    TEST_CHECK(out_id2 == out_id, "an overwrite-by-id must land back in the same slot");
}

static void test_profiles_http_save_allows_builtin_name(void)
{
    /* Opus review of 5dd23944, finding 1/BLOCKER: this test used to assert
     * the OPPOSITE (refused) -- that was the bug. profiles_catalog_http.c
     * emits a builtin's `code` as its JSON "name", and profiles_page.html's
     * copyBuiltin() posts that straight back as name= on a fresh USER-slot
     * save -- refusing it meant "Copy builtin" was unusable for every
     * builtin, and any user slot already named like one could never be
     * edited again. profiles_http_save() now calls
     * live_edit_name_collides_ex() with include_builtins=false, so a
     * user-slot save naming itself after a builtin's code must succeed. */
    TEST_SECTION("profiles_http_save -- a name matching the read-only builtin catalogue is now ALLOWED "
                 "for a user-slot save (Opus review of 5dd23944, finding 1 -- this used to wrongly refuse "
                 "every \"Copy builtin\")");
    memset(&s_profiles, 0, sizeof(s_profiles));
    g_fake_builtin_on = true;
    strcpy((char *)g_fake_builtin.code, "C6DHSC");

    profile_t p = make_stored_profile();
    strcpy(p.name, "c6dhsc"); /* case-insensitive match against the builtin's code */
    uint8_t out_id = 0, warn_count = 0;
    char err_msg[160] = "";
    bool ok = profiles_http_save(PROFILES_MAX_COUNT, &p, &out_id, &warn_count, err_msg, sizeof(err_msg));

    TEST_CHECK(ok, "a user-slot save naming itself after a builtin's code must be allowed -- this is "
                   "exactly what \"Copy builtin\" produces on purpose");
    TEST_CHECK(err_msg[0] == '\0', "a successful save must not leave a stale refusal message");

    g_fake_builtin_on = false;
}

static void test_profiles_http_save_allows_editing_existing_slot_named_like_builtin(void)
{
    /* Opus review of 5dd23944, finding 1/BLOCKER: exclude_id only ever
     * excludes a USER slot from the user-vs-user scan, never a builtin from
     * the (now-disabled-for-this-caller) builtin scan -- so before this fix,
     * a user slot already named like a builtin (e.g. from an earlier "Copy
     * builtin") could never be saved/edited again: every re-save of that
     * SAME slot with its OWN unchanged name collided against the builtin
     * scan, which exclude_id never touched. */
    TEST_SECTION("profiles_http_save -- re-saving an EXISTING user slot that is already named like a "
                 "builtin (e.g. a prior \"Copy builtin\") must succeed, not be permanently unrenamable");
    memset(&s_profiles, 0, sizeof(s_profiles));
    g_fake_builtin_on = true;
    strcpy((char *)g_fake_builtin.code, "C6DHSC");

    profile_t p = make_stored_profile();
    strcpy(p.name, "c6dhsc");
    uint8_t out_id = 0, warn_count = 0;
    char err_msg[160] = "";
    TEST_CHECK(profiles_http_save(PROFILES_MAX_COUNT, &p, &out_id, &warn_count, err_msg, sizeof(err_msg)),
              "setup: the initial copy-as-builtin-name save must succeed");

    profile_t p2 = p;
    p2.segments[0].dwell_min = p2.segments[0].dwell_min + 1;
    uint8_t out_id2 = 0, warn_count2 = 0;
    char err_msg2[160] = "";
    bool ok2 = profiles_http_save(out_id, &p2, &out_id2, &warn_count2, err_msg2, sizeof(err_msg2));

    TEST_CHECK(ok2, "editing an existing slot that already carries a builtin-like name must succeed");
    TEST_CHECK(out_id2 == out_id, "an edit-by-id must land back in the same slot");

    g_fake_builtin_on = false;
}

// ---------------------------------------------------------------------------
// Opus review finding A (landing pass): a collision refusal from
// profile_post_handler() (POST /api/profile) used to interpolate the
// offending name RAW into `{"ok":false,"error":"%s"}`, with no escaping.
// A name containing '"' produced invalid JSON, which made
// profiles_page.html's `r.json()` throw and fall into its network-failure
// catch -- showing "could not reach the board" instead of the real refusal.
// These two tests drive profile_post_handler() through the fake httpd_req_t
// plumbing above (real body read, real response capture) rather than only
// profiles_http_save()/live_edit_name_collides() directly, so they catch
// exactly that class of bug: a helper-level test proving err_msg CONTAINS
// the right substring says nothing about whether embedding it produced
// well-formed JSON.
// ---------------------------------------------------------------------------

// Builds a minimal, always-valid POST /api/profile body (one zone-ramp
// segment) with the given (already percent-encoded) name= value, into a
// caller-owned buffer.
static void build_minimal_post_body(char *out, size_t out_cap, const char *name_encoded)
{
    snprintf(out, out_cap,
             "id=-1&name=%s&zone_mask=1&seg_count=1&seg0_target=100&seg0_ramp=50&seg0_dwell=5", name_encoded);
}

static esp_err_t run_profile_post(const char *body)
{
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.content_len = (long long)strlen(body);
    s_post_body = body;
    s_post_body_left = strlen(body);
    s_resp_capture_len = 0;
    s_resp_capture[0] = '\0';
    s_resp_capture_on = true;
    esp_err_t err = profile_post_handler(&req);
    s_resp_capture_on = false;
    s_post_body = NULL;
    s_post_body_left = 0;
    return err;
}

// ---- Seqlock bracket coverage for the non-save writers (edit, retarget commit,
// retarget revert, delete). Each probes the slot generation from inside the
// persist seam: it must be odd while the RAM assign/persist is in flight and
// even (and advanced) once the writer returns. Removing gen_begin leaves the
// in-flight value even; removing gen_end leaves it odd afterward.
static unsigned s_sg_writes;
static unsigned s_sg_odd_writes;      /* writes that saw slot 4 odd */
static unsigned s_sg_bad_writes;      /* retarget: writes that did not see exactly one odd of 0..3 */
static bool s_sg_strict;
static unsigned sg_odd_count_0_3(void)
{
    unsigned n = 0;
    for (uint8_t i = 0; i < 4; i++) {
        n += profiles_http_slot_rev(i) & 1u;
    }
    return n;
}
/* Assign-order probe: at gen_begin the RAM slot must still hold what it held at the previous
 * gen_end (or the pre-op snapshot); a RAM assign before gen_begin makes it differ. */
static profile_t s_sg_snap[8];
static unsigned s_sg_early_assign;
static unsigned s_sg_begins;
static void sg_snap_all(void)
{
    for (uint8_t i = 0; i < 8; i++) {
        s_sg_snap[i] = s_profiles.profiles[i];
    }
}
static void sg_gen_hook(uint8_t id, bool is_begin)
{
    if (id >= 8) {
        return;
    }
    if (is_begin) {
        s_sg_begins++;
        if (memcmp(&s_sg_snap[id], &s_profiles.profiles[id], sizeof(profile_t)) != 0) {
            s_sg_early_assign++;
        }
    } else {
        s_sg_snap[id] = s_profiles.profiles[id];
    }
}
static esp_err_t sg_write_fn(const char *rel_path, const void *data, size_t len)
{
    s_sg_writes++;
    if (profiles_http_slot_rev(4) & 1u) {
        s_sg_odd_writes++;
    }
    if (s_sg_strict && sg_odd_count_0_3() != 1u) {
        s_sg_bad_writes++;
    }
    return cfg_fs_write_atomic(rel_path, data, len);
}
static esp_err_t sg_fail2_write_fn(const char *rel_path, const void *data, size_t len)
{
    unsigned i = s_sg_writes;
    esp_err_t r = sg_write_fn(rel_path, data, len);
    return (i == 1) ? ESP_FAIL : r; /* the 2nd write fails -> commit reverts */
}
static esp_err_t sg_delete_fn(const char *rel_path)
{
    if (profiles_http_slot_rev(4) & 1u) {
        s_sg_odd_writes++;
    }
    return cfg_fs_delete(rel_path);
}

static void test_profile_edit_post_slot_gen(void)
{
    TEST_SECTION("slot generation: profile_post_handler (edit) brackets its RAM assign + persist");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    g_fake_exec_state = PROFILE_EXEC_IDLE;
    g_fake_exec_profile_id = 0xFF;
    profile_t p = make_stored_profile();
    uint8_t out_id = 0xFF;
    char err[128];
    TEST_CHECK(profiles_http_save(4, &p, &out_id, NULL, err, sizeof(err)), "seed slot 4");
    uint32_t before = profiles_http_slot_rev(4);
    s_sg_writes = s_sg_odd_writes = 0;
    s_sg_strict = false;
    profiles_cfg_fs_set_write_fn(sg_write_fn);
    sg_snap_all();
    s_sg_early_assign = s_sg_begins = 0;
    profiles_slot_gen_set_hook_for_test(sg_gen_hook);
    esp_err_t r = run_profile_post("id=4&name=SgEdit&zone_mask=1&seg_count=1&seg0_target=100&seg0_ramp=50&seg0_dwell=5");
    profiles_slot_gen_set_hook_for_test(NULL);
    profiles_cfg_fs_reset_write_fn_for_test();
    TEST_CHECK(s_sg_begins >= 1 && s_sg_early_assign == 0, "edit: RAM slot unchanged at gen_begin (bracket opens before the assign)");
    TEST_CHECK(r == ESP_OK && strstr(s_resp_capture, "\"ok\":true") != NULL, "edit POST succeeded");
    TEST_CHECK(s_sg_writes >= 1 && s_sg_odd_writes >= 1, "generation odd while the edit persists (gen_begin)");
    TEST_CHECK((profiles_http_slot_rev(4) & 1u) == 0u && profiles_http_slot_rev(4) != before,
               "generation even and advanced after the edit (gen_end)");
}

static void test_profile_post_expected_rev(void)
{
    TEST_SECTION("POST /api/profile -- expected_rev/expected_name: stale or reused slot -> 409 and nothing "
                 "written; matching -> saves and returns the new slot_rev (WEB_UI_JS_AUDIT M-2)");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    g_fake_exec_state = PROFILE_EXEC_IDLE;
    g_fake_exec_profile_id = 0xFF;
    profile_t p = make_stored_profile();
    uint8_t out_id = 0xFF;
    char err[128];
    TEST_CHECK(profiles_http_save(4, &p, &out_id, NULL, err, sizeof(err)), "seed slot 4");
    uint32_t rev0 = profiles_http_slot_rev(4);
    char body[400];
    const char *tail = "&zone_mask=1&seg_count=1&seg0_target=100&seg0_ramp=50&seg0_dwell=5";

    /* an edit by someone else moves the rev */
    run_profile_post("id=4&name=Other&zone_mask=1&seg_count=1&seg0_target=100&seg0_ramp=50&seg0_dwell=5");
    TEST_CHECK(profiles_http_slot_rev(4) != rev0, "setup: the other writer moved the rev");
    uint32_t rev1 = profiles_http_slot_rev(4);

    snprintf(body, sizeof(body), "id=4&name=Mine&expected_rev=%lu%s", (unsigned long)rev0, tail);
    esp_err_t r = run_profile_post(body);
    TEST_CHECK(r == ESP_OK && strstr(s_resp_capture, "profile_changed") != NULL, "stale expected_rev refused with profile_changed");
    TEST_CHECK(strcmp(s_profiles.profiles[4].name, "Other") == 0, "stale save wrote nothing");
    TEST_CHECK(profiles_http_slot_rev(4) == rev1, "stale save did not move the rev");

    snprintf(body, sizeof(body), "id=4&name=Mine&expected_rev=%lu&expected_name=WrongName%s", (unsigned long)rev1, tail);
    run_profile_post(body);
    TEST_CHECK(strstr(s_resp_capture, "profile_changed") != NULL, "matching rev but different identity (name) refused");
    TEST_CHECK(strcmp(s_profiles.profiles[4].name, "Other") == 0, "identity mismatch wrote nothing");

    snprintf(body, sizeof(body), "id=4&name=Mine&expected_rev=%lu&expected_name=Other%s", (unsigned long)rev1, tail);
    run_profile_post(body);
    TEST_CHECK(strstr(s_resp_capture, "\"ok\":true") != NULL && strstr(s_resp_capture, "\"slot_rev\":") != NULL,
               "matching token saves and reports slot_rev");
    TEST_CHECK(strcmp(s_profiles.profiles[4].name, "Mine") == 0, "matching save wrote the slot");

    /* deleted slot: an expected_* overwrite must not resurrect it */
    TEST_CHECK(profiles_http_delete(4), "delete slot 4");
    snprintf(body, sizeof(body), "id=4&name=Ghost&expected_rev=%lu%s", (unsigned long)profiles_http_slot_rev(4), tail);
    run_profile_post(body);
    TEST_CHECK(strstr(s_resp_capture, "profile_changed") != NULL, "overwrite of a deleted slot with a token refused");
    TEST_CHECK(!profiles_slot_used(4), "deleted slot not resurrected");

    /* no token: old behaviour */
    run_profile_post("id=4&name=Legacy&zone_mask=1&seg_count=1&seg0_target=100&seg0_ramp=50&seg0_dwell=5");
    TEST_CHECK(strstr(s_resp_capture, "\"ok\":true") != NULL, "no token keeps the old create-or-overwrite behaviour");
}

static void test_retarget_slot_gen(void)
{
    TEST_SECTION("slot generation: retarget commit and revert bracket each slot's assign + persist");
    for (int pass = 0; pass < 2; pass++) {
        rt_seed();
        uint32_t before[4];
        for (uint8_t i = 0; i < 4; i++) {
            before[i] = profiles_http_slot_rev(i);
        }
        profiles_retarget_counts_t c;
        char err[160] = "";
        s_sg_writes = s_sg_odd_writes = s_sg_bad_writes = 0;
        s_sg_strict = true;
        /* pass 0: clean commit. pass 1: 2nd write fails, so the revert writes too. */
        profiles_cfg_fs_set_write_fn(pass == 0 ? sg_write_fn : sg_fail2_write_fn);
        sg_snap_all();
        s_sg_early_assign = s_sg_begins = 0;
        profiles_slot_gen_set_hook_for_test(sg_gen_hook);
        bool ok = profiles_retarget_zone_to_aux_commit(RT_ZONE, RT_RELAY, true, &c, err, sizeof(err));
        profiles_slot_gen_set_hook_for_test(NULL);
        profiles_cfg_fs_reset_write_fn_for_test();
        TEST_CHECK(s_sg_begins >= 1 && s_sg_early_assign == 0, "retarget: RAM slot unchanged at every gen_begin (bracket opens before the assign)");
        s_sg_strict = false;
        TEST_CHECK(ok == (pass == 0), pass == 0 ? "commit succeeds" : "injected failure fails the commit");
        TEST_CHECK(s_sg_writes >= (pass == 0 ? 2u : 3u), pass == 0 ? "commit wrote slots" : "revert wrote after the failure");
        TEST_CHECK(s_sg_bad_writes == 0, pass == 0 ? "every commit write saw exactly one slot in flight (gen_begin/gen_end)"
                                                   : "every commit and revert write saw exactly one slot in flight (gen_begin/gen_end)");
        TEST_CHECK(sg_odd_count_0_3() == 0, "all slot generations even afterward");
        bool advanced = false;
        for (uint8_t i = 0; i < 4; i++) {
            advanced = advanced || profiles_http_slot_rev(i) != before[i];
        }
        TEST_CHECK(advanced, "generations advanced");
    }
}

static void test_delete_slot_gen(void)
{
    TEST_SECTION("slot generation: profiles_delete_slot brackets its erase + RAM clear");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    g_fake_exec_state = PROFILE_EXEC_IDLE;
    g_fake_exec_profile_id = 0xFF;
    profile_t p = make_stored_profile();
    uint8_t out_id = 0xFF;
    char err[128];
    TEST_CHECK(profiles_http_save(4, &p, &out_id, NULL, err, sizeof(err)), "seed slot 4");
    uint32_t before = profiles_http_slot_rev(4);
    s_sg_odd_writes = 0;
    profiles_cfg_fs_set_delete_fn(sg_delete_fn);
    sg_snap_all();
    s_sg_early_assign = s_sg_begins = 0;
    profiles_slot_gen_set_hook_for_test(sg_gen_hook);
    TEST_CHECK(profiles_delete_slot(4) == PROFILES_DELETE_OK, "delete slot 4");
    profiles_slot_gen_set_hook_for_test(NULL);
    profiles_cfg_fs_reset_delete_fn_for_test();
    TEST_CHECK(s_sg_begins >= 1 && s_sg_early_assign == 0, "delete: RAM slot unchanged at gen_begin");
    TEST_CHECK(s_sg_odd_writes >= 1, "generation odd while the erase runs (gen_begin)");
    TEST_CHECK((profiles_http_slot_rev(4) & 1u) == 0u && profiles_http_slot_rev(4) != before,
               "generation even and advanced after the delete (gen_end)");
}

// Strict input audit (2026-10-09): a non-numeric / trailing-garbage / overlong id must
// be a 400, never slot 0 (overwrite) or "create new".
static void test_profile_post_handler_rejects_malformed_id(void)
{
    TEST_SECTION("profile_post_handler() -- id=abc / id=1abc / overlong id / id=%00 are refused with 400");
    memset(&s_profiles, 0, sizeof(s_profiles));
    const char *rest = "&name=StrictId&zone_mask=1&seg_count=1&seg0_target=100&seg0_ramp=50&seg0_dwell=5";
    const char *ids[] = { "id=abc", "id=1abc", "id=123456789", "id=-2", "id=100", "id=127", "id=129", "id=1%002" };
    for (size_t k = 0; k < sizeof(ids) / sizeof(ids[0]); k++) {
        char body[256];
        snprintf(body, sizeof(body), "%s%s", ids[k], rest);
        s_last_err_code = 0;
        TEST_CHECK(run_profile_post(body) == ESP_OK, "handler replies itself");
        TEST_CHECK(s_last_err_code == 400, "malformed id answers 400");
        TEST_CHECK(!profiles_slot_used(0) && !profiles_slot_used(1), "nothing was written to any slot");
    }
}

// Built-in copy: id=<builtin id> saves into the first free slot (same as -1)
// and never touches the built-in; non-builtin ids above the slot range stay 400.
static void test_profile_post_builtin_id_copies_to_first_free(void)
{
    TEST_SECTION("profile_post_handler() -- id=128 (builtin) saves into first free slot, builtin untouched");
    memset(&s_profiles, 0, sizeof(s_profiles));
    pcfg_reset_all();
    g_fake_builtin_on = true;
    char body[256];
    snprintf(body, sizeof(body),
             "id=%d&name=BuiltinCopy&zone_mask=1&seg_count=1&seg0_target=100&seg0_ramp=50&seg0_dwell=5",
             (int)PROFILE_BUILTIN_ID_BASE);
    s_last_err_code = 0;
    TEST_CHECK(run_profile_post(body) == ESP_OK, "handler replies itself");
    TEST_CHECK(s_last_err_code != 400, "builtin id is not a 400");
    TEST_CHECK(profiles_slot_used(0), "copy landed in the first free slot");
    TEST_CHECK(strstr(s_resp_capture, "\"ok\":true") != NULL, "reply is ok:true");
    TEST_CHECK(strstr(s_resp_capture, "\"id\":0,") != NULL, "reply id is the new user slot (0), not the builtin id");
    TEST_CHECK(strcmp(s_profiles.profiles[0].name, "BuiltinCopy") == 0, "slot 0 holds the posted contents");
    TEST_CHECK(!profiles_slot_used(1), "exactly one slot was created");
    TEST_CHECK(g_builtin_profile_count == 0, "builtin catalogue unchanged");
    g_fake_builtin_on = false;
}

static void test_profile_name_nul_refused(void)
{
    TEST_SECTION("profile_post_handler() -- name=%00 is refused, not stored as an empty name");
    memset(&s_profiles, 0, sizeof(s_profiles));
    char body[256];
    build_minimal_post_body(body, sizeof(body), "a%00b");
    run_profile_post(body);
    TEST_CHECK(strstr(s_resp_capture, "\"ok\":false") != NULL, "NUL in name answered ok:false");
    TEST_CHECK(!profiles_slot_used(0), "nothing stored");
}

static void test_profile_rule_temp_nan_refused_even_with_cmp_none(void)
{
    TEST_SECTION("profile_post_handler() -- rule0_temp_c=nan refused even when temp_cmp is NONE");
    memset(&s_profiles, 0, sizeof(s_profiles));
    char body[320];
    snprintf(body, sizeof(body),
             "id=-1&name=NanRule&zone_mask=1&seg_count=1&seg0_target=100&seg0_ramp=50&seg0_dwell=5"
             "&rule_count=1&rule0_zone=0&rule0_seg=0&rule0_temp_cmp=0&rule0_temp_c=nan");
    run_profile_post(body);
    TEST_CHECK(!profiles_slot_used(0), "a NaN rule threshold is never persisted");
}

static void test_profile_favorite_empty_slot_refused(void)
{
    TEST_SECTION("profile_favorite_post_handler() -- an empty slot or a bad favorite value is refused");
    memset(&s_profiles, 0, sizeof(s_profiles));
    pcfg_reset_all();
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    const char *b1 = "id=3&favorite=1";
    req.content_len = (long long)strlen(b1);
    s_post_body = b1;
    s_post_body_left = strlen(b1);
    s_last_err_code = 0;
    (void)profile_favorite_post_handler(&req);
    TEST_CHECK(s_last_err_code == 404, "favoriting an empty slot answers 404");
    TEST_CHECK(!profiles_favorites_is(3), "empty slot not marked favorite");
    const char *b2 = "id=abc";
    req.content_len = (long long)strlen(b2);
    s_post_body = b2;
    s_post_body_left = strlen(b2);
    s_last_err_code = 0;
    (void)profile_favorite_post_handler(&req);
    TEST_CHECK(s_last_err_code == 400, "id=abc answers 400");
    s_post_body = NULL;
    s_post_body_left = 0;
}

static void test_profile_detail_long_query(void)
{
    TEST_SECTION("profile_detail_get_handler()/builtin_list -- a cache-buster query no longer becomes 'id missing'; "
                 "a truncated query is refused 400");
    memset(&s_profiles, 0, sizeof(s_profiles));
    profiles_slot_bitmap_set(&s_profiles.used_bitmap, 0);
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    s_stub_query_str = "id=0&_=1728480000000000000000000000000000000";
    s_last_err_code = 0;
    (void)profile_detail_get_handler(&req);
    TEST_CHECK(s_last_err_code != 400, "a 46-char query still resolves the id");
    s_stub_query_str = "id=0&_=1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890";
    s_last_err_code = 0;
    (void)profile_detail_get_handler(&req);
    TEST_CHECK(s_last_err_code == 400, "an over-long query is refused 400");
    s_last_err_code = 0;
    (void)builtin_list_get_handler(&req);
    TEST_CHECK(s_last_err_code == 400, "builtin_list: truncated query refused 400");
    s_stub_query_str = "id=1abc";
    s_last_err_code = 0;
    (void)profile_detail_get_handler(&req);
    TEST_CHECK(s_last_err_code == 404, "id=1abc is not slot 1");
    s_stub_query_str = NULL;
}

static void test_profile_post_handler_collision_response_is_well_formed_json(void)
{
    TEST_SECTION("profile_post_handler() -- a plain (no-quote) dup-name collision response is well-formed "
                 "JSON and names the offending profile");
    memset(&s_profiles, 0, sizeof(s_profiles));

    char body1[256];
    build_minimal_post_body(body1, sizeof(body1), "Bisque%20Fast");
    TEST_CHECK(run_profile_post(body1) == ESP_OK, "setup: the first save must succeed");

    char body2[256];
    build_minimal_post_body(body2, sizeof(body2), "Bisque%20Fast");
    TEST_CHECK(run_profile_post(body2) == ESP_OK, "handler must still return ESP_OK on a refusal "
                                                  "(it replies with a 400 JSON body, not a transport error)");
    TEST_CHECK(json_is_well_formed(s_resp_capture),
              "a plain-name collision response must already be well-formed JSON");
    TEST_CHECK(strstr(s_resp_capture, "Bisque Fast") != NULL,
              "the response must name the offending profile");
    TEST_CHECK(strstr(s_resp_capture, "\"ok\":false") != NULL, "the response must report ok:false");
}

static void test_profile_post_handler_collision_response_escapes_quote_in_name(void)
{
    TEST_SECTION("profile_post_handler() -- a dup-name collision response stays well-formed JSON even "
                 "when the colliding name itself contains a '\"' (Opus review finding A -- this used to "
                 "produce invalid JSON, which made profiles_page.html's r.json() throw)");
    memset(&s_profiles, 0, sizeof(s_profiles));

    // %22 -- percent-encoded '"'. http_form_url_decode() (http_form.h) turns
    // this back into a literal '"' before it ever reaches
    // live_edit_name_collides(), the same way a real browser-submitted form
    // body would.
    char body1[256];
    build_minimal_post_body(body1, sizeof(body1), "Bisque%20%2210%22");
    TEST_CHECK(run_profile_post(body1) == ESP_OK, "setup: the first save (name containing '\"') must succeed");

    char body2[256];
    build_minimal_post_body(body2, sizeof(body2), "Bisque%20%2210%22");
    TEST_CHECK(run_profile_post(body2) == ESP_OK, "handler must still return ESP_OK on a refusal");
    TEST_CHECK(json_is_well_formed(s_resp_capture),
              "a collision response embedding a name with '\"' must still be well-formed JSON -- this is "
              "the exact defect Opus review finding A reported (unescaped name broke the JSON, and the "
              "page's r.json() threw instead of showing the refusal reason)");
    TEST_CHECK(strstr(s_resp_capture, "Bisque \\\"10\\\"") != NULL,
              "the escaped name must still be recoverable from the JSON (backslash-escaped quotes)");
}

static void test_profile_post_handler_collision_response_escapes_newline_in_name(void)
{
    /* Opus review of 5dd23944, finding 3: the old per-file json_escape()
     * copies only escaped '"' and '\\' -- a raw control byte (a literal
     * newline here, decoded from %0A the same way a browser-submitted form
     * body would carry one) landed unescaped in the JSON body and broke it,
     * exactly like the unescaped-quote defect finding A already covered.
     * The shared profiles_http_json_escape() in profiles_http_internal.h now also emits
     * \u00XX for any byte < 0x20. PROFILE_NAME_MAX_LEN is 15, so this name
     * (plus the quote from finding A, both together per the review) stays
     * short: "A\n\"B" (4 chars decoded). */
    TEST_SECTION("profile_post_handler() -- a dup-name collision response stays well-formed JSON even "
                 "when the colliding name contains a raw newline AND a '\"' (Opus review finding 3)");
    memset(&s_profiles, 0, sizeof(s_profiles));

    /* F5 (WEB_UI_XSS_AUDIT_2026-10-09): such a name can no longer be CREATED over HTTP --
     * the parse layer refuses a control byte with 400. The escaper itself is
     * covered for control bytes by test_dashboard_json.c / test_http_auth_enforce.c. */
    char body0[256];
    build_minimal_post_body(body0, sizeof(body0), "A%0A%22B");
    s_resp_capture[0] = '\0';
    (void)run_profile_post(body0);
    TEST_CHECK(strstr(s_resp_capture, "control character") != NULL,
              "a new profile whose name carries a newline is refused as a control character");

    char body1[256];
    build_minimal_post_body(body1, sizeof(body1), "Aplain");
    TEST_CHECK(run_profile_post(body1) == ESP_OK, "setup: the first save must succeed");
    TEST_CHECK(run_profile_post(body1) == ESP_OK, "handler must still return ESP_OK on a refusal");
    TEST_CHECK(json_is_well_formed(s_resp_capture), "collision response is well-formed JSON");
}

// ---------------------------------------------------------------------------
// WP-4 follow-up: the rule/target checks must hold on EVERY entry point, not
// just profiles_http_save(). POST /api/profile and the live-edit accept path
// both run profiles_validate_candidate() (live edit in HARD mode), which used
// to skip validate_on_off_rules() entirely. Also: at most one rule per
// (segment, target).
// ---------------------------------------------------------------------------
static profile_t aux_candidate_one_segment(void)
{
    profile_t p;
    memset(&p, 0, sizeof(p));
    strcpy(p.name, "AuxCand");
    p.zone_mask = 0x01;
    p.segment_count = 1;
    p.segments[0].target_c = 100.0f;
    p.segments[0].ramp_c_per_hr = 50.0f;
    p.segments[0].dwell_min = 5;
    g_stub_zone_max_temp_c[0] = 1300.0f;
    g_stub_zone_max_ramp_c_per_hr[0] = 1000.0f;
    return p;
}

static void test_aux_rule_checks_hold_on_every_entry_point(void)
{
    TEST_SECTION("rule target checks run from profiles_validate_candidate (POST + live edit) and duplicates are refused");
    char err[224];
    memset(g_stub_aux, 0, sizeof(g_stub_aux));

    for (int mode = 0; mode < 2; mode++) {
        profile_validate_mode_t m = mode == 0 ? PROFILE_VALIDATE_HARD : PROFILE_VALIDATE_ADVISORY;
        char warn[256];
        profile_t p = aux_candidate_one_segment();
        p.on_off_rule_count = 1;
        p.on_off_rules[0].zone_index = 11; /* aux relay 4, currently disabled */
        p.on_off_rules[0].enable = 1;
        err[0] = '\0';
        TEST_CHECK(!profiles_validate_candidate(&p, m, warn, sizeof(warn), err, sizeof(err)) &&
                       strstr(err, "not an enabled aux output"),
                  "candidate with a disabled-aux rule target is refused (HARD and ADVISORY)");
        p.on_off_rules[0].zone_index = 200;
        err[0] = '\0';
        TEST_CHECK(!profiles_validate_candidate(&p, m, warn, sizeof(warn), err, sizeof(err)) &&
                       strstr(err, "out of range"),
                  "candidate with an out-of-range rule target is refused (HARD and ADVISORY)");

        g_stub_aux[3].enabled = true;
        p.on_off_rules[0].zone_index = 11;
        TEST_CHECK(profiles_validate_candidate(&p, m, warn, sizeof(warn), err, sizeof(err)),
                  "the same candidate is accepted once that aux is enabled");
        p.on_off_rule_count = 2;
        p.on_off_rules[1] = p.on_off_rules[0];
        err[0] = '\0';
        TEST_CHECK(!profiles_validate_candidate(&p, m, warn, sizeof(warn), err, sizeof(err)) &&
                       strstr(err, "duplicates rule 0"),
                  "a second rule on the same (segment, target) is refused");
        memset(g_stub_aux, 0, sizeof(g_stub_aux));
    }

    /* Same target on a DIFFERENT segment is fine. */
    memset(g_stub_aux, 0, sizeof(g_stub_aux));
    g_stub_aux[3].enabled = true;
    profile_t q = aux_candidate_one_segment();
    q.segment_count = 2;
    q.segments[1] = q.segments[0];
    q.on_off_rule_count = 2;
    q.on_off_rules[0].zone_index = 11;
    q.on_off_rules[0].segment_index = 0;
    q.on_off_rules[1].zone_index = 11;
    q.on_off_rules[1].segment_index = 1;
    TEST_CHECK(validate_on_off_rules(&q, err, sizeof(err)), "same target on different segments is accepted");
    memset(g_stub_aux, 0, sizeof(g_stub_aux));

    /* RELAY_IO segment on an aux-bound relay, via the candidate validator. */
    g_stub_aux[3].enabled = true;
    profile_t r = aux_candidate_one_segment();
    r.segments[0].seg_kind = PROFILE_SEG_KIND_RELAY_IO;
    r.segments[0].io_state = 1;
    r.segments[0].io_blocking = 1;
    r.segments[0].io_target = 4;
    g_zone_relay_mask_zone0 = 0x00;
    err[0] = '\0';
    TEST_CHECK(!profiles_validate_candidate(&r, PROFILE_VALIDATE_HARD, NULL, 0, err, sizeof(err)) &&
                   strstr(err, "aux output"),
              "live-edit (HARD) candidate with a RELAY_IO segment on an aux relay is refused");
    memset(g_stub_aux, 0, sizeof(g_stub_aux));

    /* POST /api/profile with a rule aimed at a disabled aux / out-of-range / duplicate. */
    const char *bad_bodies[3] = {
        "id=-1&name=AuxPostA&zone_mask=1&seg_count=1&seg0_target=100&seg0_ramp=50&seg0_dwell=5"
        "&rule0_zone=11&rule0_segment=0&rule0_enable=1",
        "id=-1&name=AuxPostB&zone_mask=1&seg_count=1&seg0_target=100&seg0_ramp=50&seg0_dwell=5"
        "&rule0_zone=200&rule0_segment=0&rule0_enable=1",
        "id=-1&name=AuxPostC&zone_mask=1&seg_count=1&seg0_target=100&seg0_ramp=50&seg0_dwell=5"
        "&rule0_zone=11&rule0_segment=0&rule0_enable=1&rule1_zone=11&rule1_segment=0&rule1_enable=1",
    };
    const char *want[3] = { "not an enabled aux output", "out of range", "duplicates rule 0" };
    for (int i = 0; i < 3; i++) {
        memset(&s_profiles, 0, sizeof(s_profiles));
        g_stub_aux[3].enabled = (i == 2); /* the duplicate case must fail on duplication, not on a disabled aux */
        TEST_CHECK(run_profile_post(bad_bodies[i]) == ESP_OK, "handler replies (400 body), not a transport error");
        TEST_CHECK(strstr(s_resp_capture, "\"ok\":false") != NULL && strstr(s_resp_capture, want[i]) != NULL,
                  "POST /api/profile refuses the bad rule with the validator's own message");
    }
    memset(g_stub_aux, 0, sizeof(g_stub_aux));
}

static void test_profile_post_handler_allows_builtin_name(void)
{
    /* Opus review nit N2: profile_post_handler() (POST /api/profile) is the
     * actual handler behind "Save"/"Save As" and copyBuiltin() -- covers it
     * the same way test_profiles_http_save_allows_builtin_name() already
     * covers profiles_http_save() (profiles_edit_http.c:566's live_edit_
     * name_collides_ex() call with include_builtins=false). Negative-tested:
     * flipping that literal false->true here reproduces a wrongful 400. */
    TEST_SECTION("profile_post_handler() -- a name matching the read-only builtin catalogue is ALLOWED "
                 "(Opus review nit N2 -- covers profiles_edit_http.c:566)");
    memset(&s_profiles, 0, sizeof(s_profiles));
    g_fake_builtin_on = true;
    strcpy((char *)g_fake_builtin.code, "C6DHSC");

    char body[256];
    build_minimal_post_body(body, sizeof(body), "c6dhsc");
    esp_err_t err = run_profile_post(body);

    g_fake_builtin_on = false;

    TEST_CHECK(err == ESP_OK, "the handler must return ESP_OK");
    TEST_CHECK(strstr(s_resp_capture, "\"ok\":true") != NULL,
              "a save naming itself after a builtin's code must succeed, not be refused as a 400 collision");
}

static void test_profiles_list_marks_exceeds_ceiling(void)
{
    TEST_SECTION("profiles_list_get_handler -- a saved over-ceiling profile is marked "
                 "\"exceeds_ceiling\":true in the list response, so the UI can surface it without a "
                 "failed run first");
    memset(&s_profiles, 0, sizeof(s_profiles));
    g_stub_zone_max_temp_c[0] = 80.0f;

    profile_t p;
    memset(&p, 0, sizeof(p));
    strcpy(p.name, "Gas Kiln");
    p.zone_mask = 0x01;
    p.segment_count = 1;
    p.segments[0].target_c = 2015.0f;
    p.segments[0].ramp_c_per_hr = 0.0f;
    p.segments[0].dwell_min = 5;
    uint8_t out_id = 0, warn_count = 0;
    char err_msg[160] = "";
    TEST_CHECK(profiles_http_save(PROFILES_MAX_COUNT, &p, &out_id, &warn_count, err_msg, sizeof(err_msg)),
              "setup: the gas-kiln profile must save");

    s_chunk_capture_len = 0;
    s_chunk_capture[0] = '\0';
    s_chunk_capture_on = true;
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = profiles_list_get_handler(&req);
    s_chunk_capture_on = false;

    TEST_CHECK(err == ESP_OK, "the list handler must not error");
    TEST_CHECK(strstr(s_chunk_capture, "\"exceeds_ceiling\":true") != NULL,
              "the over-ceiling profile must be marked exceeds_ceiling:true in the list");
}

// ---- profiles_validate_candidate() -- docs/LIVE_PROFILE_EDIT.md section 7/8 -------
// The one function the save handler, the live-edit handler and the executor's pickup
// check all share. These tests exercise it directly (no httpd_req_t needed -- it reads
// only zone config), proving HARD refuses what ADVISORY only warns about, and that the
// two modes otherwise agree.

static void test_validate_candidate_hard_mode_refuses_target_above_zone_ceiling(void)
{
    TEST_SECTION("profiles_validate_candidate HARD -- a target above the zone's max_temp_c is refused, "
                 "not merely warned (the live-edit rule, stricter than save-time)");
    g_stub_zone_max_temp_c[0] = 80.0f;
    g_stub_zone_max_ramp_c_per_hr[0] = 1000.0f;

    profile_t p;
    memset(&p, 0, sizeof(p));
    strcpy(p.name, "Over Ceiling");
    p.zone_mask = 0x01;
    p.segment_count = 1;
    p.segments[0].target_c = 80.1f;
    p.segments[0].ramp_c_per_hr = 0.0f;
    p.segments[0].dwell_min = 5;

    char err[224] = "";
    bool ok = profiles_validate_candidate(&p, PROFILE_VALIDATE_HARD, NULL, 0, err, sizeof(err));
    TEST_CHECK(!ok, "80.1C against an 80C ceiling must be refused in HARD mode");
    TEST_CHECK(strstr(err, "segment 1") != NULL, "the refusal must name the segment");

    // Same candidate in ADVISORY mode must still save (this is the existing,
    // unchanged save-time behavior -- proves the two modes share one function
    // and differ only by mode, per section 11's test-coverage requirement).
    char warn[512] = "";
    char err2[224] = "";
    bool ok2 = profiles_validate_candidate(&p, PROFILE_VALIDATE_ADVISORY, warn, sizeof(warn), err2, sizeof(err2));
    TEST_CHECK(ok2, "the identical candidate must still be ACCEPTED in ADVISORY mode");
    TEST_CHECK(strstr(warn, "80") != NULL, "ADVISORY mode must still surface the condition as a warning");
}

static void test_validate_candidate_hard_mode_refuses_ramp_above_zone_ceiling(void)
{
    TEST_SECTION("profiles_validate_candidate HARD -- a ramp rate above the zone's max_ramp_c_per_hr "
                 "is refused");
    g_stub_zone_max_temp_c[0] = 200.0f;
    g_stub_zone_max_ramp_c_per_hr[0] = 100.0f;

    profile_t p;
    memset(&p, 0, sizeof(p));
    strcpy(p.name, "Over Ramp");
    p.zone_mask = 0x01;
    p.segment_count = 1;
    p.segments[0].target_c = 150.0f;
    p.segments[0].ramp_c_per_hr = 150.0f; /* > the 100 C/hr ceiling */
    p.segments[0].dwell_min = 5;

    char err[224] = "";
    bool ok = profiles_validate_candidate(&p, PROFILE_VALIDATE_HARD, NULL, 0, err, sizeof(err));
    TEST_CHECK(!ok, "150 C/hr against a 100 C/hr ceiling must be refused in HARD mode");
}

static void test_validate_candidate_hard_mode_refuses_uncommissioned_zone(void)
{
    TEST_SECTION("profiles_validate_candidate HARD -- max_temp_c == 0 (uncommissioned) is refused, "
                 "never treated as an infinite ceiling");
    g_stub_zone_max_temp_c[0] = 0.0f;
    g_stub_zone_max_ramp_c_per_hr[0] = 1000.0f;

    profile_t p;
    memset(&p, 0, sizeof(p));
    strcpy(p.name, "Uncommissioned");
    p.zone_mask = 0x01;
    p.segment_count = 1;
    p.segments[0].target_c = 50.0f;
    p.segments[0].ramp_c_per_hr = 0.0f;
    p.segments[0].dwell_min = 5;

    char err[224] = "";
    bool ok = profiles_validate_candidate(&p, PROFILE_VALIDATE_HARD, NULL, 0, err, sizeof(err));
    TEST_CHECK(!ok, "an uncommissioned zone (max_temp_c == 0) must refuse a HARD-mode edit, not pass it "
                    "as if there were no ceiling");
}

static void test_validate_candidate_hard_mode_20pct_ramp_band_still_only_warns(void)
{
    TEST_SECTION("profiles_validate_candidate HARD -- a ramp rate within 20%% of the ceiling (but not "
                 "over it) still only warns, in both modes, matching PROFILE_RAMP_WARN_FRACTION");
    g_stub_zone_max_temp_c[0] = 200.0f;
    g_stub_zone_max_ramp_c_per_hr[0] = 100.0f;

    profile_t p;
    memset(&p, 0, sizeof(p));
    strcpy(p.name, "Near Ramp Ceiling");
    p.zone_mask = 0x01;
    p.segment_count = 1;
    p.segments[0].target_c = 150.0f;
    p.segments[0].ramp_c_per_hr = 85.0f; /* within 20% of 100, but not over it */
    p.segments[0].dwell_min = 5;

    char warn[512] = "";
    char err[224] = "";
    bool ok = profiles_validate_candidate(&p, PROFILE_VALIDATE_HARD, warn, sizeof(warn), err, sizeof(err));
    TEST_CHECK(ok, "85 C/hr against a 100 C/hr ceiling is within the warn band, not over it -- must be accepted");
    TEST_CHECK(strstr(warn, "20%") != NULL, "the 20%% warning must still fire in HARD mode");

    // Restore every zone's stub ramp/temp ceiling to its pre-existing
    // default. g_stub_zone_max_ramp_c_per_hr is a global this group of tests
    // is the only caller to override away from 1000.0f -- leaving zone 0 at
    // 100.0f here would silently poison every later test in this file that
    // exercises a >100 C/hr ramp on zone 0 (e.g. make_stored_profile()'s
    // 150 C/hr segment), since nothing else in the file resets it.
    for (int zi = 0; zi < 8; zi++) {
        g_stub_zone_max_ramp_c_per_hr[zi] = 1000.0f;
    }
    g_stub_zone_max_temp_c[0] = 0.0f;
}

static void test_nvs_save_slot_refuses_when_calling_stack_is_external_ram(void)
{
    TEST_SECTION("nvs_save_slot -- refuses (does not crash) when called with a PSRAM stack "
                 "underneath it (DRAM_PSRAM_PLAN.md section 7.2 safety net)");
    memset(&s_profiles, 0, sizeof(s_profiles));

    fake_kv_set_write_safe_here(false); // simulate being called from a PSRAM-stacked task

    esp_err_t err = nvs_save_slot(0);

    TEST_CHECK(err == ESP_ERR_INVALID_STATE,
               "the wrong-task guard refuses with a diagnosable error, not a crash -- exactly "
               "the class of bug (an NVS write reached from a PSRAM-stack task) this net exists "
               "to catch before a future task relocation (DRAM_PSRAM_PLAN.md section 7) makes it "
               "reachable for real");

    fake_kv_set_write_safe_here(true); // leave shared fake state as every other test expects
}

static void test_nvs_save_slot_proceeds_normally_on_an_internal_ram_stack(void)
{
    TEST_SECTION("nvs_save_slot -- proceeds normally when the calling task's stack is internal RAM");
    nvs_stub_reset();

    // fake_kv_set_write_safe_here(true) is the fake's default state.
    esp_err_t err = nvs_save_slot(0);

    TEST_CHECK(err == ESP_OK, "the guard does not fire on an internal-RAM stack -- the write "
                              "proceeds exactly as before this net was added");
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Builtin catalogue JSON: seg_kind and a resolved zone_mask.
//
// Both fields were missing/zero before 2026-09-09, which made main_page.html's
// computeProfileLimitWarning() silently inert for the ENTIRE shipped builtin
// catalogue -- the class most likely to exceed a bench board's ceiling
// (BQ1000 peaks over 1000C; this bench's zones are capped at 80C). The JS side
// walks zone_mask's bits (0 -> no zone checked) and requires seg_kind === 0
// (absent -> `undefined !== 0` -> every segment skipped), so EITHER omission
// alone was enough to hide the warning.
//
// This drives the real emitters, not a hand-written fixture: send_builtin_*
// are the same static functions builtin_list_get_handler() and
// profile_detail_get_handler() call.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// profiles_slot_bitmap_t widening (docs/PROFILE_SLOTS_100.md section 7
// task 1) -- two REQUIRED regression tests named by that task:
//   1. the persisted NVS_KEY_USED byte for a fixed 8-slot fixture must stay
//      byte-identical to the pre-widening uint8_t scalar format.
//   2. ids 31, 32, 33, 99 (each landing in a different word of the new
//      4-word bitmap, and each >= 32 -- the exact width that made the old
//      `1u << id` scalar test undefined behavior) must round-trip through
//      the accessors.
// ---------------------------------------------------------------------------
/* Fixed at 8, deliberately NOT PROFILES_MAX_COUNT: this test's whole point is
 * a small fixture that used to fit in the pre-widening single scalar byte,
 * independent of wherever PROFILES_MAX_COUNT sits today (100 as of task 6). */
#define SLOT_BITMAP_FIXTURE_SLOTS 8

static void test_slot_bitmap_legacy_u8_migrates_on_read(void)
{
    TEST_SECTION("profiles_slot_bitmap_t -- a pre-widening single-byte NVS_KEY_USED "
                 "(0xFF, 8 slots) migrates losslessly on read");

    nvs_stub_reset();
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0);

    /* nvs_load_all_from() clears a slot's used bit unless a decodable "profN"
     * blob is also present at that id (one bad/missing slot must not take
     * any other slot down, TODO.md 8.1) -- stage a real decodable blob at
     * each of the 8 fixture ids so this test actually exercises the
     * used_bitmap migration path rather than tripping that unrelated guard. */
    profile_t src = make_stored_profile();
    profile_persisted_t persisted = { .version = PROFILE_VERSION, .profile = src, .crc32 = 0 };
    persisted.crc32 = compute_profile_crc(&persisted);
    for (uint8_t id = 0; id < SLOT_BITMAP_FIXTURE_SLOTS; id++) {
        stage_profile_blob(id, &persisted, sizeof(persisted));
    }

    nvs_handle_t h;
    nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    esp_err_t err = nvs_set_u8(h, NVS_KEY_USED, 0xFFu);
    TEST_CHECK(err == ESP_OK, "staging the legacy scalar byte must succeed");
    nvs_commit(h);
    nvs_close(h);

    profiles_state_t out;
    bool any_found = false;
    err = nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found);
    TEST_CHECK(err == ESP_OK, "reading a legacy single-byte used_bitmap must not error");
    for (uint8_t id = 0; id < SLOT_BITMAP_FIXTURE_SLOTS; id++) {
        TEST_CHECK(profiles_slot_bitmap_test(&out.used_bitmap, id),
                  "legacy bit for a fixture slot must migrate to the widened bitmap");
    }
    for (uint8_t id = SLOT_BITMAP_FIXTURE_SLOTS; id < PROFILES_MAX_COUNT; id++) {
        TEST_CHECK(!profiles_slot_bitmap_test(&out.used_bitmap, id),
                  "a legacy byte only ever set bits 0..7 -- no other id may come back used");
    }

    nvs_stub_reset();
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0);
}

static void test_slot_bitmap_persisted_byte_identical_for_8slot_fixture(void)
{
    TEST_SECTION("profiles_slot_bitmap_t -- saving a full 8-slot fixture persists the "
                 "widened blob and round-trips through the real loader");

    nvs_stub_reset();
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0);
    for (uint8_t id = 0; id < SLOT_BITMAP_FIXTURE_SLOTS; id++) {
        profiles_slot_set(id);
    }
    TEST_CHECK(profiles_slot_bitmap_to_u32(&s_profiles.used_bitmap) == 0xFFu,
              "all 8 fixture slots set must reduce to the same 0xFF word[0] the old scalar held");

    /* nvs_save_slot() persists the whole used_bitmap as a side effect of
     * saving any one slot -- stage a real, decodable profile at every one of
     * the 8 fixture slots so the reload below (which re-derives its own used
     * bit per slot from a successful decode, not just from the raw
     * persisted bitmap) actually reports all 8 as used. */
    profile_t src = make_stored_profile();
    esp_err_t err = ESP_OK;
    for (uint8_t id = 0; id < SLOT_BITMAP_FIXTURE_SLOTS; id++) {
        s_profiles.profiles[id] = src;
        esp_err_t e = nvs_save_slot(id);
        if (e != ESP_OK) {
            err = e;
        }
    }
    TEST_CHECK(err == ESP_OK, "saving all 8 fixture slots as used must not error");

    /* Saves are cfg-file-only: no prof_used key (legacy or widened) is
     * written to NVS any more, and the loader derives the used bitmap from the
     * files alone (no kiln_cfg namespace exists on this partition). */
    {
        nvs_handle_t h;
        profiles_slot_bitmap_t persisted;
        size_t len = sizeof(persisted);
        bool nvs_has_used = nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK &&
                            (nvs_get_blob(h, NVS_KEY_USED, &persisted, &len) == ESP_OK);
        TEST_CHECK(!nvs_has_used, "a save must not write the prof_used key to NVS");
    }

    /* The round trip back through the real loader reconstructs the 8-slot
     * set from the cfg files. */
    profiles_state_t out;
    bool any_found = false;
    err = nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found);
    TEST_CHECK(err == ESP_OK && any_found, "reload after the fixture save must succeed (files-only path)");
    TEST_CHECK(profiles_slot_bitmap_to_u32(&out.used_bitmap) == 0xFFu,
              "word[0] of the reloaded bitmap for an 8-slot fixture must be 0xFF");
    for (uint8_t id = 0; id < SLOT_BITMAP_FIXTURE_SLOTS; id++) {
        TEST_CHECK(profiles_slot_bitmap_test(&out.used_bitmap, id),
                  "every one of the 8 fixture slots must still read back used");
    }

    /* Leave global state clean for later tests. */
    nvs_stub_reset();
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0);
}

static void test_slot_bitmap_round_trips_high_ids(void)
{
    TEST_SECTION("profiles_slot_bitmap_t -- ids 31, 32, 33, 99 round-trip through the "
                 "accessors (32 is the exact width that made the old `1u << id` scalar "
                 "test undefined behavior; 31/33 flank the word-0/word-1 boundary; 99 "
                 "lands in word 3)");

    const unsigned ids[] = {31, 32, 33, 99};
    profiles_slot_bitmap_t bm;
    profiles_slot_bitmap_from_u32(&bm, 0);

    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        TEST_CHECK(!profiles_slot_bitmap_test(&bm, ids[i]), "id must start clear");
    }

    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        profiles_slot_bitmap_set(&bm, ids[i]);
        TEST_CHECK(profiles_slot_bitmap_test(&bm, ids[i]), "id must read back set immediately after set");
    }
    /* Setting one id must not disturb any other -- the whole point of using
     * separate words rather than one overflowing scalar. */
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        TEST_CHECK(profiles_slot_bitmap_test(&bm, ids[i]), "id must still read back set after siblings were set");
    }
    TEST_CHECK(!profiles_slot_bitmap_test(&bm, 0), "an unrelated low id must remain untouched");
    TEST_CHECK(!profiles_slot_bitmap_test(&bm, 63), "an unrelated word-1/word-2 boundary id must remain untouched");
    /* word[0] covers bits 0..31 -- of the four test ids, only 31 lands in it.
     * The persistence seam (profiles_slot_bitmap_to_u32()) only ever reads
     * word[0], so this pins down exactly what a persist would see: id 31's
     * bit, and nothing leaked in from 32/33/99. */
    TEST_CHECK(profiles_slot_bitmap_to_u32(&bm) == (1u << 31),
              "only id 31 lives in word[0] -- 32/33/99 must not leak a bit into the "
              "persisted word even though all four ids are set in the wider struct");

    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        profiles_slot_bitmap_clear(&bm, ids[i]);
        TEST_CHECK(!profiles_slot_bitmap_test(&bm, ids[i]), "id must read back clear immediately after clear");
    }
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        TEST_CHECK(!profiles_slot_bitmap_test(&bm, ids[i]), "id must still read back clear after siblings were cleared");
    }
}

// docs/PROFILE_SLOTS_100.md section 5's explicit task-6 regression: fill
// all 100 slots, list, delete slot 50, then save with requested_id ==
// PROFILES_MAX_COUNT (the "first free slot" sentinel) and confirm the new
// profile lands back in the one hole, slot 50.
static void test_profiles_http_save_fills_all_100_then_reuses_deleted_slot(void)
{
    TEST_SECTION("profiles_http_save/_delete -- fill all 100 slots, delete slot 50, "
                 "then a first-free-slot save lands back in 50");

    nvs_stub_reset();
    memset(&s_profiles, 0, sizeof(s_profiles));
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0);

    profile_t p = make_stored_profile();
    for (uint8_t id = 0; id < PROFILES_MAX_COUNT; id++) {
        uint8_t out_id = 0, warn_count = 0;
        char err_msg[128];
        snprintf(p.name, sizeof(p.name), "P%u", (unsigned)id);
        bool ok = profiles_http_save(id, &p, &out_id, &warn_count, err_msg, sizeof(err_msg));
        TEST_CHECK(ok, "saving directly into each of the 100 slots by id must succeed");
        TEST_CHECK(out_id == id, "a save aimed at a specific empty slot must land there");
    }
    for (uint8_t id = 0; id < PROFILES_MAX_COUNT; id++) {
        TEST_CHECK(profiles_slot_bitmap_test(&s_profiles.used_bitmap, id),
                  "every one of the 100 slots must read back used after the fill");
    }

    /* A first-free-slot save now must refuse -- there is no hole. */
    {
        uint8_t out_id = 0, warn_count = 0;
        char err_msg[128];
        snprintf(p.name, sizeof(p.name), "Overflow");
        bool ok = profiles_http_save(PROFILES_MAX_COUNT, &p, &out_id, &warn_count, err_msg, sizeof(err_msg));
        TEST_CHECK(!ok, "a first-free-slot save with all 100 slots full must be refused, not overwrite anything");
    }

    TEST_CHECK(profiles_http_delete(50), "deleting slot 50 out of a full board must succeed");
    TEST_CHECK(!profiles_slot_bitmap_test(&s_profiles.used_bitmap, 50), "slot 50 must read back unused after delete");

    uint8_t out_id = 0, warn_count = 0;
    char err_msg[128];
    snprintf(p.name, sizeof(p.name), "Refill");
    bool ok = profiles_http_save(PROFILES_MAX_COUNT, &p, &out_id, &warn_count, err_msg, sizeof(err_msg));
    TEST_CHECK(ok, "a first-free-slot save with exactly one hole must succeed");
    TEST_CHECK(out_id == 50, "the first-free-slot save must land in the one deleted hole, slot 50, not append past 99");
    TEST_CHECK(profiles_slot_bitmap_test(&s_profiles.used_bitmap, 50), "slot 50 must read back used again after the refill");

    nvs_stub_reset();
    memset(&s_profiles, 0, sizeof(s_profiles));
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0);
}

// docs/PROFILE_SLOTS_100.md's "Owner decision, 2026-09-19 (post phase-A
// review)": PROFILE_BENCH_SLOT_ID (101) must be structurally invisible --
// never in the catalogue, never favoritable, never reachable through the
// LCD-picker ordering module -- exactly like LIVE_EDIT_WORKING_SLOT_ID (100)
// already is. profiles_bench_slot.h documents this as "free by construction"
// (every ordinary loop is bounded by PROFILES_MAX_COUNT); this test pins
// that claim down for the three call sites the plan names.
static void test_bench_slot_id_excluded_from_catalogue_favorites_and_lcd_order(void)
{
    TEST_SECTION("PROFILE_BENCH_SLOT_ID (101) is excluded from the catalogue loop bound, "
                 "the favorites accessor, and the LCD picker's deletable-id range");

    TEST_CHECK(PROFILE_BENCH_SLOT_ID == LIVE_EDIT_WORKING_SLOT_ID + 1,
              "bench slot id must be exactly one past the live-edit working slot");
    TEST_CHECK(PROFILE_BENCH_SLOT_ID == PROFILES_MAX_COUNT + 1,
              "at today's PROFILES_MAX_COUNT (100) the bench slot must be id 101");
    TEST_CHECK(PROFILE_BENCH_SLOT_ID < PROFILE_BUILTIN_ID_BASE,
              "the bench slot must still leave every builtin id (>= 128) untouched");

    /* Catalogue/favorites loop bound: PROFILE_BENCH_SLOT_ID is never < PROFILES_MAX_COUNT,
     * so any `for (id = 0; id < PROFILES_MAX_COUNT; id++)` loop -- profiles_list_get_handler(),
     * favorites_list_get_handler(), profiles_export_http.c, backup export/import,
     * ui_page_profile_picker.c's catalogue build -- never reaches it. */
    TEST_CHECK(!(PROFILE_BENCH_SLOT_ID < PROFILES_MAX_COUNT),
              "the bench slot id must fall outside every ordinary 0..PROFILES_MAX_COUNT-1 loop");

    /* Favorites: fav_locate() rejects it -- neither a user slot nor a valid
     * builtin id, so profiles_favorites_is()/_set() answer false/refuse. */
    TEST_CHECK(profiles_favorites_is((uint8_t)PROFILE_BENCH_SLOT_ID) == false,
              "the bench slot must never read back as favorited");
    esp_err_t fav_err = profiles_favorites_set((uint8_t)PROFILE_BENCH_SLOT_ID, true);
    TEST_CHECK(fav_err == ESP_ERR_INVALID_ARG,
              "favoriting the bench slot id must be refused, exactly like any other out-of-range id");

    /* LCD picker: ui_page_profile_picker_is_deletable() must also say no --
     * same structural test the live-edit slot already gets. */
    TEST_CHECK(ui_page_profile_picker_is_deletable((uint8_t)PROFILE_BENCH_SLOT_ID) == false,
              "the LCD picker must never treat the bench slot as a deletable user slot");
}

static void test_builtin_json_emits_seg_kind_and_resolved_zone_mask(void)
{
    TEST_SECTION("builtin catalogue JSON carries seg_kind and the zone_mask the schedule "
                 "would really run on, so the dashboard's configured-limit warning is not "
                 "silently inert for every shipped profile");

    g_stub_thermo_count = 3; /* a realistic board, and 0x7 != 0xFF */
    memset(&g_fake_builtin, 0, sizeof(g_fake_builtin));
    memcpy((char *)g_fake_builtin.code, "BQ1000", 7);
    g_fake_builtin.title = "Bench Bisque 1000";
    g_fake_builtin.slug = "bench-bisque";
    g_fake_builtin.family = "Test";
    g_fake_builtin.firing_type = PROFILE_FIRING_BISQUE;
    g_fake_builtin.segment_count = 2;
    g_fake_builtin.segments[0].target_c = 600.0f;
    g_fake_builtin.segments[0].ramp_c_per_hr = 100.0f;
    g_fake_builtin.segments[1].target_c = 1000.0f;
    g_fake_builtin.segments[1].ramp_c_per_hr = 60.0f;

    memset(&g_fake_builtin_profile, 0, sizeof(g_fake_builtin_profile));
    g_fake_builtin_profile.segment_count = g_fake_builtin.segment_count;
    g_fake_builtin_profile.segments[0] = g_fake_builtin.segments[0];
    g_fake_builtin_profile.segments[1] = g_fake_builtin.segments[1];
    g_fake_builtin_on = true;
    g_last_feasibility_zone_mask = 0xAA;

    httpd_req_t req;
    memset(&req, 0, sizeof(req));

    s_chunk_capture_len = 0;
    s_chunk_capture[0] = '\0';
    s_chunk_capture_on = true;
    esp_err_t err = send_builtin_full(&req, PROFILE_BUILTIN_ID_BASE, &g_fake_builtin, true);
    s_chunk_capture_on = false;

    TEST_CHECK(err == ESP_OK, "the builtin detail emitter must not error");
    TEST_CHECK(strstr(s_chunk_capture, "\"zone_mask\":7") != NULL,
              "a builtin must report the zone set it would actually run on (all 3 configured "
              "zones -> 7), not the literal 0 that made every consumer check no zones at all");
    TEST_CHECK(strstr(s_chunk_capture, "\"zone_mask\":0,") == NULL,
              "the old hardcoded zero mask must be gone");
    TEST_CHECK(count_occurrences(s_chunk_capture, "\"seg_kind\":0") == 2,
              "every builtin segment must state seg_kind explicitly -- a missing field must "
              "never be left for a consumer to guess as kind 0");
    TEST_CHECK(g_last_feasibility_zone_mask == 0x7,
              "the same resolved mask must reach the feasibility roll-up, so the emitted "
              "zone_mask and the emitted feasibility verdict describe one zone set, not two");
    TEST_CHECK(json_is_well_formed(s_chunk_capture),
              "the widened per-segment object must still fit its chunk buffer -- a truncated "
              "chunk would be the silent failure mode of adding a field here");

    /* The summary emitter (GET /api/profiles' listing rows) resolves the same
     * way -- the dashboard's <select> is populated from THAT response, so a
     * mask of 0 there would mislabel every builtin as targeting no zones. */
    s_chunk_capture_len = 0;
    s_chunk_capture[0] = '\0';
    s_chunk_capture_on = true;
    err = send_builtin_summary(&req, PROFILE_BUILTIN_ID_BASE, &g_fake_builtin, true);
    s_chunk_capture_on = false;
    TEST_CHECK(err == ESP_OK, "the builtin summary emitter must not error");
    TEST_CHECK(strstr(s_chunk_capture, "\"zone_mask\":7") != NULL,
              "the listing summary must resolve the mask the same way the detail response does");

    /* A board with no zones configured honestly reports 0 -- there is no zone
     * to judge against, and profiles_http.c's own getter says the same. */
    g_stub_thermo_count = 0;
    s_chunk_capture_len = 0;
    s_chunk_capture[0] = '\0';
    s_chunk_capture_on = true;
    err = send_builtin_summary(&req, PROFILE_BUILTIN_ID_BASE, &g_fake_builtin, true);
    s_chunk_capture_on = false;
    TEST_CHECK(err == ESP_OK, "the zero-zone case must not error");
    TEST_CHECK(strstr(s_chunk_capture, "\"zone_mask\":0,") != NULL,
              "a board with no zones configured reports 0, not a phantom zone");

    g_fake_builtin_on = false;
    g_stub_thermo_count = 8;
}

static void test_profiles_refused_until_boot_load_done(void)
{
    TEST_SECTION("profile starts refused until the boot load finished (review LOW-1)");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    g_fake_exec_state = PROFILE_EXEC_IDLE;
    g_fake_exec_profile_id = 0xFF;
    profile_t p = make_stored_profile();
    uint8_t out_id = 0xFF;
    char err[128];
    TEST_CHECK(profiles_http_save(4, &p, &out_id, NULL, err, sizeof(err)), "save slot 4");
    profiles_http_test_set_loaded(false);
    TEST_CHECK(!profiles_http_slot_runnable(4), "user slot not runnable before load finished");
    TEST_CHECK(!profiles_http_slot_runnable_rev(4, profiles_http_slot_rev(4)), "rev check refuses before load finished");
    /* review 14 LOW-5: every writer is refused for the whole pre-load window, not only while s_boot_loading */
    {
        uint32_t rev_pre = profiles_http_slot_rev(4);
        TEST_CHECK(!profiles_http_save(4, &p, &out_id, NULL, err, sizeof(err)) && strncmp(err, "busy:", 5) == 0,
                   "save refused busy before the boot load finished");
        TEST_CHECK(profiles_delete_slot(4) == PROFILES_DELETE_BUSY, "delete refused busy before the boot load finished");
        profiles_retarget_counts_t rc;
        TEST_CHECK(!profiles_retarget_zone_to_aux_commit(0, 1, true, &rc, err, sizeof(err)) &&
                       strncmp(err, "busy:", 5) == 0,
                   "retarget commit refused busy before the boot load finished");
        TEST_CHECK(!profiles_retarget_zone_to_aux_resume(0, 1, true, &rc, err, sizeof(err)),
                   "retarget resume refused before the boot load finished");
        TEST_CHECK(!profiles_retarget_zone_to_aux_revert(0, 1), "retarget revert refused before the boot load finished");
        TEST_CHECK(profiles_http_slot_rev(4) == rev_pre && profiles_slot_used(4), "refused writers left slot 4 untouched");
    }
    profiles_http_test_set_loaded(true);
    TEST_CHECK(profiles_http_slot_runnable(4), "runnable once loaded");
    /* profiles_http_start() runs the load and publishes the flag (the stub then returns INVALID_STATE). */
    profiles_http_test_set_loaded(false);
    (void)profiles_http_start();
    TEST_CHECK(profiles_http_slot_runnable(4), "profiles_http_start publishes loaded after the boot load");
    TEST_CHECK((profiles_http_slot_rev(4) & 1u) == 0u, "boot load bracket leaves the generation even");
    /* review LOW-9: a save while the boot load runs is refused busy (409), nothing written */
    profiles_http_test_set_boot_loading(true);
    uint32_t rev_before = profiles_http_slot_rev(4);
    TEST_CHECK(!profiles_http_save(4, &p, &out_id, NULL, err, sizeof(err)) && strncmp(err, "busy:", 5) == 0,
               "save refused busy during boot load");
    TEST_CHECK(profiles_http_slot_rev(4) == rev_before, "refused save did not touch the slot");
    profiles_http_test_set_boot_loading(false);
    TEST_CHECK(profiles_http_save(4, &p, &out_id, NULL, err, sizeof(err)), "save works again after boot load");
}

// HTTP fuzz campaign part 2: hostile POST /api/profile bodies and a body shorter than Content-Length.
static void test_fuzz_profile_post_hostile(void)
{
    TEST_SECTION("profile_post_handler() fuzz -- hostile bodies store nothing; short body is a 400");
    const char *seg = "&zone_mask=1&seg_count=1&seg0_target=100&seg0_ramp=50&seg0_dwell=5";
    static const char *const heads[] = {
        "id=-1", "id=-1&name=",
        "id=-1&name=ThisNameIsFarTooLongForAProfileSlotxxxxxxxxxxxxxxxxxxxx",
        "id=-1&name=A&zone_mask=0", "id=-1&name=A&zone_mask=-1", "id=-1&name=A&zone_mask=99999999999",
        "id=-1&name=A&zone_mask=1&seg_count=0", "id=-1&name=A&zone_mask=1&seg_count=-1",
        "id=-1&name=A&zone_mask=1&seg_count=99999",
        "id=-1&name=A&zone_mask=1&seg_count=1&seg0_target=nan&seg0_ramp=50&seg0_dwell=5",
        "id=-1&name=A&zone_mask=1&seg_count=1&seg0_target=1e999&seg0_ramp=50&seg0_dwell=5",
        "id=-1&name=A&zone_mask=1&seg_count=1&seg0_target=100&seg0_ramp=inf&seg0_dwell=5",
        "id=-1&name=A&zone_mask=1&seg_count=1&seg0_target=100&seg0_ramp=50&seg0_dwell=-1",
        "id=-1&name=A&zone_mask=1&seg_count=1&seg0_target=100&seg0_ramp=50&seg0_dwell=99999999999",
        "id=-1&name=A&zone_mask=1&seg_count=1&seg0_target=99999&seg0_ramp=50&seg0_dwell=5",
        "id=-1&name=A&zone_mask=1&seg_count=1&seg0_target=-500&seg0_ramp=50&seg0_dwell=5",
    };
    for (size_t k = 0; k < sizeof(heads) / sizeof(heads[0]); k++) {
        memset(&s_profiles, 0, sizeof(s_profiles));
        pcfg_reset_all();
        char body[512];
        /* bodies that already carry segment fields get no extra suffix */
        snprintf(body, sizeof(body), "%s%s", heads[k], strstr(heads[k], "seg0_") ? "" : seg);
        run_profile_post(body);
        char msg[200];
        snprintf(msg, sizeof(msg), "hostile profile body #%zu stores nothing (%s)", k, heads[k]);
        TEST_CHECK(!profiles_slot_used(0), msg);
    }
    /* seg_count bigger than the fields supplied */
    memset(&s_profiles, 0, sizeof(s_profiles));
    run_profile_post("id=-1&name=A&zone_mask=1&seg_count=3&seg0_target=100&seg0_ramp=50&seg0_dwell=5");
    TEST_CHECK(!profiles_slot_used(0), "seg_count=3 with only seg0 present stores nothing");

    /* Content-Length longer than the body actually received: recv returns 0 early */
    memset(&s_profiles, 0, sizeof(s_profiles));
    pcfg_reset_all();
    const char *good = "id=-1&name=Short&zone_mask=1&seg_count=1&seg0_target=100&seg0_ramp=50&seg0_dwell=5";
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.content_len = (long long)strlen(good) + 40;
    s_post_body = good;
    s_post_body_left = strlen(good);
    s_last_err_code = 0;
    (void)profile_post_handler(&req);
    s_post_body = NULL;
    s_post_body_left = 0;
    TEST_CHECK(s_last_err_code == 400, "body shorter than Content-Length answers 400");
    TEST_CHECK(!profiles_slot_used(0), "short body stores nothing");

    /* Content-Length 0 and a negative one */
    for (int i = 0; i < 2; i++) {
        memset(&req, 0, sizeof(req));
        req.content_len = i ? -5 : 0;
        s_last_err_code = 0;
        (void)profile_post_handler(&req);
        TEST_CHECK(s_last_err_code == 400 && !profiles_slot_used(0), "empty/negative Content-Length is a 400, nothing stored");
    }
    /* oversized Content-Length never allocates/reads */
    memset(&req, 0, sizeof(req));
    req.content_len = 100 * 1024 * 1024;
    s_last_err_code = 0;
    (void)profile_post_handler(&req);
    TEST_CHECK(s_last_err_code == 400 || s_last_err_code == 413, "100 MB Content-Length refused up front");
}

// HTTP body fuzz part 3: delete / builtin hide / builtin restore / favorite.
// A refused request must erase, hide, unhide or favorite nothing.
static int fuzz_post(esp_err_t (*h)(httpd_req_t *), const char *body, long long clen, size_t fail_after, int fail_ret)
{
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.content_len = clen;
    s_post_body = body;
    s_post_body_left = strlen(body);
    s_post_served = 0;
    s_fail_after = fail_after;
    s_fail_ret = fail_ret;
    s_last_err_code = 0;
    (void)h(&req);
    s_post_body = NULL;
    s_post_body_left = 0;
    s_fail_after = (size_t)-1;
    s_fail_ret = 0;
    return s_last_err_code;
}

static void fuzz_small_setup(void)
{
    memset(&s_profiles, 0, sizeof(s_profiles));
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);
    s_profiles.profiles[8] = make_stored_profile();
    profiles_slot_bitmap_from_u32(&s_profiles.used_bitmap, 0x100);
    (void)nvs_save_slot(8);
    (void)profiles_favorites_set(8, false);
    g_fake_builtin_on = true;
    s_fake_hide_calls = 0;
    s_fake_restore_calls = 0;
}

static void test_fuzz_small_body_handlers(void)
{
    TEST_SECTION("delete/hide/restore/favorite fuzz -- hostile bodies change nothing");
    const char *dgood = "id=8";
    size_t dl = strlen(dgood);
    int st;
    long long lens[] = { 0, -1, 65, 100000000LL, 0x7fffffffffffffffLL, 0 /* body length + 30, set below */ };
    size_t nlens = sizeof(lens) / sizeof(lens[0]);

    // delete: recv failure at every offset, Content-Length extremes, bad ids.
    for (size_t at = 0; at < dl; at++) {
        for (int ret = -1; ret <= 0; ret++) {
            fuzz_small_setup();
            st = fuzz_post(profile_delete_post_handler, dgood, (long long)dl, at, ret);
            TEST_CHECK(st == 400 && profiles_slot_used(8), "delete: recv failure mid-body erases nothing");
        }
    }
    lens[5] = (long long)dl + 30;
    for (size_t i = 0; i < nlens; i++) {
        fuzz_small_setup();
        st = fuzz_post(profile_delete_post_handler, dgood, lens[i], (size_t)-1, 0);
        TEST_CHECK(st == 400 && profiles_slot_used(8), "delete: bad Content-Length erases nothing");
    }
    const char *dbad[] = { "id=", "id=+8", "id=8x", "id=-8", "id=%00", "id=8%00", "id=9999999999999", "id=256",
                           "id=8%", "id=%zz", "xid=8", "id[]=8", "ID=8", "id=1.5", "id=0x8", "id= 8", "id=8 ",
                           "id=%ff",
                           "id=8&pad=pppppppppppppppppppppppppppppppppppppppppppppppppppppppppppp" /* valid id, body over the 64-byte cap */ };
    for (size_t i = 0; i < sizeof(dbad) / sizeof(dbad[0]); i++) {
        fuzz_small_setup();
        st = fuzz_post(profile_delete_post_handler, dbad[i], (long long)strlen(dbad[i]), (size_t)-1, 0);
        char msg[120];
        snprintf(msg, sizeof(msg), "delete: refused, slot 8 intact: %s", dbad[i]);
        TEST_CHECK(st >= 400 && profiles_slot_used(8), msg);
    }
    fuzz_small_setup();
    st = fuzz_post(profile_delete_post_handler, dgood, (long long)dl, (size_t)-1, 0);
    TEST_CHECK(st == 0 && !profiles_slot_used(8), "delete: control, clean body erases the slot");

    // favorite
    const char *fgood = "id=8&favorite=1";
    size_t fl = strlen(fgood);
    for (size_t at = 0; at < fl; at++) {
        for (int ret = -1; ret <= 0; ret++) {
            fuzz_small_setup();
            st = fuzz_post(profile_favorite_post_handler, fgood, (long long)fl, at, ret);
            TEST_CHECK(st == 400 && !profiles_favorites_is(8), "favorite: recv failure mid-body marks nothing");
        }
    }
    lens[5] = (long long)fl + 30;
    for (size_t i = 0; i < nlens; i++) {
        fuzz_small_setup();
        st = fuzz_post(profile_favorite_post_handler, fgood, lens[i], (size_t)-1, 0);
        TEST_CHECK(st == 400 && !profiles_favorites_is(8), "favorite: bad Content-Length marks nothing");
    }
    const char *fbad[] = { "id=8&favorite=2", "id=8&favorite=10", "id=8&favorite=%31%31", "id=8&favorite=1x",
                           "id=8&favorite=+1", "id=8&favorite=%00", "id=8&favorite=-1", "id=%00", "id=8x&favorite=1",
                           "id=", "favorite=1", "id=300&favorite=1", "id=8&favorite=0000000000" };
    for (size_t i = 0; i < sizeof(fbad) / sizeof(fbad[0]); i++) {
        fuzz_small_setup();
        st = fuzz_post(profile_favorite_post_handler, fbad[i], (long long)strlen(fbad[i]), (size_t)-1, 0);
        char msg[120];
        snprintf(msg, sizeof(msg), "favorite: refused, nothing marked: %s", fbad[i]);
        TEST_CHECK(st >= 400 && !profiles_favorites_is(8), msg);
    }
    fuzz_small_setup();
    st = fuzz_post(profile_favorite_post_handler, fgood, (long long)fl, (size_t)-1, 0);
    TEST_CHECK(st == 0 && profiles_favorites_is(8), "favorite: control, clean body marks the slot");

    // builtin hide (fake builtin id is PROFILE_BUILTIN_ID_BASE)
    char hgood[48];
    snprintf(hgood, sizeof(hgood), "id=%d&hidden=1", (int)PROFILE_BUILTIN_ID_BASE);
    size_t hl = strlen(hgood);
    for (size_t at = 0; at < hl; at++) {
        for (int ret = -1; ret <= 0; ret++) {
            fuzz_small_setup();
            st = fuzz_post(builtin_hide_post_handler, hgood, (long long)hl, at, ret);
            TEST_CHECK(st == 400 && s_fake_hide_calls == 0, "hide: recv failure mid-body hides nothing");
        }
    }
    lens[5] = (long long)hl + 30;
    for (size_t i = 0; i < nlens; i++) {
        fuzz_small_setup();
        st = fuzz_post(builtin_hide_post_handler, hgood, lens[i], (size_t)-1, 0);
        TEST_CHECK(st >= 400 && s_fake_hide_calls == 0, "hide: bad Content-Length hides nothing");
    }
    const char *hbad[] = { "id=", "id=-1", "id=+1", "id=%00", "xid=1", "hidden=1", "id=1000", "id=%zz", "id=8" };
    for (size_t i = 0; i < sizeof(hbad) / sizeof(hbad[0]); i++) {
        fuzz_small_setup();
        st = fuzz_post(builtin_hide_post_handler, hbad[i], (long long)strlen(hbad[i]), (size_t)-1, 0);
        char msg[120];
        snprintf(msg, sizeof(msg), "hide: refused, nothing hidden: %s", hbad[i]);
        TEST_CHECK(st >= 400 && s_fake_hide_calls == 0, msg);
    }
    fuzz_small_setup();
    st = fuzz_post(builtin_hide_post_handler, hgood, (long long)hl, (size_t)-1, 0);
    TEST_CHECK(st == 0 && s_fake_hide_calls == 1, "hide: control, clean body hides once");

    // builtin restore: a bad body must not restore.
    for (size_t i = 0; i < nlens; i++) {
        if (lens[i] <= 0) {
            continue; /* empty body is the legitimate form */
        }
        fuzz_small_setup();
        st = fuzz_post(builtin_restore_post_handler, "x=1", lens[i], (size_t)-1, 0);
        if (lens[i] == 3) {
            continue;
        }
        TEST_CHECK(st == 400 && s_fake_restore_calls == 0, "restore: bad Content-Length restores nothing");
    }
    for (int ret = -1; ret <= 0; ret++) {
        fuzz_small_setup();
        st = fuzz_post(builtin_restore_post_handler, "abc", 3, 1, ret);
        TEST_CHECK(st == 400 && s_fake_restore_calls == 0, "restore: recv failure restores nothing");
    }
    fuzz_small_setup();
    st = fuzz_post(builtin_restore_post_handler, "", 0, (size_t)-1, 0);
    TEST_CHECK(st == 0 && s_fake_restore_calls == 1, "restore: control, empty body restores");
    g_fake_builtin_on = false;
}

void run_test_profiles_http(void)
{
    test_fuzz_profile_post_hostile();
    profiles_http_test_set_loaded(true); /* tests below exercise runnable checks without a boot sequence */
    test_legacy_u8_used_key_with_target_not_found();
    test_v1_blob_loads_and_preserves_all_fields();
    test_profiles_refused_until_boot_load_done();
    profiles_http_test_set_loaded(true);
    test_version_zero_rejected();
    test_length_mismatch_rejected();
    test_bad_crc_rejected();
    test_v2_blob_migrates_distinct_multi_segment_values();
    test_v3_blob_migrates_with_no_rules();
    test_on_off_rule_round_trip_through_real_save_and_load();
    test_validate_on_off_rules_rejects_bad_segment_index();
    test_validate_on_off_rules_rejects_heater_zone();
    test_newer_version_refused_not_wiped();
    test_one_bad_slot_does_not_affect_others();
    test_profiles_list_json_valid_with_escape_heavy_names();
    test_profiles_list_floors_unknown_header_covers_free_slots();
    test_profile_detail_json_valid_at_max_capacity();
    test_profiles_list_carries_last_run_started_unix_s();
    test_profiles_list_reports_rev_unknown();
    test_validate_io_segment_zone_ownership();
    test_validate_on_off_rules_aux_targets();
    test_aux_rule_survives_real_save_and_load();
    test_validate_io_segment_refuses_aux_bound_relay();
    test_validate_io_segment_drdy_lcd_gap_refused();
    test_profiles_http_save_accepts_cone10_profile_on_80c_zone();
    test_profiles_http_save_accepts_in_range_profile();
    test_profiles_http_save_accepts_target_exactly_at_zone_limit();
    test_profiles_http_save_accepts_one_degree_over_zone_limit();
    test_profiles_http_save_accepts_2015c_gas_kiln_profile_on_80c_zone();
    test_profiles_http_save_rejects_exact_duplicate_name();
    test_profiles_http_save_rejects_case_and_whitespace_variant_name();
    test_profiles_http_save_allows_overwriting_a_slot_with_its_own_name();
    test_profiles_http_save_allows_builtin_name();
    test_profiles_http_save_allows_editing_existing_slot_named_like_builtin();
    test_profile_post_handler_rejects_malformed_id();
    test_profile_post_builtin_id_copies_to_first_free();
    test_profile_name_nul_refused();
    test_profile_rule_temp_nan_refused_even_with_cmp_none();
    test_profile_favorite_empty_slot_refused();
    test_fuzz_small_body_handlers();
    test_profile_detail_long_query();
    test_profile_post_handler_collision_response_is_well_formed_json();
    test_profile_post_handler_collision_response_escapes_quote_in_name();
    test_profile_post_handler_collision_response_escapes_newline_in_name();
    test_profile_post_handler_allows_builtin_name();
    test_aux_rule_checks_hold_on_every_entry_point();
    test_retarget_commit_success();
    test_retarget_plan_refusals();
    test_retarget_commit_rollback_at_every_write();
    test_retarget_commit_and_revert_hold_save_lock();
    test_retarget_commit_rechecks_plan_refusals();
    test_save_revalidates_under_lock();
    test_retarget_resume_and_whole_blob_verify();
    test_profiles_list_marks_exceeds_ceiling();
    test_validate_candidate_hard_mode_refuses_target_above_zone_ceiling();
    test_validate_candidate_hard_mode_refuses_ramp_above_zone_ceiling();
    test_validate_candidate_hard_mode_refuses_uncommissioned_zone();
    test_validate_candidate_hard_mode_20pct_ramp_band_still_only_warns();
    test_nvs_save_slot_refuses_when_calling_stack_is_external_ram();
    test_nvs_save_slot_proceeds_normally_on_an_internal_ram_stack();
    test_slot_bitmap_persisted_byte_identical_for_8slot_fixture();
    test_slot_bitmap_legacy_u8_migrates_on_read();
    test_slot_bitmap_round_trips_high_ids();
    test_profiles_http_save_fills_all_100_then_reuses_deleted_slot();
    test_bench_slot_id_excluded_from_catalogue_favorites_and_lcd_order();
    test_builtin_json_emits_seg_kind_and_resolved_zone_mask();

    test_nvs_erase_slot_prunes_firing_stats();
    test_nvs_erase_slot_prunes_firing_stats_for_never_fired_slot();
    test_profiles_http_delete_clears_favorite();
    test_profiles_http_delete_stats_and_erase_failures();
    test_favorites_cfg_only_storage();
    test_favorites_refused_when_unmounted();
    test_favorites_legacy_nvs_migrates();
    test_profiles_http_delete_refuses_running_slot();
    test_profiles_delete_start_race_l23();
    test_profiles_slot_gen_seqlock();
    test_save_ex_rollback_unlinks_written_file();
    test_rvfx_rollback_keeps_unexamined_file();
    test_rvfx_rollback_keeps_file_with_other_rev();
    test_save_ex_fresh_slot_rolled_back_in_lock();
    test_profile_edit_post_slot_gen();
    test_profile_post_expected_rev();
    test_retarget_slot_gen();
    test_delete_slot_gen();
    test_delete_clears_favorite_before_erase_wiring();

    test_pcfg_mounted_migrates_nvs_only_slot_to_file();
    test_pcfg_file_wins_when_it_has_the_higher_rev();
    test_pcfg_adopted_file_retires_legacy_nvs_blob();
    test_pcfg_retire_keeps_blob_when_file_not_adopted();
    test_pcfg_nvs_wins_when_it_has_the_higher_rev_and_resyncs_file();
    test_pcfg_unused_slot_keeps_nvs_rev_floor();
    test_pcfg_boot_load_failure_still_resolves_files();
    test_pcfg_files_only_seeds_floor_from_persisted_revs();
    test_pcfg_files_only_unknown_floor_refuses_save();
    test_pcfg_files_only_keeps_files_when_rev_array_equals_file_rev();
    test_pcfg_full_load_short_rev_blob_marks_fileless_slots_unknown();
    test_pcfg_legacy_32_byte_rev_array_is_known();
    test_pcfg_non_multiple_of_4_rev_blob_stays_unknown();
    test_pcfg_longer_rev_array_is_known_tail_ignored();
    test_pcfg_junk_rev_repair_raises_fileless_to_max();
    test_pcfg_resolve_scratch_oom_leaves_file_untouched();
    test_pcfg_boot_profile_scratch_oom_fails_closed();
    test_pcfg_junk_rev_repair_scratch_oom_fails_closed();
    test_pcfg_junk_rev_repair_refuses_on_external_ram_stack();
    test_pcfg_boot_fallback_keeps_rev_unknown_marks();
    test_save_mutex_serializes_rev_write_bump();
    test_profiles_saves_refuse_under_reset_mark();
    test_profiles_save_reserves_flash_worker();
    test_delete_paths_hold_save_lock_at_erase_seam();
    test_pcfg_junk_repair_load_error_fails_closed();
    test_pcfg_junk_rev_repair_deferred_without_cfg();
    test_pcfg_truncated_rev_blob_not_known_lengths();
    test_pcfg_junk_rev_blob_is_repaired_once();
    test_pcfg_files_only_junk_rev_is_repaired();
    test_pcfg_unknown_floors_flag_file_backed_slots_too();
    test_pcfg_corrupt_nvs_blob_keeps_live_file();
    test_nvs_erase_slot_refuses_when_rev_array_unreadable();
    test_nvs_erase_slot_repairs_wrong_size_used_bitmap();
    test_nvs_erase_slot_keeps_longer_used_bitmap_tail();
    test_favorites_wrong_size_blob_is_an_error();
    test_used_bitmap_rebuild_keeps_present_slot_bit();
    test_used_bitmap_boot_load_longer_and_junk_length();
    test_used_bitmap_genuine_read_error_does_not_rebuild();
    test_used_bitmap_rebuild_fails_closed_on_probe_error();
    test_favorites_set_refuses_while_user_mask_unresolved();
    test_nvs_erase_slot_propagates_firing_stats_error();
    test_pcfg_rev0_file_with_invalid_nvs_is_adopted_not_deleted();
    test_pcfg_stale_file_after_delete_is_not_resurrected();
    test_pcfg_delete_does_not_revert_or_drop_other_file_only_slots();
    test_pcfg_partition_absent_behaves_exactly_like_before();
    test_pcfg_mount_failed_behaves_like_absent();
    test_pcfg_interrupted_write_leaves_old_file_intact();
    test_pcfg_save_load_delete_round_trip_through_real_api();
}


int main(void)
{
    run_test_profiles_http();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
