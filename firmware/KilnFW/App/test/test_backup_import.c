// Host tests for App/drivers/http/backup_http.c's import validation pass
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
#include <stdlib.h>
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
void test_stub_zones_set_model(uint8_t zone_index, float k_dc, float tau_s, float dead_time_s);

// Defined in test_kiln_cfg_store.c -- see its own doc comment; flips one
// byte of the shared zones_config_export_blob() stub content.
void test_stub_kiln_cfg_export_content_toggle_byte0(void);

// Defined in test_safety_cfg_store.c -- flags the live safety_cfg_store cache as
// fetched without touching its contents. kiln_cfg_store only captures the Pico
// half of a slot from a fetched cache, and these tests expect populated slots.
void test_safety_cfg_store_mark_fetched_for_kiln_cfg_store_test(void);

// relay_authority.h -- backup_import.c's system_mode_gate wiring
// (docs/SYSTEM_MODE_GATE_PLAN.md, gate-slices-2/4/5, 2026-09-25). Defaults
// idle so every pre-existing test in this file keeps exercising exactly the
// scenario it did before; test_backup_import_refused_by_system_mode_gate()
// below is the one test that sets this to prove the refusal is wired.
static bool s_test_profile_running_for_mode_gate = false;
static bool s_test_autotune_running_for_mode_gate = false;
// Reviewer addition (A4 follow-up A): sample the restore-in-flight flag at
// the moment the job-side re-check reads the heat claim, so a test can prove
// the flag is actually SET there (store-then-read pairing), not merely clear
// before and after.
bool backup_import_restore_in_flight(void);
static bool s_test_restore_flag_seen_set_at_heat_read = false;
void relay_authority_heat_run_active(bool *profile_running_out, bool *autotune_running_out)
{
    if (backup_import_restore_in_flight()) s_test_restore_flag_seen_set_at_heat_read = true;
    if (profile_running_out) *profile_running_out = s_test_profile_running_for_mode_gate;
    if (autotune_running_out) *autotune_running_out = s_test_autotune_running_for_mode_gate;
}

// backup_http.c split into four files 2026-09-04 (ROADMAP.md M15's
// 1500-line item) -- backup_json.c/backup_export.c/backup_import.c/
// backup_http.c, see backup_http_internal.h for the map. All four are still
// pulled in here, same convention as before the split (backup_import_apply()
// is `static` with no public seam, and the other three still have to
// compile and link even though these tests only ever call it).
#include "../drivers/persist/backup_json.c"
#include "../drivers/http/backup_export.c"
#include "../drivers/http/backup_import.c"

// backup_import_apply() gained mode/dry_run/ack_delete_count/
// ack_no_safety_processor/plan/partial_write parameters (kiln_configs[]
// restore, 2026-09-19) -- every pre-existing call site in this file predates
// that and only cares about the profiles/zones behaviour, always wants a
// real (non-dry-run) MERGE apply with no pending mirror-deletes to
// acknowledge, and has no reason to inspect the plan or the partial-write
// flag. Rather than hand-edit all of them, this thin wrapper supplies
// KILN_CFG_RESTORE_MERGE/false/-1/true/scratch plan+flag so every old call
// site unchanged in spirit still compiles and behaves exactly as before;
// tests that specifically exercise kiln_configs[]/mode/dry_run/ack-delete
// call backup_import_apply() directly instead.
static kiln_cfg_plan_t s_test_backup_plan;
static bool test_backup_import_apply(const char *body, char *err_msg, size_t err_cap)
{
    memset(&s_test_backup_plan, 0, sizeof(s_test_backup_plan));
    bool partial_write = false;
    return backup_import_apply(body, KILN_CFG_RESTORE_MERGE, false, -1, true, &s_test_backup_plan, &partial_write,
                               err_msg, err_cap);
}
#include "../drivers/http/backup_http.c"
// WP9 (GITHUB_RELEASE_UPDATE_PLAN.md): update_settings_http.c is #included, not linked, so the tests
// below can call its static POST/GET handlers directly (its other two symbols resolve against
// the real http_auth_http.c / the NULL wifi_provision_http_get_server() fake in this executable).
esp_err_t httpd_resp_send_500(httpd_req_t *r); // defined below with the other httpd stubs
#include "../drivers/update/update_settings_http.c"

// LIVE_EDIT_WORKING_SLOT_ID (== PROFILES_MAX_COUNT) -- used by the
// live-edit-slot-id-rejected test below (task 7,
// PROFILE_SLOTS_100_PLAN.md section 7). Header-only, no persistence
// functions linked in, so this pulls in nothing beyond profiles_types.h,
// already a transitive dependency of every include above.
#include "../drivers/persist/live_profile.h"

#undef asm

// The shared settings_source chain-walk (zones_http.h's
// zones_config_settings_source_import_has_cycle() is backed by this same
// code in zones_http.c) -- included AFTER backup_http.c so the MAX31856.h/
// zones_http.h types it needs are already in scope from that file's own
// #include chain, same convention as everything else in this stub section.
// The stub below calls this directly instead of re-implementing the walk,
// so a break in the SHIPPED algorithm (zones_http.c's real
// zone_settings_source_chain.h use) shows up here too -- see this header's
// own comment for why that gap existed before today.
#include "../drivers/persist/zone_settings_source_chain.h"

// docs/WEB_AUTH_PLAN.md item 12b -- web_auth_store.c (linked for real, see
// build_host_tests.ps1's comment on this executable's $sources entry) so
// test_export_never_contains_credential_markers() below can seed a REAL
// credential and check its actual hash/salt bytes, not merely the absence
// of the kiln_auth/web_auth/lcd_auth/auth_policy identifier strings. fake_kv.h
// gives fake_kv_reset_all()/hal_kv_init_partition() -- this executable
// already links the real fake_kv.c backend for boot_guard.c/kiln_cfg_store.c/
// etc. above. Its psa/crypto.h host stub needs exactly one
// g_stub_psa_import_key_result definition per executable; none of this
// executable's other files define it (ota_http.c/test_web_auth_store.c each
// live in their own separate executables), so it is defined here.
#include "web_auth_store.h"
#include "update_settings.h"
#include "fake_kv.h"
#include "psa/crypto.h"
psa_status_t g_stub_psa_import_key_result = PSA_SUCCESS;

#include "profiles_builtin.h"

// ---- live_profile.c's wire-format dependency, faked -----------------------
// live_profile.c (linked for real, see build_host_tests.ps1) calls
// profile_encode_current_blob()/profile_decode_blob() from its own
// save/load paths -- unreachable from this file's tests (only
// live_edit_name_collides() is ever called), but still referenced inside
// live_profile.c's object file, so the linker needs them to resolve.
// Deliberately trivial fakes, NOT the real wire format -- same convention
// and same guarded profile_decode_result_t shim as test_live_profile.c's
// identical fakes (see that file's header comment); the real format's own
// correctness is test_profiles_http.c's job.
//
// N5 follow-on: backup_import.c (self-included above at line 79) now pulls
// in profiles_http_internal.h directly (for profiles_http_first_free_slot()),
// which already declares the REAL profile_decode_result_t with these same
// three values -- so this shim must not redeclare it in that case, or MSVC
// reports "redefinition" (profiles_http_internal.h has no shim-cooperating
// guard of its own to check instead). Guarding on its own include guard
// macro, PROFILES_HTTP_INTERNAL_H, keeps this shim usable standalone too, in
// case some future edit stops pulling that header in.
#if !defined(PROFILE_DECODE_RESULT_SHIM_DECLARED) && !defined(PROFILES_HTTP_INTERNAL_H)
#define PROFILE_DECODE_RESULT_SHIM_DECLARED
typedef enum {
    PROFILE_DECODE_OK,
    PROFILE_DECODE_CORRUPT,
    PROFILE_DECODE_NEWER,
} profile_decode_result_t;
#endif

size_t profile_encode_current_blob(const profile_t *profile, void *out, size_t cap)
{
    if (!profile || !out || cap < sizeof(profile_t)) {
        return 0;
    }
    memcpy(out, profile, sizeof(profile_t));
    return sizeof(profile_t);
}

profile_decode_result_t profile_decode_blob(const void *blob, size_t len, profile_t *out, const char **err_reason)
{
    if (!blob || !out || len != sizeof(profile_t)) {
        if (err_reason) *err_reason = "bad length";
        return PROFILE_DECODE_CORRUPT;
    }
    memcpy(out, blob, sizeof(profile_t));
    return PROFILE_DECODE_OK;
}

// ---- profiles_builtin.h stub bodies ---------------------------------------
// live_profile.c (linked for real, see build_host_tests.ps1's comment on this
// executable's $sources entry -- Opus review of 5dd23944 finding B) also
// scans the read-only builtin catalogue inside live_edit_name_collides_ex().
// This executable has no other reason to link the real profiles_builtin.c
// table, so a "no builtins exist" fake is enough for most tests here --
// same convention as test_profiles_http.c's own g_fake_builtin.
//
// Opus review of 5dd23944, finding 1/BLOCKER: this toggle now defaults ON
// for one id (rather than being permanently off) so
// test_backup_import_allows_profile_named_like_a_builtin() below actually
// exercises the include_builtins=false arm of backup_import.c's pass-1
// dup-name check -- with the catalogue permanently empty, that test would
// pass vacuously (nothing to collide against either way), the same class of
// gap the BLOCKER finding itself came from. Off by default so every
// pre-existing test in this file (none of which cares about builtins) is
// unaffected.
static bool g_fake_builtin_on = false;
bool profiles_builtin_id_valid(uint8_t id)
{
    return g_fake_builtin_on && id == PROFILE_BUILTIN_ID_BASE;
}
const builtin_profile_t *profiles_builtin_entry(uint8_t id)
{
    static builtin_profile_t fake;
    if (!profiles_builtin_id_valid(id)) {
        return NULL;
    }
    memset(&fake, 0, sizeof(fake));
    strcpy((char *)fake.code, "C6DHSC");
    return &fake;
}

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
static char s_send_last_body[320] = "";
static int s_send_last_err_code = 0;
static char s_send_last_err_msg[96] = "";
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, long long buf_len)
{
    (void)r;
    s_send_last_body[0] = '\0';
    if (buf != NULL) {
        size_t n = buf_len < 0 ? strlen(buf) : (size_t)buf_len;
        if (n >= sizeof(s_send_last_body)) {
            n = sizeof(s_send_last_body) - 1;
        }
        memcpy(s_send_last_body, buf, n);
        s_send_last_body[n] = '\0';
    }
    return ESP_OK;
}
/* Export-side capture: backup_export_get_handler() streams its JSON out
 * through httpd_resp_send_chunk() (see backup_stream_flush()), never
 * httpd_resp_send() -- every prior test in this file only ever calls
 * backup_import_apply() directly, so this stayed a no-op. The export tests
 * below need to see what was actually streamed, so this appends every
 * chunk (including the final buf==NULL/buf_len==0 terminator, which appends
 * nothing) into a growing heap buffer a test can inspect via
 * test_export_capture_reset()/s_export_body. */
static char *s_export_body;
static size_t s_export_len;
static size_t s_export_cap;

static void test_export_capture_reset(void)
{
    free(s_export_body);
    s_export_body = NULL;
    s_export_len = 0;
    s_export_cap = 0;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, size_t buf_len)
{
    (void)r;
    if (!buf || buf_len == 0) {
        return ESP_OK; /* the chunked-terminator call */
    }
    size_t need = s_export_len + buf_len + 1;
    if (need > s_export_cap) {
        size_t new_cap = s_export_cap ? s_export_cap * 2 : 1024;
        while (new_cap < need) {
            new_cap *= 2;
        }
        char *grown = realloc(s_export_body, new_cap);
        if (!grown) {
            return ESP_FAIL;
        }
        s_export_body = grown;
        s_export_cap = new_cap;
    }
    memcpy(s_export_body + s_export_len, buf, buf_len);
    s_export_len += buf_len;
    s_export_body[s_export_len] = '\0';
    return ESP_OK;
}
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t error, const char *msg)
{
    (void)r;
    s_send_last_err_code = (int)error;
    snprintf(s_send_last_err_msg, sizeof(s_send_last_err_msg), "%s", msg != NULL ? msg : "");
    return ESP_OK;
}
esp_err_t httpd_resp_send_500(httpd_req_t *r)
{
    (void)r;
    s_send_last_err_code = 500;
    s_send_last_err_msg[0] = '\0';
    return ESP_FAIL;
}
// Task 1c (docs/SYSTEM_MODE_GATE_PLAN.md known gap): the handler-level order
// test below (test_backup_import_post_*) needs to see what status/body the
// real backup_import_post_handler() actually sent for a refusal, so these two
// capture their last argument into static buffers rather than discarding it.
// Purely additive -- every prior test in this file calls backup_import_apply()
// directly and never inspects these, so capturing here cannot change any
// existing test's outcome.
static char s_post_last_status[32] = "";
static char s_post_last_body[256] = "";
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status)
{
    (void)r;
    if (status) {
        strncpy(s_post_last_status, status, sizeof(s_post_last_status) - 1);
        s_post_last_status[sizeof(s_post_last_status) - 1] = '\0';
    }
    return ESP_OK;
}
// 2026-09-25 fix-then-push review: backup_import_post_handler() now sends a
// 409 body via httpd_resp_sendstr() when http_async_job_busy() refuses a
// concurrent restore -- stub it the same way every other test file in this
// executable already does for its own handler under test.
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s)
{
    (void)r;
    if (s) {
        strncpy(s_post_last_body, s, sizeof(s_post_last_body) - 1);
        s_post_last_body[sizeof(s_post_last_body) - 1] = '\0';
    }
    return ESP_OK;
}
// Optional body feed for tests that drive backup_import_job() directly
// (A4 review, 2026-09-28). NULL (the default) keeps the original "return 0"
// behaviour every earlier test relied on.
static const char *s_recv_feed = NULL;
static size_t s_recv_feed_off = 0;
int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len)
{
    (void)r;
    if (!s_recv_feed) {
        return 0;
    }
    size_t left = strlen(s_recv_feed) - s_recv_feed_off;
    size_t n = left < buf_len ? left : buf_len;
    memcpy(buf, s_recv_feed + s_recv_feed_off, n);
    s_recv_feed_off += n;
    return (int)n;
}

// wifi_provision_http_get_server() used to be defined by test_wifi_prov.c
// (also linked into this executable) -- that file moved to its own
// executable 2026-09-28 (see build_host_tests.ps1's comment), so this
// translation unit needs its own fake now.
httpd_handle_t wifi_provision_http_get_server(void) { return NULL; }

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

// Task 1c: proves backup_import_post_handler()'s mode-gate -> interlock ->
// http_async_job_busy() ordering the same way test_zones_http.c's
// g_probe_interlock_called does -- a call counter, additive to the existing
// stub, so test_kiln_cfg_store.c's own interlock-backstop test (sharing this
// one definition) is unaffected.
int g_stub_ota_interlock_call_count = 0;

ota_interlock_result_t ota_http_check_interlocks(bool ack_no_safety_processor, char *reason_out,
                                                 size_t reason_cap)
{
    g_stub_ota_interlock_call_count++;
    g_stub_ota_interlock_saw_ack = ack_no_safety_processor;
    if (reason_out && reason_cap) {
        strncpy(reason_out, g_stub_ota_interlock_reason, reason_cap - 1);
        reason_out[reason_cap - 1] = '\0';
    }
    return g_stub_ota_interlock_result;
}

// The other two ota_http.h symbols backup_http.c's handler now references.
// ota_http_req_ack_no_safety() is reachable now that Task 1c drives the real
// handler directly -- controllable via the global below (default false,
// unchanged from this stub's original hardcoded body).
bool g_stub_ota_ack_no_safety = false;
bool ota_http_req_ack_no_safety(httpd_req_t *req)
{
    (void)req;
    return g_stub_ota_ack_no_safety;
}

// Records the reason the code under test actually handed to the refusal
// sender, so a test can prove a specific interlock reason (e.g. the zone
// sweep's) reaches the response unmodified rather than only that the
// interlock was called.
static char g_stub_ota_last_refusal_reason[OTA_INTERLOCK_REASON_MAX] = "";
esp_err_t ota_http_send_interlock_refusal(httpd_req_t *req, ota_interlock_result_t r,
                                          const char *reason)
{
    (void)req;
    (void)r;
    snprintf(g_stub_ota_last_refusal_reason, sizeof(g_stub_ota_last_refusal_reason), "%s",
             reason ? reason : "");
    return ESP_OK;
}

// Task 1c (docs/SYSTEM_MODE_GATE_PLAN.md known gap): backup_import_post_handler()
// also checks http_async_job_busy() after the mode gate and the OTA
// interlock, the same three-stage order zones_post_handler() uses. Unlike
// the mode gate and the interlock, this symbol is NOT locally stubbed here --
// this file is #included into the "main" combined host-test executable
// (test_main.c), which links exactly one real http_async_job_busy() (reached
// via test_http_async_job.c's own #include of http_async_job.c). Admitting a
// real job to force it busy would leave that ONE SHARED instance busy for
// every test that runs afterward in this executable (host xTaskCreate() never
// actually runs the job body to clear it -- the same "MUST RUN LAST" hazard
// test_zones_http.c's own busy test documents), and dozens of unrelated
// suites run after this one (run_test_kiln_cfg_store(), run_test_safety_cfg_store(),
// run_test_http_async_job() itself, etc.) -- so that path is not exercised
// here. The busy-refused leg of the ordering is covered by direct code
// inspection instead (backup_import.c's handler calls http_async_job_busy()
// immediately after ota_http_check_interlocks() returns OK, before reading
// the body) plus the two tests below, which prove the two legs that ARE safe
// to drive end-to-end without touching the shared job singleton.

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
    bool set_fuzzy_strength_called;
    float fuzzy_strength_pct;
    /* 2026-08-30 (ZONES_CFG_VERSION 10->11): row, not a pair -- one flag/
     * value PER CELL, since backup_http.c now commits via the single-cell
     * setter (zones_config_set_coupling_cell()) rather than the whole-row
     * one, one cell at a time, so a partial import (some cells present,
     * others not) is observable per cell, matching the real setter's own
     * per-cell semantics. */
    bool set_coupling_cell_called[MAX31856_CHANNEL_COUNT];
    float coupling_coeff[MAX31856_CHANNEL_COUNT];
    /* ZONES_CFG_VERSION 11->12 (DATA PLUMBING pass): the setter widened to
     * carry tau_s/dead_time_s alongside coeff -- tracked here the same
     * per-cell way. */
    float coupling_tau_s[MAX31856_CHANNEL_COUNT];
    float coupling_dead_time_s[MAX31856_CHANNEL_COUNT];
    /* One flag/value PER GROUP now (Opus review of 5672719, item 1/4): the
     * backup format widened from a single scalar fanned out to every group
     * to one value per SRC_GROUP_COUNT group, so this stub must be able to
     * tell groups apart to prove the fan-out (or its absence) for real. */
    bool set_settings_source_called[SRC_GROUP_COUNT];
    uint8_t settings_source[SRC_GROUP_COUNT];
    /* ZONES_CFG_VERSION 14->15 (PID_EXPANSION_PLAN.md 3.2 follow-up): the
     * coupling identification's own diagonal cell -- same "settable, called
     * flag observable" convention as set_fuzzy_strength_called above. */
    bool set_coupling_diag_k_dc_called;
    float coupling_diag_k_dc;
    /* 2026-09-16 backup-round-trip-gap closure (owner: "shouldn't ct normals
     * be part of a config backup?") -- one flag/value per newly-closed
     * field, same convention as every field above. normal_current_a is the
     * owner's own named example (CT calibration); the rest close every
     * other confirmed-closeable gap found in the same audit. */
    bool set_ease_off_window_mult_called;
    float ease_off_window_mult;
    bool set_approach_rate_cap_called;
    float approach_rate_cap_c_per_hr;
    bool set_error_band_c_called;
    float error_band_c;
    bool set_rate_band_c_per_s_called;
    float rate_band_c_per_s;
    bool set_relay_type_called;
    uint8_t relay_type;
    bool set_progress_band_c_called;
    float progress_band_c;
    bool set_zone_type_called;
    uint8_t zone_type;
    bool set_model_fit_context_called;
    float model_fit_temp_c, model_fit_ambient_c;
    bool set_coil_power_w_called;
    float coil_power_w;
    bool set_autotune_baseline_k_dc_called;
    float autotune_baseline_k_dc;
    bool set_adaptive_tune_enabled_called;
    bool adaptive_tune_enabled;
    bool set_tuning_quality_called;
    zone_tuning_quality_t tuning_quality;
    uint32_t tuning_seq; /* models zone_cfg_t::tuning_seq: bumped on EVERY setter call, like the real one */
    bool set_normal_current_called;
    bool normal_current_measured;
    float normal_current_a;
    /* 2026-09-16 backup-round-trip-gap closure, group 1/2/3: same
     * flag/value-per-field convention as every field above. */
    bool set_failsafe_state_called;
    bool failsafe_state;
    bool set_hyst_c_called;
    float hyst_c;
    bool set_min_on_s_called;
    uint16_t min_on_s;
    bool set_min_off_s_called;
    uint16_t min_off_s;
    bool set_timing_profile_index_called;
    uint8_t timing_profile_index;
} zone_write_t;

/* Group 3: the named timing_profiles[] bundle -- not per-zone, so kept
 * outside zone_write_t, mirroring the real zones_cfg_t::timing_profiles[]/
 * timing_profile_count split. */
typedef struct {
    char name[64];
    float progress_duty_min, progress_window_s, drift_hysteresis_c, frozen_eps_c, cross_zone_period_s,
        bangbang_hysteresis_c, cooling_limited_margin_c, cooling_limited_hold_s, ramp_lock_band_c;
} timing_profile_write_t;
static timing_profile_write_t s_timing_profiles[STUB_ZONE_COUNT];
static uint8_t s_timing_profile_count;
static uint8_t s_set_timing_profile_raw_calls;

/* Group 4: CT map / k_ct_v_per_a -- EXPORT-ONLY (see backup_export.c's own
 * comment on why import never restores these); this stub only needs to
 * feed the two derived-value getters backup_export.c calls, no setter
 * observability is needed since backup_import_apply() never calls one. */
static uint8_t s_ct_map_derived_mask;
static uint8_t s_ct_map_zone[MAX31856_CHANNEL_COUNT];
static uint8_t s_k_ct_derived_mask;
static float s_k_ct_v_per_a[MAX31856_CHANNEL_COUNT];

static zone_write_t s_writes[STUB_ZONE_COUNT];
static uint8_t s_relay_count = 4;
static bool s_safety_tc_set;
static uint8_t s_safety_tc_type;
// 2026-09-15 (Opus adversarial re-review, F6): declared here, ahead of
// reset_stub_state() below which seeds it, rather than down by the
// zones_get_safety_pico_tc_type() stub itself -- moved up from an initial
// placement that put the declaration AFTER reset_stub_state()'s use of it.
static uint8_t s_pico_tc_type;
static bool s_pico_tc_type_known = true;
/* 2026-09-16 config-backup round-trip gap closure: the Pico's OWN
 * i_normal_a[zi] (0x031A-0x031C), read via zones_get_safety_pico_i_normal_a()
 * -- same "known/value" shape as s_pico_tc_type/_known above. Defaults to
 * NOT known on every zone, matching the real bench (ct_cal reports
 * has_value:false on all 3 channels today) and this codebase's "skip
 * unmeasured rather than emit a false 0.0" export convention -- a test must
 * explicitly seed a zone's value via s_pico_i_normal_a_known[]/[]_a[] to see
 * the export key appear. */
static bool s_pico_i_normal_a_known[STUB_ZONE_COUNT];
static float s_pico_i_normal_a[STUB_ZONE_COUNT];

/* Forward declarations: definitions live further down (next to the
 * safety_cfg_write_apply_pairs() stub they belong to), but reset_stub_state()
 * above that point needs to clear them each test. */
extern bool g_stub_safety_cfg_write_result;
extern char g_stub_safety_cfg_write_reason[128];
extern int g_stub_safety_cfg_write_n_pairs;
extern bool g_stub_safety_cfg_write_commit_arg;
extern safety_cfg_post_pair_t g_stub_safety_cfg_write_pairs[8];
extern int g_settings_source_save_calls;
extern bool s_relay_type_pushed[STUB_ZONE_COUNT];
extern int g_relay_type_push_calls;
/* safety_ceiling_sync_apply_lower() fake (review of 34a2da1b): counts calls
 * and captures the per-zone target it was handed, so a test can assert
 * backup_import_apply() tracks the Pico ceiling DOWN to whatever the live
 * zones config holds after the import -- committed batch or rolled-back
 * snapshot. Defined below next to the guard_raise() fake. */
static int g_apply_lower_calls;
static float g_apply_lower_last_max[STUB_ZONE_COUNT];
static int g_total_write_calls;
static int g_profile_save_calls;
static uint8_t g_last_saved_profile_id;
static profile_t g_last_saved_profile;

// profiles_http_get()'s backing store -- defined here (not down in the
// "profiles_http.h stubs" section below) so reset_stub_state() can clear it;
// see that section for profiles_http_get()/test_stub_profiles_set().
static bool s_profile_present[PROFILES_MAX_COUNT];
static profile_t s_profile_slots[PROFILES_MAX_COUNT];

static void reset_stub_state(void)
{
    test_safety_cfg_store_mark_fetched_for_kiln_cfg_store_test();
    memset(s_writes, 0, sizeof(s_writes));
    /* Real decoded zones configs never leave a zone's settings_source at raw
     * 0 unintentionally -- zones_http.c's convert_zone_v9() etc. explicitly
     * seed ZONE_SETTINGS_SOURCE_CUSTOM for anything not actually configured
     * ("NEVER 0", see that function's own comment); 0 is a REAL, DIFFERENT
     * value ("copy zone 0's settings"), not a "not set" sentinel. The plain
     * memset(0) above would otherwise leave every stub zone looking like it
     * explicitly links to zone 0 -- indistinguishable, for zone 0 itself,
     * from a self-reference -- which is not a state real firmware ever
     * produces and would make zones_config_settings_source_import_has_cycle()
     * below manufacture a false cycle out of zones no test ever actually
     * linked. */
    for (uint8_t i = 0; i < STUB_ZONE_COUNT; i++) {
        for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
            s_writes[i].settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
        }
    }
    s_relay_count = 4;
    s_safety_tc_set = false;
    s_safety_tc_type = 0;
    s_pico_tc_type = 0;
    s_pico_tc_type_known = true;
    memset(s_pico_i_normal_a_known, 0, sizeof(s_pico_i_normal_a_known));
    memset(s_pico_i_normal_a, 0, sizeof(s_pico_i_normal_a));
    g_stub_safety_cfg_write_result = true;
    g_stub_safety_cfg_write_reason[0] = '\0';
    g_stub_safety_cfg_write_n_pairs = -1;
    g_stub_safety_cfg_write_commit_arg = false;
    memset(g_stub_safety_cfg_write_pairs, 0, sizeof(g_stub_safety_cfg_write_pairs));
    g_total_write_calls = 0;
    g_profile_save_calls = 0;
    g_last_saved_profile_id = 0;
    memset(&g_last_saved_profile, 0, sizeof(g_last_saved_profile));
    memset(s_profile_present, 0, sizeof(s_profile_present));
    memset(s_profile_slots, 0, sizeof(s_profile_slots));
    memset(s_timing_profiles, 0, sizeof(s_timing_profiles));
    s_timing_profile_count = 0;
    s_set_timing_profile_raw_calls = 0;
    s_ct_map_derived_mask = 0;
    memset(s_ct_map_zone, 0, sizeof(s_ct_map_zone));
    s_k_ct_derived_mask = 0;
    memset(s_k_ct_v_per_a, 0, sizeof(s_k_ct_v_per_a));
    test_stub_zones_set_thermo_count(3);
    for (uint8_t i = 0; i < STUB_ZONE_COUNT; i++) {
        test_stub_zones_set_max_ramp(i, true, 1000.0f);
    }
    g_settings_source_save_calls = 0;
    /* s_test_snapshot itself is not reset here -- zones_config_get_full_copy()
     * always overwrites it in full before any test's import can reach a
     * restore, same as the real zones_cfg_t snapshot it stands in for, so
     * stale content from a prior test can never be observed. */
    memset(s_relay_type_pushed, 0, sizeof(s_relay_type_pushed));
    g_relay_type_push_calls = 0;
    g_apply_lower_calls = 0;
    memset(g_apply_lower_last_max, 0, sizeof(g_apply_lower_last_max));
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

/* 2026-09-10 opus review, "the Pico-ceiling invariant is enforced at one
 * door only": backup_import.c (#included above) now calls
 * safety_ceiling_sync_guard_raise() before committing any zone tuning
 * entry, and references s_hw_safety (extern'd from zones_http_internal.h,
 * defined for real in zones_http.c -- not part of this executable's link,
 * same "own fake bodies" convention every other zones_config_get_*() stub
 * on this page already follows). s_hw_safety stays NULL here: this file's
 * job is backup_import_apply()'s validate-then-commit logic, not the
 * ceiling-sync feature itself (that is test_safety_ceiling_policy.c at the
 * pure-logic layer and test_zones_http.c's test_reconcile_on_link_up_*()
 * plus its own backup-import-specific ceiling test at the ESP-glue layer)
 * -- and safety_ceiling_sync_guard_raise() with link == NULL always
 * returns true (NONE), which is exactly "nothing to guard" and matches
 * every pre-existing test in this file's assumption that importing a
 * backup's zone tuning always succeeds once validated. */
SafetyLinkClass *s_hw_safety = NULL;

bool safety_ceiling_sync_guard_raise(SafetyLinkClass *link, const float *new_max_temp_c, size_t n,
                                      safety_ceiling_sync_result_t *out_result, char *reason_out,
                                      size_t reason_cap, safety_ceiling_refusal_class_t *out_refusal_class)
{
    (void)link;
    (void)new_max_temp_c;
    (void)n;
    if (out_result) {
        *out_result = SAFETY_CEILING_SYNC_NONE;
    }
    if (reason_out && reason_cap > 0) {
        reason_out[0] = '\0';
    }
    if (out_refusal_class) {
        *out_refusal_class = SAFETY_CEILING_REFUSAL_NONE;
    }
    return true;
}

void safety_ceiling_sync_apply_lower(SafetyLinkClass *link, const float *new_max_temp_c, size_t n,
                                      safety_ceiling_sync_result_t *out_result, char *reason_out,
                                      size_t reason_cap)
{
    (void)link;
    g_apply_lower_calls++;
    for (size_t i = 0; i < n && i < STUB_ZONE_COUNT; i++) {
        g_apply_lower_last_max[i] = new_max_temp_c[i];
    }
    if (out_result) {
        *out_result = SAFETY_CEILING_SYNC_NONE;
    }
    if (reason_out && reason_cap > 0) {
        reason_out[0] = '\0';
    }
}

/* 2026-09-16 config-backup round-trip gap closure: backup_import.c
 * (#included above) now calls safety_cfg_write_apply_pairs() to push the
 * backup's safety_i_normal_a values back onto the Pico through the EXISTING
 * stage/COMMIT_CONFIG/forced-read-back-confirm machinery. That real function
 * lives in safety_cfg_write.c, which is NOT linked into this executable
 * (same "own fake bodies, real module linked elsewhere" convention as
 * safety_ceiling_sync_guard_raise() just above) -- test_safety_cfg_http.c is
 * the executable that exercises the real implementation's staging/commit/
 * confirm logic. This file's job is only backup_import_apply()'s own
 * validate-then-commit control flow: did it build the right pairs, did it
 * abort the WHOLE import loudly on a refused/unconfirmed write, did it skip
 * the call entirely when the backup carries no safety_i_normal_a key.
 * Controllable via g_stub_safety_cfg_write_result; captures every pair
 * passed in g_stub_safety_cfg_write_pairs/_n_pairs so a test can assert the
 * exact param_id/value_text backup_import.c built. */
bool g_stub_safety_cfg_write_result = true;
char g_stub_safety_cfg_write_reason[128] = "";
safety_cfg_post_pair_t g_stub_safety_cfg_write_pairs[8];
int g_stub_safety_cfg_write_n_pairs = -1; /* -1 == "never called" */
bool g_stub_safety_cfg_write_commit_arg = false;

bool safety_cfg_write_apply_pairs(SafetyLinkClass *link, const safety_cfg_post_pair_t *pairs, int n_pairs,
                                   bool commit, char *reason_out, size_t reason_cap,
                                   safety_ceiling_refusal_class_t *out_class)
{
    (void)link;
    g_stub_safety_cfg_write_n_pairs = n_pairs;
    g_stub_safety_cfg_write_commit_arg = commit;
    int n_copy = (n_pairs < 8) ? n_pairs : 8;
    for (int i = 0; i < n_copy; i++) {
        g_stub_safety_cfg_write_pairs[i] = pairs[i];
    }
    if (reason_out && reason_cap > 0) {
        strncpy(reason_out, g_stub_safety_cfg_write_reason, reason_cap - 1);
        reason_out[reason_cap - 1] = '\0';
    }
    if (out_class) {
        *out_class = g_stub_safety_cfg_write_result ? SAFETY_CEILING_REFUSAL_NONE : SAFETY_CEILING_REFUSAL_OTHER;
    }
    return g_stub_safety_cfg_write_result;
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

bool zones_config_get_fuzzy_strength_pct(uint8_t zone_index, float *out_pct)
{
    if (!out_pct || zone_index >= STUB_ZONE_COUNT) {
        return false;
    }
    *out_pct = s_writes[zone_index].fuzzy_strength_pct;
    return true;
}

/* Cleared to "the getter answers" by this file's stub reset; test_profile_
 * feasibility.c flips it through test_stub_zones_set_coupling() below to
 * exercise profile_feasibility.c's "no coupling matrix at all" path. */
static bool s_coupling_getter_answers[STUB_ZONE_COUNT] = { true, true, true };

/* Cross-file hook for test_profile_feasibility.c -- same arrangement, and the
 * same reason, as test_stub_zones_set_max_ramp() in the other direction: this
 * file owns the only definition of zones_config_get_coupling() linked into the
 * host-test binary, so the feasibility tests drive that state from here rather
 * than defining a second (multiply-defined) body of their own. */
void test_stub_zones_set_coupling(uint8_t zone_index, bool answers,
                                  const float row[MAX31856_CHANNEL_COUNT])
{
    if (zone_index >= STUB_ZONE_COUNT) {
        return;
    }
    s_coupling_getter_answers[zone_index] = answers;
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        s_writes[zone_index].coupling_coeff[j] = row ? row[j] : 0.0f;
    }
}

bool zones_config_get_coupling(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (!out_row || zone_index >= STUB_ZONE_COUNT || !s_coupling_getter_answers[zone_index]) {
        return false;
    }
    memcpy(out_row, s_writes[zone_index].coupling_coeff, sizeof(s_writes[zone_index].coupling_coeff));
    /* Mirror zones_config_accessors.c's own on/off belt-and-braces mask
     * (docs/ON_OFF_ZONE_PLAN.md sec 1) so this stub's behavior matches
     * production closely enough to reproduce the bench A4 (2026-09-28)
     * backup-export bug: a currently on/off zone reads its whole row as
     * zero, and any OTHER zone reads a zero in the column of an on/off
     * neighbor. zones_config_get_coupling_raw() below is the unmasked
     * sibling backup_export.c now uses instead. */
    if (zone_index < STUB_ZONE_COUNT && s_writes[zone_index].zone_type == (uint8_t)ZONE_TYPE_ON_OFF) {
        memset(out_row, 0, MAX31856_CHANNEL_COUNT * sizeof(out_row[0]));
        return true;
    }
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        if (j < STUB_ZONE_COUNT && s_writes[j].zone_type == (uint8_t)ZONE_TYPE_ON_OFF) {
            out_row[j] = 0.0f;
        }
    }
    return true;
}

