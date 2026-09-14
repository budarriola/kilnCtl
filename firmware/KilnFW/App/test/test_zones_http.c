// Host test for App/drivers/http/zones_http.c's zones_http_parse_zone_fields(), added
// 2026-08-21 for the whole-page-zone-save data-loss defect found by
// black-box testing against the live board: a POST to /api/zones that
// carries fields only for zones 0..thermo_count-1 used to silently ZERO
// relay_mask/thermo_mask/every other per-zone field for any zone index at
// or past thermo_count, because zones_post_handler() memset(0)s its
// candidate config before parsing and zones_http_parse_zone_fields() returned early
// (before touching any of those fields) for i >= thermo_count. Reproduced
// live: with thermo_count=1, an ordinary whole-page save that only replayed
// what GET had just reported zeroed zone 1 and zone 2's thermo_mask, with a
// 200 OK and no warning.
//
// This is its own SEPARATE host-test executable (own main(), not merged into
// test_main.c/kilnctl_host_tests.exe) -- see build_host_tests.ps1's second
// build+run step. Reason: zones_http_parse_zone_fields() is file-scope-
// internal (was `static` before the 2026-09-04 split widened it so a
// sibling .c file could call it -- see zones_http_internal.h), not part of
// zones_http.h's public API, so the only way to reach it directly is to
// #include zones_http.c itself (same convention
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
// Stub surface below is wider than zones_http_parse_zone_fields() itself touches, for
// the same reason test_backup_import.c's is wider than backup_import_apply()
// needs: the whole of zones_http.c (page GET, JSON GET, the POST wrapper,
// zones_http_start()'s hardware bring-up) is compiled into this one
// translation unit and must link, even though these tests call
// zones_http_parse_zone_fields() directly and never invoke any of the real handlers.
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
#include "fake_kv.h"

// RELAY_LIFE_BUDGET.md: zones_config_store.c now calls
// relay_cycles_set_type() (relay_cycles.h) on load and on every successful
// save. relay_cycles.c itself is its OWN separate host-test executable
// (build_host_tests.ps1's "run_state_relay_cycles" step) with real NVS/
// PSRAM-stack dependencies this executable does not pull in -- so, same
// convention as every other cross-module dependency in this file (hal_kv.h
// via fake_kv.h, etc.), this is a fake body, not the real relay_cycles.c,
// linked into this ONE translation unit. It only needs to record what was
// pushed so a test can assert on it -- relay_cycles.c's own host tests
// already cover the real budget math/persistence.
#include "../drivers/persist/relay_cycles.h"
static relay_type_t s_test_relay_type_pushed[RELAY_CYCLES_COUNT];
static uint32_t s_test_relay_override_pushed[RELAY_CYCLES_COUNT];
static int s_test_relay_type_push_count[RELAY_CYCLES_COUNT];
static void test_relay_cycles_reset_pushes(void)
{
    memset(s_test_relay_type_pushed, 0, sizeof(s_test_relay_type_pushed));
    memset(s_test_relay_override_pushed, 0, sizeof(s_test_relay_override_pushed));
    memset(s_test_relay_type_push_count, 0, sizeof(s_test_relay_type_push_count));
}
void relay_cycles_set_type(uint8_t relay, relay_type_t type, uint32_t rated_override)
{
    if (relay >= RELAY_CYCLES_COUNT) {
        return;
    }
    s_test_relay_type_pushed[relay] = type;
    s_test_relay_override_pushed[relay] = rated_override;
    s_test_relay_type_push_count[relay]++;
}

// asm("_binary_...") is a GCC/binutils extension (EMBED_TXTFILES,
// CMakeLists.txt) with no MSVC equivalent -- #define it away to nothing so
// `extern const uint8_t X[] asm("...");` parses as plain
// `extern const uint8_t X[];`. Real (empty) definitions follow the include.
#define asm(x)

// zones_http.c split 2026-09-01 into six files along its natural seams (it
// had grown to 5628 lines, the largest file in the firmware -- see
// ../drivers/persist/zones_http_internal.h's header comment for the seam
// rationale). One of those six, zones_http_handlers.c, was itself split
// again 2026-09-04 (ROADMAP.md M15's 1500-line item, it had grown to 1598
// lines) into zones_http_get.c/zones_http_post_parse.c/zones_http_post.c/
// zones_http_pid.c. zones_http_parse_zone_fields() -- the whole reason this
// test reaches for source inclusion instead of linking -- now lives in
// zones_http_post_parse.c. The pattern this file's own header comment
// describes is unchanged: #include every one of the split's .c files into
// this ONE translation unit so their (now cross-file) `static`/non-static
// mix still resolves exactly the way it does in the real, separately-
// compiled firmware build, and reach every `static` internal directly.
// Order matches the original file's top-to-bottom order.
#include "../drivers/http/zones_http.c"
#include "../drivers/persist/zones_config_store.c"
#include "../drivers/persist/zones_config_accessors.c"
#include "../drivers/control/pid_fuzzy.c" /* pid_fuzzy_derive_bands() -- zones_http_get.c's
                                            * fuzzy_model_valid call site (GAP 2,
                                            * docs/audits/observability_gaps_closed_2026-09-14.md) */
#include "../drivers/http/zones_http_get.c"
#include "../drivers/http/zones_http_post_parse.c"
#include "../drivers/http/zones_http_post.c"
#include "../drivers/http/zones_http_pid.c"
#include "../drivers/control/zones_current_sweep_engine.c"
#include "../drivers/control/zones_current_sweep_task.c"

#undef asm

// Owner request 2026-09-10 ("if i change the max temp in the web gui it
// should change it in the pico too."): zones_http_post.c (#included above)
// now calls safety_ceiling_sync_guard_raise()/_apply_lower(), whose real
// implementation (safety_ceiling_sync.c, linked into this executable as a
// plain .c file -- see build_host_tests.ps1) calls THIS function to actually
// stage+commit+confirm the Pico write. The real body
// (safety_cfg_http.c's safety_cfg_http_set_and_confirm_f32()) lives inside a
// giant ESP-httpd-owning translation unit this executable has no business
// pulling in (same "fake body, not the real file" reasoning as
// relay_cycles_set_type() above) -- so this fake always reports success,
// which is the correct behaviour for every existing test in this file: none
// of them are testing the Pico-ceiling-sync feature itself (that is
// test_safety_ceiling_policy.c's job, entirely at the pure-logic layer,
// with its own fake writer) -- they only need a whole-page zone POST to
// keep working exactly as it did before this feature existed.
/* 2026-09-10 opus review: link-down bypass / boot-time reconciliation test
 * below (test_reconcile_on_link_up_*) needs to observe whether THIS fake
 * was actually called and with what target, not just that it always
 * succeeds -- so it now records call count and last value alongside the
 * existing unconditional-success behaviour every other test in this file
 * still relies on. */
static int s_ceiling_writer_calls = 0;
static float s_ceiling_writer_last_target_c = 0.0f;

bool safety_cfg_http_set_and_confirm_f32(SafetyLinkClass *link, uint16_t param_id, float value,
                                          char *reason_out, size_t reason_cap,
                                          safety_ceiling_refusal_class_t *out_class)
{
    (void)link;
    (void)param_id;
    s_ceiling_writer_calls++;
    s_ceiling_writer_last_target_c = value;
    if (reason_out && reason_cap > 0) {
        reason_out[0] = '\0';
    }
    if (out_class) {
        *out_class = SAFETY_CEILING_REFUSAL_NONE;
    }
    return true;
}

// ---- HW_ABSTRACTION.md Phase 3 item 3: nvs.h -> hal_kv.h migration ---
// zones_http.c/zones_config_store.c now call hal_kv_*() instead of nvs_*()
// directly (production no longer includes nvs.h/nvs_flash.h at all), so
// stubs/nvs.h's single-blob-slot stub (nvs_test_enable()/nvs_test_clear())
// is no longer reachable through production code and cannot be used here
// either. Rather than hand-edit every one of this file's ~90
// nvs_test_enable(true)/nvs_test_clear() call-site pairs (every test below
// follows the identical enable/stage/assert/disable discipline the original
// stub required), these two names are re-implemented as thin shims over
// fake_kv.h -- the real per-(partition,namespace,key) RAM store every other
// migrated file's host test now uses (see test_relay_cycles.c/
// test_kiln_cfg_store.c). This is a STRICT improvement in fidelity, not a
// workaround: the old stub collapsed every (partition, namespace, key) into
// one shared slot (its own header comment called this out explicitly), which
// happened to make writes to the wrong partition invisible-but-harmless;
// fake_kv is a real per-partition store, so a write actually has to land in
// KILN_NVS_PARTITION (kiln_nvs) to be visible to nvs_load_from("kiln_nvs",
// ...) the way it would on real hardware.
static inline void nvs_test_enable(bool enable)
{
    if (enable) {
        fake_kv_reset_all();
        hal_kv_init_partition(KILN_NVS_PARTITION); // zones_http_start()/nvs_partition_init()
                                                    // does this once at boot; tests below call
                                                    // nvs_load_from()/relay_names_save()/etc.
                                                    // directly without going through boot, so it
                                                    // has to be done here instead.
    }
    (void)enable; // no separate "disabled" state to model -- fake_kv_reset_all() below undoes it.
                  // The retired nvs.h stub's nvs_test_enable(false) made every subsequent open
                  // fail closed globally; this shim's disable is a no-op, and nvs_test_clear()
                  // below only wipes contents -- it leaves the fake partition mounted. So once
                  // the first bracketed test has run, the store stays open, silently, for the
                  // rest of the run: a future test written without the enable(true)/clear
                  // bracket does NOT fail closed the way it would have against the old stub --
                  // it just gets a working store handed to it for free. Every persistence test
                  // in this file MUST keep the enable(true)/nvs_test_clear() bracket regardless.
}

static inline void nvs_test_clear(void)
{
    // Called both right after nvs_test_enable(true) (to guarantee a clean
    // store before staging) and right after nvs_test_enable(false) (test
    // teardown) -- both cases want the same "back to a known-empty state"
    // effect the old stub's nvs_test_clear() gave.
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
}

// ---- Embedded-page symbols page_get_handler() references ------------------
// Never actually sent by these tests (that handler is never called), but
// must exist for the linker.
const uint8_t zones_page_html_gz_start[1] = { 0 };
const uint8_t zones_page_html_gz_end[1] = { 0 };
const uint8_t safety_config_page_html_gz_start[1] = { 0 };
const uint8_t safety_config_page_html_gz_end[1] = { 0 };
// tuning_recommendations_json_start/_end (2026-09-02 host-link fix, commit
// 333dd4e's build_host_tests.ps1 break): same 1-byte-placeholder convention
// as the gz pairs above -- these two are ESP-IDF EMBED_FILES symbols with no
// host-toolchain equivalent, and tuning_rec_get_handler() is never called by
// these tests (only its extracted tuning_rec_body_len() helper is, with a
// real synthetic buffer -- see test_tuning_rec_body_len_* below). A 1-byte
// pair is enough for the linker.
const uint8_t tuning_recommendations_json_start[1] = { 0 };
const uint8_t tuning_recommendations_json_end[1] = { 0 };

// ---- ota_http.c's interlock gate --------------------------------------------
// zones_http.c's POST handler now refuses to rewrite zone config while a
// firing is running, through the same ota_http_check_interlocks() gate
// kiln_cfg_http and backup_http use. None of these is reachable from the
// tests here (only zones_http_parse_zone_fields()/the NVS decode path is called
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
// None of these is ever invoked by this file's tests (only zones_http_parse_zone_fields()
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
/* PID_EXPANSION_PLAN.md Phase 4 round-trip test support: captures the last
 * body httpd_resp_send() was asked to send, so a test can inspect what
 * zones_get_handler() actually emitted -- every prior test in this file only
 * ever calls zones_http_parse_zone_fields()/zones_post_handler(), never
 * zones_get_handler(), so this capture is inert for them. */
static char s_last_resp_body[8192];
static size_t s_last_resp_len;
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, long long buf_len)
{
    (void)r;
    size_t n = (buf_len < 0) ? 0 : (size_t)buf_len;
    if (n >= sizeof(s_last_resp_body)) {
        n = sizeof(s_last_resp_body) - 1;
    }
    if (buf && n > 0) {
        memcpy(s_last_resp_body, buf, n);
    }
    s_last_resp_body[n] = '\0';
    s_last_resp_len = n;
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
 * have no seam of their own to call directly, unlike zones_config_json_parse_u8_field()/
 * zones_config_json_parse_float_field()). NULL/false by default so every pre-existing test in
 * this file, which only calls zones_http_parse_zone_fields() directly and never
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
    s_test_ok_called = true;
    /* Also captured into s_last_resp_body, same as httpd_resp_send() above --
     * added for zones_get_handler()'s malloc-failure 500 path (2026-08-31
     * PSRAM fix), which sends its error body via httpd_resp_sendstr(), not
     * httpd_resp_send(). Every pre-existing test in this file only checks
     * s_test_ok_called/s_test_err_called, never s_last_resp_body after a
     * sendstr() call, so this addition is inert for them. */
    if (s) {
        size_t n = strlen(s);
        if (n >= sizeof(s_last_resp_body)) {
            n = sizeof(s_last_resp_body) - 1;
        }
        memcpy(s_last_resp_body, s, n);
        s_last_resp_body[n] = '\0';
        s_last_resp_len = n;
    }
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

// M12: zone_sweep_push_ct_channel_map() stages the derived map over these
// two, so zones_http.c now references them. Test-controlled stand-ins for
// the same reason safety_link_get_status() above is one -- nothing here
// exercises the wire, only the decision that leads to it.
//
// M12b: the CT-scale push (zone_sweep_push_k_ct_v_per_a()) needs more than a
// fixed "always succeeds" answer -- its whole safety argument is what it does
// when a stage fails, a commit is rejected, or the read-back disagrees, and
// none of those arms is reachable while the stubs can only succeed. So these
// now RECORD every SET_PARAM and can be told to fail; defaults are exactly
// the old fixed behaviour, so every pre-existing test is unaffected.
#define TEST_SETPARAM_LOG_MAX 32
typedef struct {
    uint16_t param_id;
    uint8_t  type;
    kilnlink_param_value_t value;
} test_setparam_call_t;

static test_setparam_call_t s_setparam_log[TEST_SETPARAM_LOG_MAX];
static int s_setparam_count = 0;
// Fail the Nth (0-based) SET_PARAM of the run; -1 fails none.
static int s_setparam_fail_at = -1;
static esp_err_t s_commit_err = ESP_OK;
static bool s_commit_rejected = false;
static int s_commit_count = 0;

static void test_link_reset(void)
{
    s_setparam_count = 0;
    s_setparam_fail_at = -1;
    s_commit_err = ESP_OK;
    s_commit_rejected = false;
    s_commit_count = 0;
    memset(s_setparam_log, 0, sizeof(s_setparam_log));
}

esp_err_t safety_link_send_set_param(SafetyLinkClass *link, uint16_t param_id, uint8_t type,
                                      kilnlink_param_value_t value)
{
    (void)link;
    bool fail = (s_setparam_fail_at >= 0 && s_setparam_count == s_setparam_fail_at);
    if (s_setparam_count < TEST_SETPARAM_LOG_MAX) {
        s_setparam_log[s_setparam_count].param_id = param_id;
        s_setparam_log[s_setparam_count].type = type;
        s_setparam_log[s_setparam_count].value = value;
    }
    s_setparam_count++;
    return fail ? ESP_FAIL : ESP_OK;
}
esp_err_t safety_link_send_commit_config(SafetyLinkClass *link, uint16_t *out_param_id,
                                          uint8_t *out_reason, bool *out_rejected)
{
    (void)link;
    s_commit_count++;
    if (out_param_id) *out_param_id = 0x0308;
    if (out_reason) *out_reason = 3;
    if (out_rejected) *out_rejected = s_commit_rejected;
    return s_commit_err;
}

// H1 fix (opus review, 2026-08-28): zone_sweep_confirm_ct_map_landed() forces
// a LIVE re-fetch of the Pico's record and re-reads it, so zones_http.c now
// references safety_cfg_store's read side and safety_link_get_peer_build_
// status(). Same test-controlled stand-in reasoning as the two link senders
// above -- nothing here drives the push, only the decisions that feed it.
esp_err_t safety_link_get_peer_build_status(SafetyLinkClass *link, bool *out_known, bool *out_dirty,
                                             uint8_t *commit_buf, uint8_t *out_commit_len,
                                             uint8_t *datetime_buf, uint8_t *out_datetime_len,
                                             uint8_t *out_config_version, uint16_t *out_config_crc)
{
    (void)link; (void)out_dirty; (void)commit_buf; (void)out_commit_len;
    (void)datetime_buf; (void)out_datetime_len; (void)out_config_version;
    if (out_known) *out_known = false;
    if (out_config_crc) *out_config_crc = 0;
    return ESP_OK;
}

// M12b: a programmable stand-in for the ESP's cache of the Pico's committed
// record. Empty by default (safety_cfg_store_param_count() == 0), which is
// byte-for-byte the old fixed stub -- a test that wants the committed side of
// a read-back or a nameplate answer populates it explicitly.
#define TEST_CFG_ROWS_MAX 16
static safety_cfg_param_t s_cfg_rows[TEST_CFG_ROWS_MAX];
static size_t s_cfg_row_count = 0;
static bool s_cfg_refetch_ok = true;
// Set true to make refetch() re-point the store at whatever the pushes have
// staged -- i.e. model a Pico that really did commit what it was sent.
static bool s_cfg_refetch_applies_staged = false;
// opus review finding (MEDIUM): zones_current_sweep_start() now refuses to
// start when the safety param cache has never been fetched
// (safety_cfg_store_fetched_ms_ago() == UINT32_MAX). Defaults to "just
// fetched" (0) so every existing sweep-start test that doesn't care about
// this predicate keeps behaving exactly as before; a test that wants the
// unfetched-cache refusal sets this to UINT32_MAX explicitly.
static uint32_t s_cfg_fetched_ms_ago = 0;

static void test_ct_cal_reset(void); /* defined below -- forward declared so this reset stays first */

static void test_cfg_rows_reset(void)
{
    memset(s_cfg_rows, 0, sizeof(s_cfg_rows));
    s_cfg_row_count = 0;
    s_cfg_refetch_ok = true;
    s_cfg_refetch_applies_staged = false;
    s_cfg_fetched_ms_ago = 0;
    test_ct_cal_reset();
}

static void test_cfg_set_f32(uint16_t param_id, float v, bool is_set)
{
    for (size_t i = 0; i < s_cfg_row_count; i++) {
        if (s_cfg_rows[i].param_id == param_id) {
            s_cfg_rows[i].value.f32_val = v;
            s_cfg_rows[i].set = is_set;
            return;
        }
    }
    TEST_CHECK(s_cfg_row_count < TEST_CFG_ROWS_MAX, "test config row table has room");
    if (s_cfg_row_count >= TEST_CFG_ROWS_MAX) {
        return;
    }
    s_cfg_rows[s_cfg_row_count].param_id = param_id;
    s_cfg_rows[s_cfg_row_count].type = KILNLINK_PARAM_TYPE_F32;
    s_cfg_rows[s_cfg_row_count].value.f32_val = v;
    s_cfg_rows[s_cfg_row_count].set = is_set;
    s_cfg_row_count++;
}

bool safety_cfg_store_refetch(SafetyLinkClass *link, uint16_t config_crc)
{
    (void)link; (void)config_crc;
    if (!s_cfg_refetch_ok) {
        return false;
    }
    if (s_cfg_refetch_applies_staged) {
        // Every SET_PARAM this run sent, now "committed" -- the read-back a
        // Pico that genuinely applied the commit would answer with.
        // Only the f32 CT-scale ids: the map push stages u8 values into the
        // same union, and copying one of those back as an f32 would answer
        // zone_ct_map_committed_value() with a reinterpreted float.
        for (int i = 0; i < s_setparam_count && i < TEST_SETPARAM_LOG_MAX; i++) {
            uint16_t id = s_setparam_log[i].param_id;
            // 0x0308-0x030A: k_ct_v_per_a[0..2]. 0x031A onward: i_normal_a[zi]
            // -- added 2026-09-10 for zone_sweep_push_kct_and_inormal()'s
            // integration test, which needs a Pico fake that genuinely
            // applies BOTH param groups in the same staged commit, not just
            // k_ct alone.
            if ((id >= 0x0308u && id <= 0x030Au) ||
                (id >= 0x031Au && id < (uint16_t)(0x031Au + MAX31856_CHANNEL_COUNT))) {
                test_cfg_set_f32(id, s_setparam_log[i].value.f32_val, true);
            }
        }
    }
    return true;
}
size_t safety_cfg_store_param_count(void)
{
    return s_cfg_row_count;
}
uint32_t safety_cfg_store_fetched_ms_ago(void)
{
    return s_cfg_fetched_ms_ago;
}
bool safety_cfg_store_get_by_index(size_t index, safety_cfg_param_t *out)
{
    if (index >= s_cfg_row_count) {
        return false;
    }
    if (out) {
        *out = s_cfg_rows[index];
    }
    return true;
}

// CT_COMMISSIONING_PLAN.md steps 1/3 -- a programmable stand-in for
// safety_cfg_store's ct_cal record, same "empty by default, a test that
// wants it populates it explicitly" convention as s_cfg_rows above.
// zone_sweep_plan_k_ct() (manual-wins-over-sweep) and
// zone_sweep_task_record_normal()/_record_ct_channels() (summed topology)
// both call the real safety_cfg_store_get_ct_cal_input() signature; this
// test binary never links safety_cfg_store.c itself (see s_cfg_rows'
// comment), so it needs its own stub the same way get_by_index() does.
static bool s_ct_cal_has_value[SAFETY_CT_CAL_CHANNELS];
static float s_ct_cal_a_fs[SAFETY_CT_CAL_CHANNELS];
static float s_ct_cal_zero_mv[SAFETY_CT_CAL_CHANNELS];
static safety_ct_cal_source_t s_ct_cal_source[SAFETY_CT_CAL_CHANNELS];

static void test_ct_cal_reset(void)
{
    memset(s_ct_cal_has_value, 0, sizeof(s_ct_cal_has_value));
    memset(s_ct_cal_a_fs, 0, sizeof(s_ct_cal_a_fs));
    memset(s_ct_cal_zero_mv, 0, sizeof(s_ct_cal_zero_mv));
    memset(s_ct_cal_source, 0, sizeof(s_ct_cal_source));
}

static void test_ct_cal_set(size_t ch, float a_fs, float zero_mv, safety_ct_cal_source_t source)
{
    if (ch >= SAFETY_CT_CAL_CHANNELS) {
        return;
    }
    s_ct_cal_has_value[ch] = true;
    s_ct_cal_a_fs[ch] = a_fs;
    s_ct_cal_zero_mv[ch] = zero_mv;
    s_ct_cal_source[ch] = source;
}

bool safety_cfg_store_get_ct_cal_input(size_t ch, float *out_a_fs, float *out_zero_mv,
                                        safety_ct_cal_source_t *out_source)
{
    if (ch >= SAFETY_CT_CAL_CHANNELS || !s_ct_cal_has_value[ch]) {
        return false;
    }
    if (out_a_fs) *out_a_fs = s_ct_cal_a_fs[ch];
    if (out_zero_mv) *out_zero_mv = s_ct_cal_zero_mv[ch];
    if (out_source) *out_source = s_ct_cal_source[ch];
    return true;
}

bool safety_cfg_store_set_ct_cal_input(size_t ch, float a_fs, float zero_mv, safety_ct_cal_source_t source,
                                        float *out_k_ct_v_per_a, uint16_t *out_zero_counts,
                                        esp_err_t *out_nvs_err)
{
    // Not exercised via this path by any test in this file today (the real
    // HTTP handler is what calls this in practice) -- present only so the
    // real header's declaration is satisfied and a future test can drive it.
    (void)out_k_ct_v_per_a; (void)out_zero_counts;
    if (out_nvs_err) *out_nvs_err = ESP_OK;
    test_ct_cal_set(ch, a_fs, zero_mv, source);
    return true;
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
    z.heater_window_ms = 60000.0f;
    z.heater_min_on_ms = 100.0f;
    z.heater_min_off_ms = 100.0f;
    z.cross_zone_max_delta_c = 40.0f;
    z.tc_type = 3;
    z.model_k_dc = 12.0f;
    z.model_tau_s = 300.0f;
    z.model_dead_time_s = 30.0f;
    return z;
}

// The defect, reproduced directly against zones_http_parse_zone_fields(): a body that
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
                        "z0_maxtemp=1300&z0_mintemp=-20&z0_window=60000&z0_minon=0&z0_minoff=0";

    const uint8_t thermo_count = 1; // zone index 1 is past this -- the defect's exact trigger
    const uint8_t relay_count = 4;
    const uint8_t timing_profile_count = 1; // zone 1 is past thermo_count -- the early-return
                                            // preserve path never reaches z1_timingprofile's parse
    const char *err_reason = "unset";
    bool ok = zones_http_parse_zone_fields(body, /*i=*/1, thermo_count, relay_count, timing_profile_count,
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

// ZONES_CFG_VERSION 12->13: the whole-page POST /api/zones path is a SECOND
// gain-writing path (zones_http_parse_zone_fields() writes z->pid_kp/ki/kd directly,
// never through zones_config_set_pid()'s choke point -- see this function's
// own comment on why the invalidation had to be duplicated here) -- exactly
// the kind of second path this repo's reset-one-side bug class keeps
// producing when a pair of writers isn't found. This proves BOTH halves: an
// actual gain change invalidates, and an untouched gain leaves the record
// standing (an operator editing an unrelated field on the same page must not
// lose a good tuning record for no reason).
static void test_whole_page_post_invalidates_tuning_quality_only_when_gains_actually_change(void)
{
    TEST_SECTION("parse_zone_fields -- whole-page POST invalidates tuning_valid when pid_kp/ki/kd "
                 "actually change, and leaves it standing when they don't");

    zone_cfg_t current = make_stored_zone();
    current.tuning_valid = 1;
    current.tuning_method = 0;
    current.tuning_rule = 0;
    current.tuning_baseline_c = 25.0f;

    // Case 1: kp changed (2.0 -> 3.5) -- must invalidate.
    {
        zone_cfg_t out;
        memset(&out, 0, sizeof(out));
        const char *body = "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&z0_timingprofile=0&"
                            "z0_cal=1.5&z0_kp=3.5&z0_ki=0.3&z0_kd=0.05&z0_ramp=120&z0_sanity=5&z0_mode=2&"
                            "z0_maxtemp=1300&z0_mintemp=-10&z0_window=60000&z0_minon=0&z0_minoff=0";
        const char *err_reason = "unset";
        bool ok = zones_http_parse_zone_fields(body, /*i=*/0, /*thermo_count=*/1, /*relay_count=*/4,
                                    /*timing_profile_count=*/1, &current, &out, &err_reason);
        TEST_CHECK(ok, "a well-formed submission with a changed kp is accepted");
        TEST_CHECK_NEAR(out.pid_kp, 3.5f, 1e-6, "sanity: kp really did change in the parsed result");
        TEST_CHECK(!out.tuning_valid,
                  "an actual gain change on the whole-page path invalidates the tuning-quality record, "
                  "same as the narrow POST /api/zones/pid path");
    }

    // Case 2: every gain field resubmitted IDENTICAL to what is already
    // stored (the ordinary "change something else, resave" case) -- must
    // NOT invalidate.
    {
        zone_cfg_t out;
        memset(&out, 0, sizeof(out));
        const char *body = "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&z0_timingprofile=0&"
                            "z0_cal=1.5&z0_kp=2.0&z0_ki=0.3&z0_kd=0.05&z0_ramp=120&z0_sanity=5&z0_mode=2&"
                            "z0_maxtemp=1300&z0_mintemp=-10&z0_window=60000&z0_minon=0&z0_minoff=0";
        const char *err_reason = "unset";
        bool ok = zones_http_parse_zone_fields(body, /*i=*/0, /*thermo_count=*/1, /*relay_count=*/4,
                                    /*timing_profile_count=*/1, &current, &out, &err_reason);
        TEST_CHECK(ok, "a well-formed submission with unchanged gains is accepted");
        TEST_CHECK_NEAR(out.pid_kp, current.pid_kp, 1e-6, "sanity: kp is unchanged");
        TEST_CHECK(out.tuning_valid,
                  "resubmitting the SAME gains (an unrelated field changed) must leave a good "
                  "tuning-quality record standing, not wipe it for no reason");
    }
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
                        "z0_maxtemp=1300&z0_mintemp=-20&z0_window=60000&z0_minon=0&z0_minoff=0";
    const char *err_reason = "unset";
    bool ok = zones_http_parse_zone_fields(body, /*i=*/0, /*thermo_count=*/2, /*relay_count=*/4,
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
// asserts against the CURRENT (fixed) zones_http_parse_zone_fields(); the bug's old
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
// FIX 2 -- zones_config_json_parse_u8_field()/zones_config_json_parse_float_field() trailing-garbage rejection,
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
    bool ok = zones_config_json_parse_u8_field("thermo_count=3X", "thermo_count", 0, 10, &out);
    TEST_CHECK(!ok, "\"3X\" must be rejected outright, not silently accepted as 3");
    TEST_CHECK(out == 99, "out must be untouched on rejection");
}

static void test_parse_u8_field_accepts_clean_value(void)
{
    TEST_SECTION("parse_u8_field -- positive control: a clean in-range value is still accepted");
    uint8_t out = 0;
    bool ok = zones_config_json_parse_u8_field("thermo_count=3", "thermo_count", 0, 10, &out);
    TEST_CHECK(ok, "a clean value must still parse");
    TEST_CHECK(out == 3, "parsed value must be correct");
}

static void test_parse_float_field_rejects_trailing_garbage(void)
{
    TEST_SECTION("parse_float_field -- trailing garbage after a valid numeric prefix is rejected (FIX 2)");
    float out = -1.0f;
    bool ok = zones_config_json_parse_float_field("z0_kp=1200X", "z0_kp", 0.0f, 5000.0f, &out);
    TEST_CHECK(!ok, "\"1200X\" must be rejected outright, not silently accepted as 1200.0");
    TEST_CHECK(out == -1.0f, "out must be untouched on rejection");
}

static void test_parse_float_field_rejects_unit_suffix(void)
{
    TEST_SECTION("parse_float_field -- a value with a trailing unit suffix is rejected (FIX 2)");
    float out = -1.0f;
    bool ok = zones_config_json_parse_float_field("z0_maxtemp=1300C", "z0_maxtemp", 0.0f, 1400.0f, &out);
    TEST_CHECK(!ok, "\"1300C\" must be rejected outright, not silently accepted as 1300.0");
}

static void test_parse_float_field_accepts_clean_value(void)
{
    TEST_SECTION("parse_float_field -- positive control: a clean in-range value is still accepted");
    float out = 0.0f;
    bool ok = zones_config_json_parse_float_field("z0_kp=2.5", "z0_kp", 0.0f, 5000.0f, &out);
    TEST_CHECK(ok, "a clean value must still parse");
    TEST_CHECK_NEAR(out, 2.5, 1e-6, "parsed value must be correct");
}

// zones_post_handler() end-to-end, for the two inline strtol sites that have
// no standalone function to call directly. thermo_count=0/relay_count=0
// keeps the body minimal -- every zone block is then past thermo_count, so
// zones_http_parse_zone_fields() treats all its fields as optional/preserve-existing
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

// A single per-zone body block, shared by the whole-page cross-zone cycle
// test below -- every field zones_http_parse_zone_fields() requires for an in-range
// zone (thermo_count covers both zone 0 and zone 1 in that test).
#define TWO_ZONE_MINIMAL_BODY(SRC0, SRC1) \
    "thermo_count=2&relay_count=4&" MINIMAL_TIMING_PROFILE_BODY "&" \
    "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&" \
    "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=3&" \
    "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&z0_timingprofile=0&" \
    "z0_settings_source=" SRC0 "&" \
    "z1_name=Bottom&z1_tctype=2&z1_relay_mask=2&z1_thermo_mask=2&" \
    "z1_cal=0&z1_kp=1&z1_ki=0&z1_kd=0&z1_ramp=100&z1_sanity=0&z1_mode=3&" \
    "z1_maxtemp=1300&z1_mintemp=-20&z1_window=0&z1_minon=0&z1_minoff=0&z1_timingprofile=0&" \
    "z1_settings_source=" SRC1

// ---- POST /api/zones/pid -- narrow mid-firing PID-gain exception ----------
// zones_pid_post_handler() (zones_http.c). Same "stage the body, call the
// real handler, inspect the real side effects" discipline as run_zones_post()
// above -- s_test_post_body/content_len/httpd_resp_send_err/sendstr are all
// the SAME stub surface, so no new hooks were needed for this endpoint.
static void run_zones_pid_post(const char *body)
{
    test_post_hooks_reset();
    s_test_post_body = body;
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.content_len = (long long)strlen(body);
    esp_err_t err = zones_pid_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "zones_pid_post_handler must always return ESP_OK (errors go through httpd_resp_send_err)");
}

// Seeds a live two-zone config identical to TWO_ZONE_MINIMAL_BODY's own
// baseline (relay_mask/thermo_mask/control_mode/max_temp_c/etc all at known,
// non-default values) through the REAL whole-page path, so every "still
// forbidden while running" test below has a concrete baseline value to prove
// untouched, not just an assumed zero.
static void seed_two_zone_pid_baseline(void)
{
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    /* "255" == ZONE_SETTINGS_SOURCE_CUSTOM (zones_http.h) on both zones --
     * "0" would make zone 0 self-reference (zones_http_parse_zone_fields() refuses
     * that as a degenerate cycle), which is what the very first version of
     * this seed used and every test in this group failed at this line
     * because of it. */
    run_zones_post(TWO_ZONE_MINIMAL_BODY("255", "255"));
    TEST_CHECK(!s_test_err_called, "baseline seed must itself be accepted");
}

static void test_zones_pid_post_accepts_while_profile_running(void)
{
    TEST_SECTION("POST /api/zones/pid -- a PID-only change is ACCEPTED while profile_executor reports RUNNING");
    seed_two_zone_pid_baseline();
    s_test_profile_status.state = PROFILE_EXEC_RUNNING; /* zones_pid_post_handler() must not even look at this --
                                                           * proven by the fact this still succeeds. */
    run_zones_pid_post("zone=0&kp=0.0318&ki=0.00012&kd=0.8401");
    s_test_profile_status.state = PROFILE_EXEC_IDLE;
    TEST_CHECK(!s_test_err_called, "a PID-only submit must not be refused just because a firing is running");
    TEST_CHECK(s_test_ok_called, "a PID-only submit must report success");
    float kp = 0.0f, ki = 0.0f, kd = 0.0f;
    TEST_CHECK(zones_config_get_pid(0, &kp, &ki, &kd), "zone 0 must still be readable after the write");
    TEST_CHECK_NEAR(kp, 0.0318, 1e-6, "kp must be applied exactly");
    TEST_CHECK_NEAR(ki, 0.00012, 1e-9, "ki (~1e-4 magnitude) must be applied exactly, not rounded toward zero");
    TEST_CHECK_NEAR(kd, 0.8401, 1e-6, "kd must be applied exactly");
}

static void test_zones_pid_post_bumps_generation(void)
{
    TEST_SECTION("POST /api/zones/pid -- the generation counter bumps, so profile_executor will observe the change");
    seed_two_zone_pid_baseline();
    uint32_t gen_before = zones_config_generation();
    run_zones_pid_post("zone=1&kp=0.0485&ki=0.00018&kd=1.0548");
    TEST_CHECK(!s_test_err_called, "clean submit must not be refused");
    TEST_CHECK(zones_config_generation() > gen_before,
              "s_config_generation must advance on a successful PID write, or reload_config_if_changed() "
              "will never notice the new gains");
}

static void test_zones_pid_post_rejects_kp_over_bound(void)
{
    TEST_SECTION("POST /api/zones/pid -- kp above ZONE_PID_GAIN_MAX is refused by the SHARED validator");
    seed_two_zone_pid_baseline();
    float kp_before = 0.0f, ki_before = 0.0f, kd_before = 0.0f;
    zones_config_get_pid(0, &kp_before, &ki_before, &kd_before);
    run_zones_pid_post("zone=0&kp=1000.001&ki=0.0001&kd=1.0");
    TEST_CHECK(s_test_err_called, "kp past 1000.0 must be refused");
    TEST_CHECK(!s_test_ok_called, "must not report success for a rejected submission");
    float kp = 0.0f, ki = 0.0f, kd = 0.0f;
    zones_config_get_pid(0, &kp, &ki, &kd);
    TEST_CHECK_NEAR(kp, kp_before, 1e-9, "a rejected submission must not partially apply -- kp must be unchanged");
}

static void test_zones_pid_post_rejects_negative_ki(void)
{
    TEST_SECTION("POST /api/zones/pid -- a negative ki is refused by the SHARED validator");
    seed_two_zone_pid_baseline();
    run_zones_pid_post("zone=0&kp=0.03&ki=-0.0001&kd=1.0");
    TEST_CHECK(s_test_err_called, "a negative ki must be refused, matching zones_config_json_parse_float_field()'s "
                                  "0.0f floor");
    TEST_CHECK(!s_test_ok_called, "must not report success for a rejected submission");
}

static void test_zones_pid_post_rejects_non_numeric_kd(void)
{
    TEST_SECTION("POST /api/zones/pid -- a non-numeric kd is refused by the SHARED validator");
    seed_two_zone_pid_baseline();
    run_zones_pid_post("zone=0&kp=0.03&ki=0.0001&kd=notanumber");
    TEST_CHECK(s_test_err_called, "a non-numeric kd must be refused");
    TEST_CHECK(!s_test_ok_called, "must not report success for a rejected submission");
}

static void test_zones_pid_post_rejects_zone_out_of_range(void)
{
    TEST_SECTION("POST /api/zones/pid -- a zone index at or past thermo_count is refused");
    seed_two_zone_pid_baseline(); /* thermo_count=2 -- zone 2 does not exist */
    run_zones_pid_post("zone=2&kp=0.03&ki=0.0001&kd=1.0");
    TEST_CHECK(s_test_err_called, "an unconfigured zone index must be refused");
    TEST_CHECK(!s_test_ok_called, "must not report success for a rejected submission");
}

// ---- "Still forbidden while running" -- one test PER field, proving the
// field cannot be smuggled through this fixed-shape endpoint (it parses
// ONLY zone/kp/ki/kd, so an attacker-controlled extra key in the same POST
// body is simply never looked at). Each test posts a clean, ACCEPTED PID
// change alongside an attempted edit of one forbidden field (using the SAME
// key zones_http_parse_zone_fields()/the whole-page path would recognise), then proves
// that field's live value is exactly the seeded baseline -- not the smuggled
// value.
static void test_zones_pid_post_ignores_relay_mask(void)
{
    TEST_SECTION("POST /api/zones/pid -- relay_mask / zone membership stays refused while running "
                "(cannot be smuggled through this endpoint)");
    seed_two_zone_pid_baseline();
    run_zones_pid_post("zone=0&kp=0.05&ki=0.0002&kd=1.0&z0_relay_mask=2&relay_mask=2");
    TEST_CHECK(!s_test_err_called, "the PID part of this submit is clean and must still succeed");
    uint8_t mask = 0xFF;
    zones_config_get_relay_mask(0, &mask);
    TEST_CHECK(mask == 1, "relay_mask must remain the seeded value (1) -- z0_relay_mask=2 must be ignored");
}

static void test_zones_pid_post_ignores_control_mode(void)
{
    TEST_SECTION("POST /api/zones/pid -- control_mode stays refused while running");
    seed_two_zone_pid_baseline();
    run_zones_pid_post("zone=0&kp=0.05&ki=0.0002&kd=1.0&z0_mode=0&control_mode=0&mode=0");
    TEST_CHECK(!s_test_err_called, "the PID part of this submit is clean and must still succeed");
    TEST_CHECK(s_zones.cfg.zones[0].control_mode == 3,
              "control_mode must remain the seeded value (3, PID_FUZZY) -- z0_mode=0 must be ignored");
}

static void test_zones_pid_post_ignores_max_temp(void)
{
    TEST_SECTION("POST /api/zones/pid -- max_temp_c/min_temp_c stay refused while running");
    seed_two_zone_pid_baseline();
    run_zones_pid_post("zone=0&kp=0.05&ki=0.0002&kd=1.0&z0_maxtemp=1&maxtemp=1&z0_mintemp=-999&mintemp=-999");
    TEST_CHECK(!s_test_err_called, "the PID part of this submit is clean and must still succeed");
    TEST_CHECK_NEAR(s_zones.cfg.zones[0].max_temp_c, 1300.0, 1e-6,
                    "max_temp_c must remain the seeded value -- z0_maxtemp=1 must be ignored");
    TEST_CHECK_NEAR(s_zones.cfg.zones[0].min_temp_c, -20.0, 1e-6,
                    "min_temp_c must remain the seeded value -- z0_mintemp=-999 must be ignored");
}

static void test_zones_pid_post_ignores_max_ramp(void)
{
    TEST_SECTION("POST /api/zones/pid -- max_ramp_c_per_hr stays refused while running");
    seed_two_zone_pid_baseline();
    run_zones_pid_post("zone=0&kp=0.05&ki=0.0002&kd=1.0&z0_ramp=1&ramp=1");
    TEST_CHECK(!s_test_err_called, "the PID part of this submit is clean and must still succeed");
    TEST_CHECK_NEAR(s_zones.cfg.zones[0].max_ramp_c_per_hr, 100.0, 1e-6,
                    "max_ramp_c_per_hr must remain the seeded value -- z0_ramp=1 must be ignored");
}

static void test_zones_pid_post_ignores_cal_offset(void)
{
    TEST_SECTION("POST /api/zones/pid -- cal_offset_c stays refused while running");
    seed_two_zone_pid_baseline();
    run_zones_pid_post("zone=0&kp=0.05&ki=0.0002&kd=1.0&z0_cal=25&cal=25");
    TEST_CHECK(!s_test_err_called, "the PID part of this submit is clean and must still succeed");
    TEST_CHECK_NEAR(s_zones.cfg.zones[0].cal_offset_c, 0.0, 1e-6,
                    "cal_offset_c must remain the seeded value (0) -- z0_cal=25 must be ignored");
}

static void test_zones_pid_post_ignores_guard_thresholds(void)
{
    TEST_SECTION("POST /api/zones/pid -- guard thresholds stay refused while running");
    seed_two_zone_pid_baseline();
    run_zones_pid_post("zone=0&kp=0.05&ki=0.0002&kd=1.0&z0_wrongdirwindow=999&wrongdirwindow=999&"
                       "z0_runawaymargin=1&runawaymargin=1");
    TEST_CHECK(!s_test_err_called, "the PID part of this submit is clean and must still succeed");
    TEST_CHECK_NEAR(s_zones.cfg.zones[0].guard_wrong_dir_window_s, 0.0, 1e-6,
                    "guard_wrong_dir_window_s must remain the seeded value (0) -- must be ignored");
    TEST_CHECK_NEAR(s_zones.cfg.zones[0].guard_runaway_margin_c, 0.0, 1e-6,
                    "guard_runaway_margin_c must remain the seeded value (0) -- must be ignored");
}

static void test_zones_pid_post_ignores_coupling_matrix(void)
{
    TEST_SECTION("POST /api/zones/pid -- the coupling matrix stays refused while running");
    seed_two_zone_pid_baseline();
    run_zones_pid_post("zone=0&kp=0.05&ki=0.0002&kd=1.0&z0_coupling_c1=999&coupling_c1=999");
    TEST_CHECK(!s_test_err_called, "the PID part of this submit is clean and must still succeed");
    TEST_CHECK_NEAR(s_zones.cfg.zones[0].coupling_coeff[1], 0.0, 1e-6,
                    "coupling_coeff[1] must remain the seeded value (0) -- z0_coupling_c1=999 must be ignored");
}

static void test_zones_pid_post_ignores_model_parameters(void)
{
    TEST_SECTION("POST /api/zones/pid -- the plant model (K_dc/tau) stays refused while running");
    seed_two_zone_pid_baseline();
    run_zones_pid_post("zone=0&kp=0.05&ki=0.0002&kd=1.0&z0_k=99&k=99&z0_tau=99&tau=99");
    TEST_CHECK(!s_test_err_called, "the PID part of this submit is clean and must still succeed");
    TEST_CHECK_NEAR(s_zones.cfg.zones[0].model_k_dc, 0.0, 1e-6,
                    "model_k_dc must remain the seeded value (0) -- z0_k=99 must be ignored");
    TEST_CHECK_NEAR(s_zones.cfg.zones[0].model_tau_s, 0.0, 1e-6,
                    "model_tau_s must remain the seeded value (0) -- z0_tau=99 must be ignored");
}

static void test_zones_pid_post_ignores_thermo_mask(void)
{
    TEST_SECTION("POST /api/zones/pid -- thermo_mask/ct_mask stay refused while running");
    seed_two_zone_pid_baseline();
    run_zones_pid_post("zone=0&kp=0.05&ki=0.0002&kd=1.0&z0_thermo_mask=4&thermo_mask=4");
    TEST_CHECK(!s_test_err_called, "the PID part of this submit is clean and must still succeed");
    uint8_t mask = 0xFF;
    zones_config_get_thermo_mask(0, &mask);
    TEST_CHECK(mask == 1, "thermo_mask must remain the seeded value (1) -- z0_thermo_mask=4 must be ignored");
}

static void test_zones_pid_post_ignores_settings_source(void)
{
    TEST_SECTION("POST /api/zones/pid -- settings_source (coupling to the safety-relevant zone) stays refused "
                "while running");
    seed_two_zone_pid_baseline();
    run_zones_pid_post("zone=0&kp=0.05&ki=0.0002&kd=1.0&z0_settings_source=1&settings_source=1");
    TEST_CHECK(!s_test_err_called, "the PID part of this submit is clean and must still succeed");
    TEST_CHECK(s_zones.cfg.zones[0].settings_source[SRC_GROUP_LIMITS] == 0xFF,
              "settings_source must remain the seeded value (0xFF, CUSTOM) -- z0_settings_source=1 must be ignored");
}

static void test_zones_pid_post_ignores_safety_tc_type(void)
{
    TEST_SECTION("POST /api/zones/pid -- safety_tc_type (mirrored to the RP2040 safety processor) stays "
                "refused while running");
    seed_two_zone_pid_baseline();
    run_zones_pid_post("zone=0&kp=0.05&ki=0.0002&kd=1.0&safety_tc_type=5");
    TEST_CHECK(!s_test_err_called, "the PID part of this submit is clean and must still succeed");
    TEST_CHECK(s_zones.cfg.safety_tc_type == 0,
              "safety_tc_type must remain the seeded value (0) -- safety_tc_type=5 must be ignored");
}

// zones_http_parse_zone_fields()'s own per-zone chain-walk only ever checks the zone
// being written against every OTHER zone's LIVE stored value -- it cannot
// see a SECOND zone changing in the very same whole-page POST. z0's own
// entry (z0_settings_source=1) is not a cycle against the pre-POST live
// config (both zones start Custom -- see zones_post_handler()'s own body,
// tmp is zero-initialized, and s_zones.cfg is never touched by this test
// group before this call), and neither is z1's own entry
// (z1_settings_source=0) checked in isolation -- only TOGETHER, once both
// are assembled into tmp.zones[], do they close 0 -> 1 -> 0. This is exactly
// what zones_post_handler()'s own post-loop re-walk (right after the
// per-zone zones_http_parse_zone_fields() loop, before the commit point) exists to
// catch.
static void test_post_whole_page_cross_zone_cycle_refused(void)
{
    TEST_SECTION("zones_post_handler -- two zones' settings_source keys in the SAME whole-page POST "
                 "that only close a cycle TOGETHER (neither is a cycle against the live config alone) "
                 "are refused, before either is committed");
    /* Explicit clean slate (Custom/Custom) -- run_zones_post() does not
     * reset s_zones.cfg, and this door's per-zone cross-check (inside
     * zones_http_parse_zone_fields(), for the single-zone case) also consults the live
     * config for every OTHER zone, so a leftover raw-zero from an earlier
     * test (or this file's own zero-initialized BSS default) must not leak
     * in as an accidental link -- see zones_config_json_settings_source_chain_has_cycle()'s
     * own comment on why 0 is real data, not a "not set" sentinel. */
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 2; /* the getters below refuse zone_index >= thermo_count */
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
            s_zones.cfg.zones[z].settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
        }
    }
    run_zones_post(TWO_ZONE_MINIMAL_BODY("1", "0"));
    TEST_CHECK(s_test_err_called, "the whole submission is refused");
    TEST_CHECK(!s_test_ok_called, "must not report success for a rejected submission");
    TEST_CHECK(strstr(s_test_err_msg, "settings_source") != NULL, "the refusal names the field");
    TEST_CHECK(strstr(s_test_err_msg, "cycle") != NULL, "and calls out the cycle specifically");
    uint8_t s0 = 0xAA, s1 = 0xAA;
    TEST_CHECK(zones_config_get_settings_source(0, SRC_GROUP_LIMITS, &s0) && s0 == ZONE_SETTINGS_SOURCE_CUSTOM &&
              zones_config_get_settings_source(1, SRC_GROUP_LIMITS, &s1) && s1 == ZONE_SETTINGS_SOURCE_CUSTOM,
              "NOTHING was committed -- both zones are still at their pre-POST Custom default, not "
              "half-applied with zone 0's individually-legal-looking link written and zone 1's not");
}

// Positive control: the identical two-zone whole-page shape, but with a
// legal (acyclic) chain -- proves the post-loop re-walk refuses only a
// genuine cycle, not any submission that happens to touch two zones'
// settings_source in the same POST.
static void test_post_whole_page_cross_zone_legal_chain_accepted(void)
{
    TEST_SECTION("zones_post_handler -- a legal (acyclic) settings_source chain spanning two zones in "
                 "the same whole-page POST is still accepted");
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 2;
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
            s_zones.cfg.zones[z].settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
        }
    }
    run_zones_post(TWO_ZONE_MINIMAL_BODY("1", "255"));
    TEST_CHECK(!s_test_err_called, "a legal two-zone chain (0 -> 1 -> Custom) must not be rejected");
    TEST_CHECK(s_test_ok_called, "and must report success");
    uint8_t s0 = 0xAA, s1 = 0xAA;
    TEST_CHECK(zones_config_get_settings_source(0, SRC_GROUP_LIMITS, &s0) && s0 == 1, "zone 0's link committed as 1");
    TEST_CHECK(zones_config_get_settings_source(1, SRC_GROUP_LIMITS, &s1) && s1 == ZONE_SETTINGS_SOURCE_CUSTOM,
              "zone 1's link committed as Custom");
}

// ---------------------------------------------------------------------------
// FIX 1 -- a found-but-refused newer-version zones blob must not look like
// "nothing found" to the legacy-migration decision, and must never be
// overwritten by it. nvs_load_from() is `static`; reached directly, same
// convention as zones_http_parse_zone_fields() above. Uses stubs/nvs.h's opt-in
// single-blob-slot NVS stub (nvs_test_enable()/nvs_test_clear()) -- off by
// default, so every test above this section (which never touches NVS) is
// unaffected.
// ---------------------------------------------------------------------------

static void stage_zones_blob(const void *data, size_t len)
{
    // fake_kv, unlike the retired single-slot stub, is a real per-partition
    // store -- this MUST target KILN_NVS_PARTITION ("kiln_nvs"), the same
    // partition nvs_load_from()/nvs_load() below actually read, or the
    // staged bytes would simply never be seen.
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    (void)err; // the fake always succeeds once hal_kv_init_partition() has been called (nvs_test_enable(true))
    hal_kv_set_blob(&h, NVS_KEY_ZONES, data, len);
    hal_kv_commit(&h);
    hal_kv_close(&h);
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

// 2026-09-09 (opus review defect D). The model_fit_temp_c/model_fit_ambient_c
// sentinel backfill existed only in the MIGRATION branch. A board booting on
// blank/unreadable/refused/corrupt NVS keeps the zero-initialised struct, so
// both fields read 0.0f -- and zones_config_json_validate() accepts 0.0 (it
// only rejects <= -50 and >= 1300), so a virgin board reported every zone as
// honestly "fitted at 0 C", the exact value zone_cfg_t's comment says must
// never mean "unknown". Covers both no-record and corrupt, since they are
// different return paths.
static void test_nvs_load_from_defaults_carry_the_model_fit_unknown_sentinel(void)
{
    TEST_SECTION("nvs_load_from -- a DEFAULTS config (nothing stored, and separately a corrupt blob) "
                 "reports model_fit_temp_c/model_fit_ambient_c as the UNKNOWN sentinel, never 0.0 "
                 "(which is a plausible genuine ambient and would read as a real fit)");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_t out_cfg;
    bool found = true, valid = true;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);
    TEST_CHECK(err == ESP_OK && !found && !valid, "precondition: nothing stored -- defaults");
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        TEST_CHECK(out_cfg.zones[z].model_fit_temp_c == ZONE_MODEL_FIT_TEMP_UNKNOWN,
                   "a never-configured zone's model_fit_temp_c must be the UNKNOWN sentinel, not 0.0");
        TEST_CHECK(out_cfg.zones[z].model_fit_ambient_c == ZONE_MODEL_FIT_TEMP_UNKNOWN,
                   "and the same for model_fit_ambient_c");
    }

    // Genuine corruption takes a different return path (the switch's CORRUPT
    // case, after zones_config_json_decode_blob() has re-zeroed the struct)
    // -- it must land on the sentinel too.
    stage_zones_blob("", 0);
    found = true; valid = true;
    err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);
    TEST_CHECK(err == ESP_OK && !valid, "precondition: corrupt blob -- defaults again");
    TEST_CHECK(out_cfg.zones[0].model_fit_temp_c == ZONE_MODEL_FIT_TEMP_UNKNOWN,
               "the corrupt path must land on the sentinel as well -- decode_blob re-zeroes the "
               "struct after this function's own entry memset, so a fix applied only at entry "
               "would silently miss this path");

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
    src.timing_profile_count = 1; // must never be 0 in a config zones_config_json_validate() accepts
    src.crc32 = zones_config_json_compute_crc(&src); // CRC now checked on the current-version path (item 4)
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

// A pre-existing stored settings_source cycle (however it got onto flash --
// direct NVS tampering, or a blob written by firmware that predates the
// chain-walk guard) must NOT brick the config on load. zones_config_json_validate()
// rejecting it would drive zones_config_json_decode_blob() to the CORRUPT/wipe outcome
// and destroy an entire commissioned config over one stale UI-only
// provenance link -- this repo has lost configs to exactly that shape of
// overreaction before (raise_heater_timing_to_floors()'s neighboring
// precedent, cited by zones_config_json_normalize_settings_source_cycles()'s own comment). So
// nvs_load_from() must instead collapse the cyclic zone(s) to Custom and
// keep booting with everything else intact, the same "collapse to Custom"
// resolution zones_page.html's own client-side resolveTerminal() already
// performs on a stale page load.
static void test_nvs_load_from_pre_existing_cycle_normalizes_not_wipes(void)
{
    TEST_SECTION("nvs_load_from -- a pre-existing stored settings_source cycle collapses to Custom on "
                 "load instead of being rejected as corrupt -- must not wipe a commissioned config over "
                 "a stale UI-only provenance link");
    nvs_test_enable(true);
    nvs_test_clear();
    zones_cfg_t src;
    memset(&src, 0, sizeof(src));
    src.version = ZONES_CFG_VERSION;
    src.thermo_count = 3;
    src.timing_profile_count = 1;
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
            src.zones[z].settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
        }
    }
    // The stored 2-cycle this same guard refuses at write time -- reachable
    // here only because it predates the guard (or was written by some other
    // means entirely outside this file's own setters/parser).
    src.zones[0].settings_source[SRC_GROUP_LIMITS] = 1;
    src.zones[1].settings_source[SRC_GROUP_LIMITS] = 0;
    // A real, meaningful field on the cyclic zone, to prove normalization
    // touches ONLY settings_source and does not zero the rest of the zone.
    src.zones[0].pid_kp = 7.25f;
    src.zones[0].tc_type = 5;
    // A THIRD zone with a legal (non-cyclic) link, to prove the collapse is
    // scoped to the zone(s) actually on a cycle, not a blanket "any zone
    // with a real link gets wiped" overreaction.
    src.zones[2].settings_source[SRC_GROUP_LIMITS] = 0; // "copies zone 0" -- legal once zone 0 is Custom below
    src.crc32 = zones_config_json_compute_crc(&src);
    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found, "a stored cycle is still a REAL blob -- found=true");
    TEST_CHECK(valid, "and it is still VALID -- the cycle is normalized, not treated as corrupt "
              "(this is the assertion that proves normalize-not-reject: a zones_config_json_validate() "
              "rejection here would flip this to false and wipe out_cfg via zones_config_json_decode_blob()'s "
              "CORRUPT path)");
    TEST_CHECK(out_cfg.thermo_count == 3, "the rest of the config survives the load untouched");
    uint8_t s0 = 0xAA, s1 = 0xAA;
    TEST_CHECK(out_cfg.zones[0].settings_source[SRC_GROUP_LIMITS] != 1 || out_cfg.zones[1].settings_source[SRC_GROUP_LIMITS] != 0,
              "the stored 2-cycle no longer exists in the decoded config -- at least one of the two "
              "links was broken by normalization");
    s0 = out_cfg.zones[0].settings_source[SRC_GROUP_LIMITS];
    s1 = out_cfg.zones[1].settings_source[SRC_GROUP_LIMITS];
    TEST_CHECK(s0 == ZONE_SETTINGS_SOURCE_CUSTOM || s1 == ZONE_SETTINGS_SOURCE_CUSTOM,
              "the break was made by collapsing (at least) one of the two cyclic zones to Custom, the "
              "same resolution zones_page.html's own client-side cycle guard already uses -- not some "
              "other repair");
    TEST_CHECK_NEAR(out_cfg.zones[0].pid_kp, 7.25f, 1e-6,
                    "normalization touches ONLY settings_source -- zone 0's pid_kp survives intact, "
                    "proving this is not a wipe of the zone or the config");
    TEST_CHECK(out_cfg.zones[0].tc_type == 5, "and zone 0's tc_type survives intact too");

    nvs_test_enable(false);
    nvs_test_clear();
}

// zones_config_json_normalize_settings_source_cycles() non-blocking finding (opus review of
// commit b69b74a): a zone that merely LEADS INTO a cycle (its own chain is
// fine, it just happens to walk into one) must be left untouched -- only the
// zone(s) actually ON the cycle may be collapsed to Custom. An earlier
// version of this function walked start-to-finish in index order and reset
// `start`'s own link whenever ITS walk hit a cycle, which is index-order
// dependent: with a stored 1<->2 cycle and zone 0 -> 1 merely leading into
// it, scanning from i=0 would hit the cycle via zone 0's own walk and reset
// zone 0's OWN (innocent, non-cyclic) link first, even though breaking 1<->2
// alone would have sufficed. This test seeds exactly that shape and proves
// zone 0's link survives.
static void test_nvs_load_from_cycle_normalization_does_not_touch_lead_in_zone(void)
{
    TEST_SECTION("nvs_load_from -- zones_config_json_normalize_settings_source_cycles() breaks ONLY the zone(s) actually "
                 "ON a cycle, not a zone that merely leads into one (index-order independence)");
    nvs_test_enable(true);
    nvs_test_clear();
    zones_cfg_t src;
    memset(&src, 0, sizeof(src));
    src.version = ZONES_CFG_VERSION;
    src.thermo_count = 3;
    src.timing_profile_count = 1;
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
            src.zones[z].settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
        }
    }
    // Zone 0 leads into the cycle (0 -> 1) but is not itself part of it.
    src.zones[0].settings_source[SRC_GROUP_LIMITS] = 1;
    // The actual 1 <-> 2 cycle.
    src.zones[1].settings_source[SRC_GROUP_LIMITS] = 2;
    src.zones[2].settings_source[SRC_GROUP_LIMITS] = 1;
    src.crc32 = zones_config_json_compute_crc(&src);
    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a stored cycle collapses, it does not get treated as corrupt");
    TEST_CHECK(out_cfg.zones[0].settings_source[SRC_GROUP_LIMITS] == 1,
              "zone 0's link (0 -> 1) survives untouched -- it was never ON the cycle, only LEADING "
              "INTO it, and breaking the real 1<->2 cycle alone is sufficient to fix zone 0's chain too");
    TEST_CHECK(out_cfg.zones[1].settings_source[SRC_GROUP_LIMITS] != 2 || out_cfg.zones[2].settings_source[SRC_GROUP_LIMITS] != 1,
              "the actual 1<->2 cycle no longer exists -- at least one of its two links was broken");
    uint8_t s1 = out_cfg.zones[1].settings_source[SRC_GROUP_LIMITS], s2 = out_cfg.zones[2].settings_source[SRC_GROUP_LIMITS];
    TEST_CHECK(s1 == ZONE_SETTINGS_SOURCE_CUSTOM || s2 == ZONE_SETTINGS_SOURCE_CUSTOM,
              "the break was made by collapsing (at least) one of the two cyclic zones to Custom");

    nvs_test_enable(false);
    nvs_test_clear();
}

// zones_config_import_blob()'s own pass-1 settings_source cycle check (added
// alongside the two-pass restore fix -- see backup_http.c's importer, which
// calls THIS function to apply a decoded blob) had no test at all before
// this pair, positive or negative: nvs_load_from()'s cycle tests above only
// exercise zones_config_json_normalize_settings_source_cycles() on the LOAD path, which
// deliberately COLLAPSES a cycle rather than rejecting it -- a completely
// different code path from zones_config_import_blob()'s pass-1 walk, which
// must REJECT instead (see that function's own comment: "there is a live
// client on the other end of this call who can be handed a clear reason").
// Negative first: a genuinely cyclic blob must be refused, with the commit
// point (s_zones.cfg = cand) never reached -- proven by checking the live
// config is untouched, not just that the call returns false.
static void test_import_blob_pass1_rejects_cyclic_settings_source(void)
{
    TEST_SECTION("zones_config_import_blob -- pass 1: a genuinely cyclic settings_source blob is "
                 "REFUSED, nothing committed to the live config");
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 2;
    s_zones.cfg.timing_profile_count = 1;
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
            s_zones.cfg.zones[z].settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
        }
    }
    s_zones.cfg.zones[0].max_temp_c = 1234.0f; // a live value the rejected import must not disturb
    s_zones_config_valid = true;
    uint32_t generation_before = s_config_generation;

    zones_cfg_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = ZONES_CFG_VERSION;
    blob.thermo_count = 2;
    blob.timing_profile_count = 1;
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
            blob.zones[z].settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
        }
    }
    blob.zones[0].settings_source[SRC_GROUP_LIMITS] = 1; // the 2-cycle this pass-1 check must catch
    blob.zones[1].settings_source[SRC_GROUP_LIMITS] = 0;
    blob.crc32 = zones_config_json_compute_crc(&blob);

    char reason[128];
    reason[0] = '\0';
    bool ok = zones_config_import_blob(&blob, sizeof(blob), reason, sizeof(reason));

    TEST_CHECK(!ok, "a cyclic settings_source import is refused");
    TEST_CHECK(strstr(reason, "settings_source") != NULL && strstr(reason, "cycle") != NULL,
              "the refusal names the field and calls out the cycle specifically");
    TEST_CHECK(s_zones.cfg.zones[0].max_temp_c == 1234.0f,
              "the pre-existing live config is completely untouched -- pass 1 rejected before the "
              "commit point, not partway through it");
    TEST_CHECK(s_zones.cfg.zones[0].settings_source[SRC_GROUP_LIMITS] == ZONE_SETTINGS_SOURCE_CUSTOM &&
              s_zones.cfg.zones[1].settings_source[SRC_GROUP_LIMITS] == ZONE_SETTINGS_SOURCE_CUSTOM,
              "the live settings_source links are still Custom -- the cyclic blob's links never landed");
    TEST_CHECK(s_config_generation == generation_before, "no config generation bump for a rejected import");
}

// Positive control for the test above: the identical shape, but the blob's
// settings_source chain is legal (acyclic) -- proves pass 1 refuses ONLY a
// genuine cycle, not any blob that happens to link two zones' settings_source
// together, and that a legitimate import still lands.
static void test_import_blob_pass1_accepts_acyclic_settings_source(void)
{
    TEST_SECTION("zones_config_import_blob -- pass 1: a legal (acyclic) settings_source chain is "
                 "accepted and actually committed");
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 2;
    s_zones.cfg.timing_profile_count = 1;
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
            s_zones.cfg.zones[z].settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
        }
    }
    s_zones_config_valid = false;

    zones_cfg_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = ZONES_CFG_VERSION;
    blob.thermo_count = 2;
    blob.timing_profile_count = 1;
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
            blob.zones[z].settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
        }
    }
    blob.zones[0].settings_source[SRC_GROUP_LIMITS] = 1; // 0 -> 1 -> Custom: terminates cleanly, no cycle
    blob.crc32 = zones_config_json_compute_crc(&blob);

    char reason[128];
    reason[0] = '\0';
    bool ok = zones_config_import_blob(&blob, sizeof(blob), reason, sizeof(reason));

    TEST_CHECK(ok, "an acyclic settings_source import is accepted");
    TEST_CHECK(s_zones_config_valid, "and marked valid/committed");
    TEST_CHECK(s_zones.cfg.zones[0].settings_source[SRC_GROUP_LIMITS] == 1,
              "the imported link is actually live in the committed config");
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
// from "kiln_nvs has never had anything saved". Under fake_kv (a real
// per-partition store, unlike the retired stub's single shared slot),
// migrate_from_default_partition() cannot even reach a decision here: its
// own nvs_load_from(NVS_DEFAULT_PART_NAME, ...) call opens a partition
// ("nvs") that was never hal_kv_init_partition()'d in this test, so it comes
// back not-OK and migrate_from_default_partition() returns immediately --
// but the real guard under test is zones_http_start()'s own
// found_in_kiln_nvs check, which must be true for a refused-newer blob and
// therefore must skip calling migrate_from_default_partition() at all. The
// load-bearing assertion is s_zones_config_valid staying false AND the
// bytes actually stored under kiln_nvs staying byte-for-byte identical
// after the call: an nvs_save() from ANY path (migration or otherwise)
// would stamp a fresh ZONES_CFG_VERSION into byte 0, which the staged
// (ZONES_CFG_VERSION + 1) can never equal.
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
    hal_kv_handle_t readback_h;
    hal_status_t readback_err = hal_kv_open(&readback_h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    uint8_t readback[sizeof(zones_cfg_t)];
    size_t readback_len = sizeof(readback);
    if (readback_err == HAL_OK) {
        readback_err = hal_kv_get_blob(&readback_h, NVS_KEY_ZONES, readback, &readback_len);
        hal_kv_close(&readback_h);
    }
    TEST_CHECK(readback_err == HAL_OK && readback_len == sizeof(src) &&
                  memcmp(readback, blob_before, sizeof(src)) == 0,
              "FIX 1: the on-flash blob must be byte-for-byte unchanged -- if migrate_from_default_"
              "partition() ran and saved, nvs_save() would have stamped ZONES_CFG_VERSION (not "
              "ZONES_CFG_VERSION+1) into byte 0, which this check catches");

    nvs_test_enable(false);
    nvs_test_clear();
}

// ---------------------------------------------------------------------------
// "Saved securely like the others" -- the real defect this pass fixes.
// nvs_load_from()'s old load path (for a stored version OLDER than current)
// did no length check, never ran zones_config_json_validate(), assumed zone_cfg_t
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
    src.crc32 = zones_config_json_compute_crc(&src) ^ 0x1u; // one bit off from the real CRC
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

// Item 3 -- zones_config_json_validate() now runs on the NVS load path too, not just
// zones_config_import_blob(). A current-version blob with the right length
// AND a correct CRC (proving the bytes are exactly what was written) but an
// out-of-range field must still be rejected, not adopted with the valid flag
// set -- the config was written wrong in the first place, and a correct CRC
// over wrong data is not a reason to trust it.
static void test_nvs_load_from_failed_validation_is_rejected(void)
{
    TEST_SECTION("nvs_load_from -- item 3: length+CRC correct but zones_config_json_validate() fails "
                 "-> rejected, not partially adopted");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_t src;
    memset(&src, 0, sizeof(src));
    src.version = ZONES_CFG_VERSION;
    src.thermo_count = 1;
    src.zones[0].max_temp_c = 999999.0f; // past ZONE_MAX_TEMP_C_MAX -- zones_config_json_validate() must reject
    src.crc32 = zones_config_json_compute_crc(&src); // CRC is genuinely correct for these (bad) bytes
    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = true, valid = true;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "a validation failure is a handled outcome, not an NVS error");
    TEST_CHECK(!found, "a zones_config_json_validate() failure must be rejected (found=false), matching "
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

// Dedicated v1 blob test (v1/v2 previously only reached indirectly via
// convert_zone_v1() as invoked by convert_zone_v3()/etc.'s own tests). v1
// predates continue_on_zone_trip/safety_tc_type entirely -- those come out at
// their documented defaults (0 / THERMO_TC_K) -- but the per-zone fields v1
// DOES carry (name, gains, thresholds, model params) must be the real staged
// values, not zeros or a wrong-offset read.
static void test_nvs_load_from_v1_blob_upconverts_fields_correctly(void)
{
    TEST_SECTION("nvs_load_from -- a v1 blob (predates continue_on_zone_trip/safety_tc_type) "
                 "upconverts every zone's real field values correctly");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v1_t src;
    memset(&src, 0, sizeof(src));
    src.version = 1;
    src.thermo_count = 2;
    src.relay_count = 2;
    src.max_simultaneous_relays = 2;

    snprintf(src.zones[0].name, sizeof(src.zones[0].name), "ZoneA");
    src.zones[0].relay_mask = 0x01;
    src.zones[0].cal_offset_c = 1.5f;
    src.zones[0].pid_kp = 3.5f;
    src.zones[0].pid_ki = 0.2f;
    src.zones[0].pid_kd = 0.1f;
    src.zones[0].max_temp_c = 1150.0f;
    src.zones[0].min_temp_c = -10.0f;
    src.zones[0].model_k_dc = 7.0f;
    src.zones[0].model_tau_s = 300.0f;
    src.zones[0].model_dead_time_s = 12.0f;

    snprintf(src.zones[1].name, sizeof(src.zones[1].name), "ZoneB");
    src.zones[1].relay_mask = 0x02;
    src.zones[1].cal_offset_c = 2.5f;
    src.zones[1].pid_kp = 4.5f;
    src.zones[1].max_temp_c = 1250.0f;

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v1 blob must migrate to a valid current config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped with the current version");
    TEST_CHECK(out_cfg.continue_on_zone_trip == 0, "v1 predates continue_on_zone_trip -- documented default 0");
    TEST_CHECK(out_cfg.safety_tc_type == THERMO_TC_K, "v1 predates safety_tc_type -- documented default THERMO_TC_K");

    TEST_CHECK(strcmp(out_cfg.zones[0].name, "ZoneA") == 0, "zones[0].name (real v1 value) carried through");
    TEST_CHECK(out_cfg.zones[0].relay_mask == 0x01, "zones[0].relay_mask carried through");
    TEST_CHECK_NEAR(out_cfg.zones[0].cal_offset_c, 1.5f, 1e-6, "zones[0].cal_offset_c carried through");
    TEST_CHECK_NEAR(out_cfg.zones[0].pid_kp, 3.5f, 1e-6, "zones[0].pid_kp carried through");
    TEST_CHECK_NEAR(out_cfg.zones[0].pid_ki, 0.2f, 1e-6, "zones[0].pid_ki carried through");
    TEST_CHECK_NEAR(out_cfg.zones[0].max_temp_c, 1150.0f, 1e-6, "zones[0].max_temp_c carried through");
    TEST_CHECK_NEAR(out_cfg.zones[0].min_temp_c, -10.0f, 1e-6, "zones[0].min_temp_c carried through");
    TEST_CHECK_NEAR(out_cfg.zones[0].model_k_dc, 7.0f, 1e-6, "zones[0].model_k_dc carried through");
    TEST_CHECK_NEAR(out_cfg.zones[0].model_tau_s, 300.0f, 1e-6, "zones[0].model_tau_s carried through");
    TEST_CHECK_NEAR(out_cfg.zones[0].model_dead_time_s, 12.0f, 1e-6, "zones[0].model_dead_time_s carried through");
    TEST_CHECK(out_cfg.zones[0].tc_type == THERMO_TC_K, "zones[0].tc_type defaults to THERMO_TC_K (v1 predates it)");
    TEST_CHECK(out_cfg.zones[0].thermo_mask == 0x01, "zones[0].thermo_mask defaults to legacy 1<<0 mapping");

    TEST_CHECK(strcmp(out_cfg.zones[1].name, "ZoneB") == 0, "zones[1].name must NOT be shifted");
    TEST_CHECK(out_cfg.zones[1].relay_mask == 0x02, "zones[1].relay_mask must NOT be shifted");
    TEST_CHECK_NEAR(out_cfg.zones[1].cal_offset_c, 2.5f, 1e-6, "zones[1].cal_offset_c must be the real value");
    TEST_CHECK_NEAR(out_cfg.zones[1].pid_kp, 4.5f, 1e-6, "zones[1].pid_kp must be the real value");
    TEST_CHECK_NEAR(out_cfg.zones[1].max_temp_c, 1250.0f, 1e-6, "zones[1].max_temp_c must be the real value");
    TEST_CHECK(out_cfg.zones[1].thermo_mask == 0x02, "zones[1].thermo_mask defaults to legacy 1<<1 mapping");

    nvs_test_enable(false);
    nvs_test_clear();
}

// Dedicated v2 blob test -- v2 adds continue_on_zone_trip over v1 but shares
// v1's zone layout (zone_cfg_v1_t) and converter (convert_zone_v1()).
static void test_nvs_load_from_v2_blob_upconverts_fields_correctly(void)
{
    TEST_SECTION("nvs_load_from -- a v2 blob (adds continue_on_zone_trip over v1) upconverts "
                 "every zone's real field values correctly");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v2_t src;
    memset(&src, 0, sizeof(src));
    src.version = 2;
    src.thermo_count = 2;
    src.relay_count = 2;
    src.continue_on_zone_trip = 1;

    snprintf(src.zones[0].name, sizeof(src.zones[0].name), "ZoneA");
    src.zones[0].relay_mask = 0x01;
    src.zones[0].pid_kp = 3.5f;
    src.zones[0].max_temp_c = 1150.0f;

    snprintf(src.zones[1].name, sizeof(src.zones[1].name), "ZoneB");
    src.zones[1].relay_mask = 0x02;
    src.zones[1].pid_kp = 4.5f;
    src.zones[1].max_temp_c = 1250.0f;
    src.zones[1].model_tau_s = 250.0f;

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v2 blob must migrate to a valid current config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped with the current version");
    TEST_CHECK(out_cfg.continue_on_zone_trip == 1, "v2's real continue_on_zone_trip value carried through (unlike v1's forced 0)");
    TEST_CHECK(out_cfg.safety_tc_type == THERMO_TC_K, "v2 predates safety_tc_type -- documented default THERMO_TC_K");

    TEST_CHECK(strcmp(out_cfg.zones[1].name, "ZoneB") == 0, "zones[1].name must NOT be shifted");
    TEST_CHECK(out_cfg.zones[1].relay_mask == 0x02, "zones[1].relay_mask must NOT be shifted");
    TEST_CHECK_NEAR(out_cfg.zones[1].pid_kp, 4.5f, 1e-6, "zones[1].pid_kp must be the real value");
    TEST_CHECK_NEAR(out_cfg.zones[1].max_temp_c, 1250.0f, 1e-6, "zones[1].max_temp_c must be the real value");
    TEST_CHECK_NEAR(out_cfg.zones[1].model_tau_s, 250.0f, 1e-6, "zones[1].model_tau_s must be the real value");

    nvs_test_enable(false);
    nvs_test_clear();
}

// Dedicated v3 blob test -- v3 adds the 8 guard thresholds over v1's zone
// layout; still predates thermo_mask/tc_type/ct_mask.
static void test_nvs_load_from_v3_blob_upconverts_guard_thresholds_correctly(void)
{
    TEST_SECTION("nvs_load_from -- a v3 blob (adds the 8 guard thresholds) upconverts every "
                 "zone's real field values, guard thresholds included, correctly");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v3_t src;
    memset(&src, 0, sizeof(src));
    src.version = 3;
    src.thermo_count = 2;
    src.relay_count = 2;
    src.continue_on_zone_trip = 1;

    src.zones[0].relay_mask = 0x01;
    src.zones[0].max_temp_c = 1150.0f;
    src.zones[0].guard_wrong_dir_window_s = 45.0f;
    src.zones[0].guard_runaway_margin_c = 15.0f;

    snprintf(src.zones[1].name, sizeof(src.zones[1].name), "ZoneB");
    src.zones[1].relay_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;
    src.zones[1].guard_wrong_dir_window_s = 55.0f;
    src.zones[1].guard_wrong_dir_rate_c_per_min = 5.0f;
    src.zones[1].guard_off_settle_s = 20.0f;
    src.zones[1].guard_runaway_rate_c_per_min = 8.0f;
    src.zones[1].guard_runaway_margin_c = 25.0f;
    src.zones[1].guard_drift_period_s = 60.0f;
    src.zones[1].guard_sensor_fault_debounce_ticks = 3.0f;
    src.zones[1].guard_frozen_window_s = 90.0f;

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v3 blob must migrate to a valid current config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped with the current version");

    TEST_CHECK(out_cfg.zones[1].relay_mask == 0x02, "zones[1].relay_mask must NOT be shifted");
    TEST_CHECK_NEAR(out_cfg.zones[1].guard_wrong_dir_window_s, 55.0f, 1e-6, "zones[1].guard_wrong_dir_window_s -- new in v3 -- carried through");
    TEST_CHECK_NEAR(out_cfg.zones[1].guard_wrong_dir_rate_c_per_min, 5.0f, 1e-6, "zones[1].guard_wrong_dir_rate_c_per_min carried through");
    TEST_CHECK_NEAR(out_cfg.zones[1].guard_off_settle_s, 20.0f, 1e-6, "zones[1].guard_off_settle_s carried through");
    TEST_CHECK_NEAR(out_cfg.zones[1].guard_runaway_rate_c_per_min, 8.0f, 1e-6, "zones[1].guard_runaway_rate_c_per_min carried through");
    TEST_CHECK_NEAR(out_cfg.zones[1].guard_runaway_margin_c, 25.0f, 1e-6, "zones[1].guard_runaway_margin_c carried through");
    TEST_CHECK_NEAR(out_cfg.zones[1].guard_drift_period_s, 60.0f, 1e-6, "zones[1].guard_drift_period_s carried through");
    TEST_CHECK_NEAR(out_cfg.zones[1].guard_sensor_fault_debounce_ticks, 3.0f, 1e-6, "zones[1].guard_sensor_fault_debounce_ticks carried through");
    TEST_CHECK_NEAR(out_cfg.zones[1].guard_frozen_window_s, 90.0f, 1e-6, "zones[1].guard_frozen_window_s carried through");

    TEST_CHECK(out_cfg.zones[0].relay_mask == 0x01, "zones[0].relay_mask still correct (sanity check)");
    TEST_CHECK_NEAR(out_cfg.zones[0].guard_wrong_dir_window_s, 45.0f, 1e-6, "zones[0].guard_wrong_dir_window_s still correct (sanity check)");

    nvs_test_enable(false);
    nvs_test_clear();
}

// Dedicated v4 blob test -- v4 appends thermo_mask (a REAL, operator-set
// value now, not the legacy 1<<chan_idx default v1/v3 fall back to) at the
// tail of v3's zone layout; still predates tc_type.
static void test_nvs_load_from_v4_blob_upconverts_thermo_mask_correctly(void)
{
    TEST_SECTION("nvs_load_from -- a v4 blob (adds real thermo_mask over v3) upconverts every "
                 "zone's real field values, including the operator-set thermo_mask, correctly");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v4_t src;
    memset(&src, 0, sizeof(src));
    src.version = 4;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.continue_on_zone_trip = 1;

    src.zones[0].relay_mask = 0x01;
    src.zones[0].max_temp_c = 1150.0f;
    src.zones[0].thermo_mask = 0x04; // deliberately NOT 1<<0, to distinguish real value from the legacy default

    snprintf(src.zones[1].name, sizeof(src.zones[1].name), "ZoneB");
    src.zones[1].relay_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;
    src.zones[1].guard_runaway_margin_c = 25.0f;
    src.zones[1].thermo_mask = 0x01; // deliberately NOT 1<<1

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v4 blob must migrate to a valid current config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped with the current version");

    TEST_CHECK(out_cfg.zones[0].thermo_mask == 0x04,
              "zones[0].thermo_mask is the real v4 operator-set value, not the legacy 1<<0 default");
    TEST_CHECK(out_cfg.zones[1].relay_mask == 0x02, "zones[1].relay_mask must NOT be shifted");
    TEST_CHECK(out_cfg.zones[1].thermo_mask == 0x01,
              "zones[1].thermo_mask is the real v4 operator-set value, not the legacy 1<<1 default");
    TEST_CHECK_NEAR(out_cfg.zones[1].max_temp_c, 1250.0f, 1e-6, "zones[1].max_temp_c carried through");
    TEST_CHECK_NEAR(out_cfg.zones[1].guard_runaway_margin_c, 25.0f, 1e-6, "zones[1].guard_runaway_margin_c carried through");
    TEST_CHECK(out_cfg.zones[1].tc_type == THERMO_TC_K, "zones[1].tc_type defaults to THERMO_TC_K (v4 predates it)");

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

/* A config that zones_config_json_validate() accepts, so a test can change exactly
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
     * resolve to a REAL profile for zones_config_json_validate() to accept this --
     * see zones_cfg_t::timing_profile_count's own comment. */
    cfg->timing_profile_count = 1;
    strncpy(cfg->timing_profiles[0].name, "Default", TIMING_PROFILE_NAME_MAX_LEN);
    /* ZONES_CFG_VERSION 16->17: zones[*].ease_off_window_mult's 0 (the
     * memset default above) IS the legal "use the firmware default"
     * sentinel -- no explicit per-zone assignment needed here, unlike a
     * field whose 0 is illegal. */
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
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason), "guard_progress_duty_min > 1.0 is rejected");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.timing_profiles[0].guard_progress_window_s = -1.0f;
        const char *reason = NULL;
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason), "a negative guard_progress_window_s is rejected");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.timing_profiles[0].ramp_lock_band_c = ZONE_GUARD_MARGIN_C_MAX + 1.0f;
        const char *reason = NULL;
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason), "ramp_lock_band_c past its ceiling is rejected");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.pc_link_abort_silence_ms = ZONE_PC_LINK_SILENCE_MS_MAX + 1.0f;
        const char *reason = NULL;
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason), "pc_link_abort_silence_ms past its ceiling is rejected");
    }
    /* ZONES_CFG_VERSION 15->16: ease_off_window_mult's range check --
     * NEGATIVE tests on both ceiling and floor (a zero-or-negative window
     * multiplier would disable or invert the taper -- see this field's own
     * doc comment), plus the two sentinel/boundary values that MUST stay
     * legal so the check does not accidentally reject real, in-range A/B
     * values or the "reset to default" sentinel. */
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].ease_off_window_mult = ZONE_EASE_OFF_WINDOW_MULT_MAX + 1.0f;
        const char *reason = NULL;
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason), "ease_off_window_mult past its ceiling is rejected");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        /* Between 0 (the legal sentinel) and MIN (the legal floor) -- a real,
         * nonzero-but-too-small window, not the "use the default" sentinel.
         * Proves the check is a genuine [MIN,MAX]-or-0 union, not just
         * `value <= MAX` with the floor forgotten. */
        cfg.zones[0].ease_off_window_mult = ZONE_EASE_OFF_WINDOW_MULT_MIN / 2.0f;
        const char *reason = NULL;
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason),
                  "a nonzero ease_off_window_mult below its floor is rejected (not the 0 sentinel)");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].ease_off_window_mult = -1.0f;
        const char *reason = NULL;
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason), "a negative ease_off_window_mult is rejected");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].ease_off_window_mult = NAN;
        const char *reason = NULL;
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason), "a NaN ease_off_window_mult is rejected");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].ease_off_window_mult = 0.0f; /* the "use the firmware default" sentinel */
        const char *reason = NULL;
        TEST_CHECK(zones_config_json_validate(&cfg, &reason), "0 stays legal for ease_off_window_mult (the default sentinel)");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].ease_off_window_mult = ZONE_EASE_OFF_WINDOW_MULT_MAX; /* the ceiling itself -- inclusive */
        const char *reason = NULL;
        TEST_CHECK(zones_config_json_validate(&cfg, &reason), "ease_off_window_mult exactly at its ceiling is accepted");
    }
    /* 0 must stay legal on every one of them -- it is the "use the firmware
     * default" value, not a missing setting. */
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        const char *reason = NULL;
        TEST_CHECK(zones_config_json_validate(&cfg, &reason), "all-zero timing profile fields stay valid (0 = firmware default)");
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
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason), "timing_profile_count == 0 is rejected");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.timing_profile_count = MAX31856_CHANNEL_COUNT + 1;
        const char *reason = NULL;
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason), "timing_profile_count past MAX31856_CHANNEL_COUNT is rejected");
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
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason),
                  "a zone's timing_profile referencing a profile past timing_profile_count is rejected");
    }
}

// ---------------------------------------------------------------------------
// PID_EXPANSION_PLAN.md Phase 2/4 (2026-08-30): ZONE_CONTROL_MODE_PID_FUZZY,
// fuzzy_strength_pct/coupling_coeff/coupling_neighbor_zone/settings_source,
// and the ZONES_CFG_VERSION 9->10 migration. Every test below was run once
// against a deliberately-broken version of the change it covers (see the
// task report) to confirm it can actually fail, per this file's own
// "negative-test every check" convention.
// ---------------------------------------------------------------------------

// THE migration test this bump is about: a v9 blob (predating all four new
// fields) must upgrade losslessly -- every pre-existing field survives
// unchanged, the three new floats read as 0 (their documented "not
// configured" default), and settings_source lands at ZONE_SETTINGS_SOURCE_CUSTOM
// (0xFF) for EVERY zone, never 0 -- 0 would silently mean "copies zone 0's
// settings" and would be overwritten by zone 0's numbers on the next save
// once Phase 5's resolve-on-save logic lands (a later pass). This test would
// FAIL if convert_zone_v9() ever left settings_source at its zero-initialized
// default instead of explicitly setting it.
static void test_nvs_load_from_v9_blob_upconverts_new_fields_default_and_settings_source_is_custom(void)
{
    TEST_SECTION("nvs_load_from -- a v9 blob upconverts to v10: new float fields default to 0, "
                 "settings_source lands at CUSTOM (0xFF) for every zone (NOT 0), and every "
                 "pre-existing field survives unchanged");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v9_t src;
    memset(&src, 0, sizeof(src));
    src.version = 9;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.continue_on_zone_trip = 1;
    src.safety_tc_type = 3;
    src.pc_link_abort_silence_ms = 45000.0f;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");

    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].tc_type = 3;
    src.zones[0].control_mode = 2; /* PID -- a real, distinct, pre-existing operator setting */
    src.zones[0].pid_kp = 2.5f;
    src.zones[0].max_temp_c = 1300.0f;

    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;

    src.zones[2].relay_mask = 0x04;
    src.zones[2].thermo_mask = 0x04;
    src.zones[2].max_temp_c = 1200.0f;

    src.crc32 = 0; /* v9's own CRC is not checked on the old-version path */

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v9 blob must migrate to a valid current (v10) config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");

    for (uint8_t i = 0; i < 3; i++) {
        char msg[128];
        snprintf(msg, sizeof(msg), "zones[%u].fuzzy_strength_pct defaults to 0 (not configured)", i);
        TEST_CHECK_NEAR(out_cfg.zones[i].fuzzy_strength_pct, 0.0f, 1e-6, msg);
        for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
            snprintf(msg, sizeof(msg), "zones[%u].coupling_coeff[%u] defaults to 0 (no coupling measured)", i, j);
            TEST_CHECK_NEAR(out_cfg.zones[i].coupling_coeff[j], 0.0f, 1e-6, msg);
        }
        snprintf(msg, sizeof(msg),
                "zones[%u].settings_source lands at ZONE_SETTINGS_SOURCE_CUSTOM (0xFF), NOT 0 -- "
                "0 would silently claim \"copies zone 0's settings\"", i);
        TEST_CHECK(out_cfg.zones[i].settings_source[SRC_GROUP_LIMITS] == ZONE_SETTINGS_SOURCE_CUSTOM, msg);
    }

    // Pre-existing fields must survive the upgrade completely unchanged.
    TEST_CHECK(out_cfg.thermo_count == 3 && out_cfg.relay_count == 3, "counts carried through");
    TEST_CHECK(out_cfg.continue_on_zone_trip == 1, "continue_on_zone_trip carried through");
    TEST_CHECK(out_cfg.safety_tc_type == 3, "safety_tc_type carried through");
    TEST_CHECK_NEAR(out_cfg.pc_link_abort_silence_ms, 45000.0f, 1e-6, "pc_link_abort_silence_ms carried through");
    TEST_CHECK(out_cfg.timing_profile_count == 1, "timing_profile_count carried through");
    TEST_CHECK(strcmp(out_cfg.timing_profiles[0].name, "Default") == 0, "timing profile name carried through");

    TEST_CHECK(out_cfg.zones[0].relay_mask == 0x01 && out_cfg.zones[1].relay_mask == 0x02 &&
              out_cfg.zones[2].relay_mask == 0x04, "relay_mask must NOT be shifted for any zone");
    TEST_CHECK(out_cfg.zones[0].thermo_mask == 0x01 && out_cfg.zones[1].thermo_mask == 0x02 &&
              out_cfg.zones[2].thermo_mask == 0x04, "thermo_mask must NOT be shifted for any zone");
    TEST_CHECK(out_cfg.zones[0].control_mode == 2, "zones[0].control_mode (PID) survives the upgrade");
    TEST_CHECK_NEAR(out_cfg.zones[0].pid_kp, 2.5f, 1e-6, "zones[0].pid_kp survives the upgrade");
    TEST_CHECK_NEAR(out_cfg.zones[0].max_temp_c, 1300.0f, 1e-6, "zones[0].max_temp_c survives");
    TEST_CHECK_NEAR(out_cfg.zones[1].max_temp_c, 1250.0f, 1e-6, "zones[1].max_temp_c survives");
    TEST_CHECK_NEAR(out_cfg.zones[2].max_temp_c, 1200.0f, 1e-6, "zones[2].max_temp_c survives");

    nvs_test_enable(false);
    nvs_test_clear();
}

// ---------------------------------------------------------------------------
// Same-day follow-up (2026-08-30): ZONES_CFG_VERSION 10->11 -- the single
// coupling_coeff/coupling_neighbor_zone pair is replaced by a full directed
// coupling_coeff[] row (bench-measured coupling is asymmetric AND
// multi-neighbor; see zone_cfg_t's own doc comment). Every test below was
// run once against a deliberately-broken version of the change it covers to
// confirm it can actually fail, per this file's own "negative-test every
// check" convention (see the task report for the specific break/restore
// pairs).
// ---------------------------------------------------------------------------

// THE migration test this bump is about: a v10 blob's single
// (coupling_coeff, coupling_neighbor_zone) pair must land in exactly the
// right cell of the new row, every other cell (including the diagonal) 0,
// and every other pre-existing v10 field survives unchanged. sizeof(src) is
// zone_cfg_v10_t/zones_cfg_v10_t -- both frozen, historical types with their
// own _Static_assert(sizeof(...) == N) in zones_http.c checked against a
// HAND-COMPUTED byte count, never the live zone_cfg_t/zones_cfg_t (which by
// now is already the v11 row-coupling shape) -- so staging with sizeof(src)
// here stages a byte count independently verified to match the real v10
// on-flash layout, not a number that happens to match today's struct.
static void test_nvs_load_from_v10_blob_folds_single_pair_into_row_cell(void)
{
    TEST_SECTION("nvs_load_from -- a v10 blob upconverts to v11: coupling_coeff/coupling_neighbor_zone "
                 "fold into the right coupling_coeff[] cell, every other cell (incl. diagonal) is 0, "
                 "and every other pre-existing v10 field survives unchanged");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v10_t src;
    memset(&src, 0, sizeof(src));
    src.version = 10;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.continue_on_zone_trip = 1;
    src.safety_tc_type = 3;
    src.pc_link_abort_silence_ms = 45000.0f;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");

    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].max_temp_c = 1300.0f;
    src.zones[0].fuzzy_strength_pct = 40.0f;
    src.zones[0].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    /* zone 0's ONE storable neighbor before this pass: zone 2, coeff 6.75. */
    src.zones[0].coupling_coeff = 6.75f;
    src.zones[0].coupling_neighbor_zone = 2.0f;

    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;
    src.zones[1].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    /* zone 1: no coupling ever measured on a v10 board -- coupling_coeff
     * stays at its 0 default, same "not configured" meaning either side of
     * the migration. */

    src.zones[2].relay_mask = 0x04;
    src.zones[2].thermo_mask = 0x04;
    src.zones[2].max_temp_c = 1200.0f;
    src.zones[2].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.crc32 = 0; /* v10's own CRC is not checked on the old-version path */

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v10 blob must migrate to a valid current (v11) config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");

    // The one cell that must be populated: zone 0's row, column 2.
    TEST_CHECK_NEAR(out_cfg.zones[0].coupling_coeff[2], 6.75f, 1e-6,
                    "zone 0's v10 (coeff, neighbor=2) pair lands in coupling_coeff[2]");
    // Every other cell of zone 0's row, including its own diagonal, is 0.
    TEST_CHECK_NEAR(out_cfg.zones[0].coupling_coeff[0], 0.0f, 1e-6, "zone 0's diagonal cell is 0");
    TEST_CHECK_NEAR(out_cfg.zones[0].coupling_coeff[1], 0.0f, 1e-6, "zone 0's untouched neighbor cell (1) is 0");
    // Zones 1 and 2 never had a v10 coupling pair -- their whole rows are 0.
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "zone 1's untouched coupling_coeff[%u] is 0", j);
        TEST_CHECK_NEAR(out_cfg.zones[1].coupling_coeff[j], 0.0f, 1e-6, msg);
        snprintf(msg, sizeof(msg), "zone 2's untouched coupling_coeff[%u] is 0", j);
        TEST_CHECK_NEAR(out_cfg.zones[2].coupling_coeff[j], 0.0f, 1e-6, msg);
    }

    // Pre-existing v10 fields must survive the upgrade completely unchanged.
    TEST_CHECK(out_cfg.thermo_count == 3 && out_cfg.relay_count == 3, "counts carried through");
    TEST_CHECK(out_cfg.continue_on_zone_trip == 1, "continue_on_zone_trip carried through");
    TEST_CHECK_NEAR(out_cfg.pc_link_abort_silence_ms, 45000.0f, 1e-6, "pc_link_abort_silence_ms carried through");
    TEST_CHECK(strcmp(out_cfg.timing_profiles[0].name, "Default") == 0, "timing profile name carried through");
    TEST_CHECK(out_cfg.zones[0].relay_mask == 0x01 && out_cfg.zones[1].relay_mask == 0x02 &&
              out_cfg.zones[2].relay_mask == 0x04, "relay_mask must NOT be shifted for any zone");
    TEST_CHECK_NEAR(out_cfg.zones[0].fuzzy_strength_pct, 40.0f, 1e-6, "zones[0].fuzzy_strength_pct survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].max_temp_c, 1300.0f, 1e-6, "zones[0].max_temp_c survives");
    TEST_CHECK_NEAR(out_cfg.zones[1].max_temp_c, 1250.0f, 1e-6, "zones[1].max_temp_c survives");
    TEST_CHECK_NEAR(out_cfg.zones[2].max_temp_c, 1200.0f, 1e-6, "zones[2].max_temp_c survives");
    TEST_CHECK(out_cfg.zones[0].settings_source[SRC_GROUP_LIMITS] == ZONE_SETTINGS_SOURCE_CUSTOM,
              "zones[0].settings_source (already a real v10 field) is carried through verbatim, "
              "not re-defaulted the way v9's migration must");

    nvs_test_enable(false);
    nvs_test_clear();
}

// 2026-08-31 defect fix: a v10 blob whose single coupling pair is
// self-referencing (coupling_neighbor_zone == the zone's OWN index) must
// have that pair dropped, not folded into the diagonal cell. Before this
// fix convert_zone_v10() had no chan_idx parameter to compare against (unlike
// convert_zone_v1(), which has always taken one and skipped exactly this
// case), so a self-referencing pair wrote a nonzero diagonal cell;
// zones_config_json_validate() rejects any nonzero diagonal, and zones_config_json_decode_blob()
// then returns CORRUPT for the WHOLE migrated struct -- discarding the
// entire commissioned config over one stray self-reference. Unreachable
// today (no board has ever held a v10 blob with a self-referencing pair),
// but cheap to guard and this proves the guard actually does something.
static void test_nvs_load_from_v10_blob_self_referencing_pair_is_dropped(void)
{
    TEST_SECTION("nvs_load_from -- a v10 blob with a self-referencing coupling pair "
                 "(neighbor == own zone index) is dropped, not folded into the diagonal, "
                 "so the migrated config is still VALID rather than CORRUPT");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v10_t src;
    memset(&src, 0, sizeof(src));
    src.version = 10;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.safety_tc_type = 3;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");

    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].max_temp_c = 1300.0f;
    src.zones[0].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;
    src.zones[1].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    /* the defect: zone 1's stored pair points at itself. */
    src.zones[1].coupling_coeff = 9.5f;
    src.zones[1].coupling_neighbor_zone = 1.0f;

    src.zones[2].relay_mask = 0x04;
    src.zones[2].thermo_mask = 0x04;
    src.zones[2].max_temp_c = 1200.0f;
    src.zones[2].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.crc32 = 0;

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid,
              "a v10 blob with a self-referencing coupling pair must still migrate to a VALID "
              "config, not be discarded as CORRUPT");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "zone 1's self-referencing pair leaves coupling_coeff[%u] at 0", j);
        TEST_CHECK_NEAR(out_cfg.zones[1].coupling_coeff[j], 0.0f, 1e-6, msg);
    }

    nvs_test_enable(false);
    nvs_test_clear();
}

// ZONES_CFG_VERSION 11->12 (DATA PLUMBING pass): a v11 blob (coupling_coeff[]
// row shape, but predating coupling_tau_s[]/coupling_dead_time_s[] entirely)
// must migrate cleanly to the current (v12) config with both new arrays at
// 0 -- "not measured", not garbage -- while coupling_coeff[] itself and every
// other pre-existing v11 field survive unchanged. sizeof(src) is
// zone_cfg_v11_t/zones_cfg_v11_t -- both frozen, historical types with their
// own _Static_assert(sizeof(...) == N) checked against a hand-computed byte
// count, never the live zone_cfg_t/zones_cfg_t (already the v12 shape by
// now), mirroring test_nvs_load_from_v10_blob_folds_single_pair_into_row_cell()
// above exactly.
static void test_nvs_load_from_v11_blob_defaults_coupling_tau_dead_time_to_zero(void)
{
    TEST_SECTION("nvs_load_from -- a v11 blob upconverts to v12: coupling_tau_s[]/"
                 "coupling_dead_time_s[] default to 0 (never stored pre-v12), coupling_coeff[] "
                 "and every other pre-existing v11 field survive unchanged");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v11_t src;
    memset(&src, 0, sizeof(src));
    src.version = 11;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.continue_on_zone_trip = 1;
    src.safety_tc_type = 3;
    src.pc_link_abort_silence_ms = 45000.0f;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");

    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].max_temp_c = 1300.0f;
    src.zones[0].fuzzy_strength_pct = 40.0f;
    src.zones[0].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    // A v11 board that HAD already measured a coupling gain (the row this
    // pass's own bump exists to grow with tau/L) -- coupling_coeff[2] must
    // survive the migration untouched while coupling_tau_s[2]/
    // coupling_dead_time_s[2] (fields v11 never had at all) come out at 0.
    src.zones[0].coupling_coeff[2] = 6.75f;

    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;
    src.zones[1].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.zones[2].relay_mask = 0x04;
    src.zones[2].thermo_mask = 0x04;
    src.zones[2].max_temp_c = 1200.0f;
    src.zones[2].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.crc32 = 0; // v11's own CRC is not checked on the old-version path

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v11 blob must migrate to a valid current (v12) config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");

    // coupling_coeff[] carried through verbatim -- this migration does not
    // touch the row's existing shape at all.
    TEST_CHECK_NEAR(out_cfg.zones[0].coupling_coeff[2], 6.75f, 1e-6,
                    "zone 0's pre-existing coupling_coeff[2] survives the v11->v12 migration untouched");

    // THE thing this test is really about: coupling_tau_s[]/
    // coupling_dead_time_s[] must be all-zero for every zone and every cell
    // -- not garbage, not uninitialized memory -- since no version before v12
    // ever stored either array.
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
            char msg[96];
            snprintf(msg, sizeof(msg), "zones[%u].coupling_tau_s[%u] is 0 (v11 never stored this)", i, j);
            TEST_CHECK_NEAR(out_cfg.zones[i].coupling_tau_s[j], 0.0f, 1e-6, msg);
            snprintf(msg, sizeof(msg), "zones[%u].coupling_dead_time_s[%u] is 0 (v11 never stored this)", i, j);
            TEST_CHECK_NEAR(out_cfg.zones[i].coupling_dead_time_s[j], 0.0f, 1e-6, msg);
        }
    }

    // Pre-existing v11 fields must survive the upgrade completely unchanged.
    TEST_CHECK(out_cfg.thermo_count == 3 && out_cfg.relay_count == 3, "counts carried through");
    TEST_CHECK(out_cfg.continue_on_zone_trip == 1, "continue_on_zone_trip carried through");
    TEST_CHECK_NEAR(out_cfg.pc_link_abort_silence_ms, 45000.0f, 1e-6, "pc_link_abort_silence_ms carried through");
    TEST_CHECK(strcmp(out_cfg.timing_profiles[0].name, "Default") == 0, "timing profile name carried through");
    TEST_CHECK(out_cfg.zones[0].relay_mask == 0x01 && out_cfg.zones[1].relay_mask == 0x02 &&
              out_cfg.zones[2].relay_mask == 0x04, "relay_mask must NOT be shifted for any zone");
    TEST_CHECK_NEAR(out_cfg.zones[0].fuzzy_strength_pct, 40.0f, 1e-6, "zones[0].fuzzy_strength_pct survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].max_temp_c, 1300.0f, 1e-6, "zones[0].max_temp_c survives");
    TEST_CHECK_NEAR(out_cfg.zones[1].max_temp_c, 1250.0f, 1e-6, "zones[1].max_temp_c survives");
    TEST_CHECK_NEAR(out_cfg.zones[2].max_temp_c, 1200.0f, 1e-6, "zones[2].max_temp_c survives");
    for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
        char ss_msg[96];
        snprintf(ss_msg, sizeof(ss_msg), "zones[0].settings_source group %u carried through verbatim (Opus "
                "review of 5672719, item 1: the prefix+tail helper's fan-out must reach every group, not "
                "just LIMITS)", (unsigned)g);
        TEST_CHECK(out_cfg.zones[0].settings_source[g] == ZONE_SETTINGS_SOURCE_CUSTOM, ss_msg);
    }

    nvs_test_enable(false);
    nvs_test_clear();
}

// Full end-to-end chain: a genuine v9 blob (predates coupling entirely) must
// still land on a valid, all-zero-coupling v11 config after going through
// BOTH migration steps back-to-back (v9->v10->v11 inside one
// convert_versioned_blob_to_current(9, ...) call, since case 9 lands
// directly on CURRENT format rather than stopping at v10). Exists
// separately from the two single-step tests above because a chain bug
// (e.g. an intermediate value never making it into the final struct) is
// exactly the kind of thing that would NOT show up testing each hop in
// isolation.
// Dedicated v6 blob test -- v6 shares v7's zone layout (zone_cfg_v7_t) and
// converter (convert_zone_v7()) but predates v7's own crc32 field. Only v7
// had a dedicated blob test before this; v6 was only reached indirectly.
static void test_nvs_load_from_v6_blob_upconverts_fields_correctly(void)
{
    TEST_SECTION("nvs_load_from -- a v6 blob (v7's zone layout, no crc32 yet) upconverts every "
                 "zone's real field values, including ct_mask, correctly");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v6_t src;
    memset(&src, 0, sizeof(src));
    src.version = 6;
    src.thermo_count = 2;
    src.relay_count = 2;
    src.continue_on_zone_trip = 1;
    src.safety_tc_type = 4;

    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].tc_type = 2;
    src.zones[0].max_temp_c = 1150.0f;
    src.zones[0].ct_mask = 0x01;

    snprintf(src.zones[1].name, sizeof(src.zones[1].name), "ZoneB");
    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].tc_type = 3;
    src.zones[1].max_temp_c = 1250.0f;
    src.zones[1].guard_runaway_margin_c = 25.0f;
    src.zones[1].ct_mask = 0x02;

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v6 blob must migrate to a valid current config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped with the current version");
    TEST_CHECK(out_cfg.safety_tc_type == 4, "top-level safety_tc_type (real v6 value) carried through");

    TEST_CHECK(out_cfg.zones[1].relay_mask == 0x02, "zones[1].relay_mask must NOT be shifted");
    TEST_CHECK(out_cfg.zones[1].thermo_mask == 0x02, "zones[1].thermo_mask must NOT be shifted");
    TEST_CHECK(out_cfg.zones[1].tc_type == 3, "zones[1].tc_type carried through");
    TEST_CHECK_NEAR(out_cfg.zones[1].max_temp_c, 1250.0f, 1e-6, "zones[1].max_temp_c carried through");
    TEST_CHECK_NEAR(out_cfg.zones[1].guard_runaway_margin_c, 25.0f, 1e-6, "zones[1].guard_runaway_margin_c carried through");
    TEST_CHECK(out_cfg.zones[1].ct_mask == 0x02, "zones[1].ct_mask -- new since v4 -- carried through, not shifted");

    TEST_CHECK(out_cfg.zones[0].ct_mask == 0x01, "zones[0].ct_mask still correct (sanity check)");
    TEST_CHECK_NEAR(out_cfg.pc_link_abort_silence_ms, 0.0f, 1e-6, "v6 predates pc_link_abort_silence_ms -- documented default 0");

    nvs_test_enable(false);
    nvs_test_clear();
}

// End-to-end composition test: the OLDEST supported version (v1) all the way
// to CURRENT (v15), asserting real field values -- not just defaults --
// survive every hop. "Composes by inspection" was the audit's own phrase for
// this gap; this test replaces that inspection with an executed assertion.
// A test that only checked new-field defaults (coupling_diag_k_dc == 0, etc.)
// would pass even if every pre-existing field were silently dropped along the
// way -- so this asserts real, non-default, non-zero values for fields that
// have existed since v1 (name, pid_kp, max_temp_c, model_tau_s) all still
// read back correctly out of the fully-migrated v15 struct.
static void test_nvs_load_from_v1_blob_chains_end_to_end_to_v16_preserving_real_values(void)
{
    TEST_SECTION("nvs_load_from -- a v1 blob migrates end-to-end to the current (v16) config: "
                 "real v1 field values (not just new-field defaults) survive every hop");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v1_t src;
    memset(&src, 0, sizeof(src));
    src.version = 1;
    src.thermo_count = 2;
    src.relay_count = 2;
    src.max_simultaneous_relays = 2;

    snprintf(src.zones[0].name, sizeof(src.zones[0].name), "OldestZone");
    src.zones[0].relay_mask = 0x01;
    src.zones[0].cal_offset_c = 1.5f;
    src.zones[0].pid_kp = 3.5f;
    src.zones[0].pid_ki = 0.2f;
    src.zones[0].pid_kd = 0.1f;
    src.zones[0].max_temp_c = 1150.0f;
    src.zones[0].min_temp_c = -10.0f;
    src.zones[0].model_k_dc = 7.0f;
    src.zones[0].model_tau_s = 300.0f;
    src.zones[0].model_dead_time_s = 12.0f;

    src.zones[1].relay_mask = 0x02;
    src.zones[1].pid_kp = 4.5f;
    src.zones[1].max_temp_c = 1250.0f;

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a v1 blob must migrate end-to-end to a valid current (v16) config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current (v16) version");

    // Real values that existed at v1 and must have survived every hop to v15.
    TEST_CHECK(strcmp(out_cfg.zones[0].name, "OldestZone") == 0,
              "zones[0].name -- present since v1 -- survives all the way to v15");
    TEST_CHECK_NEAR(out_cfg.zones[0].pid_kp, 3.5f, 1e-6, "zones[0].pid_kp survives all the way to v15");
    TEST_CHECK_NEAR(out_cfg.zones[0].pid_ki, 0.2f, 1e-6, "zones[0].pid_ki survives all the way to v15");
    TEST_CHECK_NEAR(out_cfg.zones[0].max_temp_c, 1150.0f, 1e-6, "zones[0].max_temp_c survives all the way to v15");
    TEST_CHECK_NEAR(out_cfg.zones[0].model_tau_s, 300.0f, 1e-6, "zones[0].model_tau_s survives all the way to v15");
    TEST_CHECK_NEAR(out_cfg.zones[1].pid_kp, 4.5f, 1e-6, "zones[1].pid_kp (a DIFFERENT zone's value) survives too, unshifted");
    TEST_CHECK_NEAR(out_cfg.zones[1].max_temp_c, 1250.0f, 1e-6, "zones[1].max_temp_c survives too, unshifted");

    // Fields introduced well after v1 (coupling matrix: v11/v12; tuning
    // quality record: v13; adaptive_tune_enabled: v14; coupling_diag_k_dc:
    // v15) correctly land at their documented zero/unset defaults, since a v1
    // board never had any such data to carry.
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        TEST_CHECK_NEAR(out_cfg.zones[0].coupling_coeff[j], 0.0f, 1e-6, "v1 has no coupling data -- 0 (unset)");
        TEST_CHECK_NEAR(out_cfg.zones[0].coupling_tau_s[j], 0.0f, 1e-6, "v1 has no coupling tau data -- 0 (unset)");
        TEST_CHECK_NEAR(out_cfg.zones[0].coupling_dead_time_s[j], 0.0f, 1e-6, "v1 has no coupling dead-time data -- 0 (unset)");
    }
    TEST_CHECK_NEAR(out_cfg.zones[0].coupling_diag_k_dc, 0.0f, 1e-6, "v1 has no coupling_diag_k_dc -- 0 (unset)");
    TEST_CHECK(out_cfg.zones[0].adaptive_tune_enabled == 0, "v1 has no adaptive_tune_enabled -- documented default 0");
    TEST_CHECK(out_cfg.zones[0].tuning_valid == 0, "v1 has no tuning-quality record -- documented default 0 (not valid)");
    // ease_off_window_mult: introduced at v16 (ZONES_CFG_VERSION 15->16) as a
    // global scalar, moved PER-ZONE at v16->v17 (this pass). A v1 blob has
    // nothing to say about it either way, so convert_zone_v1() leaves every
    // zone's copy at its memset(0) default -- the SAME documented "0 = use
    // the firmware default" sentinel every other 0-defaulted field here uses
    // (unlike the raw literal-2.0 story this test told before the field
    // became per-zone). The actual "behaviour UNCHANGED" proof is therefore
    // two-part: the raw stored value is the legal sentinel, AND the
    // accessor a real caller (zone_taper_climb_rate()) actually uses
    // resolves that sentinel to ZONE_EASE_OFF_WINDOW_MULT_DEFAULT (2.0),
    // the exact value the removed PROFILE_EXECUTOR_EASE_OFF_WINDOW_MULT
    // #define held -- checked on every zone, not just zone 0.
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        TEST_CHECK_NEAR(out_cfg.zones[j].ease_off_window_mult, 0.0f, 1e-9,
                        "v1 has no ease_off_window_mult -- stored as the literal 0 sentinel, not resolved eagerly");
    }
    s_zones.cfg = out_cfg;
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        float got = -1.0f;
        TEST_CHECK(zones_config_get_ease_off_window_mult(j, &got) && fabsf(got - ZONE_EASE_OFF_WINDOW_MULT_DEFAULT) < 1e-6,
                  "v1 (and every pre-v16 version) resolves every zone's ease_off_window_mult to 2.0 through "
                  "the accessor, matching the removed compile-time #define exactly");
    }

    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_nvs_load_from_v9_blob_chains_through_v10_to_v11_with_zero_coupling(void)
{
    TEST_SECTION("nvs_load_from -- a v9 blob migrates through v10 to v11 (end-to-end): valid, "
                 "current version, and an all-zero coupling row (v9 never had ANY coupling data)");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v9_t src;
    memset(&src, 0, sizeof(src));
    src.version = 9;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");
    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[2].relay_mask = 0x04;
    src.zones[2].thermo_mask = 0x04;
    src.crc32 = 0;

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v9 blob must migrate all the way to a valid v11 config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
            char msg[96];
            snprintf(msg, sizeof(msg), "zones[%u].coupling_coeff[%u] is 0 (v9 has no coupling data at all)", i, j);
            TEST_CHECK_NEAR(out_cfg.zones[i].coupling_coeff[j], 0.0f, 1e-6, msg);
        }
    }

    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_validate_accepts_control_mode_pid_fuzzy_rejects_past_it(void)
{
    TEST_SECTION("validate_zones_cfg / zones_config_set_control_mode -- ZONE_CONTROL_MODE_PID_FUZZY "
                 "(3) is accepted, 4 is still rejected");
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].control_mode = (uint8_t)ZONE_CONTROL_MODE_PID_FUZZY;
        const char *reason = NULL;
        TEST_CHECK(zones_config_json_validate(&cfg, &reason), "control_mode 3 (PID_FUZZY) validates");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].control_mode = 4;
        const char *reason = NULL;
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason), "control_mode 4 is still out of range and rejected");
    }

    // zones_config_set_control_mode() enforces the identical bound. Its
    // success path calls nvs_save(), which needs the NVS stub armed (same
    // convention every other setter test in this file that persists uses).
    {
        nvs_test_enable(true);
        nvs_test_clear();
        s_zones.cfg.thermo_count = 1;
        TEST_CHECK(zones_config_set_control_mode(0, ZONE_CONTROL_MODE_PID_FUZZY),
                  "zones_config_set_control_mode(PID_FUZZY) is accepted");
        zone_control_mode_t got;
        TEST_CHECK(zones_config_get_control_mode(0, &got) && got == ZONE_CONTROL_MODE_PID_FUZZY,
                  "the accepted mode reads back correctly");
        TEST_CHECK(!zones_config_set_control_mode(0, (zone_control_mode_t)4),
                  "zones_config_set_control_mode(4) is refused");
        nvs_test_enable(false);
        nvs_test_clear();
    }
}

static void test_validate_accepts_relay_type_mercury_rejects_past_it(void)
{
    TEST_SECTION("validate_zones_cfg / zones_config_set_relay_type -- ZONE_RELAY_TYPE_MAX "
                 "(2, Mercury) is accepted, 3 is rejected (RELAY_LIFE_BUDGET.md)");
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].relay_type = ZONE_RELAY_TYPE_MAX;
        const char *reason = NULL;
        TEST_CHECK(zones_config_json_validate(&cfg, &reason), "relay_type 2 (Mercury) validates");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].relay_type = ZONE_RELAY_TYPE_MAX + 1;
        const char *reason = NULL;
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason), "relay_type past ZONE_RELAY_TYPE_MAX is rejected");
        TEST_CHECK(reason && strstr(reason, "relay_type") != NULL, "the rejection names the field");
    }

    // zones_config_set_relay_type() enforces the identical bound, and its
    // success path pushes the new type to relay_cycles_set_type() for every
    // relay in the zone's relay_mask.
    {
        nvs_test_enable(true);
        nvs_test_clear();
        memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
        s_zones.cfg.thermo_count = 1;
        s_zones.cfg.relay_count = 2;
        s_zones.cfg.zones[0].relay_mask = 0x03; // relays 0 and 1
        s_zones.cfg.timing_profile_count = 1;
        test_relay_cycles_reset_pushes();

        uint8_t got = 99;
        TEST_CHECK(zones_config_set_relay_type(0, 2), "zones_config_set_relay_type(zone 0, Mercury) is accepted");
        TEST_CHECK(zones_config_get_relay_type(0, &got) && got == 2, "the accepted type reads back correctly");
        TEST_CHECK(!zones_config_set_relay_type(0, 3), "zones_config_set_relay_type(zone 0, 3) is refused");
        TEST_CHECK(!zones_config_set_relay_type(MAX31856_CHANNEL_COUNT, 0),
                  "zones_config_set_relay_type() with an out-of-range zone index is refused");

        // THE push proof: both relays named in relay_mask (0 and 1) got the
        // new type; a relay NOT in the mask (2) was never touched.
        TEST_CHECK(s_test_relay_type_push_count[0] >= 1 && s_test_relay_type_pushed[0] == RELAY_TYPE_MERCURY,
                  "relay 0 (in zone 0's mask) was pushed RELAY_TYPE_MERCURY");
        TEST_CHECK(s_test_relay_type_push_count[1] >= 1 && s_test_relay_type_pushed[1] == RELAY_TYPE_MERCURY,
                  "relay 1 (in zone 0's mask) was pushed RELAY_TYPE_MERCURY");
        TEST_CHECK(s_test_relay_type_push_count[2] == 0,
                  "relay 2 (NOT in zone 0's mask) was never pushed -- the push is scoped to relay_mask");
        TEST_CHECK(s_test_relay_override_pushed[0] == 0, "rated_override is pushed as 0 (use the type's table)");

        nvs_test_enable(false);
        nvs_test_clear();
    }
}

// NEGATIVE TEST (feedback_negative_test_every_check.md): proves the push-
// scoping assertion above can actually fail, by temporarily widening the
// mask check to see relay 2 get touched too, then restoring it.
//   Broke zones_config_push_relay_type()'s loop condition from
//   `if (z->relay_mask & (1u << r))` to `if (1)` (push every relay
//   unconditionally, ignoring relay_mask) and re-ran this test: the
//   "relay 2 ... was never pushed" TEST_CHECK above failed as expected
//   (s_test_relay_type_push_count[2] became 1, not 0). Restored the real
//   `if (z->relay_mask & (1u << r))` condition afterward -- verified by hand
//   2026-09-06, not committed as a standing test (the same discipline
//   test_relay_cycles.c's own v1->v2 migration negative test documents in
//   its own comment).

// opus review finding (MEDIUM): zones_config_push_all_relay_types() only
// ever pushed a type onto relays named in SOME zone's relay_mask -- a relay
// dropped from every zone's mask (config edit, relay reassigned away) kept
// whatever contactor/mercury type it last had FOREVER, since nothing ever
// visited that slot again. Fixed by clearing every heater-relay slot
// (0..KILN_IO_RELAY_COUNT-1) to RELAY_TYPE_SSR before the per-zone loop
// re-asserts the real type for every relay still claimed.
static void test_push_all_relay_types_clears_orphaned_relay(void)
{
    TEST_SECTION("zones_config_push_all_relay_types -- a relay dropped from every zone's "
                 "relay_mask falls back to RELAY_TYPE_SSR instead of keeping its stale type forever");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 1;
    s_zones.cfg.relay_count = 4;
    // Only zone 0 claims relay 0, as Mercury. Relays 1..3 are claimed by no
    // zone at all -- exactly the "removed from every zone" scenario.
    s_zones.cfg.zones[0].relay_mask = 0x01;
    s_zones.cfg.zones[0].relay_type = 2; // Mercury
    s_zones.cfg.timing_profile_count = 1;
    test_relay_cycles_reset_pushes();

    zones_config_push_all_relay_types();

    TEST_CHECK(s_test_relay_type_pushed[0] == RELAY_TYPE_MERCURY,
              "relay 0 (still claimed by zone 0) ends up Mercury");
    TEST_CHECK(s_test_relay_type_pushed[1] == RELAY_TYPE_SSR &&
              s_test_relay_type_pushed[2] == RELAY_TYPE_SSR &&
              s_test_relay_type_pushed[3] == RELAY_TYPE_SSR,
              "relays 1..3 (claimed by no zone) are pushed back to RELAY_TYPE_SSR, not left stale");
    TEST_CHECK(s_test_relay_type_push_count[1] == 1 && s_test_relay_type_push_count[2] == 1 &&
              s_test_relay_type_push_count[3] == 1,
              "each orphaned relay is pushed exactly once (the clearing pass), never twice");
    // RELAY_CYCLES_SAFETY_INDEX must be left completely untouched -- that
    // slot belongs to safety_cfg_store.c, never to zones config.
    TEST_CHECK(s_test_relay_type_push_count[RELAY_CYCLES_SAFETY_INDEX] == 0,
              "the safety relay slot is never touched by zones config's push");
}

// NEGATIVE TEST (feedback_negative_test_every_check.md): proves the check
// above can actually fail. Reverted zones_config_push_all_relay_types()'s
// fix by hand -- removed the `for (r = 0; r < KILN_IO_RELAY_COUNT; r++)
// relay_cycles_set_type(r, RELAY_TYPE_SSR, 0);` clearing loop, restoring the
// old orphan-leaving behavior -- and re-ran this test: s_test_relay_type_
// push_count[1..3] all read 0 (never pushed at all, since RELAY_TYPE_SSR is
// also the fake stub's zero-initialized default, so the "pushed ==
// RELAY_TYPE_SSR" check happened to pass on a stale array read, but the
// "pushed exactly once" push_count checks failed as expected: 0 != 1).
// Restored the real clearing loop afterward -- verified by hand 2026-09-06,
// not committed as a standing mutation (same discipline as the negative
// test immediately above).

static void test_post_mode_pid_fuzzy_accepted_by_parser(void)
{
    TEST_SECTION("parse_zone_fields -- z0_mode=3 (PID_FUZZY) is accepted, z0_mode=4 is refused (0-3)");
    char body3[512];
    snprintf(body3, sizeof(body3),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=3&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&"
             "z0_timingprofile=0");
    zone_cfg_t current = make_stored_zone();
    zone_cfg_t out;
    memset(&out, 0, sizeof(out));
    const char *err_reason = "unset";
    bool ok = zones_http_parse_zone_fields(body3, 0, 1, 4, 1, &current, &out, &err_reason);
    TEST_CHECK(ok, "z0_mode=3 (PID_FUZZY) must be accepted");
    TEST_CHECK(out.control_mode == (uint8_t)ZONE_CONTROL_MODE_PID_FUZZY, "the parsed mode is PID_FUZZY");

    char body4[512];
    snprintf(body4, sizeof(body4),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=4&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&"
             "z0_timingprofile=0");
    memset(&out, 0, sizeof(out));
    err_reason = "unset";
    ok = zones_http_parse_zone_fields(body4, 0, 1, 4, 1, &current, &out, &err_reason);
    TEST_CHECK(!ok, "z0_mode=4 must still be refused -- appending PID_FUZZY did not widen the ceiling further");
    TEST_CHECK(err_reason && strstr(err_reason, "control_mode") != NULL, "the refusal names the field");
}

static void test_post_relay_type_optional_range_and_preserve(void)
{
    TEST_SECTION("parse_zone_fields -- z0_relaytype: accepted 0-2, out-of-range refused, omitted preserves "
                 "the currently-stored value (RELAY_LIFE_BUDGET.md)");

    // Present and in range (Mercury) is accepted and parsed verbatim.
    char body_ok[512];
    snprintf(body_ok, sizeof(body_ok),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_relaytype=2&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=1&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&"
             "z0_timingprofile=0");
    zone_cfg_t current = make_stored_zone();
    current.relay_type = 0; // SSR, the stored value before this submission
    zone_cfg_t out;
    memset(&out, 0, sizeof(out));
    const char *err_reason = "unset";
    bool ok = zones_http_parse_zone_fields(body_ok, 0, 1, 4, 1, &current, &out, &err_reason);
    TEST_CHECK(ok, "z0_relaytype=2 (Mercury) must be accepted");
    TEST_CHECK(out.relay_type == 2, "the parsed relay_type is Mercury (2)");

    // Present but out of range (3, past Mercury) is refused, naming the field.
    char body_bad[512];
    snprintf(body_bad, sizeof(body_bad),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_relaytype=3&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=1&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&"
             "z0_timingprofile=0");
    memset(&out, 0, sizeof(out));
    err_reason = "unset";
    ok = zones_http_parse_zone_fields(body_bad, 0, 1, 4, 1, &current, &out, &err_reason);
    TEST_CHECK(!ok, "z0_relaytype=3 is out of range (0-2) and must be refused");
    TEST_CHECK(err_reason && strstr(err_reason, "relay_type") != NULL, "the refusal names the field");

    // Omitted entirely: OPTIONAL, same as tc_type -- preserves current_z's
    // stored value rather than defaulting to 0, so an older client's
    // whole-page replay (or any body predating this field) cannot silently
    // erase an operator's earlier Contactor/Mercury choice.
    char body_omit[512];
    snprintf(body_omit, sizeof(body_omit),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=1&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&"
             "z0_timingprofile=0");
    zone_cfg_t current_contactor = make_stored_zone();
    current_contactor.relay_type = 1; // Contactor, previously chosen
    memset(&out, 0, sizeof(out));
    err_reason = "unset";
    ok = zones_http_parse_zone_fields(body_omit, 0, 1, 4, 1, &current_contactor, &out, &err_reason);
    TEST_CHECK(ok, "a body omitting z0_relaytype entirely must still be accepted");
    TEST_CHECK(out.relay_type == 1,
              "omitting z0_relaytype preserves the currently-stored value (Contactor), not a silent reset to SSR");
}

/* docs/ON_OFF_ZONE_PLAN.md step 6: zone_type/failsafe_state/hyst_c/min_on_s/
 * min_off_s -- same optional/range-checked/omit-preserves shape as
 * z0_relaytype above, this is the first pass that lets a POST touch these
 * five fields at all (step 1 only added storage). */
static void test_post_on_off_fields_optional_range_and_preserve(void)
{
    TEST_SECTION("parse_zone_fields -- z0_zonetype/z0_failsafe/z0_hystc/z0_minons/z0_minoffs: "
                 "accepted in range, out-of-range refused, omitted preserves the stored value "
                 "(ON_OFF_ZONE_PLAN.md step 6)");

    char body_ok[700];
    snprintf(body_ok, sizeof(body_ok),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=1&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&"
             "z0_timingprofile=0&"
             "z0_zonetype=1&z0_failsafe=1&z0_hystc=5.5&z0_minons=45&z0_minoffs=90");
    zone_cfg_t current = make_stored_zone();
    current.zone_type = 0;
    current.failsafe_state = 0;
    current.hyst_c = 0.0f;
    current.min_on_s = 0;
    current.min_off_s = 0;
    zone_cfg_t out;
    memset(&out, 0, sizeof(out));
    const char *err_reason = "unset";
    bool ok = zones_http_parse_zone_fields(body_ok, 0, 1, 4, 1, &current, &out, &err_reason);
    TEST_CHECK(ok, "in-range zone_type/failsafe/hyst_c/min_on_s/min_off_s must be accepted");
    TEST_CHECK(out.zone_type == 1, "zone_type=1 (ON_OFF) is parsed verbatim");
    TEST_CHECK(out.failsafe_state == 1, "failsafe_state=1 (ON) is parsed verbatim");
    TEST_CHECK(out.hyst_c > 5.49f && out.hyst_c < 5.51f, "hyst_c=5.5 is parsed verbatim");
    TEST_CHECK(out.min_on_s == 45, "min_on_s=45 is parsed verbatim");
    TEST_CHECK(out.min_off_s == 90, "min_off_s=90 is parsed verbatim");

    // zone_type past ZONE_TYPE_ON_OFF (1) is refused, naming the field.
    char body_bad_type[700];
    snprintf(body_bad_type, sizeof(body_bad_type),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=1&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&"
             "z0_timingprofile=0&z0_zonetype=2");
    memset(&out, 0, sizeof(out));
    err_reason = "unset";
    ok = zones_http_parse_zone_fields(body_bad_type, 0, 1, 4, 1, &current, &out, &err_reason);
    TEST_CHECK(!ok, "z0_zonetype=2 is out of range (0-1) and must be refused");
    TEST_CHECK(err_reason && strstr(err_reason, "zone_type") != NULL, "the refusal names the field");

    // hyst_c below ZONE_HYST_C_MIN (but nonzero) is refused.
    char body_bad_hyst[700];
    snprintf(body_bad_hyst, sizeof(body_bad_hyst),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=1&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&"
             "z0_timingprofile=0&z0_hystc=0.1");
    memset(&out, 0, sizeof(out));
    err_reason = "unset";
    ok = zones_http_parse_zone_fields(body_bad_hyst, 0, 1, 4, 1, &current, &out, &err_reason);
    TEST_CHECK(!ok, "z0_hystc=0.1 is below ZONE_HYST_C_MIN (0.5) and must be refused");
    TEST_CHECK(err_reason && strstr(err_reason, "hyst_c") != NULL, "the refusal names the field");

    // min_on_s past ZONE_MIN_ON_OFF_S_MAX is refused.
    char body_bad_minon[700];
    snprintf(body_bad_minon, sizeof(body_bad_minon),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=1&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&"
             "z0_timingprofile=0&z0_minons=999999");
    memset(&out, 0, sizeof(out));
    err_reason = "unset";
    ok = zones_http_parse_zone_fields(body_bad_minon, 0, 1, 4, 1, &current, &out, &err_reason);
    TEST_CHECK(!ok, "z0_minons=999999 is past ZONE_MIN_ON_OFF_S_MAX (3600) and must be refused");
    TEST_CHECK(err_reason && strstr(err_reason, "min_on_s") != NULL, "the refusal names the field");

    // All five omitted entirely: preserves current_z's stored values, same
    // "an older client must not silently reset this" reasoning as
    // z0_relaytype's own omit case above.
    char body_omit[700];
    snprintf(body_omit, sizeof(body_omit),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=1&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&"
             "z0_timingprofile=0");
    zone_cfg_t current_onoff = make_stored_zone();
    current_onoff.zone_type = 1;
    current_onoff.failsafe_state = 1;
    current_onoff.hyst_c = 3.0f;
    current_onoff.min_on_s = 60;
    current_onoff.min_off_s = 120;
    memset(&out, 0, sizeof(out));
    err_reason = "unset";
    ok = zones_http_parse_zone_fields(body_omit, 0, 1, 4, 1, &current_onoff, &out, &err_reason);
    TEST_CHECK(ok, "a body omitting all five on/off fields must still be accepted");
    TEST_CHECK(out.zone_type == 1 && out.failsafe_state == 1 && out.hyst_c == 3.0f &&
              out.min_on_s == 60 && out.min_off_s == 120,
              "omitting the on/off fields preserves every stored value, not a silent reset to HEATER/OFF/0");
}

/* One clean body with only the field-under-test varied, mirroring
 * post_body_with_minon()'s own pattern above. */
static bool post_body_with_fuzzy_strength(const char *literal, const char **err_reason_out, float *out_value)
{
    char body[600];
    snprintf(body, sizeof(body),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=3&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&"
             "z0_timingprofile=0&z0_fuzzy_strength=%s",
             literal);
    zone_cfg_t current = make_stored_zone();
    zone_cfg_t out;
    memset(&out, 0, sizeof(out));
    const char *err_reason = "unset";
    bool ok = zones_http_parse_zone_fields(body, 0, 1, 4, 1, &current, &out, &err_reason);
    if (err_reason_out) {
        *err_reason_out = err_reason;
    }
    if (out_value) {
        *out_value = out.fuzzy_strength_pct;
    }
    return ok;
}

static void test_post_fuzzy_strength_out_of_range_refused_not_clamped(void)
{
    TEST_SECTION("parse_zone_fields -- z0_fuzzy_strength=101 and =-1 are REFUSED, not clamped to 100/0");
    const char *reason = "unset";
    float value = -99.0f;

    TEST_CHECK(!post_body_with_fuzzy_strength("101", &reason, &value),
              "101 (just above the 0-100 range) must be refused outright");
    TEST_CHECK(reason && strstr(reason, "fuzzy_strength_pct") != NULL, "the refusal names the field");

    TEST_CHECK(!post_body_with_fuzzy_strength("-1", &reason, &value),
              "-1 (just below the 0-100 range) must be refused outright");
    TEST_CHECK(reason && strstr(reason, "fuzzy_strength_pct") != NULL, "the refusal names the field");

    // Positive control: the boundary values themselves must still be accepted
    // EXACTLY, proving a rejected 101/-1 is a real range check, not a
    // bug that rejects everything.
    TEST_CHECK(post_body_with_fuzzy_strength("0", &reason, &value), "0 is accepted");
    TEST_CHECK_NEAR(value, 0.0f, 1e-6, "0 is stored as 0 exactly");
    TEST_CHECK(post_body_with_fuzzy_strength("100", &reason, &value), "100 is accepted");
    TEST_CHECK_NEAR(value, 100.0f, 1e-6, "100 is stored as 100 exactly");
}

/* Generic single-field body, so the three post-review refusals below can be
 * exercised without one helper per field. */
static bool post_body_with_extra(const char *extra_kv, const char **err_reason_out, zone_cfg_t *out_zone)
{
    char body[700];
    snprintf(body, sizeof(body),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=3&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&"
             "z0_timingprofile=0&%s",
             extra_kv);
    zone_cfg_t current = make_stored_zone();
    zone_cfg_t out;
    memset(&out, 0, sizeof(out));
    const char *err_reason = "unset";
    /* thermo_count = MAX31856_CHANNEL_COUNT, not 1: settings_source's
     * cross-zone chain-walk (zones_http_parse_zone_fields()'s own comment on it) is
     * bounded by thermo_count -- the same "unused trailing slot" discipline
     * used everywhere else in this file -- so a thermo_count of 1 would make
     * every OTHER zone index invisible to that check and silently defeat
     * the cycle tests that exercise this door. Every other field this
     * helper posts only ever touches zone 0, so widening thermo_count here
     * does not change what any of those checks accept or reject. */
    bool ok = zones_http_parse_zone_fields(body, 0, MAX31856_CHANNEL_COUNT, 4, 1, &current, &out, &err_reason);
    if (err_reason_out) *err_reason_out = err_reason;
    if (out_zone) *out_zone = out;
    return ok;
}

/* All three of these were found by review AFTER the phase was first called
 * done, and each one silently accepted bad input rather than refusing it. */
static void test_post_new_fields_present_but_unparseable_are_refused(void)
{
    TEST_SECTION("parse_zone_fields -- a PRESENT but empty/over-long new field is refused, not "
                 "treated as omitted");
    const char *reason = "unset";
    zone_cfg_t out;

    /* An empty value ("key=") makes http_form_find_field() return 0, and an
     * over-long one returns -2. The original probe tested `> 0`, so BOTH
     * fell into the preserve-on-omit branch: a 200 OK, the old value kept,
     * and the operator told nothing. Only a genuinely absent key is an
     * omission. */
    TEST_CHECK(!post_body_with_extra("z0_fuzzy_strength=", &reason, &out),
              "an empty z0_fuzzy_strength= is refused, not silently treated as omitted");
    TEST_CHECK(!post_body_with_extra("z0_coupling_c1=", &reason, &out),
              "an empty z0_coupling_c1= is refused");
    TEST_CHECK(!post_body_with_extra(
                   "z0_fuzzy_strength=999999999999999999999999999999999999", &reason, &out),
              "an over-long z0_fuzzy_strength value is refused, not swallowed by the probe buffer");

    /* Positive control: genuinely omitting the key still succeeds, so the
     * above is a real present-vs-absent distinction and not a blanket
     * rejection. */
    TEST_CHECK(post_body_with_extra("z0_kp=1", &reason, &out),
              "omitting the new fields entirely is still accepted (preserve-on-omit intact)");
}

// 2026-08-30 (ZONES_CFG_VERSION 10->11): coupling_neighbor_zone as a
// separate field no longer exists -- "which neighbor" is now which
// z%u_coupling_c%u key was posted, not a value. What replaces the old
// integrality check is the diagonal rule: z0_coupling_c0 (zone 0's OWN
// index) must be exactly 0, since a zone's response to its own heater is
// model_k_dc, not a coupling cell.
static void test_post_coupling_diagonal_must_be_zero(void)
{
    TEST_SECTION("parse_zone_fields -- z0_coupling_c0 (the diagonal) must be exactly 0, "
                 "an off-diagonal cell accepts the normal range");
    const char *reason = "unset";
    zone_cfg_t out;

    TEST_CHECK(!post_body_with_extra("z0_coupling_c0=1.5", &reason, &out),
              "a nonzero diagonal cell is refused at the door");
    TEST_CHECK(reason && strstr(reason, "coupling_coeff") != NULL, "the refusal names the field");

    TEST_CHECK(post_body_with_extra("z0_coupling_c0=0", &reason, &out),
              "a diagonal cell posted as exactly 0 is accepted (a no-op, not an error)");
    TEST_CHECK_NEAR(out.coupling_coeff[0], 0.0f, 1e-6, "and reads back as 0");

    TEST_CHECK(post_body_with_extra("z0_coupling_c2=8.25", &reason, &out),
              "an off-diagonal cell in range is accepted");
    TEST_CHECK_NEAR(out.coupling_coeff[2], 8.25f, 1e-6, "and is stored exactly");
}

static void test_post_settings_source_self_reference_refused(void)
{
    TEST_SECTION("parse_zone_fields -- a zone cannot claim 'same settings as' itself");
    const char *reason = "unset";
    zone_cfg_t out;

    /* zones_http_parse_zone_fields()'s settings_source cross-zone chain-walk consults
     * the LIVE s_zones.cfg for every zone other than the one being posted
     * (post_body_with_extra()'s own `current` argument only ever covers
     * zone 0's OTHER fields, not this cross-zone check) -- so this test
     * must not run against whatever s_zones.cfg happened to be left at by
     * an earlier test in this suite. Reset every zone to Custom (the same
     * "never leave an in-use zone at raw 0 unintentionally" reasoning as
     * every other settings_source test in this file) so "pointing at a
     * different zone" below is checked against a clean, unlinked config. */
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
            s_zones.cfg.zones[z].settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
        }
    }

    TEST_CHECK(!post_body_with_extra("z0_settings_source=0", &reason, &out),
              "zone 0 pointing at zone 0 is the degenerate cycle and is refused");
    TEST_CHECK(reason && strstr(reason, "settings_source") != NULL, "the refusal names the field");

    /* Positive controls: CUSTOM and a different zone both still work, so the
     * check is specifically self-reference and not a blanket refusal. */
    TEST_CHECK(post_body_with_extra("z0_settings_source=255", &reason, &out),
              "ZONE_SETTINGS_SOURCE_CUSTOM (0xFF) is still accepted");
    TEST_CHECK(out.settings_source[SRC_GROUP_LIMITS] == 0xFF, "and is stored as CUSTOM");
    TEST_CHECK(post_body_with_extra("z0_settings_source=1", &reason, &out),
              "pointing at a DIFFERENT zone is still accepted");
    TEST_CHECK(out.settings_source[SRC_GROUP_LIMITS] == 1, "and is stored as that zone index");
}

static void test_post_omitting_new_fields_preserves_stored_values(void)
{
    TEST_SECTION("parse_zone_fields -- omitting z0_fuzzy_strength/z0_coupling_coeff/"
                 "z0_coupling_neighbor/z0_settings_source succeeds and PRESERVES the "
                 "previously-stored values, never zeroing them");
    char body[512];
    snprintf(body, sizeof(body),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=3&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&"
             "z0_timingprofile=0"); // none of the four new fields present

    zone_cfg_t current = make_stored_zone();
    current.fuzzy_strength_pct = 42.0f;
    current.coupling_coeff[1] = 5.0f;
    current.coupling_coeff[2] = 7.5f;
    current.settings_source[SRC_GROUP_LIMITS] = 1; /* "copies zone 1" -- a real, previously-chosen value */
    /* ZONES_CFG_VERSION 14->15: a real, previously-measured diagonal cell --
     * z0_coupling_diag_k_dc is also absent from body above, so this must
     * survive the same "omit preserves" rule as the four fields already
     * covered by this test. */
    current.coupling_diag_k_dc = 18.75f;
    /* ZONES_CFG_VERSION 16->17: a real, previously-set per-zone A/B value --
     * z0_easeoffmult is also absent from body above, so this must survive
     * the same "omit preserves" rule too, exactly like coupling_diag_k_dc
     * just above it. */
    current.ease_off_window_mult = 3.5f;
    /* ZONES_CFG_VERSION 13->14: the operator's adaptive-tune opt-in. It has
     * NO z%u_ POST key at all (adaptive_tune.c is its only writer), so this
     * is the strongest form of "omitted": a client CANNOT send it even if it
     * wanted to, and a whole-page save must therefore never clear it. */
    current.adaptive_tune_enabled = 1;
    /* ZONES_CFG_VERSION 25->26 (opus adversarial review of 9728865): the
     * adaptive-tune ratchet anchor. Like adaptive_tune_enabled it has NO
     * z%u_ POST key at all, and unlike most preserved fields its zero value
     * is a live sentinel ("no baseline recorded yet") that makes the next
     * accepted refinement re-anchor to the already-adapted model_k_dc --
     * i.e. clearing it here silently restores the very ratchet 9728865
     * removed. */
    current.autotune_baseline_k_dc = 12.5f;

    zone_cfg_t out;
    memset(&out, 0, sizeof(out));
    const char *err_reason = "unset";
    bool ok = zones_http_parse_zone_fields(body, 0, 1, 4, 1, &current, &out, &err_reason);

    TEST_CHECK(ok, "a POST omitting all five new fields must still succeed (optionality)");
    TEST_CHECK_NEAR(out.fuzzy_strength_pct, 42.0f, 1e-6,
                    "fuzzy_strength_pct must be PRESERVED, not zeroed, when omitted");
    TEST_CHECK_NEAR(out.coupling_coeff[1], 5.0f, 1e-6,
                    "coupling_coeff[1] must be PRESERVED, not zeroed, when omitted");
    TEST_CHECK_NEAR(out.coupling_coeff[2], 7.5f, 1e-6,
                    "coupling_coeff[2] must be PRESERVED, not zeroed, when omitted");
    TEST_CHECK(out.settings_source[SRC_GROUP_LIMITS] == 1,
              "settings_source must be PRESERVED at its previously-chosen value, not reset to CUSTOM "
              "or zeroed to \"copies zone 0\"");
    TEST_CHECK_NEAR(out.coupling_diag_k_dc, 18.75f, 1e-6,
                    "coupling_diag_k_dc must be PRESERVED, not zeroed, when omitted -- it is a "
                    "measured quantity, same as fuzzy_strength_pct/coupling_coeff above");
    TEST_CHECK_NEAR(out.ease_off_window_mult, 3.5f, 1e-6,
                    "ease_off_window_mult must be PRESERVED at its previously-set per-zone A/B value, "
                    "not reset to 0/2.0, when z0_easeoffmult is omitted -- an older client that "
                    "predates this field must not silently end a running z0-only A/B arm");
    TEST_CHECK(out.adaptive_tune_enabled == 1,
              "adaptive_tune_enabled must be PRESERVED by a whole-page save -- it has no POST key "
              "at all, so a save that clears it silently disables adaptive tuning behind the "
              "operator's back and persists that to NVS");
    TEST_CHECK_NEAR(out.autotune_baseline_k_dc, 12.5f, 1e-6,
                    "autotune_baseline_k_dc must be PRESERVED by a whole-page save -- it has no POST "
                    "key at all, and zeroing it re-arms the adaptive_tune K_dc ratchet by making the "
                    "next accepted refinement bootstrap its anchor from the already-adapted value");
}

// docs/audits/zones_post_omit_preserves_model_2026-09-14.md: owner directive,
// following a REAL casualty (docs/audits/plant_model_loss_investigation_
// 2026-09-14.md -- all three bench zones' identified models genuinely
// zeroed by a whole-page POST that omitted these keys, undetected for three
// days). zones_http_post_parse.c's z%u_k/z%u_tau/z%u_deadtime block used to
// delete the stored model on omission, deliberately and documented
// (docs/audits/zones_post_model_key_omission_2026-09-13.md pinned that OLD
// behaviour down as intentional the day before the real casualty was found).
// THIS TEST IS THE ONE THAT WOULD HAVE CAUGHT THE ORIGINAL DEFECT: a
// whole-page POST that omits all three model keys must now PRESERVE the
// stored model, matching every other measured-quantity field on this page
// (fuzzy_strength_pct, coupling_coeff[], coupling_diag_k_dc, etc).
// post_body_with_extra()'s own base body (unlike make_stored_zone(), which
// seeds a real measured triple) never includes z0_k/z0_tau/z0_deadtime, so
// any call through it already exercises the omit path -- this test names
// that explicitly instead of relying on it being incidental.
static void test_post_omitting_model_fields_preserves_them(void)
{
    TEST_SECTION("parse_zone_fields -- omitting z0_k/z0_tau/z0_deadtime PRESERVES the stored "
                 "plant model (2026-09-14 contract change -- this is the regression test for "
                 "the real plant-model-loss incident)");
    const char *reason = "unset";
    zone_cfg_t out;

    // make_stored_zone() (post_body_with_extra()'s `current`) seeds a real
    // measured triple: model_k_dc=12.0, model_tau_s=300.0,
    // model_dead_time_s=30.0. A whole-page body that never mentions any of
    // the three z%u_ keys must be ACCEPTED (they are optional, for
    // pre-model clients) AND must read back the prior triple unchanged --
    // this is the exact scenario that zeroed the live board's models.
    TEST_CHECK(post_body_with_extra("z0_kp=1", &reason, &out),
              "a whole-page submission omitting z0_k/z0_tau/z0_deadtime entirely is still accepted");
    TEST_CHECK_NEAR(out.model_k_dc, 12.0f, 1e-6,
                    "model_k_dc must be PRESERVED, not deleted -- this is the exact field the live "
                    "bench board lost for three days undetected (plant_model_loss_investigation_"
                    "2026-09-14.md)");
    TEST_CHECK_NEAR(out.model_tau_s, 300.0f, 1e-6, "model_tau_s likewise preserved, not zeroed");
    TEST_CHECK_NEAR(out.model_dead_time_s, 30.0f, 1e-6, "model_dead_time_s likewise preserved, not zeroed");

    // Per-field behaviour: an EXPLICIT z0_k (with tau/deadtime omitted) is
    // honoured exactly as sent for the field that was present, and the
    // omitted siblings are independently preserved from current_z -- proving
    // this is a true per-field "present -> parse, absent -> preserve" rule,
    // not an all-or-nothing group.
    TEST_CHECK(post_body_with_extra("z0_kp=1&z0_k=55.5", &reason, &out),
              "an explicit z0_k alongside omitted z0_tau/z0_deadtime is accepted");
    TEST_CHECK_NEAR(out.model_k_dc, 55.5f, 1e-6, "the explicitly posted z0_k is honoured exactly");
    TEST_CHECK_NEAR(out.model_tau_s, 300.0f, 1e-6,
                    "model_tau_s is independently preserved -- each of the three keys is "
                    "evaluated on its own presence, not as a single all-or-nothing group");
    TEST_CHECK_NEAR(out.model_dead_time_s, 30.0f, 1e-6, "model_dead_time_s independently preserved too");

    // Deliberate clear: sending all three keys explicitly as 0 still zeroes
    // the model -- this is the one intentional way left to erase it (see
    // this file's own comment above the z%u_k block). A client must KNOW
    // about and explicitly choose this; mere omission can no longer do it.
    TEST_CHECK(post_body_with_extra("z0_kp=1&z0_k=0&z0_tau=0&z0_deadtime=0", &reason, &out),
              "explicitly posting all three model keys as 0 (deliberate clear) is accepted");
    TEST_CHECK_NEAR(out.model_k_dc, 0.0f, 1e-6, "model_k_dc explicitly cleared when the operator asks");
    TEST_CHECK_NEAR(out.model_tau_s, 0.0f, 1e-6, "model_tau_s explicitly cleared when the operator asks");
    TEST_CHECK_NEAR(out.model_dead_time_s, 0.0f, 1e-6, "model_dead_time_s explicitly cleared when the operator asks");

    // A whole-page save that echoes every one of the three keys back
    // (zones_page.html's actual behaviour -- see its own "Echoed back
    // exactly as loaded" comment above the z0_k/_tau/_deadtime pushes) must
    // still round-trip the model unchanged, exactly as before this change.
    TEST_CHECK(post_body_with_extra("z0_kp=1&z0_k=12&z0_tau=300&z0_deadtime=30", &reason, &out),
              "explicitly re-posting all three model keys (the shipped page's own behaviour) "
              "is accepted");
    TEST_CHECK_NEAR(out.model_k_dc, 12.0f, 1e-6, "model_k_dc round-trips unchanged");
    TEST_CHECK_NEAR(out.model_tau_s, 300.0f, 1e-6, "model_tau_s round-trips unchanged");
    TEST_CHECK_NEAR(out.model_dead_time_s, 30.0f, 1e-6, "model_dead_time_s round-trips unchanged");
}

// Opus review of 5672719 (item 2): no host test posted a
// z%u_settings_source_<group> key before this -- docs/ARCHITECTURE_DECISIONS.md#zones-page-clean-up-info-disclosure-schema-v20-v21-chartjs's
// per-group split (zones_http_post_parse.c's z%u_settings_source_%s block)
// had zero direct coverage. Three tests below: per-group keys with no legacy
// scalar present; both keys in the same submission (per-group key must win);
// and every key omitted (must preserve the live per-group values, not reset
// any group to CUSTOM).
static void test_post_settings_source_group_keys_only(void)
{
    TEST_SECTION("parse_zone_fields -- z0_settings_source_<group> keys with no legacy "
                 "z0_settings_source scalar present: each group takes its own posted value");
    char body[512];
    snprintf(body, sizeof(body),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=3&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&"
             "z0_timingprofile=0&"
             "z0_settings_source_limits=1&z0_settings_source_relaytiming=2&"
             "z0_settings_source_control=255&z0_settings_source_guards=1&z0_settings_source_tc=2");

    zone_cfg_t current = make_stored_zone();
    for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
        current.settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
    }

    zone_cfg_t out;
    memset(&out, 0, sizeof(out));
    const char *err_reason = "unset";
    bool ok = zones_http_parse_zone_fields(body, 0, 1, 4, 1, &current, &out, &err_reason);

    TEST_CHECK(ok, "a submission naming only the per-group keys must succeed");
    TEST_CHECK(out.settings_source[SRC_GROUP_LIMITS] == 1, "LIMITS took its own posted value (1)");
    TEST_CHECK(out.settings_source[SRC_GROUP_RELAY_TIMING] == 2, "RELAY_TIMING took its own posted value (2)");
    TEST_CHECK(out.settings_source[SRC_GROUP_CONTROL] == ZONE_SETTINGS_SOURCE_CUSTOM,
              "CONTROL took its own posted value (255/CUSTOM)");
    TEST_CHECK(out.settings_source[SRC_GROUP_GUARDS] == 1, "GUARDS took its own posted value (1)");
    TEST_CHECK(out.settings_source[SRC_GROUP_TC] == 2, "TC took its own posted value (2)");
}

static void test_post_settings_source_group_key_wins_over_legacy_scalar(void)
{
    TEST_SECTION("parse_zone_fields -- a submission with BOTH the legacy z0_settings_source "
                 "scalar AND a per-group key for the same zone: the per-group key wins for that "
                 "one group, the legacy scalar is the default for every other group");
    char body[512];
    snprintf(body, sizeof(body),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=3&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&"
             "z0_timingprofile=0&"
             "z0_settings_source=1&z0_settings_source_control=2");

    zone_cfg_t current = make_stored_zone();
    for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
        current.settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
    }

    zone_cfg_t out;
    memset(&out, 0, sizeof(out));
    const char *err_reason = "unset";
    bool ok = zones_http_parse_zone_fields(body, 0, 1, 4, 1, &current, &out, &err_reason);

    TEST_CHECK(ok, "legacy scalar + one per-group key together must succeed");
    TEST_CHECK(out.settings_source[SRC_GROUP_CONTROL] == 2,
              "the per-group z0_settings_source_control key WINS over the legacy scalar for CONTROL");
    TEST_CHECK(out.settings_source[SRC_GROUP_LIMITS] == 1,
              "LIMITS has no per-group key, so it falls back to the legacy scalar (1)");
    TEST_CHECK(out.settings_source[SRC_GROUP_RELAY_TIMING] == 1,
              "RELAY_TIMING has no per-group key either, same legacy-scalar fallback (1)");
    TEST_CHECK(out.settings_source[SRC_GROUP_GUARDS] == 1,
              "GUARDS has no per-group key either, same legacy-scalar fallback (1)");
    TEST_CHECK(out.settings_source[SRC_GROUP_TC] == 1,
              "TC has no per-group key either, same legacy-scalar fallback (1)");
}

static void test_post_settings_source_all_keys_omitted_preserves_every_group(void)
{
    TEST_SECTION("parse_zone_fields -- omitting BOTH the legacy scalar and every per-group "
                 "settings_source key preserves every group's own previously-stored value "
                 "independently, never collapsing them to one another or to CUSTOM");
    char body[512];
    snprintf(body, sizeof(body),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=3&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&"
             "z0_timingprofile=0"); // no settings_source key of any kind

    zone_cfg_t current = make_stored_zone();
    current.settings_source[SRC_GROUP_LIMITS] = 1;
    current.settings_source[SRC_GROUP_RELAY_TIMING] = 2;
    current.settings_source[SRC_GROUP_CONTROL] = ZONE_SETTINGS_SOURCE_CUSTOM;
    current.settings_source[SRC_GROUP_GUARDS] = 1;
    current.settings_source[SRC_GROUP_TC] = 2;

    zone_cfg_t out;
    memset(&out, 0, sizeof(out));
    const char *err_reason = "unset";
    bool ok = zones_http_parse_zone_fields(body, 0, 1, 4, 1, &current, &out, &err_reason);

    TEST_CHECK(ok, "a submission with no settings_source key at all must still succeed");
    TEST_CHECK(out.settings_source[SRC_GROUP_LIMITS] == 1, "LIMITS preserved at its own live value (1)");
    TEST_CHECK(out.settings_source[SRC_GROUP_RELAY_TIMING] == 2,
              "RELAY_TIMING preserved at its own DIFFERENT live value (2), not LIMITS's 1");
    TEST_CHECK(out.settings_source[SRC_GROUP_CONTROL] == ZONE_SETTINGS_SOURCE_CUSTOM,
              "CONTROL preserved at CUSTOM");
    TEST_CHECK(out.settings_source[SRC_GROUP_GUARDS] == 1, "GUARDS preserved at its own live value (1)");
    TEST_CHECK(out.settings_source[SRC_GROUP_TC] == 2, "TC preserved at its own live value (2)");
}

// End-to-end round trip through the real handlers: a POST carrying real
// values for all four new fields, then a GET, must report exactly what was
// posted. Uses run_zones_post() (the real zones_post_handler()) and
// zones_get_handler() directly, with httpd_resp_send() captured by this
// file's own stub above.
static void test_post_then_get_round_trips_new_fields(void)
{
    TEST_SECTION("zones_post_handler -> zones_get_handler -- the four new fields round-trip exactly");

    char body[900];
    snprintf(body, sizeof(body),
             "thermo_count=1&relay_count=1&" MINIMAL_TIMING_PROFILE_BODY
             "&z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&z0_timingprofile=0&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=0&z0_sanity=0&z0_mode=3&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&"
             "z0_fuzzy_strength=12.5&z0_coupling_c1=1.5&z0_coupling_c2=3.25&"
             "z0_coupling_diag_k_dc=25.75&"
             "z0_easeoffmult=3.5&"
             "z0_settings_source=255");
    run_zones_post(body);
    TEST_CHECK(s_test_ok_called && !s_test_err_called, "the whole-page POST with new fields must be accepted");

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = zones_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "zones_get_handler must return ESP_OK");

    TEST_CHECK(strstr(s_last_resp_body, "\"control_mode\":3") != NULL,
              "GET reports the posted control_mode (PID_FUZZY)");
    TEST_CHECK(strstr(s_last_resp_body, "\"fuzzy_strength_pct\":12.50") != NULL,
              "GET reports the posted fuzzy_strength_pct exactly");
    TEST_CHECK(strstr(s_last_resp_body, "\"coupling_c1\":1.5000") != NULL,
              "GET reports the posted coupling_coeff[1] exactly");
    TEST_CHECK(strstr(s_last_resp_body, "\"coupling_c2\":3.2500") != NULL,
              "GET reports the posted coupling_coeff[2] exactly");
    TEST_CHECK(strstr(s_last_resp_body, "\"coupling_c0\":0.0000") != NULL,
              "GET reports the untouched diagonal cell as 0 (never omitted)");
    TEST_CHECK(strstr(s_last_resp_body, "\"coupling_diag_k_dc\":25.7500") != NULL,
              "GET reports the posted coupling_diag_k_dc exactly");
    TEST_CHECK(strstr(s_last_resp_body, "\"ease_off_window_mult\":3.500") != NULL,
              "GET reports the posted per-zone ease_off_window_mult exactly");
    TEST_CHECK(strstr(s_last_resp_body, "\"settings_source\":255") != NULL,
              "GET reports the posted settings_source (CUSTOM) exactly");
    /* 2026-09-09 (opus review defect D): the v23->v24 schema bump recorded
     * model_fit_temp_c/model_fit_ambient_c but exposed them nowhere -- no
     * JSON key, and zones_config_get_model_fit_context() had no production
     * caller -- so the recorded operating point could not be read off the
     * board at all and the retrospective-schedule use case that motivated
     * the bump was unreachable. Read-only, like the tuning_* record. MOVED
     * to GET /api/zones_diag 2026-09-14 (docs/audits/
     * zones_diag_endpoint_split_2026-09-14.md) -- zones_get_handler() no
     * longer emits either key at all now, confirmed here (a regression that
     * brought them back onto /api/zones would eat back the headroom this
     * split recovered), and the diag endpoint's own emission is checked by
     * test_zones_diag_get_handler_round_trips_moved_fields() below. */
    TEST_CHECK(strstr(s_last_resp_body, "\"model_fit_temp_c\":") == NULL,
              "GET /api/zones must NOT emit model_fit_temp_c any more -- moved to /api/zones_diag");
    TEST_CHECK(strstr(s_last_resp_body, "\"model_fit_ambient_c\":") == NULL,
              "GET /api/zones must NOT emit model_fit_ambient_c any more -- moved to /api/zones_diag");
    TEST_CHECK(strstr(s_last_resp_body, "\"coupling_tau_c") == NULL,
              "GET /api/zones must NOT emit coupling_tau_c%u any more -- moved to /api/zones_diag");
    TEST_CHECK(strstr(s_last_resp_body, "\"coupling_dead_time_c") == NULL,
              "GET /api/zones must NOT emit coupling_dead_time_c%u any more -- moved to /api/zones_diag");

    httpd_req_t diag_req;
    memset(&diag_req, 0, sizeof(diag_req));
    err = zones_diag_get_handler(&diag_req);
    TEST_CHECK(err == ESP_OK, "zones_diag_get_handler must return ESP_OK");
    TEST_CHECK(strstr(s_last_resp_body, "\"model_fit_temp_c\":") != NULL,
              "GET /api/zones_diag must emit model_fit_temp_c -- otherwise the fit's operating "
              "point is unreadable");
    TEST_CHECK(strstr(s_last_resp_body, "\"model_fit_ambient_c\":") != NULL,
              "GET /api/zones_diag must emit model_fit_ambient_c too");
}

// docs/audits/zones_post_omit_preserves_model_2026-09-14.md: end-to-end
// regression test for the real plant-model-loss incident, through the ACTUAL
// handlers (zones_post_handler() then zones_get_handler()), not just the
// per-field parser. POST 1 establishes a real measured model. POST 2 is an
// otherwise-ordinary whole-page save that omits all three model keys --
// exactly the shape of the POST that zeroed the live bench board's models
// for three days undetected. GET afterward must still report the model from
// POST 1, and a follow-up explicit-zero POST must still be able to clear it
// on purpose.
static void test_post_then_get_round_trips_model_across_an_omitting_save(void)
{
    TEST_SECTION("zones_post_handler -> zones_get_handler -- a later whole-page save that omits "
                 "z0_k/z0_tau/z0_deadtime must NOT erase an earlier real model (regression test "
                 "for the live plant-model-loss incident, docs/audits/"
                 "plant_model_loss_investigation_2026-09-14.md)");

    char body[900];
    snprintf(body, sizeof(body),
             "thermo_count=1&relay_count=1&" MINIMAL_TIMING_PROFILE_BODY
             "&z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&z0_timingprofile=0&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=0&z0_sanity=0&z0_mode=3&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&"
             "z0_k=42.731&z0_tau=255.6&z0_deadtime=40.3");
    run_zones_post(body);
    TEST_CHECK(s_test_ok_called && !s_test_err_called, "POST 1 (establishes the model) must be accepted");
    TEST_CHECK_NEAR(s_zones.cfg.zones[0].model_k_dc, 42.731, 1e-3, "model_k_dc landed as posted");

    // POST 2: an ordinary whole-page save (e.g. only touching pid_kp) that
    // never mentions z0_k/z0_tau/z0_deadtime at all -- the real-world shape
    // of the POST that caused the incident.
    snprintf(body, sizeof(body),
             "thermo_count=1&relay_count=1&" MINIMAL_TIMING_PROFILE_BODY
             "&z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&z0_timingprofile=0&"
             "z0_cal=0&z0_kp=2&z0_ki=0&z0_kd=0&z0_ramp=0&z0_sanity=0&z0_mode=3&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0");
    run_zones_post(body);
    TEST_CHECK(s_test_ok_called && !s_test_err_called,
              "POST 2 (omits the model keys entirely) must still be accepted");

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = zones_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "zones_get_handler must return ESP_OK");
    TEST_CHECK(strstr(s_last_resp_body, "\"model_k_dc\":42.7310") != NULL,
              "GET after the omitting save must still report the ORIGINAL model_k_dc -- this is "
              "the exact assertion that would have caught the live incident before it happened");
    TEST_CHECK(strstr(s_last_resp_body, "\"model_tau_s\":255.6") != NULL,
              "GET after the omitting save must still report the original model_tau_s");
    TEST_CHECK(strstr(s_last_resp_body, "\"model_dead_time_s\":40.3") != NULL,
              "GET after the omitting save must still report the original model_dead_time_s");
    TEST_CHECK(strstr(s_last_resp_body, "\"pid_kp\":2.0000") != NULL,
              "the field POST 2 actually changed (pid_kp) did take effect -- proves this is a real "
              "per-field save, not a no-op");

    // POST 3: the deliberate-clear path -- explicit 0 for all three keys --
    // must still be able to erase the model on purpose.
    snprintf(body, sizeof(body),
             "thermo_count=1&relay_count=1&" MINIMAL_TIMING_PROFILE_BODY
             "&z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&z0_timingprofile=0&"
             "z0_cal=0&z0_kp=2&z0_ki=0&z0_kd=0&z0_ramp=0&z0_sanity=0&z0_mode=3&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&"
             "z0_k=0&z0_tau=0&z0_deadtime=0");
    run_zones_post(body);
    TEST_CHECK(s_test_ok_called && !s_test_err_called,
              "POST 3 (deliberate explicit-zero clear) must be accepted");
    memset(&req, 0, sizeof(req));
    err = zones_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "zones_get_handler must return ESP_OK");
    TEST_CHECK(strstr(s_last_resp_body, "\"model_k_dc\":0.0000") != NULL,
              "an explicit all-zero POST must still clear the model on purpose");
}

// docs/audits/zones_diag_endpoint_split_2026-09-14.md: the fields moved off
// GET /api/zones must still round-trip readably through GET /api/zones_diag
// after a real POST, same discipline as test_post_then_get_round_trips_new_
// fields above -- coupling_tau_c%u/coupling_dead_time_c%u have no POST field
// of their own (autotune_engine.c's finalize_fit() is their only writer,
// same as before the split), so this sets them directly on the live config
// the way that writer would, then confirms the real zones_diag_get_handler()
// reports them back exactly, indexed to line up with GET /api/zones' own
// "index" per zone.
static void test_zones_diag_get_handler_round_trips_moved_fields(void)
{
    TEST_SECTION("zones_diag_get_handler -- coupling_tau_c%u/coupling_dead_time_c%u/"
                 "model_fit_temp_c/model_fit_ambient_c round-trip (docs/audits/"
                 "zones_diag_endpoint_split_2026-09-14.md)");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = MAX31856_CHANNEL_COUNT;
    s_zones.cfg.zones[0].coupling_tau_s[1] = 111.0f;
    s_zones.cfg.zones[0].coupling_dead_time_s[1] = 22.0f;
    s_zones.cfg.zones[0].model_fit_temp_c = 950.5f;
    s_zones.cfg.zones[0].model_fit_ambient_c = 21.25f;
    s_zones.cfg.zones[1].coupling_tau_s[0] = 333.0f;

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = zones_diag_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "zones_diag_get_handler must return ESP_OK");
    TEST_CHECK(strstr(s_last_resp_body, "\"index\":0") != NULL,
              "zones_diag_get_handler emits a dense index per zone, same as GET /api/zones");
    TEST_CHECK(strstr(s_last_resp_body, "\"coupling_tau_c1\":111.0") != NULL,
              "zone 0's coupling_tau_c1 reports exactly what was set");
    TEST_CHECK(strstr(s_last_resp_body, "\"coupling_dead_time_c1\":22.0") != NULL,
              "zone 0's coupling_dead_time_c1 reports exactly what was set");
    TEST_CHECK(strstr(s_last_resp_body, "\"model_fit_temp_c\":950.50") != NULL,
              "zone 0's model_fit_temp_c reports exactly what was set");
    TEST_CHECK(strstr(s_last_resp_body, "\"model_fit_ambient_c\":21.25") != NULL,
              "zone 0's model_fit_ambient_c reports exactly what was set");
    TEST_CHECK(strstr(s_last_resp_body, "\"coupling_tau_c0\":333.0") != NULL,
              "zone 1's coupling_tau_c0 (a DIFFERENT cell, DIFFERENT zone) is not confused "
              "with zone 0's -- an index-transposed bug would fail this");
}

// docs/audits/zones_field_sourcing_and_generation_2026-09-14.md sec 2 (opus
// review, defect 2): /api/zones and /api/zones_diag are two separate
// requests with no shared snapshot; a config write landing between them can
// pair a NEW gain with an OLD fit operating point. zones_config_generation()
// is now emitted on BOTH responses so a client can detect that. This test
// confirms both handlers report the SAME value for the SAME underlying
// config generation, and that the value actually reflects a real write
// (moves after a setter call) -- a client comparing two counters that never
// moved could pass by construction, so the "before/after a real write"
// wired-through check matters as much as the top-level presence.
//
// Negative test: delete the "\"generation\":%u," APPEND fragment (and its
// config_generation/zones_config_generation() argument) from either handler
// -> this test's strstr()-based TEST_CHECK for that handler goes red (the
// literal "\"generation\":" substring disappears from the response).
static void test_get_and_diag_emit_matching_generation(void)
{
    TEST_SECTION("zones_get_handler/zones_diag_get_handler -- both emit the SAME "
                 "zones_config_generation() value (docs/audits/"
                 "zones_field_sourcing_and_generation_2026-09-14.md sec 2)");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = MAX31856_CHANNEL_COUNT;

    /* Bump the generation with a real setter (not a hand-poked struct field)
     * so this test also proves the emitted value tracks an actual config
     * write, not just a static default. */
    zones_config_set_model(0, 10.0f, 100.0f, 5.0f);
    uint32_t gen = zones_config_generation();

    char gen_needle[48];
    snprintf(gen_needle, sizeof(gen_needle), "\"generation\":%u,", (unsigned)gen);

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = zones_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "zones_get_handler must return ESP_OK");
    TEST_CHECK(strstr(s_last_resp_body, gen_needle) != NULL,
              "GET /api/zones emits the current generation as a top-level key");

    memset(&req, 0, sizeof(req));
    err = zones_diag_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "zones_diag_get_handler must return ESP_OK");
    TEST_CHECK(strstr(s_last_resp_body, gen_needle) != NULL,
              "GET /api/zones_diag emits the SAME generation value -- a client can "
              "compare the two to detect a config write landing between the two GETs");

    /* Bump again and confirm BOTH handlers move together -- proves this
     * isn't two independently-initialised counters that happen to start
     * equal (the exact "reset one side of a pair" shape this repo has hit
     * before). */
    zones_config_set_model(1, 20.0f, 200.0f, 8.0f);
    uint32_t gen2 = zones_config_generation();
    TEST_CHECK(gen2 > gen, "a second setter call bumps the generation again");
    char gen2_needle[48];
    snprintf(gen2_needle, sizeof(gen2_needle), "\"generation\":%u,", (unsigned)gen2);

    memset(&req, 0, sizeof(req));
    err = zones_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "zones_get_handler must return ESP_OK (2nd call)");
    TEST_CHECK(strstr(s_last_resp_body, gen2_needle) != NULL,
              "GET /api/zones reflects the bumped generation");

    memset(&req, 0, sizeof(req));
    err = zones_diag_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "zones_diag_get_handler must return ESP_OK (2nd call)");
    TEST_CHECK(strstr(s_last_resp_body, gen2_needle) != NULL,
              "GET /api/zones_diag reflects the SAME bumped generation");
}

// docs/audits/zones_get_autotune_baseline_exposure_2026-09-13.md: GET /api/
// zones never emitted autotune_baseline_k_dc even though POST accepts it
// (preserved-only, no z%u_ key of its own -- adaptive_tune.c is its sole
// writer) and it is persisted since ZONES_CFG_VERSION 26. Set it directly on
// the live config (the same way adaptive_tune.c's only writer would, since
// there is no POST field to drive it through parse_zone_fields()), then
// confirm the real zones_get_handler() reports it back exactly -- proving
// the ratchet-anchor value 97288659/36f88d62 depend on is now externally
// observable, not just internally trusted.
static void test_get_emits_autotune_baseline_k_dc(void)
{
    TEST_SECTION("zones_get_handler -- emits autotune_baseline_k_dc (docs/audits/"
                 "zones_get_autotune_baseline_exposure_2026-09-13.md)");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 1;
    s_zones.cfg.zones[0].autotune_baseline_k_dc = 12.5f;

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = zones_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "zones_get_handler must return ESP_OK");
    TEST_CHECK(strstr(s_last_resp_body, "\"autotune_baseline_k_dc\":12.5000") != NULL,
              "GET reports the stored autotune_baseline_k_dc exactly -- previously this key "
              "never appeared in the response at all");

    /* The sentinel case: 0 means "no baseline recorded yet"
     * (zones_config_accessors.h) and must be emitted RAW, not substituted or
     * omitted, same convention as every other 0-sentinel field on this
     * endpoint (hyst_c, coil_power_w, ease_off_window_mult, ...) -- a client
     * cannot tell "not recorded" from "recorded as exactly 0" any other way. */
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 1;
    err = zones_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "zones_get_handler must return ESP_OK for the sentinel case too");
    TEST_CHECK(strstr(s_last_resp_body, "\"autotune_baseline_k_dc\":0.0000") != NULL,
              "GET reports the unrecorded sentinel (0) verbatim, never a substituted value");

    /* Round-trip through the real POST path too: parse_zone_fields() has no
     * z%u_ key for this field, so a whole-page POST must PRESERVE whatever
     * is already stored (test_post_omitting_new_fields_preserves_stored_
     * values already covers parse_zone_fields() directly) and the GET
     * response after that POST must still show the preserved value -- this
     * is the actual end-to-end path an operator/tool would observe. */
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 1;
    s_zones.cfg.zones[0].autotune_baseline_k_dc = 33.75f;
    char body[900];
    snprintf(body, sizeof(body),
             "thermo_count=1&relay_count=1&" MINIMAL_TIMING_PROFILE_BODY
             "&z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&z0_timingprofile=0&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=0&z0_sanity=0&z0_mode=3&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=0&z0_minon=0&z0_minoff=0&"
             "z0_settings_source=255");
    run_zones_post(body);
    TEST_CHECK(s_test_ok_called && !s_test_err_called,
              "the whole-page POST (no autotune_baseline_k_dc key at all) must be accepted");
    err = zones_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "zones_get_handler must return ESP_OK after the round-trip POST");
    TEST_CHECK(strstr(s_last_resp_body, "\"autotune_baseline_k_dc\":33.7500") != NULL,
              "GET after an ordinary whole-page POST still reports the anchor PRESERVED, "
              "exactly like the pre-POST value -- this is the observable proof that "
              "36f88d62's zeroing fix actually holds end-to-end");
}

// GAP 2, docs/audits/observability_gaps_closed_2026-09-14.md: fuzzy_
// model_valid must track the SAME predicate the live control tick uses
// (pid_fuzzy_derive_bands(), profile_executor_pid_tick.c's resolve_fuzzy_
// bands()) against this zone's own persisted model_k_dc/model_tau_s -- a
// zone never autotuned (both 0, the documented sentinel) must read false,
// and a zone with a real fit must read true, independent of fuzzy_
// strength_pct (this field reports whether fuzzy COULD run, not whether it
// is configured to).
//
// Negative test: change the call site in zones_http_get.c to
// `bool fuzzy_model_valid = true;` (or delete the field from the APPEND
// call) -> the first TEST_CHECK below goes red.
static void test_get_emits_fuzzy_model_valid(void)
{
    TEST_SECTION("zones_get_handler -- emits fuzzy_model_valid, tracking pid_fuzzy_derive_"
                 "bands()'s own model_valid predicate (docs/audits/observability_gaps_closed_"
                 "2026-09-14.md GAP 2)");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 2;
    /* Zone 0: never autotuned -- model_k_dc/model_tau_s both 0, the
     * documented "no identified plant" sentinel. Configured for fuzzy
     * (strength 75%) but must read fuzzy_model_valid:false regardless --
     * this is exactly the case an operator previously had to infer from
     * model_k_dc alone. */
    s_zones.cfg.zones[0].fuzzy_strength_pct = 75.0f;
    /* Zone 1: a real fit. */
    s_zones.cfg.zones[1].model_k_dc = 42.0f;
    s_zones.cfg.zones[1].model_tau_s = 260.0f;
    s_zones.cfg.zones[1].fuzzy_strength_pct = 75.0f;

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = zones_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "zones_get_handler must return ESP_OK");
    TEST_CHECK(strstr(s_last_resp_body,
                       "\"index\":0,\"name\":\"\"") != NULL ||
               strstr(s_last_resp_body, "\"index\":0,") != NULL,
              "zone 0 present in the response");
    /* Locate each zone's own object rather than grep the whole body, so a
     * false-true swap between zones cannot be masked by the OTHER zone's
     * substring appearing first. */
    const char *zone0 = strstr(s_last_resp_body, "\"index\":0,");
    const char *zone1 = strstr(s_last_resp_body, "\"index\":1,");
    TEST_CHECK(zone0 != NULL && zone1 != NULL, "both zone objects found");
    if (zone0 && zone1) {
        const char *fmv0 = strstr(zone0, "\"fuzzy_model_valid\":");
        const char *fmv1 = strstr(zone1, "\"fuzzy_model_valid\":");
        TEST_CHECK(fmv0 != NULL && fmv0 < zone1,
                  "zone 0 emits fuzzy_model_valid within its own object");
        TEST_CHECK(fmv1 != NULL, "zone 1 emits fuzzy_model_valid");
        TEST_CHECK(fmv0 && strncmp(fmv0, "\"fuzzy_model_valid\":false", 25) == 0,
                  "zone 0 (no identified model, but fuzzy_strength_pct=75) reports "
                  "fuzzy_model_valid:false -- configured but inactive");
        TEST_CHECK(fmv1 && strncmp(fmv1, "\"fuzzy_model_valid\":true", 24) == 0,
                  "zone 1 (a real fit) reports fuzzy_model_valid:true");
    }
}

// docs/audits/zones_field_sourcing_and_generation_2026-09-14.md sec 6 (opus
// review, defect: "incidental, not structural"): fuzzy_model_valid used to
// read z->model_k_dc/model_tau_s DIRECTLY and call pid_fuzzy_derive_bands()
// itself, bypassing zone_model_at() -- the SAME function profile_executor_
// pid_tick.c's resolve_fuzzy_bands() calls. This test targets exactly the
// divergence that bypass caused: zone_model_at() -> zones_config_get_model()
// returns false for zone_index >= thermo_count, so a zone past thermo_count
// carrying a STALE non-zero model must read fuzzy_model_valid:false (matching
// what the control tick would see, since that zone is not controlled), not
// true (what a direct z->model_k_dc read would report). Before the fix this
// zone read true; after it, false.
//
// Negative test: change the call site in zones_http_get.c back to `bool
// fuzzy_model_valid = pid_fuzzy_derive_bands(z->model_k_dc, z->model_tau_s,
// NULL, NULL);` (the pre-fix form) -> this test's TEST_CHECK goes red.
static void test_get_fuzzy_model_valid_single_sourced_past_thermo_count(void)
{
    TEST_SECTION("zones_get_handler -- fuzzy_model_valid routes through zone_model_at() "
                 "(single-sourced with the control tick), not a direct model_k_dc/"
                 "model_tau_s read (docs/audits/zones_field_sourcing_and_generation_"
                 "2026-09-14.md sec 6)");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 2;
    /* Zone 2 is >= thermo_count (2) -- not a controlled zone -- but carries a
     * STALE non-zero model from before thermo_count was lowered (or a config
     * that was never cleared). zones_config_get_model() (and therefore the
     * control tick's zone_model_at() call) refuses any zone_index >=
     * thermo_count regardless of what is stored. */
    s_zones.cfg.zones[2].model_k_dc = 42.0f;
    s_zones.cfg.zones[2].model_tau_s = 260.0f;
    s_zones.cfg.zones[2].fuzzy_strength_pct = 75.0f;

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = zones_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "zones_get_handler must return ESP_OK");

    const char *zone2 = strstr(s_last_resp_body, "\"index\":2,");
    TEST_CHECK(zone2 != NULL, "zone 2 object found");
    if (zone2) {
        const char *fmv2 = strstr(zone2, "\"fuzzy_model_valid\":");
        TEST_CHECK(fmv2 && strncmp(fmv2, "\"fuzzy_model_valid\":false", 25) == 0,
                  "zone 2 (>= thermo_count, stale non-zero model) reports "
                  "fuzzy_model_valid:false, matching zone_model_at()'s thermo_count "
                  "guard -- a direct model_k_dc read would wrongly report true");
    }
}

// Opus review of 992f3954 (zones v22, progress_band_c), item 3: json_cap in
// zones_get_handler() (7360 bytes, heap_caps_malloc) has a hand-maintained
// comment chain of every bump's worst-case reasoning, but that chain stopped
// at ZONES_CFG_VERSION 13 -- five fields landed since then without a
// matching entry (coupling_diag_k_dc, ease_off_window_mult,
// approach_rate_cap_c_per_hr, error_band_c/rate_band_c_per_s, the
// settings_source_groups nested object, and now progress_band_c) with no
// check that the buffer still has room. Rather than re-deriving the
// worst-case byte count by hand (fragile, and already proven to drift),
// this test drives the REAL handler with every zone field pinned at its
// documented MAX bound (ZONE_*_MAX from zones_config_accessors.h/
// zones_config_json.h), all relay names and timing-profile names at their
// max length, and asserts the actual produced length stays under json_cap
// -- so a future field addition that finally exhausts the buffer fails
// loudly here instead of silently truncating a live board's GET response.
static void test_zones_get_handler_max_width_response_fits_json_cap(void)
{
    TEST_SECTION("zones_get_handler -- every zone/profile/relay field at its documented max width "
                 "must still fit within json_cap (Opus review of 992f3954, item 3)");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = MAX31856_CHANNEL_COUNT;
    s_zones.cfg.relay_count = KILN_IO_RELAY_COUNT;
    s_zones.cfg.max_simultaneous_relays = 255;
    s_zones.cfg.continue_on_zone_trip = true;
    s_zones.cfg.safety_tc_type = 255;
    s_zones.cfg.pc_link_abort_silence_ms = 600000.0f;

    for (uint8_t r = 0; r < KILN_IO_RELAY_COUNT; r++) {
        memset(s_relay_names.cfg.names[r], 'X', RELAY_NAME_MAX_LEN);
        s_relay_names.cfg.names[r][RELAY_NAME_MAX_LEN] = '\0';
    }

    s_zones.cfg.timing_profile_count = MAX31856_CHANNEL_COUNT;
    for (uint8_t p = 0; p < MAX31856_CHANNEL_COUNT; p++) {
        zone_timing_profile_t *tp = &s_zones.cfg.timing_profiles[p];
        memset(tp->name, 'P', TIMING_PROFILE_NAME_MAX_LEN);
        tp->name[TIMING_PROFILE_NAME_MAX_LEN] = '\0';
        tp->guard_progress_duty_min = 1.0f;
        tp->guard_progress_window_s = ZONE_GUARD_TIME_S_MAX;
        tp->guard_drift_hysteresis_c = ZONE_GUARD_MARGIN_C_MAX;
        tp->guard_frozen_eps_c = ZONE_GUARD_EPS_C_MAX;
        tp->guard_cross_zone_period_s = ZONE_GUARD_TIME_S_MAX;
        tp->bangbang_hysteresis_c = ZONE_GUARD_MARGIN_C_MAX;
        tp->cooling_limited_margin_c = ZONE_GUARD_MARGIN_C_MAX;
        tp->cooling_limited_hold_s = ZONE_GUARD_TIME_S_MAX;
        tp->ramp_lock_band_c = ZONE_GUARD_MARGIN_C_MAX;
    }

    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        zone_cfg_t *z = &s_zones.cfg.zones[i];
        memset(z->name, 'Z', ZONE_NAME_MAX_LEN);
        z->name[ZONE_NAME_MAX_LEN] = '\0';
        z->relay_mask = 255;
        z->thermo_mask = 255;
        z->cal_offset_c = ZONE_CAL_OFFSET_MAX_C;
        z->pid_kp = ZONE_PID_GAIN_MAX;
        z->pid_ki = ZONE_PID_GAIN_MAX;
        z->pid_kd = ZONE_PID_GAIN_MAX;
        z->max_ramp_c_per_hr = ZONE_MAX_RAMP_C_PER_HR_MAX;
        z->sanity_rate_c_per_min = ZONE_SANITY_RATE_MAX_C_PER_MIN;
        z->control_mode = 255;
        z->max_temp_c = ZONE_MAX_TEMP_C_MAX;
        z->min_temp_c = ZONE_MIN_TEMP_C_MAX;
        z->heater_window_ms = ZONE_HEATER_WINDOW_MS_MAX;
        z->heater_min_on_ms = ZONE_HEATER_MIN_ON_OFF_MS_MAX;
        z->heater_min_off_ms = ZONE_HEATER_MIN_ON_OFF_MS_MAX;
        z->guard_wrong_dir_window_s = ZONE_GUARD_TIME_S_MAX;
        z->guard_wrong_dir_rate_c_per_min = ZONE_GUARD_RATE_C_PER_MIN_MAX;
        z->guard_off_settle_s = ZONE_GUARD_TIME_S_MAX;
        z->guard_runaway_rate_c_per_min = ZONE_GUARD_RATE_C_PER_MIN_MAX;
        z->guard_runaway_margin_c = ZONE_GUARD_MARGIN_C_MAX;
        z->guard_drift_period_s = ZONE_GUARD_TIME_S_MAX;
        z->guard_sensor_fault_debounce_ticks = ZONE_GUARD_DEBOUNCE_TICKS_MAX;
        z->guard_frozen_window_s = ZONE_GUARD_TIME_S_MAX;
        z->cross_zone_max_delta_c = ZONE_CROSS_ZONE_DELTA_C_MAX;
        z->model_k_dc = ZONE_MODEL_K_MAX;
        z->model_tau_s = ZONE_MODEL_TIME_MAX_S;
        z->model_dead_time_s = ZONE_MODEL_TIME_MAX_S;
        z->tc_type = 255;
        z->ct_mask = 255;
        z->timing_profile = 255;
        z->relay_type = ZONE_RELAY_TYPE_MAX;
        z->fuzzy_strength_pct = ZONE_FUZZY_STRENGTH_PCT_MAX;
        /* fuzzy_model_valid (GAP 2, docs/audits/observability_gaps_closed_
         * 2026-09-14.md) is DERIVED from model_k_dc/model_tau_s at render
         * time (pid_fuzzy_derive_bands()), not its own stored field --
         * model_k_dc/model_tau_s are set to ZONE_MODEL_K_MAX/ZONE_MODEL_TIME_
         * MAX_S just below for THEIR OWN max-width coverage, and any values
         * that large are necessarily > 0.0f, so this renders "true" (4
         * chars) here. That is the true joint worst case, not an
         * under-measurement: forcing "false" (5 chars, the wider literal)
         * requires model_k_dc or model_tau_s to be non-positive, which
         * shrinks THEIR OWN rendered width by far more than one byte -- the
         * two fields cannot be independently maximised, and the combination
         * that maximises the total is this one.  */
        for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
            z->coupling_coeff[j] = ZONE_COUPLING_COEFF_MAX;
            z->coupling_tau_s[j] = ZONE_MODEL_TIME_MAX_S;
            z->coupling_dead_time_s[j] = ZONE_MODEL_TIME_MAX_S;
        }
        z->coupling_diag_k_dc = ZONE_MODEL_K_MAX;
        z->ease_off_window_mult = ZONE_EASE_OFF_WINDOW_MULT_MAX;
        z->approach_rate_cap_c_per_hr = ZONE_APPROACH_RATE_CAP_C_PER_HR_MAX;
        z->error_band_c = ZONE_ERROR_BAND_C_MAX;
        z->rate_band_c_per_s = ZONE_RATE_BAND_C_PER_S_MAX;
        z->progress_band_c = ZONE_PROGRESS_BAND_C_MAX;
        for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
            z->settings_source[g] = 255;
        }
        /* tuning_valid/settled/extrapolation_converged/tau_consistent render
         * as "true"/"false" -- "false" (5 chars) is the wider literal, so 0
         * (not 1) is the worst case for these four. */
        z->tuning_valid = 0;
        z->tuning_method = 255;
        z->tuning_rule = 255;
        z->tuning_settled = 0;
        z->tuning_extrapolation_converged = 0;
        z->tuning_tau_consistent = 0;
        z->tuning_baseline_c = ZONE_MAX_TEMP_C_MAX;
        z->tuning_step_ambient_c = ZONE_MAX_TEMP_C_MAX;
        z->tuning_raw_rise_c = ZONE_MAX_TEMP_C_MAX;
        z->tuning_rise_inf_c = ZONE_MAX_TEMP_C_MAX;
        z->tuning_seq = 0xFFFFFFFFu;
        z->adaptive_tune_enabled = 255;
        /* docs/ON_OFF_ZONE_PLAN.md step 6 (ZONES_CFG_VERSION 22->23):
         * zone_type/failsafe_state render as small integers/booleans
         * ("false" is the wider literal, same reasoning as the tuning_*
         * booleans above), hyst_c/min_on_s/min_off_s at their documented
         * max widths. */
        z->zone_type = (uint8_t)ZONE_TYPE_ON_OFF;
        z->failsafe_state = 0;
        z->hyst_c = ZONE_HYST_C_MAX;
        z->min_on_s = ZONE_MIN_ON_OFF_S_MAX;
        z->min_off_s = ZONE_MIN_ON_OFF_S_MAX;
        /* ZONES_CFG_VERSION 23->24's model_fit_temp_c/model_fit_ambient_c,
         * emitted at %.2f -- widest render is the same 7 characters for the
         * max temperature and for the -273.15 UNKNOWN sentinel. */
        z->model_fit_temp_c = ZONE_MAX_TEMP_C_MAX;
        z->model_fit_ambient_c = ZONE_MAX_TEMP_C_MAX;
        /* ZONES_CFG_VERSION 25->26's autotune_baseline_k_dc, now emitted by
         * zones_get_handler() (docs/audits/zones_get_autotune_baseline_
         * exposure_2026-09-13.md) -- pinned at its documented max, same
         * discipline as model_k_dc/coupling_diag_k_dc above. */
        z->autotune_baseline_k_dc = ZONE_AUTOTUNE_K_DC_MAX;
    }

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    s_last_resp_body[0] = '\0';
    s_last_resp_len = 0;
    esp_err_t err = zones_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "zones_get_handler must return ESP_OK even at max field width");
    /* json_cap itself (zones_http_get.c, zones_get_handler()) -- kept in
     * sync by hand, same discipline as this file's own MINIMAL_TIMING_
     * PROFILE_BODY macro; if that constant changes, update this literal
     * alongside it. */
    const size_t json_cap = 7360;
    /* This message is built BEFORE either check below and used by both,
     * because the realistic overflow shape is the handler's OWN
     * APPEND-macro truncation firing mid-render (zones_http_get.c never lets
     * `json` grow past json_cap, so it goes to its `truncated:` label and
     * sends a short error body instead) -- s_last_resp_len then reads far
     * UNDER json_cap despite the real content not fitting at all, so the
     * length check below can never be the one that actually catches this in
     * practice. Whichever check fires, the failure now names the measured
     * headroom and points at the plan doc, rather than leaving the next
     * person who adds a field to hit a bare "must fit"/"must not truncate"
     * failure with no idea what to do about it
     * (docs/audits/zones_json_headroom_plan_2026-09-14.md, task 5): as of
     * that doc only 161 bytes of headroom remained in this 7360-byte
     * json_cap. docs/audits/zones_diag_endpoint_split_2026-09-14.md then
     * implemented that plan's recommended split -- moving coupling_tau_c%u/
     * coupling_dead_time_c%u and model_fit_temp_c/model_fit_ambient_c to the
     * new GET /api/zones_diag route (test_zones_diag_get_handler_max_width_
     * response_fits_json_cap() below covers ITS json_cap the same way) --
     * which recovered headroom to 854 bytes. `fuzzy_model_valid` (GAP 2,
     * docs/audits/observability_gaps_closed_2026-09-14.md) then spent 75 of
     * those bytes (25 bytes/zone x MAX31856_CHANNEL_COUNT=3 for
     * `"fuzzy_model_valid":true,`), leaving 779 bytes, confirmed by this
     * test. The buffer must still NOT simply be enlarged (this repo has two
     * documented panics from oversized httpd-worker stack locals and a
     * standing rule against growing httpd buffers) -- read the plan and the
     * split doc for ranked, consumer-checked savings and structural
     * alternatives before touching json_cap. */
    char headroom_msg[500];
    snprintf(headroom_msg, sizeof(headroom_msg),
             "max-width GET /api/zones must fit inside json_cap without hitting the "
             "handler's own truncation path (rendered %zu bytes; json_cap is %zu bytes; "
             "measured headroom after fuzzy_model_valid (2026-09-14) was 779 bytes; this "
             "attempt %s). "
             "Do NOT enlarge json_cap (two documented httpd-worker-stack-local panics + a "
             "standing rule against growing httpd buffers) -- see "
             "docs/audits/zones_json_headroom_plan_2026-09-14.md and "
             "docs/audits/zones_diag_endpoint_split_2026-09-14.md for ranked, "
             "consumer-checked savings and structural alternatives before adding another "
             "field here.",
             s_last_resp_len, json_cap,
             (strstr(s_last_resp_body, "did not fit") != NULL)
                 ? "overflowed mid-render and was truncated by the handler itself"
                 : (s_last_resp_len < json_cap ? "still fit, but see the headroom above" : "exceeded json_cap outright"));
    /* zones_get_handler()'s own truncated: label sends a SHORT error body
     * ("...did not fit...firmware sizing bug...") on overflow, so a bare
     * s_last_resp_len < json_cap check would pass even in the failure case
     * (the short error string is always well under json_cap). Check for the
     * absence of that marker directly, so an actual overflow fails loudly
     * here instead of being masked by the small length of its own error
     * response. */
    TEST_CHECK(strstr(s_last_resp_body, "did not fit") == NULL, headroom_msg);
    TEST_CHECK(strstr(s_last_resp_body, "\"zones\":[{") != NULL,
              "max-width GET /api/zones must actually render zone content, not an error body");
    TEST_CHECK(s_last_resp_len > 0 && s_last_resp_len < json_cap, headroom_msg);
    printf("  GET /api/zones max-width render: %zu bytes, against json_cap=%zu -- measured "
          "headroom = %zd bytes\n",
          s_last_resp_len, json_cap, (ptrdiff_t)json_cap - (ptrdiff_t)s_last_resp_len);
}

// docs/audits/zones_diag_endpoint_split_2026-09-14.md: GET /api/zones_diag is
// a SECOND httpd handler with the identical buffer-sizing hazard class as
// GET /api/zones above (a fixed heap_caps_malloc() json_cap that must fit
// the true worst case or fall into its own truncated: path) -- it gets the
// same max-width discipline, not a smaller one just because the buffer is
// smaller. Every field this handler emits (coupling_tau_c%u/coupling_dead_
// time_c%u/model_fit_temp_c/model_fit_ambient_c) is pinned at its documented
// MAX bound, same ZONE_*_MAX constants the /api/zones sibling test uses.
static void test_zones_diag_get_handler_max_width_response_fits_json_cap(void)
{
    TEST_SECTION("zones_diag_get_handler -- every field at its documented max width must still "
                 "fit within its own json_cap (docs/audits/zones_diag_endpoint_split_2026-09-14.md)");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = MAX31856_CHANNEL_COUNT;
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        zone_cfg_t *z = &s_zones.cfg.zones[i];
        for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
            z->coupling_tau_s[j] = ZONE_MODEL_TIME_MAX_S;
            z->coupling_dead_time_s[j] = ZONE_MODEL_TIME_MAX_S;
        }
        z->model_fit_temp_c = ZONE_MAX_TEMP_C_MAX;
        z->model_fit_ambient_c = ZONE_MAX_TEMP_C_MAX;
    }

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    s_last_resp_body[0] = '\0';
    s_last_resp_len = 0;
    esp_err_t err = zones_diag_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "zones_diag_get_handler must return ESP_OK even at max field width");
    /* json_cap itself (zones_http_get.c, zones_diag_get_handler()) -- kept in
     * sync by hand, same discipline as the /api/zones sibling test above. */
    const size_t json_cap = 1024;
    char headroom_msg[500];
    snprintf(headroom_msg, sizeof(headroom_msg),
             "max-width GET /api/zones_diag must fit inside its json_cap without hitting the "
             "handler's own truncation path (rendered %zu bytes; json_cap is %zu bytes; this "
             "attempt %s). This is a NEW, small heap buffer (docs/audits/"
             "zones_diag_endpoint_split_2026-09-14.md) -- do not grow it past what a real "
             "measured worst case requires, same buffer-discipline reasoning as GET /api/zones' "
             "own json_cap (two documented httpd-worker-stack-local panics + a standing rule "
             "against growing httpd buffers casually) -- see "
             "docs/audits/zones_diag_endpoint_split_2026-09-14.md before touching this json_cap.",
             s_last_resp_len, json_cap,
             (strstr(s_last_resp_body, "did not fit") != NULL)
                 ? "overflowed mid-render and was truncated by the handler itself"
                 : (s_last_resp_len < json_cap ? "still fit, but see the headroom above" : "exceeded json_cap outright"));
    TEST_CHECK(strstr(s_last_resp_body, "did not fit") == NULL, headroom_msg);
    TEST_CHECK(strstr(s_last_resp_body, "\"zones\":[{") != NULL,
              "max-width GET /api/zones_diag must actually render zone content, not an error body");
    TEST_CHECK(s_last_resp_len > 0 && s_last_resp_len < json_cap, headroom_msg);
    printf("  GET /api/zones_diag max-width render: %zu bytes, against json_cap=%zu -- measured "
          "headroom = %zd bytes\n",
          s_last_resp_len, json_cap, (ptrdiff_t)json_cap - (ptrdiff_t)s_last_resp_len);
}

// tuning_rec_body_len() (2026-09-02 host-link fix, commit 333dd4e): the
// real tuning_recommendations_json_start/_end symbols are ESP-IDF
// EMBED_FILES symbols this host build cannot reproduce (see this file's
// placeholder definitions above), so this exercises the extracted length
// arithmetic directly with a real, deliberately-adjacent synthetic buffer --
// the exact case the handler's own comment documents: EMBED_TXTFILES
// appends a NUL after the file content and places _end past it, so the
// reported length must be (end - start) - 1, never the bare difference.
static void test_tuning_rec_body_len_strips_the_idf_appended_nul(void)
{
    TEST_SECTION("tuning_rec_body_len -- EMBED_TXTFILES trailing-NUL arithmetic");

    // Ordinary case: content "abc" (3 bytes) + IDF's appended NUL (1 byte) =
    // a 4-byte buffer, exactly like tuning_recommendations_json's real
    // start/end pair.
    static const uint8_t buf[] = { 'a', 'b', 'c', '\0' };
    size_t n = tuning_rec_body_len(buf, buf + sizeof(buf));
    TEST_CHECK(n == 3, "3 content bytes + 1 appended NUL must report length 3, not 4");

    // Degenerate: start == end (diff 0) must floor at 0, not underflow a
    // size_t to SIZE_MAX -- the `raw > 0 ? raw - 1 : 0` guard this tests.
    n = tuning_rec_body_len(buf, buf);
    TEST_CHECK(n == 0, "start == end must report length 0, not underflow");

    // Degenerate: a 1-byte buffer (just the appended NUL, empty content)
    // must also report 0, the same floor as start == end.
    n = tuning_rec_body_len(buf, buf + 1);
    TEST_CHECK(n == 0, "a 1-byte (NUL-only) buffer must report length 0");
}

// Coordinator review, 2026-08-31 httpd_worker stack-overflow fix:
// zones_get_handler()'s json[5760] moved off the stack and onto
// heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) -- the largest
// single stack buffer on the whole httpd_worker task's request path (>70%
// of its 8192-byte stack by itself). This is the ONE handler in this
// codebase's audit that is host-testable calling the REAL production
// function (dashboard_http.c's equivalent handlers cannot compile on the
// host at all -- see dashboard_json.h's own note), so it is the test that
// actually proves the heap-allocation pattern works end to end: a normal
// call still renders real config, and a simulated out-of-memory (via this
// file's heap_caps_malloc_test_set_fail() stub hook) returns a clean 500
// with a diagnosable body instead of crashing on a NULL deref -- the exact
// property "keep the NULL check, clean 500 on failure" asks for, previously
// completely unexercised (the prior heap-conversion test only re-implemented
// a fragment of the render inline and never called a handler or exercised
// the failure path at all).
static void test_zones_get_handler_malloc_failure_returns_clean_500(void)
{
    TEST_SECTION("zones_get_handler -- heap_caps_malloc() failure must return a clean 500 with a "
                 "diagnosable body, not crash");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 1;
    s_zones.cfg.relay_count = 1;

    test_post_hooks_reset();
    s_last_resp_body[0] = '\0';
    s_last_resp_len = 0;

    heap_caps_malloc_test_set_fail(true);
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = zones_get_handler(&req);
    heap_caps_malloc_test_set_fail(false); /* restore immediately, before any other check/test can run */

    TEST_CHECK(err == ESP_OK, "zones_get_handler must still return ESP_OK (httpd_resp_sendstr succeeded) "
              "even when its own response buffer could not be allocated");
    TEST_CHECK(s_test_ok_called, "the malloc-failure path must actually call httpd_resp_sendstr, not "
              "silently return without sending anything");
    TEST_CHECK(strstr(s_last_resp_body, "\"ok\":false") != NULL,
              "the 500 body must report ok:false, not a bare crash or an empty response");
    TEST_CHECK(strstr(s_last_resp_body, "out of memory") != NULL,
              "the 500 body must say WHY -- out of memory, not a generic failure -- so this is "
              "diagnosable from the wire instead of looking like a hang");
}

// Sanity companion to the failure test above: with the fail hook OFF (the
// normal case, and every other test in this file), zones_get_handler must
// still render real config through the now-heap-allocated buffer, exactly
// as it did on the stack -- proves the heap move didn't break the success
// path either.
static void test_zones_get_handler_succeeds_when_malloc_does_not_fail(void)
{
    TEST_SECTION("zones_get_handler -- with heap_caps_malloc() NOT failing, the handler still renders "
                 "real config through its now-heap-allocated buffer");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 1;
    s_zones.cfg.relay_count = 1;
    strncpy(s_zones.cfg.zones[0].name, "Kiln Zone", ZONE_NAME_MAX_LEN - 1);

    heap_caps_malloc_test_set_fail(false);
    s_last_resp_body[0] = '\0';
    s_last_resp_len = 0;
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = zones_get_handler(&req);

    TEST_CHECK(err == ESP_OK, "zones_get_handler must return ESP_OK on a normal call");
    TEST_CHECK(strstr(s_last_resp_body, "\"thermo_count\":1") != NULL,
              "the heap-allocated buffer must render real config, same as the stack version did");
    TEST_CHECK(strstr(s_last_resp_body, "Kiln Zone") != NULL,
              "a real zone name set just above must actually appear in the rendered JSON");
}

// ---------------------------------------------------------------------------
// zones_config_set_fuzzy_strength_pct / zones_config_get/set_coupling /
// zones_config_get/set_settings_source -- new accessors backup_http.c's
// import needs, added because zones_http_parse_zone_fields() previously was the only
// door onto these four fields. Same nvs_test_enable discipline as every other
// setter test in this file: the setter writes RAM immediately either way, but
// its return value (and thus TEST_CHECK) depends on nvs_save() succeeding.
// ---------------------------------------------------------------------------

static void test_fuzzy_strength_pct_setter_round_trip_and_bounds(void)
{
    TEST_SECTION("zones_config_set/get_fuzzy_strength_pct -- accepts in-range, refuses out-of-range");
    nvs_test_enable(true);
    nvs_test_clear();
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 1;

    TEST_CHECK(zones_config_set_fuzzy_strength_pct(0, 55.5f), "55.5 is in range 0-100");
    float out = -1.0f;
    TEST_CHECK(zones_config_get_fuzzy_strength_pct(0, &out), "getter succeeds");
    TEST_CHECK_NEAR(out, 55.5f, 1e-6, "the exact value set comes back out");

    TEST_CHECK(!zones_config_set_fuzzy_strength_pct(0, 100.0001f), "just over 100 is refused");
    TEST_CHECK(!zones_config_set_fuzzy_strength_pct(0, -0.0001f), "just under 0 is refused");
    TEST_CHECK(zones_config_get_fuzzy_strength_pct(0, &out) && out == 55.5f,
              "a refused setter call must not have changed the stored value (refuse, not clamp)");

    TEST_CHECK(!zones_config_set_fuzzy_strength_pct(1, 10.0f), "zone_index >= thermo_count is refused");

    nvs_test_enable(false);
    nvs_test_clear();
}

// 2026-08-30 (ZONES_CFG_VERSION 10->11): row-based, not a bundled pair --
// see zones_config_get/set_coupling()'s own doc comment (zones_http.h).
// THE test the whole 10->11 bump exists to make possible: two different
// off-diagonal cells of the SAME row hold distinct, asymmetric values
// simultaneously -- the bench-measured c(1->0)=10.887 / c(1->2)=3.332 pair
// this task's own brief cites as the reason a single (coeff, neighbor) pair
// could never represent this kiln's real coupling.
static void test_coupling_row_whole_setter_round_trip_and_bounds(void)
{
    TEST_SECTION("zones_config_set/get_coupling -- whole row, diagonal must stay 0, "
                 "multi-neighbor asymmetric values survive together");
    nvs_test_enable(true);
    nvs_test_clear();
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;

    // The exact bench-measured asymmetric pair this whole change exists for.
    float row[MAX31856_CHANNEL_COUNT] = {10.887f, 0.0f, 3.332f};
    TEST_CHECK(zones_config_set_coupling(1, row), "zone 1's asymmetric two-neighbor row is accepted");
    float out_row[MAX31856_CHANNEL_COUNT] = {-1.0f, -1.0f, -1.0f};
    TEST_CHECK(zones_config_get_coupling(1, out_row), "getter succeeds");
    TEST_CHECK_NEAR(out_row[0], 10.887f, 1e-6, "c(1->0) comes back exactly");
    TEST_CHECK_NEAR(out_row[2], 3.332f, 1e-6, "c(1->2) comes back exactly, DISTINCT from c(1->0)");
    TEST_CHECK(out_row[0] != out_row[2], "the whole point: two neighbors of one zone hold different values");
    TEST_CHECK_NEAR(out_row[1], 0.0f, 1e-6, "the diagonal (zone 1's own cell) reads back as 0");

    // A nonzero diagonal is refused outright, the whole row left untouched.
    float bad_diag[MAX31856_CHANNEL_COUNT] = {10.887f, 1.0f, 3.332f};
    TEST_CHECK(!zones_config_set_coupling(1, bad_diag), "a nonzero diagonal cell is refused");

    // An out-of-range off-diagonal cell is refused outright too.
    float bad_range[MAX31856_CHANNEL_COUNT] = {10.887f, 0.0f, ZONE_COUPLING_COEFF_MAX + 1.0f};
    TEST_CHECK(!zones_config_set_coupling(1, bad_range), "an off-diagonal cell above ZONE_COUPLING_COEFF_MAX is refused");

    TEST_CHECK(zones_config_get_coupling(1, out_row) && out_row[0] == 10.887f && out_row[2] == 3.332f,
              "every refused whole-row call above left the previously-stored row untouched (refuse, not clamp)");

    nvs_test_enable(false);
    nvs_test_clear();
}

// docs/ON_OFF_ZONE_PLAN.md sec 1 "belt and braces": an on/off zone's
// coupling row AND column must both read zero, at every read, regardless of
// what is actually stored -- and a caller may not write a nonzero cell
// against an on/off zone's row or column either.
static void test_coupling_zeroed_for_on_off_zone_row_and_column(void)
{
    TEST_SECTION("zones_config_get/set_coupling -- an on/off zone's row AND column are forced to "
                 "zero, defense at both read and write time");
    nvs_test_enable(true);
    nvs_test_clear();
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;

    // Zone 1 has real, previously-measured coupling to zones 0 and 2.
    float row1[MAX31856_CHANNEL_COUNT] = {10.887f, 0.0f, 3.332f};
    TEST_CHECK(zones_config_set_coupling(1, row1), "zone 1's row is accepted while still a HEATER");
    // Zone 0 also names zone 1 as a neighbor (the OTHER side of zone 1's row).
    float row0[MAX31856_CHANNEL_COUNT] = {0.0f, 4.2f, 0.0f};
    TEST_CHECK(zones_config_set_coupling(0, row0), "zone 0's row (naming zone 1 as a neighbor) is accepted");

    // Now make zone 1 an on/off device.
    TEST_CHECK(zones_config_set_zone_type(1, ZONE_TYPE_ON_OFF), "zone 1 becomes ON_OFF");

    // ROW: zone 1's own row must now read all-zero, even though the raw
    // stored bytes still hold 10.887/3.332 -- "no neighbour's heat is
    // corrected for on this zone".
    float out_row[MAX31856_CHANNEL_COUNT] = {-1.0f, -1.0f, -1.0f};
    TEST_CHECK(zones_config_get_coupling(1, out_row), "getter still succeeds");
    TEST_CHECK_NEAR(out_row[0], 0.0f, 1e-9, "zone 1's row, cell 0, reads zero now that zone 1 is on/off");
    TEST_CHECK_NEAR(out_row[2], 0.0f, 1e-9, "zone 1's row, cell 2, reads zero now that zone 1 is on/off");

    // COLUMN: zone 0 (still a HEATER) must now see its OWN cell pointed at
    // zone 1 read as zero too -- "this zone injects no heat into anyone".
    float out_row0[MAX31856_CHANNEL_COUNT] = {-1.0f, -1.0f, -1.0f};
    TEST_CHECK(zones_config_get_coupling(0, out_row0), "getter for zone 0 still succeeds");
    TEST_CHECK_NEAR(out_row0[1], 0.0f, 1e-9,
                    "zone 0's cell pointed at on/off zone 1 reads zero -- a fan injects no heat");

    // WRITE side: a caller may not write a nonzero cell against zone 1's
    // row, nor a nonzero cell pointed at zone 1 from another zone's row.
    float bad_row1[MAX31856_CHANNEL_COUNT] = {1.0f, 0.0f, 0.0f};
    TEST_CHECK(!zones_config_set_coupling(1, bad_row1), "a nonzero row for the on/off zone itself is refused");
    float bad_row0[MAX31856_CHANNEL_COUNT] = {0.0f, 9.9f, 0.0f};
    TEST_CHECK(!zones_config_set_coupling(0, bad_row0), "a nonzero cell pointed at the on/off zone is refused");

    nvs_test_enable(false);
    nvs_test_clear();
}

// The single-cell setter autotune_engine.c's finalize_fit() uses to persist
// one neighbor's measured coefficient without disturbing the others.
//
// Storage convention (2026-08-31 correction -- these two bench numbers were
// previously both placed in zone 1's row, which is backwards): row index =
// the AFFECTED zone (the one whose temperature the coefficient describes the
// response of), column index = the STEPPED zone (the one whose heater was
// perturbed to produce the measurement). The bench pair this file cites
// elsewhere -- stepping zone 1's heater moves zone 0 by 10.887 and zone 2 by
// 3.332 -- therefore lands in TWO DIFFERENT rows, both at column 1:
// coupling_coeff[0][1] = 10.887 and coupling_coeff[2][1] = 3.332. It never
// belonged in zone 1's own row at all.
static void test_coupling_single_cell_setter_preserves_other_cells(void)
{
    TEST_SECTION("zones_config_set_coupling_cell -- updates ONE cell, leaves other rows alone, "
                 "diagonal write of 0 is a harmless no-op, nonzero diagonal is refused");
    nvs_test_enable(true);
    nvs_test_clear();
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;

    // row=affected, column=stepped: zone 0's response to zone 1's step, and
    // zone 2's response to that SAME step -- two different rows, same column.
    TEST_CHECK(zones_config_set_coupling_cell(0, 1, 10.887f, 42.0f, 6.0f),
              "zone 0's response to zone 1's step is set");
    TEST_CHECK(zones_config_set_coupling_cell(2, 1, 3.332f, 18.0f, 2.5f),
              "zone 2's response to zone 1's step is set separately");
    float row0[MAX31856_CHANNEL_COUNT] = {0};
    float row2[MAX31856_CHANNEL_COUNT] = {0};
    float tau0[MAX31856_CHANNEL_COUNT] = {0};
    float dead0[MAX31856_CHANNEL_COUNT] = {0};
    TEST_CHECK(zones_config_get_coupling(0, row0), "zone 0's getter succeeds");
    TEST_CHECK(zones_config_get_coupling(2, row2), "zone 2's getter succeeds");
    TEST_CHECK_NEAR(row0[1], 10.887f, 1e-6, "zone 0's cell survived zone 2's write");
    TEST_CHECK_NEAR(row2[1], 3.332f, 1e-6, "and zone 2's cell is exactly what was set, in its OWN row");
    // ZONES_CFG_VERSION 11->12: tau_s/dead_time_s ride alongside coeff.
    TEST_CHECK(zones_config_get_coupling_tau(0, tau0), "zone 0's tau getter succeeds");
    TEST_CHECK(zones_config_get_coupling_dead_time(0, dead0), "zone 0's dead-time getter succeeds");
    TEST_CHECK_NEAR(tau0[1], 42.0f, 1e-6, "zone 0's coupling_tau_s[1] is exactly what was set");
    TEST_CHECK_NEAR(dead0[1], 6.0f, 1e-6, "zone 0's coupling_dead_time_s[1] is exactly what was set");

    TEST_CHECK(zones_config_set_coupling_cell(0, 0, 0.0f, 0.0f, 0.0f),
              "writing the diagonal to exactly 0 is accepted");
    TEST_CHECK(!zones_config_set_coupling_cell(0, 0, 5.0f, 0.0f, 0.0f), "writing a NONZERO diagonal is refused");
    TEST_CHECK(zones_config_get_coupling(0, row0) && row0[1] == 10.887f,
              "the refused diagonal write left zone 0's other cell untouched");
    TEST_CHECK(zones_config_get_coupling(2, row2) && row2[1] == 3.332f,
              "zone 0's diagonal write did not cross into zone 2's row");

    TEST_CHECK(!zones_config_set_coupling_cell(0, 1, ZONE_COUPLING_COEFF_MAX + 1.0f, 1.0f, 1.0f),
              "a single cell above ZONE_COUPLING_COEFF_MAX is refused");
    TEST_CHECK(zones_config_get_coupling(0, row0) && row0[1] == 10.887f,
              "the refused cell write did not clamp or corrupt the previously-good value");

    // ZONES_CFG_VERSION 11->12: an in-range coeff paired with an
    // out-of-range tau_s must refuse the WHOLE cell -- coeff must NOT land
    // while tau_s/dead_time_s are silently dropped (the all-or-nothing rule
    // zones_config_set_coupling_cell()'s own header comment documents).
    TEST_CHECK(!zones_config_set_coupling_cell(0, 1, 99.0f, ZONE_MODEL_TIME_MAX_S + 1.0f, 1.0f),
              "an in-range coeff with an out-of-range tau_s is refused entirely");
    TEST_CHECK(zones_config_get_coupling(0, row0) && row0[1] == 10.887f,
              "the all-or-nothing refusal left coeff at its previous value, not the rejected 99.0");
    TEST_CHECK(zones_config_get_coupling_tau(0, tau0) && tau0[1] == 42.0f,
              "the all-or-nothing refusal left tau_s at its previous value too");

    nvs_test_enable(false);
    nvs_test_clear();
}

// Bounded search: find `needle` inside the JSON object for zone `zone_index`
// within a GET /api/zones response, never past the following zone's object
// (or the end of the "zones" array) -- so a value that legitimately appears
// in a NEIGHBOR zone's object can't produce a false match for this one.
static bool zone_json_field_present(const char *body, uint8_t zone_index, const char *needle)
{
    const char *zones_arr = strstr(body, "\"zones\":[");
    if (!zones_arr) return false;
    char marker[24];
    snprintf(marker, sizeof(marker), "\"index\":%u,", (unsigned)zone_index);
    const char *start = strstr(zones_arr, marker);
    if (!start) return false;
    char next_marker[24];
    snprintf(next_marker, sizeof(next_marker), "\"index\":%u,", (unsigned)zone_index + 1);
    const char *end = strstr(start, next_marker);
    const char *found = strstr(start, needle);
    if (!found) return false;
    if (end && found >= end) return false;
    return true;
}

// PID_EXPANSION_PLAN.md section 3.2's re-solved matrix (adopted
// 2026-09-02), applied here in its storage orientation
// coupling_coeff[affected][stepped]:
//
//     [[38.13, 27.32, 21.72],
//      [14.30, 35.90, 22.15],
//      [ 8.33, 12.42, 35.32]]
//
// This matrix is DELIBERATELY, strongly asymmetric off-diagonal (z1's step
// raises z0 by ~22C; z0's step raises z1 by only ~9C) specifically so a
// transposed apply is easy to catch here rather than on hardware -- see
// zones_http.h's coupling_coeff doc comment ("row i is the affected zone,
// column j is the stepped zone. Do not transpose").
//
// REVIEW 2026-09-02: an earlier version of this test set every row through
// zones_config_set_coupling() and read it back through
// zones_config_get_coupling() ONLY. That is orientation-BLIND by
// construction: if the setter's storage location and the getter's read
// location were transposed the SAME way (e.g. both indexed by the stepped
// zone instead of the affected one), the pair would still round-trip
// correctly with each other while every OTHER consumer of the same storage
// -- the GET /api/zones JSON, zone_coupling_solve.c, autotune_engine.c's
// coupling-aware feedforward -- would silently read the wrong cell. A round
// trip through one matched getter/setter pair can never rule that out.
//
// This version drives the matrix in through zones_config_set_coupling() (the
// same setter finalize_fit() and the z%u_coupling_c%u POST path both funnel
// through) and reads it back through zones_get_handler() -- the REAL
// production GET /api/zones handler, which serializes coupling_coeff[]
// straight out of zone_cfg_t with no dependency on zones_config_get_coupling()
// at all (see zones_http_handlers.c's "coupling_c%u" emission). That is a
// genuinely independent second view of the same storage: a setter bug that
// lands cell [affected][stepped] in the wrong zone's array shows up here
// even if a same-shaped getter bug would have hidden it from a get/set-only
// round trip.
static void test_coupling_matrix_2026_09_02_adopted_orientation_not_transposed(void)
{
    TEST_SECTION("zones_config_set_coupling -> zones_get_handler -- the adopted 2026-09-02 matrix lands in "
                 "[affected][stepped] storage orientation, not transposed (verified through the independent "
                 "GET /api/zones wire path, not the matching getter)");
    nvs_test_enable(true);
    nvs_test_clear();
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;

    // matrix[affected][stepped], diagonal forced to 0 by the setter's own rule.
    const float matrix[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT] = {
        {0.0f,   27.32f, 21.72f},
        {14.30f, 0.0f,   22.15f},
        {8.33f,  12.42f, 0.0f},
    };

    for (uint8_t affected = 0; affected < MAX31856_CHANNEL_COUNT; affected++) {
        TEST_CHECK(zones_config_set_coupling(affected, matrix[affected]),
                  "row `affected` is accepted");
    }

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    s_last_resp_body[0] = '\0';
    s_last_resp_len = 0;
    esp_err_t err = zones_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "zones_get_handler must return ESP_OK");

    for (uint8_t affected = 0; affected < MAX31856_CHANNEL_COUNT; affected++) {
        for (uint8_t stepped = 0; stepped < MAX31856_CHANNEL_COUNT; stepped++) {
            char needle[40];
            snprintf(needle, sizeof(needle), "\"coupling_c%u\":%.4f", (unsigned)stepped,
                     (double)matrix[affected][stepped]);
            char msg[192];
            snprintf(msg, sizeof(msg),
                     "GET /api/zones's zone %u must report coupling_c%u == the adopted matrix's "
                     "[affected=%u][stepped=%u] cell (%.4f), NOT its transpose [stepped][affected] -- this "
                     "reads the storage independently of zones_config_get_coupling()",
                     affected, stepped, affected, stepped, (double)matrix[affected][stepped]);
            TEST_CHECK(zone_json_field_present(s_last_resp_body, affected, needle), msg);
        }
    }

    // The asymmetry itself, called out explicitly: z1's step on z0 (14.30)
    // must not be confused with z0's step on z1 (27.32) -- a transposed
    // apply would swap exactly this pair and still "look plausible" (both
    // are positive, both in range) without this check.
    TEST_CHECK(zone_json_field_present(s_last_resp_body, 0, "\"coupling_c1\":27.3200"),
              "zone 0 (affected) reports coupling_c1 (stepped=1) as 27.32, z1's step raising z0");
    TEST_CHECK(zone_json_field_present(s_last_resp_body, 1, "\"coupling_c0\":14.3000"),
              "zone 1 (affected) reports coupling_c0 (stepped=0) as only 14.30, z0's step raising z1");

    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_settings_source_setter_round_trip_and_bounds(void)
{
    TEST_SECTION("zones_config_set/get_settings_source -- CUSTOM or a real other zone, self-reference refused");
    nvs_test_enable(true);
    nvs_test_clear();
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;
    /* Real decoded configs never leave an in-use zone's settings_source at
     * raw 0 unintentionally (convert_zone_v9() etc. explicitly seed
     * ZONE_SETTINGS_SOURCE_CUSTOM, "NEVER 0" -- see that function's own
     * comment); memset(0) above is a test-only shortcut that leaves zones 1
     * and 2 looking like they explicitly link to zone 0, which is not a
     * state real firmware ever produces. Seed them CUSTOM here so this test
     * exercises the setter against a realistic starting config, not an
     * artifact of the test harness's own zeroing. */
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
            s_zones.cfg.zones[z].settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
        }
    }

    TEST_CHECK(zones_config_set_settings_source(0, SRC_GROUP_LIMITS, 1), "zone 0 copying zone 1 is legal");
    uint8_t out = 0;
    TEST_CHECK(zones_config_get_settings_source(0, SRC_GROUP_LIMITS, &out), "getter succeeds");
    TEST_CHECK(out == 1, "the exact value set comes back out");

    TEST_CHECK(zones_config_set_settings_source(0, SRC_GROUP_LIMITS, ZONE_SETTINGS_SOURCE_CUSTOM), "0xFF (CUSTOM) is always legal");
    TEST_CHECK(zones_config_get_settings_source(0, SRC_GROUP_LIMITS, &out) && out == ZONE_SETTINGS_SOURCE_CUSTOM,
              "CUSTOM round-trips exactly");

    TEST_CHECK(!zones_config_set_settings_source(0, SRC_GROUP_LIMITS, 0), "zone 0 cannot copy itself (self-reference refused)");
    TEST_CHECK(!zones_config_set_settings_source(0, SRC_GROUP_LIMITS, 3), "zone index 3 does not exist (thermo_count is 3, 0-2 valid)");
    TEST_CHECK(zones_config_get_settings_source(0, SRC_GROUP_LIMITS, &out) && out == ZONE_SETTINGS_SOURCE_CUSTOM,
              "every refused call above left the stored value at CUSTOM (refuse, not clamp)");

    nvs_test_enable(false);
    nvs_test_clear();
}

// ---------------------------------------------------------------------------
// ZONES_CFG_VERSION 12->13: the tuning-quality record (set 1 of the owner's
// two-sets request -- see zone_cfg_t::tuning_valid's own doc comment).
// ---------------------------------------------------------------------------

// Round-trip through zones_config_set_tuning_quality()/get_tuning_quality()
// using a DELIBERATELY ASYMMETRIC per-zone fixture (every field distinct
// between zone 0 and zone 2, and zone 1 left entirely unset) -- a transposed
// zone index, or a getter/setter that silently shares one struct across
// zones, would fail this test rather than coincidentally pass it, the same
// discipline test_coupling_single_cell_setter_preserves_other_cells() above
// applies to the coupling row setter.
static void test_tuning_quality_round_trip_asymmetric_per_zone(void)
{
    TEST_SECTION("zones_config_set/get_tuning_quality -- asymmetric per-zone round-trip, "
                 "zone 1 stays unknown, a transposed zone index would fail this");
    nvs_test_enable(true);
    nvs_test_clear();
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;

    zone_tuning_quality_t q0 = {
        .valid = true, .method = 0 /* STEP */, .rule = 0 /* SIMC */,
        .settled = true, .extrapolation_converged = true, .tau_consistent = false,
        .baseline_c = 25.5f, .step_ambient_c = 24.0f, .raw_rise_c = 40.0f, .rise_inf_c = 45.5f,
    };
    zone_tuning_quality_t q2 = {
        .valid = true, .method = 1 /* RELAY, stored even though finalize_fit() never actually
                                     * calls this setter for a relay run -- the getter/setter pair
                                     * itself must not assume STEP */
        , .rule = 3 /* Cohen-Coon */,
        .settled = false, .extrapolation_converged = false, .tau_consistent = true,
        .baseline_c = 802.1f, .step_ambient_c = 799.9f, .raw_rise_c = 12.25f, .rise_inf_c = 11.9f,
    };

    TEST_CHECK(zones_config_set_tuning_quality(0, &q0), "zone 0's record is set");
    TEST_CHECK(zones_config_set_tuning_quality(2, &q2), "zone 2's record is set separately");

    zone_tuning_quality_t out0 = {0}, out1 = {0}, out2 = {0};
    TEST_CHECK(zones_config_get_tuning_quality(0, &out0), "zone 0's getter succeeds");
    TEST_CHECK(zones_config_get_tuning_quality(1, &out1), "zone 1's getter succeeds (even though unset)");
    TEST_CHECK(zones_config_get_tuning_quality(2, &out2), "zone 2's getter succeeds");

    TEST_CHECK(out0.valid, "zone 0 reads valid");
    TEST_CHECK(out0.method == 0 && out0.rule == 0, "zone 0's method/rule are exactly what was set");
    TEST_CHECK(out0.settled && out0.extrapolation_converged && !out0.tau_consistent,
              "zone 0's three flags are exactly what was set, including the one false");
    TEST_CHECK_NEAR(out0.baseline_c, 25.5f, 1e-4, "zone 0 baseline_c");
    TEST_CHECK_NEAR(out0.step_ambient_c, 24.0f, 1e-4, "zone 0 step_ambient_c");
    TEST_CHECK_NEAR(out0.raw_rise_c, 40.0f, 1e-4, "zone 0 raw_rise_c");
    TEST_CHECK_NEAR(out0.rise_inf_c, 45.5f, 1e-4, "zone 0 rise_inf_c");

    TEST_CHECK(!out1.valid, "zone 1 (never written) reads INVALID -- unknown, not a stray zero-valued record");

    TEST_CHECK(out2.valid, "zone 2 reads valid");
    TEST_CHECK(out2.method == 1 && out2.rule == 3, "zone 2's method/rule are exactly what was set -- "
              "DISTINCT from zone 0's, so a transposed zone index would fail here");
    TEST_CHECK(!out2.settled && !out2.extrapolation_converged && out2.tau_consistent,
              "zone 2's three flags are exactly what was set, the mirror-image pattern of zone 0's");
    TEST_CHECK_NEAR(out2.baseline_c, 802.1f, 1e-3, "zone 2 baseline_c -- far from zone 0's 25.5, catches a swap");
    TEST_CHECK_NEAR(out2.step_ambient_c, 799.9f, 1e-3, "zone 2 step_ambient_c");
    TEST_CHECK_NEAR(out2.raw_rise_c, 12.25f, 1e-4, "zone 2 raw_rise_c");
    TEST_CHECK_NEAR(out2.rise_inf_c, 11.9f, 1e-4, "zone 2 rise_inf_c");

    // Sequence counter: bumped by the setter itself, independently per zone.
    TEST_CHECK(s_zones.cfg.zones[0].tuning_seq == 1, "zone 0's seq is 1 after its first write");
    TEST_CHECK(s_zones.cfg.zones[2].tuning_seq == 1, "zone 2's seq is 1 after its first write, independent of zone 0's");
    TEST_CHECK(zones_config_set_tuning_quality(0, &q0), "zone 0 written a second time");
    TEST_CHECK(s_zones.cfg.zones[0].tuning_seq == 2, "zone 0's seq bumps again; zone 2's own write did not bump it");
    TEST_CHECK(s_zones.cfg.zones[2].tuning_seq == 1, "zone 2's seq is untouched by zone 0's second write");

    // Refusals: q->valid == false, NULL, out-of-range zone_index, non-finite,
    // and an out-of-range method/rule must all be refused without touching
    // storage -- same all-or-nothing discipline as every other setter here.
    zone_tuning_quality_t q_invalid = q0;
    q_invalid.valid = false;
    TEST_CHECK(!zones_config_set_tuning_quality(0, &q_invalid),
              "q->valid == false is refused -- a caller wanting to clear the record uses "
              "zones_config_set_pid() instead, never this setter");
    TEST_CHECK(!zones_config_set_tuning_quality(0, NULL), "NULL q is refused");
    TEST_CHECK(!zones_config_set_tuning_quality(3, &q0), "out-of-range zone_index is refused");
    zone_tuning_quality_t q_nan = q0;
    q_nan.baseline_c = NAN;
    TEST_CHECK(!zones_config_set_tuning_quality(0, &q_nan), "non-finite baseline_c is refused");
    zone_tuning_quality_t q_bad_rule = q0;
    q_bad_rule.rule = 4;
    TEST_CHECK(!zones_config_set_tuning_quality(0, &q_bad_rule), "an out-of-range rule value is refused");
    TEST_CHECK(zones_config_get_tuning_quality(0, &out0) && out0.tau_consistent == false &&
              out0.baseline_c == 25.5f,
              "every refusal above left zone 0's stored record exactly at its last successful write "
              "(the second q0 write), not partially applied and not touched by the refused calls");

    nvs_test_enable(false);
    nvs_test_clear();
}

// The invalidation this whole feature exists to get right: ANY gain change
// through zones_config_set_pid() -- a manual PID edit is the operator-facing
// example, but the setter cannot distinguish its callers -- must invalidate
// a previously-written tuning-quality record. This is the reset-one-side
// guard: comment out the invalidation line in zones_config_set_pid() and
// this test must fail. Verified by hand (see this file's own build log) --
// removing `z->tuning_valid = 0;` from zones_config_accessors.c leaves
// tuning_valid at 1, and the "invalidated" TEST_CHECK below goes red.
static void test_zones_config_set_pid_invalidates_tuning_quality(void)
{
    TEST_SECTION("zones_config_set_pid() invalidates a zone's tuning-quality record -- "
                 "the reset-one-side guard (a manual PID edit must not leave a stale record standing)");
    nvs_test_enable(true);
    nvs_test_clear();
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;

    zone_tuning_quality_t q = {
        .valid = true, .method = 0, .rule = 0,
        .settled = true, .extrapolation_converged = true, .tau_consistent = true,
        .baseline_c = 25.0f, .step_ambient_c = 25.0f, .raw_rise_c = 50.0f, .rise_inf_c = 50.0f,
    };
    TEST_CHECK(zones_config_set_tuning_quality(1, &q), "zone 1's record is set (autotune just accepted)");
    zone_tuning_quality_t before = {0};
    TEST_CHECK(zones_config_get_tuning_quality(1, &before) && before.valid, "sanity: zone 1 reads valid before the edit");

    // Same call POST /api/zones/pid's handler makes for a manual PID edit
    // (zones_http_handlers.c) -- this test goes through the shared setter
    // directly, which is exactly the point: invalidation lives in the ONE
    // choke point every gain-writing path (autotune accept, a manual edit,
    // adaptive_tune.c's re-blend, backup_http.c's restore, the LCD UI/
    // uart_bridge_ext.c path) already goes through, not duplicated at each
    // call site.
    TEST_CHECK(zones_config_set_pid(1, 12.0f, 0.5f, 3.0f), "a manual PID edit on zone 1 succeeds");

    zone_tuning_quality_t after = {0};
    TEST_CHECK(zones_config_get_tuning_quality(1, &after), "getter still succeeds after the edit");
    TEST_CHECK(!after.valid, "invalidated -- zone 1's tuning-quality record must read unknown "
              "after its gains changed by ANY path, not just an autotune accept");

    // Zone 0's untouched record is unaffected -- invalidation is per-zone,
    // not a global flag (would itself be a reset-one-side-shaped bug in the
    // other direction: wiping every zone's record because one was edited).
    TEST_CHECK(zones_config_set_tuning_quality(0, &q), "zone 0's own record is set");
    TEST_CHECK(zones_config_set_pid(1, 1.0f, 1.0f, 1.0f), "zone 1 edited again");
    zone_tuning_quality_t zone0_after = {0};
    TEST_CHECK(zones_config_get_tuning_quality(0, &zone0_after) && zone0_after.valid,
              "zone 0's record survives a DIFFERENT zone's gain edit -- invalidation is per-zone");

    nvs_test_enable(false);
    nvs_test_clear();
}

// ZONES_CFG_VERSION 12->13: a v12 blob (predates the tuning-quality record
// entirely) must migrate cleanly, with every tuning_* field reading as
// UNKNOWN (tuning_valid == false) rather than a value indistinguishable
// from a real (if zero-valued) measurement -- e.g. tuning_rule == 0 must
// NOT be readable as "SIMC, deliberately chosen", and tuning_settled == 0
// must NOT be readable as "measured and found unsettled". sizeof(src) is
// zone_cfg_v12_t/zones_cfg_v12_t -- frozen, historical types, never the
// live zone_cfg_t/zones_cfg_t (already the v13 shape by now), mirroring
// test_nvs_load_from_v11_blob_defaults_coupling_tau_dead_time_to_zero()
// above exactly.
static void test_nvs_load_from_v12_blob_defaults_tuning_quality_to_unknown(void)
{
    TEST_SECTION("nvs_load_from -- a v12 blob upconverts to v13: every tuning_* field defaults to "
                 "UNKNOWN (tuning_valid=false), never a value that reads as a real measurement, while "
                 "coupling_tau_s[]/model_k_dc/etc survive unchanged");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v12_t src;
    memset(&src, 0, sizeof(src));
    src.version = 12;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.continue_on_zone_trip = 1;
    src.safety_tc_type = 3;
    src.pc_link_abort_silence_ms = 45000.0f;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");

    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].max_temp_c = 1300.0f;
    src.zones[0].model_k_dc = 20.969f;
    src.zones[0].model_tau_s = 640.0f;
    src.zones[0].model_dead_time_s = 45.0f;
    src.zones[0].coupling_tau_s[1] = 42.0f;
    src.zones[0].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;
    src.zones[1].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.zones[2].relay_mask = 0x04;
    src.zones[2].thermo_mask = 0x04;
    src.zones[2].max_temp_c = 1200.0f;
    src.zones[2].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.crc32 = 0; // v12's own CRC is not checked on the old-version path

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v12 blob must migrate to a valid current (v13) config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");

    // THE thing this test is really about: every tuning_* field must be at
    // its "unknown" zero default for every zone -- not garbage, not
    // uninitialized memory -- since no version before v13 ever stored a
    // tuning-quality record.
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "zones[%u].tuning_valid is false (v12 never stored a quality record)", i);
        TEST_CHECK(!out_cfg.zones[i].tuning_valid, msg);
        snprintf(msg, sizeof(msg), "zones[%u].tuning_method reads 0, but gated unknown by tuning_valid", i);
        TEST_CHECK(out_cfg.zones[i].tuning_method == 0, msg);
        snprintf(msg, sizeof(msg), "zones[%u].tuning_settled reads 0, but gated unknown by tuning_valid", i);
        TEST_CHECK(out_cfg.zones[i].tuning_settled == 0, msg);
        snprintf(msg, sizeof(msg), "zones[%u].tuning_seq is 0 (never written)", i);
        TEST_CHECK(out_cfg.zones[i].tuning_seq == 0, msg);
    }
    // Pre-existing v12 fields must survive the upgrade completely unchanged
    // -- the same "everything else carries through" proof
    // test_nvs_load_from_v11_blob_defaults_coupling_tau_dead_time_to_zero()
    // makes for the v11->v12 migration.
    TEST_CHECK(out_cfg.thermo_count == 3 && out_cfg.relay_count == 3, "counts carried through");
    TEST_CHECK(out_cfg.continue_on_zone_trip == 1, "continue_on_zone_trip carried through");
    TEST_CHECK_NEAR(out_cfg.pc_link_abort_silence_ms, 45000.0f, 1e-6, "pc_link_abort_silence_ms carried through");
    TEST_CHECK(strcmp(out_cfg.timing_profiles[0].name, "Default") == 0, "timing profile name carried through");
    TEST_CHECK(out_cfg.zones[0].relay_mask == 0x01 && out_cfg.zones[1].relay_mask == 0x02 &&
              out_cfg.zones[2].relay_mask == 0x04, "relay_mask must NOT be shifted for any zone");
    TEST_CHECK_NEAR(out_cfg.zones[0].model_k_dc, 20.969f, 1e-6, "zones[0].model_k_dc survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].model_tau_s, 640.0f, 1e-6, "zones[0].model_tau_s survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].model_dead_time_s, 45.0f, 1e-6, "zones[0].model_dead_time_s survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].coupling_tau_s[1], 42.0f, 1e-6, "zones[0].coupling_tau_s[1] survives");
    for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
        char ss_msg[96];
        snprintf(ss_msg, sizeof(ss_msg), "zones[0].settings_source group %u carried through verbatim (Opus "
                "review of 5672719, item 1: the prefix+tail helper's fan-out must reach every group, not "
                "just LIMITS)", (unsigned)g);
        TEST_CHECK(out_cfg.zones[0].settings_source[g] == ZONE_SETTINGS_SOURCE_CUSTOM, ss_msg);
    }

    nvs_test_enable(false);
    nvs_test_clear();
}

// ZONES_CFG_VERSION 13->14: a v13 blob (predates adaptive_tune_enabled
// entirely -- the full tuning-quality record and everything before it, but
// no opt-in flag of its own) must migrate cleanly, with adaptive_tune_
// enabled defaulting to 0 ("opted out") for every zone, while every
// pre-existing field -- including the v13 tuning_* quality record and
// coupling_coeff[]/coupling_tau_s[]/coupling_dead_time_s[] -- survives
// unchanged. This is the migration test that was missing: convert_zone_v13()
// and case 13 of convert_versioned_blob_to_current() had never been
// exercised by a dedicated nvs_load_from() test, unlike every neighboring
// version step (v9 through v12, and v14). sizeof(src) is zone_cfg_v13_t/
// zones_cfg_v13_t -- frozen, historical types, never the live zone_cfg_t/
// zones_cfg_t (already the v14 shape by now), mirroring
// test_nvs_load_from_v12_blob_defaults_tuning_quality_to_unknown() above.
static void test_nvs_load_from_v13_blob_defaults_adaptive_tune_enabled_to_zero(void)
{
    TEST_SECTION("nvs_load_from -- a v13 blob upconverts to v14: adaptive_tune_enabled defaults to 0 "
                 "(\"opted out\") for every zone, while the tuning_* quality record/coupling_coeff[]/"
                 "coupling_tau_s[]/model_k_dc/etc survive unchanged");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v13_t src;
    memset(&src, 0, sizeof(src));
    src.version = 13;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.continue_on_zone_trip = 1;
    src.safety_tc_type = 3;
    src.pc_link_abort_silence_ms = 45000.0f;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");

    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].max_temp_c = 1300.0f;
    src.zones[0].model_k_dc = 20.969f;
    src.zones[0].model_tau_s = 640.0f;
    src.zones[0].model_dead_time_s = 45.0f;
    src.zones[0].coupling_coeff[1] = 10.887f;
    src.zones[0].coupling_tau_s[1] = 42.0f;
    src.zones[0].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    src.zones[0].tuning_valid = 1;
    src.zones[0].tuning_method = 1; /* autotune_method_t: 0=STEP, 1=RELAY */
    src.zones[0].tuning_rule = 3;   /* autotune_rule_t: 0-3 valid */
    src.zones[0].tuning_baseline_c = 25.5f;
    src.zones[0].tuning_seq = 7;

    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;
    src.zones[1].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.zones[2].relay_mask = 0x04;
    src.zones[2].thermo_mask = 0x04;
    src.zones[2].max_temp_c = 1200.0f;
    src.zones[2].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.crc32 = 0; // v13's own CRC is not checked on the old-version path

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v13 blob must migrate to a valid current (v14) config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");

    // THE thing this test is really about: adaptive_tune_enabled must be at
    // its "opted out" zero default for every zone -- not garbage, not
    // uninitialized memory -- since no version before v14 ever stored it.
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "zones[%u].adaptive_tune_enabled reads 0 (v13 never stored it)", i);
        TEST_CHECK(out_cfg.zones[i].adaptive_tune_enabled == 0, msg);
    }
    // Pre-existing v13 fields, including the tuning-quality record, must
    // survive the upgrade completely unchanged.
    TEST_CHECK(out_cfg.thermo_count == 3 && out_cfg.relay_count == 3, "counts carried through");
    TEST_CHECK(out_cfg.continue_on_zone_trip == 1, "continue_on_zone_trip carried through");
    TEST_CHECK_NEAR(out_cfg.pc_link_abort_silence_ms, 45000.0f, 1e-6, "pc_link_abort_silence_ms carried through");
    TEST_CHECK(strcmp(out_cfg.timing_profiles[0].name, "Default") == 0, "timing profile name carried through");
    TEST_CHECK(out_cfg.zones[0].relay_mask == 0x01 && out_cfg.zones[1].relay_mask == 0x02 &&
              out_cfg.zones[2].relay_mask == 0x04, "relay_mask must NOT be shifted for any zone");
    TEST_CHECK_NEAR(out_cfg.zones[0].model_k_dc, 20.969f, 1e-6, "zones[0].model_k_dc survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].coupling_coeff[1], 10.887f, 1e-6, "zones[0].coupling_coeff[1] survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].coupling_tau_s[1], 42.0f, 1e-6, "zones[0].coupling_tau_s[1] survives");
    TEST_CHECK(out_cfg.zones[0].tuning_valid == 1, "zones[0].tuning_valid survives");
    TEST_CHECK(out_cfg.zones[0].tuning_method == 1, "zones[0].tuning_method survives");
    TEST_CHECK(out_cfg.zones[0].tuning_rule == 3, "zones[0].tuning_rule survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].tuning_baseline_c, 25.5f, 1e-6, "zones[0].tuning_baseline_c survives");
    TEST_CHECK(out_cfg.zones[0].tuning_seq == 7, "zones[0].tuning_seq survives");
    for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
        char ss_msg[96];
        snprintf(ss_msg, sizeof(ss_msg), "zones[0].settings_source group %u carried through verbatim (Opus "
                "review of 5672719, item 1: the prefix+tail helper's fan-out must reach every group, not "
                "just LIMITS)", (unsigned)g);
        TEST_CHECK(out_cfg.zones[0].settings_source[g] == ZONE_SETTINGS_SOURCE_CUSTOM, ss_msg);
    }

    nvs_test_enable(false);
    nvs_test_clear();
}

// ZONES_CFG_VERSION 14->15 (PID_EXPANSION_PLAN.md section 3.2 follow-up): a
// v14 blob (predates coupling_diag_k_dc entirely -- adaptive_tune_enabled
// and everything before it, but no diagonal cell of its own) must migrate
// cleanly, with coupling_diag_k_dc defaulting to 0.0 ("not measured", same
// convention coupling_coeff[]/coupling_tau_s[] already use) for every zone,
// while every pre-existing field -- including adaptive_tune_enabled and the
// existing coupling_coeff[]/coupling_tau_s[] rows -- survives unchanged.
// sizeof(src) is zone_cfg_v14_t/zones_cfg_v14_t -- frozen, historical types,
// never the live zone_cfg_t/zones_cfg_t (already the v15 shape by now),
// mirroring test_nvs_load_from_v12_blob_defaults_tuning_quality_to_unknown()
// above exactly.
static void test_nvs_load_from_v14_blob_defaults_coupling_diag_k_dc_to_zero(void)
{
    TEST_SECTION("nvs_load_from -- a v14 blob upconverts to v15: coupling_diag_k_dc defaults to 0.0 "
                 "(\"not measured\") for every zone, while adaptive_tune_enabled/coupling_coeff[]/"
                 "coupling_tau_s[]/model_k_dc/etc survive unchanged");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v14_t src;
    memset(&src, 0, sizeof(src));
    src.version = 14;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.continue_on_zone_trip = 1;
    src.safety_tc_type = 3;
    src.pc_link_abort_silence_ms = 45000.0f;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");

    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].max_temp_c = 1300.0f;
    src.zones[0].model_k_dc = 20.969f;
    src.zones[0].model_tau_s = 640.0f;
    src.zones[0].model_dead_time_s = 45.0f;
    src.zones[0].coupling_coeff[1] = 10.887f;
    src.zones[0].coupling_tau_s[1] = 42.0f;
    src.zones[0].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    src.zones[0].adaptive_tune_enabled = 1;

    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;
    src.zones[1].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.zones[2].relay_mask = 0x04;
    src.zones[2].thermo_mask = 0x04;
    src.zones[2].max_temp_c = 1200.0f;
    src.zones[2].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.crc32 = 0; // v14's own CRC is not checked on the old-version path

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v14 blob must migrate to a valid current (v15) config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");

    // THE thing this test is really about: coupling_diag_k_dc must be at its
    // "not measured" zero default for every zone -- not garbage, not
    // uninitialized memory -- since no version before v15 ever stored it.
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "zones[%u].coupling_diag_k_dc reads 0.0 (v14 never stored it)", i);
        TEST_CHECK(out_cfg.zones[i].coupling_diag_k_dc == 0.0f, msg);
    }
    // Pre-existing v14 fields must survive the upgrade completely unchanged
    // -- the same "everything else carries through" proof
    // test_nvs_load_from_v12_blob_defaults_tuning_quality_to_unknown() makes
    // for the v12->v13 migration.
    TEST_CHECK(out_cfg.thermo_count == 3 && out_cfg.relay_count == 3, "counts carried through");
    TEST_CHECK(out_cfg.continue_on_zone_trip == 1, "continue_on_zone_trip carried through");
    TEST_CHECK_NEAR(out_cfg.pc_link_abort_silence_ms, 45000.0f, 1e-6, "pc_link_abort_silence_ms carried through");
    TEST_CHECK(strcmp(out_cfg.timing_profiles[0].name, "Default") == 0, "timing profile name carried through");
    TEST_CHECK(out_cfg.zones[0].relay_mask == 0x01 && out_cfg.zones[1].relay_mask == 0x02 &&
              out_cfg.zones[2].relay_mask == 0x04, "relay_mask must NOT be shifted for any zone");
    TEST_CHECK_NEAR(out_cfg.zones[0].model_k_dc, 20.969f, 1e-6, "zones[0].model_k_dc survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].coupling_coeff[1], 10.887f, 1e-6, "zones[0].coupling_coeff[1] survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].coupling_tau_s[1], 42.0f, 1e-6, "zones[0].coupling_tau_s[1] survives");
    for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
        char ss_msg[96];
        snprintf(ss_msg, sizeof(ss_msg), "zones[0].settings_source group %u carried through verbatim (Opus "
                "review of 5672719, item 1: the prefix+tail helper's fan-out must reach every group, not "
                "just LIMITS)", (unsigned)g);
        TEST_CHECK(out_cfg.zones[0].settings_source[g] == ZONE_SETTINGS_SOURCE_CUSTOM, ss_msg);
    }
    TEST_CHECK(out_cfg.zones[0].adaptive_tune_enabled == 1,
              "zones[0].adaptive_tune_enabled survives -- a real prior opt-in choice must not be lost");
    TEST_CHECK(out_cfg.zones[1].adaptive_tune_enabled == 0,
              "zones[1].adaptive_tune_enabled stays 0 (never opted in)");

    nvs_test_enable(false);
    nvs_test_clear();
}

// The A/B-campaign task's own migration proof: a v15 blob (the version
// immediately before ease_off_window_mult existed) must upconvert to v16
// with the new field defaulted to EXACTLY ZONE_EASE_OFF_WINDOW_MULT_DEFAULT
// (2.0) -- the same value the removed compile-time
// PROFILE_EXECUTOR_EASE_OFF_WINDOW_MULT #define held -- so a v15->v16
// upgrade changes NOTHING about how any existing board's firing behaves,
// while every other real v15 value (including coupling_diag_k_dc, the
// field the previous migration test above proves survives its own hop)
// keeps surviving through this one too. Same shape as
// test_nvs_load_from_v14_blob_defaults_coupling_diag_k_dc_to_zero() just
// above, one version later.
static void test_nvs_load_from_v15_blob_defaults_ease_off_window_mult_to_default(void)
{
    TEST_SECTION("nvs_load_from -- a v15 blob upconverts (chained through v16) to v17: every zone's "
                 "ease_off_window_mult resolves to 2.0 (matching the removed compile-time #define exactly "
                 "-- behaviour UNCHANGED), while coupling_diag_k_dc/model_k_dc/pc_link_abort_silence_ms/etc "
                 "survive unchanged");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v15_t src;
    memset(&src, 0, sizeof(src));
    src.version = 15;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.continue_on_zone_trip = 1;
    src.safety_tc_type = 3;
    src.pc_link_abort_silence_ms = 45000.0f;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");

    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].max_temp_c = 1300.0f;
    src.zones[0].model_k_dc = 20.969f;
    src.zones[0].model_tau_s = 640.0f;
    src.zones[0].model_dead_time_s = 45.0f;
    src.zones[0].coupling_coeff[1] = 10.887f;
    src.zones[0].coupling_diag_k_dc = 19.4f; /* a REAL, already-measured v15 value -- must survive */
    src.zones[0].settings_source = 1; /* real link to zone 1 -- not self-referencing, so normalize_settings_source_cycles() cannot silently mask a fan-out bug the way the CUSTOM/0 sentinel could for zone 0 */
    src.zones[0].adaptive_tune_enabled = 1;

    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;
    src.zones[1].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.zones[2].relay_mask = 0x04;
    src.zones[2].thermo_mask = 0x04;
    src.zones[2].max_temp_c = 1200.0f;
    src.zones[2].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.crc32 = 0; // v15's own CRC is not checked on the old-version path

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v15 blob must migrate to a valid current (v16) config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");

    // THE thing this test is really about: ease_off_window_mult -- now
    // per-zone (ZONES_CFG_VERSION 16->17) -- must land at the legal 0
    // sentinel on every zone (v15 never stored it, at any level), which
    // resolves through the accessor to ZONE_EASE_OFF_WINDOW_MULT_DEFAULT
    // (2.0) for EVERY zone, not just zone 0.
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        TEST_CHECK_NEAR(out_cfg.zones[j].ease_off_window_mult, 0.0f, 1e-9,
                        "v15 has no ease_off_window_mult -- lands on the 0 sentinel, not resolved eagerly");
    }
    s_zones.cfg = out_cfg;
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        float got = -1.0f;
        TEST_CHECK(zones_config_get_ease_off_window_mult(j, &got) && fabsf(got - ZONE_EASE_OFF_WINDOW_MULT_DEFAULT) < 1e-6,
                  "ease_off_window_mult resolves to 2.0 on every zone (v15 never stored it) -- behaviour "
                  "UNCHANGED from the removed compile-time #define");
    }

    // Pre-existing v15 fields -- including a REAL, non-default
    // coupling_diag_k_dc -- must survive the upgrade completely unchanged.
    TEST_CHECK(out_cfg.thermo_count == 3 && out_cfg.relay_count == 3, "counts carried through");
    TEST_CHECK(out_cfg.continue_on_zone_trip == 1, "continue_on_zone_trip carried through");
    TEST_CHECK_NEAR(out_cfg.pc_link_abort_silence_ms, 45000.0f, 1e-6, "pc_link_abort_silence_ms carried through");
    TEST_CHECK(strcmp(out_cfg.timing_profiles[0].name, "Default") == 0, "timing profile name carried through");
    TEST_CHECK(out_cfg.zones[0].relay_mask == 0x01 && out_cfg.zones[1].relay_mask == 0x02 &&
              out_cfg.zones[2].relay_mask == 0x04, "relay_mask must NOT be shifted for any zone");
    TEST_CHECK_NEAR(out_cfg.zones[0].model_k_dc, 20.969f, 1e-6, "zones[0].model_k_dc survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].coupling_coeff[1], 10.887f, 1e-6, "zones[0].coupling_coeff[1] survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].coupling_diag_k_dc, 19.4f, 1e-6,
                    "zones[0].coupling_diag_k_dc (a real v15 measurement) survives the v15->v16 hop");
    for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
        char ss_msg[96];
        snprintf(ss_msg, sizeof(ss_msg), "zones[0].settings_source group %u carried through verbatim (Opus "
                "review of 5672719, item 1: the prefix+tail helper's fan-out must reach every group, not "
                "just LIMITS)", (unsigned)g);
        TEST_CHECK(out_cfg.zones[0].settings_source[g] == 1, ss_msg);
    }
    TEST_CHECK(out_cfg.zones[0].adaptive_tune_enabled == 1,
              "zones[0].adaptive_tune_enabled survives -- a real prior opt-in choice must not be lost");

    nvs_test_enable(false);
    nvs_test_clear();
}

// THE migration this whole pass exists for (PID_EXPANSION_PLAN.md sec
// 3.6d): a v16 board that already ran the 2.0-vs-3.0 global A/B campaign
// (logs/coupling/easeoff_ab_20260904_report.md) has a REAL, possibly
// non-default global ease_off_window_mult sitting on flash right now -- this
// proves that value is carried forward VERBATIM to every single zone, not
// silently reset to the 2.0 default the way a naive "just re-default it"
// migration would. This is the exact backward-compatibility case the task
// calls out: "a stored config must load with every zone taking the old
// global value."
static void test_nvs_load_from_v16_blob_carries_global_ease_off_mult_to_every_zone(void)
{
    TEST_SECTION("nvs_load_from -- a v16 blob's single global ease_off_window_mult (e.g. a live 3.0 "
                 "A/B arm) migrates to v17 landing on EVERY zone verbatim -- not re-defaulted, not left "
                 "on only zone 0");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v16_t src;
    memset(&src, 0, sizeof(src));
    src.version = 16;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.safety_tc_type = 3;
    src.pc_link_abort_silence_ms = 30000.0f;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");
    src.ease_off_window_mult = 3.0f; /* a REAL, non-default, live A/B arm value -- must reach every zone */

    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].max_temp_c = 1300.0f;
    src.zones[0].model_k_dc = 22.5f;

    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;

    src.zones[2].relay_mask = 0x04;
    src.zones[2].thermo_mask = 0x04;
    src.zones[2].max_temp_c = 1200.0f;

    src.crc32 = 0; // v16's own CRC is not checked on the old-version path

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v16 blob must migrate to a valid current (v17) config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");

    // THE thing this test is really about: every zone's ease_off_window_mult
    // -- not just zone 0's -- must be EXACTLY the v16 board's one global
    // value, carried forward verbatim.
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        TEST_CHECK_NEAR(out_cfg.zones[j].ease_off_window_mult, 3.0f, 1e-6,
                        "every zone's ease_off_window_mult carries the v16 board's global 3.0 verbatim");
    }

    // Pre-existing v16 fields must survive unchanged too.
    TEST_CHECK(out_cfg.thermo_count == 3 && out_cfg.relay_count == 3, "counts carried through");
    TEST_CHECK_NEAR(out_cfg.pc_link_abort_silence_ms, 30000.0f, 1e-6, "pc_link_abort_silence_ms carried through");
    TEST_CHECK_NEAR(out_cfg.zones[0].model_k_dc, 22.5f, 1e-6, "zones[0].model_k_dc survives");
    TEST_CHECK(out_cfg.zones[1].relay_mask == 0x02 && out_cfg.zones[2].relay_mask == 0x04,
              "relay_mask must NOT be shifted for any zone");

    nvs_test_enable(false);
    nvs_test_clear();
}

// zones_config_get/set_ease_off_window_mult() -- the accessor pair
// profile_executor_feedforward.c's zone_taper_climb_rate() actually calls
// every tick (via a locally forward-declared prototype -- see that file's
// own comment on why it does not #include zones_config_json.h directly).
// Covers: round-trip through a real NVS save (not just an in-RAM field
// poke), the refuse-don't-clamp discipline on both the ceiling and floor,
// NaN refusal, and the 0 "reset to default" sentinel actually resolving to
// ZONE_EASE_OFF_WINDOW_MULT_DEFAULT through the GETTER (not merely stored
// as literal 0) -- the same distinction zone_taper_climb_rate() itself
// depends on to never divide by an effectively-zero window.
static void test_ease_off_window_mult_accessor_get_set_and_range(void)
{
    TEST_SECTION("zones_config_get/set_ease_off_window_mult() -- PER-ZONE: round-trips through NVS, "
                 "refuses out-of-range (ceiling AND floor) and NaN, resolves the 0 sentinel to the "
                 "real default through the getter, an invalid zone index is refused, and setting "
                 "ONE zone's value never touches another zone's");
    nvs_test_enable(true);
    nvs_test_clear();

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 2;
    s_zones.cfg.relay_count = 2;
    s_zones.cfg.zones[0].relay_mask = 0x01;
    s_zones.cfg.zones[0].thermo_mask = 0x01;
    s_zones.cfg.zones[0].max_temp_c = 1300.0f;
    s_zones.cfg.zones[1].relay_mask = 0x02;
    s_zones.cfg.zones[1].thermo_mask = 0x02;
    s_zones.cfg.zones[1].max_temp_c = 1300.0f;
    s_zones.cfg.timing_profile_count = 1;
    strncpy(s_zones.cfg.timing_profiles[0].name, "Default", TIMING_PROFILE_NAME_MAX_LEN);
    TEST_CHECK(nvs_save() == ESP_OK, "initial save must succeed");

    // An invalid zone index is refused outright, same discipline as every
    // other per-zone accessor in this file.
    float ignored = -1.0f;
    TEST_CHECK(!zones_config_get_ease_off_window_mult(MAX31856_CHANNEL_COUNT, &ignored),
              "get() with an out-of-range zone index is refused");
    TEST_CHECK(!zones_config_set_ease_off_window_mult(MAX31856_CHANNEL_COUNT, 2.0f),
              "set() with an out-of-range zone index is refused");

    // A real, in-range A/B value actually persists and reads back through
    // the accessor -- the "config set -> persisted -> read" leg of the
    // full chain this task cares about most. Set on zone 0 ONLY.
    TEST_CHECK(zones_config_set_ease_off_window_mult(0, 0.75f), "set(zone 0, 0.75) -- a real in-range A/B value -- succeeds");
    float got = -1.0f;
    TEST_CHECK(zones_config_get_ease_off_window_mult(0, &got) && fabsf(got - 0.75f) < 1e-6,
              "get(zone 0) reads back exactly the value just set, from LIVE state");
    // THE per-zone isolation proof: zone 1 must NOT have moved. If the
    // setter (or its underlying storage) ever wrote the wrong zone's slot,
    // or a single board-wide field were still backing this call, zone 1
    // would read 0.75 too.
    float got_z1 = -1.0f;
    TEST_CHECK(zones_config_get_ease_off_window_mult(1, &got_z1) && fabsf(got_z1 - ZONE_EASE_OFF_WINDOW_MULT_DEFAULT) < 1e-6,
              "zone 1's ease_off_window_mult is UNTOUCHED by zone 0's set() -- still resolves to the "
              "2.0 default, not zone 0's 0.75");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg)); // wipe the live struct, force a real reload
    bool found = false, valid = false;
    TEST_CHECK(nvs_load(&found, &valid) == ESP_OK && found && valid, "reload after set() must succeed");
    got = -1.0f;
    TEST_CHECK(zones_config_get_ease_off_window_mult(0, &got) && fabsf(got - 0.75f) < 1e-6,
              "0.75 survives a genuine NVS round trip, not just an in-RAM poke");
    got_z1 = -1.0f;
    TEST_CHECK(zones_config_get_ease_off_window_mult(1, &got_z1) && fabsf(got_z1 - ZONE_EASE_OFF_WINDOW_MULT_DEFAULT) < 1e-6,
              "zone 1 still reads the 2.0 default after the reload -- the isolation survives a real NVS "
              "round trip, not just an in-RAM poke");

    // Refuse, never clamp -- ceiling, floor, and NaN, none of which may
    // silently become a different number or corrupt the live value.
    TEST_CHECK(!zones_config_set_ease_off_window_mult(0, ZONE_EASE_OFF_WINDOW_MULT_MAX + 1.0f),
              "set() above the ceiling is refused");
    TEST_CHECK(!zones_config_set_ease_off_window_mult(0, ZONE_EASE_OFF_WINDOW_MULT_MIN / 2.0f),
              "set() below the floor (but nonzero) is refused");
    TEST_CHECK(!zones_config_set_ease_off_window_mult(0, -1.0f), "set() of a negative value is refused");
    TEST_CHECK(!zones_config_set_ease_off_window_mult(0, NAN), "set() of NaN is refused");
    got = -1.0f;
    TEST_CHECK(zones_config_get_ease_off_window_mult(0, &got) && fabsf(got - 0.75f) < 1e-6,
              "every refused set() above left the live value at 0.75, untouched -- refuse, not clamp");

    // The 0 sentinel: legal to SET (an A/B campaign resetting an arm),
    // resolves through the GETTER to the real default -- not literal 0,
    // which zone_taper_climb_rate() could not safely divide by.
    TEST_CHECK(zones_config_set_ease_off_window_mult(0, 0.0f), "set(zone 0, 0.0) -- the reset-to-default sentinel -- succeeds");
    TEST_CHECK_NEAR(s_zones.cfg.zones[0].ease_off_window_mult, 0.0f, 1e-9, "the sentinel is stored as literal 0, not resolved eagerly");
    got = -1.0f;
    TEST_CHECK(zones_config_get_ease_off_window_mult(0, &got) && fabsf(got - ZONE_EASE_OFF_WINDOW_MULT_DEFAULT) < 1e-6,
              "the getter resolves the stored 0 sentinel to ZONE_EASE_OFF_WINDOW_MULT_DEFAULT (2.0)");

    nvs_test_enable(false);
    nvs_test_clear();
}

// THE migration this pass exists for (PID_EXPANSION_PLAN.md sec 3.6d /
// PER_ZONE_TARGET_DESIGN_STUDY.md option (b)): a v17 board loading its own
// real, already-commissioned config (PID gains, plant models, coupling rows,
// a per-zone ease_off_window_mult override) must come up with every zone's
// BRAND NEW approach_rate_cap_c_per_hr at the 0 (uncapped) sentinel -- unlike
// ease_off_window_mult's v16->v17 migration, there is no prior global scalar
// to carry forward, so this is a pure "does the new field survive as the
// safe default, not garbage from the memset boundary" proof, alongside every
// pre-existing v17 field surviving completely unchanged.
static void test_nvs_load_from_v17_blob_defaults_approach_rate_cap_to_uncapped(void)
{
    TEST_SECTION("nvs_load_from -- a v17 blob upconverts to v18: every zone's new "
                 "approach_rate_cap_c_per_hr lands on the 0 (uncapped) sentinel -- behaviour UNCHANGED -- "
                 "while ease_off_window_mult/coupling_diag_k_dc/model_k_dc/etc survive unchanged");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v17_t src;
    memset(&src, 0, sizeof(src));
    src.version = 17;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.continue_on_zone_trip = 1;
    src.safety_tc_type = 3;
    src.pc_link_abort_silence_ms = 45000.0f;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");

    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].max_temp_c = 1300.0f;
    src.zones[0].model_k_dc = 20.969f;
    src.zones[0].model_tau_s = 640.0f;
    src.zones[0].model_dead_time_s = 45.0f;
    src.zones[0].coupling_coeff[1] = 10.887f;
    src.zones[0].coupling_diag_k_dc = 19.4f;
    src.zones[0].ease_off_window_mult = 3.5f; /* a REAL, already-running A/B arm -- must survive */
    src.zones[0].settings_source = 1; /* real link to zone 1 -- not self-referencing, so normalize_settings_source_cycles() cannot silently mask a fan-out bug the way the CUSTOM/0 sentinel could for zone 0 */

    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;
    src.zones[1].ease_off_window_mult = 2.0f;
    src.zones[1].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.zones[2].relay_mask = 0x04;
    src.zones[2].thermo_mask = 0x04;
    src.zones[2].max_temp_c = 1200.0f;
    src.zones[2].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.crc32 = 0; // v17's own CRC is not checked on the old-version path

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v17 blob must migrate to a valid current (v18) config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");

    // THE thing this test is really about: approach_rate_cap_c_per_hr must
    // land at the legal 0 (uncapped) sentinel on EVERY zone -- v17 never
    // stored it, at any level, and unlike ease_off_window_mult there is no
    // global scalar this migration needs to carry forward instead.
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        TEST_CHECK_NEAR(out_cfg.zones[j].approach_rate_cap_c_per_hr, 0.0f, 1e-9,
                        "v17 has no approach_rate_cap_c_per_hr -- lands on the 0 (uncapped) sentinel");
        float got = -1.0f;
        s_zones.cfg = out_cfg;
        TEST_CHECK(zones_config_get_approach_rate_cap_c_per_hr(j, &got) && got == 0.0f,
                  "the accessor reports every migrated zone uncapped -- not resolved to any other value");
    }

    // Pre-existing v17 fields -- including a REAL, non-default per-zone
    // ease_off_window_mult A/B arm -- must survive the upgrade completely
    // unchanged.
    TEST_CHECK(out_cfg.thermo_count == 3 && out_cfg.relay_count == 3, "counts carried through");
    TEST_CHECK_NEAR(out_cfg.pc_link_abort_silence_ms, 45000.0f, 1e-6, "pc_link_abort_silence_ms carried through");
    TEST_CHECK(out_cfg.zones[0].relay_mask == 0x01 && out_cfg.zones[1].relay_mask == 0x02 &&
              out_cfg.zones[2].relay_mask == 0x04, "relay_mask must NOT be shifted for any zone");
    TEST_CHECK_NEAR(out_cfg.zones[0].model_k_dc, 20.969f, 1e-6, "zones[0].model_k_dc survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].coupling_diag_k_dc, 19.4f, 1e-6, "zones[0].coupling_diag_k_dc survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].ease_off_window_mult, 3.5f, 1e-6,
                    "zones[0]'s REAL, non-default ease_off_window_mult A/B arm survives the v17->v18 hop");
    TEST_CHECK_NEAR(out_cfg.zones[1].ease_off_window_mult, 2.0f, 1e-6, "zones[1].ease_off_window_mult survives");
    for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
        char ss_msg[96];
        snprintf(ss_msg, sizeof(ss_msg), "zones[0].settings_source group %u carried through verbatim (Opus "
                "review of 5672719, item 1: the prefix+tail helper's fan-out must reach every group, not "
                "just LIMITS)", (unsigned)g);
        TEST_CHECK(out_cfg.zones[0].settings_source[g] == 1, ss_msg);
    }

    nvs_test_enable(false);
    nvs_test_clear();
}

// Accessor pair for zone_cfg_t::approach_rate_cap_c_per_hr (PID_EXPANSION_
// PLAN.md sec 3.6d / PER_ZONE_TARGET_DESIGN_STUDY.md option (b)) -- same
// shape as test_ease_off_window_mult_accessor_get_set_and_range() just
// above, with the one deliberate semantic difference: 0 here means
// "uncapped," reported VERBATIM by the getter, never resolved into some
// other substituted default the way ease_off_window_mult's 0 resolves to
// 2.0 (there is no sensible non-zero default rate to substitute for "no
// cap" -- see ZONE_APPROACH_RATE_CAP_C_PER_HR_MIN's own comment).
static void test_approach_rate_cap_accessor_get_set_and_range(void)
{
    TEST_SECTION("zones_config_get/set_approach_rate_cap_c_per_hr() -- round trip, per-zone isolation, "
                 "refuse-don't-clamp bounds, and the 0 'uncapped' sentinel reported verbatim (NOT resolved "
                 "to a substituted default, unlike ease_off_window_mult's own 0)");
    nvs_test_enable(true);
    nvs_test_clear();

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 2;
    s_zones.cfg.relay_count = 2;
    s_zones.cfg.zones[0].relay_mask = 0x01;
    s_zones.cfg.zones[0].thermo_mask = 0x01;
    s_zones.cfg.zones[0].max_temp_c = 1300.0f;
    s_zones.cfg.zones[1].relay_mask = 0x02;
    s_zones.cfg.zones[1].thermo_mask = 0x02;
    s_zones.cfg.zones[1].max_temp_c = 1300.0f;
    s_zones.cfg.timing_profile_count = 1;
    strncpy(s_zones.cfg.timing_profiles[0].name, "Default", TIMING_PROFILE_NAME_MAX_LEN);
    TEST_CHECK(nvs_save() == ESP_OK, "initial save must succeed");

    // A freshly-loaded/uncommissioned zone reads 0 (uncapped), the default.
    float got = -1.0f;
    TEST_CHECK(zones_config_get_approach_rate_cap_c_per_hr(0, &got) && got == 0.0f,
              "zone 0's cap defaults to 0.0 (uncapped) -- bit-identical to every zone before this field existed");

    // An invalid zone index is refused outright, same discipline as every
    // other per-zone accessor in this file.
    float ignored = -1.0f;
    TEST_CHECK(!zones_config_get_approach_rate_cap_c_per_hr(MAX31856_CHANNEL_COUNT, &ignored),
              "get() with an out-of-range zone index is refused");
    TEST_CHECK(!zones_config_set_approach_rate_cap_c_per_hr(MAX31856_CHANNEL_COUNT, 30.0f),
              "set() with an out-of-range zone index is refused");

    // A real, in-range A/B value persists and reads back -- set on zone 0 ONLY.
    TEST_CHECK(zones_config_set_approach_rate_cap_c_per_hr(0, 30.0f), "set(zone 0, 30.0 C/hr) succeeds");
    got = -1.0f;
    TEST_CHECK(zones_config_get_approach_rate_cap_c_per_hr(0, &got) && fabsf(got - 30.0f) < 1e-6,
              "get(zone 0) reads back exactly the value just set, from LIVE state");
    // THE per-zone isolation proof: zone 1 must NOT have moved. This is the
    // exact bug class a transposed index (or a stray single shared field)
    // would produce.
    float got_z1 = -1.0f;
    TEST_CHECK(zones_config_get_approach_rate_cap_c_per_hr(1, &got_z1) && got_z1 == 0.0f,
              "zone 1's approach_rate_cap_c_per_hr is UNTOUCHED by zone 0's set() -- still 0.0 (uncapped), "
              "not zone 0's 30.0");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg)); // wipe the live struct, force a real reload
    bool found = false, valid = false;
    TEST_CHECK(nvs_load(&found, &valid) == ESP_OK && found && valid, "reload after set() must succeed");
    got = -1.0f;
    TEST_CHECK(zones_config_get_approach_rate_cap_c_per_hr(0, &got) && fabsf(got - 30.0f) < 1e-6,
              "30.0 survives a genuine NVS round trip, not just an in-RAM poke");
    got_z1 = -1.0f;
    TEST_CHECK(zones_config_get_approach_rate_cap_c_per_hr(1, &got_z1) && got_z1 == 0.0f,
              "zone 1 still reads 0.0 (uncapped) after the reload -- isolation survives a real NVS round trip");

    // Refuse, never clamp -- ceiling, the (0, MIN) sliver, and NaN, none of
    // which may silently become a different number or corrupt the live value.
    TEST_CHECK(!zones_config_set_approach_rate_cap_c_per_hr(0, ZONE_APPROACH_RATE_CAP_C_PER_HR_MAX + 1.0f),
              "set() above the ceiling is refused");
    TEST_CHECK(!zones_config_set_approach_rate_cap_c_per_hr(0, ZONE_APPROACH_RATE_CAP_C_PER_HR_MIN / 2.0f),
              "set() below the floor (but nonzero) is refused");
    TEST_CHECK(!zones_config_set_approach_rate_cap_c_per_hr(0, -1.0f), "set() of a negative value is refused");
    TEST_CHECK(!zones_config_set_approach_rate_cap_c_per_hr(0, NAN), "set() of NaN is refused");
    got = -1.0f;
    TEST_CHECK(zones_config_get_approach_rate_cap_c_per_hr(0, &got) && fabsf(got - 30.0f) < 1e-6,
              "every refused set() above left the live value at 30.0, untouched -- refuse, not clamp");

    // The 0 sentinel: legal to SET (an A/B campaign ending an arm), reported
    // VERBATIM by the getter -- UNLIKE ease_off_window_mult's 0, this is NOT
    // resolved into any other substituted value.
    TEST_CHECK(zones_config_set_approach_rate_cap_c_per_hr(0, 0.0f), "set(zone 0, 0.0) -- uncap -- succeeds");
    got = -1.0f;
    TEST_CHECK(zones_config_get_approach_rate_cap_c_per_hr(0, &got) && got == 0.0f,
              "the getter reports the stored 0 sentinel VERBATIM -- 'uncapped' IS the answer, not a "
              "substituted default the way ease_off_window_mult's 0 resolves to 2.0");

    nvs_test_enable(false);
    nvs_test_clear();
}

// THE migration this pass exists for (PID_EXPANSION_PLAN.md sec 3.6g): a v18
// board loading its own real, already-commissioned config (PID gains, plant
// models, coupling rows, a per-zone approach_rate_cap_c_per_hr override) must
// come up with every zone's BRAND NEW error_band_c/rate_band_c_per_s at the 0
// (use-firmware-default) sentinel -- same shape as approach_rate_cap_c_per_hr's
// own v17->v18 migration (no prior global scalar to carry forward), NOT
// ease_off_window_mult's v16->v17 shape (which carried a real removed global
// forward). This is a pure "does the new field survive as the safe default,
// not garbage from the memset boundary, AND resolve through the accessor to
// bit-identical 20.0/0.5" proof, alongside every pre-existing v18 field
// surviving completely unchanged.
static void test_nvs_load_from_v18_blob_defaults_fuzzy_bands_to_firmware_default(void)
{
    TEST_SECTION("nvs_load_from -- a v18 blob upconverts to v19: every zone's new "
                 "error_band_c/rate_band_c_per_s land on the 0 sentinel, which the accessors resolve to "
                 "the UNCHANGED firmware default (20.0/0.5) -- while approach_rate_cap_c_per_hr/"
                 "ease_off_window_mult/model_k_dc/etc survive unchanged");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v18_t src;
    memset(&src, 0, sizeof(src));
    src.version = 18;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.continue_on_zone_trip = 1;
    src.safety_tc_type = 3;
    src.pc_link_abort_silence_ms = 45000.0f;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");

    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].max_temp_c = 1300.0f;
    src.zones[0].model_k_dc = 20.969f;
    src.zones[0].model_tau_s = 640.0f;
    src.zones[0].model_dead_time_s = 45.0f;
    src.zones[0].coupling_coeff[1] = 10.887f;
    src.zones[0].coupling_diag_k_dc = 19.4f;
    src.zones[0].ease_off_window_mult = 3.5f;
    src.zones[0].approach_rate_cap_c_per_hr = 30.0f; /* a REAL, already-running A/B arm -- must survive */
    src.zones[0].settings_source = 1; /* real link to zone 1 -- not self-referencing, so normalize_settings_source_cycles() cannot silently mask a fan-out bug the way the CUSTOM/0 sentinel could for zone 0 */

    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;
    src.zones[1].ease_off_window_mult = 2.0f;
    src.zones[1].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.zones[2].relay_mask = 0x04;
    src.zones[2].thermo_mask = 0x04;
    src.zones[2].max_temp_c = 1200.0f;
    src.zones[2].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.crc32 = 0; // v18's own CRC is not checked on the old-version path

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v18 blob must migrate to a valid current (v19) config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");

    // THE thing this test is really about: error_band_c/rate_band_c_per_s
    // must land at the legal 0 sentinel on EVERY zone's raw stored field (v18
    // never stored either, at any level), and the ACCESSOR must resolve that
    // 0 to the bit-identical firmware default -- 20.0/0.5, exactly what the
    // removed ERROR_BAND_C/RATE_BAND_C_PER_S #defines always were.
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        TEST_CHECK_NEAR(out_cfg.zones[j].error_band_c, 0.0f, 1e-9,
                        "v18 has no error_band_c -- the raw stored field lands on the 0 sentinel");
        TEST_CHECK_NEAR(out_cfg.zones[j].rate_band_c_per_s, 0.0f, 1e-9,
                        "v18 has no rate_band_c_per_s -- the raw stored field lands on the 0 sentinel");
        s_zones.cfg = out_cfg;
        float got_e = -1.0f, got_r = -1.0f;
        TEST_CHECK(zones_config_get_error_band_c(j, &got_e) && fabsf(got_e - 20.0f) < 1e-6,
                  "the accessor resolves every migrated zone's error band to the bit-identical "
                  "firmware default 20.0 -- an existing board migrates to EXACTLY today's behaviour");
        TEST_CHECK(zones_config_get_rate_band_c_per_s(j, &got_r) && fabsf(got_r - 0.5f) < 1e-6,
                  "the accessor resolves every migrated zone's rate band to the bit-identical "
                  "firmware default 0.5 -- an existing board migrates to EXACTLY today's behaviour");
    }

    // Pre-existing v18 fields -- including a REAL, non-default per-zone
    // approach_rate_cap_c_per_hr A/B arm -- must survive the upgrade
    // completely unchanged.
    TEST_CHECK(out_cfg.thermo_count == 3 && out_cfg.relay_count == 3, "counts carried through");
    TEST_CHECK_NEAR(out_cfg.pc_link_abort_silence_ms, 45000.0f, 1e-6, "pc_link_abort_silence_ms carried through");
    TEST_CHECK(out_cfg.zones[0].relay_mask == 0x01 && out_cfg.zones[1].relay_mask == 0x02 &&
              out_cfg.zones[2].relay_mask == 0x04, "relay_mask must NOT be shifted for any zone");
    TEST_CHECK_NEAR(out_cfg.zones[0].model_k_dc, 20.969f, 1e-6, "zones[0].model_k_dc survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].coupling_diag_k_dc, 19.4f, 1e-6, "zones[0].coupling_diag_k_dc survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].ease_off_window_mult, 3.5f, 1e-6, "zones[0].ease_off_window_mult survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].approach_rate_cap_c_per_hr, 30.0f, 1e-6,
                    "zones[0]'s REAL, non-default approach_rate_cap_c_per_hr A/B arm survives the v18->v19 hop");
    for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
        char ss_msg[96];
        snprintf(ss_msg, sizeof(ss_msg), "zones[0].settings_source group %u carried through verbatim (Opus "
                "review of 5672719, item 1: the prefix+tail helper's fan-out must reach every group, not "
                "just LIMITS)", (unsigned)g);
        TEST_CHECK(out_cfg.zones[0].settings_source[g] == 1, ss_msg);
    }

    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_nvs_load_from_v19_blob_defaults_relay_type_to_ssr(void)
{
    TEST_SECTION("nvs_load_from -- a v19 blob upconverts to v20: every zone's new "
                 "relay_type lands on the 0 (ssr) sentinel -- while error_band_c/rate_band_c_per_s/"
                 "approach_rate_cap_c_per_hr/model_k_dc/settings_source/etc survive unchanged");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v19_t src;
    memset(&src, 0, sizeof(src));
    src.version = 19;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.continue_on_zone_trip = 1;
    src.safety_tc_type = 3;
    src.pc_link_abort_silence_ms = 45000.0f;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");

    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].max_temp_c = 1300.0f;
    src.zones[0].model_k_dc = 20.969f;
    src.zones[0].error_band_c = 15.0f; /* a REAL, non-default per-zone fuzzy band -- must survive */
    src.zones[0].rate_band_c_per_s = 0.8f;
    src.zones[0].approach_rate_cap_c_per_hr = 30.0f;
    src.zones[0].settings_source = 1; /* real link to zone 1 -- not self-referencing, so normalize_settings_source_cycles() cannot silently mask a fan-out bug the way the CUSTOM/0 sentinel could for zone 0 */

    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;
    src.zones[1].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.zones[2].relay_mask = 0x04;
    src.zones[2].thermo_mask = 0x04;
    src.zones[2].max_temp_c = 1200.0f;
    src.zones[2].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.crc32 = 0; // v19's own CRC is not checked on the old-version path

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v19 blob must migrate to a valid current (v20) config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");

    // THE thing this test is really about: relay_type must land at the legal
    // 0 (ssr) sentinel on EVERY zone (v19 never stored this field at all) --
    // which is also, per RELAY_LIFE_BUDGET.md's design, exactly the
    // correct answer for every existing board's EE2-12NUH heater relays.
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        TEST_CHECK(out_cfg.zones[j].relay_type == 0,
                  "v19 has no relay_type -- every migrated zone lands on the 0 (ssr) sentinel");
    }

    // Pre-existing v19 fields, including a REAL, non-default per-zone
    // error_band_c/rate_band_c_per_s pair, must survive the upgrade
    // completely unchanged.
    TEST_CHECK(out_cfg.thermo_count == 3 && out_cfg.relay_count == 3, "counts carried through");
    TEST_CHECK_NEAR(out_cfg.pc_link_abort_silence_ms, 45000.0f, 1e-6, "pc_link_abort_silence_ms carried through");
    TEST_CHECK(out_cfg.zones[0].relay_mask == 0x01 && out_cfg.zones[1].relay_mask == 0x02 &&
              out_cfg.zones[2].relay_mask == 0x04, "relay_mask must NOT be shifted for any zone");
    TEST_CHECK_NEAR(out_cfg.zones[0].model_k_dc, 20.969f, 1e-6, "zones[0].model_k_dc survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].error_band_c, 15.0f, 1e-6,
                    "zones[0]'s REAL, non-default error_band_c survives the v19->v20 hop");
    TEST_CHECK_NEAR(out_cfg.zones[0].rate_band_c_per_s, 0.8f, 1e-6,
                    "zones[0]'s REAL, non-default rate_band_c_per_s survives the v19->v20 hop");
    TEST_CHECK_NEAR(out_cfg.zones[0].approach_rate_cap_c_per_hr, 30.0f, 1e-6,
                    "zones[0].approach_rate_cap_c_per_hr survives the v19->v20 hop");
    for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
        char ss_msg[96];
        snprintf(ss_msg, sizeof(ss_msg), "zones[0].settings_source group %u carried through verbatim (Opus "
                "review of 5672719, item 1: the prefix+tail helper's fan-out must reach every group, not "
                "just LIMITS)", (unsigned)g);
        TEST_CHECK(out_cfg.zones[0].settings_source[g] == 1, ss_msg);
    }

    // relay_cycles_set_type() push: nvs_load() (not nvs_load_from() directly,
    // which this test calls to isolate the migration itself) is the call
    // site that pushes relay_type out to relay_cycles.c -- covered by
    // zones_config_push_relay_type_pushes_to_relay_cycles below, which
    // exercises s_zones.cfg + the push helper directly rather than
    // duplicating the full nvs_load() path here.

    nvs_test_enable(false);
    nvs_test_clear();
}

// docs/ARCHITECTURE_DECISIONS.md#zones-page-clean-up-info-disclosure-schema-v20-v21-chartjs, ZONES_CFG_VERSION 20->21: the single whole-zone
// settings_source byte becomes settings_source[SRC_GROUP_COUNT]. THE thing
// this test is about: a v20 blob with settings_source=1 must migrate to
// FIVE 1s (one per group), not a fresh CUSTOM on four of the five -- a board
// mid-firing with zone 1 set to "same as zone 0" must keep behaving
// identically on every group after this upgrade.
static void test_nvs_load_from_v20_blob_fans_out_settings_source_to_every_group(void)
{
    TEST_SECTION("nvs_load_from -- a v20 blob upconverts to v21: settings_source=1 fans out to "
                 "all SRC_GROUP_COUNT groups, and CUSTOM (0xFF) does the same");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v20_t src;
    memset(&src, 0, sizeof(src));
    src.version = 20;
    src.thermo_count = 3;
    src.relay_count = 3;
    src.continue_on_zone_trip = 1;
    src.safety_tc_type = 3;
    src.pc_link_abort_silence_ms = 45000.0f;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");

    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].max_temp_c = 1300.0f;
    src.zones[0].settings_source = 1; // "copies zone 1" -- must land as 1 on all five groups

    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;
    src.zones[1].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.zones[2].relay_mask = 0x04;
    src.zones[2].thermo_mask = 0x04;
    src.zones[2].max_temp_c = 1200.0f;
    src.zones[2].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

    src.crc32 = 0; // v20's own CRC is not checked on the old-version path

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v20 blob must migrate to a valid current (v21) config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");

    // THE assertion: all five groups, not just one.
    for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "zone 0's old scalar (1) fanned out to group %u", (unsigned)g);
        TEST_CHECK(out_cfg.zones[0].settings_source[g] == 1, msg);
    }
    for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
        TEST_CHECK(out_cfg.zones[1].settings_source[g] == ZONE_SETTINGS_SOURCE_CUSTOM,
                  "zone 1's CUSTOM sentinel fanned out to every group too");
    }

    // A field from BEFORE settings_source (relay_mask, prefix) and a field
    // from AFTER it (tuning_valid onward has nothing set here, but
    // max_temp_c sits in the prefix too) must both still land correctly --
    // proves the split copy (prefix, then fanned-out settings_source, then
    // tail) did not corrupt either side of the widened field.
    TEST_CHECK(out_cfg.zones[0].relay_mask == 0x01 && out_cfg.zones[1].relay_mask == 0x02 &&
              out_cfg.zones[2].relay_mask == 0x04, "relay_mask (prefix field) must NOT be shifted for any zone");
    TEST_CHECK_NEAR(out_cfg.zones[0].max_temp_c, 1300.0f, 1e-6, "max_temp_c (prefix field) survives");
    TEST_CHECK(out_cfg.zones[0].tuning_valid == 0, "tuning_valid (tail field, v20 never set it) defaults false");

    nvs_test_enable(false);
    nvs_test_clear();
}

// Per-group resolution is cycle-safe INDEPENDENTLY per group: zone 0 -> zone
// 1 on LIMITS while zone 1 -> zone 0 on CONTROL must be accepted (each
// group's own chain terminates cleanly), and each door (get/set) must
// operate on the group actually asked for, not silently cross-talk with any
// other group on the same zone.
static void test_settings_source_per_group_resolution_is_independent(void)
{
    TEST_SECTION("zones_config_get/set_settings_source -- per-group chains are independent: a link "
                 "on one group never affects another group's chain on the same zone pair");
    nvs_test_enable(false);
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
            s_zones.cfg.zones[z].settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
        }
    }

    TEST_CHECK(zones_config_set_settings_source(0, SRC_GROUP_LIMITS, 1), "zone 0 -> zone 1 on LIMITS");
    TEST_CHECK(zones_config_set_settings_source(1, SRC_GROUP_CONTROL, 0), "zone 1 -> zone 0 on CONTROL -- "
              "legal: CONTROL's own chain (1->0->Custom) never touches LIMITS's chain (0->1->Custom)");

    uint8_t limits0 = 0xAA, control1 = 0xAA, limits1 = 0xAA, control0 = 0xAA;
    TEST_CHECK(zones_config_get_settings_source(0, SRC_GROUP_LIMITS, &limits0) && limits0 == 1,
              "zone 0's LIMITS link is 1");
    TEST_CHECK(zones_config_get_settings_source(1, SRC_GROUP_CONTROL, &control1) && control1 == 0,
              "zone 1's CONTROL link is 0");
    TEST_CHECK(zones_config_get_settings_source(1, SRC_GROUP_LIMITS, &limits1) &&
              limits1 == ZONE_SETTINGS_SOURCE_CUSTOM, "zone 1's LIMITS link is untouched (still Custom)");
    TEST_CHECK(zones_config_get_settings_source(0, SRC_GROUP_CONTROL, &control0) &&
              control0 == ZONE_SETTINGS_SOURCE_CUSTOM, "zone 0's CONTROL link is untouched (still Custom)");

    // Now closing a REAL cycle on LIMITS (0->1 already set; 1->0 on LIMITS
    // too would close it) must still be refused, proving the independence
    // above is not simply "the cycle-walk is broken and never refuses
    // anything."
    TEST_CHECK(!zones_config_set_settings_source(1, SRC_GROUP_LIMITS, 0),
              "zone 1 -> zone 0 on LIMITS IS refused -- it would close 0 -> 1 -> 0 on that same group");
}

// PRE-FLIGHT for the actual v18->v19 flash still pending on the physical
// board (2026-09-04, board still running commit 2bcdc2d / v18): the test
// above proves the migration on a hand-picked synthetic blob. This one runs
// the SAME real migration path against the board's OWN live config, read
// off it via kiln_call(control_get_zones)/get_board_state() immediately
// before this test was written (kilnCtl repo, this task) -- Kp/Ki/Kd, cal
// offset, ramp ceiling, temperature range, relay_mask/control_mode, and the
// live coupling_coeff matrix -- so the field this pass could most plausibly
// corrupt (a real, non-default value only a commissioned board would ever
// populate) is exercised with the actual number, not a stand-in.
static void test_nvs_load_from_v18_blob_real_board_values_migration(void)
{
    TEST_SECTION("nvs_load_from -- the LIVE board's real v18 config (read via kiln_call "
                 "control_get_zones/get_board_state, not synthesized) migrates to v19 with every "
                 "pre-existing field byte-identical and both new fuzzy-band fields resolving to 20.0/0.5");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v18_t src;
    memset(&src, 0, sizeof(src));
    src.version = 18;
    src.thermo_count = 3;
    src.relay_count = 4;
    src.continue_on_zone_trip = 1;
    src.safety_tc_type = 3;
    src.pc_link_abort_silence_ms = 45000.0f;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");

    // Zone 0 -- board's actual reported values (control_get_zones, 2026-09-04)
    src.zones[0].relay_mask = 0x01;
    src.zones[0].control_mode = 3;
    src.zones[0].cal_offset_c = 0.0f;
    src.zones[0].pid_kp = 0.03180000185966492f;
    src.zones[0].pid_ki = 9.999999747378752e-05f;
    src.zones[0].pid_kd = 0.8400999903678894f;
    src.zones[0].max_ramp_c_per_hr = 900.0f;
    src.zones[0].max_temp_c = 80.0f;
    src.zones[0].min_temp_c = 0.0f;
    src.zones[0].coupling_coeff[1] = 27.32f;
    src.zones[0].coupling_coeff[2] = 21.72f;

    // Zone 1 -- board's actual reported values
    src.zones[1].relay_mask = 0x02;
    src.zones[1].control_mode = 3;
    src.zones[1].cal_offset_c = 0.0f;
    src.zones[1].pid_kp = 0.048500001430511475f;
    src.zones[1].pid_ki = 0.00019999999494757503f;
    src.zones[1].pid_kd = 1.054800033569336f;
    src.zones[1].max_ramp_c_per_hr = 900.0f;
    src.zones[1].max_temp_c = 80.0f;
    src.zones[1].min_temp_c = 0.0f;
    src.zones[1].coupling_coeff[0] = 14.30f;
    src.zones[1].coupling_coeff[2] = 22.15f;

    // Zone 2 -- board's actual reported values
    src.zones[2].relay_mask = 0x04;
    src.zones[2].control_mode = 3;
    src.zones[2].cal_offset_c = 0.0f;
    src.zones[2].pid_kp = 0.06310000270605087f;
    src.zones[2].pid_ki = 0.00019999999494757503f;
    src.zones[2].pid_kd = 1.069000005722046f;
    src.zones[2].max_ramp_c_per_hr = 900.0f;
    src.zones[2].max_temp_c = 80.0f;
    src.zones[2].min_temp_c = 0.0f;
    src.zones[2].coupling_coeff[0] = 8.33f;
    src.zones[2].coupling_coeff[1] = 12.42f;

    src.crc32 = 0; // v18's own CRC is not checked on the old-version path

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "the board's real v18 blob must migrate to a valid current (v19) config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");

    // Every pre-existing field the board actually reported must survive
    // byte-identically -- this is the failure mode being guarded against:
    // a migration that works on all-defaults but corrupts a field only a
    // real, commissioned config populates.
    static const float kp[3] = {0.03180000185966492f, 0.048500001430511475f, 0.06310000270605087f};
    static const float ki[3] = {9.999999747378752e-05f, 0.00019999999494757503f, 0.00019999999494757503f};
    static const float kd[3] = {0.8400999903678894f, 1.054800033569336f, 1.069000005722046f};
    static const uint8_t relay_mask[3] = {0x01, 0x02, 0x04};

    for (uint8_t j = 0; j < 3; j++) {
        TEST_CHECK(out_cfg.zones[j].relay_mask == relay_mask[j], "relay_mask survives unchanged");
        TEST_CHECK(out_cfg.zones[j].control_mode == 3, "control_mode survives unchanged");
        TEST_CHECK_NEAR(out_cfg.zones[j].cal_offset_c, 0.0f, 1e-9, "cal_offset_c survives unchanged");
        TEST_CHECK_NEAR(out_cfg.zones[j].pid_kp, kp[j], 1e-9, "pid_kp survives byte-identically");
        TEST_CHECK_NEAR(out_cfg.zones[j].pid_ki, ki[j], 1e-9, "pid_ki survives byte-identically");
        TEST_CHECK_NEAR(out_cfg.zones[j].pid_kd, kd[j], 1e-9, "pid_kd survives byte-identically");
        TEST_CHECK_NEAR(out_cfg.zones[j].max_ramp_c_per_hr, 900.0f, 1e-9, "max_ramp_c_per_hr survives unchanged");
        TEST_CHECK_NEAR(out_cfg.zones[j].max_temp_c, 80.0f, 1e-9, "max_temp_c survives unchanged");
        TEST_CHECK_NEAR(out_cfg.zones[j].min_temp_c, 0.0f, 1e-9, "min_temp_c survives unchanged");
    }
    TEST_CHECK_NEAR(out_cfg.zones[0].coupling_coeff[1], 27.32f, 1e-6, "zones[0].coupling_coeff[1] survives");
    TEST_CHECK_NEAR(out_cfg.zones[0].coupling_coeff[2], 21.72f, 1e-6, "zones[0].coupling_coeff[2] survives");
    TEST_CHECK_NEAR(out_cfg.zones[1].coupling_coeff[0], 14.30f, 1e-6, "zones[1].coupling_coeff[0] survives");
    TEST_CHECK_NEAR(out_cfg.zones[1].coupling_coeff[2], 22.15f, 1e-6, "zones[1].coupling_coeff[2] survives");
    TEST_CHECK_NEAR(out_cfg.zones[2].coupling_coeff[0], 8.33f, 1e-6, "zones[2].coupling_coeff[0] survives");
    TEST_CHECK_NEAR(out_cfg.zones[2].coupling_coeff[1], 12.42f, 1e-6, "zones[2].coupling_coeff[1] survives");

    // THE two new fields, on a REAL board config, not a synthetic one: must
    // land on the 0 sentinel and resolve through the accessor to the
    // bit-identical firmware default (20.0/0.5).
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        TEST_CHECK_NEAR(out_cfg.zones[j].error_band_c, 0.0f, 1e-9,
                        "real board's migrated zone lands error_band_c on the 0 sentinel");
        TEST_CHECK_NEAR(out_cfg.zones[j].rate_band_c_per_s, 0.0f, 1e-9,
                        "real board's migrated zone lands rate_band_c_per_s on the 0 sentinel");
        s_zones.cfg = out_cfg;
        float got_e = -1.0f, got_r = -1.0f;
        TEST_CHECK(zones_config_get_error_band_c(j, &got_e) && fabsf(got_e - 20.0f) < 1e-6,
                  "accessor resolves the real board's migrated error band to 20.0");
        TEST_CHECK(zones_config_get_rate_band_c_per_s(j, &got_r) && fabsf(got_r - 0.5f) < 1e-6,
                  "accessor resolves the real board's migrated rate band to 0.5");
    }

    nvs_test_enable(false);
    nvs_test_clear();
}

// Accessor pair for zone_cfg_t::error_band_c/::rate_band_c_per_s (PID_
// EXPANSION_PLAN.md sec 3.6g) -- same shape as
// test_ease_off_window_mult_accessor_get_set_and_range(): 0 means "use the
// firmware default," resolved by the getter, never reported verbatim (unlike
// approach_rate_cap_c_per_hr's own 0).
static void test_fuzzy_bands_accessor_get_set_and_range(void)
{
    TEST_SECTION("zones_config_get/set_error_band_c()/_rate_band_c_per_s() -- round trip, per-zone "
                 "isolation, refuse-don't-clamp bounds, and the 0 sentinel resolving to the "
                 "bit-identical firmware default (20.0/0.5), NOT reported verbatim");
    nvs_test_enable(true);
    nvs_test_clear();

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 2;
    s_zones.cfg.relay_count = 2;
    s_zones.cfg.zones[0].relay_mask = 0x01;
    s_zones.cfg.zones[0].thermo_mask = 0x01;
    s_zones.cfg.zones[0].max_temp_c = 1300.0f;
    s_zones.cfg.zones[1].relay_mask = 0x02;
    s_zones.cfg.zones[1].thermo_mask = 0x02;
    s_zones.cfg.zones[1].max_temp_c = 1300.0f;
    s_zones.cfg.timing_profile_count = 1;
    strncpy(s_zones.cfg.timing_profiles[0].name, "Default", TIMING_PROFILE_NAME_MAX_LEN);
    TEST_CHECK(nvs_save() == ESP_OK, "initial save must succeed");

    // A freshly-loaded/uncommissioned zone reads the firmware default on both axes.
    float got_e = -1.0f, got_r = -1.0f;
    TEST_CHECK(zones_config_get_error_band_c(0, &got_e) && fabsf(got_e - 20.0f) < 1e-6,
              "zone 0's error band defaults to 20.0 -- bit-identical to the removed ERROR_BAND_C constant");
    TEST_CHECK(zones_config_get_rate_band_c_per_s(0, &got_r) && fabsf(got_r - 0.5f) < 1e-6,
              "zone 0's rate band defaults to 0.5 -- bit-identical to the removed RATE_BAND_C_PER_S constant");

    // An invalid zone index is refused outright, same discipline as every
    // other per-zone accessor in this file.
    float ignored = -1.0f;
    TEST_CHECK(!zones_config_get_error_band_c(MAX31856_CHANNEL_COUNT, &ignored),
              "get(error_band_c) with an out-of-range zone index is refused");
    TEST_CHECK(!zones_config_set_error_band_c(MAX31856_CHANNEL_COUNT, 7.0f),
              "set(error_band_c) with an out-of-range zone index is refused");
    TEST_CHECK(!zones_config_get_rate_band_c_per_s(MAX31856_CHANNEL_COUNT, &ignored),
              "get(rate_band_c_per_s) with an out-of-range zone index is refused");
    TEST_CHECK(!zones_config_set_rate_band_c_per_s(MAX31856_CHANNEL_COUNT, 0.22f),
              "set(rate_band_c_per_s) with an out-of-range zone index is refused");

    // A real, measured-envelope rescale persists and reads back -- set on
    // zone 0 ONLY. Uses fuzzy_bands_envelope_20260904e_report.md's own
    // recommended figures (~6-8 degC / ~0.2-0.25 degC/s), not arbitrary ones.
    TEST_CHECK(zones_config_set_error_band_c(0, 7.0f), "set(zone 0, error_band_c=7.0) succeeds");
    TEST_CHECK(zones_config_set_rate_band_c_per_s(0, 0.22f), "set(zone 0, rate_band_c_per_s=0.22) succeeds");
    got_e = -1.0f;
    got_r = -1.0f;
    TEST_CHECK(zones_config_get_error_band_c(0, &got_e) && fabsf(got_e - 7.0f) < 1e-6,
              "get(zone 0) reads back exactly the error band just set, from LIVE state");
    TEST_CHECK(zones_config_get_rate_band_c_per_s(0, &got_r) && fabsf(got_r - 0.22f) < 1e-6,
              "get(zone 0) reads back exactly the rate band just set, from LIVE state");

    // THE per-zone isolation proof: zone 1 must NOT have moved. This is the
    // exact bug class a transposed index (or a stray single shared field)
    // would produce -- and it is the FIRST of the two mandatory negative
    // tests, exercised for real below.
    float got_e1 = -1.0f, got_r1 = -1.0f;
    TEST_CHECK(zones_config_get_error_band_c(1, &got_e1) && fabsf(got_e1 - 20.0f) < 1e-6,
              "zone 1's error band is UNTOUCHED by zone 0's set() -- still the 20.0 default, not zone 0's 7.0");
    TEST_CHECK(zones_config_get_rate_band_c_per_s(1, &got_r1) && fabsf(got_r1 - 0.5f) < 1e-6,
              "zone 1's rate band is UNTOUCHED by zone 0's set() -- still the 0.5 default, not zone 0's 0.22");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg)); // wipe the live struct, force a real reload
    bool found = false, valid = false;
    TEST_CHECK(nvs_load(&found, &valid) == ESP_OK && found && valid, "reload after set() must succeed");
    got_e = -1.0f;
    TEST_CHECK(zones_config_get_error_band_c(0, &got_e) && fabsf(got_e - 7.0f) < 1e-6,
              "7.0 survives a genuine NVS round trip, not just an in-RAM poke");
    got_e1 = -1.0f;
    TEST_CHECK(zones_config_get_error_band_c(1, &got_e1) && fabsf(got_e1 - 20.0f) < 1e-6,
              "zone 1 still reads the 20.0 default after the reload -- isolation survives a real NVS round trip");

    // Refuse, never clamp -- ceiling, the (0, MIN) sliver, and NaN, none of
    // which may silently become a different number or corrupt the live value.
    TEST_CHECK(!zones_config_set_error_band_c(0, ZONE_ERROR_BAND_C_MAX + 1.0f), "set() above the ceiling is refused");
    TEST_CHECK(!zones_config_set_error_band_c(0, ZONE_ERROR_BAND_C_MIN / 2.0f), "set() below the floor (but nonzero) is refused");
    TEST_CHECK(!zones_config_set_error_band_c(0, -1.0f), "set() of a negative value is refused");
    TEST_CHECK(!zones_config_set_error_band_c(0, NAN), "set() of NaN is refused");
    TEST_CHECK(!zones_config_set_rate_band_c_per_s(0, ZONE_RATE_BAND_C_PER_S_MAX + 1.0f), "set() above the rate ceiling is refused");
    TEST_CHECK(!zones_config_set_rate_band_c_per_s(0, ZONE_RATE_BAND_C_PER_S_MIN / 2.0f), "set() below the rate floor (but nonzero) is refused");
    TEST_CHECK(!zones_config_set_rate_band_c_per_s(0, -1.0f), "set() of a negative rate value is refused");
    TEST_CHECK(!zones_config_set_rate_band_c_per_s(0, NAN), "set() of NaN rate value is refused");
    got_e = -1.0f;
    TEST_CHECK(zones_config_get_error_band_c(0, &got_e) && fabsf(got_e - 7.0f) < 1e-6,
              "every refused set() above left the live error band at 7.0, untouched -- refuse, not clamp");

    // The 0 sentinel: legal to SET (reset to the firmware default), and
    // RESOLVED by the getter -- UNLIKE approach_rate_cap_c_per_hr's 0, this
    // is NOT reported verbatim.
    TEST_CHECK(zones_config_set_error_band_c(0, 0.0f), "set(zone 0, 0.0) -- reset to firmware default -- succeeds");
    got_e = -1.0f;
    TEST_CHECK(zones_config_get_error_band_c(0, &got_e) && fabsf(got_e - 20.0f) < 1e-6,
              "the getter resolves the stored 0 sentinel to the firmware default 20.0 -- NOT reported "
              "verbatim, unlike approach_rate_cap_c_per_hr's own 0");

    nvs_test_enable(false);
    nvs_test_clear();
}

// Accessor pair for zone_cfg_t::progress_band_c (ZONES_CFG_VERSION 21->22,
// docs/audits/consumer_without_producer_2026-09-06.md finding 1) -- same
// shape as test_fuzzy_bands_accessor_get_set_and_range() just above: 0
// means "use the firmware default" (thermal_guard.c's PROGRESS_BAND_C,
// 3.0), resolved by the getter, never reported verbatim.
static void test_progress_band_c_accessor_get_set_and_range(void)
{
    TEST_SECTION("zones_config_get/set_progress_band_c() -- round trip, per-zone isolation, "
                 "refuse-don't-clamp bounds, and the 0 sentinel resolving to the firmware default "
                 "3.0, NOT reported verbatim");
    nvs_test_enable(true);
    nvs_test_clear();

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 2;
    s_zones.cfg.relay_count = 2;
    s_zones.cfg.zones[0].relay_mask = 0x01;
    s_zones.cfg.zones[0].thermo_mask = 0x01;
    s_zones.cfg.zones[0].max_temp_c = 1300.0f;
    s_zones.cfg.zones[1].relay_mask = 0x02;
    s_zones.cfg.zones[1].thermo_mask = 0x02;
    s_zones.cfg.zones[1].max_temp_c = 1300.0f;
    s_zones.cfg.timing_profile_count = 1;
    strncpy(s_zones.cfg.timing_profiles[0].name, "Default", TIMING_PROFILE_NAME_MAX_LEN);
    TEST_CHECK(nvs_save() == ESP_OK, "initial save must succeed");

    float got = -1.0f;
    TEST_CHECK(zones_config_get_progress_band_c(0, &got) && fabsf(got - ZONE_PROGRESS_BAND_C_DEFAULT) < 1e-6,
              "zone 0's progress band defaults to 3.0 -- bit-identical to thermal_guard.c's PROGRESS_BAND_C");

    float ignored = -1.0f;
    TEST_CHECK(!zones_config_get_progress_band_c(MAX31856_CHANNEL_COUNT, &ignored),
              "get(progress_band_c) with an out-of-range zone index is refused");
    TEST_CHECK(!zones_config_set_progress_band_c(MAX31856_CHANNEL_COUNT, 7.0f),
              "set(progress_band_c) with an out-of-range zone index is refused");

    TEST_CHECK(zones_config_set_progress_band_c(0, 10.0f), "set(zone 0, progress_band_c=10.0) succeeds");
    got = -1.0f;
    TEST_CHECK(zones_config_get_progress_band_c(0, &got) && fabsf(got - 10.0f) < 1e-6,
              "get(zone 0) reads back exactly the band just set, from LIVE state");

    float got1 = -1.0f;
    TEST_CHECK(zones_config_get_progress_band_c(1, &got1) && fabsf(got1 - ZONE_PROGRESS_BAND_C_DEFAULT) < 1e-6,
              "zone 1's progress band is UNTOUCHED by zone 0's set() -- still the 3.0 default, not zone 0's 10.0");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg)); // wipe the live struct, force a real reload
    bool found = false, valid = false;
    TEST_CHECK(nvs_load(&found, &valid) == ESP_OK && found && valid, "reload after set() must succeed");
    got = -1.0f;
    TEST_CHECK(zones_config_get_progress_band_c(0, &got) && fabsf(got - 10.0f) < 1e-6,
              "10.0 survives a genuine NVS round trip, not just an in-RAM poke");

    TEST_CHECK(!zones_config_set_progress_band_c(0, ZONE_PROGRESS_BAND_C_MAX + 1.0f), "set() above the ceiling is refused");
    TEST_CHECK(!zones_config_set_progress_band_c(0, ZONE_PROGRESS_BAND_C_MIN / 2.0f), "set() below the floor (but nonzero) is refused");
    TEST_CHECK(!zones_config_set_progress_band_c(0, -1.0f), "set() of a negative value is refused");
    TEST_CHECK(!zones_config_set_progress_band_c(0, NAN), "set() of NaN is refused");
    got = -1.0f;
    TEST_CHECK(zones_config_get_progress_band_c(0, &got) && fabsf(got - 10.0f) < 1e-6,
              "every refused set() above left the live progress band at 10.0, untouched -- refuse, not clamp");

    TEST_CHECK(zones_config_set_progress_band_c(0, 0.0f), "set(zone 0, 0.0) -- reset to firmware default -- succeeds");
    got = -1.0f;
    TEST_CHECK(zones_config_get_progress_band_c(0, &got) && fabsf(got - ZONE_PROGRESS_BAND_C_DEFAULT) < 1e-6,
              "the getter resolves the stored 0 sentinel to the firmware default 3.0 -- NOT reported verbatim");

    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_NEGATIVE_wrong_zone_progress_band_read_is_caught(void)
{
    TEST_SECTION("NEGATIVE TEST -- the REAL zones_config_get_progress_band_c() must return zone "
                 "0's OWN progress_band_c, not zone 1's, after zones_config_set_progress_band_c(0, ...)");
    nvs_test_enable(true);
    nvs_test_clear();
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = MAX31856_CHANNEL_COUNT;
    s_zones.cfg.relay_count = MAX31856_CHANNEL_COUNT;

    TEST_CHECK(zones_config_set_progress_band_c(0, 10.0f), "set(zone 0, 10.0) via the REAL setter succeeds");

    float got0 = -1.0f, got1 = -1.0f;
    TEST_CHECK(zones_config_get_progress_band_c(0, &got0) && fabsf(got0 - 10.0f) < 1e-6,
              "the REAL getter reads zone 0's own 10.0 back");
    TEST_CHECK(zones_config_get_progress_band_c(1, &got1) && fabsf(got1 - ZONE_PROGRESS_BAND_C_DEFAULT) < 1e-6,
              "CAUGHT (would fail if the getter read the wrong zone): zone 1's progress_band_c is "
              "UNTOUCHED by zone 0's set() -- still resolves to the 3.0 firmware default, not zone 0's 10.0");

    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_nvs_load_from_v21_blob_defaults_progress_band_c_to_default(void)
{
    TEST_SECTION("nvs_load_from -- a v21 blob upconverts to v22: every zone's new progress_band_c "
                 "lands on the 0 sentinel, resolved by the REAL accessor to the 3.0 firmware default "
                 "-- while error_band_c/rate_band_c_per_s/relay_type/settings_source/etc survive unchanged");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v21_t src;
    memset(&src, 0, sizeof(src));
    src.version = 21;
    src.thermo_count = 2;
    src.relay_count = 2;
    src.safety_tc_type = 3;
    src.pc_link_abort_silence_ms = 45000.0f;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");

    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].max_temp_c = 1300.0f;
    src.zones[0].error_band_c = 15.0f; /* a REAL, non-default sibling field -- must survive untouched */
    src.zones[0].relay_type = 1; /* RELAY_TYPE_CONTACTOR -- must also survive untouched */
    for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
        src.zones[0].settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
    }

    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;
    for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
        src.zones[1].settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
    }

    src.crc32 = 0; // v21's own CRC is not checked on the old-version path

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v21 blob must migrate to a valid current (v22) config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");

    for (uint8_t j = 0; j < 2; j++) {
        TEST_CHECK_NEAR(out_cfg.zones[j].progress_band_c, 0.0f, 1e-9,
                        "v21 has no progress_band_c -- the raw migrated field lands on the 0 sentinel");
    }
    TEST_CHECK_NEAR(out_cfg.zones[0].error_band_c, 15.0f, 1e-6, "sibling error_band_c survives the hop unchanged");
    TEST_CHECK(out_cfg.zones[0].relay_type == 1, "sibling relay_type survives the hop unchanged");

    s_zones.cfg = out_cfg;
    float got = -1.0f;
    TEST_CHECK(zones_config_get_progress_band_c(0, &got) && fabsf(got - ZONE_PROGRESS_BAND_C_DEFAULT) < 1e-6,
              "CAUGHT (would fail if the accessor stopped resolving the sentinel): the REAL accessor "
              "resolves the migrated 0 sentinel to the 3.0 firmware default, not the raw 0.0 -- a "
              "board upgrading from v21 gets today's guard-1 arrival-band behaviour, not a near-zero "
              "band that demands a rise the moment it settles");

    nvs_test_enable(false);
    nvs_test_clear();
}

// ---------------------------------------------------------------------------
// docs/ON_OFF_ZONE_PLAN.md step 1 (ZONES_CFG_VERSION 22->23): zone_type/
// failsafe_state/hyst_c/min_on_s/min_off_s, tail-appended after
// progress_band_c. Same shape as the progress_band_c tests just above.

// Accessor pair for zone_cfg_t::zone_type.
static void test_zone_type_accessor_get_set_and_range(void)
{
    TEST_SECTION("zones_config_get/set_zone_type() -- round trip, per-zone isolation, "
                 "refuse-don't-clamp bounds, and ZONE_TYPE_HEATER==0 as the fail-safe default");
    nvs_test_enable(true);
    nvs_test_clear();

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 2;
    s_zones.cfg.relay_count = 2;
    s_zones.cfg.zones[0].relay_mask = 0x01;
    s_zones.cfg.zones[0].thermo_mask = 0x01;
    s_zones.cfg.zones[0].max_temp_c = 1300.0f;
    s_zones.cfg.zones[1].relay_mask = 0x02;
    s_zones.cfg.zones[1].thermo_mask = 0x02;
    s_zones.cfg.zones[1].max_temp_c = 1300.0f;
    s_zones.cfg.timing_profile_count = 1;
    strncpy(s_zones.cfg.timing_profiles[0].name, "Default", TIMING_PROFILE_NAME_MAX_LEN);
    TEST_CHECK(nvs_save() == ESP_OK, "initial save must succeed");

    zone_type_t got = ZONE_TYPE_ON_OFF;
    TEST_CHECK(zones_config_get_zone_type(0, &got) && got == ZONE_TYPE_HEATER,
              "a freshly-saved zone defaults to ZONE_TYPE_HEATER -- fail-safe default, not on/off");

    zone_type_t ignored;
    TEST_CHECK(!zones_config_get_zone_type(MAX31856_CHANNEL_COUNT, &ignored),
              "get(zone_type) with an out-of-range zone index is refused");
    TEST_CHECK(!zones_config_set_zone_type(MAX31856_CHANNEL_COUNT, ZONE_TYPE_ON_OFF),
              "set(zone_type) with an out-of-range zone index is refused");
    TEST_CHECK(!zones_config_set_zone_type(0, (zone_type_t)2),
              "set(zone_type) with a value past ZONE_TYPE_ON_OFF is refused, not clamped");

    TEST_CHECK(zones_config_set_zone_type(0, ZONE_TYPE_ON_OFF), "set(zone 0, ZONE_TYPE_ON_OFF) succeeds");
    got = ZONE_TYPE_HEATER;
    TEST_CHECK(zones_config_get_zone_type(0, &got) && got == ZONE_TYPE_ON_OFF,
              "get(zone 0) reads back ON_OFF, from LIVE state");

    zone_type_t got1 = ZONE_TYPE_ON_OFF;
    TEST_CHECK(zones_config_get_zone_type(1, &got1) && got1 == ZONE_TYPE_HEATER,
              "zone 1 is UNTOUCHED by zone 0's set() -- still HEATER");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg)); // wipe the live struct, force a real reload
    bool found = false, valid = false;
    TEST_CHECK(nvs_load(&found, &valid) == ESP_OK && found && valid, "reload after set() must succeed");
    got = ZONE_TYPE_HEATER;
    TEST_CHECK(zones_config_get_zone_type(0, &got) && got == ZONE_TYPE_ON_OFF,
              "ZONE_TYPE_ON_OFF survives a genuine NVS round trip, not just an in-RAM poke");

    nvs_test_enable(false);
    nvs_test_clear();
}

// Fail-safe default OFF: a zero-initialized zone_cfg_t (fresh save, partial
// form, a migrated blob) must never report zone_type == ON_OFF or
// failsafe_state == ON. This is the non-negotiable from docs/
// ON_OFF_ZONE_PLAN.md sec 5 -- checked directly against the real struct
// layout, not a mirror.
static void test_zero_initialized_zone_cfg_is_heater_and_failsafe_off(void)
{
    TEST_SECTION("a zero-initialized zone_cfg_t must default to ZONE_TYPE_HEATER and "
                 "failsafe_state==OFF -- zero must never energise a relay with nothing owning it");
    zone_cfg_t z;
    memset(&z, 0, sizeof(z));
    TEST_CHECK(z.zone_type == ZONE_TYPE_HEATER, "zero-initialized zone_type is ZONE_TYPE_HEATER (0)");
    TEST_CHECK(z.failsafe_state == 0, "zero-initialized failsafe_state is OFF (0)");
}

// Migration test (both directions per the task's own requirement): forward,
// a v22 blob upconverts to v23 with the new fields on their safe-default
// sentinels while every sibling field survives unchanged; and the round trip
// (a v23 struct with the new fields set for real survives a genuine NVS
// save/reload, same discipline test_zone_type_accessor_get_set_and_range()
// above already exercises for zone_type alone) -- there is no v23->v22
// downgrade path anywhere in this codebase (no migration ever goes
// backwards; see docs/UPDATE_PROTOCOL.md's rollback-hazard note instead),
// so "both directions" here means forward-migrate and round-trip, not an
// actual downgrade converter.
static void test_nvs_load_from_v22_blob_defaults_zone_type_and_failsafe_to_zero(void)
{
    TEST_SECTION("nvs_load_from -- a v22 blob upconverts to v23: every zone's new zone_type/"
                 "failsafe_state/hyst_c/min_on_s/min_off_s land on their 0 sentinels, while "
                 "progress_band_c/error_band_c/relay_type/settings_source survive unchanged");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v22_t src;
    memset(&src, 0, sizeof(src));
    src.version = 22;
    src.thermo_count = 2;
    src.relay_count = 2;
    src.safety_tc_type = 3;
    src.pc_link_abort_silence_ms = 45000.0f;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");

    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].max_temp_c = 1300.0f;
    src.zones[0].error_band_c = 15.0f; /* a REAL, non-default sibling field -- must survive untouched */
    src.zones[0].relay_type = 1;       /* RELAY_TYPE_CONTACTOR -- must also survive untouched */
    src.zones[0].progress_band_c = 5.0f; /* likewise -- v22's own newest field must survive too */
    for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
        src.zones[0].settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
    }

    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;
    for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
        src.zones[1].settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
    }

    src.crc32 = 0; // v22's own CRC is not checked on the old-version path

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v22 blob must migrate to a valid current (v23) config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");

    for (uint8_t j = 0; j < 2; j++) {
        TEST_CHECK(out_cfg.zones[j].zone_type == ZONE_TYPE_HEATER,
                  "v22 has no zone_type -- the raw migrated field lands on the ZONE_TYPE_HEATER (0) sentinel");
        TEST_CHECK(out_cfg.zones[j].failsafe_state == 0,
                  "v22 has no failsafe_state -- lands on the OFF (0) sentinel, never ON");
        TEST_CHECK_NEAR(out_cfg.zones[j].hyst_c, 0.0f, 1e-9, "v22 has no hyst_c -- lands on the 0 sentinel");
        TEST_CHECK(out_cfg.zones[j].min_on_s == 0, "v22 has no min_on_s -- lands on the 0 sentinel");
        TEST_CHECK(out_cfg.zones[j].min_off_s == 0, "v22 has no min_off_s -- lands on the 0 sentinel");
    }
    TEST_CHECK_NEAR(out_cfg.zones[0].error_band_c, 15.0f, 1e-6, "sibling error_band_c survives the hop unchanged");
    TEST_CHECK(out_cfg.zones[0].relay_type == 1, "sibling relay_type survives the hop unchanged");
    TEST_CHECK_NEAR(out_cfg.zones[0].progress_band_c, 5.0f, 1e-6, "sibling progress_band_c survives the hop unchanged");

    /* Round-trip leg: save the migrated config back out (with a real,
     * nonzero opt-in on zone 0) and reload it via the CURRENT (v23) path --
     * not another historical blob -- to prove the new fields persist for
     * real, not only across a migration. */
    s_zones.cfg = out_cfg;
    TEST_CHECK(zones_config_set_zone_type(0, ZONE_TYPE_ON_OFF), "set(zone 0, ON_OFF) on the migrated config succeeds");
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    TEST_CHECK(nvs_load(&found, &valid) == ESP_OK && found && valid, "reload of the v23-native save must succeed");
    zone_type_t rt = ZONE_TYPE_HEATER;
    TEST_CHECK(zones_config_get_zone_type(0, &rt) && rt == ZONE_TYPE_ON_OFF,
              "ZONE_TYPE_ON_OFF survives a genuine v23-native NVS round trip");

    nvs_test_enable(false);
    nvs_test_clear();
}

// docs/audits/high_temperature_transfer_analysis_2026-09-08.md item 4: a v23
// blob upconverts to v24 with model_fit_temp_c/model_fit_ambient_c landing on
// the ZONE_MODEL_FIT_TEMP_UNKNOWN sentinel -- NEVER 0.0f, which would read as
// a plausible genuine ambient fit rather than "no context recorded" -- even
// for a zone whose model_k_dc/tau/dead_time ARE real, pre-existing values.
// Round-trip leg proves a real fit context written after migration survives
// a genuine v24-native NVS reload, same discipline the v22->v23 test above
// uses for zone_type.
static void test_nvs_load_from_v23_blob_defaults_model_fit_context_to_unknown(void)
{
    TEST_SECTION("nvs_load_from -- a v23 blob upconverts to v24: every zone's new "
                 "model_fit_temp_c/model_fit_ambient_c land on the ZONE_MODEL_FIT_TEMP_UNKNOWN "
                 "sentinel, never 0.0f, even when model_k_dc/tau/dead_time are real pre-existing values");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v23_t src;
    memset(&src, 0, sizeof(src));
    src.version = 23;
    src.thermo_count = 2;
    src.relay_count = 2;
    src.safety_tc_type = 3;
    src.pc_link_abort_silence_ms = 45000.0f;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");

    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].max_temp_c = 1300.0f;
    /* A REAL, pre-existing fit -- exactly the "old fit, unknown operating
     * point" scenario docs/audits/high_temperature_transfer_analysis_
     * 2026-09-08.md's backfill requirement targets. */
    src.zones[0].model_k_dc = 500.0f;
    src.zones[0].model_tau_s = 300.0f;
    src.zones[0].model_dead_time_s = 20.0f;
    for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
        src.zones[0].settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
    }

    src.zones[1].relay_mask = 0x02;
    src.zones[1].thermo_mask = 0x02;
    src.zones[1].max_temp_c = 1250.0f;
    for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
        src.zones[1].settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
    }

    src.crc32 = 0; // v23's own CRC is not checked on the old-version path

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);

    TEST_CHECK(err == ESP_OK, "no NVS error");
    TEST_CHECK(found && valid, "a well-formed v23 blob must migrate to a valid current (v24) config");
    TEST_CHECK(out_cfg.version == ZONES_CFG_VERSION, "migrated config is stamped the current version");

    for (uint8_t j = 0; j < 2; j++) {
        TEST_CHECK(out_cfg.zones[j].model_fit_temp_c == ZONE_MODEL_FIT_TEMP_UNKNOWN,
                  "v23 has no model_fit_temp_c -- the raw migrated field lands on the UNKNOWN sentinel, "
                  "never 0.0f");
        TEST_CHECK(out_cfg.zones[j].model_fit_ambient_c == ZONE_MODEL_FIT_TEMP_UNKNOWN,
                  "v23 has no model_fit_ambient_c -- lands on the UNKNOWN sentinel, never 0.0f");
    }
    TEST_CHECK_NEAR(out_cfg.zones[0].model_k_dc, 500.0f, 1e-6, "sibling model_k_dc survives the hop unchanged");
    TEST_CHECK_NEAR(out_cfg.zones[0].model_tau_s, 300.0f, 1e-6, "sibling model_tau_s survives the hop unchanged");
    TEST_CHECK_NEAR(out_cfg.zones[0].model_dead_time_s, 20.0f, 1e-6,
                    "sibling model_dead_time_s survives the hop unchanged");

    /* Round-trip leg: a real fit context written after migration must
     * survive a genuine v24-native NVS reload -- not only migrate onto the
     * sentinel. */
    s_zones.cfg = out_cfg;
    TEST_CHECK(zones_config_set_model_fit_context(0, 875.0f, 22.0f),
              "set(zone 0, real fit context) on the migrated config succeeds");
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    TEST_CHECK(nvs_load(&found, &valid) == ESP_OK && found && valid, "reload of the v24-native save must succeed");
    float rt_temp = 0.0f, rt_ambient = 0.0f;
    TEST_CHECK(zones_config_get_model_fit_context(0, &rt_temp, &rt_ambient) && rt_temp == 875.0f &&
                  rt_ambient == 22.0f,
              "the real fit context survives a genuine v24-native NVS round trip, bit-identical");

    nvs_test_enable(false);
    nvs_test_clear();
}

// Opus review finding 10 (2026-09-10): the ZONES_CFG_VERSION N-1->N bump
// moves the immediately-prior version OFF the current-version path (full CRC
// check) and ONTO the older-version migration path, which discarded every
// historical version's own crc32 unchecked -- so a blob one version behind
// current, with a single flipped bit, could silently migrate. zones_config_
// migrate.c's decode_zones_blob() verifies THAT ONE prior version's own
// crc32 (computed the same "everything before crc32, then 4 zero bytes" way
// as the current-version check) BEFORE calling its converter -- narrowly,
// for ZONES_CFG_VERSION-1 only, not every historical version (see that
// fix's own comment for why). The 25->26 pass (autotune_baseline_k_dc)
// moves this coverage from v24 (tested until that pass) to v25 -- these
// tests were updated in lockstep rather than left describing a version this
// gate no longer covers, so a green suite still means what it claims.
static void make_minimal_valid_v25_blob(zones_cfg_v25_t *src)
{
    memset(src, 0, sizeof(*src));
    src->version = 25;
    src->thermo_count = 2;
    src->relay_count = 2;
    src->safety_tc_type = 3;
    src->pc_link_abort_silence_ms = 45000.0f;
    src->timing_profile_count = 1;
    snprintf(src->timing_profiles[0].name, sizeof(src->timing_profiles[0].name), "Default");
    src->zones[0].relay_mask = 0x01;
    src->zones[0].thermo_mask = 0x01;
    src->zones[0].max_temp_c = 1300.0f;
    src->zones[0].model_fit_temp_c = ZONE_MODEL_FIT_TEMP_UNKNOWN;
    src->zones[0].model_fit_ambient_c = ZONE_MODEL_FIT_TEMP_UNKNOWN;
    for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
        src->zones[0].settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
    }
    src->zones[1].relay_mask = 0x02;
    src->zones[1].thermo_mask = 0x02;
    src->zones[1].max_temp_c = 1250.0f;
    src->zones[1].model_fit_temp_c = ZONE_MODEL_FIT_TEMP_UNKNOWN;
    src->zones[1].model_fit_ambient_c = ZONE_MODEL_FIT_TEMP_UNKNOWN;
    for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
        src->zones[1].settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
    }
    // Same "everything before crc32, then 4 zero bytes" computation
    // zones_config_migrate.c's decode_zones_blob() itself uses to verify a
    // v25 blob (check_link_impl_isolation.ps1: no separate CRC-named
    // function here, INLINE only, matching production's own inline
    // computation rather than a locally reimplemented routine -- this is
    // esp_crc32_le(), CommonFW's own CRC32 primitive, called twice, not a
    // hand-rolled CRC algorithm).
    {
        static const uint8_t zero4[sizeof(uint32_t)] = {0};
        uint32_t crc = esp_crc32_le(0, (const uint8_t *)src, offsetof(zones_cfg_v25_t, crc32));
        src->crc32 = esp_crc32_le(crc, zero4, sizeof(zero4));
    }
}

static void test_decode_zones_blob_accepts_a_v25_blob_with_a_correct_crc(void)
{
    TEST_SECTION("decode_zones_blob -- a v25 blob with a CORRECT crc32 still migrates to v26 "
                 "(opus review finding 10's fix must not refuse good blobs)");

    zones_cfg_v25_t src;
    make_minimal_valid_v25_blob(&src);

    zones_cfg_t out;
    const char *reason = "unset";
    zones_decode_result_t r = zones_config_json_decode_blob(&src, sizeof(src), &out, &reason);
    TEST_CHECK(r == ZONES_DECODE_OK, "a well-formed, correctly-CRC'd v25 blob migrates cleanly");
    TEST_CHECK(out.version == ZONES_CFG_VERSION, "migrated config is stamped the current (v26) version");
    TEST_CHECK(out.thermo_count == 2, "sibling field survives the hop unchanged");
    TEST_CHECK(out.zones[0].autotune_baseline_k_dc == 0.0f,
               "a migrated zone's new autotune_baseline_k_dc reads the 'not recorded yet' sentinel, "
               "not garbage from beyond the v25 blob's own tail");
}

static void test_decode_zones_blob_refuses_a_v25_blob_with_a_corrupted_crc(void)
{
    TEST_SECTION("decode_zones_blob -- NEGATIVE TEST: a v25 blob with a flipped bit and its OLD "
                 "(now-mismatched) crc32 is refused, not silently migrated (opus review finding 10)");

    zones_cfg_v25_t src;
    make_minimal_valid_v25_blob(&src);
    // Flip one bit of a real, in-range value (a PID gain) -- exactly finding
    // 10's own worked example of a corruption this check must catch that
    // zones_config_json_validate() alone would not (the flipped value can
    // easily still be in-range).
    src.zones[0].pid_kp = 12.5f;
    // valid CRC over the pre-corruption bytes -- same inline computation as
    // make_minimal_valid_v25_blob() above (check_link_impl_isolation.ps1:
    // no separate CRC-named function, just esp_crc32_le() called directly).
    {
        static const uint8_t zero4[sizeof(uint32_t)] = {0};
        uint32_t crc = esp_crc32_le(0, (const uint8_t *)&src, offsetof(zones_cfg_v25_t, crc32));
        src.crc32 = esp_crc32_le(crc, zero4, sizeof(zero4));
    }
    uint8_t *raw = (uint8_t *)&src;
    raw[offsetof(zones_cfg_v25_t, zones[0].pid_kp)] ^= 0x01; // corrupt AFTER computing the CRC

    zones_cfg_t out;
    const char *reason = "unset";
    zones_decode_result_t r = zones_config_json_decode_blob(&src, sizeof(src), &out, &reason);
    TEST_CHECK(r == ZONES_DECODE_CORRUPT, "a v25 blob whose bytes no longer match its own crc32 is refused");
    TEST_CHECK(out.thermo_count == 0, "a refused blob leaves *out zeroed, never a half-migrated struct");
}

// docs/ON_OFF_ZONE_PLAN.md sec 1/sec 2 predicates.
static void test_zone_is_on_off_and_zone_needs_ceiling(void)
{
    TEST_SECTION("zone_is_on_off()/zone_needs_ceiling() -- the predicates every guard/ramp-lock/"
                 "coupling/autotune exclusion in this pass is built on");
    nvs_test_enable(true);
    nvs_test_clear();

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;
    s_zones.cfg.relay_count = 3;
    s_zones.cfg.zones[0].relay_mask = 0x01; s_zones.cfg.zones[0].thermo_mask = 0x01; s_zones.cfg.zones[0].max_temp_c = 1300.0f;
    s_zones.cfg.zones[1].relay_mask = 0x02; s_zones.cfg.zones[1].thermo_mask = 0x02; s_zones.cfg.zones[1].max_temp_c = 1300.0f;
    s_zones.cfg.zones[2].relay_mask = 0x04; /* zone 2: on/off, NO thermocouple assigned */
    s_zones.cfg.timing_profile_count = 1;
    strncpy(s_zones.cfg.timing_profiles[0].name, "Default", TIMING_PROFILE_NAME_MAX_LEN);
    TEST_CHECK(nvs_save() == ESP_OK, "initial save must succeed");

    TEST_CHECK(!zone_is_on_off(0), "zone 0 (HEATER, default) is not on/off");
    TEST_CHECK(zone_needs_ceiling(0), "a HEATER zone always needs a ceiling");

    TEST_CHECK(zones_config_set_zone_type(1, ZONE_TYPE_ON_OFF), "set zone 1 to ON_OFF");
    TEST_CHECK(zone_is_on_off(1), "zone 1 now reports on/off");
    TEST_CHECK(zone_needs_ceiling(1), "an on/off zone WITH a thermocouple still needs a ceiling");

    TEST_CHECK(zones_config_set_zone_type(2, ZONE_TYPE_ON_OFF), "set zone 2 to ON_OFF");
    TEST_CHECK(zone_is_on_off(2), "zone 2 reports on/off");
    TEST_CHECK(!zone_needs_ceiling(2), "an on/off zone with NO thermocouple does not need a ceiling");

    TEST_CHECK(!zone_is_on_off(MAX31856_CHANNEL_COUNT), "an out-of-range index is fail-closed to \"not on/off\"");
    TEST_CHECK(zone_needs_ceiling(MAX31856_CHANNEL_COUNT), "an out-of-range index is fail-closed to \"needs a ceiling\"");

    nvs_test_enable(false);
    nvs_test_clear();
}

// ---------------------------------------------------------------------------
// MANDATORY negative tests (PID_EXPANSION_PLAN.md sec 3.6g task instructions).
// Both call the REAL production functions through the same direct seam
// test_fuzzy_bands_accessor_get_set_and_range() above already uses -- no
// hand-reimplemented mirror, no self-flipped boolean. Verified for real
// (2026-09-04 review pass): the production function was broken, this test
// was shown to fail with the broken build, then the function was restored
// and the test shown passing again. See the task's final report for the
// verbatim before/after output.
//
// Test 1 exercises zones_config_set_error_band_c()/zones_config_get_error_
// band_c() directly -- the same "wrong zone's value" bug class approach_
// rate_cap_c_per_hr's own negative test caught, but caught here by actually
// calling the accessor pair rather than a mirror of it.
// Test 2 exercises the REAL v18->v19 migration (nvs_load_from() over a
// staged v18 blob, via stage_zones_blob()) followed by the REAL accessor --
// proving the 0-sentinel-resolves-to-20.0 contract along the actual
// migration+accessor path, not a hand-copied resolution formula.
// ---------------------------------------------------------------------------

static void test_NEGATIVE_wrong_zone_band_read_is_caught(void)
{
    TEST_SECTION("NEGATIVE TEST 1/2 -- the REAL zones_config_get_error_band_c() must return zone "
                 "0's OWN error_band_c, not zone 1's, after zones_config_set_error_band_c(0, ...) -- "
                 "a wrong-zone read (or a stray shared field) must be caught by name, not pass silently");
    nvs_test_enable(true);
    nvs_test_clear();
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = MAX31856_CHANNEL_COUNT;
    s_zones.cfg.relay_count = MAX31856_CHANNEL_COUNT;

    TEST_CHECK(zones_config_set_error_band_c(0, 7.0f), "set(zone 0, 7.0) via the REAL setter succeeds");

    float got0 = -1.0f, got1 = -1.0f;
    TEST_CHECK(zones_config_get_error_band_c(0, &got0) && fabsf(got0 - 7.0f) < 1e-6,
              "the REAL getter reads zone 0's own 7.0 back");
    TEST_CHECK(zones_config_get_error_band_c(1, &got1) && fabsf(got1 - 20.0f) < 1e-6,
              "CAUGHT (would fail if the getter read the wrong zone): zone 1's error_band_c is "
              "UNTOUCHED by zone 0's set() -- still resolves to the 20.0 firmware default, not "
              "zone 0's 7.0");

    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_NEGATIVE_migration_default_of_zero_instead_of_20_is_caught(void)
{
    TEST_SECTION("NEGATIVE TEST 2/2 -- a v18 blob migrated through the REAL nvs_load_from() must "
                 "leave error_band_c resolving, via the REAL zones_config_get_error_band_c(), to the "
                 "20.0 firmware default -- not the raw 0 sentinel leaking out as if it meant 0.0 degC");
    nvs_test_enable(true);
    nvs_test_clear();

    zones_cfg_v18_t src;
    memset(&src, 0, sizeof(src));
    src.version = 18;
    src.thermo_count = 1;
    src.relay_count = 1;
    src.timing_profile_count = 1;
    snprintf(src.timing_profiles[0].name, sizeof(src.timing_profiles[0].name), "Default");
    src.zones[0].relay_mask = 0x01;
    src.zones[0].thermo_mask = 0x01;
    src.zones[0].max_temp_c = 1300.0f;
    src.zones[0].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
    src.crc32 = 0; // v18's own CRC is not checked on the old-version path

    stage_zones_blob(&src, sizeof(src));

    zones_cfg_t out_cfg;
    bool found = false, valid = false;
    esp_err_t err = nvs_load_from("kiln_nvs", &out_cfg, &found, &valid);
    TEST_CHECK(err == ESP_OK && found && valid, "the v18 blob migrates via the REAL migration path");

    TEST_CHECK_NEAR(out_cfg.zones[0].error_band_c, 0.0f, 1e-9,
                    "v18 has no error_band_c -- the raw migrated field lands on the 0 sentinel");

    s_zones.cfg = out_cfg;
    float got_e = -1.0f;
    TEST_CHECK(zones_config_get_error_band_c(0, &got_e) && fabsf(got_e - 20.0f) < 1e-6,
              "CAUGHT (would fail if the accessor stopped resolving the sentinel): the REAL accessor "
              "resolves the migrated 0 sentinel to the bit-identical 20.0 firmware default, not the "
              "raw 0.0 -- a board upgrading from v18 gets today's behaviour, not a near-zero "
              "membership band that fires on sensor noise");

    nvs_test_enable(false);
    nvs_test_clear();
}

// ---------------------------------------------------------------------------
// PID_EXPANSION_PLAN.md line ~864's three negative tests for zone settings
// inheritance. The feature itself (self-reference refused at the door) was
// already covered above; these three were missing entirely.
// ---------------------------------------------------------------------------

// 1. Save/reload round-trip: a zone's settings_source link (a real OTHER
// zone index, not CUSTOM) must survive an nvs_save()/nvs_load() cycle intact
// -- same shape as test_nvs_save_load_round_trip_current_version() above,
// narrowed to the one field this task calls out by name.
static void test_settings_source_save_reload_inheritance_round_trip(void)
{
    TEST_SECTION("settings_source -- a real inheritance link (zone 1 copies zone 2) survives "
                 "nvs_save()/nvs_load() and still names the right source zone afterward");
    nvs_test_enable(true);
    nvs_test_clear();

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;
    s_zones.cfg.relay_count = 3;
    s_zones.cfg.timing_profile_count = 1;
    strncpy(s_zones.cfg.timing_profiles[0].name, "Default", TIMING_PROFILE_NAME_MAX_LEN);
    for (uint8_t i = 0; i < 3; i++) {
        zone_cfg_t *z = &s_zones.cfg.zones[i];
        z->relay_mask = (uint8_t)(1u << i);
        z->thermo_mask = (uint8_t)(1u << i);
        z->max_temp_c = 1300.0f;
        z->timing_profile = 0;
    }
    for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
        s_zones.cfg.zones[0].settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
    }
    s_zones.cfg.zones[1].settings_source[SRC_GROUP_LIMITS] = 2; /* zone 1 inherits from zone 2 */
    for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
        s_zones.cfg.zones[2].settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
    }
    /* A distinctive value on the SOURCE zone, so "resolves to the right
     * source zone" is checkable, not just "some link survived". */
    s_zones.cfg.zones[2].tc_type = 5;
    s_zones.cfg.zones[2].pid_kp = 7.25f;

    esp_err_t save_err = nvs_save();
    TEST_CHECK(save_err == ESP_OK, "nvs_save() must succeed");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg)); // wipe the live struct, force a real reload
    bool found = false, valid = false;
    esp_err_t load_err = nvs_load(&found, &valid);
    TEST_CHECK(load_err == ESP_OK && found && valid, "nvs_load() must succeed and report found+valid");

    uint8_t src = 0xAA;
    TEST_CHECK(zones_config_get_settings_source(1, SRC_GROUP_LIMITS, &src) && src == 2,
              "zone 1's settings_source (2, a real zone) survives the round trip exactly");
    TEST_CHECK(s_zones.cfg.zones[0].settings_source[SRC_GROUP_LIMITS] == ZONE_SETTINGS_SOURCE_CUSTOM,
              "zone 0's CUSTOM marker round-trips too, not disturbed by zone 1's link");
    /* "resolves to the right source zone": follow the link this test set up
     * and confirm the values sitting there are still the source zone's own
     * -- not zeroed, not zone 1's, proving the link actually points
     * somewhere real and reloadable, not just a surviving integer. */
    TEST_CHECK(s_zones.cfg.zones[src].tc_type == 5, "the zone settings_source(1) names still holds its own tc_type");
    TEST_CHECK_NEAR(s_zones.cfg.zones[src].pid_kp, 7.25f, 1e-6,
                    "and still holds its own pid_kp -- zone 1's link resolves to the correct data");

    nvs_test_enable(false);
    nvs_test_clear();
}

// 2. Cycle refusal -- REGRESSION TEST for the fix (was: a documented defect).
//
// PID_EXPANSION_PLAN.md line ~864 requires that a configuration forming an
// inheritance cycle "must not be storable or must collapse safely". The
// storage layer (zones_config_set_settings_source(), SRC_GROUP_LIMITS, and zones_http_parse_zone_fields()'s
// identical z%u_settings_source door) used to refuse only SELF-reference
// (settings_source == zone_index) -- see both functions' own comments -- and
// never checked the TARGET zone's own settings_source, so a genuine 2-zone
// cycle (zone 0 -> zone 1, zone 1 -> zone 0) was reachable by two ordinary,
// individually-legal setter calls -- exactly the "hand-edited backup" path an
// opus reviewer flagged: backup_http.c's importer commits settings_source
// through this same setter, one zone entry at a time.
//
// Both doors now run zones_config_json_settings_source_chain_has_cycle() (a bounded
// visited-set walk, capped at MAX31856_CHANNEL_COUNT hops so it terminates
// even against an already-corrupt stored chain) before committing a new
// link, so the second call of any cycle-closing pair is refused and the
// live config is left exactly where the first call put it -- refuse, not
// partially apply. This test used to assert the opposite (the cycle IS
// storable) as a positive proof of the then-open defect; it now asserts the
// fix: the SAME sequence of calls is refused at the second link, for both
// the 2-cycle and the 3-cycle, through both doors (the direct setter and
// zones_http_parse_zone_fields()), and a legal (acyclic) chain is still storable so
// this isn't a blanket refusal.
static void test_settings_source_two_and_three_zone_cycles_are_refused(void)
{
    TEST_SECTION("settings_source -- FIX: a 2-zone and a 3-zone inheritance cycle are BOTH refused "
                 "(only the SECOND, cycle-closing link of each pair) -- PID_EXPANSION_PLAN.md line ~864's "
                 "'must not be storable' requirement, now enforced by a real chain-walk");
    nvs_test_enable(true);
    nvs_test_clear();
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;
    /* Real decoded configs never leave an in-use zone's settings_source at
     * raw 0 unintentionally (see convert_zone_v9()'s "NEVER 0" comment) --
     * 0 is a REAL, DIFFERENT settings_source value ("copy zone 0"), not a
     * "not set" sentinel, so a plain memset(0) here would make zones 1 and 2
     * look like they already explicitly link to zone 0 before this test
     * ever touches them. Seed CUSTOM so the starting config matches what
     * real firmware actually produces, not a memset artifact. */
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
            s_zones.cfg.zones[z].settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
        }
    }

    // ---- Direct setter, 2-cycle: zone 0 -> zone 1, then zone 1 -> zone 0 ----
    TEST_CHECK(zones_config_set_settings_source(0, SRC_GROUP_LIMITS, 1), "zone 0 -> zone 1 is accepted on its own "
              "(0 -> 1 -> Custom terminates cleanly, no cycle yet)");
    TEST_CHECK(!zones_config_set_settings_source(1, SRC_GROUP_LIMITS, 0), "zone 1 -> zone 0 is REFUSED -- it would close "
              "a genuine mutual cycle (0 -> 1 -> 0), and the chain-walk now catches that the first call "
              "alone could not");
    uint8_t s0 = 0xAA, s1 = 0xAA;
    TEST_CHECK(zones_config_get_settings_source(0, SRC_GROUP_LIMITS, &s0) && s0 == 1, "zone 0's link is still 1 (the "
              "legal first call)");
    TEST_CHECK(zones_config_get_settings_source(1, SRC_GROUP_LIMITS, &s1) && s1 == ZONE_SETTINGS_SOURCE_CUSTOM,
              "zone 1 is still Custom -- the refused call left it untouched, not partially applied");

    // ---- Direct setter, 3-cycle: 0 -> 1 already set above; 1 -> 2, then 2 -> 0 ----
    TEST_CHECK(zones_config_set_settings_source(1, SRC_GROUP_LIMITS, 2), "zone 1 -> zone 2 is accepted (1 -> 2 -> Custom, "
              "no cycle yet -- zone 0's own 0 -> 1 link doesn't participate in THIS chain's termination "
              "check, only in whether closing it later would cycle)");
    TEST_CHECK(!zones_config_set_settings_source(2, SRC_GROUP_LIMITS, 0), "zone 2 -> zone 0 is REFUSED -- it would close "
              "the 3-zone cycle 0 -> 1 -> 2 -> 0");
    uint8_t s2 = 0xAA;
    TEST_CHECK(zones_config_get_settings_source(0, SRC_GROUP_LIMITS, &s0) && s0 == 1 &&
              zones_config_get_settings_source(1, SRC_GROUP_LIMITS, &s1) && s1 == 2 &&
              zones_config_get_settings_source(2, SRC_GROUP_LIMITS, &s2) && s2 == ZONE_SETTINGS_SOURCE_CUSTOM,
              "zone 0 -> 1 -> 2 (a legal, acyclic chain) is exactly what's stored -- the refused "
              "2 -> 0 call left zone 2 at Custom, not half-applied");

    // ---- Self-reference: still refused (unchanged behaviour, not a regression) ----
    TEST_CHECK(!zones_config_set_settings_source(0, SRC_GROUP_LIMITS, 0), "self-reference is still refused directly, "
              "independent of the longer-chain guard added above");

    // ---- Positive control: a legal chain (no cycle) is still storable ----
    // Reset zone 2 back to Custom and re-close the SAME 0 -> 1 -> 2 chain
    // from a clean start, proving the guard above refuses ONLY the
    // cycle-closing link and does not over-reject an ordinary acyclic chain.
    TEST_CHECK(zones_config_set_settings_source(2, SRC_GROUP_LIMITS, ZONE_SETTINGS_SOURCE_CUSTOM),
              "zone 2 reset to Custom (2 -> Custom is never a cycle)");
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
            s_zones.cfg.zones[z].settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
        }
    }
    TEST_CHECK(zones_config_set_settings_source(0, SRC_GROUP_LIMITS, 1), "0 -> 1 (fresh chain)");
    TEST_CHECK(zones_config_set_settings_source(1, SRC_GROUP_LIMITS, 2), "1 -> 2 (fresh chain, still acyclic: 0->1->2->Custom)");
    TEST_CHECK(zones_config_get_settings_source(0, SRC_GROUP_LIMITS, &s0) && s0 == 1 &&
              zones_config_get_settings_source(1, SRC_GROUP_LIMITS, &s1) && s1 == 2,
              "the legal 0 -> 1 -> 2 chain is fully storable -- the guard does not over-reject a chain "
              "that never revisits a zone");

    // ---- The same guard through zones_http_parse_zone_fields() (the POST/import door) ----
    // Current live state: zone 0 -> 1 -> 2 -> Custom (set directly above).
    // Free zone 0 back to Custom, then point zone 1 at zone 0 directly (a
    // fresh, legal link: 1 -> 0 -> Custom) so a SUBSEQUENT zones_http_parse_zone_fields()
    // call closing zone 0 back onto zone 1 is a genuine 2-cycle, not a
    // freshly-created one.
    TEST_CHECK(zones_config_set_settings_source(0, SRC_GROUP_LIMITS, ZONE_SETTINGS_SOURCE_CUSTOM),
              "zone 0 reset to Custom, freeing it to be pointed at");
    TEST_CHECK(zones_config_set_settings_source(1, SRC_GROUP_LIMITS, 0), "zone 1 -> zone 0 now legal (0 is Custom): "
              "live state is now zone 0 = Custom, zone 1 -> 0");
    zone_cfg_t current0 = make_stored_zone();
    zone_cfg_t out0;
    const char *reason = "unset";
    TEST_CHECK(!post_body_with_extra("z0_settings_source=1", &reason, &out0),
              "zones_http_parse_zone_fields() (the POST/import door) REFUSES the identical 2-cycle a whole-page POST "
              "or backup_http.c's importer would create: zone 1 already points at zone 0 live, so posting "
              "z0_settings_source=1 (zone 0 -> zone 1) would close 0 -> 1 -> 0 -- refused through this "
              "door too, not just the direct setter");
    TEST_CHECK(reason && strstr(reason, "settings_source") != NULL && strstr(reason, "cycle") != NULL,
              "the refusal names the field and calls out the cycle specifically");
    (void)current0;

    nvs_test_enable(false);
    nvs_test_clear();
}

// 3. Shared-channel tc_type agreement. PID_EXPANSION_PLAN.md's corrected
// rule (2026-08-30, after a real bug): a zone with its OWN custom settings
// writes tc_type as the identity -- zone i's field sets channel i's type,
// full stop. The mask fan-out ("copy the terminal zone's type onto every
// channel this zone's thermo_mask actually reads") is exclusively a
// zones_page.html UI computation performed BEFORE the value is POSTed --
// PID_EXPANSION_PLAN.md's own "Storage" note says so explicitly ("this is a
// UI-level convenience... not a new inheritance layer in the firmware...
// an inheriting zone writes the resolved values into its own zone_cfg_t
// exactly as if they had been typed"). There is no fan-out, and no
// settings_source-conditioned override, anywhere in zones_http.c's storage
// layer -- confirmed by grep: tc_type is written by exactly one path
// (zones_http_parse_zone_fields()'s z%u_tctype handling / zones_config_set_tc_type()),
// keyed ONLY on the zone/channel index the caller names, never touched by
// settings_source or thermo_mask.
//
// This test is the storage-layer half of that contract: it proves tc_type
// is written per-index, independent of that same zone's settings_source --
// i.e. firmware never silently re-types a channel based on inheritance
// state, which is exactly the bug PID_EXPANSION_PLAN.md's correction
// describes ("an operator setting channel 2 silently re-types channel 0").
// Two zones sharing a channel getting DIFFERENT tc_type values here (last-
// write-wins, as the plan explicitly allows) is the proof: if firmware were
// doing its own fan-out/sync, zone 1's tc_type would have been forced to
// match zone 0's once both were saved.
static void test_tc_type_write_is_identity_independent_of_settings_source(void)
{
    TEST_SECTION("tc_type -- storage writes it as the identity (zone i sets channel i), independent of "
                 "that zone's settings_source -- no firmware-side fan-out/re-sync to reintroduce the "
                 "'wrong channel re-typed' bug PID_EXPANSION_PLAN.md's 3.5 correction describes");
    nvs_test_enable(true);
    nvs_test_clear();
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 3;

    // Zone 0: CUSTOM settings, its own tc_type.
    TEST_CHECK(zones_config_set_settings_source(0, SRC_GROUP_LIMITS, ZONE_SETTINGS_SOURCE_CUSTOM), "zone 0 is Custom");
    TEST_CHECK(zones_config_set_tc_type(0, 2), "zone 0's own channel is set to type 2");

    // Zone 1: inherits from a DIFFERENT terminal zone (zone 0), but is
    // itself posted with tc_type=2 too -- what zones_page.html's UI would
    // have written after resolving/fanning-out zone 0's type onto zone 1's
    // own channel index BEFORE the save, per the plan's corrected rule. The
    // firmware-side assertion here is simply that whatever value comes in
    // for zone 1's own index is what gets stored for zone 1's own index --
    // it does not derive or override it from settings_source itself.
    TEST_CHECK(zones_config_set_settings_source(1, SRC_GROUP_LIMITS, 0), "zone 1 is set to inherit from zone 0");
    TEST_CHECK(zones_config_set_tc_type(1, 2), "zone 1's own channel is written with the fanned-out value (2)");

    // Zone 2: also CUSTOM, but its OWN channel is a genuinely different
    // type. If firmware secretly synced tc_type across zones sharing a
    // settings_source chain or a mask, this would have been dragged to 2 as
    // well by one of the writes above -- last-write-wins per zone's own
    // index is the only rule actually enforced.
    TEST_CHECK(zones_config_set_settings_source(2, SRC_GROUP_LIMITS, ZONE_SETTINGS_SOURCE_CUSTOM), "zone 2 is Custom");
    TEST_CHECK(zones_config_set_tc_type(2, 6), "zone 2's own channel is set to a DIFFERENT type (6)");

    uint8_t t0 = 0xFF, t1 = 0xFF, t2 = 0xFF;
    TEST_CHECK(zones_config_get_tc_type(0, &t0) && t0 == 2, "zone 0's tc_type is exactly what zone 0 set");
    TEST_CHECK(zones_config_get_tc_type(1, &t1) && t1 == 2,
              "zone 1's tc_type is exactly what was posted for zone 1's OWN index -- identity write, "
              "not derived from settings_source at the storage layer");
    TEST_CHECK(zones_config_get_tc_type(2, &t2) && t2 == 6,
              "zone 2's tc_type is untouched by zones 0/1's writes -- no cross-zone fan-out happens in "
              "firmware storage, proving the mask fan-out described in the plan is exclusively the UI's "
              "job, done before the POST, never re-derived or overridden here");

    // Now change zone 1's settings_source WITHOUT touching its tc_type at
    // all -- firmware must not silently re-type zone 1's channel just
    // because its inheritance link moved. (Re-resolving tc_type on a source
    // change is the UI's job, per the plan's own "must re-resolve and
    // re-save... on save in renderZones()'s submit path" note -- a firmware
    // guard that re-derived it here on a bare settings_source write would
    // be the identical bug in a different spot.)
    TEST_CHECK(zones_config_set_settings_source(1, SRC_GROUP_LIMITS, 2), "zone 1's link is repointed to zone 2");
    TEST_CHECK(zones_config_get_tc_type(1, &t1) && t1 == 2,
              "zone 1's tc_type is STILL 2 (its own last-written value) after its settings_source moved -- "
              "not zone 2's type (6), proving no implicit re-typing on a bare source change");

    nvs_test_enable(false);
    nvs_test_clear();
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
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    (void)err; // the fake always succeeds once hal_kv_init_partition() has been called (nvs_test_enable(true))
    hal_kv_set_blob(&h, NVS_KEY_RELAY_NAMES, data, len);
    hal_kv_commit(&h);
    hal_kv_close(&h);
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
                 "(same NEWER-refuses-to-load discipline as zones_config_json_decode_blob())");
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
    TEST_CHECK(zone_sweep_check_refusal(false, true, true, 1, false, false, true, false, false, false) == ZONE_SWEEP_REFUSE_OK,
              "all-clear inputs refuse nothing");

    TEST_CHECK(zone_sweep_check_refusal(true, true, true, 1, false, false, true, false, false, false) ==
                  ZONE_SWEEP_REFUSE_ALREADY_RUNNING,
              "already_running is refused");
    TEST_CHECK(zone_sweep_check_refusal(false, false, true, 1, false, false, true, false, false, false) ==
                  ZONE_SWEEP_REFUSE_NO_HW,
              "no hardware is refused");
    TEST_CHECK(zone_sweep_check_refusal(false, true, false, 1, false, false, true, false, false, false) ==
                  ZONE_SWEEP_REFUSE_CONFIG_INVALID,
              "invalid zones config is refused");
    TEST_CHECK(zone_sweep_check_refusal(false, true, true, 0, false, false, true, false, false, false) ==
                  ZONE_SWEEP_REFUSE_NO_ZONES,
              "thermo_count == 0 is refused");
    TEST_CHECK(zone_sweep_check_refusal(false, true, true, 1, true, false, true, false, false, false) ==
                  ZONE_SWEEP_REFUSE_PROFILE_RUNNING,
              "a running/paused profile is refused");
    TEST_CHECK(zone_sweep_check_refusal(false, true, true, 1, false, true, true, false, false, false) ==
                  ZONE_SWEEP_REFUSE_AUTOTUNE_RUNNING,
              "an active autotune is refused");
    TEST_CHECK(zone_sweep_check_refusal(false, true, true, 1, false, false, false, false, false, false) ==
                  ZONE_SWEEP_REFUSE_LINK_DOWN,
              "a down safety link is refused");
    TEST_CHECK(zone_sweep_check_refusal(false, true, true, 1, false, false, true, true, false, false) ==
                  ZONE_SWEEP_REFUSE_TRIP_LATCHED,
              "a latched trip is refused");
    // N9 (opus review, 2026-08-28): a relay already on (dashboard, or a
    // profile that just ended) must refuse the start outright, not silently
    // measure a foreign load.
    TEST_CHECK(zone_sweep_check_refusal(false, true, true, 1, false, false, true, false, true, false) ==
                  ZONE_SWEEP_REFUSE_RELAYS_ON,
              "N9: any relay already on is refused");
    // opus review finding (MEDIUM): an unfetched safety param cache
    // (safety_cfg_store_fetched_ms_ago() == UINT32_MAX) must refuse rather
    // than silently be read as "per_zone" by zone_cfg_committed_ct_topology().
    TEST_CHECK(zone_sweep_check_refusal(false, true, true, 1, false, false, true, false, false, true) ==
                  ZONE_SWEEP_REFUSE_CT_TOPOLOGY_UNKNOWN,
              "unfetched CT topology cache is refused");
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
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, /* relay_mask */ 0, &deps, &avg, NULL, NULL);

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
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, NULL, NULL);

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
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, NULL, NULL);

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
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, NULL, NULL);

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
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, NULL, NULL);

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
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, NULL, NULL);

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
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, NULL, &refused_sources);

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
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, NULL, NULL);

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
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, NULL, NULL);

    TEST_CHECK(outcome == ZONE_SWEEP_ZONE_OK,
              "N1: a single invalid poll surrounded by valid ones must not itself trip TEMP_LOST -- the "
              "streak resets on the very next valid read, and the zone completes normally, proving one "
              "poll of noise tolerance is real, not accidental");
}

// N10 (opus review, 2026-08-28): a safety trip latching mid-zone must end
// the sweep promptly instead of grinding on for the rest of the dwell.
// M12: zone_sweep_derive_ct_channel() -- the unambiguity condition
// COMMISSIONING_UX.md sec 1.2 requires before ct_channel_map may be derived
// at all. Each negative case below is a mapping this MUST refuse to produce:
// a check that cannot fail would let the sweep write a guessed value into a
// field S3/S4/S14 then trust.
static void test_zone_sweep_derive_ct_channel_picks_the_dominant_channel(void)
{
    float per_ch[ZONE_CT_CHANNEL_COUNT] = { 0.1f, 14.0f, 0.2f };
    uint8_t ch = 0xFF;
    TEST_CHECK(zone_sweep_derive_ct_channel(per_ch, &ch), "one channel far above the rest resolves");
    TEST_CHECK(ch == 1, "the resolved channel is the one that carried the current");
}

static void test_zone_sweep_derive_ct_channel_refuses_below_the_load_threshold(void)
{
    // Nothing conducted -- a CT not fitted, or a zone whose relay closed onto
    // a dead element. Noise must never be read as "this channel is zone 0's".
    float per_ch[ZONE_CT_CHANNEL_COUNT] = { 0.3f, 0.05f, 0.0f };
    uint8_t ch = 0xFF;
    TEST_CHECK(!zone_sweep_derive_ct_channel(per_ch, &ch), "sub-threshold current resolves nothing");
}

static void test_zone_sweep_derive_ct_channel_refuses_a_shared_ct(void)
{
    // Two channels within the dominance factor: ct_mask explicitly permits
    // one CT feeding more than one zone, and that case is not invertible.
    float per_ch[ZONE_CT_CHANNEL_COUNT] = { 10.0f, 8.0f, 0.0f };
    uint8_t ch = 0xFF;
    TEST_CHECK(!zone_sweep_derive_ct_channel(per_ch, &ch), "two comparable channels resolve nothing");
}

static void test_zone_sweep_derive_ct_channel_refuses_nan(void)
{
    // No per-channel sampler, or a link read that failed mid-window: NaN
    // must read as "cannot tell", never as 0 A on that channel.
    float per_ch[ZONE_CT_CHANNEL_COUNT] = { 12.0f, NAN, 0.0f };
    uint8_t ch = 0xFF;
    TEST_CHECK(!zone_sweep_derive_ct_channel(per_ch, &ch), "a NaN channel resolves nothing");
}

// M12 / opus review 2026-08-28: zone_sweep_derive_ct_channel() above is only
// HALF the derivation. zone_sweep_task_record_ct_channels() is the other
// half, and it owns the two refusals nothing above can express -- the
// zone-id/relay-id identity precondition, and the two-zones-one-channel
// conflict. Both were untested; both write (or decline to write) the field
// S14 aims the over-current guard with. These two exercise them directly.
static void test_zone_sweep_record_ct_refuses_a_zone_whose_relay_is_not_its_own_bit(void)
{
    // ct_channel_map[] is indexed straight into the Pico's relay_now_mask, so
    // the derived value is only meaningful while zone id == relay id. Zone 0
    // wired to relay bit 1 (or to two relays at once) breaks that: the sweep
    // measured real, unambiguous current, and it STILL must not map it --
    // writing the zone index would point S14 at somebody else's relay.
    memset(&s_ct_derive, 0, sizeof(s_ct_derive));
    float per_ch[ZONE_CT_CHANNEL_COUNT] = { 0.1f, 14.0f, 0.2f };

    zone_sweep_task_record_ct_channels(NULL, 0, 0x02, per_ch);       /* zone 0, relay bit 1 */
    zone_sweep_task_record_ct_channels(NULL, 1, 0x03, per_ch);       /* zone 1, TWO relays */

    TEST_CHECK(s_ct_derive.derived_mask == 0,
              "a zone whose relay_mask is not exactly its own bit must derive NOTHING -- the same "
              "current that would map cleanly for a zone/relay-identity board must be refused here");
    TEST_CHECK((s_ct_derive.unresolved_zone_mask & 0x03u) == 0x03u,
              "both refused zones must land in unresolved_zone_mask so the page asks for them by "
              "hand, rather than the refusal being silent");

    // Negative half: the SAME current, on a zone that does satisfy the
    // identity, maps -- proving the refusal above is the relay_mask check
    // and not the measurement failing to resolve.
    memset(&s_ct_derive, 0, sizeof(s_ct_derive));
    zone_sweep_task_record_ct_channels(NULL, 0, 0x01, per_ch);
    TEST_CHECK(s_ct_derive.derived_mask == 0x02u && s_ct_derive.zone_for_ch[1] == 0,
              "control: relay_mask == 1<<zone_index maps that same measurement to CT1");
}

static void test_zone_sweep_record_ct_refuses_two_zones_claiming_one_channel(void)
{
    // Each zone looked perfectly unambiguous on its own -- the conflict is
    // only visible ACROSS zones, which is exactly why no per-zone check can
    // catch it. COMMISSIONING_UX.md sec 1.2 requires a one-to-one inversion:
    // neither claimant may win, and a third claimant must not revive it.
    memset(&s_ct_derive, 0, sizeof(s_ct_derive));
    float per_ch[ZONE_CT_CHANNEL_COUNT] = { 0.1f, 14.0f, 0.2f };

    zone_sweep_task_record_ct_channels(NULL, 0, 0x01, per_ch);
    TEST_CHECK(s_ct_derive.derived_mask == 0x02u, "zone 0 alone resolves CT1 (setup)");

    zone_sweep_task_record_ct_channels(NULL, 1, 0x02, per_ch);
    TEST_CHECK((s_ct_derive.derived_mask & 0x02u) == 0,
              "a second zone dominating the SAME CT must drop that channel entirely -- not let the "
              "first claimant stand, and not let the last one win");
    TEST_CHECK((s_ct_derive.conflict_mask & 0x02u) != 0,
              "the dropped channel must be recorded as a conflict, so the operator is told two zones "
              "share one CT rather than just seeing a missing row");

    zone_sweep_task_record_ct_channels(NULL, 2, 0x04, per_ch);
    TEST_CHECK((s_ct_derive.derived_mask & 0x02u) == 0,
              "a third claimant must not resurrect a channel an earlier conflict disqualified");

    // A DIFFERENT channel, claimed by exactly one zone, still maps -- the
    // conflict must poison only the contested channel.
    float per_ch0[ZONE_CT_CHANNEL_COUNT] = { 14.0f, 0.1f, 0.2f };
    memset(&s_ct_derive, 0, sizeof(s_ct_derive));
    zone_sweep_task_record_ct_channels(NULL, 0, 0x01, per_ch);   /* -> CT1 */
    zone_sweep_task_record_ct_channels(NULL, 1, 0x02, per_ch);   /* -> CT1, conflict */
    zone_sweep_task_record_ct_channels(NULL, 2, 0x04, per_ch0);  /* -> CT0, uncontested */
    TEST_CHECK(s_ct_derive.derived_mask == 0x01u && s_ct_derive.zone_for_ch[0] == 2,
              "an uncontested channel in the same run still maps -- the conflict poisons only the "
              "channel two zones actually claimed");
}

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
    zone_sweep_zone_outcome_t outcome = zone_sweep_run_one_zone(0, 0x01, &deps, &avg, NULL, NULL);

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
/* Set by test_zone_sweep_run_all_zones_energize_refused_reason_decodes_fault_words()
 * below to make the energize call refuse with a known safety-source mask,
 * driving zone_sweep_run_all_zones()'s ZONE_SWEEP_ZONE_ENERGIZE_REFUSED ->
 * out->reason build path (the actual call site being tested). */
static bool s_all_refuse_with_safety = false;
static uint32_t s_all_refuse_sources = 0;

static kiln_io_owner_relay_result_t fake_all_energize(void *ctx, uint8_t relay_mask, uint32_t *out_safety_sources)
{
    (void)ctx;
    (void)relay_mask;
    if (s_all_refuse_with_safety) {
        if (out_safety_sources) *out_safety_sources = s_all_refuse_sources;
        return KILN_IO_OWNER_RELAY_ERR_SAFETY;
    }
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

/* ROADMAP.md M13 ("every fault... what was detected wrong"): the
 * ZONE_SWEEP_ZONE_ENERGIZE_REFUSED case in zone_sweep_run_all_zones() used to
 * render out->reason as "zone %u energize refused (0x%02X)" -- a bare hex
 * mask. This drives that exact call site (not safety_fault_source_words() in
 * isolation, which already works) and asserts the decoded source name is in
 * the operator-facing reason string, not a hex byte. */
static void test_zone_sweep_run_all_zones_energize_refused_reason_decodes_fault_words(void)
{
    TEST_SECTION("zone_sweep_run_all_zones() -- ENERGIZE_REFUSED reason decodes the safety fault-source "
                 "mask instead of showing a bare hex value (ROADMAP.md M13)");
    fake_all_ctx_t actx;
    memset(&actx, 0, sizeof(actx));
    actx.relay_masks[0] = 0x01;

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones.cfg.thermo_count = 1;
    s_zones.cfg.zones[0].max_temp_c = 500.0f;

    zone_sweep_zone_deps_t deps = s_fake_all_zone_deps;
    zone_sweep_all_hooks_t hooks = s_fake_all_hooks;
    hooks.ctx = &actx;

    s_all_any_relay_on = false;
    s_all_overlap_detected = false;
    s_all_refuse_with_safety = true;
    s_all_refuse_sources = 0x04u; /* main-board thermocouple fault, per safety_link.h */

    zone_sweep_all_result_t result;
    zone_sweep_run_all_zones(1, &deps, &hooks, &result);

    s_all_refuse_with_safety = false;
    s_all_refuse_sources = 0;

    TEST_CHECK(result.state == ZONE_SWEEP_FAILED, "an owner safety refusal fails the sweep");
    TEST_CHECK(strstr(result.reason, "0x") == NULL,
              "RED before this fix: the old format string put a bare \"0x04\" hex mask in the reason -- "
              "this must be gone now that the call site decodes the mask instead");
    /* The call site caps the decoded word at 16 bytes (see its comment) so
     * -Werror=format-truncation can prove out->reason never overflows --
     * "main-board thermocouple fault" is cut to "main-board therm" at that
     * width, so assert on the prefix that survives, not the full word. */
    TEST_CHECK(strstr(result.reason, "main-board") != NULL,
              "the decoded fault-source word (\"main-board...\", from safety_fault_source_words()) is in "
              "the reason string, not just the raw mask");

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
// CT_COMMISSIONING_PLAN.md step 3 -- summed-topology normal derivation, and
// step 1's "manual wins over the sweep" gate on zone_sweep_plan_k_ct().
// ---------------------------------------------------------------------------

static void test_zone_sweep_summed_normal_a_basic(void)
{
    TEST_SECTION("zone_sweep_summed_normal_a -- normal_a[zone] = sum(with zone on) - sum(idle), "
                 "CT_COMMISSIONING_PLAN.md step 3's own formula");

    float n = -1.0f;
    TEST_CHECK(zone_sweep_summed_normal_a(5.0f, 0.5f, 1.0f, &n), "converts with a real idle baseline");
    TEST_CHECK(fabsf(n - 4.5f) < 1e-6f, "5.0 - 0.5 = 4.5A");

    TEST_CHECK(zone_sweep_summed_normal_a(3.0f, 0.0f, 1.0f, &n), "converts with a zero idle baseline");
    TEST_CHECK(fabsf(n - 3.0f) < 1e-6f, "no idle draw -- the reading is the normal as-is");

    // opus review finding (MEDIUM): a channel reading LOWER with the zone
    // on than at idle (noise, a settling transient) used to clamp to 0 and
    // return true, which PERSISTS an indistinguishable-from-real-zero
    // normal and silently makes S14/S15 inert for that zone forever. It
    // must now report "not measured" instead.
    n = -1.0f;
    TEST_CHECK(zone_sweep_summed_normal_a(0.4f, 0.5f, 1.0f, &n) == false,
              "on < idle is refused (unmeasured), not clamped to a persisted zero");
    TEST_CHECK(n == -1.0f, "out param is left untouched on refusal");

    // Quantized-counts case: two readings that would floor to the same ADC
    // count as their idle baseline (a real, small negative delta from noise
    // at the CT's actual resolution, not a contrived exact-equal test input
    // -- see project_idealized_test_input_bug_class.md) must also refuse.
    TEST_CHECK(zone_sweep_summed_normal_a(1.996f, 2.001f, 1.0f, &n) == false,
              "a small quantization-scale negative delta is refused, not clamped");

    // Opus review finding 1 (S15 false-WARN): equal on/idle (delta exactly
    // 0) used to be accepted as "a real zero normal". That is exactly the
    // shape of a single ADC count of drift -- indistinguishable from noise
    // -- and this bench's own measured idle wander (channel 2: 60-79 counts
    // around zero_counts=63) means a difference has to clear a real noise
    // floor before it can be trusted as a measurement at all, not merely be
    // non-negative. See ZONE_SWEEP_NORMAL_NOISE_FLOOR_A's own comment for
    // where 45 mA comes from. A delta of exactly 0 is now refused, same as
    // any other below-floor delta -- there is no such thing as a
    // noise-floor-exempt "real zero" for a heater channel.
    n = -1.0f;
    TEST_CHECK(zone_sweep_summed_normal_a(2.0f, 2.0f, 1.0f, &n) == false,
              "on == idle (delta 0) is below the noise floor -- refused, not persisted as zero");
    TEST_CHECK(n == -1.0f, "out param is left untouched on refusal");

    // Below the floor but still positive: noise, not a measurement.
    n = -1.0f;
    TEST_CHECK(zone_sweep_summed_normal_a(2.010f, 2.0f, 1.0f, &n) == false,
              "a 10 mA delta is below the 45 mA noise floor -- refused");
    TEST_CHECK(n == -1.0f, "out param is left untouched on refusal");

    // At/above the floor: a real measurement.
    n = -1.0f;
    TEST_CHECK(zone_sweep_summed_normal_a(2.050f, 2.0f, 1.0f, &n),
              "a 50 mA delta clears the 45 mA noise floor -- accepted");
    TEST_CHECK(fabsf(n - 0.050f) < 1e-6f, "reports the measured delta");

    TEST_CHECK(zone_sweep_summed_normal_a(NAN, 0.5f, 1.0f, &n) == false, "a NaN reading is refused");
    TEST_CHECK(zone_sweep_summed_normal_a(5.0f, NAN, 1.0f, &n) == false, "a NaN idle baseline is refused");
}

static void test_zone_sweep_summed_normal_a_rescales_floor_with_live_k_ct(void)
{
    TEST_SECTION("zone_sweep_summed_normal_a -- the noise floor rescales with the channel's live "
                 "k_ct_v_per_a (2026-09-10 fix, opus review round 2 finding C)");

    // Reference case: at k_ct=1.0 (ZONE_SWEEP_NORMAL_NOISE_FLOOR_REF_K_CT),
    // a 40 mA delta is below the 45 mA reference floor -- refused, matching
    // test_zone_sweep_summed_normal_a_basic's own 10 mA-below-floor case.
    float n = -1.0f;
    TEST_CHECK(zone_sweep_summed_normal_a(2.040f, 2.0f, 1.0f, &n) == false,
              "at the reference k_ct, a 40 mA delta stays below the 45 mA floor");

    // The review's own numeric example: k_ct commissioned DOWN to 0.1 V/A
    // means the SAME raw ADC noise converts to a 10x LARGER amps reading,
    // so the floor must scale up by the same 10x (450 mA) to keep rejecting
    // it -- a 40 mA delta must stay refused, not newly accepted just
    // because the raw amps number looks tiny.
    n = -1.0f;
    TEST_CHECK(zone_sweep_summed_normal_a(2.040f, 2.0f, 0.1f, &n) == false,
              "at k_ct=0.1 (10x more sensitive), the floor must scale to 450 mA -- 40 mA is still noise");

    // And the flip side: at k_ct=0.1, a delta that WOULD have cleared the
    // unscaled 45 mA reference floor (but not the correctly-scaled 450 mA
    // one) must also be refused -- this is exactly the failure mode a
    // fixed-amps floor misses (accepting noise as a measurement).
    n = -1.0f;
    TEST_CHECK(zone_sweep_summed_normal_a(2.100f, 2.0f, 0.1f, &n) == false,
              "100 mA clears the UNSCALED reference floor but not the correctly-scaled 450 mA one "
              "at k_ct=0.1 -- if this test ever regresses to accepted, the floor stopped tracking "
              "live k_ct");

    // A delta that genuinely clears the rescaled floor at k_ct=0.1 is
    // accepted, same as any other above-floor measurement.
    n = -1.0f;
    TEST_CHECK(zone_sweep_summed_normal_a(2.500f, 2.0f, 0.1f, &n),
              "500 mA clears the correctly-scaled 450 mA floor at k_ct=0.1 -- accepted");
    TEST_CHECK(fabsf(n - 0.500f) < 1e-6f, "reports the measured delta");

    // A committed-value of 0.0f/non-finite (zone_cfg_committed_f32's own
    // "never committed" contract) must fall back to the UNSCALED reference
    // floor, not divide by zero or otherwise misbehave.
    n = -1.0f;
    TEST_CHECK(zone_sweep_summed_normal_a(2.040f, 2.0f, 0.0f, &n) == false,
              "an uncommissioned (0.0f) live k_ct falls back to the reference floor, unscaled");
    n = -1.0f;
    TEST_CHECK(zone_sweep_summed_normal_a(2.050f, 2.0f, 0.0f, &n),
              "...and a delta above the unscaled reference floor is still accepted in that fallback");
}

static void test_record_ct_channels_summed_mode_derives_normal_from_channel3(void)
{
    TEST_SECTION("zone_sweep_task_record_ct_channels -- CT_COMMISSIONING_PLAN.md step 3: in "
                 "summed mode, records normal_a[zone] from channel 3 (index 2) minus the sampled "
                 "idle baseline, and skips the one-relay-one-channel derivation entirely");

    nvs_test_enable(true);
    nvs_test_clear();
    memset(&s_ct_derive, 0, sizeof(s_ct_derive));
    memset(&s_zone_normals.cfg, 0, sizeof(s_zone_normals.cfg));

    s_ct_topology_summed = true;
    s_ct_summed_idle_a = 0.3f;

    float per_ch_avg_a[ZONE_CT_CHANNEL_COUNT] = { NAN, NAN, 2.3f }; // channels 0/1 not fitted in this mode
    zone_sweep_task_record_ct_channels(NULL, /*zi=*/1, /*relay_mask=*/0x02u, per_ch_avg_a);

    float amps = -1.0f;
    bool measured = false;
    TEST_CHECK(zones_config_get_normal_current(1, &amps, &measured), "getter answers for zone 1");
    TEST_CHECK(measured, "summed mode still records a normal for the energized zone");
    TEST_CHECK(fabsf(amps - 2.0f) < 1e-6f, "2.3 - 0.3 idle = 2.0A, from channel index 2 alone");

    TEST_CHECK(s_ct_derive.derived_mask == 0,
               "s_ct_derive (the ct_channel_map derivation) is left completely untouched in summed "
               "mode -- the one-relay-one-channel check never even runs, it is not run-and-refused");

    s_ct_topology_summed = false;
    s_ct_summed_idle_a = 0.0f;
    memset(&s_zone_normals.cfg, 0, sizeof(s_zone_normals.cfg));
    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_record_ct_channels_summed_mode_negative_delta_leaves_zone_unmeasured(void)
{
    TEST_SECTION("zone_sweep_task_record_ct_channels -- opus review finding (MEDIUM): a "
                 "with-zone-on reading below the idle baseline must leave the zone UNMEASURED "
                 "(not persist a clamped zero) and record it in s_sweep.summed_unmeasured_mask");

    nvs_test_enable(true);
    nvs_test_clear();
    memset(&s_ct_derive, 0, sizeof(s_ct_derive));
    memset(&s_zone_normals.cfg, 0, sizeof(s_zone_normals.cfg));
    memset((void *)&s_sweep, 0, sizeof(s_sweep));

    s_ct_topology_summed = true;
    s_ct_summed_idle_a = 2.0f;

    float per_ch_avg_a[ZONE_CT_CHANNEL_COUNT] = { NAN, NAN, 1.9f }; // below idle -- noise/settling
    zone_sweep_task_record_ct_channels(NULL, /*zi=*/2, /*relay_mask=*/0x04u, per_ch_avg_a);

    float amps = -1.0f;
    bool measured = true;
    TEST_CHECK(zones_config_get_normal_current(2, &amps, &measured), "getter still answers for zone 2");
    TEST_CHECK(!measured, "nothing was persisted for zone 2 -- it stays at its unmeasured default");
    TEST_CHECK((s_sweep.summed_unmeasured_mask & (1u << 2)) != 0,
              "zone 2's bit is set in summed_unmeasured_mask so the operator can see it was skipped");

    s_ct_topology_summed = false;
    s_ct_summed_idle_a = 0.0f;
    memset(&s_zone_normals.cfg, 0, sizeof(s_zone_normals.cfg));
    memset((void *)&s_sweep, 0, sizeof(s_sweep));
    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_record_ct_channels_summed_mode_nan_sample_leaves_zone_unmeasured(void)
{
    TEST_SECTION("zone_sweep_task_record_ct_channels -- 2026-09-10 fix: a NaN shared-CT sample "
                 "must leave the zone UNMEASURED and set summed_unmeasured_mask, exactly like the "
                 "below-idle case -- the pre-fix code fell straight through the branch and left "
                 "summed_unmeasured_mask clear, letting an incomplete measured_total_a derive an "
                 "arbitrarily wrong k_ct with no refusal");

    nvs_test_enable(true);
    nvs_test_clear();
    memset(&s_ct_derive, 0, sizeof(s_ct_derive));
    memset(&s_zone_normals.cfg, 0, sizeof(s_zone_normals.cfg));
    memset((void *)&s_sweep, 0, sizeof(s_sweep));

    s_ct_topology_summed = true;
    s_ct_summed_idle_a = 0.3f;

    float per_ch_avg_a[ZONE_CT_CHANNEL_COUNT] = { NAN, NAN, NAN }; // shared channel sample itself is NaN
    zone_sweep_task_record_ct_channels(NULL, /*zi=*/1, /*relay_mask=*/0x02u, per_ch_avg_a);

    float amps = -1.0f;
    bool measured = true;
    TEST_CHECK(zones_config_get_normal_current(1, &amps, &measured), "getter still answers for zone 1");
    TEST_CHECK(!measured, "nothing was persisted for zone 1 on a NaN sample");
    TEST_CHECK((s_sweep.summed_unmeasured_mask & (1u << 1)) != 0,
              "zone 1's bit IS set in summed_unmeasured_mask on a NaN sample -- this is the exact "
              "line that was missing before the fix");
    TEST_CHECK(s_ct_derive.measured_total_a == 0.0f,
               "a NaN sample never contributes to measured_total_a, matching the refusal signal above");

    s_ct_topology_summed = false;
    s_ct_summed_idle_a = 0.0f;
    memset(&s_zone_normals.cfg, 0, sizeof(s_zone_normals.cfg));
    memset((void *)&s_sweep, 0, sizeof(s_sweep));
    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_sweep_status_get_handler_reports_summed_unmeasured_mask(void)
{
    TEST_SECTION("sweep_status_get_handler -- opus review finding (MEDIUM): "
                 "summed_unmeasured_mask is set on s_sweep by the summed-CT unmeasured path but "
                 "was never emitted in the /api/zones/current_sweep/status JSON; the operator saw "
                 "a 'done' sweep with no indication a zone still needs a re-sweep");

    memset((void *)&s_sweep, 0, sizeof(s_sweep));
    s_sweep.state = ZONE_SWEEP_DONE;
    s_sweep.summed_unmeasured_mask = (uint8_t)((1u << 0) | (1u << 2));

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = sweep_status_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "sweep_status_get_handler must return ESP_OK");

    TEST_CHECK(strstr(s_last_resp_body, "\"summed_unmeasured_mask\":5") != NULL,
              "the JSON carries the mask (bits 0 and 2 -> 5), after k_ct_reason");

    // A clean run (nothing unmeasured) still reports the field, as 0 -- the
    // page's `typeof st.summed_unmeasured_mask === 'number'` check depends
    // on it always being present, not omitted-when-zero.
    memset((void *)&s_sweep, 0, sizeof(s_sweep));
    s_sweep.state = ZONE_SWEEP_DONE;
    memset(&req, 0, sizeof(req));
    err = sweep_status_get_handler(&req);
    TEST_CHECK(err == ESP_OK, "sweep_status_get_handler must return ESP_OK for a clean run too");
    TEST_CHECK(strstr(s_last_resp_body, "\"summed_unmeasured_mask\":0") != NULL,
              "a clean run reports the field as 0, never omitted");

    memset((void *)&s_sweep, 0, sizeof(s_sweep));
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

// ---------------------------------------------------------------------------
// 2026-09-10 opus review, "the Pico-ceiling invariant is enforced at one
// door only": zones_post_handler's guard_raise()/apply_lower() calls
// (zones_http_post.c) only ever run from that one POST handler, and
// safety_ceiling_sync_guard_raise()/apply_lower() both treat `link == NULL`
// as "nothing to guard" -- so a board that boots (or whose safety link
// drops and reconnects) with a Pico ceiling behind the ESP's already-
// persisted zone config has nothing to close that gap until an operator
// happens to POST the zones page again. safety_ceiling_sync_reconcile_
// on_link_up() (safety_ceiling_sync.c) closes it: called every
// safety_poll_task tick the link is up (safety_link_poll.c), it re-derives
// the ESP's own ceiling target and re-runs the same guard_raise() the POST
// handler uses. These three tests exercise it directly, using the same
// fake safety_cfg_http_set_and_confirm_f32() (now call-counted) every other
// ceiling-adjacent test in this file already relies on.
static void reconcile_test_reset(void)
{
    s_ceiling_writer_calls = 0;
    s_ceiling_writer_last_target_c = 0.0f;
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    s_zones_config_valid = false;
}

static void test_reconcile_on_link_up_null_link_is_a_noop(void)
{
    reconcile_test_reset();
    s_zones_config_valid = true;
    s_zones.cfg.thermo_count = 1;
    s_zones.cfg.zones[0].max_temp_c = 1200.0f;

    safety_ceiling_sync_reconcile_on_link_up(NULL);

    TEST_CHECK(s_ceiling_writer_calls == 0,
              "link == NULL means no safety processor to reconcile against this boot -- must never "
              "attempt a write");
    reconcile_test_reset();
}

static void test_reconcile_on_link_up_invalid_config_is_a_noop(void)
{
    static SafetyLinkClass dummy_safety;
    memset(&dummy_safety, 0, sizeof(dummy_safety));
    reconcile_test_reset();
    s_zones_config_valid = false; // never loaded a real config yet

    safety_ceiling_sync_reconcile_on_link_up(&dummy_safety);

    TEST_CHECK(s_ceiling_writer_calls == 0,
              "an invalid (never-loaded) zones config must never be used to derive a Pico target -- "
              "same gate safety_sync_tc_type() uses");
    reconcile_test_reset();
}

static void test_reconcile_on_link_up_raises_when_pico_ceiling_is_unknown(void)
{
    static SafetyLinkClass dummy_safety;
    memset(&dummy_safety, 0, sizeof(dummy_safety));
    reconcile_test_reset();
    s_zones_config_valid = true;
    s_zones.cfg.thermo_count = 1;
    s_zones.cfg.zones[0].max_temp_c = 1200.0f;

    // safety_ceiling_sync_get_current_pico_ceiling() reads safety_cfg_store's
    // cache, which this executable never populates with a real fetched
    // abs_max_temp_c row -- exactly the "current_known == false" case
    // safety_ceiling_policy_guard_raise() documents as "assume the worst,
    // always raise" (test_safety_ceiling_policy.c covers that decision at
    // the pure-logic layer; this proves the ESP-glue call site actually
    // reaches it on a link-up reconcile, not just from zones_post_handler).
    safety_ceiling_sync_reconcile_on_link_up(&dummy_safety);

    TEST_CHECK(s_ceiling_writer_calls == 1,
              "an unknown Pico ceiling against a real configured max_temp_c must trigger exactly one "
              "raise+confirm write on a link-up reconcile");
    TEST_CHECK_NEAR(s_ceiling_writer_last_target_c, 1200.0, 1e-6,
                   "target must be the configured max (1200) EXACTLY -- 2026-09-10 owner correction removed "
                   "the policy's former +5C headroom so the web page's setting is a hard cutoff");
    reconcile_test_reset();
}

// ---------------------------------------------------------------------------
// M12b: calibrating k_ct_v_per_a from the sweep
// ---------------------------------------------------------------------------
// The pure decision first. Every refusal below is a NEGATIVE test in the
// sense this repo means it: it is exercised by breaking the input the guard
// exists to catch, and each one is paired with the same call made valid, so a
// guard that had been deleted would show up as a passing "refuses" check
// only if the OK case also stopped resolving -- which it does not.

static void test_zone_sweep_derive_k_ct_scales_by_the_measured_over_expected_ratio(void)
{
    TEST_SECTION("zone_sweep_derive_k_ct -- k_new = k_old * (measured / nameplate) amps");

    float k = 0.0f;
    // 7.2 kW at 240 V is 30 A expected; the sweep measured exactly that, so
    // the existing k is already right and must come back unchanged.
    TEST_CHECK(zone_sweep_derive_k_ct(30.0f, 7200.0f, 240.0f, 0.0333f, &k) == ZONE_KCT_DERIVE_OK,
               "a measurement that matches the nameplate derives");
    TEST_CHECK(fabsf(k - 0.0333f) < 1e-6f, "an exact match leaves k_ct_v_per_a unchanged");

    // Reading HALF the nameplate current means the board is under-reporting,
    // which happens when k_ct is set too small -- so k must come down, not up.
    TEST_CHECK(zone_sweep_derive_k_ct(15.0f, 7200.0f, 240.0f, 0.0333f, &k) == ZONE_KCT_DERIVE_OK,
               "a half-scale measurement derives");
    TEST_CHECK(fabsf(k - 0.01665f) < 1e-6f, "reading half the nameplate halves k_ct_v_per_a");

    // And the other direction -- the sign of the correction is the half of
    // this that a transposed formula would get exactly backwards.
    TEST_CHECK(zone_sweep_derive_k_ct(60.0f, 7200.0f, 240.0f, 0.0333f, &k) == ZONE_KCT_DERIVE_OK,
               "a double-scale measurement derives");
    TEST_CHECK(k > 0.0333f, "reading twice the nameplate RAISES k_ct_v_per_a, never lowers it");
}

static void test_zone_sweep_derive_k_ct_refuses_without_the_nameplate_answers(void)
{
    TEST_SECTION("zone_sweep_derive_k_ct -- refuses unless BOTH commissioning answers are real");

    float k = 0.0f;
    TEST_CHECK(zone_sweep_derive_k_ct(30.0f, 0.0f, 240.0f, 0.0333f, &k) == ZONE_KCT_DERIVE_NO_NAMEPLATE,
               "no full-output power answer derives nothing");
    TEST_CHECK(zone_sweep_derive_k_ct(30.0f, 7200.0f, 0.0f, 0.0333f, &k) == ZONE_KCT_DERIVE_NO_NAMEPLATE,
               "no mains voltage answer derives nothing");
    TEST_CHECK(zone_sweep_derive_k_ct(30.0f, NAN, 240.0f, 0.0333f, &k) == ZONE_KCT_DERIVE_NO_NAMEPLATE,
               "a NaN power answer derives nothing");
    TEST_CHECK(zone_sweep_derive_k_ct(30.0f, 7200.0f, -240.0f, 0.0333f, &k) == ZONE_KCT_DERIVE_NO_NAMEPLATE,
               "a negative mains voltage derives nothing");
}

static void test_zone_sweep_derive_k_ct_refuses_without_a_measurement(void)
{
    TEST_SECTION("zone_sweep_derive_k_ct -- refuses a total that is noise, or no total at all");

    float k = 0.0f;
    TEST_CHECK(zone_sweep_derive_k_ct(0.0f, 7200.0f, 240.0f, 0.0333f, &k) == ZONE_KCT_DERIVE_NO_MEASUREMENT,
               "a zero measured total derives nothing");
    TEST_CHECK(zone_sweep_derive_k_ct(1.9f, 7200.0f, 240.0f, 0.0333f, &k) == ZONE_KCT_DERIVE_NO_MEASUREMENT,
               "a total below the load threshold derives nothing");
    TEST_CHECK(zone_sweep_derive_k_ct(NAN, 7200.0f, 240.0f, 0.0333f, &k) == ZONE_KCT_DERIVE_NO_MEASUREMENT,
               "a NaN total derives nothing");
}

static void test_zone_sweep_derive_k_ct_refuses_an_uncommissioned_prior_k(void)
{
    TEST_SECTION("zone_sweep_derive_k_ct -- an uncommissioned k_old carries no scale to correct");

    // The link reports AMPS, not counts, and the Pico computes those amps
    // with k_ct_v_per_a -- so k_old == 0 means every reading was 0.0 A and
    // there is nothing to scale. Inventing a starting value here is exactly
    // what this refusal exists to prevent.
    float k = 0.0f;
    TEST_CHECK(zone_sweep_derive_k_ct(30.0f, 7200.0f, 240.0f, 0.0f, &k) == ZONE_KCT_DERIVE_NO_PRIOR_K,
               "k_ct_v_per_a at its uncommissioned 0.0 derives nothing");
    TEST_CHECK(zone_sweep_derive_k_ct(30.0f, 7200.0f, 240.0f, -0.03f, &k) == ZONE_KCT_DERIVE_NO_PRIOR_K,
               "a negative k_ct_v_per_a derives nothing");
    TEST_CHECK(zone_sweep_derive_k_ct(30.0f, 7200.0f, 240.0f, NAN, &k) == ZONE_KCT_DERIVE_NO_PRIOR_K,
               "a NaN k_ct_v_per_a derives nothing");
}

static void test_zone_sweep_derive_k_ct_refuses_an_implausible_correction(void)
{
    TEST_SECTION("zone_sweep_derive_k_ct -- a disagreement too large to be a scale error is refused");

    float k = 0.0f;
    // 300 A measured against a 30 A nameplate is not a mis-scaled CT, it is a
    // wiring or units error -- and scaling k_ct by 10 would make the amps
    // read right while moving the presence threshold to match the error.
    TEST_CHECK(zone_sweep_derive_k_ct(300.0f, 7200.0f, 240.0f, 0.0333f, &k) == ZONE_KCT_DERIVE_IMPLAUSIBLE,
               "measuring 10x the nameplate derives nothing");
    TEST_CHECK(zone_sweep_derive_k_ct(3.0f, 7200.0f, 240.0f, 0.0333f, &k) == ZONE_KCT_DERIVE_IMPLAUSIBLE,
               "measuring a tenth of the nameplate derives nothing");
    // The absolute band is a separate guard from the ratio band: this ratio
    // (2x) is perfectly acceptable, but the k it produces is not a CT.
    TEST_CHECK(zone_sweep_derive_k_ct(60.0f, 7200.0f, 240.0f, 0.4f, &k) == ZONE_KCT_DERIVE_IMPLAUSIBLE,
               "an in-band ratio that lands k outside the plausible CT band derives nothing");
    // ...and the same in-band ratio on a plausible k_old still derives, so
    // the check above is the absolute band talking, not the ratio band.
    TEST_CHECK(zone_sweep_derive_k_ct(60.0f, 7200.0f, 240.0f, 0.0333f, &k) == ZONE_KCT_DERIVE_OK,
               "the same 2x ratio on a plausible k_old is accepted");
}

// ---------------------------------------------------------------------------
// Owner feature (2026-09-10): per-coil nameplate wattage + expected-current
// advisory. zone_sweep_expected_coil_current_a()/zone_sweep_check_expected_
// current() are the pure decisions; every refusal below is paired with the
// same call made valid, same "prove the guard can actually fail" discipline
// as the k_ct block above.
// ---------------------------------------------------------------------------

static void test_zone_sweep_expected_coil_current_equal_split_default(void)
{
    TEST_SECTION("zone_sweep_expected_coil_current_a -- no override: equal split of the sum nameplate");

    // Owner's own worked example: "account that all coils are the same
    // wattage and the nameplate is for the sum." A 3-zone board with a 9kW
    // sum nameplate and no per-coil override splits to 3kW/coil; at 240V
    // that is 12.5A per coil.
    float a = zone_sweep_expected_coil_current_a(0.0f, 9000.0f, 3, 240.0f);
    TEST_CHECK(fabsf(a - 12.5f) < 1e-4f, "9kW sum / 3 coils / 240V = 12.5A expected per coil");

    // This bench's own real fixture (owner-confirmed, ~4W/120V, roughly
    // 33-90 mA total): the split-and-divide must accept a tiny nameplate as
    // legitimate, not clamp or reject it as implausibly small.
    a = zone_sweep_expected_coil_current_a(0.0f, 4.0f, 3, 120.0f);
    TEST_CHECK(a > 0.0f && fabsf(a - (4.0f / 3.0f / 120.0f)) < 1e-6f,
               "a ~4W/120V bench nameplate splits and derives a plausible tiny expected current, not a rejection");
}

static void test_zone_sweep_expected_coil_current_override_wins(void)
{
    TEST_SECTION("zone_sweep_expected_coil_current_a -- a per-coil override always wins over the equal split");

    // Different-wattage elements: zone 0 is a 1000W coil the operator
    // entered directly, on a board whose SUM nameplate (9000W over 3 coils)
    // would otherwise imply 3000W/coil -- the override must win outright,
    // not blend with or get clamped by the equal share.
    float a_override = zone_sweep_expected_coil_current_a(1000.0f, 9000.0f, 3, 240.0f);
    float a_equal_share = zone_sweep_expected_coil_current_a(0.0f, 9000.0f, 3, 240.0f);
    TEST_CHECK(fabsf(a_override - (1000.0f / 240.0f)) < 1e-4f, "an override zone reports ITS OWN wattage / voltage");
    TEST_CHECK(fabsf(a_override - a_equal_share) > 1e-3f,
               "an overridden coil's expected current differs from the un-overridden equal-share answer");
}

static void test_zone_sweep_expected_coil_current_divides_by_zone_count_not_relay_count(void)
{
    TEST_SECTION("zone_sweep_expected_coil_current_a -- opus review finding 6: divides by ZONE count, "
                 "never relay count, because a zone's measured current is one CT reading for every relay "
                 "in that zone's relay_mask combined");

    // A board where zone 0 alone drives TWO relays (a double-element zone --
    // relay_count over the whole board is 4, but there are still only 3
    // zones/measurements). The correct equal share is the 9kW sum split
    // across the 3 ZONES (3kW/zone), NOT across the 4 physical relays
    // (2.25kW/relay) -- there is no per-relay measurement to compare a
    // per-relay figure against; zone 0's single CT reads both its relays'
    // current summed together.
    float a_by_zone_count = zone_sweep_expected_coil_current_a(0.0f, 9000.0f, /*zone_count=*/3, 240.0f);
    float a_if_relay_count_were_used = zone_sweep_expected_coil_current_a(0.0f, 9000.0f, /*wrong divisor=*/4, 240.0f);
    TEST_CHECK(fabsf(a_by_zone_count - 12.5f) < 1e-4f, "3kW/zone / 240V = 12.5A -- the correct per-zone answer");
    TEST_CHECK(fabsf(a_by_zone_count - a_if_relay_count_were_used) > 1e-3f,
               "the zone-count divisor and a (wrong) relay-count divisor must NOT coincidentally agree here -- "
               "proves this test can tell the two apart, not just that some plausible number came out");
}

static void test_zone_sweep_expected_coil_current_refuses_without_inputs(void)
{
    TEST_SECTION("zone_sweep_expected_coil_current_a -- refuses (negative sentinel) without a usable nameplate");

    TEST_CHECK(zone_sweep_expected_coil_current_a(0.0f, 0.0f, 3, 240.0f) < 0.0f,
               "no sum and no override answers nothing");
    TEST_CHECK(zone_sweep_expected_coil_current_a(0.0f, 9000.0f, 0, 240.0f) < 0.0f,
               "zero zone_count cannot be split across");
    TEST_CHECK(zone_sweep_expected_coil_current_a(0.0f, 9000.0f, 3, 0.0f) < 0.0f,
               "zero mains voltage -- P/V would divide by zero -- answers nothing");
    // A NaN override is treated as "not an override" (isfinite() fails), the
    // same as 0.0f -- it falls through to the equal-share default rather
    // than propagating NaN, so a corrupted override field degrades to the
    // safe default instead of poisoning the whole comparison.
    TEST_CHECK(fabsf(zone_sweep_expected_coil_current_a(NAN, 9000.0f, 3, 240.0f) - 12.5f) < 1e-4f,
               "a NaN override falls through to the equal-share answer, not a fabricated or negative one");
}

static void test_zone_sweep_check_expected_current_ok_and_mismatch(void)
{
    TEST_SECTION("zone_sweep_check_expected_current -- reuses the k_ct ratio band, does not invent a new one");

    // Exact match and the same ZONE_KCT_RATIO_MIN/MAX (0.2x-5.0x) band the
    // k_ct calibration above already exercises -- deliberately the SAME
    // constants, not a re-typed copy, so a change to one changes both.
    TEST_CHECK(zone_sweep_check_expected_current(12.5f, 12.5f) == ZONE_NAMEPLATE_CHECK_OK,
               "an exact match is OK");
    TEST_CHECK(zone_sweep_check_expected_current(3.0f, 12.5f) == ZONE_NAMEPLATE_CHECK_OK,
               "0.24x is inside the 0.2x floor -- OK");
    TEST_CHECK(zone_sweep_check_expected_current(62.0f, 12.5f) == ZONE_NAMEPLATE_CHECK_OK,
               "4.96x is inside the 5.0x ceiling -- OK");
    TEST_CHECK(zone_sweep_check_expected_current(1.0f, 12.5f) == ZONE_NAMEPLATE_CHECK_MISMATCH,
               "0.08x (well under 0.2x) is a MISMATCH -- 'much lower than expected'");
    TEST_CHECK(zone_sweep_check_expected_current(100.0f, 12.5f) == ZONE_NAMEPLATE_CHECK_MISMATCH,
               "8x (well over 5.0x) is a MISMATCH -- 'much higher than expected'");
}

static void test_zone_sweep_check_expected_current_refuses_without_data(void)
{
    TEST_SECTION("zone_sweep_check_expected_current -- NO_NAMEPLATE / NO_MEASUREMENT, never a fabricated verdict");

    TEST_CHECK(zone_sweep_check_expected_current(12.5f, -1.0f) == ZONE_NAMEPLATE_CHECK_NO_NAMEPLATE,
               "the negative sentinel from zone_sweep_expected_coil_current_a() reads as NO_NAMEPLATE");
    TEST_CHECK(zone_sweep_check_expected_current(-1.0f, 12.5f) == ZONE_NAMEPLATE_CHECK_NO_MEASUREMENT,
               "an unmeasured zone (this function's own 'not measured' sentinel) reads as NO_MEASUREMENT");
    TEST_CHECK(zone_sweep_check_expected_current(NAN, 12.5f) == ZONE_NAMEPLATE_CHECK_NO_MEASUREMENT,
               "a NaN measurement reads as NO_MEASUREMENT, not a crash or a false OK");
}

// ---- the plan, which is where the run's own completeness is judged ---------

// Puts s_ct_derive and the fake committed record into the state a clean
// three-zone sweep leaves behind: every channel resolved, both nameplate
// answers present, all three k_ct_v_per_a committed at the CT's datasheet
// figure, and a measured total that exactly matches the nameplate.
static void kct_setup_clean_run(void)
{
    test_link_reset();
    test_cfg_rows_reset();
    memset(&s_ct_derive, 0, sizeof(s_ct_derive));
    zone_k_ct_clear(); // no test may inherit an earlier test's provenance record
    s_ct_derive.derived_mask = 0x07;
    s_ct_derive.zone_for_ch[0] = 0;
    s_ct_derive.zone_for_ch[1] = 1;
    s_ct_derive.zone_for_ch[2] = 2;
    s_ct_derive.measured_total_a = 30.0f;
    test_cfg_set_f32(ZONE_MAINS_VOLTAGE_PARAM_ID, 240.0f, true);
    test_cfg_set_f32(ZONE_MAX_POWER_PARAM_ID, 7200.0f, true);
    for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
        test_cfg_set_f32(ZONE_KCT_PARAM_ID(c), 0.0333f, true);
    }
}

static void test_zone_sweep_plan_k_ct_clean_run_plans_every_derived_channel(void)
{
    TEST_SECTION("zone_sweep_plan_k_ct -- a complete run with both answers plans all three channels");

    kct_setup_clean_run();
    float k[ZONE_CT_CHANNEL_COUNT] = {0};
    char note[96] = "";
    TEST_CHECK(zone_sweep_plan_k_ct(k, note, sizeof(note)) == 0x07,
               "every channel the map resolved is calibrated");
    TEST_CHECK(note[0] == '\0', "a clean plan says nothing");
    TEST_CHECK(fabsf(k[1] - 0.0333f) < 1e-6f, "a matching measurement leaves the scale where it was");
}

static void test_zone_sweep_plan_k_ct_skips_a_manually_calibrated_channel(void)
{
    TEST_SECTION("zone_sweep_plan_k_ct -- CT_COMMISSIONING_PLAN.md step 1: manual wins over the "
                 "sweep, so a channel the operator hand-entered A_fs/zero_mv for is never planned");

    kct_setup_clean_run();
    test_ct_cal_set(0, 1.0f, 0.0f, SAFETY_CT_CAL_SOURCE_MANUAL);

    float k_new[ZONE_CT_CHANNEL_COUNT] = {0};
    char note[96];
    uint8_t plan_mask = zone_sweep_plan_k_ct(k_new, note, sizeof(note));

    TEST_CHECK((plan_mask & 0x01u) == 0, "channel 0 (manually calibrated) is excluded from the plan");
    TEST_CHECK((plan_mask & 0x02u) != 0, "channel 1 (untouched) is still planned normally");
    TEST_CHECK((plan_mask & 0x04u) != 0, "channel 2 (untouched) is still planned normally");

    test_ct_cal_reset();
}

// 2026-09-10 fix (finding A, s14_s15_ct_calibration_sweep audit): before this
// fix, ct_topology=summed left s_ct_derive.derived_mask at 0 for the whole
// run, so zone_sweep_plan_k_ct()'s per-channel loop -- gated on that same
// mask -- could never plan anything on a summed board, regardless of load.
// zone_sweep_plan_k_ct_summed() is the fix: one shared channel, one
// whole-kiln total, no per-relay map needed at all.
static void kct_setup_clean_summed_run(void)
{
    test_link_reset();
    test_cfg_rows_reset();
    memset(&s_ct_derive, 0, sizeof(s_ct_derive));
    zone_k_ct_clear();
    s_sweep.summed_unmeasured_mask = 0;
    // Deliberately NOT touching derived_mask/zone_for_ch -- the summed path
    // must not need them, and a stray non-zero value here would be a bug in
    // the test, not the production path.
    s_ct_derive.measured_total_a = 30.0f; // matches the nameplate exactly
    test_cfg_set_f32(ZONE_MAINS_VOLTAGE_PARAM_ID, 240.0f, true);
    test_cfg_set_f32(ZONE_MAX_POWER_PARAM_ID, 7200.0f, true);
    test_cfg_set_f32(ZONE_KCT_PARAM_ID(ZONE_CT_CHANNEL_COUNT - 1), 0.0333f, true);
}

static void test_zone_sweep_plan_k_ct_summed_derives_from_shared_channel(void)
{
    TEST_SECTION("zone_sweep_plan_k_ct -- summed topology: a complete run derives the ONE shared "
                 "channel from the whole-kiln total, with no per-relay map involved at all");

    s_ct_topology_summed = true;
    kct_setup_clean_summed_run();

    float k[ZONE_CT_CHANNEL_COUNT] = {0};
    char note[96] = "";
    uint8_t mask = zone_sweep_plan_k_ct(k, note, sizeof(note));

    TEST_CHECK(mask == (uint8_t)(1u << (ZONE_CT_CHANNEL_COUNT - 1)),
               "only the shared channel's bit is set");
    TEST_CHECK(note[0] == '\0', "a clean plan says nothing");
    TEST_CHECK(fabsf(k[ZONE_CT_CHANNEL_COUNT - 1] - 0.0333f) < 1e-6f,
               "a matching measurement leaves the scale where it was");
    TEST_CHECK(s_ct_derive.derived_mask == 0,
               "the ct_channel_map derivation state is left untouched -- still 'not applicable'");

    s_ct_topology_summed = false;
}

static void test_zone_sweep_plan_k_ct_summed_refuses_when_any_zone_unmeasured(void)
{
    TEST_SECTION("zone_sweep_plan_k_ct -- summed topology: NEGATIVE TEST, an incomplete pass "
                 "(one zone never cleared the noise floor) refuses rather than scaling k_ct off a "
                 "partial total");

    s_ct_topology_summed = true;
    kct_setup_clean_summed_run();
    s_sweep.summed_unmeasured_mask = (uint8_t)(1u << 1); // zone 1 never measured

    float k[ZONE_CT_CHANNEL_COUNT] = {0};
    char note[96] = "";
    uint8_t mask = zone_sweep_plan_k_ct(k, note, sizeof(note));

    TEST_CHECK(mask == 0, "an incomplete pass derives nothing");
    TEST_CHECK(strstr(note, "incomplete") != NULL, "and says why");

    s_sweep.summed_unmeasured_mask = 0;
    s_ct_topology_summed = false;
}

static void test_zone_sweep_plan_k_ct_summed_skips_a_manually_calibrated_channel(void)
{
    TEST_SECTION("zone_sweep_plan_k_ct -- summed topology: CT_COMMISSIONING_PLAN.md step 1 still "
                 "applies -- a hand-entered A_fs/zero_mv on the shared channel is never overwritten");

    s_ct_topology_summed = true;
    kct_setup_clean_summed_run();
    test_ct_cal_set(ZONE_CT_CHANNEL_COUNT - 1, 1.0f, 0.0f, SAFETY_CT_CAL_SOURCE_MANUAL);

    float k[ZONE_CT_CHANNEL_COUNT] = {0};
    char note[96] = "";
    uint8_t mask = zone_sweep_plan_k_ct(k, note, sizeof(note));

    TEST_CHECK(mask == 0, "the manually-calibrated shared channel is excluded from the plan");
    TEST_CHECK(strstr(note, "manually") != NULL, "and says why");

    test_ct_cal_reset();
    s_ct_topology_summed = false;
}

// PROOF this is really the new summed branch executing, not the pre-existing
// per_zone code silently doing the same thing: with s_ct_topology_summed
// left false (kct_setup_clean_run()'s per_zone fixture, which never touches
// s_sweep.summed_unmeasured_mask), the ORIGINAL derived_mask-gated behavior
// must still run unchanged.
static void test_zone_sweep_plan_k_ct_per_zone_path_unaffected_by_summed_fix(void)
{
    TEST_SECTION("zone_sweep_plan_k_ct -- NEGATIVE TEST: per_zone topology is untouched by the "
                 "summed-mode fix above -- still gated on derived_mask, not summed_unmeasured_mask");

    kct_setup_clean_run(); // s_ct_topology_summed stays false
    s_sweep.summed_unmeasured_mask = 0xFFu; // would refuse every summed plan if the branch leaked

    float k[ZONE_CT_CHANNEL_COUNT] = {0};
    char note[96] = "";
    TEST_CHECK(zone_sweep_plan_k_ct(k, note, sizeof(note)) == 0x07,
               "per_zone path plans all three channels, ignoring summed_unmeasured_mask entirely");

    s_sweep.summed_unmeasured_mask = 0;
}

static void test_zone_sweep_plan_k_ct_plans_every_channel_when_none_are_manual(void)
{
    TEST_SECTION("zone_sweep_plan_k_ct -- NEGATIVE TEST: with no manual channel at all, every "
                 "resolved channel IS planned -- proves the exclusion above is the ct_cal source "
                 "check actually firing, not some channel being unconditionally skipped");

    kct_setup_clean_run();
    // Deliberately leave every channel's ct_cal record unset (test_ct_cal_
    // reset()'s default) -- if zone_sweep_plan_k_ct() unconditionally
    // excluded channel 0 regardless of source, this would catch it.
    float k_new[ZONE_CT_CHANNEL_COUNT] = {0};
    char note[96];
    uint8_t plan_mask = zone_sweep_plan_k_ct(k_new, note, sizeof(note));
    TEST_CHECK(plan_mask == 0x07u, "all three channels are planned when none is manually calibrated");
}

static void test_zone_sweep_plan_k_ct_refuses_an_incomplete_run(void)
{
    TEST_SECTION("zone_sweep_plan_k_ct -- a zone that did not resolve makes the kiln total a lie");

    // The guard being proved: an unresolved zone still drew current, so the
    // total is short by that zone's share and the derived scale would be
    // dragged DOWN by exactly that much -- silently.
    kct_setup_clean_run();
    s_ct_derive.unresolved_zone_mask = 0x04;
    float k[ZONE_CT_CHANNEL_COUNT] = {0};
    char note[96] = "";
    TEST_CHECK(zone_sweep_plan_k_ct(k, note, sizeof(note)) == 0,
               "an unresolved zone calibrates nothing");
    TEST_CHECK(strstr(note, "incomplete") != NULL, "and the operator is told why");

    // The same run with that zone resolved plans normally -- so the refusal
    // above is the completeness check, not some other failure.
    s_ct_derive.unresolved_zone_mask = 0;
    TEST_CHECK(zone_sweep_plan_k_ct(k, note, sizeof(note)) == 0x07,
               "clearing the unresolved zone lets the same run calibrate");

    // A two-zones-one-channel conflict is the same hole seen from the other
    // side, and must refuse identically.
    s_ct_derive.conflict_mask = 0x02;
    TEST_CHECK(zone_sweep_plan_k_ct(k, note, sizeof(note)) == 0,
               "a shared-CT conflict calibrates nothing either");
}

static void test_zone_sweep_plan_k_ct_refuses_after_a_failed_map_push(void)
{
    TEST_SECTION("zone_sweep_plan_k_ct -- never commits on top of a staged buffer the map push abandoned");

    // Both pushes stage into the SAME buffer on the Pico and each ends with
    // its own COMMIT_CONFIG. If the map push failed and could not fully back
    // its staging out, this commit would carry those leftovers into flash --
    // which is precisely the hazard the map push's own H3 repair exists for.
    kct_setup_clean_run();
    s_ct_derive.map_push_failed = true;
    float k[ZONE_CT_CHANNEL_COUNT] = {0};
    char note[96] = "";
    TEST_CHECK(zone_sweep_plan_k_ct(k, note, sizeof(note)) == 0,
               "a failed CT-map push blocks the CT-scale commit outright");
    TEST_CHECK(strstr(note, "CT map write failed") != NULL, "and says so");

    s_ct_derive.map_push_failed = false;
    TEST_CHECK(zone_sweep_plan_k_ct(k, note, sizeof(note)) == 0x07,
               "with the map push clean the same run calibrates");
}

static void test_zone_sweep_plan_k_ct_refuses_an_unanswered_nameplate(void)
{
    TEST_SECTION("zone_sweep_plan_k_ct -- an UNSET commissioning answer is not a zero");

    kct_setup_clean_run();
    // Present in the record but never set -- the case a plain "read the
    // value" would turn into 0.0 W and the derivation would then have to
    // catch as a nameplate refusal one layer later.
    test_cfg_set_f32(ZONE_MAX_POWER_PARAM_ID, 7200.0f, false);
    float k[ZONE_CT_CHANNEL_COUNT] = {0};
    char note[96] = "";
    TEST_CHECK(zone_sweep_plan_k_ct(k, note, sizeof(note)) == 0,
               "an unset full-output power answer calibrates nothing");
    TEST_CHECK(strstr(note, "mains voltage") != NULL, "and asks for the missing answers");
    /* Overflow-fix regression (zone_sweep_plan_k_ct()'s NO_NAMEPLATE arm,
     * %.72s -> %.70s): the operator-visible reason must be the COMPLETE
     * sentence, not silently cut short -- assert the exact byte-for-byte
     * message, not just a substring, so a future re-introduction of a
     * precision wider than the note[] buffer allows would fail this even if
     * the truncated tail still happened to contain "mains voltage". */
    TEST_CHECK(strcmp(note, "CT scale not calibrated: answer the mains voltage and "
                            "full-output power questions first") == 0,
               "the NO_NAMEPLATE reason lands in note[] complete and unmodified");

    test_cfg_set_f32(ZONE_MAX_POWER_PARAM_ID, 7200.0f, true);
    TEST_CHECK(zone_sweep_plan_k_ct(k, note, sizeof(note)) == 0x07,
               "answering it lets the same run calibrate");
}

static void test_zone_sweep_plan_k_ct_implausible_reason_is_not_truncated(void)
{
    TEST_SECTION("zone_sweep_plan_k_ct -- the longest real refusal reason must reach note[] whole");

    /* This drives the LONGEST of the five zone_kct_derive_t strings
     * ("measured and nameplate current disagree too far to be a scale
     * error", 67 bytes) through the exact call site
     * (zone_sweep_plan_k_ct()'s IMPLAUSIBLE arm) that used to read
     * `snprintf(note, note_cap, "CT scale not calibrated: %.72s", ...)` --
     * 25 (prefix) + 72 (precision) + 1 (NUL) = 98 bytes claimed against a
     * 96-byte note[], the exact -Wformat-truncation shape build_kilnfw's
     * -Werror has broken this build on twice before (see this file's H1/H3
     * comments). measured_total_a = 10x the nameplate expectation (300A vs
     * a 30A expected_a from 7200W/240V) lands ratio=10, past
     * ZONE_KCT_RATIO_MAX, which is what zone_sweep_derive_k_ct() reports as
     * ZONE_KCT_DERIVE_IMPLAUSIBLE. */
    kct_setup_clean_run();
    s_ct_derive.measured_total_a = 300.0f;
    float k[ZONE_CT_CHANNEL_COUNT] = {0};
    char note[96] = "";
    TEST_CHECK(zone_sweep_plan_k_ct(k, note, sizeof(note)) == 0,
               "an implausible measured/nameplate ratio calibrates nothing");
    static const char expected[] =
        "CT scale not calibrated: measured and nameplate current disagree too "
        "far to be a scale error";
    TEST_CHECK(strlen(expected) == 92, "the fixture's own expected string is the documented worst case");
    TEST_CHECK(strcmp(note, expected) == 0,
               "the operator sees the WHOLE reason, not a %.70s-truncated fragment of it");
}

// ---- zone_sweep_plan_i_normal() -- nameplate current -> S14/S15 arming ----

static void test_zone_sweep_plan_i_normal_nothing_measured_yet(void)
{
    TEST_SECTION("zone_sweep_plan_i_normal -- an unswept board plans nothing and says why");

    memset(&s_zone_normals.cfg, 0, sizeof(s_zone_normals.cfg));
    float a[MAX31856_CHANNEL_COUNT] = {0};
    char note[96] = "";
    TEST_CHECK(zone_sweep_plan_i_normal(a, note, sizeof(note)) == 0,
               "no zone has ever been measured -- nothing to push");
    TEST_CHECK(note[0] != '\0', "the operator is told S14/S15 stay dormant, not left to guess why");
}

static void test_zone_sweep_plan_i_normal_plans_every_measured_zone(void)
{
    TEST_SECTION("zone_sweep_plan_i_normal -- plans exactly the zones with a measured normal");

    memset(&s_zone_normals.cfg, 0, sizeof(s_zone_normals.cfg));
    nvs_test_enable(true);
    nvs_test_clear();
    TEST_CHECK(zone_normals_set(0, 4.2f), "zone 0 measured");
    TEST_CHECK(zone_normals_set(2, 6.9f), "zone 2 measured");
    // zone 1 deliberately left unmeasured.

    float a[MAX31856_CHANNEL_COUNT] = {0};
    char note[96] = "unset";
    uint8_t mask = zone_sweep_plan_i_normal(a, note, sizeof(note));
    TEST_CHECK(mask == 0x05, "bits 0 and 2 set, bit 1 (unmeasured) clear");
    TEST_CHECK(a[0] == 4.2f, "zone 0's planned value is exactly what was measured");
    TEST_CHECK(a[2] == 6.9f, "zone 2's planned value is exactly what was measured");
    TEST_CHECK(strcmp(note, "unset") == 0, "a non-empty plan leaves note untouched -- only a total refusal explains itself");

    memset(&s_zone_normals.cfg, 0, sizeof(s_zone_normals.cfg));
    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_zone_sweep_plan_i_normal_survives_a_partial_resweep(void)
{
    TEST_SECTION("zone_sweep_plan_i_normal -- a zone this run did not touch still keeps its old measurement armed");

    // This is the property the doc comment calls out explicitly: the plan
    // reads the durable store, not "what this run just measured" -- a sweep
    // that only re-measures zone 0 must not un-arm S14/S15 for zone 1, which
    // still has a perfectly good prior measurement on record.
    memset(&s_zone_normals.cfg, 0, sizeof(s_zone_normals.cfg));
    nvs_test_enable(true);
    nvs_test_clear();
    TEST_CHECK(zone_normals_set(1, 3.0f), "zone 1 measured by an EARLIER sweep");

    float a[MAX31856_CHANNEL_COUNT] = {0};
    char note[96] = "";
    uint8_t mask = zone_sweep_plan_i_normal(a, note, sizeof(note));
    TEST_CHECK(mask == 0x02, "zone 1's old measurement is still planned even though no new sweep touched it");
    TEST_CHECK(a[1] == 3.0f, "the persisted value, not a freshly-measured one, is what gets planned");

    memset(&s_zone_normals.cfg, 0, sizeof(s_zone_normals.cfg));
    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_zone_normals_invalidate_mask_clears_measured_bits_and_amps(void)
{
    TEST_SECTION("zone_normals_invalidate_mask -- 2026-09-10 fix (k_ct rescale vs. stale "
                 "i_normal): a confirmed k_ct change must invalidate every zone's stored normal "
                 "that was measured under the OLD scale, so zone_sweep_plan_i_normal() sees "
                 "'never measured' rather than pushing an S14/S15 threshold on the wrong scale");

    memset(&s_zone_normals.cfg, 0, sizeof(s_zone_normals.cfg));
    nvs_test_enable(true);
    nvs_test_clear();
    TEST_CHECK(zone_normals_set(0, 4.2f), "zone 0 measured under the OLD k_ct");
    TEST_CHECK(zone_normals_set(1, 3.0f), "zone 1 measured under the OLD k_ct");
    TEST_CHECK(zone_normals_set(2, 6.9f), "zone 2 measured under the OLD k_ct");

    TEST_CHECK(zone_normals_invalidate_mask((uint8_t)((1u << 0) | (1u << 2))),
               "invalidating zones 0 and 2 (the ones whose scale just changed) succeeds");

    float amps = -1.0f;
    bool measured = true;
    TEST_CHECK(zones_config_get_normal_current(0, &amps, &measured) && !measured,
               "zone 0's stale measurement is gone");
    TEST_CHECK(zones_config_get_normal_current(2, &amps, &measured) && !measured,
               "zone 2's stale measurement is gone");
    TEST_CHECK(zones_config_get_normal_current(1, &amps, &measured) && measured && amps == 3.0f,
               "zone 1 was NOT in the invalidated mask -- its measurement (a different channel's "
               "scale) is untouched");

    // The downstream effect this whole fix exists for: the guard plan no
    // longer offers the stale zones at all.
    float a[MAX31856_CHANNEL_COUNT] = {0};
    char note[96] = "";
    uint8_t mask = zone_sweep_plan_i_normal(a, note, sizeof(note));
    TEST_CHECK(mask == 0x02, "only zone 1 (never invalidated) is still planned after the rescale");

    memset(&s_zone_normals.cfg, 0, sizeof(s_zone_normals.cfg));
    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_zone_normals_invalidate_mask_noop_on_zero_mask(void)
{
    TEST_SECTION("zone_normals_invalidate_mask -- an empty mask (no channel's k_ct actually "
                 "changed) touches nothing, so an unrelated push cannot un-arm zones it never "
                 "affected");

    memset(&s_zone_normals.cfg, 0, sizeof(s_zone_normals.cfg));
    nvs_test_enable(true);
    nvs_test_clear();
    TEST_CHECK(zone_normals_set(0, 4.2f), "zone 0 measured");

    TEST_CHECK(zone_normals_invalidate_mask(0), "a zero mask is accepted as a trivial success");

    float amps = -1.0f;
    bool measured = false;
    TEST_CHECK(zones_config_get_normal_current(0, &amps, &measured) && measured && amps == 4.2f,
               "zone 0's measurement survives an empty invalidation");

    memset(&s_zone_normals.cfg, 0, sizeof(s_zone_normals.cfg));
    nvs_test_enable(false);
    nvs_test_clear();
}

// ---- the push, and every way it can fail after something has been staged ---

static int kct_setparam_count_for(uint16_t param_id)
{
    int n = 0;
    for (int i = 0; i < s_setparam_count && i < TEST_SETPARAM_LOG_MAX; i++) {
        if (s_setparam_log[i].param_id == param_id) {
            n++;
        }
    }
    return n;
}

static void test_zone_sweep_push_k_ct_happy_path_writes_and_confirms(void)
{
    TEST_SECTION("zone_sweep_push_k_ct_v_per_a -- stages, commits, and confirms by read-back");

    kct_setup_clean_run();
    s_ct_derive.measured_total_a = 60.0f; // 2x the nameplate -> a real correction to write
    s_cfg_refetch_applies_staged = true;  // a Pico that genuinely applied the commit
    s_hw_safety = (SafetyLinkClass *)1;

    zone_sweep_push_k_ct_v_per_a();

    TEST_CHECK(s_sweep.k_ct_derived_mask == 0x07, "all three channels report as derived");
    TEST_CHECK(s_sweep.k_ct_reason[0] == '\0', "a confirmed write says nothing");
    TEST_CHECK(s_setparam_count == 3, "exactly one SET_PARAM per channel, no repair writes");
    TEST_CHECK(s_commit_count == 1, "one COMMIT_CONFIG");
    TEST_CHECK(fabsf(s_setparam_log[0].value.f32_val - 0.0666f) < 1e-5f,
               "the value staged is the corrected scale, not the old one");
    uint8_t dm = 0;
    float persisted[ZONE_CT_CHANNEL_COUNT] = {0};
    zones_ct_k_v_per_a_derived(&dm, persisted);
    TEST_CHECK(dm == 0x07, "the provenance record says all three were derived here");
    TEST_CHECK(fabsf(persisted[2] - 0.0666f) < 1e-5f, "and remembers the value it wrote");
    s_hw_safety = NULL;
}

static void test_zone_sweep_push_k_ct_unconfirmed_readback_derives_nothing_and_backs_out(void)
{
    TEST_SECTION("zone_sweep_push_k_ct_v_per_a -- an ACKed commit that does not read back is NOT written");

    // H1, for this push: SET_PARAM/COMMIT_CONFIG are fire-and-forget, so an
    // un-rejected commit proves nothing. Here the Pico "acks" but the record
    // still reads the OLD value -- the push must report nothing derived AND
    // put the staged buffer back, or the next unrelated commit carries this
    // run's abandoned scale into flash.
    kct_setup_clean_run();
    s_ct_derive.measured_total_a = 60.0f;
    s_cfg_refetch_applies_staged = false; // the record never changes -> read-back disagrees
    s_hw_safety = (SafetyLinkClass *)1;

    zone_sweep_push_k_ct_v_per_a();

    TEST_CHECK(s_sweep.k_ct_derived_mask == 0, "an unconfirmed write derives nothing");
    TEST_CHECK(strstr((const char *)s_sweep.k_ct_reason, "NOT written") != NULL,
               "and says the value was not written");
    TEST_CHECK(s_setparam_count == 6, "every staged channel is re-staged back (3 writes + 3 repairs)");
    for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
        TEST_CHECK(kct_setparam_count_for(ZONE_KCT_PARAM_ID(c)) == 2,
                   "each channel was both staged and backed out");
        TEST_CHECK(fabsf(s_setparam_log[3 + c].value.f32_val - 0.0333f) < 1e-6f,
                   "the backout restores the value the Pico has actually committed");
    }
    uint8_t dm = 0xFF;
    zones_ct_k_v_per_a_derived(&dm, NULL);
    TEST_CHECK(dm == 0, "and nothing is persisted as derived");
    s_hw_safety = NULL;
}

static void test_zone_sweep_push_k_ct_rejected_commit_backs_the_staging_out(void)
{
    TEST_SECTION("zone_sweep_push_k_ct_v_per_a -- a REJECTED commit still leaves staging to back out");

    kct_setup_clean_run();
    s_ct_derive.measured_total_a = 60.0f;
    s_commit_rejected = true; // the Pico refuses config writes while armed
    s_hw_safety = (SafetyLinkClass *)1;

    zone_sweep_push_k_ct_v_per_a();

    TEST_CHECK(s_sweep.k_ct_derived_mask == 0, "a rejected commit derives nothing");
    TEST_CHECK(strstr((const char *)s_sweep.k_ct_reason, "rejected") != NULL,
               "and the rejection is reported rather than inferred from an unchanged value");
    TEST_CHECK(s_setparam_count == 6, "the three staged channels are all backed out");
    s_hw_safety = NULL;
}

static void test_zone_sweep_push_k_ct_partial_staging_failure_backs_out_what_staged(void)
{
    TEST_SECTION("zone_sweep_push_k_ct_v_per_a -- a mid-loop staging failure backs out the earlier writes");

    // H3, for this push: the failure arrives on channel 1, so channel 0 is
    // already sitting in the Pico's staged buffer. It must be overwritten
    // with the committed value, and no COMMIT_CONFIG may be sent at all.
    kct_setup_clean_run();
    s_ct_derive.measured_total_a = 60.0f;
    s_setparam_fail_at = 1;
    s_hw_safety = (SafetyLinkClass *)1;

    zone_sweep_push_k_ct_v_per_a();

    TEST_CHECK(s_sweep.k_ct_derived_mask == 0, "a failed stage derives nothing");
    TEST_CHECK(s_commit_count == 0, "and nothing is committed");
    TEST_CHECK(kct_setparam_count_for(ZONE_KCT_PARAM_ID(0)) == 2,
               "the channel that DID stage is written back");
    TEST_CHECK(kct_setparam_count_for(ZONE_KCT_PARAM_ID(2)) == 0,
               "and the channel the loop never reached is left alone");
    s_hw_safety = NULL;
}

static void test_zone_sweep_push_k_ct_backout_restores_the_uncommissioned_zero(void)
{
    TEST_SECTION("zone_sweep_push_k_ct_v_per_a -- backing out a never-committed channel restores 0.0");

    // A channel with no committed value has nothing to restore, so the
    // repair writes config_store's own uncommissioned 0.0f -- which puts the
    // Pico back on the SAFE side of current_presence_policy's branch rather
    // than leaving this run's abandoned scale staged.
    kct_setup_clean_run();
    s_ct_derive.derived_mask = 0x01; // only channel 0
    s_ct_derive.measured_total_a = 60.0f;
    test_cfg_set_f32(ZONE_KCT_PARAM_ID(0), 0.0333f, true); // usable as k_old...
    s_commit_rejected = true;
    s_hw_safety = (SafetyLinkClass *)1;
    zone_sweep_push_k_ct_v_per_a();
    TEST_CHECK(s_setparam_count == 2 && fabsf(s_setparam_log[1].value.f32_val - 0.0333f) < 1e-6f,
               "a committed channel is restored to its committed value");

    // ...and now the same channel with the row marked never-set.
    kct_setup_clean_run();
    s_ct_derive.derived_mask = 0x01;
    s_ct_derive.measured_total_a = 60.0f;
    s_commit_rejected = true;
    // k_old must still be readable for the DERIVATION, so this test drives
    // the restore path directly rather than through a plan that would refuse.
    uint8_t staged_only_ch0 = 0x01;
    test_cfg_set_f32(ZONE_KCT_PARAM_ID(0), 0.0333f, false); // present but never set
    char note[96] = "";
    float restore_prior[ZONE_CT_CHANNEL_COUNT] = {0};
    // commit_may_have_landed=false: nothing was ever committed on this
    // direct-drive path, so this exercises the restage-only branch, same
    // as the two callers above.
    zone_sweep_unstage_k_ct(staged_only_ch0, restore_prior, false, note, sizeof(note));
    TEST_CHECK(s_setparam_count == 1 && s_setparam_log[0].value.f32_val == 0.0f,
               "a never-committed channel is restored to the uncommissioned 0.0");
    s_hw_safety = NULL;
}

// ---- zone_sweep_push_kct_and_inormal() -- the INTEGRATION the two 2026-09-10
// opus review findings live in: neither zone_sweep_push_k_ct_v_per_a() nor
// zone_sweep_plan_i_normal() alone can see either defect, since both are
// about what happens when a k_ct commit and an i_normal commit meet in the
// same sweep run. ---------------------------------------------------------

static void test_zone_sweep_push_kct_and_inormal_calibrating_sweep_arms_the_guard(void)
{
    TEST_SECTION("zone_sweep_push_kct_and_inormal -- a sweep whose k_ct DOES change still arms "
                 "S14/S15, on the Pico, in the SAME commit -- opus review findings 1+2: the prior "
                 "fix cleared the ESP's own record (finding 1: nothing sent to the Pico, which kept "
                 "its OLD threshold forever) and did so unconditionally on every confirmed k_ct "
                 "write, including one that just measured this very zone (finding 2: a successful "
                 "calibration destroyed the evidence that would have armed it, so the guard could "
                 "only ever arm on a run whose OWN k_ct derivation refused)");

    kct_setup_clean_summed_run();
    s_ct_topology_summed = true;
    // A ratio-2 correction: the nameplate implies 60A, but only 30A measured
    // -- a real, plausible re-calibration (well inside ZONE_KCT_RATIO_MIN/MAX)
    // that actually changes k_ct, unlike kct_setup_clean_summed_run()'s
    // baseline exact-match scenario.
    s_ct_derive.measured_total_a = 15.0f; // half of the 30A nameplate-implied expectation
    s_zones.cfg.thermo_count = 3;
    nvs_test_enable(true);
    nvs_test_clear();
    memset(&s_zone_normals.cfg, 0, sizeof(s_zone_normals.cfg));
    // Zone 1 was measured THIS run (or an earlier one -- the property holds
    // either way) at 3.0A under the OLD k_ct (0.0333).
    TEST_CHECK(zone_normals_set(1, 3.0f), "zone 1 has a measured normal under the OLD scale");
    s_cfg_refetch_applies_staged = true; // a Pico that genuinely applies every commit
    s_hw_safety = (SafetyLinkClass *)1;

    zone_sweep_push_kct_and_inormal();

    // k_new = k_old * ratio = 0.0333 * (15.0/30.0) = 0.01665.
    TEST_CHECK(s_sweep.k_ct_derived_mask != 0, "the k_ct change is confirmed and derived");
    TEST_CHECK(s_sweep.k_ct_reason[0] == '\0', "a confirmed k_ct write says nothing");

    // THE central property: the guard is ARMED, not dormant, after a
    // successful calibration -- this is exactly backwards under the
    // superseded fix (it would read 0 here, plan_mask cleared by the
    // now-empty i_normal plan).
    TEST_CHECK((s_sweep.i_normal_pushed_mask & (1u << 1)) != 0,
               "zone 1's i_normal_a IS pushed -- the guard arms on a calibrating sweep");
    TEST_CHECK(s_sweep.i_normal_reason[0] == '\0', "a confirmed i_normal_a write says nothing");

    // THE exact-rescale property: amps_new = amps_old * k_old/k_new = 3.0 *
    // (0.0333/0.01665) = 6.0 -- not the stale 3.0A, and not a cleared 0.
    float pushed_i_normal = NAN;
    for (int i = 0; i < s_setparam_count && i < TEST_SETPARAM_LOG_MAX; i++) {
        if (s_setparam_log[i].param_id == (uint16_t)(0x031Au + 1)) {
            pushed_i_normal = s_setparam_log[i].value.f32_val;
        }
    }
    TEST_CHECK(!isnan(pushed_i_normal), "an i_normal_a SET_PARAM for zone 1 was actually sent");
    TEST_CHECK(fabsf(pushed_i_normal - 6.0f) < 1e-3f,
               "the value sent to the Pico is rescaled to the NEW k_ct, not the stale old-scale 3.0A");

    // Both param groups landed in ONE shared commit -- the atomicity fix:
    // the Pico is never observed holding the new k_ct with the old
    // i_normal_a, not even for one link round trip.
    TEST_CHECK(s_commit_count == 1,
               "k_ct and i_normal_a are staged together and committed with ONE COMMIT_CONFIG, "
               "never two separate transactions");

    // The ESP's own record agrees with what the Pico now holds.
    float amps = -1.0f;
    bool measured = false;
    TEST_CHECK(zones_config_get_normal_current(1, &amps, &measured) && measured,
               "zone 1's ESP-side record is still measured (not cleared)");
    TEST_CHECK(fabsf(amps - 6.0f) < 1e-3f,
               "and rescaled to the new scale, matching exactly what was just confirmed on the wire");

    s_hw_safety = NULL;
    s_ct_topology_summed = false;
    memset(&s_zone_normals.cfg, 0, sizeof(s_zone_normals.cfg));
    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_zone_sweep_push_kct_and_inormal_no_kct_change_falls_back_unchanged(void)
{
    TEST_SECTION("zone_sweep_push_kct_and_inormal -- no k_ct change this run (channel manually "
                 "calibrated, so the sweep is refused outright) -> falls back to the original two "
                 "independent pushes, unchanged (an already-armed zone keeps its own threshold, on "
                 "the SAME scale, with no rescale needed)");

    kct_setup_clean_summed_run();
    s_ct_topology_summed = true;
    // Manual calibration wins over the sweep (CT_COMMISSIONING_PLAN.md step
    // 1) -- zone_sweep_plan_k_ct_summed() refuses outright, so kct_plan_mask
    // comes back 0 regardless of measured_total_a.
    test_ct_cal_set(ZONE_CT_CHANNEL_COUNT - 1, 1.0f, 0.0f, SAFETY_CT_CAL_SOURCE_MANUAL);
    s_zones.cfg.thermo_count = 3;
    nvs_test_enable(true);
    nvs_test_clear();
    memset(&s_zone_normals.cfg, 0, sizeof(s_zone_normals.cfg));
    TEST_CHECK(zone_normals_set(0, 4.2f), "zone 0 already armed from an earlier sweep");
    s_cfg_refetch_applies_staged = true;
    s_hw_safety = (SafetyLinkClass *)1;

    zone_sweep_push_kct_and_inormal();

    TEST_CHECK(s_sweep.k_ct_derived_mask == 0, "the manually-calibrated channel is not touched");

    TEST_CHECK((s_sweep.i_normal_pushed_mask & (1u << 0)) != 0, "zone 0 is still pushed/armed");
    float amps = -1.0f;
    bool measured = false;
    TEST_CHECK(zones_config_get_normal_current(0, &amps, &measured) && measured && amps == 4.2f,
               "unchanged -- no rescale applied, since k_ct did not change");
    // The refused k_ct plan stages/commits nothing at all (plan_mask == 0
    // inside zone_sweep_push_k_ct_v_per_a() too) -- only i_normal_a's own
    // independent push commits, so this is ONE commit here, not the
    // combined transaction's shape. Falling back to two independently
    // callable functions rather than one merged path is exactly the point:
    // there is nothing here to keep in lockstep.
    TEST_CHECK(s_commit_count == 1, "only i_normal_a's own independent commit runs -- the refused "
                                     "k_ct plan sends nothing");

    s_hw_safety = NULL;
    s_ct_topology_summed = false;
    test_ct_cal_reset();
    memset(&s_zone_normals.cfg, 0, sizeof(s_zone_normals.cfg));
    nvs_test_enable(false);
    nvs_test_clear();
}

// ---------------------------------------------------------------------------
// heater_min_on_ms and HEATER_MIN_ON_MS_FLOOR (10 s, set by the owner
// 2026-08-28).
//
// The floor is enforced in four places; this block covers three of them (the
// fourth, heater_output_duty()'s own quantization + running hold, is
// test_heater_output.c's and stays there as defense in depth):
//   1. zones_http_parse_zone_fields()  -- the POST /api/zones door an operator types at;
//   2. zones_config_json_validate() -- every stored or imported blob;
//   3. raise_heater_timing_to_floors(), via zones_config_json_decode_blob() -- a pre-floor stored
//      value is raised on load, so a GET can never report a number the very
//      next POST would bounce.
//
// The HTTP layer REFUSES rather than clamps. A clamped-and-stored value would
// leave /api/zones reporting a number the kiln is not actually using, which
// is the dishonesty this page's other read-only/derived labelling exists to
// avoid. See ZONE_HEATER_MIN_ON_MS_FLOOR's comment in zones_http.h.
// ---------------------------------------------------------------------------

/* Set by post_body_with_minon() on the accept path so a caller can assert on
 * the value that actually LANDED, not merely on the accept/refuse verdict --
 * "accepted" alone would not catch a clamp. */
static float s_last_parsed_minon = -1.0f;

/* One clean body with only z0_minon varied, so any difference in the verdict
 * can only be that one field. */
static bool post_body_with_minon(const char *minon_literal, const char **err_reason_out)
{
    char body[512];
    snprintf(body, sizeof(body),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=0&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=60000&z0_minon=%s&z0_minoff=2000&"
             "z0_timingprofile=0",
             minon_literal);

    zone_cfg_t current = make_stored_zone();
    current.heater_min_on_ms = (float)HEATER_MIN_ON_MS_FLOOR; /* a legal stored value */
    zone_cfg_t out;
    memset(&out, 0, sizeof(out));
    const char *err_reason = "unset";
    bool ok = zones_http_parse_zone_fields(body, /*i=*/0, /*thermo_count=*/1, /*relay_count=*/4,
                                /*timing_profile_count=*/1, &current, &out, &err_reason);
    if (err_reason_out) {
        *err_reason_out = err_reason;
    }
    s_last_parsed_minon = ok ? out.heater_min_on_ms : -1.0f;
    return ok;
}

static void test_post_minon_below_floor_is_refused(void)
{
    TEST_SECTION("parse_zone_fields -- heater_min_on_ms below the 10 s floor is REFUSED, not clamped");

    const char *reason = "unset";
    TEST_CHECK(!post_body_with_minon("3000", &reason),
               "z0_minon=3000 must be refused outright, never stored and silently run at 10000");
    TEST_CHECK(reason && strstr(reason, "heater_min_on_ms") != NULL,
               "the refusal names the field, so the page can tell the operator which one");
    TEST_CHECK(reason && strstr(reason, "10000") != NULL,
               "the refusal names the floor itself, not just 'out of range'");

    /* One millisecond under. If the comparison were a >= where it should be a
     * <, nothing else in this suite would notice. */
    TEST_CHECK(!post_body_with_minon("9999", &reason),
               "9999 ms -- one under the floor -- must also be refused");
}

static void test_post_minon_zero_and_at_or_above_floor_are_accepted(void)
{
    TEST_SECTION("parse_zone_fields -- 0 and >= floor accepted, and a value above the floor is "
                 "honored EXACTLY");

    const char *reason = "unset";
    TEST_CHECK(post_body_with_minon("0", &reason),
               "0 means 'not configured' (the default it selects IS the floor) and stays accepted");
    TEST_CHECK_NEAR(s_last_parsed_minon, 0.0f, 1e-6,
                    "0 is stored as 0, not rewritten to 10000 -- 'use the default' must stay "
                    "distinguishable from 'the operator chose 10000'");

    TEST_CHECK(post_body_with_minon("10000", &reason),
               "exactly the floor is accepted -- the boundary is inclusive");
    TEST_CHECK_NEAR(s_last_parsed_minon, 10000.0f, 1e-6, "the floor value round-trips exactly");

    /* The failure mode of a clamp written as an assignment instead of a
     * comparison: it would clobber a legitimately longer configured value. */
    TEST_CHECK(post_body_with_minon("15000", &reason),
               "15000 ms (above the floor) is accepted");
    TEST_CHECK_NEAR(s_last_parsed_minon, 15000.0f, 1e-6,
                    "15000 is honored EXACTLY -- the floor raises, it never lowers");
}

static void test_validate_rejects_sub_floor_min_on(void)
{
    TEST_SECTION("validate_zones_cfg -- a sub-floor heater_min_on_ms is rejected (the import door)");

    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].heater_min_on_ms = 3000.0f;
        const char *reason = NULL;
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason),
                   "a config carrying 3000 ms does not validate, whichever door it came through");
        TEST_CHECK(reason && strstr(reason, "heater_min_on_ms") != NULL, "the reason names the field");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].heater_min_on_ms = 15000.0f;
        const char *reason = NULL;
        TEST_CHECK(zones_config_json_validate(&cfg, &reason),
                   "15000 ms still validates -- the check must not have become 'must equal 10000'");
    }
}

static void test_stored_pre_floor_blob_is_raised_on_load_not_rejected(void)
{
    TEST_SECTION("decode_zones_blob -- a pre-floor stored heater_min_on_ms is RAISED on load, "
                 "not rejected");

    /* A board configured before 2026-08-28 legally carries 2000 ms (the old
     * HEATER_DEFAULT_MIN_ON_MS). Rejecting it at load would wipe that board's
     * config on the next boot; reporting it verbatim would make the very next
     * whole-page save fail -- the page posts back what GET returned -- on a
     * number the operator never typed. Raising on load makes both
     * impossible, and leaves the refusal above firing only on human input. */
    zones_cfg_t stored;
    make_minimal_valid_cfg(&stored);
    stored.zones[0].heater_min_on_ms = 2000.0f;
    stored.zones[1].heater_min_on_ms = 0.0f;
    stored.zones[2].heater_min_on_ms = 15000.0f;
    stored.crc32 = 0;
    stored.crc32 = zones_config_json_compute_crc(&stored);

    zones_cfg_t out;
    const char *reason = "unset";
    zones_decode_result_t r = zones_config_json_decode_blob(&stored, sizeof(stored), &out, &reason);

    TEST_CHECK(r == ZONES_DECODE_OK,
               "a pre-floor blob still decodes -- it must not be called corrupt");
    TEST_CHECK_NEAR(out.zones[0].heater_min_on_ms, (float)HEATER_MIN_ON_MS_FLOOR, 1e-6,
                    "2000 ms is raised to the 10000 ms floor on load");
    TEST_CHECK_NEAR(out.zones[1].heater_min_on_ms, 0.0f, 1e-6,
                    "0 ('not configured') is left alone -- the default it selects IS the floor");
    TEST_CHECK_NEAR(out.zones[2].heater_min_on_ms, 15000.0f, 1e-6,
                    "a longer configured value is untouched by the raise");
}

// ---------------------------------------------------------------------------
// heater_window_ms vs heater_min_on_ms (2026-08-29).
//
// The relationship, not either number. This bench's zone 0 carried
// window=2000 ms with the 10 s min-on floor: both values passed their own
// range checks, and together they made the zone incapable of rendering ANY
// fractional duty -- 0.4*2000 = 800 ms is under the floor, so
// heater_output_duty() quantized every window to OFF. An autotune step at
// duty 0.4 commanded heat for 40 minutes and never closed the relay once.
//
// Enforced the same four places the min-on floor is (refuse at the POST door,
// refuse in the setter backup import uses, refuse in zones_config_json_validate(),
// raise on load), with heater_output_cfg_expressible() as the point-of-use
// check. These tests negative-test the rule at each door -- a check nothing
// can fail is not a check.
// ---------------------------------------------------------------------------

static float s_last_parsed_window = -1.0f;

static bool post_body_with_window(const char *window_literal, const char *minon_literal,
                                  const char **err_reason_out)
{
    char body[512];
    snprintf(body, sizeof(body),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=0&z0_mode=0&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=%s&z0_minon=%s&z0_minoff=2000&"
             "z0_timingprofile=0",
             window_literal, minon_literal);

    zone_cfg_t current = make_stored_zone();
    zone_cfg_t out;
    memset(&out, 0, sizeof(out));
    const char *err_reason = "unset";
    bool ok = zones_http_parse_zone_fields(body, /*i=*/0, /*thermo_count=*/1, /*relay_count=*/4,
                                /*timing_profile_count=*/1, &current, &out, &err_reason);
    if (err_reason_out) {
        *err_reason_out = err_reason;
    }
    s_last_parsed_window = ok ? out.heater_window_ms : -1.0f;
    return ok;
}

static void test_post_window_too_short_for_min_on_is_refused(void)
{
    TEST_SECTION("parse_zone_fields -- heater_window_ms shorter than 3x the min-on is REFUSED");

    const char *reason = "unset";

    /* The exact configuration that was on this bench. */
    TEST_CHECK(!post_body_with_window("2000", "0", &reason),
               "the bench's own window=2000 with the default 10 s min-on must now be refused");
    TEST_CHECK(reason && strstr(reason, "heater_window_ms") != NULL,
               "the refusal names the field, so the page can point at it");
    TEST_CHECK(reason && strstr(reason, "min") != NULL,
               "the refusal explains it is about the minimum on-time, not a plain range");

    /* One millisecond under the bound: catches a >= written as a >. */
    TEST_CHECK(!post_body_with_window("29999", "0", &reason),
               "29999 ms -- one under 3x the 10 s floor -- is refused too");

    /* The bound tracks the ZONE's own min_on, it is not the constant 30000.
     * With min_on=20000 the requirement is 60000, so 45000 must fail even
     * though it comfortably clears the default-case bound. */
    TEST_CHECK(!post_body_with_window("45000", "20000", &reason),
               "the bound is 3x THIS zone's min-on (60000), not a fixed 30000");
}

static void test_post_window_at_or_above_the_bound_is_accepted_exactly(void)
{
    TEST_SECTION("parse_zone_fields -- a window at or above 3x the min-on is accepted, unaltered");

    const char *reason = "unset";

    TEST_CHECK(post_body_with_window("30000", "0", &reason),
               "exactly 3x the 10 s floor is accepted -- the boundary is inclusive");
    TEST_CHECK_NEAR(s_last_parsed_window, 30000.0f, 1e-6, "30000 round-trips exactly, not raised");

    TEST_CHECK(post_body_with_window("60000", "0", &reason),
               "the 60 s firmware default is accepted");
    TEST_CHECK_NEAR(s_last_parsed_window, 60000.0f, 1e-6, "60000 is honored exactly");

    TEST_CHECK(post_body_with_window("0", "0", &reason),
               "0 means 'not configured' and stays accepted -- the default it selects satisfies the rule");
    TEST_CHECK_NEAR(s_last_parsed_window, 0.0f, 1e-6,
                    "0 stays 0 -- 'use the default' must stay distinguishable from 'chose 60000'");

    TEST_CHECK(post_body_with_window("60000", "20000", &reason),
               "a longer min-on with a window that still clears 3x it is accepted");
}

static void test_setter_and_validate_reject_short_window(void)
{
    TEST_SECTION("zones_config_set_heater_cfg / validate_zones_cfg -- the non-HTTP doors refuse too");

    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].heater_window_ms = 2000.0f;
        cfg.zones[0].heater_min_on_ms = 0.0f;
        const char *reason = NULL;
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason),
                   "a stored/imported config with a 2000 ms window does not validate");
        TEST_CHECK(reason && strstr(reason, "heater_window_ms") != NULL, "the reason names the field");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].heater_window_ms = 30000.0f;
        cfg.zones[0].heater_min_on_ms = 0.0f;
        const char *reason = NULL;
        TEST_CHECK(zones_config_json_validate(&cfg, &reason),
                   "30000 still validates -- the check must not reject everything");
    }
    /* The setter backup import comes through. Its refusal is the reason a
     * restore of a pre-2026-08-29 backup cannot re-install the trap. */
    TEST_CHECK(!zones_config_set_heater_cfg(0, 2000.0f, 0.0f, 2000.0f),
               "zones_config_set_heater_cfg refuses a 2000 ms window -- backup import's door");
    TEST_CHECK(!zones_config_set_heater_cfg(0, 45000.0f, 20000.0f, 2000.0f),
               "and it computes the bound from the min_on it was handed, not a constant");
}

static void test_stored_short_window_is_raised_on_load(void)
{
    TEST_SECTION("decode_zones_blob -- a stored too-short heater_window_ms is RAISED on load");

    /* Exactly this bench's flash. Rejecting the blob would wipe the board's
     * whole config on the next boot; reporting 2000 verbatim would make the
     * next whole-page save fail on a number the operator never typed. */
    zones_cfg_t stored;
    make_minimal_valid_cfg(&stored);
    stored.zones[0].heater_window_ms = 2000.0f;
    stored.zones[0].heater_min_on_ms = 0.0f;
    stored.zones[1].heater_window_ms = 0.0f;
    stored.zones[1].heater_min_on_ms = 0.0f;
    /* min_on itself sub-floor: the window bound must be computed from the
     * RAISED min_on (10000 -> needs 30000), not the stored 2000. */
    stored.zones[2].heater_window_ms = 20000.0f;
    stored.zones[2].heater_min_on_ms = 2000.0f;
    stored.crc32 = 0;
    stored.crc32 = zones_config_json_compute_crc(&stored);

    zones_cfg_t out;
    const char *reason = "unset";
    zones_decode_result_t r = zones_config_json_decode_blob(&stored, sizeof(stored), &out, &reason);

    TEST_CHECK(r == ZONES_DECODE_OK, "the blob still decodes -- it must not be called corrupt");
    TEST_CHECK_NEAR(out.zones[0].heater_window_ms, 30000.0f, 1e-6,
                    "2000 ms is raised to 3x the 10 s floor on load");
    TEST_CHECK_NEAR(out.zones[1].heater_window_ms, 0.0f, 1e-6,
                    "0 ('not configured') is left alone");
    TEST_CHECK_NEAR(out.zones[2].heater_min_on_ms, 10000.0f, 1e-6, "min_on raised first");
    TEST_CHECK_NEAR(out.zones[2].heater_window_ms, 30000.0f, 1e-6,
                    "and the window bound is computed from the RAISED min_on, not the stored 2000");

    /* And what came out must now pass the very validation that rejects the
     * input -- the whole point of raising rather than refusing at load. */
    TEST_CHECK(zones_config_json_validate(&out, &reason), "the raised config validates");
}

/* ------------------------------------------------------------------------
 * sanity_rate_c_per_min -- thermal_guard.c guard 1's minimum rise rate, the
 * dead-element check, editable per zone on the Zones page as z%u_sanity.
 * 2026-08-29: this bench's zone 0 carried 5.0 C/min (a real kiln's figure)
 * and every firing died at t=62s on "rose only 0.0C in 1.0min (need >=5.0C)"
 * while the jig was genuinely -- just slowly -- heating. The number is
 * operator config and always was; what was missing was a test that the door
 * it comes through actually enforces its bounds, and that the guard's verdict
 * really follows the configured number rather than any constant.
 * ------------------------------------------------------------------------ */
static float s_last_parsed_sanity = -1.0f;

static bool post_body_with_sanity(const char *sanity_literal, const char **err_reason_out)
{
    char body[512];
    snprintf(body, sizeof(body),
             "z0_name=Top&z0_tctype=2&z0_relay_mask=1&z0_thermo_mask=1&"
             "z0_cal=0&z0_kp=1&z0_ki=0&z0_kd=0&z0_ramp=100&z0_sanity=%s&z0_mode=0&"
             "z0_maxtemp=1300&z0_mintemp=-20&z0_window=60000&z0_minon=10000&z0_minoff=2000&"
             "z0_timingprofile=0",
             sanity_literal);

    zone_cfg_t current = make_stored_zone();
    current.sanity_rate_c_per_min = 5.0f; /* a legal stored value, and the one this bench had */
    zone_cfg_t out;
    memset(&out, 0, sizeof(out));
    const char *err_reason = "unset";
    bool ok = zones_http_parse_zone_fields(body, /*i=*/0, /*thermo_count=*/1, /*relay_count=*/4,
                                /*timing_profile_count=*/1, &current, &out, &err_reason);
    if (err_reason_out) {
        *err_reason_out = err_reason;
    }
    s_last_parsed_sanity = ok ? out.sanity_rate_c_per_min : -1.0f;
    return ok;
}

static void test_post_sanity_rate_bounds(void)
{
    TEST_SECTION("parse_zone_fields -- z0_sanity is bounded 0..ZONE_SANITY_RATE_MAX_C_PER_MIN");

    const char *reason = "unset";

    /* Negative-test the validation itself: each of these MUST be refused, or
     * the bound is decorative. */
    TEST_CHECK(!post_body_with_sanity("-0.2", &reason),
               "a negative minimum rise rate is meaningless and must be refused");
    TEST_CHECK(reason && strstr(reason, "sanity_rate_c_per_min") != NULL,
               "the refusal names the field, so the page can say which one");
    TEST_CHECK(!post_body_with_sanity("20.1", &reason),
               "above the 20 C/min ceiling is refused -- a rate no kiln sustains would trip everything");
    TEST_CHECK(!post_body_with_sanity("0.2C", &reason),
               "a unit suffix is refused, not silently parsed as 0.2");
    TEST_CHECK(!post_body_with_sanity("", &reason),
               "an empty field is refused rather than read as 0");

    /* And the accept path, with the value landing EXACTLY -- "accepted" alone
     * would not catch a clamp or a rounding to the old 5.0. */
    TEST_CHECK(post_body_with_sanity("0.2", &reason), "0.2 C/min -- this bench's jig value -- is accepted");
    TEST_CHECK_NEAR(s_last_parsed_sanity, 0.2f, 1e-6, "0.2 round-trips exactly, not clamped upward");

    TEST_CHECK(post_body_with_sanity("0", &reason),
               "0 stays legal: it means 'use the firmware default', NOT 'disable the guard'");
    TEST_CHECK_NEAR(s_last_parsed_sanity, 0.0f, 1e-6,
                    "0 is stored as 0 -- 'use the default' must stay distinguishable from a chosen 0.5");

    TEST_CHECK(post_body_with_sanity("20", &reason), "exactly the ceiling is accepted -- inclusive bound");
    TEST_CHECK_NEAR(s_last_parsed_sanity, 20.0f, 1e-6, "the ceiling value round-trips exactly");
}

static void test_validate_rejects_out_of_range_sanity_rate(void)
{
    TEST_SECTION("validate_zones_cfg -- an out-of-range sanity_rate_c_per_min is rejected at the "
                 "import door too");

    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].sanity_rate_c_per_min = 500.0f;
        const char *reason = NULL;
        TEST_CHECK(!zones_config_json_validate(&cfg, &reason),
                   "500 C/min does not validate, whichever door it came through");
        TEST_CHECK(reason && strstr(reason, "sanity_rate_c_per_min") != NULL,
                   "the reason names the field");
    }
    {
        zones_cfg_t cfg;
        make_minimal_valid_cfg(&cfg);
        cfg.zones[0].sanity_rate_c_per_min = 0.2f;
        const char *reason = NULL;
        TEST_CHECK(zones_config_json_validate(&cfg, &reason),
                   "0.2 C/min validates -- the slow-jig value must not be collateral damage");
    }
}

void run_test_zones_http(void)
{
    test_out_of_range_zone_preserves_stored_fields();
    test_whole_page_post_invalidates_tuning_quality_only_when_gains_actually_change();
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
    test_post_whole_page_cross_zone_cycle_refused();
    test_post_whole_page_cross_zone_legal_chain_accepted();

    test_zones_pid_post_accepts_while_profile_running();
    test_zones_pid_post_bumps_generation();
    test_zones_pid_post_rejects_kp_over_bound();
    test_zones_pid_post_rejects_negative_ki();
    test_zones_pid_post_rejects_non_numeric_kd();
    test_zones_pid_post_rejects_zone_out_of_range();
    test_zones_pid_post_ignores_relay_mask();
    test_zones_pid_post_ignores_control_mode();
    test_zones_pid_post_ignores_max_temp();
    test_zones_pid_post_ignores_max_ramp();
    test_zones_pid_post_ignores_cal_offset();
    test_zones_pid_post_ignores_guard_thresholds();
    test_zones_pid_post_ignores_coupling_matrix();
    test_zones_pid_post_ignores_model_parameters();
    test_zones_pid_post_ignores_thermo_mask();
    test_zones_pid_post_ignores_settings_source();
    test_zones_pid_post_ignores_safety_tc_type();

    test_nvs_load_from_too_short_is_corrupt_not_refused();
    test_nvs_load_from_defaults_carry_the_model_fit_unknown_sentinel();
    test_nvs_load_from_wrong_size_current_version_is_corrupt_not_refused();
    test_nvs_load_from_current_version_happy_path();
    test_nvs_load_from_pre_existing_cycle_normalizes_not_wipes();
    test_nvs_load_from_cycle_normalization_does_not_touch_lead_in_zone();
    test_import_blob_pass1_rejects_cyclic_settings_source();
    test_import_blob_pass1_accepts_acyclic_settings_source();
    test_nvs_load_from_newer_than_firmware_is_found_but_not_valid();
    test_zones_http_start_refused_newer_blob_not_overwritten();

    test_nvs_load_from_old_version_wrong_length_is_rejected();
    test_nvs_load_from_bad_crc_is_rejected();
    test_nvs_load_from_failed_validation_is_rejected();
    test_nvs_save_load_round_trip_current_version();
    test_nvs_load_from_v1_blob_upconverts_fields_correctly();
    test_nvs_load_from_v2_blob_upconverts_fields_correctly();
    test_nvs_load_from_v3_blob_upconverts_guard_thresholds_correctly();
    test_nvs_load_from_v4_blob_upconverts_thermo_mask_correctly();
    test_nvs_load_from_v5_blob_upconverts_zones_1_and_2_correctly();
    test_nvs_load_from_v6_blob_upconverts_fields_correctly();
    test_nvs_load_from_v1_blob_chains_end_to_end_to_v16_preserving_real_values();
    test_nvs_load_from_v7_blob_upconverts_and_defaults_new_fields();
    test_nvs_load_from_v8_blob_with_distinct_zone_values_migrates_losslessly();
    test_nvs_load_from_v8_blob_upconverts_to_shared_default_profile();
    test_validate_rejects_out_of_range_v8_fields();

    test_nvs_load_from_v9_blob_upconverts_new_fields_default_and_settings_source_is_custom();
    test_nvs_load_from_v10_blob_folds_single_pair_into_row_cell();
    test_nvs_load_from_v11_blob_defaults_coupling_tau_dead_time_to_zero();
    test_nvs_load_from_v10_blob_self_referencing_pair_is_dropped();
    test_nvs_load_from_v9_blob_chains_through_v10_to_v11_with_zero_coupling();
    test_validate_accepts_control_mode_pid_fuzzy_rejects_past_it();
    test_validate_accepts_relay_type_mercury_rejects_past_it();
    test_push_all_relay_types_clears_orphaned_relay();
    test_post_mode_pid_fuzzy_accepted_by_parser();
    test_post_relay_type_optional_range_and_preserve();
    test_post_on_off_fields_optional_range_and_preserve();
    test_post_fuzzy_strength_out_of_range_refused_not_clamped();
    test_post_omitting_new_fields_preserves_stored_values();
    test_post_omitting_model_fields_preserves_them();
    test_post_new_fields_present_but_unparseable_are_refused();
    test_post_coupling_diagonal_must_be_zero();
    test_post_settings_source_self_reference_refused();
    test_post_settings_source_group_keys_only();
    test_post_settings_source_group_key_wins_over_legacy_scalar();
    test_post_settings_source_all_keys_omitted_preserves_every_group();
    test_post_then_get_round_trips_new_fields();
    test_post_then_get_round_trips_model_across_an_omitting_save();
    test_zones_diag_get_handler_round_trips_moved_fields();
    test_get_emits_autotune_baseline_k_dc();
    test_get_emits_fuzzy_model_valid();
    test_get_fuzzy_model_valid_single_sourced_past_thermo_count();
    test_get_and_diag_emit_matching_generation();
    test_zones_get_handler_max_width_response_fits_json_cap();
    test_zones_diag_get_handler_max_width_response_fits_json_cap();
    test_tuning_rec_body_len_strips_the_idf_appended_nul();
    test_zones_get_handler_malloc_failure_returns_clean_500();
    test_zones_get_handler_succeeds_when_malloc_does_not_fail();
    test_fuzzy_strength_pct_setter_round_trip_and_bounds();
    test_coupling_row_whole_setter_round_trip_and_bounds();
    test_coupling_zeroed_for_on_off_zone_row_and_column();
    test_coupling_single_cell_setter_preserves_other_cells();
    test_coupling_matrix_2026_09_02_adopted_orientation_not_transposed();
    test_settings_source_setter_round_trip_and_bounds();
    test_nvs_load_from_v20_blob_fans_out_settings_source_to_every_group();
    test_settings_source_per_group_resolution_is_independent();
    test_tuning_quality_round_trip_asymmetric_per_zone();
    test_zones_config_set_pid_invalidates_tuning_quality();
    test_nvs_load_from_v12_blob_defaults_tuning_quality_to_unknown();
    test_nvs_load_from_v13_blob_defaults_adaptive_tune_enabled_to_zero();
    test_nvs_load_from_v14_blob_defaults_coupling_diag_k_dc_to_zero();
    test_nvs_load_from_v15_blob_defaults_ease_off_window_mult_to_default();
    test_nvs_load_from_v16_blob_carries_global_ease_off_mult_to_every_zone();
    test_ease_off_window_mult_accessor_get_set_and_range();
    test_nvs_load_from_v17_blob_defaults_approach_rate_cap_to_uncapped();
    test_approach_rate_cap_accessor_get_set_and_range();
    test_nvs_load_from_v18_blob_defaults_fuzzy_bands_to_firmware_default();
    test_nvs_load_from_v19_blob_defaults_relay_type_to_ssr();
    test_nvs_load_from_v18_blob_real_board_values_migration();
    test_fuzzy_bands_accessor_get_set_and_range();
    test_progress_band_c_accessor_get_set_and_range();
    test_NEGATIVE_wrong_zone_progress_band_read_is_caught();
    test_nvs_load_from_v21_blob_defaults_progress_band_c_to_default();
    test_zone_type_accessor_get_set_and_range();
    test_zero_initialized_zone_cfg_is_heater_and_failsafe_off();
    test_nvs_load_from_v22_blob_defaults_zone_type_and_failsafe_to_zero();
    test_nvs_load_from_v23_blob_defaults_model_fit_context_to_unknown();
    test_decode_zones_blob_accepts_a_v25_blob_with_a_correct_crc();
    test_decode_zones_blob_refuses_a_v25_blob_with_a_corrupted_crc();
    test_zone_is_on_off_and_zone_needs_ceiling();
    test_NEGATIVE_wrong_zone_band_read_is_caught();
    test_NEGATIVE_migration_default_of_zero_instead_of_20_is_caught();
    test_settings_source_save_reload_inheritance_round_trip();
    test_settings_source_two_and_three_zone_cycles_are_refused();
    test_tc_type_write_is_identity_independent_of_settings_source();

    test_post_sanity_rate_bounds();
    test_validate_rejects_out_of_range_sanity_rate();

    test_post_minon_below_floor_is_refused();
    test_post_minon_zero_and_at_or_above_floor_are_accepted();
    test_validate_rejects_sub_floor_min_on();
    test_post_window_too_short_for_min_on_is_refused();
    test_post_window_at_or_above_the_bound_is_accepted_exactly();
    test_setter_and_validate_reject_short_window();
    test_stored_short_window_is_raised_on_load();
    test_stored_pre_floor_blob_is_raised_on_load_not_rejected();

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
    test_zone_sweep_derive_ct_channel_picks_the_dominant_channel();
    test_zone_sweep_derive_ct_channel_refuses_below_the_load_threshold();
    test_zone_sweep_derive_ct_channel_refuses_a_shared_ct();
    test_zone_sweep_derive_ct_channel_refuses_nan();
    test_zone_sweep_record_ct_refuses_a_zone_whose_relay_is_not_its_own_bit();
    test_zone_sweep_record_ct_refuses_two_zones_claiming_one_channel();
    test_zone_sweep_run_all_zones_never_energizes_two_zones_at_once();
    test_zone_sweep_run_all_zones_skipped_zone_records_nothing();
    test_zone_sweep_run_all_zones_energize_refused_reason_decodes_fault_words();
    test_zone_normals_get_set_round_trip();

    test_zone_sweep_summed_normal_a_basic();
    test_zone_sweep_summed_normal_a_rescales_floor_with_live_k_ct();
    test_record_ct_channels_summed_mode_derives_normal_from_channel3();
    test_record_ct_channels_summed_mode_negative_delta_leaves_zone_unmeasured();
    test_record_ct_channels_summed_mode_nan_sample_leaves_zone_unmeasured();
    test_sweep_status_get_handler_reports_summed_unmeasured_mask();

    test_zone_sweep_derive_k_ct_scales_by_the_measured_over_expected_ratio();
    test_zone_sweep_derive_k_ct_refuses_without_the_nameplate_answers();
    test_zone_sweep_derive_k_ct_refuses_without_a_measurement();
    test_zone_sweep_derive_k_ct_refuses_an_uncommissioned_prior_k();
    test_zone_sweep_derive_k_ct_refuses_an_implausible_correction();
    test_zone_sweep_expected_coil_current_equal_split_default();
    test_zone_sweep_expected_coil_current_override_wins();
    test_zone_sweep_expected_coil_current_divides_by_zone_count_not_relay_count();
    test_zone_sweep_expected_coil_current_refuses_without_inputs();
    test_zone_sweep_check_expected_current_ok_and_mismatch();
    test_zone_sweep_check_expected_current_refuses_without_data();
    test_zone_sweep_plan_k_ct_clean_run_plans_every_derived_channel();
    test_zone_sweep_plan_k_ct_summed_derives_from_shared_channel();
    test_zone_sweep_plan_k_ct_summed_refuses_when_any_zone_unmeasured();
    test_zone_sweep_plan_k_ct_summed_skips_a_manually_calibrated_channel();
    test_zone_sweep_plan_k_ct_per_zone_path_unaffected_by_summed_fix();
    test_zone_sweep_plan_k_ct_skips_a_manually_calibrated_channel();
    test_zone_sweep_plan_k_ct_plans_every_channel_when_none_are_manual();
    test_zone_sweep_plan_k_ct_refuses_an_incomplete_run();
    test_zone_sweep_plan_k_ct_refuses_after_a_failed_map_push();
    test_zone_sweep_plan_k_ct_refuses_an_unanswered_nameplate();
    test_zone_sweep_plan_k_ct_implausible_reason_is_not_truncated();
    test_zone_sweep_push_k_ct_happy_path_writes_and_confirms();
    test_zone_sweep_push_k_ct_unconfirmed_readback_derives_nothing_and_backs_out();
    test_zone_sweep_push_k_ct_rejected_commit_backs_the_staging_out();
    test_zone_sweep_push_k_ct_partial_staging_failure_backs_out_what_staged();
    test_zone_sweep_push_k_ct_backout_restores_the_uncommissioned_zero();
    test_zone_sweep_push_kct_and_inormal_calibrating_sweep_arms_the_guard();
    test_zone_sweep_push_kct_and_inormal_no_kct_change_falls_back_unchanged();

    test_zone_sweep_plan_i_normal_nothing_measured_yet();
    test_zone_sweep_plan_i_normal_plans_every_measured_zone();
    test_zone_sweep_plan_i_normal_survives_a_partial_resweep();
    test_zone_normals_invalidate_mask_clears_measured_bits_and_amps();
    test_zone_normals_invalidate_mask_noop_on_zero_mask();

    test_ct_mapping_mismatch_silent_when_never_measured();
    test_ct_mapping_mismatch_within_band_is_silent();
    test_ct_mapping_mismatch_outside_band_warns();
    test_ct_mapping_mismatch_tiny_normal_needs_absolute_delta_too();
    test_ct_mapping_warn_mask_wiring();

    test_zones_current_sweep_start_wired_refusals();
    test_zones_current_sweep_start_atomic_gate_closes_the_race();

    test_reconcile_on_link_up_null_link_is_a_noop();
    test_reconcile_on_link_up_invalid_config_is_a_noop();
    test_reconcile_on_link_up_raises_when_pico_ceiling_is_unknown();
}

/* test_zones_config_cfg_fs.c -- separate TU, same executable (see that
 * file's header comment and build_host_tests.ps1's $cmd2). Declared here
 * rather than in a shared header since nothing else needs it. */
extern void run_test_zones_config_cfg_fs(void);

/* test_relay_names_cfg_fs.c -- same convention, item 3's cfg_fs dual-write
 * bridge tests. */
extern void run_test_relay_names_cfg_fs(void);

int main(void)
{
    run_test_zones_http();
    run_test_zones_config_cfg_fs();
    run_test_relay_names_cfg_fs();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
