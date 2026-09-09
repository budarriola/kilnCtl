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
static void nvs_stub_reset(void)
{
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
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value)
{
    (void)r;
    (void)field;
    (void)value;
    return ESP_OK;
}
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
    (void)buf;
    (void)buf_len;
    return 0;
}
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t buf_len)
{
    (void)r;
    (void)buf;
    (void)buf_len;
    return ESP_FAIL;
}
esp_err_t httpd_query_key_value(const char *qs, const char *key, char *val, size_t val_size)
{
    (void)qs;
    (void)key;
    (void)val;
    (void)val_size;
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
// rejection -- that is parse_profile_fields()/profile_post_handler()
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
bool zones_config_get_max_ramp(uint8_t zone_index, float *out_c_per_hr)
{
    (void)zone_index;
    if (out_c_per_hr) *out_c_per_hr = 1000.0f;
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

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

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
static void pcfg_reset_all(void)
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
    nvs_stub_reset();
    memset(&s_profiles, 0, sizeof(s_profiles));
    memset(s_profile_rev, 0, sizeof(s_profile_rev));
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

static void test_pcfg_mounted_migrates_nvs_only_slot_to_file(void)
{
    TEST_SECTION("profiles cfg_fs -- NVS-only slot is lazily migrated to a file on load");
    pcfg_reset_all();
    size_t reaped = 0;
    TEST_CHECK(cfg_fs_init(PCFG_SCRATCH_BASE, &reaped) == ESP_OK, "cfg_fs mounts against the scratch dir");

    profile_t src = make_stored_profile();
    s_profiles.profiles[0] = src;
    s_profiles.used_bitmap = 0x01;
    TEST_CHECK(nvs_save_slot(0) == ESP_OK, "nvs_save_slot succeeds with cfg_fs mounted");

    // Simulate a reboot: wipe the RAM/file view of what a fresh load
    // produces are not wiped (cfg_fs itself is real on-disk state), but the
    // in-RAM catalogue must be, so nvs_load_all_from()'s own decode is what
    // is actually exercised, not stale RAM.
    memset(&s_profiles, 0, sizeof(s_profiles));

    profiles_state_t out;
    bool any_found = false;
    TEST_CHECK(nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found) == ESP_OK, "reload succeeds");
    TEST_CHECK((out.used_bitmap & 0x01) != 0, "slot 0 still reported used after reload");
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

    s_profiles.profiles[0] = nvs_side;
    s_profiles.used_bitmap = 0x01;
    TEST_CHECK(nvs_save_slot(0) == ESP_OK, "nvs_save_slot(0) writes rev 1 to both sides");
    // Overwrite JUST the file with different content at a HIGHER rev, as if
    // an earlier save's file write landed but its NVS write then failed.
    TEST_CHECK(profiles_cfg_fs_save(0, &file_side, 5) == ESP_OK, "file overwritten at rev 5");

    memset(&s_profiles, 0, sizeof(s_profiles));
    profiles_state_t out;
    bool any_found = false;
    TEST_CHECK(nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found) == ESP_OK, "reload succeeds");
    TEST_CHECK((out.used_bitmap & 0x01) != 0, "slot 0 still used");
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
    s_profiles.profiles[0] = fresh_nvs;
    s_profiles.used_bitmap = 0x01;
    profile_persisted_t persisted = { .version = PROFILE_VERSION, .profile = fresh_nvs, .crc32 = 0 };
    persisted.crc32 = compute_profile_crc(&persisted);
    stage_profile_blob(0, &persisted, sizeof(persisted));
    stage_bitmap(0x01);
    uint32_t rev_arr[PROFILES_MAX_COUNT] = {0};
    rev_arr[0] = 9;
    {
        nvs_handle_t h;
        nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
        nvs_set_blob(h, NVS_KEY_PROFILE_REV, rev_arr, sizeof(rev_arr));
        nvs_close(h);
    }

    memset(&s_profiles, 0, sizeof(s_profiles));
    profiles_state_t out;
    bool any_found = false;
    TEST_CHECK(nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found) == ESP_OK, "reload succeeds");
    assert_profiles_equal(&out.profiles[0], &fresh_nvs, "NVS content wins (higher rev)");

    profile_t file_p;
    TEST_CHECK(pcfg_file_profile(0, &file_p), "the file still decodes (resync wrote it)");
    assert_profiles_equal(&file_p, &fresh_nvs, "the file was resynced to NVS's winning content");
}

