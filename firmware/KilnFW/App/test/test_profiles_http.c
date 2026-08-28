// Host test for App/drivers/profiles_http.c's on-flash blob decoding
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
// Multi-key NVS stub -- see header comment above for why stubs/nvs.h's
// single-blob model doesn't fit here. Defining this guard macro BEFORE
// profiles_http.c's "#include \"nvs.h\"" makes that header's own include
// guard (`#ifndef TEST_STUB_NVS_H`) skip its body entirely, so nothing here
// multiply-defines against it.
// ---------------------------------------------------------------------------
#define TEST_STUB_NVS_H

typedef struct nvs_opaque *nvs_handle_t;

typedef enum {
    NVS_READONLY = 0,
    NVS_READWRITE,
} nvs_open_mode_t;

#define NVS_STUB_MAX_ENTRIES 32
#define NVS_STUB_BLOB_CAP 512

typedef struct {
    bool used;
    char partition[32];
    char ns[32];
    char key[16];
    bool has_u8;
    uint8_t u8_val;
    bool has_blob;
    uint8_t blob[NVS_STUB_BLOB_CAP];
    size_t blob_len;
} nvs_stub_entry_t;

static nvs_stub_entry_t s_nvs_entries[NVS_STUB_MAX_ENTRIES];

typedef struct {
    char partition[32];
    char ns[32];
} nvs_stub_ctx_t;
static nvs_stub_ctx_t s_nvs_ctx; /* one open handle at a time is all these tests ever need */

static void nvs_stub_reset(void)
{
    memset(s_nvs_entries, 0, sizeof(s_nvs_entries));
}

static nvs_stub_entry_t *nvs_stub_find(const char *partition, const char *ns, const char *key, bool create)
{
    int free_slot = -1;
    for (int i = 0; i < NVS_STUB_MAX_ENTRIES; i++) {
        if (!s_nvs_entries[i].used) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (strcmp(s_nvs_entries[i].partition, partition) == 0 && strcmp(s_nvs_entries[i].ns, ns) == 0 &&
            strcmp(s_nvs_entries[i].key, key) == 0) {
            return &s_nvs_entries[i];
        }
    }
    if (!create || free_slot < 0) {
        return NULL;
    }
    nvs_stub_entry_t *e = &s_nvs_entries[free_slot];
    memset(e, 0, sizeof(*e));
    e->used = true;
    strncpy(e->partition, partition, sizeof(e->partition) - 1);
    strncpy(e->ns, ns, sizeof(e->ns) - 1);
    strncpy(e->key, key, sizeof(e->key) - 1);
    return e;
}

static esp_err_t nvs_open_from_partition(const char *partition, const char *ns, int mode, nvs_handle_t *out)
{
    (void)mode;
    strncpy(s_nvs_ctx.partition, partition, sizeof(s_nvs_ctx.partition) - 1);
    s_nvs_ctx.partition[sizeof(s_nvs_ctx.partition) - 1] = '\0';
    strncpy(s_nvs_ctx.ns, ns, sizeof(s_nvs_ctx.ns) - 1);
    s_nvs_ctx.ns[sizeof(s_nvs_ctx.ns) - 1] = '\0';
    if (out) {
        *out = (nvs_handle_t)&s_nvs_ctx;
    }
    return ESP_OK;
}

static void nvs_close(nvs_handle_t h) { (void)h; }

static esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    return ESP_OK;
}

static esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out)
{
    nvs_stub_ctx_t *ctx = (nvs_stub_ctx_t *)h;
    nvs_stub_entry_t *e = nvs_stub_find(ctx->partition, ctx->ns, key, false);
    if (!e || !e->has_u8) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *out = e->u8_val;
    return ESP_OK;
}

static esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t val)
{
    nvs_stub_ctx_t *ctx = (nvs_stub_ctx_t *)h;
    nvs_stub_entry_t *e = nvs_stub_find(ctx->partition, ctx->ns, key, true);
    if (!e) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    e->has_u8 = true;
    e->u8_val = val;
    return ESP_OK;
}

static esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    nvs_stub_ctx_t *ctx = (nvs_stub_ctx_t *)h;
    nvs_stub_entry_t *e = nvs_stub_find(ctx->partition, ctx->ns, key, false);
    if (!e || !e->has_blob) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (!out || !len || *len < e->blob_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, e->blob, e->blob_len);
    *len = e->blob_len;
    return ESP_OK;
}

static esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *val, size_t len)
{
    nvs_stub_ctx_t *ctx = (nvs_stub_ctx_t *)h;
    if (len > NVS_STUB_BLOB_CAP) {
        return ESP_ERR_INVALID_SIZE;
    }
    nvs_stub_entry_t *e = nvs_stub_find(ctx->partition, ctx->ns, key, true);
    if (!e) {
        return ESP_ERR_INVALID_SIZE; /* stub table full */
    }
    memcpy(e->blob, val, len);
    e->blob_len = len;
    e->has_blob = true;
    return ESP_OK;
}

static esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    nvs_stub_ctx_t *ctx = (nvs_stub_ctx_t *)h;
    nvs_stub_entry_t *e = nvs_stub_find(ctx->partition, ctx->ns, key, false);
    if (!e) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    e->used = false;
    return ESP_OK;
}

// asm("_binary_...") is a GCC/binutils extension with no MSVC equivalent --
// #define it away, same convention test_zones_http.c uses.
#define asm(x)

#include "../drivers/profiles_http.c"

#undef asm
#undef TEST_STUB_NVS_H

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
uint8_t zones_config_get_thermo_count(void)
{
    return 8;
}
bool zones_config_get_max_ramp(uint8_t zone_index, float *out_c_per_hr)
{
    (void)zone_index;
    if (out_c_per_hr) *out_c_per_hr = 1000.0f;
    return true;
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
profile_seg_verdict_t profile_feasibility_profile_mask(uint8_t zone_mask, const profile_t *p,
                                                       profile_seg_verdict_t *out_segments, size_t out_cap)
{
    (void)zone_mask;
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
const builtin_profile_t g_builtin_profiles[1] = { { { 0 }, NULL, NULL, NULL, 0, { { 0 } } } };
const size_t g_builtin_profile_count = 0;

bool profiles_builtin_id_valid(uint8_t id)
{
    (void)id;
    return false;
}
bool profiles_builtin_get(uint8_t id, profile_t *out)
{
    (void)id;
    (void)out;
    return false;
}
const builtin_profile_t *profiles_builtin_entry(uint8_t id)
{
    (void)id;
    return NULL;
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
}

// ---------------------------------------------------------------------------
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

void run_test_profiles_http(void)
{
    test_v1_blob_loads_and_preserves_all_fields();
    test_version_zero_rejected();
    test_length_mismatch_rejected();
    test_bad_crc_rejected();
    test_v2_blob_migrates_distinct_multi_segment_values();
    test_newer_version_refused_not_wiped();
    test_one_bad_slot_does_not_affect_others();
    test_profiles_list_json_valid_with_escape_heavy_names();
    test_validate_io_segment_zone_ownership();
    test_validate_io_segment_drdy_lcd_gap_refused();
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
