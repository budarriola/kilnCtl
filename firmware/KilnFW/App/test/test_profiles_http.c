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
static void pcfg_mount_fresh(void); /* defined with the cfg_fs section below */
static void nvs_stub_reset(void)
{
    pcfg_mount_fresh(); /* profile saves are cfg-file-only: every reset starts from a clean, mounted cfg */
    fake_kv_reset_all();
    hal_kv_init_partition("profiles_nvs"); /* PROFILES_NVS_PARTITION's literal -- that macro isn't
                                             * defined until profiles_http.c's own #include below */
    hal_kv_init_partition(NULL); /* the default partition, for the pre-split migration tests */
    s_kv_handle_next = 0;
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

#undef asm

// docs/PROFILE_SLOTS_100_PLAN.md section 7 task 6's bench-slot exclusion
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
static char s_chunk_capture[16384];
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
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value)
{
    (void)r;
    (void)field;
    (void)value;
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
    if (s_post_body && s_post_body_left > 0) {
        size_t n = buf_len < s_post_body_left ? buf_len : s_post_body_left;
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
    return ESP_OK;
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
bool aux_outputs_cfg_get(uint8_t relay, aux_output_t *out)
{
    if (relay < 1 || relay > AUX_OUTPUTS_COUNT || !out) return false;
    *out = g_stub_aux[relay - 1];
    return true;
}
uint8_t aux_outputs_cfg_enabled_mask(void)
{
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
    return ESP_OK;
}
esp_err_t profiles_builtin_restore_all(void)
{
    return ESP_OK;
}

// PROFILE_SLOTS_100_PLAN.md section 7 task 8: fake for the narrow accessor
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
// PROFILE_SLOTS_100_PLAN.md section 7 task 10). The real definition lives in
// profile_executor_firing_stats.c, a control-tier file with its own heavy
// dependency set (esp_heap_caps, zones_config_accessors.h, the executor's
// internal state) this HTTP-tier executable has no other reason to link --
// same reasoning as the profiles_builtin.h/profile_feasibility.h fakes
// above. This fake only records the call (last id + count) so tests below
// can assert nvs_erase_slot() reaches it with the right id, exactly once,
// without needing a real firing-stats store here.
static int     g_firing_stats_erase_calls = 0;
static uint8_t g_firing_stats_erase_last_id = 0xFF;
esp_err_t firing_stats_erase(uint8_t profile_id)
{
    g_firing_stats_erase_calls++;
    g_firing_stats_erase_last_id = profile_id;
    return ESP_OK;
}

// ---- profile_executor.h -- fake profile_executor_get_status(): Opus review
// item 2 (PROFILE_SLOTS_100_PLAN.md section 7) has profiles_http_delete()
// refuse to delete the slot the executor is currently running/paused on.
// Defaults to IDLE (nothing running); tests that need a "delete refused"
// case set g_fake_exec_state/g_fake_exec_profile_id first.
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
bool profile_executor_get_active_id(uint8_t *out_id)
{
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
    // docs/ON_OFF_ZONE_PLAN.md plan step 5 -- every existing caller of this
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
// docs/FILESYSTEM_USER_DATA_PLAN.md section 5 step 4 (user-profiles
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
    TEST_SECTION("nvs_erase_slot() prunes that id's firing history (task 10, PROFILE_SLOTS_100_PLAN.md sec 7)");
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

static void test_profiles_http_delete_clears_favorite(void)
{
    TEST_SECTION("profiles_http_delete() clears the deleted slot's favorite mark (Opus review item 1, "
                 "PROFILE_SLOTS_100_PLAN.md sec 7) -- profiles_edit_http.c's web delete handler already "
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

static void test_profiles_http_delete_refuses_running_slot(void)
{
    TEST_SECTION("profiles_http_delete() refuses a slot the executor is currently running or has paused "
                 "(Opus review item 2, PROFILE_SLOTS_100_PLAN.md sec 7)");
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
// Review fold-in (PROFILE_SLOTS_100_PLAN.md section 7): favorite-clear must
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
                 "slot-erase in source order (review fold-in, PROFILE_SLOTS_100_PLAN.md section 7)");

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
        const char *body = find_function_body(http_text, "bool profiles_http_delete(uint8_t id)", &len);
        TEST_CHECK(body != NULL, "could not find profiles_http_delete()'s function body -- update this test "
                                  "if it was renamed/restructured");
        if (body) {
            char *fn = dup_range(body, len);
            if (fn) {
                assert_favorite_clear_precedes_erase(fn, "profiles_http_delete()");
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
                assert_favorite_clear_precedes_erase(fn, "profile_delete_post_handler()");
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

// Opus review pass (docs/ON_OFF_ZONE_PLAN.md step 5b) -- profile_detail_
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

static void test_profiles_list_carries_last_run_started_unix_s(void)
{
    TEST_SECTION("profiles_list_get_handler -- carries last_run_started_unix_s from "
                 "profile_executor_last_run_started_unix_s() (PROFILE_SLOTS_100_PLAN.md section 7 task 8)");

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
// Test 4c -- docs/ON_OFF_ZONE_PLAN.md plan step 5, PROFILE_VERSION 3->4: a
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

    char body1[256];
    build_minimal_post_body(body1, sizeof(body1), "A%0A%22B");
    TEST_CHECK(run_profile_post(body1) == ESP_OK, "setup: the first save (name containing newline+'\"') must succeed");

    char body2[256];
    build_minimal_post_body(body2, sizeof(body2), "A%0A%22B");
    TEST_CHECK(run_profile_post(body2) == ESP_OK, "handler must still return ESP_OK on a refusal");
    TEST_CHECK(json_is_well_formed(s_resp_capture),
              "a collision response embedding a name with a raw newline must still be well-formed JSON");
    TEST_CHECK(strstr(s_resp_capture, "\\u000a") != NULL,
              "the newline must be escaped as \\u00XX, not left as a raw control byte");
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

// ---- profiles_validate_candidate() -- docs/LIVE_PROFILE_EDIT_PLAN.md section 7/8 -------
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
// profiles_slot_bitmap_t widening (docs/PROFILE_SLOTS_100_PLAN.md section 7
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

// docs/PROFILE_SLOTS_100_PLAN.md section 5's explicit task-6 regression: fill
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

// docs/PROFILE_SLOTS_100_PLAN.md's "Owner decision, 2026-09-19 (post phase-A
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

void run_test_profiles_http(void)
{
    test_v1_blob_loads_and_preserves_all_fields();
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
    test_profile_detail_json_valid_at_max_capacity();
    test_profiles_list_carries_last_run_started_unix_s();
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
    test_profile_post_handler_collision_response_is_well_formed_json();
    test_profile_post_handler_collision_response_escapes_quote_in_name();
    test_profile_post_handler_collision_response_escapes_newline_in_name();
    test_profile_post_handler_allows_builtin_name();
    test_aux_rule_checks_hold_on_every_entry_point();
    test_retarget_commit_success();
    test_retarget_plan_refusals();
    test_retarget_commit_rollback_at_every_write();
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
    test_profiles_http_delete_refuses_running_slot();
    test_delete_clears_favorite_before_erase_wiring();

    test_pcfg_mounted_migrates_nvs_only_slot_to_file();
    test_pcfg_file_wins_when_it_has_the_higher_rev();
    test_pcfg_nvs_wins_when_it_has_the_higher_rev_and_resyncs_file();
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
