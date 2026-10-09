// test_profile_executor_store_link.c -- store-plus-consumer link test
// (docs/RELEASE_HARDENING.md, the "real store+consumer link test" item).
//
// test_profile_executor_prestart.c compiles the REAL profile_executor_run()
// but answers its profiles_http_get() call from a hand-built fake, so a drift
// between what the profile store writes/reads (profiles_http.c: profiles_http_save(),
// NVS blob layout, nvs_load_all_from(), profiles_http_get()) and what the
// executor consumes can never fail there. This executable links the REAL
// profiles_http.c (as its own object, over the real host hal_kv backend
// fake_kv.c; profiles_cfg_fs.c is linked too, and main() mounts a scratch cfg
// directory: since the NVS dual-write close, profile saves go to the cfg
// files only) and reuses the
// prestart test's fakes for everything else by #including that file with its
// main() renamed and its two profiles_http fakes compiled out
// (PEX_STORE_LINK_TEST). Own executable for the usual reason: the prestart
// fakes would multiply-define against other host tests.
//
// Fields compared (saved vs. held by the executor): profile.segment_count,
// zone_mask, name, and per segment seg_kind, target_c, ramp_c_per_hr,
// dwell_min. Other profile_t fields are not compared.
//
// Flow under test: profiles_http_save(id, ...) -> cfg file (scratch mount) ->
// in-memory state wiped -> profiles_http_start() reloads from the file ->
// profile_executor_run(id) reads it through the real profiles_http_get() ->
// s_exec.profile (what the executor actually holds) must equal what was saved.
#define PEX_STORE_LINK_TEST 1
#define main pex_prestart_suite_main
#include "test_profile_executor_prestart.c"
#undef main

#ifdef _WIN32
#include <direct.h>
#define SL_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define SL_MKDIR(p) mkdir((p), 0755)
#endif
#include "cfg_fs.h"
#include "profiles_http_internal.h" /* profiles_storage_ensure()/profiles_state_t */
#include "profiles_http.h"
#include "live_profile.h"
#include "http_auth_http.h"
#include "aux_outputs_cfg.h"

// ---- fakes for what profiles_http.c (separate object) needs and the prestart
// fakes do not already supply ------------------------------------------------
bool live_edit_name_collides_ex(const char *candidate_name, const char *(*name_at)(void *ctx, uint8_t id), void *ctx,
                                uint8_t self_id, bool include_builtins, char *err_msg, size_t err_cap)
{
    (void)candidate_name; (void)name_at; (void)ctx; (void)self_id; (void)include_builtins;
    if (err_msg && err_cap) err_msg[0] = '\0';
    return false; /* dup-name policy is test_profiles_http.c's job */
}

esp_err_t kiln_http_register(httpd_handle_t server, const httpd_uri_t *uri_handler)
{
    (void)server; (void)uri_handler;
    return ESP_OK;
}
// aux_outputs_cfg_get()/aux_outputs_cfg_enabled_mask() now live in the
// prestart file's controllable fakes (spare-relay WP-3), which this file
// #includes; profiles_http.c (real object here) reads those same fakes.
bool zones_config_get_zone_type(uint8_t zone_index, zone_type_t *out_type)
{
    if (out_type) *out_type = ZONE_TYPE_HEATER;
    return zone_index < 8;
}
bool zones_config_get_tuning_quality(uint8_t zone_index, zone_tuning_quality_t *out)
{
    (void)zone_index;
    if (out) memset(out, 0, sizeof(*out));
    return false; /* no persisted tuning -- feasibility falls back to its defaults */
}