/* RAW sibling of the stub above -- see zones_config_accessors.c's real
 * zones_config_get_coupling_raw() for why backup_export.c must use this one
 * instead of the masking getter. */
bool zones_config_get_coupling_raw(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (!out_row || zone_index >= STUB_ZONE_COUNT || !s_coupling_getter_answers[zone_index]) {
        return false;
    }
    memcpy(out_row, s_writes[zone_index].coupling_coeff, sizeof(s_writes[zone_index].coupling_coeff));
    return true;
}

bool zones_config_get_coupling_tau(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (!out_row || zone_index >= STUB_ZONE_COUNT) {
        return false;
    }
    memcpy(out_row, s_writes[zone_index].coupling_tau_s, sizeof(s_writes[zone_index].coupling_tau_s));
    return true;
}

bool zones_config_get_coupling_dead_time(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (!out_row || zone_index >= STUB_ZONE_COUNT) {
        return false;
    }
    memcpy(out_row, s_writes[zone_index].coupling_dead_time_s, sizeof(s_writes[zone_index].coupling_dead_time_s));
    return true;
}

bool zones_config_get_coupling_diag_k_dc(uint8_t zone_index, float *out_k_dc)
{
    if (!out_k_dc || zone_index >= STUB_ZONE_COUNT) {
        return false;
    }
    *out_k_dc = s_writes[zone_index].coupling_diag_k_dc;
    return true;
}

/* docs/ARCHITECTURE_DECISIONS.md#zones-page-clean-up-info-disclosure-schema-v20-v21-chartjs (ZONES_CFG_VERSION 20->21) widened the real
 * accessor with a `group` parameter, and the backup format itself now
 * carries a value per SRC_GROUP_COUNT group too (Opus review of 5672719,
 * item 4) -- so this stub keeps s_writes[].settings_source[group], one slot
 * per group, matching the real per-group behavior it stands in for. */
bool zones_config_get_settings_source(uint8_t zone_index, uint8_t group, uint8_t *out_settings_source)
{
    if (!out_settings_source || zone_index >= STUB_ZONE_COUNT || group >= SRC_GROUP_COUNT) {
        return false;
    }
    *out_settings_source = s_writes[zone_index].settings_source[group];
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

// 2026-09-15 (Opus adversarial re-review, F6): backup_export.c now reads the
// Pico's live tc_type via this getter instead of the ESP's own possibly-
// stale zones_config_get_safety_tc_type() cache above -- deliberately a
// SEPARATE stub value (s_pico_tc_type != s_safety_tc_type by default in
// reset_all()) so a test can prove the export path actually switched
// getters rather than happening to read the same number from both.
// (s_pico_tc_type/s_pico_tc_type_known are declared earlier, alongside
// s_safety_tc_type, so reset_stub_state() can seed them.)
bool zones_get_safety_pico_tc_type(uint8_t *out_tc_type)
{
    if (!out_tc_type) {
        return false;
    }
    if (!s_pico_tc_type_known) {
        *out_tc_type = 0;
        return false;
    }
    *out_tc_type = s_pico_tc_type;
    return true;
}

/* 2026-09-16 config-backup round-trip gap closure: own stub, real accessor
 * links into a different executable -- see s_pico_i_normal_a_known's own
 * comment above. */
bool zones_get_safety_pico_i_normal_a(uint8_t zi, float *out_a)
{
    if (!out_a || zi >= STUB_ZONE_COUNT) {
        return false;
    }
    if (!s_pico_i_normal_a_known[zi]) {
        *out_a = 0.0f;
        return false;
    }
    *out_a = s_pico_i_normal_a[zi];
    return true;
}

/* 2026-09-16 backup-round-trip-gap closure -- getters for every field
 * backup_export.c now reads unconditionally (once zi has answered
 * pid_kp) return true whenever zone_index is in bounds, same as the
 * plain scalar getters above; the two CONDITIONAL export fields
 * (tuning_quality, normal_current_a) answer false/not-measured by
 * default so a test that doesn't seed them sees those keys omitted,
 * matching the real getters' own "0 means not yet answerable" gate. */
bool zones_config_get_ease_off_window_mult(uint8_t zone_index, float *out_mult)
{
    if (!out_mult || zone_index >= STUB_ZONE_COUNT) return false;
    *out_mult = s_writes[zone_index].ease_off_window_mult;
    return true;
}
bool zones_config_get_approach_rate_cap_c_per_hr(uint8_t zone_index, float *out_cap_c_per_hr)
{
    if (!out_cap_c_per_hr || zone_index >= STUB_ZONE_COUNT) return false;
    *out_cap_c_per_hr = s_writes[zone_index].approach_rate_cap_c_per_hr;
    return true;
}
bool zones_config_get_error_band_c(uint8_t zone_index, float *out_band_c)
{
    if (!out_band_c || zone_index >= STUB_ZONE_COUNT) return false;
    *out_band_c = s_writes[zone_index].error_band_c;
    return true;
}
bool zones_config_get_rate_band_c_per_s(uint8_t zone_index, float *out_band_c_per_s)
{
    if (!out_band_c_per_s || zone_index >= STUB_ZONE_COUNT) return false;
    *out_band_c_per_s = s_writes[zone_index].rate_band_c_per_s;
    return true;
}
bool zones_config_get_relay_type(uint8_t zone_index, uint8_t *out_relay_type)
{
    if (!out_relay_type || zone_index >= STUB_ZONE_COUNT) return false;
    *out_relay_type = s_writes[zone_index].relay_type;
    return true;
}
bool zones_config_get_progress_band_c(uint8_t zone_index, float *out_band_c)
{
    if (!out_band_c || zone_index >= STUB_ZONE_COUNT) return false;
    *out_band_c = s_writes[zone_index].progress_band_c;
    return true;
}
bool zones_config_get_zone_type(uint8_t zone_index, zone_type_t *out_type)
{
    if (!out_type || zone_index >= STUB_ZONE_COUNT) return false;
    *out_type = (zone_type_t)s_writes[zone_index].zone_type;
    return true;
}
bool zones_config_get_model_fit_context(uint8_t zone_index, float *out_fit_temp_c, float *out_fit_ambient_c)
{
    if (!out_fit_temp_c || !out_fit_ambient_c || zone_index >= STUB_ZONE_COUNT) return false;
    *out_fit_temp_c = s_writes[zone_index].model_fit_temp_c;
    *out_fit_ambient_c = s_writes[zone_index].model_fit_ambient_c;
    return true;
}
bool zones_config_get_coil_power_w(uint8_t zone_index, float *out_power_w)
{
    if (!out_power_w || zone_index >= STUB_ZONE_COUNT) return false;
    *out_power_w = s_writes[zone_index].coil_power_w;
    return true;
}
bool zones_config_get_autotune_baseline_k_dc(uint8_t zone_index, float *out_k_dc)
{
    if (!out_k_dc || zone_index >= STUB_ZONE_COUNT) return false;
    *out_k_dc = s_writes[zone_index].autotune_baseline_k_dc;
    return true;
}
bool zones_config_get_adaptive_tune_enabled(uint8_t zone_index)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    return s_writes[zone_index].adaptive_tune_enabled;
}
/* zones_config_get_tuning_quality()'s ONE definition in this host-test
 * binary lives in test_profile_feasibility.c (profile_feasibility.c's own
 * real production dependency) -- see that file's stub_zone_t comment. This
 * file reaches it through test_stub_zones_set_full_tuning_quality() instead
 * of redefining the symbol (multiple-definition link error), same
 * cross-file-hook convention as zones_config_get_thermo_count()/
 * zones_config_get_max_ramp() in the other direction. */
void test_stub_zones_set_full_tuning_quality(uint8_t zi, const zone_tuning_quality_t *tq);

bool zones_config_get_normal_current(uint8_t zone_index, float *out_amps, bool *out_measured)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    if (out_amps) *out_amps = s_writes[zone_index].normal_current_a;
    if (out_measured) *out_measured = s_writes[zone_index].normal_current_measured;
    return true;
}
/* 2026-09-16 backup-round-trip-gap closure, group 1/2/3 -- same
 * "always answerable once zi passed pid_kp" convention as ease_off_window_mult
 * etc. above. */
bool zones_config_get_failsafe_state(uint8_t zone_index, bool *out_on)
{
    if (!out_on || zone_index >= STUB_ZONE_COUNT) return false;
    *out_on = s_writes[zone_index].failsafe_state;
    return true;
}
bool zones_config_get_hyst_c(uint8_t zone_index, float *out_hyst_c)
{
    if (!out_hyst_c || zone_index >= STUB_ZONE_COUNT) return false;
    *out_hyst_c = s_writes[zone_index].hyst_c;
    return true;
}
bool zones_config_get_min_on_s(uint8_t zone_index, uint16_t *out_s)
{
    if (!out_s || zone_index >= STUB_ZONE_COUNT) return false;
    *out_s = s_writes[zone_index].min_on_s;
    return true;
}
bool zones_config_get_min_off_s(uint8_t zone_index, uint16_t *out_s)
{
    if (!out_s || zone_index >= STUB_ZONE_COUNT) return false;
    *out_s = s_writes[zone_index].min_off_s;
    return true;
}
bool zones_config_get_timing_profile_index(uint8_t zone_index, uint8_t *out_index)
{
    if (!out_index || zone_index >= STUB_ZONE_COUNT) return false;
    *out_index = s_writes[zone_index].timing_profile_index;
    return true;
}
uint8_t zones_config_get_timing_profile_count(void)
{
    return s_timing_profile_count;
}
bool zones_config_get_timing_profile_raw(uint8_t profile_index, char *out_name, size_t name_cap,
                                         float *out_progress_duty_min, float *out_progress_window_s,
                                         float *out_drift_hysteresis_c, float *out_frozen_eps_c,
                                         float *out_cross_zone_period_s, float *out_bangbang_hysteresis_c,
                                         float *out_cooling_limited_margin_c,
                                         float *out_cooling_limited_hold_s, float *out_ramp_lock_band_c)
{
    if (!out_name || name_cap == 0 || !out_progress_duty_min || !out_progress_window_s ||
        !out_drift_hysteresis_c || !out_frozen_eps_c || !out_cross_zone_period_s ||
        !out_bangbang_hysteresis_c || !out_cooling_limited_margin_c || !out_cooling_limited_hold_s ||
        !out_ramp_lock_band_c || profile_index >= s_timing_profile_count) {
        return false;
    }
    const timing_profile_write_t *tp = &s_timing_profiles[profile_index];
    strncpy(out_name, tp->name, name_cap - 1);
    out_name[name_cap - 1] = '\0';
    *out_progress_duty_min = tp->progress_duty_min;
    *out_progress_window_s = tp->progress_window_s;
    *out_drift_hysteresis_c = tp->drift_hysteresis_c;
    *out_frozen_eps_c = tp->frozen_eps_c;
    *out_cross_zone_period_s = tp->cross_zone_period_s;
    *out_bangbang_hysteresis_c = tp->bangbang_hysteresis_c;
    *out_cooling_limited_margin_c = tp->cooling_limited_margin_c;
    *out_cooling_limited_hold_s = tp->cooling_limited_hold_s;
    *out_ramp_lock_band_c = tp->ramp_lock_band_c;
    return true;
}
void zones_ct_channel_map_derived(uint8_t *out_derived_mask, uint8_t *out_zone_for_ch)
{
    /* Real implementation (zones_config_store.c) only ever writes
     * ZONE_CT_CHANNEL_COUNT (3) bytes into the caller's buffer -- matched
     * exactly here since backup_export.c's caller passes a buffer sized to
     * that constant, not MAX31856_CHANNEL_COUNT. */
    if (out_derived_mask) *out_derived_mask = s_ct_map_derived_mask;
    if (out_zone_for_ch) memcpy(out_zone_for_ch, s_ct_map_zone, ZONE_CT_CHANNEL_COUNT);
}
void zones_ct_k_v_per_a_derived(uint8_t *out_derived_mask, float *out_k_v_per_a)
{
    if (out_derived_mask) *out_derived_mask = s_k_ct_derived_mask;
    if (out_k_v_per_a) memcpy(out_k_v_per_a, s_k_ct_v_per_a, ZONE_CT_CHANNEL_COUNT * sizeof(float));
}

// ---- zones_http.h setters -- every one records the call and bumps the
// shared write counter, matching the real setters' "false means rejected,
// nothing written" shape (always true here since backup_import_apply()'s
// own pass 1 has already validated everything by the time pass 2 calls
// these). ----

/* Models zones_config_set_pid_no_save()'s unconditional tuning_valid = 0
 * (zones_config_accessors.c) -- the real setter clears the record on EVERY
 * gain change, which the earlier stub omitted and so let a commit-time
 * "matches live" compare pass here while failing on hardware. */
static void stub_invalidate_tuning_record(uint8_t zi)
{
    zone_tuning_quality_t cur;
    if (zones_config_get_tuning_quality(zi, &cur) && cur.valid) {
        cur.valid = false;
        test_stub_zones_set_full_tuning_quality(zi, &cur);
    }
}