static void test_pcfg_stale_file_after_delete_is_not_resurrected(void)
{
    TEST_SECTION("profiles cfg_fs -- a file left behind by an interrupted delete is not resurrected");
    pcfg_reset_all();
    size_t reaped = 0;
    cfg_fs_init(PCFG_SCRATCH_BASE, &reaped);

    profile_t p = make_stored_profile();
    s_profiles.profiles[0] = p;
    s_profiles.used_bitmap = 0x01;
    TEST_CHECK(nvs_save_slot(0) == ESP_OK, "save lands on both sides at rev 1");

    // Simulate the file-delete half of nvs_erase_slot() failing (the NVS
    // half still succeeds and bumps the persisted rev) by installing a
    // failing delete fn for exactly the delete call, then restoring it --
    // mirrors profiles_cfg_fs_delete()'s own no-op-on-failure contract.
    profiles_cfg_fs_set_delete_fn(pcfg_failing_delete_fn);
    esp_err_t erase_err = nvs_erase_slot(0);
    profiles_cfg_fs_reset_delete_fn_for_test();
    TEST_CHECK(erase_err == ESP_OK, "erase still succeeds through NVS even though the file-side delete failed");

    profile_t leftover;
    TEST_CHECK(pcfg_file_profile(0, &leftover), "the stale file is confirmed still present on disk");

    // Whether or not the delete_fn above actually intercepted anything, the
    // real assertion this test exists for is: after erase, a reload must
    // NEVER resurrect slot 0 from a leftover file, because nvs_erase_slot()
    // bumps s_profile_rev[0] past whatever the file holds.
    memset(&s_profiles, 0, sizeof(s_profiles));
    profiles_state_t out;
    bool any_found = false;
    TEST_CHECK(nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found) == ESP_OK, "reload succeeds");
    TEST_CHECK((out.used_bitmap & 0x01) == 0,
               "slot 0 stays unused after delete even if a stale file existed -- rev comparison, not file "
               "presence alone, decides");
}

static void test_pcfg_partition_absent_behaves_exactly_like_before(void)
{
    TEST_SECTION("profiles cfg_fs -- partition never mounted: NVS-only behavior is unchanged");
    pcfg_reset_all(); // cfg_fs_deinit() inside -- status is UNMOUNTED, cfg_fs_is_available() is false

    profile_t src = make_stored_profile();
    s_profiles.profiles[0] = src;
    s_profiles.used_bitmap = 0x01;
    TEST_CHECK(nvs_save_slot(0) == ESP_OK, "save succeeds with no cfg partition at all");

    memset(&s_profiles, 0, sizeof(s_profiles));
    profiles_state_t out;
    bool any_found = false;
    TEST_CHECK(nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found) == ESP_OK, "reload succeeds");
    TEST_CHECK((out.used_bitmap & 0x01) != 0, "slot 0 used");
    assert_profiles_equal(&out.profiles[0], &src, "NVS is the sole source of truth when cfg_fs is unavailable");
}