// Built-in schedules are not under test (the prestart fixture does not link
// profiles_builtin.c either): no id is a built-in, so every slot resolves to a
// user slot read from the real store.
const builtin_profile_t g_builtin_profiles[1] = { { { 0 }, NULL, NULL, NULL, PROFILE_FIRING_BISQUE, 0, 0, { { 0 } } } };
/* g_builtin_profile_count: already defined (0) by the prestart fixture above. */
bool profiles_builtin_id_valid(uint8_t id) { (void)id; return false; }
bool profiles_builtin_get(uint8_t id, profile_t *out) { (void)id; (void)out; return false; }
const builtin_profile_t *profiles_builtin_entry(uint8_t id) { (void)id; return NULL; }
bool profiles_builtin_is_hidden(uint8_t id) { (void)id; return false; }
esp_err_t profiles_builtin_set_hidden(uint8_t id, bool hidden) { (void)id; (void)hidden; return ESP_OK; }
esp_err_t profiles_builtin_restore_all(void) { return ESP_OK; }

// httpd/web symbols the (never-invoked here) handlers in profiles_catalog_http.c /
// profiles_edit_http.c reference; only link-time presence matters.
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *f, const char *v) { (void)r; (void)f; (void)v; return ESP_OK; }
esp_err_t httpd_resp_send(httpd_req_t *r, const char *b, long long n) { (void)r; (void)b; (void)n; return ESP_OK; }
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *b, size_t n) { (void)r; (void)b; (void)n; return ESP_OK; }
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *st) { (void)r; (void)st; return ESP_OK; }
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *b, size_t n) { (void)r; (void)b; (void)n; return ESP_FAIL; }
esp_err_t httpd_query_key_value(const char *q, const char *k, char *v, size_t n) { (void)q; (void)k; (void)v; (void)n; return ESP_FAIL; }
bool web_client_accepts_gzip(httpd_req_t *r) { (void)r; return true; }
esp_err_t web_send_gzip_not_acceptable(httpd_req_t *r, const char *t, const char *p) { (void)r; (void)t; (void)p; return ESP_OK; }
void web_set_asset_cache_headers(httpd_req_t *r) { (void)r; }

static void pex_check_seg(const profile_segment_t *got, const profile_segment_t *want, int idx, const char *ctx)
{
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: segment %d seg_kind", ctx, idx);
    TEST_CHECK(got->seg_kind == want->seg_kind, msg);
    snprintf(msg, sizeof(msg), "%s: segment %d target_c", ctx, idx);
    TEST_CHECK(fabsf(got->target_c - want->target_c) < 1e-6f, msg);
    snprintf(msg, sizeof(msg), "%s: segment %d ramp_c_per_hr", ctx, idx);
    TEST_CHECK(fabsf(got->ramp_c_per_hr - want->ramp_c_per_hr) < 1e-6f, msg);
    snprintf(msg, sizeof(msg), "%s: segment %d dwell_min", ctx, idx);
    TEST_CHECK(got->dwell_min == want->dwell_min, msg);
}

static void pex_store_then_run(uint8_t slot, const profile_t *want, const char *ctx)
{
    /* Zone 0's limits must be in place BEFORE the save: the real profiles_http_save()
     * refuses a ramp above the zone's ceiling (warm_start_test_setup() sets the same values later). */
    g_stub_max_ramp_c_per_hr[0] = 500.0f;
    g_stub_max_temp_c[0] = 1300.0f;

    char err[160] = {0};
    uint8_t out_id = 0xFF;
    uint8_t warns = 0;

    bool saved = profiles_http_save(slot, want, &out_id, &warns, err, sizeof(err));
    if (!saved) printf("  save error: %s\n", err);
    TEST_CHECK(saved, "profiles_http_save must accept the profile");
    TEST_CHECK(out_id == slot, "profiles_http_save must commit to the requested slot");

    /* Wipe the in-memory catalog and reload from the persisted NVS blobs the
     * way boot does, so the executor reads what the store LAYOUT round-trips,
     * not a still-resident copy of the caller's struct. profiles_http_start()
     * returns ESP_ERR_INVALID_STATE (no httpd server in the host build) only
     * AFTER the load. */
    memset(profiles_storage_ensure(), 0, sizeof(profiles_state_t));
    (void)profiles_http_start();

    profile_t via_store;
    memset(&via_store, 0, sizeof(via_store));
    TEST_CHECK(profiles_http_get(slot, &via_store), "profiles_http_get must find the reloaded slot");

    /* Executor start: the real profile_executor_run() pulls the profile via
     * profiles_http_get() (profile_executor_run.c) -- the prestart fixture
     * supplies zone/thermo/relay state; the profile itself comes ONLY from the store. */
    warm_start_test_setup(want, 50.0f); /* its fake profile out is unused: the fake getter is compiled out */
    bool ok = profile_executor_run(slot, err, sizeof(err));
    TEST_CHECK(ok, "profile_executor_run must start a profile read from the real store");
    if (!ok) {
        printf("  run error: %s\n", err);
        return;
    }
    TEST_CHECK(s_exec.profile.segment_count == want->segment_count, "executor segment_count equals stored");
    TEST_CHECK(s_exec.profile.zone_mask == want->zone_mask, "executor zone_mask equals stored");
    TEST_CHECK(strcmp(s_exec.profile.name, want->name) == 0, "executor profile name equals stored");
    for (int i = 0; i < want->segment_count; i++) {
        pex_check_seg(&s_exec.profile.segments[i], &want->segments[i], i, ctx);
    }
    TEST_CHECK(s_exec.segment_index == 0, "executor starts at segment 0 of the stored profile");
    TEST_CHECK(fabsf(s_exec.target_c - 50.0f) < 0.01f, "executor target seeds from the reading");
    TEST_CHECK(fabsf(s_exec.profile.segments[0].target_c - want->segments[0].target_c) < 1e-6f,
               "segment 0 ramp target the executor will chase is the stored one");
    profile_executor_halt();
}