bool zones_config_set_pid(uint8_t zone_index, float kp, float ki, float kd)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_pid_called = true;
    s_writes[zone_index].kp = kp;
    s_writes[zone_index].ki = ki;
    s_writes[zone_index].kd = kd;
    stub_invalidate_tuning_record(zone_index);
    g_total_write_calls++;
    return true;
}
bool zones_config_set_pid_no_save(uint8_t zone_index, float kp, float ki, float kd)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_pid_called = true;
    s_writes[zone_index].kp = kp;
    s_writes[zone_index].ki = ki;
    s_writes[zone_index].kd = kd;
    stub_invalidate_tuning_record(zone_index);
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
bool zones_config_set_model_no_save(uint8_t zone_index, float k_dc, float tau_s, float dead_time_s)
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
bool zones_config_set_tc_type_no_save(uint8_t zone_index, uint8_t tc_type)
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
bool zones_config_set_name_no_save(uint8_t zone_index, const char *name)
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
bool zones_config_set_relay_mask_no_save(uint8_t zone_index, uint8_t relay_mask)
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
bool zones_config_set_thermo_mask_no_save(uint8_t zone_index, uint8_t thermo_mask)
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
bool zones_config_set_ct_mask_no_save(uint8_t zone_index, uint8_t ct_mask)
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
bool zones_config_set_cal_offset_no_save(uint8_t zone_index, float cal_offset_c)
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
bool zones_config_set_max_ramp_no_save(uint8_t zone_index, float c_per_hr)
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
bool zones_config_set_sanity_rate_no_save(uint8_t zone_index, float c_per_min)
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
bool zones_config_set_control_mode_no_save(uint8_t zone_index, zone_control_mode_t mode)
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
bool zones_config_set_temp_limits_no_save(uint8_t zone_index, float max_temp_c, float min_temp_c)
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
bool zones_config_set_heater_cfg_no_save(uint8_t zone_index, float window_ms, float min_on_ms, float min_off_ms)
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
bool zones_config_set_guard_thresholds_no_save(uint8_t zone_index, float f1, float f2, float f3, float f4, float f5,
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
bool zones_config_set_cross_zone_delta_no_save(uint8_t zone_index, float max_delta_c)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_xzone_called = true;
    s_writes[zone_index].cross_zone_max_delta_c = max_delta_c;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_fuzzy_strength_pct(uint8_t zone_index, float pct)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_fuzzy_strength_called = true;
    s_writes[zone_index].fuzzy_strength_pct = pct;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_fuzzy_strength_pct_no_save(uint8_t zone_index, float pct)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_fuzzy_strength_called = true;
    s_writes[zone_index].fuzzy_strength_pct = pct;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_coupling_diag_k_dc(uint8_t zone_index, float k_dc)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_coupling_diag_k_dc_called = true;
    s_writes[zone_index].coupling_diag_k_dc = k_dc;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_coupling_diag_k_dc_no_save(uint8_t zone_index, float k_dc)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_coupling_diag_k_dc_called = true;
    s_writes[zone_index].coupling_diag_k_dc = k_dc;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_ease_off_window_mult(uint8_t zone_index, float mult)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_ease_off_window_mult_called = true;
    s_writes[zone_index].ease_off_window_mult = mult;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_ease_off_window_mult_no_save(uint8_t zone_index, float mult)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_ease_off_window_mult_called = true;
    s_writes[zone_index].ease_off_window_mult = mult;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_approach_rate_cap_c_per_hr(uint8_t zone_index, float cap_c_per_hr)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_approach_rate_cap_called = true;
    s_writes[zone_index].approach_rate_cap_c_per_hr = cap_c_per_hr;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_approach_rate_cap_c_per_hr_no_save(uint8_t zone_index, float cap_c_per_hr)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_approach_rate_cap_called = true;
    s_writes[zone_index].approach_rate_cap_c_per_hr = cap_c_per_hr;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_error_band_c(uint8_t zone_index, float band_c)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_error_band_c_called = true;
    s_writes[zone_index].error_band_c = band_c;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_error_band_c_no_save(uint8_t zone_index, float band_c)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_error_band_c_called = true;
    s_writes[zone_index].error_band_c = band_c;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_rate_band_c_per_s(uint8_t zone_index, float band_c_per_s)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_rate_band_c_per_s_called = true;
    s_writes[zone_index].rate_band_c_per_s = band_c_per_s;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_rate_band_c_per_s_no_save(uint8_t zone_index, float band_c_per_s)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_rate_band_c_per_s_called = true;
    s_writes[zone_index].rate_band_c_per_s = band_c_per_s;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_relay_type(uint8_t zone_index, uint8_t relay_type)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_relay_type_called = true;
    s_writes[zone_index].relay_type = relay_type;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_relay_type_no_save(uint8_t zone_index, uint8_t relay_type)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_relay_type_called = true;
    s_writes[zone_index].relay_type = relay_type;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_progress_band_c(uint8_t zone_index, float band_c)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_progress_band_c_called = true;
    s_writes[zone_index].progress_band_c = band_c;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_progress_band_c_no_save(uint8_t zone_index, float band_c)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_progress_band_c_called = true;
    s_writes[zone_index].progress_band_c = band_c;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_zone_type(uint8_t zone_index, zone_type_t type)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_zone_type_called = true;
    s_writes[zone_index].zone_type = (uint8_t)type;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_zone_type_no_save(uint8_t zone_index, zone_type_t type)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_zone_type_called = true;
    s_writes[zone_index].zone_type = (uint8_t)type;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_model_fit_context(uint8_t zone_index, float fit_temp_c, float fit_ambient_c)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_model_fit_context_called = true;
    s_writes[zone_index].model_fit_temp_c = fit_temp_c;
    s_writes[zone_index].model_fit_ambient_c = fit_ambient_c;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_model_fit_context_no_save(uint8_t zone_index, float fit_temp_c, float fit_ambient_c)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_model_fit_context_called = true;
    s_writes[zone_index].model_fit_temp_c = fit_temp_c;
    s_writes[zone_index].model_fit_ambient_c = fit_ambient_c;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_coil_power_w(uint8_t zone_index, float power_w)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_coil_power_w_called = true;
    s_writes[zone_index].coil_power_w = power_w;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_coil_power_w_no_save(uint8_t zone_index, float power_w)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_coil_power_w_called = true;
    s_writes[zone_index].coil_power_w = power_w;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_autotune_baseline_k_dc(uint8_t zone_index, float k_dc)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_autotune_baseline_k_dc_called = true;
    s_writes[zone_index].autotune_baseline_k_dc = k_dc;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_autotune_baseline_k_dc_no_save(uint8_t zone_index, float k_dc)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_autotune_baseline_k_dc_called = true;
    s_writes[zone_index].autotune_baseline_k_dc = k_dc;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_adaptive_tune_enabled(uint8_t zone_index, bool enabled)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_adaptive_tune_enabled_called = true;
    s_writes[zone_index].adaptive_tune_enabled = enabled;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_adaptive_tune_enabled_no_save(uint8_t zone_index, bool enabled)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_adaptive_tune_enabled_called = true;
    s_writes[zone_index].adaptive_tune_enabled = enabled;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_tuning_quality(uint8_t zone_index, const zone_tuning_quality_t *q)
{
    if (!q || zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_tuning_quality_called = true;
    s_writes[zone_index].tuning_quality = *q;
    s_writes[zone_index].tuning_seq++;
    /* Also feed test_profile_feasibility.c's storage -- that file owns the
     * one zones_config_get_tuning_quality() definition in this binary (see
     * this file's forward-declared hook above), so a test seeding a zone's
     * tuning quality before calling run_export() needs the GETTER to answer
     * with it too, not just this observability copy. */
    test_stub_zones_set_full_tuning_quality(zone_index, q);
    g_total_write_calls++;
    return true;
}
bool zones_config_reinstate_tuning_quality_no_save(uint8_t zone_index)
{
    zone_tuning_quality_t cur;
    if (zone_index >= STUB_ZONE_COUNT || !zones_config_get_tuning_quality(zone_index, &cur)) return false;
    cur.valid = true; /* no seq bump, no other field touched -- mirrors the real accessor */
    test_stub_zones_set_full_tuning_quality(zone_index, &cur);
    return true;
}
bool zones_config_set_tuning_quality_no_save(uint8_t zone_index, const zone_tuning_quality_t *q)
{
    if (!q || zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_tuning_quality_called = true;
    s_writes[zone_index].tuning_quality = *q;
    s_writes[zone_index].tuning_seq++;
    /* Also feed test_profile_feasibility.c's storage -- that file owns the
     * one zones_config_get_tuning_quality() definition in this binary (see
     * this file's forward-declared hook above), so a test seeding a zone's
     * tuning quality before calling run_export() needs the GETTER to answer
     * with it too, not just this observability copy. */
    test_stub_zones_set_full_tuning_quality(zone_index, q);
    g_total_write_calls++;
    return true;
}
bool zones_config_set_failsafe_state(uint8_t zone_index, bool on_state)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_failsafe_state_called = true;
    s_writes[zone_index].failsafe_state = on_state;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_failsafe_state_no_save(uint8_t zone_index, bool on_state)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_failsafe_state_called = true;
    s_writes[zone_index].failsafe_state = on_state;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_hyst_c(uint8_t zone_index, float hyst_c)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_hyst_c_called = true;
    s_writes[zone_index].hyst_c = hyst_c;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_hyst_c_no_save(uint8_t zone_index, float hyst_c)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_hyst_c_called = true;
    s_writes[zone_index].hyst_c = hyst_c;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_min_on_s(uint8_t zone_index, uint16_t min_on_s)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_min_on_s_called = true;
    s_writes[zone_index].min_on_s = min_on_s;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_min_on_s_no_save(uint8_t zone_index, uint16_t min_on_s)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_min_on_s_called = true;
    s_writes[zone_index].min_on_s = min_on_s;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_min_off_s(uint8_t zone_index, uint16_t min_off_s)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_min_off_s_called = true;
    s_writes[zone_index].min_off_s = min_off_s;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_min_off_s_no_save(uint8_t zone_index, uint16_t min_off_s)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_min_off_s_called = true;
    s_writes[zone_index].min_off_s = min_off_s;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_timing_profile_index(uint8_t zone_index, uint8_t index)
{
    if (zone_index >= STUB_ZONE_COUNT || index >= s_timing_profile_count) return false;
    s_writes[zone_index].set_timing_profile_index_called = true;
    s_writes[zone_index].timing_profile_index = index;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_timing_profile_index_no_save(uint8_t zone_index, uint8_t index)
{
    if (zone_index >= STUB_ZONE_COUNT || index >= s_timing_profile_count) return false;
    s_writes[zone_index].set_timing_profile_index_called = true;
    s_writes[zone_index].timing_profile_index = index;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_timing_profile_raw(uint8_t profile_index, const char *name,
                                         float progress_duty_min, float progress_window_s,
                                         float drift_hysteresis_c, float frozen_eps_c,
                                         float cross_zone_period_s, float bangbang_hysteresis_c,
                                         float cooling_limited_margin_c, float cooling_limited_hold_s,
                                         float ramp_lock_band_c)
{
    if (!name || profile_index > s_timing_profile_count || profile_index >= STUB_ZONE_COUNT) return false;
    timing_profile_write_t *tp = &s_timing_profiles[profile_index];
    strncpy(tp->name, name, sizeof(tp->name) - 1);
    tp->name[sizeof(tp->name) - 1] = '\0';
    tp->progress_duty_min = progress_duty_min;
    tp->progress_window_s = progress_window_s;
    tp->drift_hysteresis_c = drift_hysteresis_c;
    tp->frozen_eps_c = frozen_eps_c;
    tp->cross_zone_period_s = cross_zone_period_s;
    tp->bangbang_hysteresis_c = bangbang_hysteresis_c;
    tp->cooling_limited_margin_c = cooling_limited_margin_c;
    tp->cooling_limited_hold_s = cooling_limited_hold_s;
    tp->ramp_lock_band_c = ramp_lock_band_c;
    if (profile_index == s_timing_profile_count) {
        s_timing_profile_count = (uint8_t)(profile_index + 1);
    }
    s_set_timing_profile_raw_calls++;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_timing_profile_raw_no_save(uint8_t profile_index, const char *name,
                                         float progress_duty_min, float progress_window_s,
                                         float drift_hysteresis_c, float frozen_eps_c,
                                         float cross_zone_period_s, float bangbang_hysteresis_c,
                                         float cooling_limited_margin_c, float cooling_limited_hold_s,
                                         float ramp_lock_band_c)
{
    if (!name || profile_index > s_timing_profile_count || profile_index >= STUB_ZONE_COUNT) return false;
    timing_profile_write_t *tp = &s_timing_profiles[profile_index];
    strncpy(tp->name, name, sizeof(tp->name) - 1);
    tp->name[sizeof(tp->name) - 1] = '\0';
    tp->progress_duty_min = progress_duty_min;
    tp->progress_window_s = progress_window_s;
    tp->drift_hysteresis_c = drift_hysteresis_c;
    tp->frozen_eps_c = frozen_eps_c;
    tp->cross_zone_period_s = cross_zone_period_s;
    tp->bangbang_hysteresis_c = bangbang_hysteresis_c;
    tp->cooling_limited_margin_c = cooling_limited_margin_c;
    tp->cooling_limited_hold_s = cooling_limited_hold_s;
    tp->ramp_lock_band_c = ramp_lock_band_c;
    if (profile_index == s_timing_profile_count) {
        s_timing_profile_count = (uint8_t)(profile_index + 1);
    }
    s_set_timing_profile_raw_calls++;
    g_total_write_calls++;
    return true;
}
/* zone_normals_set() -- the owner's own named example (CT calibration),
 * a separate NVS store (zone_normals_cfg_t) from zone_cfg_t on real
 * firmware, but modeled here in the same s_writes[] table since this
 * stub file's only job is observing what backup_import_apply() commits,
 * not replicating the real store split. */
bool zone_normals_set(uint8_t zone_index, float amps)
{
    if (zone_index >= STUB_ZONE_COUNT) return false;
    s_writes[zone_index].set_normal_current_called = true;
    s_writes[zone_index].normal_current_a = amps;
    s_writes[zone_index].normal_current_measured = true;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_coupling(uint8_t zone_index, const float row[MAX31856_CHANNEL_COUNT])
{
    if (zone_index >= STUB_ZONE_COUNT || !row) return false;
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        s_writes[zone_index].set_coupling_cell_called[j] = true;
        s_writes[zone_index].coupling_coeff[j] = row[j];
    }
    g_total_write_calls++;
    return true;
}
/* backup_import_apply() commits per-cell now -- see zone_write_t's own
 * comment for why this stub tracks a called-flag PER CELL. */
bool zones_config_set_coupling_cell(uint8_t zone_index, uint8_t neighbor_index, float coeff, float tau_s,
                                     float dead_time_s)
{
    if (zone_index >= STUB_ZONE_COUNT || neighbor_index >= MAX31856_CHANNEL_COUNT) return false;
    s_writes[zone_index].set_coupling_cell_called[neighbor_index] = true;
    s_writes[zone_index].coupling_coeff[neighbor_index] = coeff;
    s_writes[zone_index].coupling_tau_s[neighbor_index] = tau_s;
    s_writes[zone_index].coupling_dead_time_s[neighbor_index] = dead_time_s;
    g_total_write_calls++;
    return true;
}
bool zones_config_set_coupling_cell_no_save(uint8_t zone_index, uint8_t neighbor_index, float coeff, float tau_s,
                                     float dead_time_s)
{
    if (zone_index >= STUB_ZONE_COUNT || neighbor_index >= MAX31856_CHANNEL_COUNT) return false;
    s_writes[zone_index].set_coupling_cell_called[neighbor_index] = true;
    s_writes[zone_index].coupling_coeff[neighbor_index] = coeff;
    s_writes[zone_index].coupling_tau_s[neighbor_index] = tau_s;
    s_writes[zone_index].coupling_dead_time_s[neighbor_index] = dead_time_s;
    g_total_write_calls++;
    return true;
}
/* Faithful to zones_http.c's real (checked) zones_config_set_settings_source():
 * bounds, self-reference, AND the chain-walk against the LIVE (s_writes[])
 * config via the same shared zone_settings_source_chain_has_cycle() the real
 * setter uses -- not just bounds/self-reference like the sibling _unchecked
 * stub below. This is deliberately the STRICTER of the two doors: it exists
 * so a break-proof (or a future regression) that makes backup_http.c's
 * commit loop call this checked door instead of the _unchecked one shows up
 * as a real, chain-walk-driven refusal here too, the same way it would on
 * real hardware -- not silently pass because this stub used to write
 * unconditionally. */
bool zones_config_set_settings_source(uint8_t zone_index, uint8_t group, uint8_t settings_source)
{
    if (zone_index >= STUB_ZONE_COUNT || group >= SRC_GROUP_COUNT) return false;
    if (settings_source != ZONE_SETTINGS_SOURCE_CUSTOM && settings_source >= MAX31856_CHANNEL_COUNT) return false;
    if (settings_source == zone_index) return false;
    uint8_t probe[STUB_ZONE_COUNT];
    for (uint8_t i = 0; i < STUB_ZONE_COUNT; i++) {
        probe[i] = s_writes[i].settings_source[group];
    }
    probe[zone_index] = settings_source;
    uint8_t thermo_count = zones_config_get_thermo_count();
    if (zone_settings_source_chain_has_cycle(probe, zone_index, thermo_count)) return false;
    s_writes[zone_index].set_settings_source_called[group] = true;
    s_writes[zone_index].settings_source[group] = settings_source;
    g_total_write_calls++;
    return true;
}
/* Stub for zones_http.c's real zones_config_set_settings_source_unchecked()
 * (zones_http.h) -- backup_http.c's pass-2 commit loop calls THIS, not the
 * checked setter above, precisely BECAUSE it must not fail once pass 1 (the
 * zones_config_settings_source_import_has_cycle() call earlier in this same
 * commit loop) has already accepted the full proposed set: see
 * backup_http.c's comment on this call site for the concrete
 * half-applied-restore scenario the checked setter used to hit. Mirrors the
 * real function's semantics exactly -- bounds and self-reference only, no
 * chain-walk against s_writes[] -- so a test that removes/weakens the real
 * check would show up here as well if this stub regressed to match. */
bool zones_config_set_settings_source_unchecked(uint8_t zone_index, uint8_t group, uint8_t settings_source)
{
    if (zone_index >= STUB_ZONE_COUNT || group >= SRC_GROUP_COUNT) return false;
    if (settings_source != ZONE_SETTINGS_SOURCE_CUSTOM && settings_source >= MAX31856_CHANNEL_COUNT) return false;
    if (settings_source == zone_index) return false;
    s_writes[zone_index].set_settings_source_called[group] = true;
    s_writes[zone_index].settings_source[group] = settings_source;
    g_total_write_calls++;
    return true;
}

/* Item 3 (Opus review of 5672719): no-save counterpart, matching the real
 * accessor's split -- writes the value but does NOT count as a persisted
 * save; test_backup_import_settings_source_single_save_per_import() below
 * checks the pairing (a nvs-save stand-in counter, g_settings_source_save_calls)
 * increments exactly once per import regardless of zone/group count. */
/* opus review finding (LOW-MEDIUM), restore-on-failure test: lets a test
 * force this call to fail for one specific (zone, group) pair without
 * needing an input pass 1's own checks would already reject -- backup_import.c
 * must restore every zone's pre-import settings_source[] when THIS call
 * fails mid-batch, and the only way to prove that is to make a call fail
 * that pass 1 could not have caught (all real rejection paths below are
 * exactly the ones pass 1 already re-validates). Off (0xFF, no zone matches)
 * by default. */
static uint8_t s_force_fail_settings_source_zone = 0xFFu;
static uint8_t s_force_fail_settings_source_group = 0xFFu;

bool zones_config_set_settings_source_unchecked_no_save(uint8_t zone_index, uint8_t group, uint8_t settings_source)
{
    if (zone_index >= STUB_ZONE_COUNT || group >= SRC_GROUP_COUNT) return false;
    if (settings_source != ZONE_SETTINGS_SOURCE_CUSTOM && settings_source >= MAX31856_CHANNEL_COUNT) return false;
    if (settings_source == zone_index) return false;
    if (zone_index == s_force_fail_settings_source_zone && group == s_force_fail_settings_source_group) {
        return false;
    }
    s_writes[zone_index].set_settings_source_called[group] = true;
    s_writes[zone_index].settings_source[group] = settings_source;
    return true;
}

int g_settings_source_save_calls;

bool zones_config_save_now(void)
{
    g_settings_source_save_calls++;
    g_total_write_calls++;
    return true;
}

/* Batched-save follow-up (2026-09-28): backup_import.c now snapshots the
 * WHOLE live config once before its per-zone/timing-profile commit loop
 * (zones_config_get_full_copy()) and restores it wholesale
 * (zones_config_restore_snapshot_no_save()) if any setter in that loop
 * fails partway through, instead of the narrower per-field restore this
 * file used to model. The real functions (zones_config_accessors.c) copy a
 * live zones_cfg_t struct; this stub's "live config" is s_writes[]/
 * s_timing_profiles[]/s_timing_profile_count instead, so it snapshots THOSE
 * into a private static buffer rather than *out -- backup_import.c only
 * ever holds `out`/`snapshot` opaquely (declares a local zones_cfg_t,
 * passes its address to both calls, never reads or writes through it
 * itself), so a stub that never touches *out is exactly as faithful as one
 * that does, and sidesteps a real problem: zone_write_t alone is far larger
 * per zone than zones_cfg_t's real ~896 B ceiling once STUB_ZONE_COUNT
 * copies of it are included, so memcpy-ing this stub's state into a
 * zones_cfg_t-sized buffer would overflow it. Single static slot is
 * correct because backup_import.c only ever takes one snapshot per import
 * attempt and never nests a second one inside it (single in-flight
 * http_async_job, and this whole call sequence runs on that one job's own
 * task, synchronously, start to finish). */
typedef struct {
    zone_write_t writes[STUB_ZONE_COUNT];
    timing_profile_write_t timing_profiles[STUB_ZONE_COUNT];
    uint8_t timing_profile_count;
} test_zones_snapshot_t;
static test_zones_snapshot_t s_test_snapshot; /* see reset_stub_state()'s comment on why this is not cleared there */

void zones_config_get_full_copy(zones_cfg_t *out)
{
    (void)out;
    memcpy(s_test_snapshot.writes, s_writes, sizeof(s_writes));
    memcpy(s_test_snapshot.timing_profiles, s_timing_profiles, sizeof(s_timing_profiles));
    s_test_snapshot.timing_profile_count = s_timing_profile_count;
}

void zones_config_restore_snapshot_no_save(const zones_cfg_t *snapshot)
{
    (void)snapshot;
    memcpy(s_writes, s_test_snapshot.writes, sizeof(s_writes));
    memcpy(s_timing_profiles, s_test_snapshot.timing_profiles, sizeof(s_timing_profiles));
    s_timing_profile_count = s_test_snapshot.timing_profile_count;
}

/* Spare-relay aux ownership (docs/SPARE_RELAY_ONOFF_PLAN.md WP-2): backup_import.c checks the
 * post-batch zone relay masks against the enabled aux mask. zones_config_json.c is not linked into
 * this executable, so this settable stand-in plays the provider-backed accessor. */
static uint8_t g_fake_aux_enabled_mask = 0;
uint8_t zones_config_json_aux_enabled_mask(void)
{
    return g_fake_aux_enabled_mask;
}

/* zones_config_push_relay_type() -- zones_http_internal.h declares this
 * (relay_cycles_set_type() push-out, defined for real in
 * zones_config_store.c); backup_import.c now calls it itself, once per
 * zone whose relay_type actually changed, only AFTER the single batched
 * zones_config_save_now() above succeeds (see backup_import.c's
 * relay_type_changed[] comment). Recorded per-zone plus a total count so
 * a host test can assert it fires exactly once per changed zone and never
 * before the save. */
bool s_relay_type_pushed[STUB_ZONE_COUNT];
int g_relay_type_push_calls;

void zones_config_push_relay_type(uint8_t zone_index)
{
    if (zone_index >= STUB_ZONE_COUNT) {
        return;
    }
    s_relay_type_pushed[zone_index] = true;
    g_relay_type_push_calls++;
}
/* Stub for zones_http.c's real zones_config_settings_source_import_has_cycle()
 * (zones_http.h) -- backup_http.c's pass-1 cross-entry cycle check calls
 * this. Used to be a hand-maintained re-implementation of the chain-walk
 * (found by 2026-08-31 opus review: it could drift from, or simply be wrong
 * relative to, the shipped algorithm with nothing here noticing -- both
 * backup-import cycle tests passed identically with the shipped check
 * deleted). Now calls zone_settings_source_chain_import_has_cycle() from
 * zone_settings_source_chain.h, the SAME shared code zones_http.c's real
 * implementation calls -- this stub's only remaining job is adapting this
 * file's own s_writes[]/zones_config_get_thermo_count() state into that
 * shared function's plain-array calling convention. */
bool zones_config_settings_source_import_has_cycle(uint8_t group,
                                                    const bool has_override[MAX31856_CHANNEL_COUNT],
                                                    const uint8_t override_source[MAX31856_CHANNEL_COUNT],
                                                    uint8_t *out_cycle_zone)
{
    if (group >= SRC_GROUP_COUNT) return true;
    uint8_t chain[STUB_ZONE_COUNT];
    for (uint8_t i = 0; i < STUB_ZONE_COUNT; i++) {
        chain[i] = s_writes[i].settings_source[group];
    }
    uint8_t thermo_count = zones_config_get_thermo_count();
    return zone_settings_source_chain_import_has_cycle(chain, has_override, override_source, thermo_count,
                                                        out_cycle_zone);
}

bool zones_config_set_safety_tc_type(uint8_t tc_type)
{
    s_safety_tc_set = true;
    s_safety_tc_type = tc_type;
    g_total_write_calls++;
    return true;
}

// ---- profiles_http.h stubs -------------------------------------------------

// Export-side control: backup_export_get_handler() reads every profile slot
// through this getter. Every earlier test in this file only ever drives
// backup_import_apply(), which never calls it -- "false for every id" was
// fine for them. The export tests below need at least one real slot to
// answer, so this is now driven by the small settable table declared above
// (s_profile_present/s_profile_slots) rather than a flat refusal;
// reset_stub_state() clears it back to "nothing saved" so import-only tests
// are unaffected.
static void test_stub_profiles_set(uint8_t id, const profile_t *p)
{
    if (id >= PROFILES_MAX_COUNT) {
        return;
    }
    s_profile_present[id] = true;
    s_profile_slots[id] = *p;
}

bool profiles_http_get(uint8_t id, profile_t *out)
{
    if (id >= PROFILES_MAX_COUNT || !s_profile_present[id]) {
        return false;
    }
    if (out) *out = s_profile_slots[id];
    return true;
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
    bool ok = test_backup_import_apply("{", err, sizeof(err));

    TEST_CHECK(!ok, "a truncated/malformed body must be refused, not crash or silently accept");
    TEST_CHECK(g_total_write_calls == 0, "no setter may run when the body cannot even be parsed for \"kind\"");
    TEST_CHECK(g_profile_save_calls == 0, "no profile may be saved either");
}

// Review fix, 2026-09-25: the mode-gate check used to live INSIDE
// backup_import_apply() (a sentinel-prefixed err_msg,
// "MODE_GATE_REFUSED:", that backup_import_post_handler() stripped before
// sending a 409). It has since moved to the top of
// backup_import_post_handler() itself, ahead of ota_http_check_interlocks(),
// so the dead-code ordering bug found in review (mode gate answering AFTER
// the OTA interlock, never reached) is fixed the same way as
// zones_http_post.c's. backup_import_apply() no longer contains any
// mode-gate logic at all -- the old test here (asserting the sentinel
// prefix) tested code that no longer exists and has been removed.
//
// Task 1c (docs/SYSTEM_MODE_GATE_PLAN.md known test gaps): the handler-level
// harness this comment used to say was out of scope now exists, following
// test_zones_http.c's run_zones_post() pattern exactly -- a staged
// httpd_req_t driving the real backup_import_post_handler() (#included above)
// directly, with content_len=0 for every test below since all three prove a
// refusal that happens before the handler ever reads the body.
static esp_err_t run_backup_import_post(void)
{
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.content_len = 0;
    return backup_import_post_handler(&req);
}

static void reset_backup_import_post_stubs(void)
{
    s_test_profile_running_for_mode_gate = false;
    s_test_autotune_running_for_mode_gate = false;
    g_stub_ota_interlock_result = OTA_INTERLOCK_OK;
    g_stub_ota_interlock_reason[0] = '\0';
    g_stub_ota_interlock_call_count = 0;
    g_stub_ota_last_refusal_reason[0] = '\0';
    s_post_last_status[0] = '\0';
    s_post_last_body[0] = '\0';
}

// Proves the ordering fix itself, not just its presence in the source: the
// mode gate must refuse and return BEFORE ota_http_check_interlocks() is ever
// called, same convention as
// test_zones_post_refused_by_mode_gate_before_interlock() in test_zones_http.c.
static void test_backup_import_post_refused_by_mode_gate_before_interlock(void)
{
    TEST_SECTION("backup_import_post_handler -- system_mode_gate refuses BEFORE "
                 "ota_http_check_interlocks() is even called, while profile_executor "
                 "reports RUNNING");
    reset_backup_import_post_stubs();
    s_test_profile_running_for_mode_gate = true;

    esp_err_t err = run_backup_import_post();

    TEST_CHECK(err == ESP_OK, "backup_import_post_handler must always return ESP_OK");
    TEST_CHECK(g_stub_ota_interlock_call_count == 0,
              "ota_http_check_interlocks() must never be reached once system_mode_gate has already refused");
    TEST_CHECK(strncmp(s_post_last_status, "409", 3) == 0,
              "the refusal must be the mode gate's own 409, not some other early exit");
    // Every later test in this combined executable shares these stubs
    // (kiln_cfg_store_delete()/save_current() consult the same
    // ota_http_check_interlocks() stub), so leave them at their defaults.
    reset_backup_import_post_stubs();
}

// Proves the interlock IS reached once the mode gate passes (so the fix
// didn't just move the dead-code problem one level down).
static void test_backup_import_post_refused_by_interlock_after_mode_gate_passes(void)
{
    TEST_SECTION("backup_import_post_handler -- ota_http_check_interlocks() is reached and can "
                 "still refuse once the mode gate itself passes");
    reset_backup_import_post_stubs();
    g_stub_ota_interlock_result = OTA_INTERLOCK_REFUSED;

    esp_err_t err = run_backup_import_post();

    TEST_CHECK(err == ESP_OK, "backup_import_post_handler must always return ESP_OK");
    TEST_CHECK(g_stub_ota_interlock_call_count == 1, "the interlock must be reached once the gate passes");
    // Must not leak OTA_INTERLOCK_REFUSED into later suites: kiln_cfg_store's
    // delete/save paths call this same stub and would refuse every later
    // slot operation (43 setup failures in the kiln_configs tests below).
    reset_backup_import_post_stubs();
}

// Backup/sweep review follow-up 1 (2026-09-28): backup_import.c never wrote
// a zone-current-sweep-specific check of its own -- it reuses
// ota_http_check_interlocks(), which already refuses unconditionally
// (ota_http.c's own "B2, opus review 2026-08-27" check, ahead of every other
// precondition in that function) whenever zones_current_sweep_is_active()
// is true, with the reason "a zone current sweep is running". Since this
// file's own ota_http_check_interlocks() stub is a total pass-through (any
// result/reason it's told to report, it reports), this test documents that
// wiring rather than re-deriving ota_http.c's own sweep logic (already
// covered end-to-end, with the real function, by test_ota_http.c's
// s_test_sweep_active tests): the exact reason a live sweep produces flows
// through backup_import_post_handler() unmodified and un-swallowed, at the
// same call site test_backup_import_post_refused_by_interlock_after_mode_
// gate_passes() above already proves is reached once the mode gate passes.
static void test_backup_import_post_refused_by_sweep_reason_via_interlock(void)
{
    TEST_SECTION("backup_import_post_handler -- a zone current sweep's own interlock reason "
                 "passes through unmodified");
    reset_backup_import_post_stubs();
    g_stub_ota_interlock_result = OTA_INTERLOCK_REFUSED;
    snprintf(g_stub_ota_interlock_reason, sizeof(g_stub_ota_interlock_reason), "a zone current sweep is running");

    esp_err_t err = run_backup_import_post();

    TEST_CHECK(err == ESP_OK, "backup_import_post_handler must always return ESP_OK");
    TEST_CHECK(g_stub_ota_interlock_call_count == 1, "the interlock (which owns the sweep check) is reached");
    TEST_CHECK(strcmp(g_stub_ota_last_refusal_reason, "a zone current sweep is running") == 0,
              "the sweep's own reason reaches the refusal sender unmodified");
    reset_backup_import_post_stubs();
}

// A4 review (2026-09-28): backup_import_job() runs on the async task after
// httpd_worker's own mode-gate/interlock checks, so a firing started in
// between must still be refused before anything is written. Drives the
// static job body directly (this file #includes backup_import.c) with a real
// body fed through the httpd_req_recv() stub.
static const char *k_job_body =
    "{\"kind\":\"kilnctl_backup\",\"version\":4,\"profiles\":[],"
    "\"zones\":[{\"index\":0,\"pid_kp\":2,\"pid_ki\":0,\"pid_kd\":0}]}";

static void run_backup_import_job_with_body(const char *body)
{
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    backup_import_job_ctx_t *ctx = malloc(sizeof(*ctx));
    TEST_CHECK(ctx != NULL, "ctx allocation");
    if (!ctx) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->mode = KILN_CFG_RESTORE_MERGE;
    ctx->ack_delete_count = -1;
    ctx->content_len = strlen(body);
    s_recv_feed = body;
    s_recv_feed_off = 0;
    backup_import_job(&req, ctx); /* frees ctx */
    s_recv_feed = NULL;
    s_recv_feed_off = 0;
}

static void test_backup_import_job_rechecks_mode_gate_before_writing(void)
{
    TEST_SECTION("backup_import_job -- a firing that started after the handler's checks is "
                 "refused by the job's own re-check, before any write");
    reset_stub_state();
    reset_backup_import_post_stubs();

    /* Positive control: with nothing running the same body DOES write, so
     * the refusal below is not vacuous. */
    g_total_write_calls = 0;
    run_backup_import_job_with_body(k_job_body);
    TEST_CHECK(g_total_write_calls > 0, "control: the job reaches backup_import_apply() and writes");

    reset_stub_state();
    reset_backup_import_post_stubs();
    s_test_profile_running_for_mode_gate = true;
    g_total_write_calls = 0;
    run_backup_import_job_with_body(k_job_body);
    TEST_CHECK(strncmp(s_post_last_status, "409", 3) == 0, "the job's re-check sends the mode gate's 409");
    TEST_CHECK(g_total_write_calls == 0, "nothing written once the re-check refuses");
    TEST_CHECK(g_stub_ota_interlock_call_count == 0, "mode gate refuses before the interlock, same order");

    reset_backup_import_post_stubs();
    g_stub_ota_interlock_result = OTA_INTERLOCK_REFUSED;
    g_total_write_calls = 0;
    run_backup_import_job_with_body(k_job_body);
    TEST_CHECK(g_stub_ota_interlock_call_count == 1, "the job re-runs the interlock");
    TEST_CHECK(g_total_write_calls == 0, "nothing written once the interlock re-check refuses");

    reset_backup_import_post_stubs();
    reset_stub_state();
}

// A4 review follow-up A (2026-09-28): backup_import_restore_in_flight() must
// read false before/after the job and (observably) true while it runs, so
// profile_executor_run()/autotune_begin_run_locked() can refuse a start that
// lands during the commit pass. The relay_authority_heat_run_active() stub
// samples it at the job-side re-check (it must already be set there), and
// this checks it is false before and false again after the job returns
// (every exit path clears it).
static void test_backup_import_job_clears_restore_in_flight_flag(void)
{
    TEST_SECTION("backup_import_job -- restore_in_flight flag is false before and after the job runs");
    reset_stub_state();
    reset_backup_import_post_stubs();

    TEST_CHECK(!backup_import_restore_in_flight(), "flag starts clear");
    s_test_restore_flag_seen_set_at_heat_read = false;
    run_backup_import_job_with_body(k_job_body);
    TEST_CHECK(s_test_restore_flag_seen_set_at_heat_read,
               "flag is already set when the job's re-check reads the heat claim");
    TEST_CHECK(!backup_import_restore_in_flight(), "flag is cleared once the job returns (success path)");

    /* Same check on a refused-by-recheck exit path -- an early return out of
     * backup_import_job_inner() must still clear it. */
    s_test_profile_running_for_mode_gate = true;
    run_backup_import_job_with_body(k_job_body);
    TEST_CHECK(!backup_import_restore_in_flight(), "flag is cleared even when the job's re-check refuses");
    s_test_profile_running_for_mode_gate = false;

    reset_backup_import_post_stubs();
    reset_stub_state();
}

// Same reused wiring, at the job's TOCTOU re-check point: a sweep that
// starts after the handler's own checks but before the job's re-check must
// still be caught, same as the firing/autotune case
// test_backup_import_job_rechecks_mode_gate_before_writing() already proves.
// Ordering note: backup_import_job() sets s_backup_restore_in_flight BEFORE
// this re-check reads zones_current_sweep_is_active(). That is only half of
// the race closure: zones_current_sweep_start() must likewise publish
// s_sweep.active BEFORE its own post-claim re-read of the flag -- proven by
// test_zones_current_sweep_start_restore_in_flight_refused_during_refetch()
// in test_zones_http.c.
static void test_backup_import_job_recheck_refused_by_sweep_reason_via_interlock(void)
{
    TEST_SECTION("backup_import_job -- a sweep that started after the handler's checks is refused "
                 "by the job's own re-check, before any write");
    reset_stub_state();
    reset_backup_import_post_stubs();
    g_stub_ota_interlock_result = OTA_INTERLOCK_REFUSED;
    snprintf(g_stub_ota_interlock_reason, sizeof(g_stub_ota_interlock_reason), "a zone current sweep is running");
    g_total_write_calls = 0;

    run_backup_import_job_with_body(k_job_body);

    TEST_CHECK(g_stub_ota_interlock_call_count == 1, "the job re-runs the interlock, which owns the sweep check");
    TEST_CHECK(g_total_write_calls == 0, "nothing written once the sweep-carrying interlock re-check refuses");
    TEST_CHECK(strcmp(g_stub_ota_last_refusal_reason, "a zone current sweep is running") == 0,
              "the job's re-check sends the sweep's own reason unmodified");

    reset_backup_import_post_stubs();
    reset_stub_state();
}

// The third leg of this order (http_async_job_busy(), checked after the
// interlock passes) is deliberately NOT driven end-to-end here -- see the
// comment above ota_http_req_ack_no_safety()/http_async_job_busy's home in
// this file for why forcing the one real, shared instance busy would leak
// into dozens of unrelated suites that run later in this same combined
// executable. Covered by direct code inspection instead.

static void test_wrong_kind_refused(void)
{
    TEST_SECTION("backup_import_apply -- wrong \"kind\" is refused");
    reset_stub_state();

    const char *body = "{\"kind\":\"something_else\",\"version\":2,\"profiles\":[],\"zones\":[]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(!ok, "a body whose \"kind\" isn't kilnctl_backup must be refused");
    TEST_CHECK(strstr(err, "kilnCtl backup") != NULL, "error names the actual problem (wrong kind)");
    TEST_CHECK(g_total_write_calls == 0, "nothing written for a wrong-kind body");
}

static void test_unknown_version_refused(void)
{
    TEST_SECTION("backup_import_apply -- unknown (too new) version is refused");
    reset_stub_state();

    /* 2026-08-30 (ZONES_CFG_VERSION 10->11): BACKUP_FORMAT_VERSION moved
     * 3 -> 4 (coupling_coeff/coupling_neighbor_zone -> indexed
     * coupling_c0..coupling_cN-1), so 4 is now a real, supported version --
     * this test moved to version 5, the new too-new boundary. Task 9
     * (bkfinish_assessment.md): BACKUP_FORMAT_VERSION moved 4 -> 5 (top-level
     * kiln_configs[] array, safety_tc_type key), so 5 is now real/supported
     * too -- this test moves again, to version 6, the new too-new boundary. */
    const char *body = "{\"kind\":\"kilnctl_backup\",\"version\":6,\"profiles\":[],\"zones\":[]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(!ok, "version 6 is newer than this firmware's BACKUP_FORMAT_VERSION (5) -- must be refused");
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
    bool ok = test_backup_import_apply(body, err, sizeof(err));

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
    bool ok = test_backup_import_apply(body, err, sizeof(err));

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
    bool ok = test_backup_import_apply(body, err, sizeof(err));

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
    bool ok = test_backup_import_apply(body, err, sizeof(err));

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
    bool ok = test_backup_import_apply(body, err, sizeof(err));

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
    bool ok = test_backup_import_apply(body, err, sizeof(err));

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
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(strlen("ExactlyFifteenC") == PROFILE_NAME_MAX_LEN, "test setup sanity: name is exactly at the limit");
    TEST_CHECK(ok, "a name exactly at the limit must be accepted, not rejected as \"too long\"");
    TEST_CHECK(g_profile_save_calls == 1, "the profile was committed");
    TEST_CHECK(strcmp(g_last_saved_profile.name, "ExactlyFifteenC") == 0, "the full, untruncated name was written");
}

// PROFILE_SLOTS_100_PLAN.md section 7 task 7: a backup carrying exactly
// PROFILES_MAX_COUNT profiles must import in full, and one carrying
// PROFILES_MAX_COUNT+1 must be refused BY NAME (the explicit
// "backup has more than %u profiles" count check a few lines above, not an
// overflow of the heap-allocated `candidates[]` array -- see
// backup_import_apply()'s comment for why that array is heap, not stack,
// allocated in the first place). Both counts are derived from the constant
// so this test passes unchanged whether PROFILES_MAX_COUNT is today's 8 or
// the 100 a sibling, concurrently-running worktree is raising it to.
//
// Builds a body with `count` profile entries, ids 0..count-1, each a
// minimal-but-valid profile (one segment, zone_mask 1 -- valid for the
// thermo_count=3 default reset_stub_state() below leaves in place).
static char *build_many_profiles_body(unsigned count)
{
    // "{\"id\":N,\"name\":\"pN\",\"zone_mask\":1,\"segments\":[{\"target_c\":100,"
    //  "\"ramp_c_per_hr\":50,\"dwell_min\":30}]}," is comfortably under 128 B
    // even at 3-digit ids; +256 for the envelope.
    size_t cap = (size_t)count * 128 + 256;
    char *body = malloc(cap);
    TEST_CHECK(body != NULL, "test setup: malloc for many-profiles body must succeed");
    size_t off = 0;
    off += (size_t)snprintf(body + off, cap - off,
                            "{\"kind\":\"kilnctl_backup\",\"version\":2,\"profiles\":[");
    for (unsigned i = 0; i < count; i++) {
        off += (size_t)snprintf(body + off, cap - off,
                                "%s{\"id\":%u,\"name\":\"p%u\",\"zone_mask\":1,"
                                "\"segments\":[{\"target_c\":100,\"ramp_c_per_hr\":50,\"dwell_min\":30}]}",
                                i == 0 ? "" : ",", i, i);
    }
    snprintf(body + off, cap - off, "],\"zones\":[]}");
    return body;
}

static void test_import_exactly_max_count_profiles_succeeds(void)
{
    TEST_SECTION("backup_import_apply -- exactly PROFILES_MAX_COUNT profiles all import");
    reset_stub_state();

    char *body = build_many_profiles_body(PROFILES_MAX_COUNT);
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));
    free(body);

    TEST_CHECK(ok, "a backup with exactly PROFILES_MAX_COUNT profiles must be accepted");
    TEST_CHECK(g_profile_save_calls == PROFILES_MAX_COUNT,
              "every one of the PROFILES_MAX_COUNT profiles must be saved, none dropped");
}

static void test_import_over_max_count_profiles_refused(void)
{
    TEST_SECTION("backup_import_apply -- PROFILES_MAX_COUNT+1 profiles refused by name, nothing written");
    reset_stub_state();

    char *body = build_many_profiles_body(PROFILES_MAX_COUNT + 1);
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));
    free(body);

    TEST_CHECK(!ok, "a backup with PROFILES_MAX_COUNT+1 profiles must be refused");
    TEST_CHECK(strstr(err, "more than") != NULL,
              "refused by the explicit count check (\"backup has more than N profiles\"), "
              "not silently truncated or an overflow");
    TEST_CHECK(g_profile_save_calls == 0, "nothing written -- refusal happens in validation, before any commit");
    TEST_CHECK(g_total_write_calls == 0, "no other setter ran either");
}

// LIVE_EDIT_WORKING_SLOT_ID is defined as PROFILES_MAX_COUNT itself
// (live_profile.h) -- the reserved slot sits exactly one past the last real
// user id. An import naming that id, or any id further out, must be
// rejected by the same "id out of range (0-PROFILES_MAX_COUNT-1)" check a
// plain out-of-range id gets; it must never reach profiles_http_save().
static void test_import_live_edit_slot_id_rejected(void)
{
    TEST_SECTION("backup_import_apply -- an id naming the live-edit slot is rejected, not written");
    reset_stub_state();

    char body[256];
    snprintf(body, sizeof(body),
            "{\"kind\":\"kilnctl_backup\",\"version\":2,"
            "\"profiles\":[{\"id\":%u,\"name\":\"live\",\"zone_mask\":1,"
            "\"segments\":[{\"target_c\":100,\"ramp_c_per_hr\":50,\"dwell_min\":30}]}],"
            "\"zones\":[]}",
            (unsigned)LIVE_EDIT_WORKING_SLOT_ID);
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(!ok, "id == LIVE_EDIT_WORKING_SLOT_ID (== PROFILES_MAX_COUNT) must be rejected");
    TEST_CHECK(strstr(err, "id out of range") != NULL, "rejected by the id-range check, naming the problem");
    TEST_CHECK(g_profile_save_calls == 0, "the live-edit slot is never written by a backup import");
}

static void test_import_id_past_max_count_rejected(void)
{
    TEST_SECTION("backup_import_apply -- an id well past PROFILES_MAX_COUNT is rejected, not written");
    reset_stub_state();

    char body[256];
    snprintf(body, sizeof(body),
            "{\"kind\":\"kilnctl_backup\",\"version\":2,"
            "\"profiles\":[{\"id\":%u,\"name\":\"far\",\"zone_mask\":1,"
            "\"segments\":[{\"target_c\":100,\"ramp_c_per_hr\":50,\"dwell_min\":30}]}],"
            "\"zones\":[]}",
            (unsigned)PROFILES_MAX_COUNT + 5);
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(!ok, "an id well past PROFILES_MAX_COUNT must be rejected");
    TEST_CHECK(strstr(err, "id out of range") != NULL, "rejected by the id-range check, naming the problem");
    TEST_CHECK(g_profile_save_calls == 0, "nothing written for an out-of-range id");
}

// Opus review of 5dd23944 finding B: backup_import_apply()'s pass 2 used to
// commit profiles one at a time, so a name collision discovered mid-batch
// (profiles_http_save() now runs live_edit_name_collides() too, same as the
// interactive POST /api/profile path) left entries 0..i-1 already written --
// a board holding two same-named slots, exported then re-imported, aborted
// half-applied instead of either fully applying or fully refusing. The three
// tests below drive the new pass-1 pre-check (backup_import.c, the
// "Pass 1a-dup" block) that refuses the WHOLE import up front instead.

static void test_import_intra_batch_duplicate_name_refused_nothing_written(void)
{
    TEST_SECTION("backup_import_apply -- two candidates with the same name in one import refused, nothing written");
    reset_stub_state();

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":2,"
        "\"profiles\":["
        "{\"name\":\"Bisque\",\"zone_mask\":1,\"segments\":[{\"target_c\":100,\"ramp_c_per_hr\":50,\"dwell_min\":30}]},"
        "{\"name\":\"bisque\",\"zone_mask\":1,\"segments\":[{\"target_c\":200,\"ramp_c_per_hr\":50,\"dwell_min\":30}]}"
        "],\"zones\":[]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(!ok, "two candidates normalizing to the same name (case-insensitive) must refuse the whole import");
    TEST_CHECK(strstr(err, "duplicate name within this import") != NULL,
              "error names the actual problem (intra-batch duplicate), not a generic failure");
    TEST_CHECK(g_profile_save_calls == 0, "neither candidate may be written -- refusal happens in pass 1, before pass 2 commits anything");
    TEST_CHECK(g_total_write_calls == 0, "no other setter ran either");
}

static void test_import_name_collides_with_existing_non_overwritten_slot_refused(void)
{
    TEST_SECTION("backup_import_apply -- candidate name collides with an existing slot this import will NOT overwrite -- refused");
    reset_stub_state();

    // Slot 3 already holds "Bisque" on the board. The import brings in a
    // new, unnumbered ("first free slot") candidate also named "Bisque" --
    // it will land in whichever slot is first free (not 3), so this is a
    // genuine collision with a slot the import leaves untouched, exactly
    // profiles_http_save()'s own interactive-path rule.
    profile_t existing;
    memset(&existing, 0, sizeof(existing));
    strncpy(existing.name, "Bisque", sizeof(existing.name) - 1);
    test_stub_profiles_set(3, &existing);

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":2,"
        "\"profiles\":["
        "{\"name\":\"Bisque\",\"zone_mask\":1,\"segments\":[{\"target_c\":100,\"ramp_c_per_hr\":50,\"dwell_min\":30}]}"
        "],\"zones\":[]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(!ok, "a candidate colliding with an existing, non-overwritten board slot must refuse the whole import");
    TEST_CHECK(g_profile_save_calls == 0, "nothing written -- refused in pass 1, before pass 2 commits anything");
}

static void test_import_name_overwriting_the_slot_holding_that_name_allowed(void)
{
    TEST_SECTION("backup_import_apply -- candidate overwrites the very slot already holding that name -- allowed");
    reset_stub_state();

    // Slot 3 already holds "Bisque". This import explicitly targets id 3
    // (has_id) with the SAME name -- exactly profiles_http_save()'s own
    // exclude_id rule: a slot is never a collision with its own current
    // contents.
    profile_t existing;
    memset(&existing, 0, sizeof(existing));
    strncpy(existing.name, "Bisque", sizeof(existing.name) - 1);
    test_stub_profiles_set(3, &existing);

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":2,"
        "\"profiles\":["
        "{\"id\":3,\"name\":\"Bisque\",\"zone_mask\":1,\"segments\":[{\"target_c\":100,\"ramp_c_per_hr\":50,\"dwell_min\":30}]}"
        "],\"zones\":[]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(ok, "overwriting the same slot that already holds this exact name must be allowed, not refused");
    TEST_CHECK(g_profile_save_calls == 1, "the one candidate must actually be written");
}

// Opus review of 5dd23944, finding 2: the pass-1 write_ids[] simulation used
// to mark EVERY has_id candidate's slot used FIRST, then allocate first-free
// slots for the remaining candidates in a SECOND pass -- but pass 2's real
// commit loop (backup_import_apply()'s "Pass 2: everything validated --
// commit") processes candidates in plain candidate ORDER, has_id and !has_id
// interleaved. That two-pass shape lets an unnumbered candidate appearing
// BEFORE a has_id candidate simulate landing on a DIFFERENT first-free slot
// than the one pass 2 will actually give it (pass 2's own first-free
// allocation, inside profiles_http_save(), only ever sees the has_id
// reservations of candidates already committed so far -- exactly what the
// single-loop simulation now matches, and the old two-pass one did not).
// This is the reviewer's own worked example: board slot 0 used, slot 1
// free, slot 3 holds "A". candidates[0] (no id, "A") would land on slot 1
// under the correct (and now real) simulation -- colliding with slot 3's
// existing "A" -- so this refuses in pass 1 with zero writes.
static void test_import_write_ids_sim_matches_pass2_commit_order(void)
{
    TEST_SECTION("backup_import_apply -- pass-1 write_ids[] simulation must match pass 2's exact commit "
                 "order (Opus review of 5dd23944, finding 2) -- reviewer's example refuses in pass 1, "
                 "zero writes");
    reset_stub_state();

    profile_t slot0;
    memset(&slot0, 0, sizeof(slot0));
    strncpy(slot0.name, "Other", sizeof(slot0.name) - 1);
    test_stub_profiles_set(0, &slot0); // "slot 0 used"
    // slot 1 left unset -- "slot 1 free"

    profile_t slot3;
    memset(&slot3, 0, sizeof(slot3));
    strncpy(slot3.name, "A", sizeof(slot3.name) - 1);
    test_stub_profiles_set(3, &slot3); // "slot 3 holds \"A\""

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":2,"
        "\"profiles\":["
        "{\"name\":\"A\",\"zone_mask\":1,\"segments\":[{\"target_c\":100,\"ramp_c_per_hr\":50,\"dwell_min\":30}]},"
        "{\"id\":1,\"name\":\"Q\",\"zone_mask\":1,\"segments\":[{\"target_c\":100,\"ramp_c_per_hr\":50,\"dwell_min\":30}]}"
        "],\"zones\":[]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(!ok, "the reviewer's example (no-id \"A\" ahead of id:1 \"Q\", with slot 3 already \"A\") "
                    "must be refused in pass 1");
    TEST_CHECK(g_profile_save_calls == 0, "nothing may be written -- refused before pass 2 commits anything");
}

// Opus review of 5dd23944, finding 1/BLOCKER: importing a backup containing
// a profile named after a builtin's code must succeed -- before this fix,
// backup_import.c's pass-1 dup-name check scanned the builtin catalogue
// unconditionally (via live_edit_name_collides()), so a backup taken from a
// board that had legitimately "Copy builtin"-ed a schedule (or was hand-
// edited/restored) refused the WHOLE restore, not just that one entry.
static void test_import_allows_profile_named_like_a_builtin(void)
{
    TEST_SECTION("backup_import_apply -- a profile named after a builtin's code imports successfully "
                 "(Opus review of 5dd23944, finding 1 -- this used to refuse the whole restore)");
    reset_stub_state();
    g_fake_builtin_on = true; // g_fake_builtin_entry's code is "C6DHSC" -- see the stub above

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":2,"
        "\"profiles\":["
        "{\"name\":\"c6dhsc\",\"zone_mask\":1,\"segments\":[{\"target_c\":100,\"ramp_c_per_hr\":50,\"dwell_min\":30}]}"
        "],\"zones\":[]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(ok, "a candidate whose name matches a builtin's code must import, not be refused");
    TEST_CHECK(g_profile_save_calls == 1, "the one candidate must actually be written");

    g_fake_builtin_on = false;
}

// RELEASE_HARDENING_PLAN.md item 7, "import of a deliberately hostile
// config": the individual rejection tests above (malformed body, wrong
// kind, too-new version, out-of-range model, overlong zone/profile name)
// each prove ONE shape is refused, but the actual release gate --
// "no such input can produce a bootable state that will command heat" --
// was never stated as its own assertion; it had to be inferred by reading
// all of them together (see docs/audits/
// release_hardening_plan_verify_1_2_5_7_8_2026-09-16.md, blocker 7). This
// test states that property directly, in one place, over every hostile
// shape this file already knows how to construct: for each, backup_import_
// apply() must both refuse (ok == false) AND leave the zero-write invariant
// intact -- g_total_write_calls and g_profile_save_calls (the two sinks
// that could otherwise persist a value a later boot would read back and
// command heat from) both stay at zero. A body that got this wrong
// (accepted, or partially committed before failing) would slip
// past the per-shape tests above only if a future edit added a new hostile
// shape without a matching rejection test for it -- this test is a single
// choke point that would still catch a REGRESSION in any of the shapes
// already listed here, which is exactly the gap the audit named.
static void test_no_hostile_backup_input_produces_a_bootable_heat_commanding_state(void)
{
    TEST_SECTION("backup_import_apply -- release gate: no hostile input shape writes anything at all");

    static const char *const kHostileBodies[] = {
        // truncated / malformed JSON
        "{",
        // wrong "kind"
        "{\"kind\":\"something_else\",\"version\":2,\"profiles\":[],\"zones\":[]}",
        // version newer than this firmware's BACKUP_FORMAT_VERSION (5)
        "{\"kind\":\"kilnctl_backup\",\"version\":6,\"profiles\":[],\"zones\":[]}",
        // a version number "from the future", far past anything ever issued
        "{\"kind\":\"kilnctl_backup\",\"version\":9999,\"profiles\":[],\"zones\":[]}",
        // valid JSON, in-range "kind"/"version", but a value outside the
        // validated model range for one zone
        "{\"kind\":\"kilnctl_backup\",\"version\":2,\"profiles\":[],"
        "\"zones\":[{\"index\":0,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,"
        "\"model_k_dc\":999999,\"model_tau_s\":100,\"model_dead_time_s\":30}]}",
        // an overlong zone name, otherwise well-formed
        "{\"kind\":\"kilnctl_backup\",\"version\":2,\"profiles\":[],"
        "\"zones\":[{\"index\":0,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,"
        "\"name\":\"ThisNameIsWayTooLongForOneZone\"}]}",
        // an overlong profile name, otherwise well-formed
        "{\"kind\":\"kilnctl_backup\",\"version\":2,"
        "\"profiles\":[{\"id\":0,\"name\":\"ThisNameIsWayTooLongForOneProfile\",\"zone_mask\":1,"
        "\"segments\":[{\"target_c\":100,\"ramp_c_per_hr\":50,\"dwell_min\":30}]}],"
        "\"zones\":[]}",
        // a self-referencing settings_source -- structurally valid JSON,
        // individually in-range values, only wrong as a whole
        "{\"kind\":\"kilnctl_backup\",\"version\":3,\"profiles\":[],"
        "\"zones\":[{\"index\":1,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,"
        "\"settings_source\":1}]}",
    };

    for (size_t i = 0; i < sizeof(kHostileBodies) / sizeof(kHostileBodies[0]); i++) {
        reset_stub_state();
        char err[160];
        bool ok = test_backup_import_apply(kHostileBodies[i], err, sizeof(err));

        char msg[256];
        snprintf(msg, sizeof(msg), "hostile body #%zu must be refused, not accepted", i);
        TEST_CHECK(!ok, msg);

        snprintf(msg, sizeof(msg),
                 "hostile body #%zu: no zone setter may have run -- a partial commit before "
                 "the refusal is exactly the shape that could leave a bootable-and-heat-"
                 "commanding config behind", i);
        TEST_CHECK(g_total_write_calls == 0, msg);

        snprintf(msg, sizeof(msg), "hostile body #%zu: no profile may have been saved", i);
        TEST_CHECK(g_profile_save_calls == 0, msg);
    }
}