static void test_pcfg_mount_failed_behaves_like_absent(void)
{
    TEST_SECTION("profiles cfg_fs -- mount attempted and failed: same degrade-to-NVS behavior");
    pcfg_reset_all();
    size_t reaped = 0;
    // A path that cannot be listed at all makes cfg_fs_init() report
    // UNAVAILABLE (cfg_fs.h's own documented contract), not MOUNTED.
    esp_err_t err = cfg_fs_init("Z:/definitely/does/not/exist/kilnctl_cfg_fs_test", &reaped);
    TEST_CHECK(err != ESP_OK, "cfg_fs_init against an unlistable path reports failure");
    TEST_CHECK(!cfg_fs_is_available(), "cfg_fs_is_available() is false after a failed mount");

    profile_t src = make_stored_profile();
    s_profiles.profiles[0] = src;
    s_profiles.used_bitmap = 0x01;
    TEST_CHECK(nvs_save_slot(0) == ESP_OK, "save still succeeds through NVS despite the failed mount");

    memset(&s_profiles, 0, sizeof(s_profiles));
    profiles_state_t out;
    bool any_found = false;
    TEST_CHECK(nvs_load_all_from(PROFILES_NVS_PARTITION, &out, &any_found) == ESP_OK, "reload succeeds");
    assert_profiles_equal(&out.profiles[0], &src, "NVS remains authoritative when the mount failed");
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
    TEST_CHECK(pcfg_file_profile(0, &file_p), "a file now backs slot 0 (dual-write landed)");
    assert_profiles_equal(&file_p, &candidate, "the file's content matches what was saved");

    profile_t got;
    TEST_CHECK(profiles_http_get(0, &got), "profiles_http_get still finds it");
    assert_profiles_equal(&got, &candidate, "get returns what was saved");

    TEST_CHECK(profiles_http_delete(0), "profiles_http_delete succeeds through the unchanged public API");
    TEST_CHECK(!profiles_http_get(0, &got), "get no longer finds a deleted slot");
    bool still_there = pcfg_file_profile(0, &file_p);
    TEST_CHECK(!still_there, "the file is also gone after delete (dual-delete landed)");
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
    TEST_CHECK((out.used_bitmap & 0x01) != 0,
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
    TEST_CHECK((out.used_bitmap & 0x01) == 0, "version 0 must never be installed as a used slot");
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
    TEST_CHECK((out.used_bitmap & 0x01) == 0, "a length mismatch for the claimed version must be rejected");
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
    TEST_CHECK((out.used_bitmap & 0x01) == 0, "a CRC mismatch must be rejected");
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
    s_profiles.used_bitmap = 0xFF; /* deliberately wrong, so a no-op bug can't accidentally read as a pass */

    (void)profiles_http_start(); /* returns ESP_ERR_INVALID_STATE (no HTTP server in this stub) AFTER the
                                  * NVS load logic below has already run -- exactly what's under test. */

    TEST_CHECK((s_profiles.used_bitmap & 0x01) == 0, "a refused newer-version blob must not be reported used");

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
    TEST_CHECK((out.used_bitmap & 0x01) != 0, "slot 0 (good) must still be used");
    TEST_CHECK((out.used_bitmap & 0x02) == 0, "slot 1 (bad CRC) must be marked unused");
    TEST_CHECK((out.used_bitmap & 0x04) != 0, "slot 2 (good) must still be used, unaffected by slot 1's corruption");
    assert_profiles_equal(&out.profiles[0], &good0, "slot 0");
    assert_profiles_equal(&out.profiles[2], &good2, "slot 2");
}

// ---------------------------------------------------------------------------
// Test 7 -- GET /api/profiles JSON stays valid with worst-case escape-heavy
// names at a full slot count (all 8 slots used, every name built entirely
// from '"' and '\\' -- json_escape()'s two double-cost characters).
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
         * json_escape() doubles every one of them, the worst case
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
        s_profiles.used_bitmap |= (uint8_t)(1u << id);
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
    TEST_CHECK((out.used_bitmap & 0x01) != 0, "a v2 blob must migrate to a used slot, not be dropped");
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
    TEST_CHECK((out.used_bitmap & 0x01) != 0, "a v3 blob must migrate to a used slot, not be dropped");
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
    TEST_SECTION("profiles_http_save/nvs_load_all_from -- an on/off rule round-trips byte-for-byte through NVS");

    memset(&s_profiles, 0, sizeof(s_profiles));
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
    assert_profiles_equal(&out.profiles[out_id], &p, "on/off rule NVS round-trip");
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
    TEST_CHECK((s_profiles.used_bitmap & (1u << out_id)) != 0, "the profile must actually be written to storage");
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
    TEST_CHECK((s_profiles.used_bitmap & (1u << out_id)) != 0, "the 2015C profile must actually be written");
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
    memset(&s_profiles, 0, sizeof(s_profiles));

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
    test_validate_io_segment_zone_ownership();
    test_validate_io_segment_drdy_lcd_gap_refused();
    test_profiles_http_save_accepts_cone10_profile_on_80c_zone();
    test_profiles_http_save_accepts_in_range_profile();
    test_profiles_http_save_accepts_target_exactly_at_zone_limit();
    test_profiles_http_save_accepts_one_degree_over_zone_limit();
    test_profiles_http_save_accepts_2015c_gas_kiln_profile_on_80c_zone();
    test_profiles_list_marks_exceeds_ceiling();
    test_nvs_save_slot_refuses_when_calling_stack_is_external_ram();
    test_nvs_save_slot_proceeds_normally_on_an_internal_ram_stack();
    test_builtin_json_emits_seg_kind_and_resolved_zone_mask();

    test_pcfg_mounted_migrates_nvs_only_slot_to_file();
    test_pcfg_file_wins_when_it_has_the_higher_rev();
    test_pcfg_nvs_wins_when_it_has_the_higher_rev_and_resyncs_file();
    test_pcfg_stale_file_after_delete_is_not_resurrected();
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