static void test_store_to_executor_multi_segment_profile(void)
{
    TEST_SECTION("store -> executor: multi-segment profile, middle slot");
    profile_t p;
    memset(&p, 0, sizeof(p));
    strncpy(p.name, "StoreLinkMid", PROFILE_NAME_MAX_LEN);
    p.zone_mask = 0x01;
    p.segment_count = 4;
    p.segments[0] = zone_ramp_seg(120.5f, 80.0f, 5);
    p.segments[1] = zone_ramp_seg(573.0f, 150.0f, 0);
    p.segments[2] = zone_ramp_seg(1010.0f, 99.0f, 17);
    p.segments[3] = zone_ramp_seg(300.0f, 123.0f, 61);
    pex_store_then_run(3, &p, "mid slot");
}

static void test_store_to_executor_highest_slot_max_segments(void)
{
    TEST_SECTION("store -> executor: highest user slot, PROFILE_MAX_SEGMENTS segments");
    profile_t p;
    memset(&p, 0, sizeof(p));
    strncpy(p.name, "StoreLinkMax", PROFILE_NAME_MAX_LEN);
    p.zone_mask = 0x01;
    p.segment_count = PROFILE_MAX_SEGMENTS;
    for (int i = 0; i < PROFILE_MAX_SEGMENTS; i++) {
        p.segments[i] = zone_ramp_seg(100.0f + 37.0f * (float)i, 50.0f + (float)i, (uint32_t)(i * 3 + 1));
    }
    pex_store_then_run(PROFILES_MAX_COUNT - 1, &p, "last slot");
}

int main(void)
{
    g_stub_thermo_count = 1; /* zone 0 configured so the real save accepts zone_mask 0x01 */
    fake_kv_reset_all();
    hal_kv_init_partition("profiles_nvs");
    hal_kv_init_partition(NULL);
    {
        /* Clean scratch cfg mount: start from no leftover profile files. */
        const char *base = "cfg_fs_test_store_link";
        (void)SL_MKDIR(base);
        cfg_fs_deinit();
        TEST_CHECK(cfg_fs_init(base, NULL) == ESP_OK, "cfg_fs mounts against the scratch dir");
        cfg_fs_entry_t ents[64];
        size_t n = 0;
        if (cfg_fs_list("", ents, 64, &n) == ESP_OK) {
            for (size_t i = 0; i < n; i++) {
                (void)cfg_fs_delete(ents[i].name);
            }
        }
    }
    test_store_to_executor_multi_segment_profile();
    test_store_to_executor_highest_slot_max_segments();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