// ---------------------------------------------------------------------------
// Version 3 (2026-08-30): PID_EXPANSION_PLAN.md Phase 2/4's four new fields
// -- fuzzy_strength_pct, coupling_coeff, coupling_neighbor_zone,
// settings_source.
//
// Version 4 (2026-08-30, same-day follow-up, ZONES_CFG_VERSION 10->11): the
// coupling_coeff/coupling_neighbor_zone pair is replaced by indexed
// coupling_c0..coupling_cN-1 keys -- see BACKUP_FORMAT_VERSION's own 3->4
// comment in backup_http.c.
// ---------------------------------------------------------------------------

static void test_v4_new_fields_round_trip_distinct_values(void)
{
    TEST_SECTION("backup_import_apply -- fuzzy_strength_pct/coupling_c0../settings_source all set to "
                 "distinctive non-default values, committed exactly, per cell");
    reset_stub_state();

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":4,\"profiles\":[],"
        "\"zones\":[{\"index\":1,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,"
        "\"fuzzy_strength_pct\":37.25,\"coupling_c0\":10.887,\"coupling_c2\":3.332,"
        "\"settings_source\":0}]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(ok, "a well-formed version-4 zone entry must import");
    TEST_CHECK(s_writes[1].set_fuzzy_strength_called, "fuzzy_strength_pct was committed");
    TEST_CHECK_NEAR(s_writes[1].fuzzy_strength_pct, 37.25, 1e-6, "fuzzy_strength_pct comes back exactly");
    TEST_CHECK(s_writes[1].set_coupling_cell_called[0], "cell 0 was committed");
    TEST_CHECK(s_writes[1].set_coupling_cell_called[2], "cell 2 was committed");
    TEST_CHECK(!s_writes[1].set_coupling_cell_called[1], "the diagonal cell (1, zone 1's own index) was NOT touched");
    TEST_CHECK_NEAR(s_writes[1].coupling_coeff[0], 10.887, 1e-6,
                    "coupling_coeff[0] comes back exactly -- the bench-measured c(1->0)");
    TEST_CHECK_NEAR(s_writes[1].coupling_coeff[2], 3.332, 1e-6,
                    "coupling_coeff[2] comes back exactly, DISTINCT from coupling_coeff[0] -- the "
                    "whole point of the 10->11 widening, round-tripped through a real import");
    TEST_CHECK(s_writes[1].set_settings_source_called[SRC_GROUP_LIMITS], "settings_source was committed");
    TEST_CHECK(s_writes[1].settings_source[SRC_GROUP_LIMITS] == 0, "settings_source comes back exactly (zone 1 copies zone 0)");
}

// ZONES_CFG_VERSION 14->15 (PID_EXPANSION_PLAN.md 3.2 follow-up): the
// coupling identification's own diagonal cell, coupling_diag_k_dc -- ordinary
// optional-field convention, no BACKUP_FORMAT_VERSION bump (see backup_http.c's
// own comment on the matching parse/commit).
static void test_coupling_diag_k_dc_round_trips_distinct_value(void)
{
    TEST_SECTION("backup_import_apply -- coupling_diag_k_dc committed exactly, distinct from "
                 "fuzzy_strength_pct/coupling_c0 in the same entry");
    reset_stub_state();

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":4,\"profiles\":[],"
        "\"zones\":[{\"index\":1,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,"
        "\"fuzzy_strength_pct\":37.25,\"coupling_c0\":10.887,"
        "\"coupling_diag_k_dc\":21.6,\"settings_source\":0}]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(ok, "a well-formed coupling_diag_k_dc entry must import");
    TEST_CHECK(s_writes[1].set_coupling_diag_k_dc_called, "coupling_diag_k_dc was committed");
    TEST_CHECK_NEAR(s_writes[1].coupling_diag_k_dc, 21.6, 1e-6,
                    "coupling_diag_k_dc comes back exactly, DISTINCT from fuzzy_strength_pct/coupling_c0 "
                    "in the same entry");
}

static void test_v2_body_imports_new_fields_default_floats_zero_source_custom(void)
{
    TEST_SECTION("backup_import_apply -- an OLDER-format (v2) backup, with none of the four new keys, "
                 "must still import: the three floats land at 0, and settings_source lands at "
                 "ZONE_SETTINGS_SOURCE_CUSTOM (0xFF), NEVER 0 -- 0 is a real, different value "
                 "(\"copies zone 0's settings\")");
    reset_stub_state();

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":2,\"profiles\":[],"
        "\"zones\":[{\"index\":0,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0}]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(ok, "a version-2-shaped body (no v3-only keys) must import cleanly under the v3 reader");
    TEST_CHECK(!s_writes[0].set_fuzzy_strength_called, "no fuzzy_strength_pct key present -- setter must not run");
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        TEST_CHECK(!s_writes[0].set_coupling_cell_called[j], "no coupling keys present -- no cell setter must run");
    }
    TEST_CHECK(!s_writes[0].set_coupling_diag_k_dc_called,
              "no coupling_diag_k_dc key present -- setter must not run");
    /* settings_source is UNLIKE the three floats: it has no has_* flag and is
     * ALWAYS committed (see zone_candidate_t's own comment in backup_http.c)
     * -- this is the field the task brief calls out as "the identical trap
     * that nearly destroyed commissioned configs in the v9->v10 NVS migration
     * earlier today" if it defaulted to 0 instead. */
    TEST_CHECK(s_writes[0].set_settings_source_called[SRC_GROUP_LIMITS], "settings_source is ALWAYS committed, even when absent");
    TEST_CHECK(s_writes[0].settings_source[SRC_GROUP_LIMITS] == ZONE_SETTINGS_SOURCE_CUSTOM,
              "and lands at ZONE_SETTINGS_SOURCE_CUSTOM (0xFF) exactly -- asserted explicitly, not \"some value\"");
}

static void test_v3_fuzzy_strength_out_of_range_rejected(void)
{
    TEST_SECTION("backup_import_apply -- fuzzy_strength_pct over ZONE_FUZZY_STRENGTH_PCT_MAX is rejected, "
                 "nothing written");
    reset_stub_state();

    char body[256];
    snprintf(body, sizeof(body),
             "{\"kind\":\"kilnctl_backup\",\"version\":3,\"profiles\":[],"
             "\"zones\":[{\"index\":0,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,\"fuzzy_strength_pct\":%.1f}]}",
             (double)(ZONE_FUZZY_STRENGTH_PCT_MAX + 1.0f));
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(!ok, "fuzzy_strength_pct over ZONE_FUZZY_STRENGTH_PCT_MAX must be rejected in validation");
    TEST_CHECK(g_total_write_calls == 0,
              "pass 1 validates the WHOLE entry before pass 2 commits ANY of it -- even pid_kp/ki/kd, "
              "which were themselves in range, must not have been written");
}

static void test_v3_coupling_neighbor_fractional_rejected(void)
{
    TEST_SECTION("backup_import_apply -- a fractional coupling_neighbor_zone (1.5, IN RANGE for 0-2) is "
                 "refused by the integrality rule, not masked by an earlier range check (LEGACY v3-shaped "
                 "pair, still readable under the v4 reader)");
    reset_stub_state();

    // MAX31856_CHANNEL_COUNT is 3, so the valid index range is 0-2 -- 1.5 is
    // deliberately IN that range so this can only fail on integrality, never
    // on the range check (2.7 would be out of range and mask the bug this
    // test exists to catch).
    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":3,\"profiles\":[],"
        "\"zones\":[{\"index\":0,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,"
        "\"coupling_coeff\":1,\"coupling_neighbor_zone\":1.5}]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(!ok, "a fractional, in-range coupling_neighbor_zone must be refused");
    TEST_CHECK(strstr(err, "coupling_neighbor_zone") != NULL, "the refusal names the field");
    TEST_CHECK(g_total_write_calls == 0, "nothing written for the whole entry, including pid_kp/ki/kd");
}

// THE lossless-backward-compat test BACKUP_FORMAT_VERSION's 3->4 comment
// promises: a genuine version-3 body's legacy pair, with no v4 indexed keys
// present at all, must still land in the right cell of the new row.
static void test_v3_legacy_pair_maps_into_row_cell(void)
{
    TEST_SECTION("backup_import_apply -- a LEGACY (version 3) coupling_coeff/coupling_neighbor_zone pair, "
                 "with no v4 coupling_c%u keys present, maps losslessly onto the matching cell");
    reset_stub_state();

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":3,\"profiles\":[],"
        "\"zones\":[{\"index\":0,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,"
        "\"coupling_coeff\":6.75,\"coupling_neighbor_zone\":2}]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(ok, "a legacy version-3 pair must import cleanly under the v4 reader");
    TEST_CHECK(s_writes[0].set_coupling_cell_called[2], "the legacy pair committed cell 2 (the named neighbor)");
    TEST_CHECK_NEAR(s_writes[0].coupling_coeff[2], 6.75, 1e-6, "and the coefficient is exact");
    TEST_CHECK(!s_writes[0].set_coupling_cell_called[0], "cell 0 (not the named neighbor) was left untouched");
    TEST_CHECK(!s_writes[0].set_coupling_cell_called[1], "cell 1 (the diagonal) was left untouched");
}

// 2026-08-31 defect fix: a LEGACY (version <=3) coupling_neighbor_zone
// pointing at the entry's OWN index must be rejected in PASS 1, same as
// every other coupling check in this function -- not deferred to pass 2's
// zones_config_set_coupling_cell(zc->index, zc->index, ...) call, which the
// setter refuses as a nonzero diagonal. Two zone entries here, in order,
// with zone 0's entry entirely well-formed and zone 1's entry carrying the
// self-referencing legacy pair: g_total_write_calls == 0 proves the defect
// is really fixed -- before the fix, zone 0 committed successfully in pass 2
// (its setters ran) and only zone 1's pass-2 call failed, leaving zone 0's
// write live. All-or-nothing means NEITHER zone may have been written.
static void test_v3_legacy_pair_self_reference_rejected_before_any_commit(void)
{
    TEST_SECTION("backup_import_apply -- a LEGACY coupling_neighbor_zone equal to the entry's OWN index "
                 "is refused in pass 1, before any earlier zone in the same import is committed");
    reset_stub_state();

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":3,\"profiles\":[],"
        "\"zones\":["
        "{\"index\":0,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0},"
        "{\"index\":1,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,"
        "\"coupling_coeff\":4.0,\"coupling_neighbor_zone\":1}]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(!ok, "a self-referencing legacy pair must be refused");
    TEST_CHECK(strstr(err, "coupling_neighbor_zone") != NULL, "the refusal names the field");
    TEST_CHECK(g_total_write_calls == 0,
              "NOTHING was written for either zone -- zone 0's well-formed entry must not have been "
              "committed before zone 1's pass-1 validation caught the self-reference");
}

// A body that carries BOTH the new indexed key and the legacy pair for the
// SAME cell must let the explicit v4 key win -- see zone_candidate_t's own
// "has_coupling_cell" comment in backup_http.c for why the legacy pair is
// ignored once the per-cell key has already claimed that cell.
static void test_v4_key_wins_over_legacy_pair_for_the_same_cell(void)
{
    TEST_SECTION("backup_import_apply -- an explicit coupling_c%u key wins over a legacy pair naming "
                 "the SAME cell, rather than the legacy pair silently overwriting it");
    reset_stub_state();

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":4,\"profiles\":[],"
        "\"zones\":[{\"index\":0,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,"
        "\"coupling_c2\":9.0,\"coupling_coeff\":1.0,\"coupling_neighbor_zone\":2}]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(ok, "a body with both an explicit key and a legacy pair for the same cell must still import");
    TEST_CHECK(s_writes[0].set_coupling_cell_called[2], "cell 2 was committed");
    TEST_CHECK_NEAR(s_writes[0].coupling_coeff[2], 9.0, 1e-6,
                    "the EXPLICIT coupling_c2 value (9.0) won, not the legacy pair's 1.0");
}

// 2026-08-30 (ZONES_CFG_VERSION 10->11): a diagonal cell posted nonzero must
// be refused at the import door -- same rule parse_zone_fields()'s
// z%u_coupling_c%u enforces at the POST layer.
static void test_v4_coupling_diagonal_rejected(void)
{
    TEST_SECTION("backup_import_apply -- a nonzero coupling_c%u for a zone's OWN index (the diagonal) "
                 "is rejected");
    reset_stub_state();

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":4,\"profiles\":[],"
        "\"zones\":[{\"index\":1,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,\"coupling_c1\":2.5}]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(!ok, "a nonzero diagonal cell must be rejected");
    TEST_CHECK(strstr(err, "coupling_c1") != NULL, "the refusal names the specific cell key");
    TEST_CHECK(g_total_write_calls == 0, "nothing written for the whole entry");
}

static void test_v3_settings_source_self_reference_rejected(void)
{
    TEST_SECTION("backup_import_apply -- settings_source pointing at its own zone index is rejected");
    reset_stub_state();

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":3,\"profiles\":[],"
        "\"zones\":[{\"index\":1,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,\"settings_source\":1}]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(!ok, "zone 1 claiming settings_source=1 (itself) must be refused");
    TEST_CHECK(strstr(err, "settings_source") != NULL, "the refusal names the field");
    TEST_CHECK(g_total_write_calls == 0, "nothing written for the whole entry");
}

// Cross-entry cycle: neither zone's own settings_source entry is a
// self-reference, and neither cycles against the LIVE config on its own
// (both start Custom, per reset_stub_state()) -- the cycle only exists
// because BOTH entries close it TOGETHER, in the same import. Each entry's
// own per-entry checks (self-reference, range) have nothing to catch here;
// this is exactly the "hand-edited backup" gap the task brief calls out --
// without the pass-1 cross-entry check, zone 0's entry would commit first
// (a plain 0 -> 1 link, legal against the pre-import Custom/Custom live
// config), and only zone 1's entry, arriving second in pass 2's commit
// loop, would discover the cycle -- at zones_config_set_settings_source()
// itself, AFTER zone 0 was already written live. That is precisely the
// half-applied-import failure mode this file's two-pass split exists to
// prevent (see this function's own header comment, and the self-reference
// test above, which the same reasoning already protects against for the
// single-entry case).
static void test_settings_source_cross_entry_cycle_rejected_before_any_commit(void)
{
    TEST_SECTION("backup_import_apply -- two zone tuning entries that only close a settings_source "
                 "cycle TOGETHER (neither is a self-reference, neither cycles against the live config "
                 "alone) are refused in pass 1, before either is committed");
    reset_stub_state();

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":3,\"profiles\":[],"
        "\"zones\":["
        "{\"index\":0,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,\"settings_source\":1},"
        "{\"index\":1,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,\"settings_source\":0}]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(!ok, "a 2-zone settings_source cycle spanning two entries in the same import must be "
              "refused");
    TEST_CHECK(strstr(err, "settings_source") != NULL, "the refusal names the field");
    TEST_CHECK(strstr(err, "cycle") != NULL, "and calls out the cycle specifically");
    TEST_CHECK(g_total_write_calls == 0,
              "NOTHING was written for either zone -- zone 0's individually-legal entry must not have "
              "been committed before zone 1's entry closed the cycle, and must not be committed at all "
              "once the whole import is refused");
    uint8_t s0 = 0xAA, s1 = 0xAA;
    TEST_CHECK(zones_config_get_settings_source(0, SRC_GROUP_LIMITS, &s0) && s0 == ZONE_SETTINGS_SOURCE_CUSTOM &&
              zones_config_get_settings_source(1, SRC_GROUP_LIMITS, &s1) && s1 == ZONE_SETTINGS_SOURCE_CUSTOM,
              "the live (stub) config is untouched -- still Custom/Custom, exactly as reset_stub_state() "
              "left it");
}

// Positive control for the cross-entry check above: a legal chain spanning
// two entries in the SAME import (zone 0 -> zone 1, zone 1 -> Custom) must
// still import cleanly -- proves the guard refuses only a genuine cycle,
// not any multi-entry settings_source import.
static void test_settings_source_cross_entry_legal_chain_still_imports(void)
{
    TEST_SECTION("backup_import_apply -- a legal (acyclic) settings_source chain spanning two entries "
                 "in the same import is still accepted");
    reset_stub_state();

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":3,\"profiles\":[],"
        "\"zones\":["
        "{\"index\":0,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,\"settings_source\":1},"
        "{\"index\":1,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,\"settings_source\":255}]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(ok, "a legal two-entry chain (0 -> 1 -> Custom) must import cleanly");
    uint8_t s0 = 0xAA, s1 = 0xAA;
    TEST_CHECK(zones_config_get_settings_source(0, SRC_GROUP_LIMITS, &s0) && s0 == 1, "zone 0's link committed as 1");
    TEST_CHECK(zones_config_get_settings_source(1, SRC_GROUP_LIMITS, &s1) && s1 == ZONE_SETTINGS_SOURCE_CUSTOM,
              "zone 1's link committed as Custom");
}

// The BLOCKING defect from the 2026-08-31 opus review of commit b69b74a:
// pass 1 (the cross-entry check exercised above) only ever validated the
// FINAL assembled state, but pass 2 used to commit settings_source one zone
// at a time through the CHECKED setter, which chain-walks against the
// PARTIALLY APPLIED live config while the commit loop is still mid-flight --
// a state pass 1 never sees and never validates. Concrete, ordinary
// scenario -- restoring a valid backup (exported from a properly-configured
// board, so its own settings_source set is internally acyclic) onto a
// DIFFERENTLY-configured live board:
//   Live:   zone 0 = Custom, zone 1 -> 0
//   Backup: zone 0 -> 1, zone 1 -> Custom (export emits in index order, so
//           entry order here is 0 then 1, same as the real exporter)
// Pass 1 probes the FINAL state {0->1, 1->Custom} together -- acyclic,
// accepted. Committing entry 0 first with the OLD checked setter walks the
// then-live {0->1, 1->0} -- a genuine cycle -- and refuses, AFTER zone 0's
// other fields (pid_kp here, standing in for name/PID/masks/cal/ramp/limits/
// heater cfg/coupling cells in the real handler) were already committed this
// same pass. This is the test the fix's own commit-loop comment in
// backup_http.c cites by name.
static void test_settings_source_restore_onto_differently_configured_board_succeeds(void)
{
    TEST_SECTION("backup_import_apply -- BLOCKING FIX: restoring a valid (acyclic) backup onto a "
                 "live board whose CURRENT settings_source links differ succeeds end-to-end, nothing "
                 "half-applied, even though committing entry-by-entry against the live config would "
                 "transiently cycle");
    reset_stub_state();
    // Seed the live (stub) config to zone 0 = Custom, zone 1 -> 0 -- the
    // "differently configured board" the backup is being restored onto.
    TEST_CHECK(zones_config_set_settings_source(1, SRC_GROUP_LIMITS, 0), "live seed: zone 1 -> zone 0");
    uint8_t seeded = 0xAA;
    TEST_CHECK(zones_config_get_settings_source(0, SRC_GROUP_LIMITS, &seeded) && seeded == ZONE_SETTINGS_SOURCE_CUSTOM,
              "live seed: zone 0 is Custom");
    g_total_write_calls = 0; // only count what the import itself does

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":3,\"profiles\":[],"
        "\"zones\":["
        "{\"index\":0,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,\"settings_source\":1},"
        "{\"index\":1,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,\"settings_source\":255}]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(ok, "the restore succeeds end-to-end -- pass 1 already proved the FINAL state "
              "(0->1, 1->Custom) is acyclic, so pass 2 must not be able to fail committing it one "
              "entry at a time against the old live state");
    uint8_t s0 = 0xAA, s1 = 0xAA;
    TEST_CHECK(zones_config_get_settings_source(0, SRC_GROUP_LIMITS, &s0) && s0 == 1,
              "zone 0's settings_source landed as the backup's value (1), not left at the refused "
              "intermediate state");
    TEST_CHECK(zones_config_get_settings_source(1, SRC_GROUP_LIMITS, &s1) && s1 == ZONE_SETTINGS_SOURCE_CUSTOM,
              "zone 1's settings_source landed as the backup's value (Custom) too -- nothing half-applied");
    TEST_CHECK(s_writes[0].set_settings_source_called[SRC_GROUP_LIMITS] && s_writes[1].set_settings_source_called[SRC_GROUP_LIMITS],
              "both entries' settings_source were actually committed, not just accepted on paper");
}

// opus review finding (LOW-MEDIUM): a _no_save() settings_source commit
// failing mid-batch used to leave every (zone, group) pair already
// committed THIS pass sitting mutated in RAM, unpersisted (settings_source_
// dirty never reaches zones_config_save_now() because the loop returns
// early). Fixed by snapshotting every zone's settings_source[] before the
// commit loop and restoring it on the failure arm. Proven here with a
// failure the stub injects (zone 1, group SRC_GROUP_LIMITS) that pass 1's
// own re-validation could never catch on its own -- exactly the kind of
// failure the restore exists for.
static void test_settings_source_commit_failure_restores_pre_import_values(void)
{
    TEST_SECTION("backup_import_apply -- opus review finding (LOW-MEDIUM): a settings_source "
                 "commit failure partway through the batch restores every zone's pre-import "
                 "settings_source[], not just refuses -- zone 0's commit (which succeeded before "
                 "zone 1's forced failure) must not survive the refusal");
    reset_stub_state();
    // Live seed: both zones start at Custom (reset_stub_state()'s own default).
    uint8_t seeded0 = 0xAA;
    TEST_CHECK(zones_config_get_settings_source(0, SRC_GROUP_LIMITS, &seeded0) && seeded0 == ZONE_SETTINGS_SOURCE_CUSTOM,
              "live seed: zone 0 starts at Custom");

    s_force_fail_settings_source_zone = 1;
    s_force_fail_settings_source_group = SRC_GROUP_LIMITS;

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":3,\"profiles\":[],"
        "\"zones\":["
        "{\"index\":0,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,\"settings_source\":1},"
        "{\"index\":1,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,\"settings_source\":255}]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    s_force_fail_settings_source_zone = 0xFFu;
    s_force_fail_settings_source_group = 0xFFu;

    TEST_CHECK(!ok, "the injected commit failure on zone 1 is refused, not silently swallowed");
    uint8_t s0 = 0xAA;
    TEST_CHECK(zones_config_get_settings_source(0, SRC_GROUP_LIMITS, &s0) && s0 == ZONE_SETTINGS_SOURCE_CUSTOM,
              "zone 0's settings_source is restored to its pre-import value (Custom), not left at "
              "the imported value (1) the commit wrote before zone 1's failure");
}

static void test_backup_import_batched_save_fires_exactly_once(void)
{
    TEST_SECTION("backup_import_apply -- a successful multi-zone import commits every zone's "
                 "settings through the batched _no_save setters and persists with exactly one "
                 "zones_config_save_now() call, not one per zone/field");
    reset_stub_state();

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":3,\"profiles\":[],"
        "\"zones\":["
        "{\"index\":0,\"pid_kp\":1,\"pid_ki\":2,\"pid_kd\":3},"
        "{\"index\":1,\"pid_kp\":4,\"pid_ki\":5,\"pid_kd\":6}]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(ok, "multi-zone import with no forced failure succeeds");
    TEST_CHECK(g_settings_source_save_calls == 1,
              "zones_config_save_now() fires exactly once for the whole batch, not per zone");
}

static void test_backup_import_success_tracks_pico_ceiling_down_to_new_max(void)
{
    TEST_SECTION("backup_import_apply -- a successful import that LOWERS a zone max_temp_c calls "
                 "safety_ceiling_sync_apply_lower() once, after the save, with the new live max, so "
                 "the Pico ceiling does not stay above the ESP target (divergence disables heat)");
    reset_stub_state();
    TEST_CHECK(zones_config_set_temp_limits(0, 1200.0f, 0.0f), "seed zone 0 max 1200 C");

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":3,\"profiles\":[],"
        "\"zones\":[{\"index\":0,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,\"max_temp_c\":1000,\"min_temp_c\":0}]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    if (!ok) {
        printf("    import refused: %s\n", err);
    }
    TEST_CHECK(ok, "the lowering import succeeds");
    TEST_CHECK(g_settings_source_save_calls == 1, "the batch saved once");
    TEST_CHECK(g_apply_lower_calls == 1, "apply_lower() ran exactly once");
    TEST_CHECK(g_apply_lower_last_max[0] == 1000.0f,
              "apply_lower() was handed the NEW live zone 0 max (1000), not the old 1200");
}

static void test_backup_import_rollback_tracks_pico_ceiling_back_to_restored_max(void)
{
    TEST_SECTION("backup_import_apply -- a mid-batch failure AFTER guard_raise() raised the Pico "
                 "ceiling rolls RAM back and then calls apply_lower() with the RESTORED (pre-import) "
                 "max, so the raised Pico ceiling is brought back down to the ESP target");
    reset_stub_state();
    TEST_CHECK(zones_config_set_temp_limits(1, 1200.0f, 0.0f), "seed zone 1 max 1200 C");
    g_total_write_calls = 0;

    s_force_fail_settings_source_zone = 1;
    s_force_fail_settings_source_group = SRC_GROUP_LIMITS;
    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":3,\"profiles\":[],"
        "\"zones\":[{\"index\":1,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,\"max_temp_c\":1250,\"min_temp_c\":0,"
        "\"settings_source\":255}]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));
    s_force_fail_settings_source_zone = 0xFFu;
    s_force_fail_settings_source_group = 0xFFu;

    float mx = 0.0f, mn = 0.0f;
    printf("    refusal: %s\n", ok ? "(none)" : err);
    TEST_CHECK(g_total_write_calls > 0,
              "control: the import got past pass-1 validation into the commit loop (a pass-1 "
              "refusal would reach apply_lower() too and make this test vacuous)");
    TEST_CHECK(!ok, "the injected commit failure is refused");
    TEST_CHECK(zones_config_get_temp_limits(1, &mx, &mn) && mx == 1200.0f,
              "zone 1's max rolled back to its pre-import 1200 C");
    TEST_CHECK(g_settings_source_save_calls == 0, "no save on a rolled-back batch");
    TEST_CHECK(g_apply_lower_calls == 1, "apply_lower() still ran on the failure path");
    TEST_CHECK(g_apply_lower_last_max[1] == 1200.0f,
              "apply_lower() was handed the RESTORED max (1200), not the refused 1250");
}

static void test_backup_import_refuses_zone_relay_claimed_by_aux(void)
{
    TEST_SECTION("backup_import_apply -- a zone relay_mask that claims a relay an enabled aux output owns "
                 "is refused and the WHOLE batch rolls back (nothing saved)");
    reset_stub_state();
    TEST_CHECK(zones_config_set_relay_mask(0, 0x01), "seed zone 0 relay_mask 0x01");
    g_total_write_calls = 0;
    g_fake_aux_enabled_mask = 0x04; /* relay 3 is an enabled aux output */
    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":3,\"profiles\":[],"
        "\"zones\":[{\"index\":0,\"pid_kp\":9,\"pid_ki\":0,\"pid_kd\":0,\"relay_mask\":4,"
        "\"settings_source\":255}]}";
    char err[200];
    bool ok = test_backup_import_apply(body, err, sizeof(err));
    printf("    refusal: %s\n", ok ? "(none)" : err);
    TEST_CHECK(g_total_write_calls > 0, "control: the import reached the commit loop (not a pass-1 refusal)");
    TEST_CHECK(!ok, "zone claiming an aux-owned relay is refused");
    TEST_CHECK(strstr(err, "aux") != NULL, "refusal names the aux conflict");
    TEST_CHECK(s_writes[0].relay_mask == 0x01, "zone 0 relay_mask rolled back to 0x01");
    float kp = 0.0f, ki = 0.0f, kd = 0.0f;
    TEST_CHECK(!(zones_config_get_pid(0, &kp, &ki, &kd) && kp == 9.0f), "unrelated imported field rolled back too");
    TEST_CHECK(g_settings_source_save_calls == 0, "no save on a refused batch");

    /* Control: same import, aux owns a different relay -> accepted. */
    reset_stub_state();
    g_fake_aux_enabled_mask = 0x08;
    ok = test_backup_import_apply(body, err, sizeof(err));
    TEST_CHECK(ok, "control: disjoint aux mask imports");
    TEST_CHECK(s_writes[0].relay_mask == 0x04, "control: relay_mask committed");
    g_fake_aux_enabled_mask = 0;
}

static void test_backup_import_failure_restores_whole_batch_not_just_settings_source(void)
{
    TEST_SECTION("backup_import_apply -- a mid-batch commit failure rolls back the WHOLE RAM "
                 "snapshot, not just the settings_source field the failure was injected on: an "
                 "unrelated already-committed field (zone 0's pid) on a different zone must also "
                 "revert, and the batched save/relay-push must never fire");
    reset_stub_state();

    float seed_kp = 0.0f, seed_ki = 0.0f, seed_kd = 0.0f;
    TEST_CHECK(zones_config_set_pid(0, 11.0f, 22.0f, 33.0f), "seed zone 0's pre-import pid");
    TEST_CHECK(zones_config_get_pid(0, &seed_kp, &seed_ki, &seed_kd) &&
                  seed_kp == 11.0f && seed_ki == 22.0f && seed_kd == 33.0f,
              "live seed: zone 0's pid reads back as seeded before import");

    s_force_fail_settings_source_zone = 1;
    s_force_fail_settings_source_group = SRC_GROUP_LIMITS;

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":3,\"profiles\":[],"
        "\"zones\":["
        "{\"index\":0,\"pid_kp\":99,\"pid_ki\":99,\"pid_kd\":99,\"settings_source\":1},"
        "{\"index\":1,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,\"settings_source\":255}]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    s_force_fail_settings_source_zone = 0xFFu;
    s_force_fail_settings_source_group = 0xFFu;

    TEST_CHECK(!ok, "the injected commit failure on zone 1 is refused");

    float kp = 0.0f, ki = 0.0f, kd = 0.0f;
    TEST_CHECK(zones_config_get_pid(0, &kp, &ki, &kd) &&
                  kp == 11.0f && ki == 22.0f && kd == 33.0f,
              "zone 0's pid (committed earlier in the same failed batch, unrelated to the "
              "injected settings_source failure) is rolled back to its pre-import seed, not "
              "left at the imported value (99/99/99) -- proves the snapshot restore is "
              "whole-config, not field-specific");
    TEST_CHECK(g_settings_source_save_calls == 0,
              "a refused batch never reaches the single batched zones_config_save_now() call");
    TEST_CHECK(g_relay_type_push_calls == 0,
              "a refused batch never pushes relay_type hardware changes either");
}

// ---------------------------------------------------------------------------
// Export coverage (PID_EXPANSION_PLAN.md line ~715): backup_export_get_
// handler() was completely untested before this -- a silent no-op or a
// malformed field would have passed every test above, none of which ever
// call it. These drive the real handler (through the #include of
// backup_http.c above, same as backup_import_apply()) with httpd_resp_send_
// chunk() captured into s_export_body by the stub above, and check the
// emitted JSON shape directly, then round-trip it back through
// backup_import_apply() to prove export and import agree on the wire format.
// ---------------------------------------------------------------------------

static esp_err_t run_export(void)
{
    test_export_capture_reset();
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    return backup_export_get_handler(&req);
}

static void test_export_emits_expected_keys_and_values_for_a_known_config(void)
{
    TEST_SECTION("backup_export_get_handler -- emits the expected top-level shape, a known profile, "
                 "and a known zone with all v2-v4 fields, exact values");
    reset_stub_state();

    profile_t p;
    memset(&p, 0, sizeof(p));
    strncpy(p.name, "Cone6", PROFILE_NAME_MAX_LEN);
    p.zone_mask = 0x03;
    p.segment_count = 1;
    p.segments[0].target_c = 1200.0f;
    p.segments[0].ramp_c_per_hr = 100.0f;
    p.segments[0].dwell_min = 10;
    test_stub_profiles_set(0, &p);

    TEST_CHECK(zones_config_set_pid(1, 5.0f, 0.6f, 0.02f), "seed zone 1 pid");
    TEST_CHECK(zones_config_set_tc_type(1, 4), "seed zone 1 tc_type");
    TEST_CHECK(zones_config_set_name(1, "Mid"), "seed zone 1 name");
    TEST_CHECK(zones_config_set_relay_mask(1, 0x02), "seed zone 1 relay_mask");
    TEST_CHECK(zones_config_set_thermo_mask(1, 0x02), "seed zone 1 thermo_mask");
    TEST_CHECK(zones_config_set_ct_mask(1, 0x02), "seed zone 1 ct_mask");
    TEST_CHECK(zones_config_set_cal_offset(1, 0.75f), "seed zone 1 cal_offset_c");
    test_stub_zones_set_max_ramp(1, true, 150.0f); /* export's get_max_ramp is this hook, not the setter below */
    TEST_CHECK(zones_config_set_sanity_rate(1, 2.5f), "seed zone 1 sanity_rate_c_per_min");
    TEST_CHECK(zones_config_set_control_mode(1, 3), "seed zone 1 control_mode (PID_FUZZY)");
    TEST_CHECK(zones_config_set_temp_limits(1, 1250.0f, -15.0f), "seed zone 1 temp limits");
    TEST_CHECK(zones_config_set_heater_cfg(1, 60000.0f, 200.0f, 200.0f), "seed zone 1 heater cfg");
    TEST_CHECK(zones_config_set_fuzzy_strength_pct(1, 42.25f), "seed zone 1 fuzzy_strength_pct");
    TEST_CHECK(zones_config_set_coupling_cell(1, 0, 10.5f, 0.0f, 0.0f), "seed zone 1 coupling cell (1,0)");
    TEST_CHECK(zones_config_set_coupling_cell(1, 2, 3.25f, 0.0f, 0.0f), "seed zone 1 coupling cell (1,2)");
    TEST_CHECK(zones_config_set_settings_source(1, SRC_GROUP_LIMITS, 2), "seed zone 1 settings_source (copies zone 2)");
    TEST_CHECK(zones_config_set_coupling_diag_k_dc(1, 33.5f), "seed zone 1 coupling_diag_k_dc");

    /* Model is the one field this stub setup cannot control from this file
     * (zones_config_get_model() is defined in test_profile_feasibility.c,
     * against its OWN backing array -- see this file's header comment on why
     * that split exists). Read the real answer the export handler will get,
     * so this test proves export forwards it correctly without guessing or
     * depending on run order. */
    float exp_k_dc = 0, exp_tau_s = 0, exp_dead_time_s = 0;
    bool have_model = zones_config_get_model(1, &exp_k_dc, &exp_tau_s, &exp_dead_time_s);

    esp_err_t err = run_export();
    TEST_CHECK(err == ESP_OK, "backup_export_get_handler must return ESP_OK");
    TEST_CHECK(s_export_body != NULL && s_export_len > 0, "the handler must have streamed something");

    TEST_CHECK(strstr(s_export_body, "\"kind\":\"kilnctl_backup\"") != NULL, "top-level kind key");
    TEST_CHECK(strstr(s_export_body, "\"version\":5") != NULL, "top-level version is the CURRENT BACKUP_FORMAT_VERSION (5)");

    TEST_CHECK(strstr(s_export_body, "\"id\":0,\"name\":\"Cone6\",\"zone_mask\":3") != NULL,
              "the seeded profile's id/name/zone_mask are emitted exactly");
    TEST_CHECK(strstr(s_export_body, "\"target_c\":1200,\"ramp_c_per_hr\":100,\"dwell_min\":10") != NULL,
              "the seeded profile's one segment is emitted exactly");

    TEST_CHECK(strstr(s_export_body, "\"index\":1,\"pid_kp\":5,\"pid_ki\":0.600000024,\"pid_kd\":0.0199999996") != NULL,
              "zone 1's pid gains are emitted exactly (%.9g, 2026-09-28)");
    TEST_CHECK(strstr(s_export_body, "\"tc_type\":4") != NULL, "zone 1's tc_type is emitted");
    TEST_CHECK(strstr(s_export_body, "\"name\":\"Mid\"") != NULL, "zone 1's name is emitted");
    TEST_CHECK(strstr(s_export_body, "\"relay_mask\":2,\"thermo_mask\":2,\"ct_mask\":2") != NULL,
              "zone 1's masks are emitted exactly");
    TEST_CHECK(strstr(s_export_body, "\"cal_offset_c\":0.75,") != NULL, "zone 1's cal_offset_c is emitted exactly");
    TEST_CHECK(strstr(s_export_body, "\"max_ramp_c_per_hr\":150,") != NULL,
              "zone 1's max_ramp_c_per_hr is emitted exactly");
    TEST_CHECK(strstr(s_export_body, "\"sanity_rate_c_per_min\":2.5,") != NULL,
              "zone 1's sanity_rate_c_per_min is emitted exactly");
    TEST_CHECK(strstr(s_export_body, "\"control_mode\":3") != NULL, "zone 1's control_mode is emitted exactly");
    TEST_CHECK(strstr(s_export_body, "\"max_temp_c\":1250,\"min_temp_c\":-15,") != NULL,
              "zone 1's temp limits are emitted exactly");
    TEST_CHECK(strstr(s_export_body, "\"heater_window_ms\":60000,\"heater_min_on_ms\":200,\"heater_min_off_ms\":200") != NULL,
              "zone 1's heater cfg is emitted exactly");
    /* The three newer fields the task brief specifically calls out. */
    TEST_CHECK(strstr(s_export_body, "\"fuzzy_strength_pct\":42.25") != NULL,
              "fuzzy_strength_pct is emitted exactly (Phase 2/4 field)");
    TEST_CHECK(strstr(s_export_body, "\"coupling_c0\":10.5") != NULL,
              "coupling_c0 (indexed key, version 4) is emitted exactly (%.9g, 2026-09-28)");
    TEST_CHECK(strstr(s_export_body, "\"coupling_c1\":0,") != NULL,
              "the diagonal cell coupling_c1 (zone 1's own index) is emitted as 0, never omitted "
              "(%.9g, 2026-09-28)");
    TEST_CHECK(strstr(s_export_body, "\"coupling_c2\":3.25") != NULL,
              "coupling_c2 is emitted exactly, DISTINCT from coupling_c0 (%.9g, 2026-09-28)");
    TEST_CHECK(strstr(s_export_body, "\"coupling_diag_k_dc\":33.5") != NULL,
              "coupling_diag_k_dc is emitted exactly (ZONES_CFG_VERSION 14->15 field) (%.9g, 2026-09-28)");
    TEST_CHECK(strstr(s_export_body, "\"settings_source\":2") != NULL,
              "settings_source is emitted exactly");

    char model_needle[96];
    if (have_model) {
        snprintf(model_needle, sizeof(model_needle), "\"model_k_dc\":%.9g,\"model_tau_s\":%.9g,\"model_dead_time_s\":%.9g",
                 (double)exp_k_dc, (double)exp_tau_s, (double)exp_dead_time_s);
        TEST_CHECK(strstr(s_export_body, model_needle) != NULL,
                  "when the model getter answers, export emits its exact values");
    } else {
        TEST_CHECK(strstr(s_export_body, "\"model_k_dc\"") == NULL,
                  "when the model getter cannot answer, export must skip the key entirely, not emit zeros");
    }
}

// Task 2 (bkfinish_assessment.md): backup_export_get_handler's kiln_configs[]
// loop used to pass sizeof(pkg_json_scratch) -- a heap POINTER, not the
// KILN_CFG_EXPORT_JSON_MAX_LEN buffer it actually allocated -- as the output
// capacity to kiln_cfg_store_export_package_json(). sizeof(char*) is far
// smaller than any real package JSON, so every populated slot's package was
// silently refused and replaced with an "omitted":"export_failed: ..." entry
// instead of its real content. This proves a populated slot's package now
// actually appears with real content.
static void test_backup_export_kiln_config_package_present(void)
{
    TEST_SECTION("backup_export_get_handler -- a populated kiln config slot's \"package\" is "
                 "the real package JSON, not an export_failed omission (task 2 sizeof-pointer fix)");
    reset_stub_state();

    int32_t id1 = -1;
    char reason[160] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Pkg-Present-Test", -1, &id1, reason, sizeof(reason)),
              "setup: save a kiln config slot from the current (stubbed) board state");

    esp_err_t err = run_export();
    TEST_CHECK(err == ESP_OK, "backup_export_get_handler must return ESP_OK");
    TEST_CHECK(s_export_body != NULL && s_export_len > 0, "the handler must have streamed something");

    TEST_CHECK(strstr(s_export_body, "\"omitted\":\"export_failed") == NULL,
              "no kiln_configs[] entry is silently omitted as export_failed");
    char *needle = strstr(s_export_body, "\"name\":\"Pkg-Present-Test\"");
    TEST_CHECK(needle != NULL, "the saved slot appears in kiln_configs[]");
    if (needle) {
        // The slot's own "package" key should follow shortly after its name
        // within the same object -- look for it in a small window rather
        // than scanning the whole body, so a DIFFERENT slot's package
        // (there is only one here, but keep the test honest about what it's
        // actually checking) can't accidentally satisfy this.
        char window[256] = {0};
        strncpy(window, needle, sizeof(window) - 1);
        TEST_CHECK(strstr(window, "\"package\":{") != NULL,
                  "the slot's own package is present as real, non-empty JSON, not an omission");
    }
}

// Task 4 (bkfinish_assessment.md): backup_import_kiln_configs()'s three
// documented restore-side contracts for the "kiln_configs" array itself,
// none of which had any test coverage before this.
static void test_kiln_configs_absent_key_is_a_no_op(void)
{
    TEST_SECTION("backup_import_apply -- an absent \"kiln_configs\" key behaves exactly like today's "
                 "pre-item-17 backups: a pre-existing kiln config slot must survive untouched");
    reset_stub_state();

    // kiln_cfg_store.c is linked as a real, once-loaded module shared by
    // every test in this executable (unlike test_kiln_cfg_store.c, this file
    // has no access to its file-static reset_to_defaults() -- it only
    // #includes the header, not the .c), so slots saved by earlier tests in
    // this run may already be present. Snapshot the count/name-set BEFORE
    // this test's own save rather than assuming a fresh store.
    kiln_cfg_summary_t before[KILN_CFG_MAX_COUNT];
    uint8_t n_before = kiln_cfg_store_list(before, KILN_CFG_MAX_COUNT);

    int32_t id1 = -1;
    char reason[160] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Untouched Marker Slot", -1, &id1, reason, sizeof(reason)),
              "setup: one kiln config slot already exists on the board");

    const char *body = "{\"kind\":\"kilnctl_backup\",\"version\":5,\"profiles\":[],\"zones\":[]}";
    char err[160] = {0};
    bool ok = test_backup_import_apply(body, err, sizeof(err));
    TEST_CHECK(ok, "a body with no \"kiln_configs\" key at all must still import cleanly");

    kiln_cfg_summary_t after[KILN_CFG_MAX_COUNT];
    uint8_t n_after = kiln_cfg_store_list(after, KILN_CFG_MAX_COUNT);
    TEST_CHECK(n_after == n_before + 1,
              "the count is exactly one more than before this test's own setup save -- nothing added, "
              "nothing removed, by the restore itself");
    bool found = false;
    for (uint8_t i = 0; i < n_after; i++) {
        if (strcmp(after[i].name, "Untouched Marker Slot") == 0) {
            found = true;
            break;
        }
    }
    TEST_CHECK(found, "this test's own slot is exactly as it was named -- untouched, not renamed");
}

static void test_kiln_configs_present_empty_merge_is_a_no_op(void)
{
    TEST_SECTION("backup_import_apply -- \"kiln_configs\":[] (present but empty) under MERGE mode makes "
                 "no change to the board's existing kiln config slots");
    reset_stub_state();

    // See test_kiln_configs_absent_key_is_a_no_op()'s comment: this file has
    // no way to reset kiln_cfg_store.c's own state, so compare against a
    // BEFORE snapshot rather than an assumed-empty store.
    kiln_cfg_summary_t before[KILN_CFG_MAX_COUNT];
    uint8_t n_before = kiln_cfg_store_list(before, KILN_CFG_MAX_COUNT);

    int32_t id1 = -1;
    char reason[160] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Still Here Marker Slot", -1, &id1, reason, sizeof(reason)),
              "setup: one kiln config slot already exists on the board");

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":5,\"profiles\":[],\"zones\":[],\"kiln_configs\":[]}";
    char err[160] = {0};
    bool ok = test_backup_import_apply(body, err, sizeof(err));
    TEST_CHECK(ok, "an explicit empty kiln_configs array must still import cleanly under MERGE");

    kiln_cfg_summary_t after[KILN_CFG_MAX_COUNT];
    uint8_t n_after = kiln_cfg_store_list(after, KILN_CFG_MAX_COUNT);
    TEST_CHECK(n_after == n_before + 1,
              "MERGE with nothing in the backup's kiln_configs array adds/removes nothing beyond this "
              "test's own setup save");
    bool found = false;
    for (uint8_t i = 0; i < n_after; i++) {
        if (strcmp(after[i].name, "Still Here Marker Slot") == 0) {
            found = true;
            break;
        }
    }
    TEST_CHECK(found, "the pre-existing slot survives an empty-array MERGE untouched");
}

static void test_kiln_configs_malformed_entry_refuses_whole_restore(void)
{
    TEST_SECTION("backup_import_apply -- a malformed kiln_configs[] entry refuses the WHOLE restore "
                 "(profiles/zones included), naming the entry index, with nothing written");
    reset_stub_state();

    // See test_kiln_configs_absent_key_is_a_no_op()'s comment: compare
    // against a BEFORE snapshot, not an assumed-empty store.
    kiln_cfg_summary_t before[KILN_CFG_MAX_COUNT];
    uint8_t n_before = kiln_cfg_store_list(before, KILN_CFG_MAX_COUNT);

    int32_t id1 = -1;
    char reason[160] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Before Restore Marker", -1, &id1, reason, sizeof(reason)),
              "setup: one kiln config slot already exists on the board");

    // A "package" object present but missing required fields (no "kind"/
    // "esp_blob_hex"/"pkg_hash" at all) is refused by
    // kiln_cfg_store_validate_package_json() -- same malformed-envelope path
    // test_kiln_cfg_store.c's own import-refuses-malformed tests exercise,
    // just reached this time through the restore's kiln_configs[] array
    // rather than a direct call.
    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":5,"
        "\"profiles\":[{\"id\":0,\"name\":\"Should Not Land\",\"segment_count\":0}],\"zones\":[],"
        "\"kiln_configs\":[{\"name\":\"Bad Entry\",\"package\":{}}]}";
    char err[160] = {0};
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(!ok, "a malformed kiln_configs[] entry must refuse the whole restore");
    TEST_CHECK(strstr(err, "kiln_configs[0]") != NULL, "the error names the specific entry index that failed");

    kiln_cfg_summary_t after[KILN_CFG_MAX_COUNT];
    uint8_t n_after = kiln_cfg_store_list(after, KILN_CFG_MAX_COUNT);
    TEST_CHECK(n_after == n_before + 1,
              "only this test's own setup save landed -- the malformed restore itself wrote nothing "
              "further (no rename/create/delete from the refused body)");
    TEST_CHECK(g_profile_save_calls == 0,
              "the profiles pass never even ran -- kiln_configs[] is validated FIRST, before any profile "
              "candidate is committed, so the whole restore is refused with nothing written anywhere");
}

// Task 7 (bkfinish_assessment.md): active-slot restoration was dropped
// entirely -- a restore's CREATE/RENAME actions never touch active_id (see
// backup_import.c's comment above the file_action_t loop). This is the host
// test that comment promised: a freshly restored slot must not get
// overwritten the moment an unrelated autosave fires, because autosave
// dispatches onto whatever active_id ALREADY was, never onto the slot a
// restore just created.
static void test_kiln_configs_restore_create_leaves_autosave_targeting_the_prior_active_slot(void)
{
    TEST_SECTION("backup_import_apply -- a restored (CREATE) kiln_configs[] slot is not the autosave target: "
                 "active_id survives the restore untouched, and a later autosave_from_live() re-captures the "
                 "PRIOR active slot, not the freshly restored one");
    reset_stub_state();

    // Defensive: this combined test binary shares one fake_kv.c partition
    // table across every test_*.c linked into it (see test_kiln_cfg_store.c),
    // and at least one earlier-running test elsewhere in that shared table
    // calls fake_kv_reset_all() without a same-test hal_kv_init_partition()
    // follow-up, which otherwise leaves kiln_nvs deregistered by the time
    // this test's restore reaches kiln_cfg_store_import_package_json_as()'s
    // commit (observed as HAL_NOT_READY there). hal_kv_init_partition() is
    // documented idempotent (a no-op if already up), so this is safe
    // regardless of what ran before.
    TEST_CHECK(hal_kv_init_partition("kiln_nvs") == HAL_OK, "setup: kiln_nvs partition is up");

    // Slot A: today's live state (thermo_count 3, from reset_stub_state()),
    // saved and therefore active.
    int32_t id_a = -1;
    char reason[160] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Active Original", -1, &id_a, reason, sizeof(reason)),
              "setup: slot A saved from the current live state");
    TEST_CHECK(kiln_cfg_store_get_active_id() == id_a, "setup: slot A is active");

    // Change the live state so a package built from it has a genuinely
    // DIFFERENT identity (schema/hash) than slot A's -- otherwise the
    // restore below would match slot A by identity (Case 1/2) instead of
    // creating a new slot, and this test would not be exercising CREATE at
    // all. Neither test_stub_zones_set_thermo_count() nor
    // zones_config_set_pid() reach the byte content
    // kiln_cfg_store_save_current() actually exports in this combined test
    // binary (test_kiln_cfg_store.c's zones_config_export_blob() stub
    // returns a fixed, shared s_stub_export_content buffer, unrelated to
    // either of those setters) -- toggle that buffer's own content directly
    // via its exported test hook instead.
    test_stub_kiln_cfg_export_content_toggle_byte0();

    // Capture that new state as a temporary slot B purely to get a real,
    // validly-hashed package JSON for it (kiln_cfg_store_export_package_json
    // exports an already-saved slot's stored blob, not raw live state), then
    // delete B so the board's only real slot is A again -- the restore body
    // built from B's export must land as a CREATE, not match anything.
    int32_t id_b = -1;
    TEST_CHECK(kiln_cfg_store_save_current("Temp For Export", -1, &id_b, reason, sizeof(reason)),
              "setup: slot B (the distinct-identity source) saved");
    char *pkg_json = (char *)malloc(KILN_CFG_EXPORT_JSON_MAX_LEN);
    TEST_CHECK(pkg_json != NULL, "test scratch alloc");
    size_t pkg_len = 0;
    TEST_CHECK(kiln_cfg_store_export_package_json(id_b, pkg_json, KILN_CFG_EXPORT_JSON_MAX_LEN, &pkg_len, reason,
                                                  sizeof(reason)),
              "setup: slot B exports a real, validly-hashed package");
    // save_current() made B active; kiln_cfg_store_delete() refuses to
    // delete the active slot outright (finding H5), so point active_id back
    // at A first -- pure bookkeeping, same call this test later proves the
    // real restore path must NOT make (see the comment on the file_action_t
    // loop in backup_import.c).
    TEST_CHECK(kiln_cfg_store_set_active_id_raw(id_a, reason, sizeof(reason)), "setup: active_id restored to A");
    TEST_CHECK(kiln_cfg_store_delete(id_b, true, reason, sizeof(reason)), "setup: slot B removed after export");

    // Rename the package's own name field so the restored slot is
    // unambiguously identifiable ("Temp For Export" -> "Restored Slot",
    // same length so no JSON length/offset bookkeeping is needed).
    char *name_field = strstr(pkg_json, "\"name\":\"Temp For Export\"");
    TEST_CHECK(name_field != NULL, "found the name field to rename before restoring");
    if (name_field) {
        memcpy(name_field, "\"name\":\"Restored Slot  \"", strlen("\"name\":\"Restored Slot  \""));
    }

    char *body = (char *)malloc(KILN_CFG_EXPORT_JSON_MAX_LEN + 256);
    TEST_CHECK(body != NULL, "test scratch alloc");
    snprintf(body, KILN_CFG_EXPORT_JSON_MAX_LEN + 256,
             "{\"kind\":\"kilnctl_backup\",\"version\":5,\"profiles\":[],\"zones\":[],"
             "\"kiln_configs\":[{\"is_active\":true,\"package\":%s}]}",
             pkg_json);

    char err[160] = {0};
    bool ok = test_backup_import_apply(body, err, sizeof(err));
    TEST_CHECK(ok, "the restore (a CREATE, distinct identity from slot A) must succeed");

    TEST_CHECK(kiln_cfg_store_get_active_id() == id_a,
              "active_id is UNCHANGED by the restore -- the freshly created slot never becomes active "
              "(task 7: active-slot restoration is dropped entirely)");

    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    uint8_t n = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    int32_t id_restored = -1;
    for (uint8_t i = 0; i < n; i++) {
        if (strcmp(rows[i].name, "Restored Slot") == 0) {
            id_restored = rows[i].id;
            break;
        }
    }
    TEST_CHECK(id_restored != -1, "the restored slot exists under its restored name");

    bool restored_pico_populated = false;
    uint16_t restored_schema_before = 0;
    uint32_t restored_hash_before = 0;
    TEST_CHECK(kiln_cfg_store_get_package_identity(id_restored, &restored_pico_populated, &restored_schema_before,
                                                   &restored_hash_before),
              "read back the restored slot's identity right after the restore");

    // The live state is still the byte-toggled export state set above
    // (nothing since has changed it). An autosave now must recapture SLOT A (the
    // prior active slot) -- never the freshly restored slot, which
    // active_id was never pointed at.
    TEST_CHECK(kiln_cfg_store_autosave_from_live(reason, sizeof(reason)), "autosave_from_live() succeeds");
    TEST_CHECK(kiln_cfg_store_get_active_id() == id_a, "active_id is still slot A after the autosave");

    uint16_t restored_schema_after = 0;
    uint32_t restored_hash_after = 0;
    TEST_CHECK(kiln_cfg_store_get_package_identity(id_restored, &restored_pico_populated, &restored_schema_after,
                                                   &restored_hash_after),
              "read back the restored slot's identity again after the autosave");
    TEST_CHECK(restored_schema_after == restored_schema_before && restored_hash_after == restored_hash_before,
              "the freshly restored slot's content is BIT-FOR-BIT UNCHANGED by the autosave -- it was not the "
              "autosave's target");

    // Sanity: prove the autosave was not simply a no-op -- slot A's own
    // identity must now match the restored slot's (both were captured from
    // the same byte-toggled live state, at different times), confirming
    // autosave really did re-capture SOMETHING.
    bool a_pico_populated = false;
    uint16_t a_schema_after = 0;
    uint32_t a_hash_after = 0;
    TEST_CHECK(kiln_cfg_store_get_package_identity(id_a, &a_pico_populated, &a_schema_after, &a_hash_after),
              "read back slot A's identity after the autosave");
    TEST_CHECK(a_schema_after == restored_schema_before && a_hash_after == restored_hash_before,
              "slot A's identity now matches the live state actually captured by the autosave, proving the "
              "autosave really ran (not a silent no-op) against the RIGHT slot");

    free(pkg_json);
    free(body);
}

// 2026-09-15 (Opus adversarial re-review, F6): before this fix,
// backup_export.c read zones_config_get_safety_tc_type() -- the ESP's own
// cache, which can be stale relative to the Pico's actual configured
// value (exactly the class of bug N5 already fixed on the GET-page path).
// This proves the export path was actually switched to the live getter:
// the two stubs are seeded with DIFFERENT values, and only the live one
// may appear in the emitted JSON.
static void test_export_emits_live_pico_tc_type_not_stale_esp_cache(void)
{
    TEST_SECTION("backup_export_get_handler -- emits the LIVE Pico tc_type, not the possibly-stale "
                 "ESP-side cache (F6)");
    reset_stub_state();
    s_safety_tc_type = 3;  // stale ESP cache -- must NOT appear
    s_pico_tc_type = 9;    // live Pico read-back -- must appear
    s_pico_tc_type_known = true;

    esp_err_t err = run_export();

    TEST_CHECK(err == ESP_OK, "backup_export_get_handler must return ESP_OK");
    TEST_CHECK(strstr(s_export_body, "\"safety_tc_type\":9") != NULL,
              "the live Pico value (9) is emitted");
    TEST_CHECK(strstr(s_export_body, "\"safety_tc_type\":3") == NULL,
              "the stale ESP-cached value (3) must not be emitted");

    // Unknown (never fetched from the Pico yet) must not fabricate a
    // plausible-looking value either -- same "0 only when known" convention
    // zones_http_get.c uses.
    reset_stub_state();
    s_safety_tc_type = 3;
    s_pico_tc_type = 9;
    s_pico_tc_type_known = false;
    err = run_export();
    TEST_CHECK(err == ESP_OK, "backup_export_get_handler must still return ESP_OK when unknown");
    TEST_CHECK(strstr(s_export_body, "\"safety_tc_type\":0") != NULL,
              "an unknown live value is emitted as 0, not the stale ESP cache (3) nor the "
              "stub's own unknown-but-set value (9)");
}

// The round trip: export a known config, then feed the SAME emitted JSON
// back into backup_import_apply() (real handler, real reader, same as any
// other test in this file) against a DIFFERENT starting state, and check the
// result matches what was exported bit for bit on every field the export
// test above already pinned down -- including the version-4 fields
// (fuzzy_strength_pct, the indexed coupling_c%u keys, settings_source,
// control_mode) the task brief calls out by name.
static void test_export_round_trips_through_import_to_identical_config(void)
{
    TEST_SECTION("backup_export_get_handler -> backup_import_apply -- round trip reproduces the exact config");
    reset_stub_state();

    profile_t p;
    memset(&p, 0, sizeof(p));
    strncpy(p.name, "Cone6", PROFILE_NAME_MAX_LEN);
    p.zone_mask = 0x03;
    p.segment_count = 1;
    p.segments[0].target_c = 1200.0f;
    p.segments[0].ramp_c_per_hr = 100.0f;
    p.segments[0].dwell_min = 10;
    test_stub_profiles_set(0, &p);

    zones_config_set_pid(1, 5.0f, 0.6f, 0.02f);
    zones_config_set_tc_type(1, 4);
    zones_config_set_name(1, "Mid");
    zones_config_set_relay_mask(1, 0x02);
    zones_config_set_thermo_mask(1, 0x02);
    zones_config_set_ct_mask(1, 0x02);
    zones_config_set_cal_offset(1, 0.75f);
    test_stub_zones_set_max_ramp(1, true, 150.0f);
    zones_config_set_sanity_rate(1, 2.5f);
    zones_config_set_control_mode(1, 3);
    zones_config_set_temp_limits(1, 1250.0f, -15.0f);
    zones_config_set_heater_cfg(1, 60000.0f, 200.0f, 200.0f);
    zones_config_set_fuzzy_strength_pct(1, 42.25f);
    zones_config_set_coupling_cell(1, 0, 10.5f, 0.0f, 0.0f);
    zones_config_set_coupling_cell(1, 2, 3.25f, 0.0f, 0.0f);
    zones_config_set_settings_source(1, SRC_GROUP_LIMITS, 2);
    zones_config_set_coupling_diag_k_dc(1, 33.5f);
    zones_config_set_failsafe_state(1, true);
    zones_config_set_hyst_c(1, 4.5f);
    zones_config_set_min_on_s(1, 30);
    zones_config_set_min_off_s(1, 45);

    esp_err_t err = run_export();
    TEST_CHECK(err == ESP_OK, "export must succeed");
    TEST_CHECK(s_export_body != NULL && s_export_len > 0, "export must have produced a body to import back");

    /* Reset to a DIFFERENT state (not all-zero, not the exported values) so
     * a re-import that silently no-ops would be caught by every field below
     * still reading the wrong, pre-import value rather than accidentally
     * matching by coincidence. */
    reset_stub_state();
    zones_config_set_pid(1, 1.0f, 1.0f, 1.0f);
    zones_config_set_settings_source(1, SRC_GROUP_LIMITS, 0);
    zones_config_set_failsafe_state(1, false);
    zones_config_set_hyst_c(1, 1.0f);
    zones_config_set_min_on_s(1, 1);
    zones_config_set_min_off_s(1, 1);
    g_total_write_calls = 0;

    char import_err[256];
    bool ok = test_backup_import_apply(s_export_body, import_err, sizeof(import_err));
    TEST_CHECK(ok, "re-importing exactly what was just exported must succeed");

    TEST_CHECK_NEAR(s_writes[1].kp, 5.0, 1e-6, "pid_kp round-trips through export->import");
    TEST_CHECK_NEAR(s_writes[1].ki, 0.6, 1e-6, "pid_ki round-trips through export->import");
    TEST_CHECK_NEAR(s_writes[1].kd, 0.02, 1e-6, "pid_kd round-trips through export->import");
    TEST_CHECK(s_writes[1].tc_type == 4, "tc_type round-trips");
    TEST_CHECK(strcmp(s_writes[1].name, "Mid") == 0, "name round-trips");
    TEST_CHECK(s_writes[1].relay_mask == 0x02 && s_writes[1].thermo_mask == 0x02 && s_writes[1].ct_mask == 0x02,
              "masks round-trip");
    TEST_CHECK_NEAR(s_writes[1].cal_offset_c, 0.75, 1e-6, "cal_offset_c round-trips");
    TEST_CHECK_NEAR(s_writes[1].sanity_rate_c_per_min, 2.5, 1e-6, "sanity_rate_c_per_min round-trips");
    TEST_CHECK(s_writes[1].control_mode == 3, "control_mode round-trips");
    TEST_CHECK_NEAR(s_writes[1].max_temp_c, 1250.0, 1e-6, "max_temp_c round-trips");
    TEST_CHECK_NEAR(s_writes[1].min_temp_c, -15.0, 1e-6, "min_temp_c round-trips");
    TEST_CHECK_NEAR(s_writes[1].heater_window_ms, 60000.0, 1e-6, "heater_window_ms round-trips");
    TEST_CHECK_NEAR(s_writes[1].fuzzy_strength_pct, 42.25, 1e-6, "fuzzy_strength_pct round-trips");
    TEST_CHECK_NEAR(s_writes[1].coupling_coeff[0], 10.5, 1e-3, "coupling_c0 round-trips (through the .4f/.4f wire format)");
    TEST_CHECK_NEAR(s_writes[1].coupling_coeff[2], 3.25, 1e-3, "coupling_c2 round-trips, DISTINCT from coupling_c0");
    TEST_CHECK(s_writes[1].coupling_coeff[1] == 0.0f, "the diagonal cell round-trips as 0");
    TEST_CHECK_NEAR(s_writes[1].coupling_diag_k_dc, 33.5, 1e-3,
                    "coupling_diag_k_dc round-trips through export->import");
    TEST_CHECK(s_writes[1].settings_source[SRC_GROUP_LIMITS] == 2, "settings_source round-trips (NOT the 0 left over from the "
              "pre-import poison state above, proving import actually ran, not a no-op that left it alone)");
    TEST_CHECK(s_writes[1].set_failsafe_state_called && s_writes[1].failsafe_state == true,
              "failsafe_state round-trips (NOT the false left over from the pre-import poison state)");
    TEST_CHECK_NEAR(s_writes[1].hyst_c, 4.5, 1e-6, "hyst_c round-trips (NOT the 1.0 poison value)");
    TEST_CHECK(s_writes[1].min_on_s == 30, "min_on_s round-trips (NOT the 1 poison value)");
    TEST_CHECK(s_writes[1].min_off_s == 45, "min_off_s round-trips (NOT the 1 poison value)");

    TEST_CHECK(g_profile_save_calls == 1, "the one exported profile was re-committed");
    TEST_CHECK(strcmp(g_last_saved_profile.name, "Cone6") == 0, "profile name round-trips");
    TEST_CHECK(g_last_saved_profile.zone_mask == 0x03, "profile zone_mask round-trips");
    TEST_CHECK(g_last_saved_profile.segment_count == 1, "profile segment_count round-trips");
    TEST_CHECK_NEAR(g_last_saved_profile.segments[0].target_c, 1200.0, 1e-6, "profile segment target_c round-trips");
    TEST_CHECK_NEAR(g_last_saved_profile.segments[0].ramp_c_per_hr, 100.0, 1e-6, "profile segment ramp_c_per_hr round-trips");
    TEST_CHECK(g_last_saved_profile.segments[0].dwell_min == 10, "profile segment dwell_min round-trips");
    TEST_CHECK(g_settings_source_save_calls == 1,
              "the whole zone-field batch above persists through exactly one "
              "zones_config_save_now() call, not one per field");
}

// Bench A4 (2026-09-28): an export/import round trip of a live 3-zone
// matrix with zone 2 typed ZONE_TYPE_ON_OFF zeroed every coupling cell
// touching zone 2 -- as either row (z2[0], z2[1]) or column (z0[2], z1[2])
// -- while cells that never touch zone 2 (z0[1], z1[0]) survived untouched.
// Root cause: backup_export.c read the matrix through
// zones_config_get_coupling(), which deliberately zeroes any row/column
// touching an on/off zone as a live-control-loop guard (docs/
// ON_OFF_ZONE_PLAN.md sec 1) -- exporting through that masking getter
// permanently lost the real stored value the moment it was written back by
// import via zones_config_set_coupling_cell() (which has no on/off
// awareness at all). Fix: export now reads zones_config_get_coupling_raw(),
// the unmasked sibling, so the true stored matrix round-trips regardless of
// the zone's current type. This test sets up exactly the bench matrix,
// exports with zone 2 on/off, and proves every cell -- including the ones
// touching zone 2 -- comes back unchanged.
static void test_export_preserves_coupling_matrix_when_a_zone_is_on_off(void)
{
    TEST_SECTION("backup_export_get_handler -> backup_import_apply -- coupling matrix round-trips "
                 "unchanged even when one zone is ZONE_TYPE_ON_OFF (bench A4, 2026-09-28)");
    reset_stub_state();

    /* Seed all three zones' PID so export sees them as configured, then the
     * exact bench matrix: z0=[0,25.42,24.52] z1=[12.44,0,28.69] z2=[8.08,10.81,0]. */
    for (uint8_t zi = 0; zi < 3; zi++) {
        TEST_CHECK(zones_config_set_pid(zi, 1.0f, 0.1f, 0.0f), "seed zone pid so export answers");
    }
    TEST_CHECK(zones_config_set_coupling_cell(0, 1, 25.42f, 0.0f, 0.0f), "seed z0[1]");
    TEST_CHECK(zones_config_set_coupling_cell(0, 2, 24.52f, 0.0f, 0.0f), "seed z0[2]");
    TEST_CHECK(zones_config_set_coupling_cell(1, 0, 12.44f, 0.0f, 0.0f), "seed z1[0]");
    TEST_CHECK(zones_config_set_coupling_cell(1, 2, 28.69f, 0.0f, 0.0f), "seed z1[2]");
    TEST_CHECK(zones_config_set_coupling_cell(2, 0, 8.08f, 0.0f, 0.0f), "seed z2[0]");
    TEST_CHECK(zones_config_set_coupling_cell(2, 1, 10.81f, 0.0f, 0.0f), "seed z2[1]");

    /* Zone 2 is currently ZONE_TYPE_ON_OFF on the bench -- set directly on
     * the stub state (not through zones_config_set_zone_type(), which this
     * file's own stub also just writes into s_writes -- either is
     * equivalent here). */
    s_writes[2].zone_type = (uint8_t)ZONE_TYPE_ON_OFF;

    esp_err_t err = run_export();
    TEST_CHECK(err == ESP_OK, "export must succeed with an on/off zone present");
    TEST_CHECK(s_export_body != NULL && s_export_len > 0, "export must have produced a body");

    /* Poison every cell to a value distinct from both the real matrix and
     * zero, so a re-import that silently no-ops or that lands a real zero
     * is equally detectable. */
    for (uint8_t a = 0; a < 3; a++) {
        for (uint8_t b = 0; b < 3; b++) {
            if (a != b) {
                s_writes[a].coupling_coeff[b] = 99.0f;
            }
        }
    }
    g_total_write_calls = 0;

    char import_err[256];
    bool ok = test_backup_import_apply(s_export_body, import_err, sizeof(import_err));
    TEST_CHECK(ok, "re-importing the exported on/off-zone matrix must succeed");

    TEST_CHECK_NEAR(s_writes[0].coupling_coeff[1], 25.42, 1e-3, "z0[1] round-trips (touches no on/off zone)");
    TEST_CHECK_NEAR(s_writes[0].coupling_coeff[2], 24.52, 1e-3,
                    "z0[2] round-trips -- NOT zeroed by exporting through the on/off column mask");
    TEST_CHECK_NEAR(s_writes[1].coupling_coeff[0], 12.44, 1e-3, "z1[0] round-trips (touches no on/off zone)");
    TEST_CHECK_NEAR(s_writes[1].coupling_coeff[2], 28.69, 1e-3,
                    "z1[2] round-trips -- NOT zeroed by exporting through the on/off column mask");
    TEST_CHECK_NEAR(s_writes[2].coupling_coeff[0], 8.08, 1e-3,
                    "z2[0] round-trips -- NOT zeroed by exporting through the on/off row mask");
    TEST_CHECK_NEAR(s_writes[2].coupling_coeff[1], 10.81, 1e-3,
                    "z2[1] round-trips -- NOT zeroed by exporting through the on/off row mask");
}

// Import-side sibling of the test above (2026-09-28 review of the A4 fix):
// backup_import.c's per-cell commit merge reads the CURRENT coefficient back
// for any cell whose import supplies tau/dead_time but omits coupling_c%u,
// then writes it straight back through zones_config_set_coupling_cell().
// Reading that through the masking zones_config_get_coupling() committed a
// real 0.0 over the stored coefficient of any cell touching an on/off zone
// -- the same loss as the export bug, via a tau-only (hand-edited or
// partial) backup instead of a plain round trip.
static void test_import_tau_only_cell_preserves_coeff_touching_on_off_zone(void)
{
    TEST_SECTION("backup_import_apply -- a tau-only cell touching an on/off zone preserves the "
                 "stored coupling coefficient instead of committing the masked 0.0");
    reset_stub_state();

    TEST_CHECK(zones_config_set_coupling_cell(0, 2, 24.52f, 111.0f, 22.0f), "seed z0[2] coeff/tau/dead_time");
    s_writes[2].zone_type = (uint8_t)ZONE_TYPE_ON_OFF;
    memset(s_writes[0].set_coupling_cell_called, 0, sizeof(s_writes[0].set_coupling_cell_called));
    g_total_write_calls = 0;

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":4,\"profiles\":[],"
        "\"zones\":[{\"index\":0,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,\"coupling_tau_c2\":200.0}]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(ok, "a tau-only cell entry must import");
    TEST_CHECK(s_writes[0].set_coupling_cell_called[2], "cell 2 was committed (tau supplied)");
    TEST_CHECK_NEAR(s_writes[0].coupling_tau_s[2], 200.0, 1e-6, "the supplied tau_s was updated");
    TEST_CHECK_NEAR(s_writes[0].coupling_coeff[2], 24.52, 1e-3,
                    "z0[2] coeff PRESERVED at the stored 24.52 -- not the masked 0.0 for an on/off column");
    TEST_CHECK_NEAR(s_writes[0].coupling_dead_time_s[2], 22.0, 1e-6, "dead_time_s preserved");
}

// A NON-empty timing_profiles[] bundle: this is the case the empty-bundle
// fix above deliberately does NOT exercise, so it needs its own test proving
// the bundle itself round-trips and a zone's timing_profile index that
// legitimately points into it is both validated and restored.
static void test_timing_profiles_bundle_round_trips_nonempty(void)
{
    TEST_SECTION("backup_import_apply -- non-empty timing_profiles[] bundle round-trips, "
                 "and a zone's timing_profile index pointing into it is restored");
    reset_stub_state();

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":4,\"profiles\":[],"
        "\"timing_profiles\":["
        "{\"name\":\"Slow\",\"progress_duty_min\":0.1,\"progress_window_s\":60,"
        "\"drift_hysteresis_c\":1.5,\"frozen_eps_c\":0.2,\"cross_zone_period_s\":30,"
        "\"bangbang_hysteresis_c\":2.0,\"cooling_limited_margin_c\":5.0,"
        "\"cooling_limited_hold_s\":120,\"ramp_lock_band_c\":25},"
        "{\"name\":\"Fast\",\"progress_duty_min\":0.5,\"progress_window_s\":10,"
        "\"drift_hysteresis_c\":0.5,\"frozen_eps_c\":0.1,\"cross_zone_period_s\":5,"
        "\"bangbang_hysteresis_c\":1.0,\"cooling_limited_margin_c\":2.0,"
        "\"cooling_limited_hold_s\":30,\"ramp_lock_band_c\":25}],"
        "\"zones\":["
        "{\"index\":0,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,\"timing_profile\":1}]}";
    char err[256];
    bool ok = test_backup_import_apply(body, err, sizeof(err));
    TEST_CHECK(ok, "importing a real, non-empty timing_profiles[] bundle with a zone index inside it must succeed");
    TEST_CHECK(s_timing_profile_count == 2, "both timing_profiles[] entries were committed");
    TEST_CHECK(strcmp(s_timing_profiles[0].name, "Slow") == 0, "profile 0 name round-trips");
    TEST_CHECK(strcmp(s_timing_profiles[1].name, "Fast") == 0, "profile 1 name round-trips");
    TEST_CHECK_NEAR(s_timing_profiles[1].progress_duty_min, 0.5, 1e-6, "profile 1 progress_duty_min round-trips");
    TEST_CHECK(s_writes[0].set_timing_profile_index_called && s_writes[0].timing_profile_index == 1,
              "zone 0's timing_profile index (1) is restored once the bundle it points into is committed");

    // Negative half of the same scenario: an index that overflows the
    // bundle actually present in the SAME backup must reject the whole
    // import, not silently clamp or skip.
    reset_stub_state();
    const char *bad_body =
        "{\"kind\":\"kilnctl_backup\",\"version\":4,\"profiles\":[],"
        "\"timing_profiles\":["
        "{\"name\":\"Slow\",\"progress_duty_min\":0.1,\"progress_window_s\":60,"
        "\"drift_hysteresis_c\":1.5,\"frozen_eps_c\":0.2,\"cross_zone_period_s\":30,"
        "\"bangbang_hysteresis_c\":2.0,\"cooling_limited_margin_c\":5.0,"
        "\"cooling_limited_hold_s\":120,\"ramp_lock_band_c\":25}],"
        "\"zones\":["
        "{\"index\":0,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,\"timing_profile\":1}]}";
    char err2[256];
    bool ok2 = test_backup_import_apply(bad_body, err2, sizeof(err2));
    TEST_CHECK(!ok2, "a timing_profile index (1) that overflows a NON-empty 1-entry bundle must be refused");
    TEST_CHECK(!s_writes[0].set_timing_profile_index_called, "the refused import must not have written anything");
}

// ---------------------------------------------------------------------------
// coupling_tau_c%u/coupling_dead_time_c%u "omit preserves" merge (backup_
// http.c ~1519-1553, ZONES_CFG_VERSION 11->12). Untested before this: every
// set_coupling_cell() seed in the tests above uses (0.0f, 0.0f) for tau/dead
// time, so a total failure to carry either field -- including a transposed
// (zone, neighbor) index -- would still read back as 0 and pass. These use
// NONZERO, ASYMMETRIC per-pair values (pair (0,1) != pair (1,0)) so a
// transpose or index bug fails loudly instead of coincidentally matching.
// ---------------------------------------------------------------------------

static void test_v4_coupling_tau_dead_time_round_trip_asymmetric_per_pair(void)
{
    TEST_SECTION("backup_import_apply -- coupling_tau_c%u/coupling_dead_time_c%u round-trip "
                 "exactly, per cell, with pair (0,1) DISTINCT from pair (1,0) so a transposed "
                 "index would fail rather than coincidentally match");
    reset_stub_state();

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":4,\"profiles\":[],"
        "\"zones\":["
        "{\"index\":0,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,"
        "\"coupling_c1\":5.0,\"coupling_tau_c1\":120.5,\"coupling_dead_time_c1\":30.5},"
        "{\"index\":1,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,"
        "\"coupling_c0\":7.0,\"coupling_tau_c0\":450.0,\"coupling_dead_time_c0\":95.0}]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(ok, "a well-formed coupling_tau_c%u/coupling_dead_time_c%u entry must import");
    TEST_CHECK(s_writes[0].set_coupling_cell_called[1], "zone 0's cell 1 (pair (0,1)) was committed");
    TEST_CHECK_NEAR(s_writes[0].coupling_coeff[1], 5.0, 1e-6, "pair (0,1) coeff round-trips");
    TEST_CHECK_NEAR(s_writes[0].coupling_tau_s[1], 120.5, 1e-6, "pair (0,1) tau_s round-trips");
    TEST_CHECK_NEAR(s_writes[0].coupling_dead_time_s[1], 30.5, 1e-6, "pair (0,1) dead_time_s round-trips");
    TEST_CHECK(s_writes[1].set_coupling_cell_called[0], "zone 1's cell 0 (pair (1,0)) was committed");
    TEST_CHECK_NEAR(s_writes[1].coupling_coeff[0], 7.0, 1e-6, "pair (1,0) coeff round-trips, DISTINCT from pair (0,1)");
    TEST_CHECK_NEAR(s_writes[1].coupling_tau_s[0], 450.0, 1e-6,
                    "pair (1,0) tau_s round-trips as 450.0, NOT pair (0,1)'s 120.5 -- proves "
                    "orientation isn't swapped");
    TEST_CHECK_NEAR(s_writes[1].coupling_dead_time_s[0], 95.0, 1e-6,
                    "pair (1,0) dead_time_s round-trips as 95.0, NOT pair (0,1)'s 30.5");
}

// 2026-10-05 bench finding: backup_export, backup_import, backup_export again
// gave a different kiln_configs package (7 float bytes in the coupling
// tau/dead-time area plus the CRC, so pkg_hash moved too) although every other
// section matched. Export printed the identified (non-round) plant values with
// %.1f/%.2f, import wrote the rounded text back into the live zones, and the
// active kiln config slot is autosaved from live zones. Same class as the
// 2026-09-28 pid %.9g fix: the fields must be exported at round-trip precision.
// Asserts the second export is byte-identical to the first and that the live
// floats are bit-exact after import.
//
// NOTE: the final strcmp alone would NOT catch the regression: rounded text
// re-exports as the same rounded text, so two exports of a rounded value are
// byte-identical even though the live float changed. The memcmp assertions on
// the live values after import are what fail on a lossy export format; the
// strcmp only guards the whole-document shape. Review round 1 (2026-10-05)
// extended the field set to max_ramp_c_per_hr (autotune writes it), the four
// tuning_* floats, model tau/dead_time and the other user-entered floats.
static void test_export_import_export_is_byte_identical_for_non_round_floats(void)
{
    TEST_SECTION("backup export -> import -> export is byte-identical with non-round identified floats "
                 "(coupling tau/dead_time, model_fit, coil_power, autotune_baseline_k_dc)");
    reset_stub_state();

    const float tau = 123.456789f, dead = 45.678901f, fit_t = 632.7435f, fit_a = 110.78753f;
    const float coil = 2750.4567f, base = 1.23456789f;
    TEST_CHECK(zones_config_set_pid(1, 5.0f, 0.6f, 0.02f), "seed zone 1 pid");
    TEST_CHECK(zones_config_set_coupling_cell(1, 0, 10.5f, tau, dead), "seed zone 1 coupling cell (1,0)");
    TEST_CHECK(zones_config_set_model_fit_context(1, fit_t, fit_a), "seed zone 1 model_fit context");
    TEST_CHECK(zones_config_set_coil_power_w(1, coil), "seed zone 1 coil_power_w");
    TEST_CHECK(zones_config_set_autotune_baseline_k_dc(1, base), "seed zone 1 autotune_baseline_k_dc");
    const float m_k = 0.123456789f, m_tau = 987.654321f, m_dead = 33.333333f, ramp = 187.654321f;
    /* export's model/max_ramp getters are the test_profile_feasibility.c hooks, not the setters */
    test_stub_zones_set_model(1, m_k, m_tau, m_dead);
    test_stub_zones_set_max_ramp(1, true, ramp);
    zone_tuning_quality_t tq;
    memset(&tq, 0, sizeof(tq));
    tq.valid = true;
    tq.method = 1;
    tq.rule = 2;
    tq.settled = true;
    tq.tau_consistent = true;
    tq.baseline_c = 24.567891f;
    tq.step_ambient_c = 23.123457f;
    tq.raw_rise_c = 88.123459f;
    tq.rise_inf_c = 95.987654f;
    TEST_CHECK(zones_config_set_tuning_quality(1, &tq), "seed zone 1 tuning_quality (valid=1, non-round floats)");

    TEST_CHECK(run_export() == ESP_OK, "first export succeeds");
    TEST_CHECK(s_export_body != NULL && s_export_len > 0, "first export produced a body");
    char *first = s_export_body ? strdup(s_export_body) : NULL;
    TEST_CHECK(first != NULL, "copy first export");
    if (!first) {
        return;
    }

    /* Poison the stub's live values (reset_stub_state clears s_writes[], which
     * backs these getters), then restore from the first export. */
    reset_stub_state();
    /* reset_stub_state() does not clear the tuning-record storage (it lives in
     * test_profile_feasibility.c), so poison it explicitly: otherwise import's
     * "file matches live" branch would reinstate the seeded record and this
     * part of the test would pass vacuously. */
    zone_tuning_quality_t poison;
    memset(&poison, 0, sizeof(poison));
    poison.baseline_c = 1.0f;
    poison.step_ambient_c = 2.0f;
    poison.raw_rise_c = 3.0f;
    poison.rise_inf_c = 4.0f;
    test_stub_zones_set_full_tuning_quality(1, &poison);
    test_stub_zones_set_model(1, 1.0f, 2.0f, 3.0f);
    test_stub_zones_set_max_ramp(1, true, 4.0f);
    char err[256];
    bool ok = test_backup_import_apply(first, err, sizeof(err));
    TEST_CHECK(ok, "importing the first export must succeed");

    TEST_CHECK(memcmp(&s_writes[1].coupling_tau_s[0], &tau, sizeof(float)) == 0,
              "coupling tau is bit-exact after import (123.456789, not 123.5)");
    TEST_CHECK(memcmp(&s_writes[1].coupling_dead_time_s[0], &dead, sizeof(float)) == 0,
              "coupling dead_time is bit-exact after import");
    TEST_CHECK(memcmp(&s_writes[1].model_fit_temp_c, &fit_t, sizeof(float)) == 0, "model_fit_temp_c bit-exact");
    TEST_CHECK(memcmp(&s_writes[1].model_fit_ambient_c, &fit_a, sizeof(float)) == 0, "model_fit_ambient_c bit-exact");
    TEST_CHECK(memcmp(&s_writes[1].coil_power_w, &coil, sizeof(float)) == 0, "coil_power_w bit-exact");
    TEST_CHECK(memcmp(&s_writes[1].autotune_baseline_k_dc, &base, sizeof(float)) == 0,
              "autotune_baseline_k_dc bit-exact");
    TEST_CHECK(memcmp(&s_writes[1].k_dc, &m_k, sizeof(float)) == 0, "model_k_dc bit-exact");
    TEST_CHECK(memcmp(&s_writes[1].tau_s, &m_tau, sizeof(float)) == 0, "model_tau_s bit-exact");
    TEST_CHECK(memcmp(&s_writes[1].dead_time_s, &m_dead, sizeof(float)) == 0, "model_dead_time_s bit-exact");
    TEST_CHECK(memcmp(&s_writes[1].max_ramp_c_per_hr, &ramp, sizeof(float)) == 0, "max_ramp_c_per_hr bit-exact");
    TEST_CHECK(s_writes[1].set_tuning_quality_called && s_writes[1].tuning_quality.valid,
              "import wrote a valid tuning record over the poisoned one");
    const zone_tuning_quality_t tq_after = s_writes[1].tuning_quality;
    TEST_CHECK(memcmp(&tq_after.baseline_c, &tq.baseline_c, sizeof(float)) == 0, "tuning_baseline_c bit-exact");
    TEST_CHECK(memcmp(&tq_after.step_ambient_c, &tq.step_ambient_c, sizeof(float)) == 0,
              "tuning_step_ambient_c bit-exact");
    TEST_CHECK(memcmp(&tq_after.raw_rise_c, &tq.raw_rise_c, sizeof(float)) == 0, "tuning_raw_rise_c bit-exact");
    TEST_CHECK(memcmp(&tq_after.rise_inf_c, &tq.rise_inf_c, sizeof(float)) == 0, "tuning_rise_inf_c bit-exact");

    /* The model/max_ramp export getters are fixed stub hooks, not backed by
     * the *_no_save setters import calls; their import was proven bit-exact
     * above, so point the hooks at that same state before re-exporting. */
    test_stub_zones_set_model(1, s_writes[1].k_dc, s_writes[1].tau_s, s_writes[1].dead_time_s);
    test_stub_zones_set_max_ramp(1, true, s_writes[1].max_ramp_c_per_hr);
    TEST_CHECK(run_export() == ESP_OK, "second export succeeds");
    TEST_CHECK(s_export_body != NULL && strcmp(first, s_export_body) == 0,
              "export, import, export is byte-identical");
    free(first);
}

static void test_v4_coupling_tau_dead_time_omitted_entirely_preserves_measured_values(void)
{
    TEST_SECTION("backup_import_apply -- an old-format backup that omits coupling_tau_c%u/"
                 "coupling_dead_time_c%u entirely (only coupling_c%u present) PRESERVES the "
                 "already-stored tau/dead_time for that cell instead of zeroing them -- the exact "
                 "claim the commit message makes about not silently wiping what autotune measured");
    reset_stub_state();

    /* Seed the "already measured" live values for zone 0's cell 1 directly
     * through the real setter, standing in for a prior autotune run. */
    TEST_CHECK(zones_config_set_coupling_cell(0, 1, 2.0f, 111.0f, 22.0f),
              "seed zone 0 cell 1 with a prior autotune-measured coeff/tau/dead_time");
    g_total_write_calls = 0;

    /* Old-format body: coupling_c1 present (a manual coefficient tweak), but
     * NEITHER coupling_tau_c1 NOR coupling_dead_time_c1 -- exactly what any
     * backup exported before ZONES_CFG_VERSION 12 looks like. */
    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":4,\"profiles\":[],"
        "\"zones\":[{\"index\":0,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,\"coupling_c1\":9.0}]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(ok, "an old-format entry with only coupling_c%u must still import");
    TEST_CHECK(s_writes[0].set_coupling_cell_called[1],
              "cell 1 was still committed -- coupling_c1 alone is enough to trigger the merge");
    TEST_CHECK_NEAR(s_writes[0].coupling_coeff[1], 9.0, 1e-6, "the supplied coeff was updated");
    TEST_CHECK_NEAR(s_writes[0].coupling_tau_s[1], 111.0, 1e-6,
                    "tau_s was PRESERVED at the previously-measured 111.0, not zeroed");
    TEST_CHECK_NEAR(s_writes[0].coupling_dead_time_s[1], 22.0, 1e-6,
                    "dead_time_s was PRESERVED at the previously-measured 22.0, not zeroed");
}

static void test_v4_coupling_tau_dead_time_partial_per_cell_presence(void)
{
    TEST_SECTION("backup_import_apply -- with two cells in one zone entry, the cell whose keys "
                 "are present updates and the cell whose keys are absent preserves its stored "
                 "value -- per-cell presence, not per-zone");
    reset_stub_state();

    /* Seed prior "measured" values in BOTH cells zone 2 can neighbor. */
    TEST_CHECK(zones_config_set_coupling_cell(2, 0, 1.0f, 60.0f, 5.0f), "seed zone 2 cell 0");
    TEST_CHECK(zones_config_set_coupling_cell(2, 1, 3.0f, 70.0f, 6.0f), "seed zone 2 cell 1");
    g_total_write_calls = 0;
    /* Clear the "was committed" latches the seeding above itself set, so the
     * assertions below observe only what THIS import does. */
    memset(s_writes[2].set_coupling_cell_called, 0, sizeof(s_writes[2].set_coupling_cell_called));

    /* Cell 0 gets a full new triple in this import; cell 1 is entirely
     * absent from the body. */
    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":4,\"profiles\":[],"
        "\"zones\":[{\"index\":2,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0,"
        "\"coupling_c0\":8.5,\"coupling_tau_c0\":200.0,\"coupling_dead_time_c0\":40.0}]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(ok, "a partial-cell entry must import");
    TEST_CHECK(s_writes[2].set_coupling_cell_called[0], "cell 0 (keys present) was committed");
    TEST_CHECK_NEAR(s_writes[2].coupling_coeff[0], 8.5, 1e-6, "cell 0's coeff updated");
    TEST_CHECK_NEAR(s_writes[2].coupling_tau_s[0], 200.0, 1e-6, "cell 0's tau_s updated");
    TEST_CHECK_NEAR(s_writes[2].coupling_dead_time_s[0], 40.0, 1e-6, "cell 0's dead_time_s updated");
    TEST_CHECK(!s_writes[2].set_coupling_cell_called[1],
              "cell 1 (no keys at all present in this entry) was NOT touched -- absent cells "
              "preserve by never being committed again");
}

// ---------------------------------------------------------------------------
// 2026-09-16 backup-round-trip-gap closure. Owner's own words: "shouldn't ct
// normals be part of a config backup? I dont want to have to recalibrate my
// kiln after a restore." Before this pass, backup_export.c never read
// zones_config_get_normal_current() (or relay_type/coil_power_w/
// adaptive_tune_enabled/tuning_quality/etc.) at all -- a restore silently
// dropped the CT calibration and every other field below, landing back on
// firmware defaults. Proves the CLOSED fields round-trip end to end, the
// same "export -> feed into import against a poisoned starting state ->
// assert" shape as test_export_round_trips_through_import_to_identical_
// config() above, but isolated to the fields this pass added so a parent
// (pre-fix) build fails this test with an honest value mismatch rather than
// a crash: on parent code, zones_config_get_normal_current()/
// zones_config_get_relay_type()/etc. are never called by export, so none of
// these keys appear in the exported JSON, so import's has_* flags never
// fire, so every assertion below (checking the POISONED value was
// overwritten) fails on a real, wrong number -- not a build or link error.
// ---------------------------------------------------------------------------
static void test_ct_normals_and_new_fields_round_trip_through_export_import(void)
{
    TEST_SECTION("backup_export_get_handler -> backup_import_apply -- CT normal_current_a "
                 "(the owner's own named example) and the other 2026-09-16 closed fields "
                 "(relay_type, coil_power_w, adaptive_tune_enabled, tuning_quality) all "
                 "round-trip through a real export/import cycle");
    reset_stub_state();

    zones_config_set_pid(1, 1.0f, 0.0f, 0.0f); /* zone 1 must answer pid_kp for export to visit it at all */
    TEST_CHECK(zone_normals_set(1, 12.345f), "seed zone 1's measured CT normal current");
    TEST_CHECK(zones_config_set_relay_type(1, 2), "seed zone 1 relay_type (2 = mercury)");
    TEST_CHECK(zones_config_set_coil_power_w(1, 2750.5f), "seed zone 1 coil_power_w");
    TEST_CHECK(zones_config_set_adaptive_tune_enabled(1, true), "seed zone 1 adaptive_tune_enabled");

    zone_tuning_quality_t tq;
    memset(&tq, 0, sizeof(tq));
    tq.valid = true;
    tq.method = 1;
    tq.rule = 2;
    tq.settled = true;
    tq.extrapolation_converged = false;
    tq.tau_consistent = true;
    tq.baseline_c = 24.5f;
    tq.step_ambient_c = 23.1f;
    tq.raw_rise_c = 88.0f;
    tq.rise_inf_c = 95.5f;
    TEST_CHECK(zones_config_set_tuning_quality(1, &tq), "seed zone 1 tuning_quality");

    esp_err_t err = run_export();
    TEST_CHECK(err == ESP_OK, "export must succeed");
    TEST_CHECK(s_export_body != NULL && s_export_len > 0, "export must have produced a body");
    TEST_CHECK(strstr(s_export_body, "\"normal_current_a\":12.3450") != NULL,
              "the measured CT normal current is actually emitted by export");
    TEST_CHECK(strstr(s_export_body, "\"relay_type\":2") != NULL, "relay_type is emitted");
    TEST_CHECK(strstr(s_export_body, "\"coil_power_w\":2750.5,") != NULL, "coil_power_w is emitted");
    TEST_CHECK(strstr(s_export_body, "\"adaptive_tune_enabled\":1") != NULL, "adaptive_tune_enabled is emitted");
    TEST_CHECK(strstr(s_export_body, "\"tuning_valid\":1") != NULL, "tuning_quality block is emitted");

    /* Reset to a DIFFERENT (poisoned) state, distinct from every seeded value
     * above, so a re-import that silently no-ops (the parent-code failure
     * mode) leaves these fields readably WRONG rather than coincidentally
     * matching. */
    reset_stub_state();
    zones_config_set_pid(1, 1.0f, 0.0f, 0.0f);
    zones_config_set_relay_type(1, 0);
    zones_config_set_coil_power_w(1, 0.0f);
    /* reset_stub_state() clears s_writes[] but not the live getter's storage (it
     * lives in test_profile_feasibility.c), so poison it too: the live tuning
     * record must read invalid, else import (rightly) skips an identical record. */
    {
        zone_tuning_quality_t none;
        memset(&none, 0, sizeof(none));
        test_stub_zones_set_full_tuning_quality(1, &none);
    }
    /* normal_current_a/adaptive_tune_enabled/tuning_quality: reset_stub_state()
     * already left these at "not measured"/false/"not called" -- the poison
     * for these three IS the untouched default, since that is exactly the
     * firmware-default state the owner's complaint says a restore must not
     * silently fall back to. */
    g_total_write_calls = 0;

    char import_err[256];
    bool ok = test_backup_import_apply(s_export_body, import_err, sizeof(import_err));
    TEST_CHECK(ok, "re-importing exactly what was just exported must succeed");

    TEST_CHECK(s_writes[1].set_normal_current_called,
              "CT normal current was actually restored by import (owner's own named example)");
    TEST_CHECK_NEAR(s_writes[1].normal_current_a, 12.345, 1e-3,
                    "the restored CT normal current matches the pre-backup measured value exactly -- "
                    "no recalibration required after a restore");
    TEST_CHECK(s_writes[1].set_relay_type_called && s_writes[1].relay_type == 2,
              "relay_type was restored, not left at the poisoned 0 (SSR)");
    TEST_CHECK(s_writes[1].set_coil_power_w_called, "coil_power_w setter ran");
    TEST_CHECK_NEAR(s_writes[1].coil_power_w, 2750.5, 1e-2, "coil_power_w restored exactly");
    TEST_CHECK(s_writes[1].set_adaptive_tune_enabled_called && s_writes[1].adaptive_tune_enabled,
              "adaptive_tune_enabled was restored true, not left at the poisoned/default false");
    TEST_CHECK(s_writes[1].set_tuning_quality_called && s_writes[1].tuning_quality.valid,
              "tuning_quality was restored, not left unset");
    TEST_CHECK_NEAR(s_writes[1].tuning_quality.baseline_c, 24.5, 1e-3, "tuning_quality.baseline_c restored exactly");
    TEST_CHECK_NEAR(s_writes[1].tuning_quality.rise_inf_c, 95.5, 1e-3, "tuning_quality.rise_inf_c restored exactly");
    TEST_CHECK(s_writes[1].tuning_quality.method == 1 && s_writes[1].tuning_quality.rule == 2,
              "tuning_quality.method/rule restored exactly");
}

// ---------------------------------------------------------------------------
// 2026-10-04 bench finding: zones_config_set_tuning_quality_no_save() always
// bumps the zone's tuning_seq, which is part of the kiln package canonical
// bytes / pkg_hash but never exported/imported. Import used to re-commit the
// record unconditionally, so restoring an UNCHANGED backup changed the active
// kiln_config's pkg_hash and a later re-import made a duplicate slot instead
// of the Case 1 no-op. The stub setter above models the unconditional bump
// (s_writes[].tuning_seq); the seq standing still is the observable proxy for
// the pkg_hash standing still.
// ---------------------------------------------------------------------------
static void test_import_of_identical_tuning_quality_does_not_bump_seq(void)
{
    TEST_SECTION("backup_import_apply -- an identical tuning_quality record is not re-committed "
                 "(tuning_seq/pkg_hash stable); a differing one still commits and bumps");
    reset_stub_state();

    zones_config_set_pid(1, 1.0f, 0.0f, 0.0f);
    zone_tuning_quality_t tq;
    memset(&tq, 0, sizeof(tq));
    tq.valid = true;
    tq.method = 1;
    tq.rule = 2;
    tq.settled = true;
    tq.tau_consistent = true;
    tq.baseline_c = 24.53719f; /* a real autotune-style value: NOT representable in the export's 3 decimals */
    tq.step_ambient_c = 23.1f;
    tq.raw_rise_c = 88.0f;
    tq.rise_inf_c = 95.5f;
    TEST_CHECK(zones_config_set_tuning_quality(1, &tq), "seed zone 1 tuning_quality");
    TEST_CHECK(s_writes[1].tuning_seq == 1, "seeding bumps the seq once");

    esp_err_t err = run_export();
    TEST_CHECK(err == ESP_OK && s_export_body != NULL, "export must succeed");
    TEST_CHECK(strstr(s_export_body, "\"tuning_valid\":1") != NULL, "tuning_quality block is emitted");

    char import_err[256];
    TEST_CHECK(test_backup_import_apply(s_export_body, import_err, sizeof(import_err)),
              "importing the unchanged export must succeed");
    TEST_CHECK(s_writes[1].tuning_seq == 1,
              "identical record: tuning_seq unchanged (pkg_hash would be unchanged)");
    TEST_CHECK(s_writes[1].tuning_quality.baseline_c == 24.53719f,
              "identical record: live baseline_c not rounded to the export precision");

    /* Again, to prove it is stable across repeated restores, not just one. */
    TEST_CHECK(test_backup_import_apply(s_export_body, import_err, sizeof(import_err)),
              "second import of the unchanged export must succeed");
    TEST_CHECK(s_writes[1].tuning_seq == 1, "identical record: tuning_seq still unchanged after a second import");

    /* Differing live record: import must still commit the file's values. */
    zone_tuning_quality_t other = tq;
    other.baseline_c = 30.0f;
    TEST_CHECK(zones_config_set_tuning_quality(1, &other), "diverge the live record from the file");
    TEST_CHECK(s_writes[1].tuning_seq == 2, "diverging write bumped the seq");
    TEST_CHECK(test_backup_import_apply(s_export_body, import_err, sizeof(import_err)),
              "importing over a differing live record must succeed");
    TEST_CHECK(s_writes[1].tuning_seq == 3, "differing record: import commits and bumps the seq");
    TEST_CHECK_NEAR(s_writes[1].tuning_quality.baseline_c, 24.537, 1e-3,
                    "differing record: the file's value was restored");
}

static void test_import_refuses_duplicate_zone_index(void)
{
    TEST_SECTION("backup_import_apply -- two zone tuning entries with the same index are refused in pass 1, "
                 "nothing committed");
    reset_stub_state();
    zones_config_set_pid(1, 1.0f, 0.0f, 0.0f);
    esp_err_t err = run_export();
    TEST_CHECK(err == ESP_OK && s_export_body != NULL, "export must succeed");
    const char *first = strstr(s_export_body, "{\"index\":1,");
    TEST_CHECK(first != NULL, "setup: the export has a zone 1 entry");
    if (!first) {
        return;
    }
    size_t head = (size_t)(first - s_export_body);
    const char *dup = "{\"index\":1,\"pid_kp\":1,\"pid_ki\":0,\"pid_kd\":0},";
    size_t total = strlen(s_export_body) + strlen(dup) + 1;
    char *body = malloc(total);
    TEST_CHECK(body != NULL, "setup: allocate the doctored body");
    if (!body) {
        return;
    }
    memcpy(body, s_export_body, head);
    strcpy(body + head, dup);
    strcpy(body + head + strlen(dup), first);
    char import_err[256] = "";
    bool ok = test_backup_import_apply(body, import_err, sizeof(import_err));
    TEST_CHECK(!ok, "a duplicate zone index is refused");
    TEST_CHECK(strstr(import_err, "duplicate index") != NULL, "the error names the duplicate index");
    free(body);
}

// ---------------------------------------------------------------------------
// 2026-10-05 bench round trip (kilnctl_backup_20261005T071413Z.json ->
// import -> ...071420Z.json): the fix above was inert on hardware, zones 1 and
// 2's tuning_seq still went 13 -> 14. Cause: the commit loop calls
// zones_config_set_pid_no_save() (which sets tuning_valid = 0 on every gain
// change) BEFORE the tuning compare, so the compare always saw an invalid live
// record. The stubs now model that invalidation; this test uses the two real
// zones' tuning blocks from that bench file, with live values at full float
// precision, and requires seq unchanged AND the record valid afterwards.
// ---------------------------------------------------------------------------
static void test_import_identity_roundtrip_keeps_tuning_seq_and_validity(void)
{
    TEST_SECTION("backup_import_apply -- identity round trip with the bench's real tuning blocks "
                 "(zones 1 and 2): tuning_seq unchanged, record still valid after set_pid's invalidation");
    reset_stub_state();

    static const struct { float base, amb, raw, inf; } bench[2] = {
        { 36.3667f, 37.0f, 15.5333f, 16.1982f },
        { 35.3281f, 36.1562f, 16.5719f, 16.9253f },
    };
    for (uint8_t zi = 1; zi <= 2; zi++) {
        zone_tuning_quality_t q;
        memset(&q, 0, sizeof(q));
        q.valid = true;
        q.method = 0;
        q.rule = 0;
        q.settled = true;
        q.extrapolation_converged = true;
        q.tau_consistent = true;
        q.baseline_c = bench[zi - 1].base;
        q.step_ambient_c = bench[zi - 1].amb;
        q.raw_rise_c = bench[zi - 1].raw;
        q.rise_inf_c = bench[zi - 1].inf;
        zones_config_set_pid(zi, 1.0f, 0.0f, 0.0f);
        TEST_CHECK(zones_config_set_tuning_quality(zi, &q), "seed tuning record");
    }
    zones_config_set_pid(0, 1.0f, 0.0f, 0.0f);
    for (uint8_t zi = 1; zi <= 2; zi++) {
        /* seeded after set_pid so the record is valid going into the export */
        TEST_CHECK(s_writes[zi].tuning_seq == 1, "seeded once");
    }

    esp_err_t err = run_export();
    TEST_CHECK(err == ESP_OK && s_export_body != NULL, "export must succeed");
    char import_err[256];
    TEST_CHECK(test_backup_import_apply(s_export_body, import_err, sizeof(import_err)),
              "importing the unchanged export must succeed");
    for (uint8_t zi = 1; zi <= 2; zi++) {
        zone_tuning_quality_t after;
        TEST_CHECK(zones_config_get_tuning_quality(zi, &after) && after.valid,
                  "tuning record is valid again after the restore (set_pid invalidated it mid-commit)");
        TEST_CHECK(s_writes[zi].tuning_seq == 1, "tuning_seq did not move on an identity restore");
        TEST_CHECK(after.baseline_c == bench[zi - 1].base, "live float not rewritten at export precision");
    }
}

// ---------------------------------------------------------------------------
// 2026-09-16, closing the LAST owner-relevant backup gap: the Pico's OWN
// i_normal_a[0..2] (0x031A-0x031C, S14/S15's arming baseline) was never
// covered by test_ct_normals_and_new_fields_round_trip_through_export_import()
// above -- that test closed the ESP-side normal_current_a, not the safety
// processor's separate copy. Four tests below, same "prove the CLOSED gap,
// a parent build fails on an honest value/behavior mismatch" shape:
//   1) export omits the key entirely when the Pico value is unset (the real
//      bench's actual state today -- ct_cal reports has_value:false on all 3
//      channels), matching the "skip unmeasured, never emit a false 0.0"
//      convention normal_current_a/model_k_dc already follow.
//   2) a set value round-trips end to end THROUGH THE REAL WRITE PATH: export
//      emits it, import builds the correct (param_id, value_text) pair and
//      calls safety_cfg_write_apply_pairs() -- the same stage/COMMIT_CONFIG/
//      forced-read-back-confirm machinery every other Pico config write uses.
//   3) a refused/unconfirmed write (stub reports false, as the real function
//      does on an ARMED refusal or a read-back mismatch) aborts the WHOLE
//      import loudly -- backup_import_apply() returns false, the reason
//      reaches the caller, and NO zone tuning from the same body is
//      committed either, since the safety write happens before the per-zone
//      commit loop.
//   4) a backup with no safety_i_normal_a key at all (old backup, or a
//      source board where this channel was never measured) causes ZERO
//      UART traffic -- safety_cfg_write_apply_pairs() must not even be
//      called -- so the target board's own existing arming baseline is left
//      untouched ("never de-arm on omission").
// ---------------------------------------------------------------------------
static void test_safety_i_normal_a_export_omitted_when_unset(void)
{
    TEST_SECTION("backup_export_get_handler -- safety_i_normal_a is OMITTED when the Pico's "
                 "i_normal_a is unset (the real bench's actual state today)");
    reset_stub_state();
    zones_config_set_pid(0, 1.0f, 0.0f, 0.0f);
    /* s_pico_i_normal_a_known[0] stays false from reset_stub_state() -- unset. */

    esp_err_t err = run_export();
    TEST_CHECK(err == ESP_OK, "export must succeed");
    TEST_CHECK(s_export_body != NULL && strstr(s_export_body, "safety_i_normal_a") == NULL,
              "the key is never emitted for an unmeasured channel");
}

static void test_safety_i_normal_a_round_trips_through_export_import(void)
{
    TEST_SECTION("backup_export_get_handler -> backup_import_apply -- the Pico's OWN "
                 "i_normal_a[zi] (S14/S15's arming baseline, 0x031A-0x031C) round-trips through "
                 "a real export/import cycle, via the real safety_cfg_write_apply_pairs() staging");
    reset_stub_state();
    zones_config_set_pid(1, 1.0f, 0.0f, 0.0f); /* zone 1 must answer pid_kp for export to visit it */
    s_pico_i_normal_a_known[1] = true;
    s_pico_i_normal_a[1] = 7.6543f;

    esp_err_t err = run_export();
    TEST_CHECK(err == ESP_OK, "export must succeed");
    TEST_CHECK(strstr(s_export_body, "\"safety_i_normal_a\":7.6543") != NULL,
              "the Pico's measured i_normal_a is actually emitted by export");

    reset_stub_state();
    zones_config_set_pid(1, 1.0f, 0.0f, 0.0f);
    g_total_write_calls = 0;

    char import_err[256];
    bool ok = test_backup_import_apply(s_export_body, import_err, sizeof(import_err));
    TEST_CHECK(ok, "re-importing exactly what was just exported must succeed");
    TEST_CHECK(g_stub_safety_cfg_write_n_pairs == 1,
              "safety_cfg_write_apply_pairs() was called with exactly one pair");
    if (g_stub_safety_cfg_write_n_pairs == 1) {
        TEST_CHECK(g_stub_safety_cfg_write_pairs[0].param_id == (uint16_t)0x031Bu,
                  "the pair targets i_normal_a[1] (0x031A + zone index 1 = 0x031B), never abs_max_temp_c");
        float parsed = strtof(g_stub_safety_cfg_write_pairs[0].value_text, NULL);
        TEST_CHECK_NEAR(parsed, 7.6543, 1e-3, "the pair's value_text encodes the exact restored value");
    }
    TEST_CHECK(g_stub_safety_cfg_write_commit_arg, "the write commits (persists to flash), not a volatile-only install");
}

static void test_safety_i_normal_a_write_failure_aborts_whole_import(void)
{
    TEST_SECTION("backup_import_apply -- safety_cfg_write_apply_pairs() refusing/failing to confirm "
                 "the i_normal_a write aborts the WHOLE import loudly -- no partial apply, and the "
                 "same body's ordinary zone tuning is NOT committed either");
    reset_stub_state();
    zones_config_set_pid(1, 1.0f, 0.0f, 0.0f);
    g_total_write_calls = 0;
    g_stub_safety_cfg_write_result = false;
    strncpy(g_stub_safety_cfg_write_reason, "refused: relay ARMED", sizeof(g_stub_safety_cfg_write_reason) - 1);

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":4,\"profiles\":[],"
        "\"zones\":[{\"index\":1,\"pid_kp\":9.0,\"pid_ki\":0,\"pid_kd\":0,\"safety_i_normal_a\":3.0}]}";
    char err[256];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(!ok, "the import must fail loudly, not complete quietly, when the Pico write is unconfirmed");
    TEST_CHECK(strstr(err, "relay ARMED") != NULL || strstr(err, "refused") != NULL || strstr(err, "unconfirmed") != NULL,
              "the refusal reason reaches the caller, not a generic message");
    TEST_CHECK(g_total_write_calls == 0,
              "NOTHING was committed -- the same body's zone 1 pid_kp=9.0 must not have landed either, "
              "since the safety write happens before the per-zone commit loop");
}

static void test_safety_i_normal_a_absent_key_never_calls_write(void)
{
    TEST_SECTION("backup_import_apply -- a backup with NO safety_i_normal_a key at all makes ZERO calls "
                 "to safety_cfg_write_apply_pairs() -- never de-arm on omission");
    reset_stub_state();
    zones_config_set_pid(0, 1.0f, 0.0f, 0.0f);
    g_total_write_calls = 0;

    const char *body =
        "{\"kind\":\"kilnctl_backup\",\"version\":4,\"profiles\":[],"
        "\"zones\":[{\"index\":0,\"pid_kp\":2.0,\"pid_ki\":0,\"pid_kd\":0}]}";
    char err[160];
    bool ok = test_backup_import_apply(body, err, sizeof(err));

    TEST_CHECK(ok, "an ordinary backup with no safety_i_normal_a key still imports");
    TEST_CHECK(g_stub_safety_cfg_write_n_pairs == -1,
              "safety_cfg_write_apply_pairs() was never called -- no UART traffic to the Pico at all");
}

// ---------------------------------------------------------------------------
// docs/WEB_AUTH_PLAN.md item 12b ("Credentials versus the config package, OTA
// and factory reset"): credentials live in their own NVS namespace
// (`kiln_auth`, on the default `nvs` partition -- item 2) and no config
// operation reads or writes it. Exclusion from a backup/config export is one
// of the concrete case-by-case outcomes item 12b lists ("Config backup /
// export ... not in the file"), and its own acceptance criterion asks for a
// host test asserting "an export's JSON contains none of the three
// credential key names and none of a set password's bytes".
//
// Item 2's credential-storage module (web_auth_store.c) landed at c3008eb8,
// so this test now does the full acceptance shape docs/WEB_AUTH_PLAN.md item
// 12b asks for: seed a REAL, synthetic credential through
// web_auth_store_set_password()/_set_pin(), export a backup, and assert the
// ACTUAL stored hash+salt bytes (hex-encoded, the only form they could appear
// in a JSON export) are absent from the export body -- not merely that the
// kiln_auth/web_auth/lcd_auth/auth_policy identifier strings are absent. Both
// checks are kept: the identifier-string check still catches a future config
// path that references the namespace/keys by name without emitting bytes
// (e.g. a stray debug log line), and the byte check catches the shape this
// test was previously blocked on -- a hypothetical export path that reads the
// raw NVS blob without knowing/using its key name. backup_export.c's
// hand-written, no-generic-NVS-enumeration section list (verified by
// inspection -- zero nvs_entry_* calls in that file) is exactly the reason
// exclusion holds; this test exercises that real writer's real output against
// a real, hashed-and-salted credential.
// ---------------------------------------------------------------------------
static const uint8_t BACKUP12B_SALT[WEB_AUTH_SALT_LEN] = {
    0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8,
    0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF, 0xF0};
#define BACKUP12B_SYNTHETIC_PASSWORD "Synthetic-Backup-Secret-3"
#define BACKUP12B_SYNTHETIC_PIN "751046"

static void hex_encode(const uint8_t *bytes, size_t len, char *out, size_t out_cap)
{
    static const char hexdigits[] = "0123456789abcdef";
    TEST_CHECK(out_cap >= (len * 2u + 1u), "hex_encode: output buffer large enough");
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = hexdigits[(bytes[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hexdigits[bytes[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

static void test_export_never_contains_credential_markers(void)
{
    TEST_SECTION("backup_export_get_handler -- WEB_AUTH_PLAN.md item 12b: the emitted backup JSON "
                 "never contains the kiln_auth NVS namespace name, any of the three credential record "
                 "key names (web_auth/lcd_auth/auth_policy), the literal word 'password', or the actual "
                 "stored hash/salt bytes of a real seeded credential -- credentials live outside config "
                 "storage entirely and no config operation reads or writes them");
    reset_stub_state();
    fake_kv_reset_all();
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init the default nvs partition (kiln_auth's home)");
    TEST_CHECK(web_auth_store_set_password(WEB_AUTH_ROLE_ADMINISTRATOR, "backuptestuser",
                                            BACKUP12B_SYNTHETIC_PASSWORD, BACKUP12B_SALT, false) == HAL_OK,
              "setup: seed a synthetic administrator password");
    TEST_CHECK(web_auth_store_set_pin(WEB_AUTH_ROLE_USER, BACKUP12B_SYNTHETIC_PIN, BACKUP12B_SALT) == HAL_OK,
              "setup: seed a synthetic user LCD PIN");

    // Pull the actual stored blobs back out via the real loader (not a
    // separately-derived hash) so this test compares against exactly what a
    // leaking export path would actually be leaking.
    web_auth_password_record_t web_blob;
    TEST_CHECK(web_auth_store_load_password(WEB_AUTH_ROLE_ADMINISTRATOR, &web_blob) == WEB_AUTH_LOAD_OK,
              "setup: real stored password blob loads back");
    web_auth_pin_record_t lcd_blob;
    TEST_CHECK(web_auth_store_load_pin(WEB_AUTH_ROLE_USER, &lcd_blob) == WEB_AUTH_LOAD_OK,
              "setup: real stored PIN blob loads back");

    char web_hash_hex[WEB_AUTH_HASH_LEN * 2 + 1];
    char web_salt_hex[WEB_AUTH_SALT_LEN * 2 + 1];
    char lcd_hash_hex[WEB_AUTH_HASH_LEN * 2 + 1];
    char lcd_salt_hex[WEB_AUTH_SALT_LEN * 2 + 1];
    hex_encode(web_blob.hash, WEB_AUTH_HASH_LEN, web_hash_hex, sizeof(web_hash_hex));
    hex_encode(web_blob.salt, WEB_AUTH_SALT_LEN, web_salt_hex, sizeof(web_salt_hex));
    hex_encode(lcd_blob.hash, WEB_AUTH_HASH_LEN, lcd_hash_hex, sizeof(lcd_hash_hex));
    hex_encode(lcd_blob.salt, WEB_AUTH_SALT_LEN, lcd_salt_hex, sizeof(lcd_salt_hex));

    profile_t p;
    memset(&p, 0, sizeof(p));
    strncpy(p.name, "Cone6", PROFILE_NAME_MAX_LEN);
    p.zone_mask = 0x01;
    p.segment_count = 1;
    p.segments[0].target_c = 1000.0f;
    p.segments[0].ramp_c_per_hr = 100.0f;
    p.segments[0].dwell_min = 5;
    test_stub_profiles_set(0, &p);
    TEST_CHECK(zones_config_set_pid(0, 1.0f, 0.1f, 0.0f), "seed zone 0 pid");
    TEST_CHECK(zones_config_set_name(0, "Top"), "seed zone 0 name");

    esp_err_t err = run_export();
    TEST_CHECK(err == ESP_OK, "export must succeed");
    TEST_CHECK(s_export_body != NULL, "export produced a body");
    if (s_export_body != NULL) {
        TEST_CHECK(strstr(s_export_body, "kiln_auth") == NULL, "export never names the kiln_auth namespace");
        TEST_CHECK(strstr(s_export_body, "web_auth") == NULL, "export never names the web_auth key");
        TEST_CHECK(strstr(s_export_body, "lcd_auth") == NULL, "export never names the lcd_auth key");
        TEST_CHECK(strstr(s_export_body, "auth_policy") == NULL, "export never names the auth_policy key");
        TEST_CHECK(strstr(s_export_body, "password") == NULL, "export never contains the literal word 'password'");
        TEST_CHECK(strstr(s_export_body, web_hash_hex) == NULL,
                  "export never contains the real stored administrator password hash's bytes");
        TEST_CHECK(strstr(s_export_body, web_salt_hex) == NULL,
                  "export never contains the real stored administrator password salt's bytes");
        TEST_CHECK(strstr(s_export_body, lcd_hash_hex) == NULL,
                  "export never contains the real stored user PIN hash's bytes");
        TEST_CHECK(strstr(s_export_body, lcd_salt_hex) == NULL,
                  "export never contains the real stored user PIN salt's bytes");
    }
}

// ---------------------------------------------------------------------------
// bkfinish review, HIGH 2 / HIGH 3: MIRROR-mode kiln_configs[] delete-ack
// gate coverage, plus the active-slot-survives-mirror fix. These call
// backup_import_apply() DIRECTLY (not the test_backup_import_apply()
// wrapper above) since they need real mode/ack_delete_count control.
// kiln_cfg_store.c's state is cumulative across every test in this shared
// binary (see the comment on test_kiln_configs_absent_key_is_a_no_op()), so
// every count used below is derived from a live kiln_cfg_store_list() BEFORE
// snapshot, never a hardcoded number, and MERGE/no-op sections that don't
// need mirror deletion are left alone by tests running earlier in this file.
// ---------------------------------------------------------------------------

// kiln_cfg_store.c's KILN_CFG_MAX_COUNT (10) slots are shared, cumulative
// state across every test in this combined binary (see the comment on
// test_kiln_configs_absent_key_is_a_no_op()) -- by the time this file's own
// later tests run, earlier ones (here and in sibling test_*.c files linked
// into the same executable) can easily have already filled the store to
// capacity, leaving no room for a new test's own setup saves. Trim back to
// a small, known baseline (every non-active slot deleted -- the active one,
// if any, is left alone since delete refuses it anyway) before any test
// below that needs to CREATE new slots.
static void mirror_test_trim_to_baseline(void)
{
    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    uint8_t n = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    char reason[160] = {0};
    for (uint8_t i = 0; i < n; i++) {
        if (!rows[i].is_active) {
            TEST_CHECK(kiln_cfg_store_delete(rows[i].id, true, reason, sizeof(reason)),
                      "setup: trimmed a leftover non-active slot from an earlier test to make room");
        }
    }
}

// Common setup for the three ack-gate tests below: ensures at least one
// ACTIVE slot and at least one NON-active slot exist, and returns the
// current non-active count (the real MIRROR delete count against an empty
// kiln_configs[] body) via `nonactive_count_out`.
//
// Names are made unique per call (a static counter) rather than reused
// literal "Mirror Ack Marker A/B" strings: kiln_cfg_store_delete() refuses
// the ACTIVE slot unconditionally, so mirror_test_trim_to_baseline() (which
// only deletes non-active rows) cannot clean up a marker slot a PRIOR call
// left behind active -- a fixed name would then collide with that leftover
// on the next save_current() and fail setup itself, which is exactly the
// intermittent "setup: marker slot ... saved" failure this replaced.
static int s_mirror_ack_marker_seq = 0;
static void mirror_ack_test_setup(int32_t *nonactive_count_out)
{
    mirror_test_trim_to_baseline();
    char reason[160] = {0};
    int32_t id_a = -1, id_b = -1;
    int seq = ++s_mirror_ack_marker_seq;
    char name_a[KILN_CFG_NAME_MAX_LEN + 1];
    char name_b[KILN_CFG_NAME_MAX_LEN + 1];
    snprintf(name_a, sizeof(name_a), "Mirror Ack Marker A %d", seq);
    snprintf(name_b, sizeof(name_b), "Mirror Ack Marker B %d", seq);
    // save_current() makes its OWN save active, so saving A then B leaves A
    // non-active and B active -- guarantees at least one of each regardless
    // of what earlier tests left behind.
    TEST_CHECK(kiln_cfg_store_save_current(name_a, -1, &id_a, reason, sizeof(reason)),
              "setup: non-active marker slot A saved");
    TEST_CHECK(kiln_cfg_store_save_current(name_b, -1, &id_b, reason, sizeof(reason)),
              "setup: marker slot B saved (becomes active)");

    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    uint8_t n = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    int32_t nonactive = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (!rows[i].is_active) {
            nonactive++;
        }
    }
    *nonactive_count_out = nonactive;
}

static void test_mirror_delete_absent_ack_refuses_and_writes_nothing(void)
{
    TEST_SECTION("backup_import_apply -- MIRROR restore that would delete kiln config slots refuses "
                 "with NO X-Kiln-Config-Ack-Delete header (ack_delete_count -1) and writes nothing");
    reset_stub_state();
    int32_t nonactive_count = 0;
    mirror_ack_test_setup(&nonactive_count);
    TEST_CHECK(nonactive_count > 0, "test assumption: at least one non-active slot exists to be deleted");

    kiln_cfg_summary_t before[KILN_CFG_MAX_COUNT];
    uint8_t n_before = kiln_cfg_store_list(before, KILN_CFG_MAX_COUNT);

    const char *body = "{\"kind\":\"kilnctl_backup\",\"version\":5,\"profiles\":[],\"zones\":[],\"kiln_configs\":[]}";
    kiln_cfg_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    bool partial_write = false;
    char err[160] = {0};
    bool ok = backup_import_apply(body, KILN_CFG_RESTORE_MIRROR, false, -1, true, &plan, &partial_write, err,
                                  sizeof(err));

    TEST_CHECK(!ok, "MIRROR with slots to delete and no ack header must refuse");
    TEST_CHECK(!partial_write, "refused before any write -- not a partial write");
    TEST_CHECK(strstr(err, "would delete") != NULL, "the refusal names that it would delete slots");

    kiln_cfg_summary_t after[KILN_CFG_MAX_COUNT];
    uint8_t n_after = kiln_cfg_store_list(after, KILN_CFG_MAX_COUNT);
    TEST_CHECK(n_after == n_before, "nothing was deleted -- count unchanged by the refused restore");
}

static void test_mirror_delete_wrong_count_refuses_and_writes_nothing(void)
{
    TEST_SECTION("backup_import_apply -- MIRROR restore refuses when X-Kiln-Config-Ack-Delete does not "
                 "exactly match the real delete count, and writes nothing");
    reset_stub_state();
    int32_t nonactive_count = 0;
    mirror_ack_test_setup(&nonactive_count);
    TEST_CHECK(nonactive_count > 0, "test assumption: at least one non-active slot exists to be deleted");

    kiln_cfg_summary_t before[KILN_CFG_MAX_COUNT];
    uint8_t n_before = kiln_cfg_store_list(before, KILN_CFG_MAX_COUNT);

    const char *body = "{\"kind\":\"kilnctl_backup\",\"version\":5,\"profiles\":[],\"zones\":[],\"kiln_configs\":[]}";
    kiln_cfg_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    bool partial_write = false;
    char err[160] = {0};
    bool ok = backup_import_apply(body, KILN_CFG_RESTORE_MIRROR, false, nonactive_count - 1, true, &plan,
                                  &partial_write, err, sizeof(err));

    TEST_CHECK(!ok, "MIRROR with a wrong (off-by-one) ack count must refuse");
    TEST_CHECK(!partial_write, "refused before any write -- not a partial write");

    kiln_cfg_summary_t after[KILN_CFG_MAX_COUNT];
    uint8_t n_after = kiln_cfg_store_list(after, KILN_CFG_MAX_COUNT);
    TEST_CHECK(n_after == n_before, "nothing was deleted -- count unchanged by the refused restore");
}

// Also covers HIGH 2: the active marker slot (B) must survive a MIRROR
// restore even though it is unmatched by the (empty) kiln_configs[] body --
// kiln_cfg_store_delete() refuses the active slot unconditionally, so the
// plan must never have queued it, and the commit must succeed rather than
// failing partway through on that refusal.
static void test_mirror_delete_correct_count_proceeds_and_keeps_active_slot(void)
{
    TEST_SECTION("backup_import_apply -- MIRROR restore with the correct X-Kiln-Config-Ack-Delete count "
                 "proceeds, deletes every unmatched NON-active slot, and (HIGH 2) never queues the "
                 "active slot for deletion");
    reset_stub_state();
    int32_t nonactive_count = 0;
    mirror_ack_test_setup(&nonactive_count);
    TEST_CHECK(nonactive_count > 0, "test assumption: at least one non-active slot exists to be deleted");

    int32_t active_id_before = kiln_cfg_store_get_active_id();
    char active_name_before[KILN_CFG_NAME_MAX_LEN + 1] = {0};
    kiln_cfg_summary_t before[KILN_CFG_MAX_COUNT];
    uint8_t n_before = kiln_cfg_store_list(before, KILN_CFG_MAX_COUNT);
    for (uint8_t i = 0; i < n_before; i++) {
        if (before[i].is_active) {
            strncpy(active_name_before, before[i].name, KILN_CFG_NAME_MAX_LEN);
        }
    }

    const char *body = "{\"kind\":\"kilnctl_backup\",\"version\":5,\"profiles\":[],\"zones\":[],\"kiln_configs\":[]}";
    kiln_cfg_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    bool partial_write = false;
    char err[160] = {0};
    bool ok = backup_import_apply(body, KILN_CFG_RESTORE_MIRROR, false, nonactive_count, true, &plan,
                                  &partial_write, err, sizeof(err));

    TEST_CHECK(ok, "MIRROR with the exactly-correct ack count must succeed");

    bool saw_keep_active = false;
    bool saw_delete_active = false;
    for (size_t i = 0; i < plan.count; i++) {
        if (strstr(plan.lines[i], "keep active") != NULL && strstr(plan.lines[i], active_name_before) != NULL) {
            saw_keep_active = true;
        }
        // Matches the literal `delete "%s"` text kiln_cfg_plan_add() emits
        // for an actual queued deletion (backup_import.c) -- NOT the
        // "keep active ... (active slot is never deleted)" informational
        // line above, which also contains the substring "delete" (from
        // "never deleted") and would otherwise false-positive here.
        if (strstr(plan.lines[i], "delete \"") != NULL && strstr(plan.lines[i], active_name_before) != NULL) {
            saw_delete_active = true;
        }
    }
    TEST_CHECK(saw_keep_active, "the plan names the active slot as kept, not queued for deletion");
    TEST_CHECK(!saw_delete_active, "the active slot never appears in a \"delete\" plan line");

    TEST_CHECK(kiln_cfg_store_get_active_id() == active_id_before,
              "the active slot itself is untouched (still active, same id) after the mirror restore");

    kiln_cfg_summary_t after[KILN_CFG_MAX_COUNT];
    uint8_t n_after = kiln_cfg_store_list(after, KILN_CFG_MAX_COUNT);
    int32_t nonactive_after = 0;
    bool active_found_after = false;
    for (uint8_t i = 0; i < n_after; i++) {
        if (!after[i].is_active) {
            nonactive_after++;
        } else if (after[i].id == active_id_before) {
            active_found_after = true;
        }
    }
    TEST_CHECK(nonactive_after == 0, "every non-active slot was deleted by the mirror restore");
    TEST_CHECK(active_found_after, "the active slot survives, present and still marked active");
}

// HIGH 1: backup_import_kiln_configs()'s own commit pass (pass 2) can fail
// PARTWAY through its create loop -- e.g. the store fills up after an
// earlier create in the SAME pass already landed. Before this fix,
// backup_import_apply() only ever set *partial_write_out from ITS OWN two
// passes (kiln_configs[] vs. profiles/zones), never from a mid-pass failure
// inside backup_import_kiln_configs() itself, so this case replied 400
// "nothing changed" while the first create had already committed.
static void test_kiln_configs_partial_write_set_on_mid_pass_create_failure(void)
{
    TEST_SECTION("backup_import_apply -- HIGH 1: a kiln_configs[] restore that creates one slot then "
                 "fails to create a second (store full) sets partial_write, not a clean refusal");
    reset_stub_state();
    TEST_CHECK(hal_kv_init_partition("kiln_nvs") == HAL_OK, "setup: kiln_nvs partition is up");
    mirror_test_trim_to_baseline();

    // Build one real, validly-hashed package from a distinct live state (the
    // same toggle-byte0 technique test_kiln_configs_restore_create_leaves_
    // autosave_targeting_the_prior_active_slot() above uses), then remove
    // its source slot so the board holds no matching identity -- both
    // create-entries below reuse this SAME package text (only its embedded
    // name differs, which the create path already disambiguates via
    // kiln_cfg_unique_name()), so only ONE distinct identity is needed.
    test_stub_kiln_cfg_export_content_toggle_byte0();
    int32_t id_src = -1;
    char reason[160] = {0};
    int32_t active_before_src = kiln_cfg_store_get_active_id();
    TEST_CHECK(kiln_cfg_store_save_current("Temp Source For Create", -1, &id_src, reason, sizeof(reason)),
              "setup: temporary source slot saved (becomes active)");
    char *pkg_json = (char *)malloc(KILN_CFG_EXPORT_JSON_MAX_LEN);
    TEST_CHECK(pkg_json != NULL, "test scratch alloc");
    size_t pkg_len = 0;
    TEST_CHECK(kiln_cfg_store_export_package_json(id_src, pkg_json, KILN_CFG_EXPORT_JSON_MAX_LEN, &pkg_len, reason,
                                                  sizeof(reason)),
              "setup: temporary source slot exports a real, validly-hashed package");
    // Restore active_id so the temp source slot can be deleted (delete
    // refuses the active slot), then remove it -- the board must hold NO
    // slot with this identity, or the two create-entries below would match
    // it instead of creating new slots.
    TEST_CHECK(kiln_cfg_store_set_active_id_raw(active_before_src, reason, sizeof(reason)),
              "setup: active_id restored so the temp source slot can be deleted");
    TEST_CHECK(kiln_cfg_store_delete(id_src, true, reason, sizeof(reason)), "setup: temp source slot removed");
    // Toggle byte0 back to its pre-toggle value: the filler slots below are
    // saved via kiln_cfg_store_save_current(), which captures whatever the
    // stub's "live" export content is AT THAT MOMENT -- if it were left at
    // the toggled value used for pkg_json above, every filler would share
    // pkg_json's exact identity (schema+hash), which would make the two
    // create-entries below match an EXISTING filler by identity (a rename/
    // no-op) instead of actually creating a new slot, silently defeating
    // this test's whole "fill to exactly one free slot, then create two"
    // setup (found by inspecting a DEBUG dump showing both creates as
    // spurious no-op successes despite the store genuinely being full).
    test_stub_kiln_cfg_export_content_toggle_byte0();

    // Fill the board to exactly ONE free slot left.
    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    uint8_t n_now = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    int free_slots = (int)KILN_CFG_MAX_COUNT - (int)n_now;
    TEST_CHECK(free_slots >= 1, "test assumption: at least one free slot remains to fill down from");
    int fillers_needed = free_slots - 1;
    for (int i = 0; i < fillers_needed; i++) {
        char name[KILN_CFG_NAME_MAX_LEN + 1];
        snprintf(name, sizeof(name), "Filler %d", i);
        int32_t filler_id = -1;
        TEST_CHECK(kiln_cfg_store_save_current(name, -1, &filler_id, reason, sizeof(reason)),
                  "setup: filler slot saved to bring the board to exactly one free slot");
    }
    uint8_t n_before_restore = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    TEST_CHECK((int)KILN_CFG_MAX_COUNT - (int)n_before_restore == 1,
              "test assumption: exactly one free slot remains before the restore");

    char *body = (char *)malloc(KILN_CFG_EXPORT_JSON_MAX_LEN * 2 + 256);
    TEST_CHECK(body != NULL, "test scratch alloc");
    snprintf(body, KILN_CFG_EXPORT_JSON_MAX_LEN * 2 + 256,
             "{\"kind\":\"kilnctl_backup\",\"version\":5,\"profiles\":[],\"zones\":[],"
             "\"kiln_configs\":[{\"package\":%s},{\"package\":%s}]}",
             pkg_json, pkg_json);

    kiln_cfg_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    bool partial_write = false;
    char err[160] = {0};
    bool ok = backup_import_apply(body, KILN_CFG_RESTORE_MERGE, false, -1, true, &plan, &partial_write, err,
                                  sizeof(err));

    TEST_CHECK(!ok, "the second create fails once the store is full");
    TEST_CHECK(partial_write,
              "HIGH 1: the first create already landed before the second failed -- this must be reported "
              "as a partial write, not a clean refusal");
    TEST_CHECK(strstr(err, "kiln_configs[1]") != NULL, "the error names the SECOND entry (index 1) as the "
                                                       "one that failed at commit");

    uint8_t n_after = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    TEST_CHECK(n_after == n_before_restore + 1,
              "exactly one of the two creates landed (the board is now completely full) -- proving the "
              "partial write actually happened, not merely that the flag was set");

    free(pkg_json);
    free(body);
}

/* backup_tuning_float_matches() formats each side into a bounded buffer.
 * A huge-but-finite float must neither truncate (two different huge values
 * comparing equal) nor overflow. FLT_MAX prints 39 integer digits under %.3f. */
static void test_backup_tuning_float_matches_huge_values(void)
{
    char buf[BACKUP_TUNING_FLOAT_BUF];
    int n = snprintf(buf, sizeof(buf), BACKUP_TUNING_FLOAT_FMT, (double)3.4e38f);
    TEST_CHECK(n > 0 && n < (int)sizeof(buf), "FLT_MAX-scale value fits BACKUP_TUNING_FLOAT_BUF untruncated");
    TEST_CHECK(backup_tuning_float_matches(1e30f, 1e30f), "equal huge values match");
    TEST_CHECK(backup_tuning_float_matches(3.4e38f, 3.4e38f), "FLT_MAX-scale values match");
    TEST_CHECK(!backup_tuning_float_matches(1e30f, -1e30f), "sign differs");
}

// ---- WP9 (docs/GITHUB_RELEASE_UPDATE_PLAN.md): the "update_repo" key ----
static void wp9_fresh_repo_setting(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition("kiln_nvs");
    update_settings_reset_ram_for_test();
    update_settings_start();
}

static const char *WP9_BODY_HEAD =
    "{\"kind\":\"kilnctl_backup\",\"version\":5,"
    "\"profiles\":[{\"id\":0,\"name\":\"P1\",\"zone_mask\":1,"
    "\"segments\":[{\"target_c\":100,\"ramp_c_per_hr\":50,\"dwell_min\":30}]}],"
    "\"zones\":[{\"index\":0,\"pid_kp\":1.5,\"pid_ki\":0.2,\"pid_kd\":0.05}]";

static bool wp9_import_with(const char *tail, char *err, size_t err_cap)
{
    char body[768];
    snprintf(body, sizeof(body), "%s%s}", WP9_BODY_HEAD, tail);
    return test_backup_import_apply(body, err, err_cap);
}

static void test_update_repo_imports_and_persists(void)
{
    TEST_SECTION("backup_import_apply -- update_repo is applied and persisted");
    reset_stub_state();
    wp9_fresh_repo_setting();
    char err[160] = "";
    TEST_CHECK(wp9_import_with(",\"update_repo\":\"someone/fork\"", err, sizeof(err)), "a valid update_repo imports");
    TEST_CHECK(strcmp(update_settings_repo(), "someone/fork") == 0, "the imported repo is live");
    update_settings_reset_ram_for_test();
    update_settings_start();
    TEST_CHECK(strcmp(update_settings_repo(), "someone/fork") == 0, "the imported repo survives a reboot");

    reset_stub_state();
    TEST_CHECK(wp9_import_with(",\"update_repo\":\"\"", err, sizeof(err)), "an empty update_repo imports");
    TEST_CHECK(update_settings_repo_is_default(), "an empty update_repo resets to the default");
}

static void test_update_repo_absent_is_noop(void)
{
    TEST_SECTION("backup_import_apply -- no update_repo key leaves the setting alone");
    reset_stub_state();
    wp9_fresh_repo_setting();
    update_settings_set("keep/me");
    char err[160] = "";
    TEST_CHECK(wp9_import_with("", err, sizeof(err)), "an older backup without the key still imports");
    TEST_CHECK(strcmp(update_settings_repo(), "keep/me") == 0, "the setting is untouched");
}

static void test_update_repo_invalid_refuses_whole_restore(void)
{
    TEST_SECTION("backup_import_apply -- an invalid update_repo refuses the whole restore, nothing written");
    static const char *const tails[] = {
        ",\"update_repo\":\"not a repo\"",
        ",\"update_repo\":\"http://evil.example/x\"",
        ",\"update_repo\":\"a/../b\"",
        ",\"update_repo\":\"-a/b\"",
        ",\"update_repo\":42",
        ",\"update_repo\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"",
    };
    for (size_t i = 0; i < sizeof(tails) / sizeof(tails[0]); i++) {
        reset_stub_state();
        wp9_fresh_repo_setting();
        update_settings_set("keep/me");
        char err[160] = "";
        TEST_CHECK(!wp9_import_with(tails[i], err, sizeof(err)), "a bad update_repo is refused");
        TEST_CHECK(g_total_write_calls == 0, "pass 1 refusal: no profile/zone write happened");
        TEST_CHECK(g_profile_save_calls == 0, "pass 1 refusal: no profile was saved");
        TEST_CHECK(strcmp(update_settings_repo(), "keep/me") == 0, "pass 1 refusal: the setting is unchanged");
        TEST_CHECK(strstr(err, "update_repo") != NULL, "the error names the update_repo field");
    }
}

static void test_update_repo_export_round_trip(void)
{
    TEST_SECTION("backup_export_get_handler -- emits update_repo, and it round-trips through import");
    reset_stub_state();
    wp9_fresh_repo_setting();
    esp_err_t e = run_export();
    TEST_CHECK(e == ESP_OK, "export succeeds");
    TEST_CHECK(strstr(s_export_body, "\"update_repo\":\"\"") != NULL,
               "an unset setting exports as empty, never as today's default string");
    TEST_CHECK(strstr(s_export_body, UPDATE_SETTINGS_DEFAULT_REPO) == NULL,
               "the default repo string does not appear in the export of a stock board");
    update_settings_set(UPDATE_SETTINGS_DEFAULT_REPO);
    e = run_export();
    TEST_CHECK(e == ESP_OK && strstr(s_export_body, "\"update_repo\":\"\"") != NULL,
               "the default spelled out is still exported as empty");

    update_settings_set("round/trip-repo");
    e = run_export();
    TEST_CHECK(e == ESP_OK, "export succeeds after a set");
    TEST_CHECK(strstr(s_export_body, "\"update_repo\":\"round/trip-repo\"") != NULL, "the set repo is exported");

    // Restore that document onto a board configured differently.
    char *saved = strdup(s_export_body);
    TEST_CHECK(saved != NULL, "copy the exported document");
    if (saved != NULL) {
        update_settings_set("other/board");
        char err[160] = "";
        bool ok = test_backup_import_apply(saved, err, sizeof(err));
        TEST_CHECK(ok, "the exported document imports back");
        TEST_CHECK(strcmp(update_settings_repo(), "round/trip-repo") == 0, "the repo is restored from the backup");
        free(saved);
    }
}

typedef struct {
    uint8_t version;
    char repo[63];
} tus_blob_probe_t; // mirrors update_settings.c's private blob layout (64 bytes)

static void wp9_http_reset_capture(void)
{
    s_post_last_status[0] = '\0';
    s_post_last_body[0] = '\0';
    s_send_last_body[0] = '\0';
    s_send_last_err_code = 0;
    s_send_last_err_msg[0] = '\0';
}

static esp_err_t wp9_http_post(const char *form)
{
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.content_len = (long long)strlen(form);
    s_recv_feed = form;
    s_recv_feed_off = 0;
    wp9_http_reset_capture();
    esp_err_t e = settings_post_handler(&req);
    s_recv_feed = NULL;
    return e;
}

static void test_update_settings_http_post(void)
{
    TEST_SECTION("update_settings_http -- POST handler: decode, refusals, mode gate, persist failure");
    reset_stub_state();
    wp9_fresh_repo_setting();
    update_settings_set("keep/me");

    // %2F decodes to '/', success replies with the settings JSON and no error status.
    TEST_CHECK(wp9_http_post("repo=someone%2Ffork") == ESP_OK, "a valid POST returns ESP_OK");
    TEST_CHECK(strcmp(update_settings_repo(), "someone/fork") == 0, "%2F is decoded and the repo applied");
    TEST_CHECK(s_post_last_status[0] == '\0', "success sets no error status (200)");
    TEST_CHECK(strstr(s_send_last_body, "\"repo\":\"someone/fork\"") != NULL, "the reply carries the new repo");

    // Invalid value: 400 with the JSON error, setting unchanged.
    wp9_http_post("repo=nope");
    TEST_CHECK(strcmp(s_post_last_status, "400 Bad Request") == 0, "an invalid repo gets 400");
    TEST_CHECK(strstr(s_send_last_body, "invalid repo") != NULL, "the 400 body names the problem");
    TEST_CHECK(strcmp(update_settings_repo(), "someone/fork") == 0, "a refused POST leaves the setting alone");

    // Leading-dot spellings.
    wp9_http_post("repo=owner%2F.");
    TEST_CHECK(strcmp(s_post_last_status, "400 Bad Request") == 0, "owner/. is refused");
    wp9_http_post("repo=.%2Fname");
    TEST_CHECK(strcmp(s_post_last_status, "400 Bad Request") == 0, "./name is refused");

    // Over-long: one over the cap (63 chars) reaches the validator, far over is refused by the form parser.
    char longform[320];
    char longrepo[130];
    memset(longrepo, 'n', sizeof(longrepo));
    longrepo[0] = 'o';
    longrepo[1] = '%';
    longrepo[2] = '2';
    longrepo[3] = 'F';
    longrepo[63 + 2] = '\0'; // "o%2F" + 61 'n' decodes to 63 characters
    snprintf(longform, sizeof(longform), "repo=%s", longrepo);
    wp9_http_post(longform);
    TEST_CHECK(strcmp(s_post_last_status, "400 Bad Request") == 0, "a 63-character repo is refused");
    TEST_CHECK(strcmp(update_settings_repo(), "someone/fork") == 0, "a 63-character repo changes nothing");
    memset(longrepo, 'n', sizeof(longrepo));
    longrepo[0] = 'o';
    longrepo[1] = '/';
    longrepo[125] = '\0';
    snprintf(longform, sizeof(longform), "repo=%s", longrepo);
    wp9_http_post(longform);
    TEST_CHECK(strcmp(s_post_last_status, "400 Bad Request") == 0, "a 125-character repo is refused, not truncated");
    TEST_CHECK(strcmp(update_settings_repo(), "someone/fork") == 0, "a 125-character repo changes nothing");

    // %00: an embedded NUL must not truncate into a valid-looking value.
    wp9_http_post("repo=a%2Fb%00zzz");
    TEST_CHECK(strcmp(s_post_last_status, "400 Bad Request") == 0, "an embedded NUL (mid-value) is refused");
    wp9_http_post("repo=other%2Frepo%00");
    TEST_CHECK(strcmp(s_post_last_status, "400 Bad Request") == 0, "a trailing %00 is refused");
    TEST_CHECK(strcmp(update_settings_repo(), "someone/fork") == 0, "%00 requests change nothing");

    // Missing field / empty / oversized body: plain 400 via httpd_resp_send_err.
    wp9_http_post("other=1");
    TEST_CHECK(s_send_last_err_code == 400, "a missing repo field gets 400");
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    wp9_http_reset_capture();
    req.content_len = 0;
    settings_post_handler(&req);
    TEST_CHECK(s_send_last_err_code == 400, "an empty body gets 400");
    wp9_http_reset_capture();
    req.content_len = SETTINGS_BODY_MAX + 1;
    settings_post_handler(&req);
    TEST_CHECK(s_send_last_err_code == 400, "an oversized body gets 400");

    // Mode gate: 409 with the dedicated reason while a firing or autotune runs.
    for (int which = 0; which < 2; which++) {
        s_test_profile_running_for_mode_gate = which == 0;
        s_test_autotune_running_for_mode_gate = which == 1;
        wp9_http_post("repo=blocked%2Fwrite");
        TEST_CHECK(strcmp(s_post_last_status, "409 Conflict") == 0, "a write during a run is refused with 409");
        TEST_CHECK(strstr(s_send_last_body, "update repository") != NULL, "the 409 names the update repository");
        TEST_CHECK(strstr(s_send_last_body, "update stage") == NULL, "the 409 is not the stage-write text");
        TEST_CHECK(strcmp(update_settings_repo(), "someone/fork") == 0, "a gated POST changes nothing");
    }
    s_test_profile_running_for_mode_gate = false;
    s_test_autotune_running_for_mode_gate = false;

    // Persist failure: reported as a 500, never as success.
    fake_kv_script_next_write_status(HAL_IO);
    wp9_http_post("repo=flaky%2Fwrite");
    TEST_CHECK(strcmp(s_post_last_status, "500 Internal Server Error") == 0, "a failed persist gets 500");
    TEST_CHECK(strstr(s_send_last_body, "\"ok\":false") != NULL, "the 500 body says ok:false");

    // GET reflects the live value.
    wp9_fresh_repo_setting();
    update_settings_set("get/check");
    httpd_req_t greq;
    memset(&greq, 0, sizeof(greq));
    wp9_http_reset_capture();
    settings_get_handler(&greq);
    TEST_CHECK(strstr(s_send_last_body, "\"repo\":\"get/check\"") != NULL, "GET reports the live repo");
    TEST_CHECK(strstr(s_send_last_body, "\"is_default\":false") != NULL, "GET reports is_default false");
}

static bool wp9_apply_full(const char *tail, kiln_cfg_restore_mode_t mode, bool dry_run, int32_t ack_delete,
                           kiln_cfg_plan_t *plan, bool *partial_write, char *err, size_t err_cap)
{
    char body[768];
    snprintf(body, sizeof(body), "%s%s}", WP9_BODY_HEAD, tail);
    memset(plan, 0, sizeof(*plan));
    *partial_write = false;
    return backup_import_apply(body, mode, dry_run, ack_delete, true, plan, partial_write, err, err_cap);
}

static bool wp9_plan_has(const kiln_cfg_plan_t *plan, const char *needle)
{
    for (size_t i = 0; i < plan->count; i++) {
        if (strstr(plan->lines[i], needle) != NULL) {
            return true;
        }
    }
    return false;
}

static void test_update_repo_backslash_refused(void)
{
    TEST_SECTION("backup_import_apply -- any backslash in update_repo refuses the whole restore");
    static const char *const tails[] = {
        ",\"update_repo\":\"some\\u006fne/fork\"",  // \u006f would unescape to 'o'
        ",\"update_repo\":\"someone\\/fork\"",     // \/ would unescape to '/'
        ",\"update_repo\":\"someone/for\\\\k\"",   // an escaped backslash
        ",\"update_repo\":\"so\\x\"",
    };
    for (size_t i = 0; i < sizeof(tails) / sizeof(tails[0]); i++) {
        reset_stub_state();
        wp9_fresh_repo_setting();
        update_settings_set("keep/me");
        char err[160] = "";
        TEST_CHECK(!wp9_import_with(tails[i], err, sizeof(err)), "an escaped update_repo is refused");
        TEST_CHECK(strstr(err, "backslash") != NULL, "the error names the backslash");
        TEST_CHECK(g_total_write_calls == 0 && g_profile_save_calls == 0, "nothing was written");
        TEST_CHECK(strcmp(update_settings_repo(), "keep/me") == 0, "the setting is unchanged");
    }
    // The dots rule reaches import too.
    reset_stub_state();
    wp9_fresh_repo_setting();
    char err[160] = "";
    TEST_CHECK(!wp9_import_with(",\"update_repo\":\"owner/.\"", err, sizeof(err)), "owner/. is refused on import");
    TEST_CHECK(!wp9_import_with(",\"update_repo\":\"./name\"", err, sizeof(err)), "./name is refused on import");
}

static void test_update_repo_default_value_is_unset(void)
{
    TEST_SECTION("backup_import_apply -- update_repo equal to the default is treated as unset");
    reset_stub_state();
    wp9_fresh_repo_setting();
    update_settings_set("custom/repo");
    char err[160] = "";
    TEST_CHECK(wp9_import_with(",\"update_repo\":\"" UPDATE_SETTINGS_DEFAULT_REPO "\"", err, sizeof(err)),
               "the default spelled out imports");
    TEST_CHECK(update_settings_repo_is_default(), "the default is in use afterwards");
    tus_blob_probe_t probe;
    memset(&probe, 0xAB, sizeof(probe));
    hal_kv_handle_t h;
    hal_kv_open(&h, "kiln_cfg", HAL_KV_MODE_READ_ONLY, "kiln_nvs");
    size_t len = sizeof(probe);
    TEST_CHECK(hal_kv_get_blob(&h, "update_repo", &probe, &len) == HAL_OK, "the blob exists");
    hal_kv_close(&h);
    TEST_CHECK(probe.repo[0] == '\0', "stored as empty, not as the default string");

    // Already default: an empty update_repo imports again and stays default.
    reset_stub_state();
    TEST_CHECK(wp9_import_with(",\"update_repo\":\"\"", err, sizeof(err)), "an empty update_repo imports again");
    TEST_CHECK(update_settings_repo_is_default(), "still the default");
}

static void test_update_repo_dry_run_names_old_and_new(void)
{
    TEST_SECTION("backup_import_apply -- dry_run names the old and new update_repo and writes nothing");
    reset_stub_state();
    wp9_fresh_repo_setting();
    update_settings_set("keep/me");
    kiln_cfg_plan_t plan;
    bool partial = false;
    char err[160] = "";
    TEST_CHECK(wp9_apply_full(",\"update_repo\":\"someone/fork\"", KILN_CFG_RESTORE_MERGE, true, -1, &plan, &partial,
                              err, sizeof(err)),
               "dry_run succeeds");
    TEST_CHECK(wp9_plan_has(&plan, "update_repo keep/me -> someone/fork"), "the plan names old and new repo");
    TEST_CHECK(strcmp(update_settings_repo(), "keep/me") == 0, "dry_run did not change the setting");
    TEST_CHECK(g_total_write_calls == 0 && g_profile_save_calls == 0, "dry_run wrote nothing");

    // Empty value: the new repo shown is the default.
    TEST_CHECK(wp9_apply_full(",\"update_repo\":\"\"", KILN_CFG_RESTORE_MERGE, true, -1, &plan, &partial, err,
                              sizeof(err)),
               "dry_run with an empty update_repo succeeds");
    TEST_CHECK(wp9_plan_has(&plan, "update_repo keep/me -> " UPDATE_SETTINGS_DEFAULT_REPO),
               "an empty value is shown as the default");

    // Same value: reported as unchanged.
    TEST_CHECK(wp9_apply_full(",\"update_repo\":\"keep/me\"", KILN_CFG_RESTORE_MERGE, true, -1, &plan, &partial, err,
                              sizeof(err)),
               "dry_run with the current value succeeds");
    TEST_CHECK(wp9_plan_has(&plan, "update_repo unchanged: keep/me"), "an identical value reads as unchanged");

    // Absent key: no line at all.
    TEST_CHECK(wp9_apply_full("", KILN_CFG_RESTORE_MERGE, true, -1, &plan, &partial, err, sizeof(err)),
               "dry_run without the key succeeds");
    TEST_CHECK(!wp9_plan_has(&plan, "update_repo"), "no update_repo line when the key is absent");

    // Worst-case line (62 + 62 characters) is not truncated.
    char big_old[64], big_tail[160];
    memset(big_old, 'a', 62);
    big_old[1] = '/';
    big_old[62] = '\0';
    update_settings_set(big_old);
    char big_new[64];
    memset(big_new, 'b', 62);
    big_new[1] = '/';
    big_new[62] = '\0';
    snprintf(big_tail, sizeof(big_tail), ",\"update_repo\":\"%s\"", big_new);
    TEST_CHECK(wp9_apply_full(big_tail, KILN_CFG_RESTORE_MERGE, true, -1, &plan, &partial, err, sizeof(err)),
               "dry_run with 62-character repos succeeds");
    char want[200];
    snprintf(want, sizeof(want), "update_repo %s -> %s", big_old, big_new);
    TEST_CHECK(wp9_plan_has(&plan, want), "the worst-case plan line is whole, not truncated");
}

static void test_update_repo_persist_failure_is_partial_write(void)
{
    TEST_SECTION("backup_import_apply -- update_repo persist failure after the rest landed is a partial write");
    reset_stub_state();
    wp9_fresh_repo_setting();
    update_settings_set("keep/me");
    kiln_cfg_plan_t plan;
    bool partial = false;
    char err[160] = "";
    fake_kv_script_next_write_status(HAL_IO);
    bool ok = wp9_apply_full(",\"update_repo\":\"someone/fork\"", KILN_CFG_RESTORE_MERGE, false, -1, &plan, &partial,
                             err, sizeof(err));
    TEST_CHECK(!ok, "a failed update_repo persist fails the restore");
    TEST_CHECK(partial, "it is reported as a partial write (the handler turns this into the 500)");
    TEST_CHECK(strstr(err, "update_repo") != NULL, "the error names update_repo");
    TEST_CHECK(g_profile_save_calls > 0, "the profiles had already been written (hence partial)");
}

static void test_update_repo_pass1_refusal_skips_kiln_configs_commit(void)
{
    TEST_SECTION("backup_import_apply -- a pass-1 update_repo refusal never reaches the kiln_configs commit");
    reset_stub_state();
    wp9_fresh_repo_setting();
    int32_t nonactive_count = 0;
    mirror_ack_test_setup(&nonactive_count);
    TEST_CHECK(nonactive_count > 0, "test assumption: a non-active slot exists that MIRROR would delete");
    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    uint8_t n_before = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);

    kiln_cfg_plan_t plan;
    bool partial = false;
    char err[160] = "";
    char body[512];
    snprintf(body, sizeof(body),
             "{\"kind\":\"kilnctl_backup\",\"version\":5,\"profiles\":[],\"zones\":[],\"kiln_configs\":[],"
             "\"update_repo\":\"not a repo\"}");
    memset(&plan, 0, sizeof(plan));
    bool ok = backup_import_apply(body, KILN_CFG_RESTORE_MIRROR, false, nonactive_count, true, &plan, &partial, err,
                                  sizeof(err));
    TEST_CHECK(!ok, "the bad update_repo refuses the restore");
    TEST_CHECK(!partial, "a pass-1 refusal is not a partial write");
    uint8_t n_after = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    TEST_CHECK(n_after == n_before, "no kiln config slot was deleted (the commit pass never ran)");

    // Control: the same body with a valid update_repo does delete, proving the setup is sensitive.
    snprintf(body, sizeof(body),
             "{\"kind\":\"kilnctl_backup\",\"version\":5,\"profiles\":[],\"zones\":[],\"kiln_configs\":[],"
             "\"update_repo\":\"fine/repo\"}");
    memset(&plan, 0, sizeof(plan));
    ok = backup_import_apply(body, KILN_CFG_RESTORE_MIRROR, false, nonactive_count, true, &plan, &partial, err,
                             sizeof(err));
    TEST_CHECK(ok, "control: the valid update_repo restore succeeds");
    n_after = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    TEST_CHECK(n_after < n_before, "control: the MIRROR commit really deletes slots when pass 1 passes");
}

// ---- aux_outputs (spare-relay on/off outputs): the top-level "aux_outputs" key ----
static void aux_bk_fresh(void)
{
    wp9_fresh_repo_setting(); // fake_kv_reset_all + kiln_nvs partition + update_settings
    (void)aux_outputs_cfg_start(0);
}

static aux_output_entry_t aux_bk_entry(bool enabled, uint8_t tc_plus1, float hyst, uint16_t on_s, uint16_t off_s)
{
    aux_output_entry_t e;
    memset(&e, 0, sizeof(e));
    e.enabled = enabled ? 1 : 0;
    e.tc_zone_plus1 = tc_plus1;
    e.hyst_c = hyst;
    e.min_on_s = on_s;
    e.min_off_s = off_s;
    return e;
}

static void test_aux_outputs_import_applies_and_persists(void)
{
    TEST_SECTION("backup_import_apply -- aux_outputs entries are applied, persisted and omit-preserve unnamed fields");
    reset_stub_state();
    aux_bk_fresh();
    aux_output_entry_t base = aux_bk_entry(false, 0, 7.0f, 90, 91);
    TEST_CHECK(aux_outputs_cfg_set(3, &base, 0) == ESP_OK, "seed relay 3 with distinctive fields");
    char err[200] = "";
    TEST_CHECK(wp9_import_with(",\"aux_outputs\":[{\"relay\":2,\"enabled\":true,\"tc_zone\":1,\"hyst_c\":3.5,"
                               "\"min_on_s\":45,\"min_off_s\":60},{\"relay\":3,\"enabled\":true},"
                               "{\"relay\":4,\"enabled\":0}]",
                               err, sizeof(err)),
               "a valid aux_outputs array imports");
    aux_output_t o;
    TEST_CHECK(aux_outputs_cfg_get(2, &o) && o.enabled && o.tc_zone == 1 && o.hyst_c == 3.5f && o.min_on_s == 45 &&
                   o.min_off_s == 60,
               "relay 2 carries every imported field");
    TEST_CHECK(aux_outputs_cfg_get(3, &o) && o.enabled && o.hyst_c == 7.0f && o.min_on_s == 90 && o.min_off_s == 91,
               "relay 3: enabled imported, omitted fields keep their current values");
    TEST_CHECK(aux_outputs_cfg_get(4, &o) && !o.enabled, "relay 4 stays disabled (enabled:0)");
    TEST_CHECK(aux_outputs_cfg_get(1, &o) && !o.enabled, "relay 1 not named: untouched");
    TEST_CHECK(aux_outputs_cfg_enabled_mask() == 0x06, "enabled mask is relays 2 and 3");
    (void)aux_outputs_cfg_start(0);
    TEST_CHECK(aux_outputs_cfg_enabled_mask() == 0x06 && aux_outputs_cfg_get(2, &o) && o.hyst_c == 3.5f,
               "the imported state survives a reboot (persisted, not RAM only)");
}

static void test_aux_outputs_absent_is_noop(void)
{
    TEST_SECTION("backup_import_apply -- no aux_outputs key leaves the aux store alone");
    reset_stub_state();
    aux_bk_fresh();
    aux_output_entry_t e = aux_bk_entry(true, 2, 4.0f, 11, 12);
    TEST_CHECK(aux_outputs_cfg_set(1, &e, 0) == ESP_OK, "seed relay 1 enabled");
    char err[200] = "";
    TEST_CHECK(wp9_import_with("", err, sizeof(err)), "an older backup without the key still imports");
    aux_output_t o;
    TEST_CHECK(aux_outputs_cfg_get(1, &o) && o.enabled && o.tc_zone == 1 && o.hyst_c == 4.0f,
               "relay 1 is untouched");
}

static void test_aux_outputs_malformed_refuses_whole_restore(void)
{
    TEST_SECTION("backup_import_apply -- a malformed aux_outputs entry refuses the whole restore, nothing applied");
    static const char *const tails[] = {
        /* a valid first entry must not land when a later one is bad */
        ",\"aux_outputs\":[{\"relay\":1,\"enabled\":true},{\"relay\":2,\"enabled\":\"x\"}]",
        ",\"aux_outputs\":5",
        ",\"aux_outputs\":[3]",
        ",\"aux_outputs\":[{\"relay\":0,\"enabled\":true}]",
        ",\"aux_outputs\":[{\"relay\":5,\"enabled\":true}]",
        ",\"aux_outputs\":[{\"relay\":1.5,\"enabled\":true}]",
        ",\"aux_outputs\":[{\"enabled\":true}]",
        ",\"aux_outputs\":[{\"relay\":1,\"enabled\":true},{\"relay\":1,\"enabled\":false}]",
        ",\"aux_outputs\":[{\"relay\":1}]",
        ",\"aux_outputs\":[{\"relay\":1,\"enabled\":2}]",
        ",\"aux_outputs\":[{\"relay\":1,\"enabled\":true,\"tc_zone\":9}]",
        ",\"aux_outputs\":[{\"relay\":1,\"enabled\":true,\"tc_zone\":0.5}]",
        ",\"aux_outputs\":[{\"relay\":1,\"enabled\":true,\"hyst_c\":0.1}]",
        ",\"aux_outputs\":[{\"relay\":1,\"enabled\":true,\"hyst_c\":99}]",
        ",\"aux_outputs\":[{\"relay\":1,\"enabled\":true,\"min_on_s\":0}]",
        ",\"aux_outputs\":[{\"relay\":1,\"enabled\":true,\"min_off_s\":4000}]",
        ",\"aux_outputs\":[{\"relay\":1,\"enabled\":true,\"min_on_s\":1.5}]",
        ",\"aux_outputs\":[{\"relay\":1,\"enabled\":true,\"hyst_c\":\"2\"}]",
    };
    for (size_t i = 0; i < sizeof(tails) / sizeof(tails[0]); i++) {
        reset_stub_state();
        aux_bk_fresh();
        aux_output_entry_t keep = aux_bk_entry(true, 0, 5.0f, 20, 21);
        TEST_CHECK(aux_outputs_cfg_set(4, &keep, 0) == ESP_OK, "seed relay 4 enabled");
        g_total_write_calls = 0;
        char err[200] = "";
        TEST_CHECK(!wp9_import_with(tails[i], err, sizeof(err)), "a bad aux_outputs is refused");
        TEST_CHECK(g_total_write_calls == 0 && g_profile_save_calls == 0,
                   "pass 1 refusal: no profile/zone write happened");
        TEST_CHECK(aux_outputs_cfg_enabled_mask() == 0x08, "pass 1 refusal: the aux store is unchanged");
        aux_output_t o;
        TEST_CHECK(aux_outputs_cfg_get(1, &o) && !o.enabled, "pass 1 refusal: relay 1 was not applied");
        TEST_CHECK(strstr(err, "aux_outputs") != NULL, "the error names aux_outputs");
    }
}

static void test_aux_outputs_quarantined_store_refuses_import(void)
{
    TEST_SECTION("backup_import_apply -- a quarantined aux store refuses a restore carrying aux_outputs, "
                 "and the export omits the key");
    reset_stub_state();
    aux_bk_fresh();
    uint8_t huge[400];
    memset(huge, 0xCD, sizeof(huge));
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, "kiln_cfg", HAL_KV_MODE_READ_WRITE, "kiln_nvs") == HAL_OK, "open for stash");
    TEST_CHECK(hal_kv_set_blob(&h, "aux_out_cfg", huge, sizeof(huge)) == HAL_OK, "stash oversize blob");
    hal_kv_commit(&h);
    hal_kv_close(&h);
    (void)aux_outputs_cfg_start(0);
    TEST_CHECK(aux_outputs_cfg_quarantined(), "test setup: store is quarantined");
    g_total_write_calls = 0;
    char err[200] = "";
    TEST_CHECK(!wp9_import_with(",\"aux_outputs\":[{\"relay\":1,\"enabled\":true}]", err, sizeof(err)),
               "refused while quarantined");
    TEST_CHECK(g_total_write_calls == 0 && g_profile_save_calls == 0, "nothing else was written");
    TEST_CHECK(strstr(err, "quarantined") != NULL, "the error says why");
    TEST_CHECK(wp9_import_with("", err, sizeof(err)), "a backup without the key still imports");
    TEST_CHECK(run_export() == ESP_OK && strstr(s_export_body, "\"aux_outputs\"") == NULL,
               "the export omits aux_outputs rather than fabricating defaults");
    aux_bk_fresh();
}

static void test_aux_outputs_conflict_with_zone_relay_refused_in_pass_1(void)
{
    TEST_SECTION("backup_import_apply -- an enabled aux relay a restored (or live) zone also claims is refused "
                 "before anything is written");
    static const char *const file_zone_body =
        "{\"kind\":\"kilnctl_backup\",\"version\":5,\"profiles\":[],"
        "\"zones\":[{\"index\":0,\"pid_kp\":9,\"pid_ki\":0,\"pid_kd\":0,\"relay_mask\":4,\"settings_source\":255}],"
        "\"aux_outputs\":[{\"relay\":3,\"enabled\":true}]}";
    reset_stub_state();
    aux_bk_fresh();
    g_total_write_calls = 0;
    char err[200] = "";
    TEST_CHECK(!test_backup_import_apply(file_zone_body, err, sizeof(err)),
               "file zone claims the relay the file enables");
    TEST_CHECK(strstr(err, "aux_outputs") != NULL, "the error names aux_outputs");
    TEST_CHECK(g_total_write_calls == 0, "pass 1 refusal: no zone write");
    TEST_CHECK(aux_outputs_cfg_enabled_mask() == 0, "pass 1 refusal: aux not enabled");

    reset_stub_state();
    aux_bk_fresh();
    TEST_CHECK(zones_config_set_relay_mask(0, 0x04), "live zone 0 claims relay 3");
    g_total_write_calls = 0;
    TEST_CHECK(!wp9_import_with(",\"aux_outputs\":[{\"relay\":3,\"enabled\":true}]", err, sizeof(err)),
               "live zone claims the relay the file enables (file has no relay_mask)");
    TEST_CHECK(g_total_write_calls == 0 && aux_outputs_cfg_enabled_mask() == 0, "nothing written");
}

static void test_aux_outputs_zone_and_aux_swap_applies_in_either_direction(void)
{
    TEST_SECTION("backup_import_apply -- a backup that moves a relay between a zone and an aux output applies "
                 "cleanly (disables land before zones, enables after)");
    static const char *const free_for_aux =
        "{\"kind\":\"kilnctl_backup\",\"version\":5,\"profiles\":[],"
        "\"zones\":[{\"index\":0,\"pid_kp\":9,\"pid_ki\":0,\"pid_kd\":0,\"relay_mask\":1,\"settings_source\":255}],"
        "\"aux_outputs\":[{\"relay\":3,\"enabled\":true}]}";
    static const char *const free_for_zone =
        "{\"kind\":\"kilnctl_backup\",\"version\":5,\"profiles\":[],"
        "\"zones\":[{\"index\":0,\"pid_kp\":9,\"pid_ki\":0,\"pid_kd\":0,\"relay_mask\":4,\"settings_source\":255}],"
        "\"aux_outputs\":[{\"relay\":3,\"enabled\":false}]}";
    char err[200] = "";
    reset_stub_state();
    aux_bk_fresh();
    TEST_CHECK(zones_config_set_relay_mask(0, 0x04), "zone 0 starts on relay 3");
    TEST_CHECK(test_backup_import_apply(free_for_aux, err, sizeof(err)), "zone gives relay 3 up while aux takes it");
    TEST_CHECK(s_writes[0].relay_mask == 0x01, "zone 0 now owns relay 1");
    TEST_CHECK(aux_outputs_cfg_enabled_mask() == 0x04, "aux relay 3 is enabled");

    reset_stub_state();
    aux_bk_fresh();
    aux_output_entry_t on = aux_bk_entry(true, 0, 0, 0, 0);
    TEST_CHECK(aux_outputs_cfg_set(3, &on, 0) == ESP_OK, "aux relay 3 starts enabled");
    TEST_CHECK(test_backup_import_apply(free_for_zone, err, sizeof(err)),
               "aux gives relay 3 up while a zone takes it");
    TEST_CHECK(s_writes[0].relay_mask == 0x04, "zone 0 now owns relay 3");
    TEST_CHECK(aux_outputs_cfg_enabled_mask() == 0, "aux relay 3 is disabled");
}

static void test_aux_outputs_dry_run_and_partial_write(void)
{
    TEST_SECTION("backup_import_apply -- aux_outputs dry run writes nothing; a failed persist is a partial write");
    reset_stub_state();
    aux_bk_fresh();
    kiln_cfg_plan_t plan;
    bool partial = false;
    char err[200] = "";
    TEST_CHECK(wp9_apply_full(",\"aux_outputs\":[{\"relay\":2,\"enabled\":true}]", KILN_CFG_RESTORE_MERGE, true, -1,
                              &plan, &partial, err, sizeof(err)),
               "dry run succeeds");
    TEST_CHECK(wp9_plan_has(&plan, "aux_outputs"), "the plan names aux_outputs");
    TEST_CHECK(aux_outputs_cfg_enabled_mask() == 0, "dry run: the aux store is untouched");

    reset_stub_state();
    aux_bk_fresh();
    fake_kv_script_next_write_status(HAL_IO);
    bool ok = wp9_apply_full(",\"aux_outputs\":[{\"relay\":2,\"enabled\":true}]", KILN_CFG_RESTORE_MERGE, false, -1,
                             &plan, &partial, err, sizeof(err));
    TEST_CHECK(!ok, "a failed aux persist fails the restore");
    TEST_CHECK(partial, "reported as a partial write (the handler turns this into the 500)");
    TEST_CHECK(strstr(err, "aux_outputs") != NULL, "the error names aux_outputs");
    TEST_CHECK(g_profile_save_calls > 0, "the profiles had already been written (hence partial)");
}

static const char *aux_bk_slice(char *dst, size_t cap, const char *doc)
{
    const char *p = strstr(doc, "\"aux_outputs\":[");
    const char *q = p ? strchr(p, ']') : NULL;
    if (p == NULL || q == NULL || (size_t)(q - p) + 2 > cap) {
        dst[0] = '\0';
        return dst;
    }
    memcpy(dst, p, (size_t)(q - p) + 1);
    dst[(size_t)(q - p) + 1] = '\0';
    return dst;
}

static void test_aux_outputs_export_round_trip(void)
{
    TEST_SECTION("backup_export_get_handler -- aux_outputs round-trips export, import, export byte-identically");
    reset_stub_state();
    aux_bk_fresh();
    TEST_CHECK(run_export() == ESP_OK &&
                   strstr(s_export_body, "\"aux_outputs\":[{\"relay\":1,\"enabled\":false,\"tc_zone\":-1") != NULL,
               "a stock board exports four disabled entries");
    aux_output_entry_t a = aux_bk_entry(true, 3, 4.25f, 10, 20);
    aux_output_entry_t b = aux_bk_entry(true, 0, 1.5f, 33, 44);
    aux_output_entry_t c = aux_bk_entry(false, 1, 8.0f, 5, 6);
    TEST_CHECK(aux_outputs_cfg_set(1, &a, 0) == ESP_OK && aux_outputs_cfg_set(3, &b, 0) == ESP_OK &&
                   aux_outputs_cfg_set(4, &c, 0) == ESP_OK,
               "configure three relays");
    TEST_CHECK(run_export() == ESP_OK, "export succeeds");
    char first[600], second[600];
    (void)aux_bk_slice(first, sizeof(first), s_export_body);
    TEST_CHECK(strstr(first, "\"relay\":1,\"enabled\":true,\"tc_zone\":2,\"hyst_c\":4.25,\"min_on_s\":10,"
                             "\"min_off_s\":20") != NULL,
               "relay 1 exported with its exact values");
    char *saved = strdup(s_export_body);
    TEST_CHECK(saved != NULL, "copy the exported document");
    if (saved != NULL) {
        aux_bk_fresh(); // a blank board
        char err[200] = "";
        TEST_CHECK(test_backup_import_apply(saved, err, sizeof(err)), "the exported document imports back");
        TEST_CHECK(run_export() == ESP_OK, "re-export succeeds");
        (void)aux_bk_slice(second, sizeof(second), s_export_body);
        TEST_CHECK(first[0] != '\0' && strcmp(first, second) == 0, "aux_outputs is byte-identical after the round trip");
        free(saved);
    }
    aux_bk_fresh();
}

void run_test_backup_import(void)
{
    test_update_settings_http_post();
    test_update_repo_backslash_refused();
    test_update_repo_default_value_is_unset();
    test_update_repo_dry_run_names_old_and_new();
    test_update_repo_persist_failure_is_partial_write();
    test_update_repo_pass1_refusal_skips_kiln_configs_commit();
    test_update_repo_imports_and_persists();
    test_update_repo_absent_is_noop();
    test_update_repo_invalid_refuses_whole_restore();
    test_update_repo_export_round_trip();
    test_aux_outputs_import_applies_and_persists();
    test_aux_outputs_absent_is_noop();
    test_aux_outputs_malformed_refuses_whole_restore();
    test_aux_outputs_quarantined_store_refuses_import();
    test_aux_outputs_conflict_with_zone_relay_refused_in_pass_1();
    test_aux_outputs_zone_and_aux_swap_applies_in_either_direction();
    test_aux_outputs_dry_run_and_partial_write();
    test_aux_outputs_export_round_trip();
    test_backup_tuning_float_matches_huge_values();
    test_backup_import_post_refused_by_mode_gate_before_interlock();
    test_backup_import_post_refused_by_interlock_after_mode_gate_passes();
    test_backup_import_post_refused_by_sweep_reason_via_interlock();
    test_backup_import_job_rechecks_mode_gate_before_writing();
    test_backup_import_job_recheck_refused_by_sweep_reason_via_interlock();
    test_backup_import_job_clears_restore_in_flight_flag();

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
    test_import_exactly_max_count_profiles_succeeds();
    test_import_over_max_count_profiles_refused();
    test_import_live_edit_slot_id_rejected();
    test_import_id_past_max_count_rejected();
    test_import_intra_batch_duplicate_name_refused_nothing_written();
    test_import_name_collides_with_existing_non_overwritten_slot_refused();
    test_import_name_overwriting_the_slot_holding_that_name_allowed();
    test_import_write_ids_sim_matches_pass2_commit_order();
    test_import_allows_profile_named_like_a_builtin();
    test_no_hostile_backup_input_produces_a_bootable_heat_commanding_state();

    test_v4_new_fields_round_trip_distinct_values();
    test_coupling_diag_k_dc_round_trips_distinct_value();
    test_v2_body_imports_new_fields_default_floats_zero_source_custom();
    test_v3_fuzzy_strength_out_of_range_rejected();
    test_v3_coupling_neighbor_fractional_rejected();
    test_v3_legacy_pair_maps_into_row_cell();
    test_v3_legacy_pair_self_reference_rejected_before_any_commit();
    test_v4_key_wins_over_legacy_pair_for_the_same_cell();
    test_v4_coupling_diagonal_rejected();
    test_v3_settings_source_self_reference_rejected();
    test_settings_source_cross_entry_cycle_rejected_before_any_commit();
    test_settings_source_cross_entry_legal_chain_still_imports();
    test_settings_source_restore_onto_differently_configured_board_succeeds();
    test_settings_source_commit_failure_restores_pre_import_values();
    test_backup_import_batched_save_fires_exactly_once();
    test_backup_import_failure_restores_whole_batch_not_just_settings_source();
    test_backup_import_refuses_zone_relay_claimed_by_aux();
    test_backup_import_success_tracks_pico_ceiling_down_to_new_max();
    test_backup_import_rollback_tracks_pico_ceiling_back_to_restored_max();

    test_export_emits_expected_keys_and_values_for_a_known_config();
    test_backup_export_kiln_config_package_present();
    test_kiln_configs_absent_key_is_a_no_op();
    test_kiln_configs_present_empty_merge_is_a_no_op();
    test_kiln_configs_malformed_entry_refuses_whole_restore();
    test_kiln_configs_restore_create_leaves_autosave_targeting_the_prior_active_slot();
    test_export_emits_live_pico_tc_type_not_stale_esp_cache();
    test_export_round_trips_through_import_to_identical_config();
    test_ct_normals_and_new_fields_round_trip_through_export_import();
    test_import_of_identical_tuning_quality_does_not_bump_seq();
    test_import_refuses_duplicate_zone_index();
    test_import_identity_roundtrip_keeps_tuning_seq_and_validity();
    test_timing_profiles_bundle_round_trips_nonempty();
    test_export_preserves_coupling_matrix_when_a_zone_is_on_off();
    test_import_tau_only_cell_preserves_coeff_touching_on_off_zone();

    test_v4_coupling_tau_dead_time_round_trip_asymmetric_per_pair();
    test_export_import_export_is_byte_identical_for_non_round_floats();
    test_v4_coupling_tau_dead_time_omitted_entirely_preserves_measured_values();
    test_v4_coupling_tau_dead_time_partial_per_cell_presence();

    test_safety_i_normal_a_export_omitted_when_unset();
    test_safety_i_normal_a_round_trips_through_export_import();
    test_safety_i_normal_a_write_failure_aborts_whole_import();
    test_safety_i_normal_a_absent_key_never_calls_write();

    test_export_never_contains_credential_markers();

    test_mirror_delete_absent_ack_refuses_and_writes_nothing();
    test_mirror_delete_wrong_count_refuses_and_writes_nothing();
    test_mirror_delete_correct_count_proceeds_and_keeps_active_slot();
    test_kiln_configs_partial_write_set_on_mid_pass_create_failure();
}
