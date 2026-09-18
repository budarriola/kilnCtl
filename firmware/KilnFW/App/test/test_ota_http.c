// Host tests for App/drivers/http/ota_http.c's security fixes shipped in commit
// f58e040 with NO host-test coverage (ota_http.c needs real ESP-IDF headers
// the host tree had no stubs for) -- verified only by code reading and on
// real hardware until now. This file closes that gap for the three
// decisions the commit made:
//
//   1. ota_http_verify_request() REFUSES outright when the AP password is
//      empty (an open AP makes the HMAC key zero-length, so anyone in range
//      could compute a valid MAC from public information) -- checked AFTER
//      the BOOT-button physical-recovery bypass (so an open-AP board is
//      never unflashable) and BEFORE any HMAC math runs.
//   2. Each ota_http_context_t has its own distinct HMAC context string and
//      its own lockout state, so a MAC signed for one action cannot
//      authorize another, and a wrong-password guess against one route
//      cannot burn another route's 3-strikes budget.
//   3. POST /api/factory_reset authenticates BEFORE its interlock check, so
//      an unauthenticated caller cannot read the refusal text (which can
//      name a live zone temperature) to learn kiln telemetry.
//
// APPROACH: ota_http_verify_request()/ota_http_authenticate_request() are
// PUBLIC (declared in ota_http.h), so in principle this file could just link
// ota_http.c as an ordinary object and call them. It does not, for one
// reason: exercising decision 2 needs a freshly issued, still-valid
// challenge nonce, and the nonce lives in ota_http.c's own file-scope static
// state (s_nonce) with no public "issue one for a test" entry point -- the
// only way to reach it is to #include ota_http.c directly, same convention
// test_zones_http.c/test_profiles_http.c already use for a `static`
// function with no other seam. This file needs the same access for a
// `static` file-scope VARIABLE instead of a function, but the reasoning and
// the mechanism are identical. reset_post_handler() (factory_reset.c),
// decision 3's target, is `static` for the ordinary reason -- #include'd for
// the same reason as those two precedents.
//
// Both .c files are #include'd into this ONE translation unit (they do not
// call each other's static internals, only the public ota_http.h surface --
// no seam conflict), which means both define a file-scope `static const
// char *TAG` with a different string literal. Two static objects with the
// SAME name in the SAME translation unit is a redefinition error in C, so
// each include is bracketed with a `#define TAG <other-name>` / `#undef` so
// the preprocessor renames it away before the clash -- the value is never
// read by any test here (nothing checks log output), so the label itself is
// arbitrary.
//
// FAKE CRYPTO, ON PURPOSE: stubs/psa/crypto.h supplies a deterministic,
// order-and-input-sensitive (but NOT cryptographically real) HMAC/hash
// stand-in -- see that file's own header comment for why that is the right
// choice here and how it is kept from being vacuous (changing the key,
// message, or their order changes every output byte, so "wrong MAC rejected"
// and "different context -> different digest" are still real properties
// being checked, not tautologies of a stub that always says yes).
//
// NEGATIVE-TEST NOTE (repo rule: every check must be provable to fail): each
// of the three decisions above was confirmed RED by temporarily editing
// ota_http.c/factory_reset.c, rebuilding, and rerunning this file's
// corresponding test, then restoring the file exactly and re-confirming
// GREEN -- see this pass's report for the specific edit and red count used
// for each. The edits are not left in this file (they were applied directly
// to the driver files, run, and reverted); this comment records what was
// done since the driver files themselves carry no trace of it.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

// Ahead of ota_http.c's/factory_reset.c's own #includes, purely for the
// TYPES the stub bodies below need (same convention test_zones_http.c/
// test_profiles_http.c use).
#include "esp_err.h"
#include "esp_http_server.h"

// ota_http.c calls esp_restart() (ota_recovery_exit_reboot_task()) without
// including esp_system.h itself -- forward-declared here, matching real
// ESP-IDF's signature, so the compiler doesn't implicitly assume `int
// esp_restart()` before factory_reset.c's later #include "esp_system.h"
// (stubs/esp_system.h) declares the real `void esp_restart(void)` and the
// two conflict. Defined further down.
void esp_restart(void);

// asm("_binary_...") is a GCC/binutils extension (EMBED_TXTFILES,
// CMakeLists.txt) with no MSVC equivalent -- #define it away to nothing, same
// convention every other host test that #includes a driver .c with an
// embedded page uses. Real (empty) definitions of the two symbols this
// exposes follow the includes below.
#define asm(x)

// stubs/freertos/semphr.h's xSemaphoreTake() deliberately ALWAYS returns
// pdFALSE on a non-NULL handle too (test_boot_button.c's own header comment:
// "deliberately always returns pdFALSE" -- every OTHER host test that
// touches a mutex only exercises the NULL-handle pre-start guard, never the
// locked/critical-section path itself, so that shared stub can afford to be
// that blunt). This file is different: ota_http_verify_request()'s actual
// decision logic runs INSIDE s_ota_lock, so a Take that never succeeds would
// make every test here dead-end at "internal lock timeout, refused" instead
// of reaching the code under test. Rather than change the shared stub (and
// risk test_boot_button.c's own documented assumption), this pulls in the
// real freertos/semphr.h first (under its true name, still asserting on a
// NULL handle -- the safety property test_profile_executor_prestart.c/
// test_autotune_engine_prestart.c rely on stays intact), then macro-
// redirects xSemaphoreTake to a LOCAL replacement -- real single-threaded
// mutex semantics (always succeeds for a non-NULL handle) -- for exactly the
// two files this test #includes below. Restored (#undef) immediately after,
// so nothing outside those two files' bodies is affected.
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <assert.h>
static inline BaseType_t ota_http_test_xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    assert(sem != NULL && "xSemaphoreTake on a NULL handle -- would assert/panic on real FreeRTOS");
    (void)ticks;
    return pdTRUE; // host tests are single-threaded -- a valid handle is always available
}
#define xSemaphoreTake ota_http_test_xSemaphoreTake

// See this file's header comment for why TAG has to be renamed around each
// include rather than left alone.
#define TAG OTA_HTTP_TAG_UNUSED
#include "../drivers/http/ota_http.c"
#undef TAG
// ota_http.c split 2026-09-04 (ROADMAP.md M15 A3, "files over 1500 lines
// should be broken up where it makes sense" -- ota_http.c had grown to 2508
// lines) into ota_http.c/ota_http_esp.c/ota_http_pico.c/ota_http_recovery.c
// -- see ota_http_internal.h's header comment for the file map. This test
// reaches ota_pico_rollback_post_handler()/ota_pico_rollback_status_get_
// handler() and the pico-rollback async state directly (both now defined
// in ota_http_pico.c, see below), same "#include the .c directly" reasoning
// as ota_http.c above -- no new TAG guard needed here, `TAG` itself was
// renamed to the non-static `OTA_HTTP_TAG` by that split (ota_http_
// internal.h), so the three files below only ever `extern` it, never
// redefine it. Same wifi_prov.c split precedent as test_wifi_prov.c's own
// header comment on its four #includes.
#include "../drivers/http/ota_http_esp.c"
#include "../drivers/http/ota_http_pico.c"
#include "../drivers/http/ota_http_recovery.c"

#define TAG FACTORY_RESET_TAG_UNUSED
#include "../drivers/http/factory_reset.c"
#undef TAG

// sw_reset_http.c -- owner request 2026-09-08's non-destructive reboot item.
// Same "#include the .c directly" reasoning as factory_reset.c just above:
// sw_reset_post_handler() is `static`, and this file needs it directly to
// prove the firing-refusal property (this pass's negative test). Its own TAG
// definition needs the same rename-around-the-include treatment.
#define TAG SW_RESET_TAG_UNUSED
#include "../drivers/http/sw_reset_http.c"
#undef TAG

#undef xSemaphoreTake
#undef asm

// web_auth_store.h -- docs/WEB_AUTH_PLAN.md item 12b's factory-reset half:
// a credential must survive every one of factory_reset.c's four scopes
// (wifi/kiln/profiles/all), none of which may erase the default `nvs`
// partition kiln_auth lives on (see factory_reset.c's kScopes[] table just
// #included above -- WIFI_NVS_PARTITION/KILN_NVS_PARTITION/
// PROFILES_NVS_PARTITION are "wifi_nvs"/"kiln_nvs"/"profiles_nvs", never
// the default partition). Linked as a REAL, separately-compiled object
// (build_host_tests.ps1's cmd8), not #include'd -- it has a public header
// and no static internals this file needs to reach, unlike ota_http.c/
// factory_reset.c above. g_stub_psa_import_key_result (just above) is the
// one extern web_auth_store.c's psa/crypto.h stub needs; already defined
// here for ota_http.c's own HMAC use.
#include "web_auth_store.h"
#include "fake_kv.h" // fake_kv_reset_all() -- the credential-survival tests below need a clean
                     // hal_kv state per scope, same convention as test_web_auth_store.c's reset_all()

// GET /api/ota/esp/status build-identity redaction tests (below, near
// test_boot_guard_status_reports_count_and_recovery_mode()) need the real
// session/auth seam: http_auth_caller_is_admin() (http_auth_http.c, linked
// as a real object per build_host_tests.ps1's cmd8), the real session table
// (http_session_iface.c's http_session_table(), the same instance
// http_auth_caller_is_admin()'s own cookie resolution reads through
// http_auth_session_resolve()), web_auth_table_create_session() to mint a
// real session by hash (net/web_auth_session.h), and
// web_auth_store_set_policy() to flip web_enabled on/off (section 11's
// "auth off == today" case). All four are already linked into this exact
// test executable -- no build-script change needed.
#include "http_auth_http.h"
#include "web_auth_session.h"
#include "http_session_iface.h"

// http_session_table() has external linkage (http_session_iface.c) but is
// deliberately NOT declared in http_session_iface.h -- that header's own
// comment reserves it for section 6's login handler, forward-declared at
// its own call site rather than exposed as general public API. Same
// forward-declare-at-point-of-use precedent this file needs here.
web_auth_table_t *http_session_table(void);

// ---- ota_http.c's embedded-page symbols -----------------------------------
const uint8_t ota_page_html_gz_start[1] = { 0 };
const uint8_t ota_page_html_gz_end[1] = { 0 };

// ---------------------------------------------------------------------------
// psa/crypto.h / esp_app_desc.h test-controllable globals (declared extern
// in those stub headers, defined once here).
// ---------------------------------------------------------------------------
psa_status_t g_stub_psa_import_key_result = PSA_SUCCESS;
const esp_app_desc_t *g_stub_esp_app_desc = NULL;

// ---------------------------------------------------------------------------
// lwip/sockets.h -- declared extern there for wifi_prov.c's host tests;
// referenced by that header's static inline getsockname()/inet_ntop(), which
// MSVC still needs a body for even though neither is ever actually called by
// any test in this file (get_client_ip()'s getpeername() always fails first).
// ---------------------------------------------------------------------------
int g_stub_getsockname_result = -1;
char g_stub_local_ip[16] = "";

// ---------------------------------------------------------------------------
// esp_system.h -- ota_recovery_exit_post_handler()'s reboot task calls this;
// never actually invoked by any test here (that task function is never run --
// xTaskCreate() is stubbed to never call its argument), but must resolve.
// ---------------------------------------------------------------------------
void esp_restart(void) {}

// ---------------------------------------------------------------------------
// hal_wdt.h -- HAL Phase 3 item 7 migrated factory_reset.c's reboot_task()
// and ota_http_recovery.c's ota_recovery_exit_reboot_task() off esp_restart()
// directly onto hal_wdt_reboot(). Same "never actually invoked" reasoning as
// esp_restart() above: neither reboot task ever runs in this file's tests
// (xTaskCreate() is stubbed to never call its argument), but the symbol must
// still resolve. A trivial no-op stub, not fake_wdt.c, matching this file's
// existing convention of defining its own minimal stand-ins for symbols it
// never actually exercises rather than linking a real backend/fake.
// ---------------------------------------------------------------------------
void hal_wdt_reboot(void) {}

// ---------------------------------------------------------------------------
// profiles_builtin.h -- factory_reset.c's execute_scope() calls this for
// "profiles"/"all" scopes; the tests in this file never reach a scope that
// executes (the interlock check always refuses first), but must resolve.
// ---------------------------------------------------------------------------
esp_err_t profiles_builtin_restore_all(void) { return ESP_OK; }

// ---------------------------------------------------------------------------
// cfg_fs_mount.h (2026-09-07) -- factory_reset.c's "all" scope now also
// formats the `cfg` LittleFS partition (docs/FILESYSTEM_USER_DATA_PLAN.md
// section 5 step 1, owner decision). Never actually exercised by any test in
// this file (same "the interlock check always refuses first" note above --
// no test here reaches a scope's actual execute_scope_job()), but the symbol
// must still resolve since factory_reset.c is compiled directly into this
// file. cfg_fs_mount.c itself is not part of this (or any) host test build
// (it needs esp_littlefs.h/esp_partition.h, ESP-IDF only).
esp_err_t cfg_fs_confirm_format_device(void) { return ESP_OK; }

// ---------------------------------------------------------------------------
// uart_bridge.h/flash_worker.h (2026-09-07) -- execute_scope() now dispatches
// its NVS erase through uart_bridge_ext_run_on_flash_worker() (see
// flash_worker_lint.py's factory_reset.c entry) instead of calling
// hal_kv_erase_partition() inline. Never actually exercised by any test in
// this file (the interlock check always refuses first, same "never reach a
// scope that executes" note above), but the symbols must still resolve --
// trivial stubs that just run fn() inline, not the shared busy-modeling
// bx_worker_stub.h (that header models re-entrancy across a dispatch this
// file never performs; a plain pass-through is enough here and keeps this
// file from taking on a dependency it does not exercise).
esp_err_t uart_bridge_ext_run_on_flash_worker(void (*fn)(void *arg), void *arg)
{
    fn(arg);
    return ESP_OK;
}
bool uart_bridge_ext_is_on_flash_worker(void) { return false; }

// ---------------------------------------------------------------------------
// esp_random.h migration (2026-09-06): ota_http.c now calls
// hal_sysinfo_fill_random() instead of esp_fill_random() -- fake_sysinfo.c
// (already linked into this executable, see build_host_tests.ps1's cmd8)
// supplies it, so no stub definition is needed here any more.
// Never actually reached by any test in this file
// (ota_challenge_get_handler() is never called).
// ---------------------------------------------------------------------------
// esp_partition.h / esp_ota_ops.h -- declared in those stub headers, defined
// here. None of these is ever invoked by this file's tests (only
// ota_http_verify_request()/ota_http_authenticate_request()/
// reset_post_handler() are exercised, never the transfer handlers), but the
// whole translation unit must still link.
// ---------------------------------------------------------------------------
esp_err_t esp_partition_write(const esp_partition_t *partition, size_t dst_offset, const void *src, size_t size)
{ (void)partition; (void)dst_offset; (void)src; (void)size; return ESP_OK; }
esp_err_t esp_partition_erase_range(const esp_partition_t *partition, size_t offset, size_t size)
{ (void)partition; (void)offset; (void)size; return ESP_OK; }
uint32_t esp_partition_get_main_flash_sector_size(void) { return 4096u; }

const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start_from)
{ (void)start_from; return NULL; }
const esp_partition_t *esp_ota_get_running_partition(void) { return NULL; }
esp_err_t esp_ota_begin(const esp_partition_t *partition, size_t image_size, esp_ota_handle_t *out_handle)
{ (void)partition; (void)image_size; if (out_handle) *out_handle = 1; return ESP_OK; }
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size)
{ (void)handle; (void)data; (void)size; return ESP_OK; }
esp_err_t esp_ota_end(esp_ota_handle_t handle) { (void)handle; return ESP_OK; }
esp_err_t esp_ota_abort(esp_ota_handle_t handle) { (void)handle; return ESP_OK; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition) { (void)partition; return ESP_OK; }
esp_err_t esp_ota_get_partition_description(const esp_partition_t *partition, esp_app_desc_t *out)
{ (void)partition; (void)out; return ESP_FAIL; }
bool esp_ota_check_rollback_is_possible(void) { return false; }
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void) { return ESP_FAIL; }

// ---------------------------------------------------------------------------
// External driver dependencies -- test-controllable where a test needs to
// steer the decision under test, fixed/trivial otherwise. None of the fixed
// ones is ever asserted on directly; they exist so the whole translation
// unit (every OTHER handler in ota_http.c/factory_reset.c, not just the
// functions under test) links.
// ---------------------------------------------------------------------------

// wifi_prov.h -- THE input decision 1 turns on.
static const char *g_stub_ap_password = "";
const char *wifi_prov_get_ap_password(void) { return g_stub_ap_password; }

// boot_button.h -- the physical-recovery bypass decision 1's ordering is
// checked against.
static bool g_stub_boot_button_bypass = false;
bool boot_button_ota_bypass_active(void) { return g_stub_boot_button_bypass; }
uint32_t boot_button_bypass_remaining_ms(void) { return g_stub_boot_button_bypass ? 5000u : 0u; }

// profile_executor.h -- instrumented: ota_http_check_interlocks() calls this
// FIRST, unconditionally, every time it runs -- used as decision 3's "did
// the interlock check actually run" probe.
bool g_probe_interlock_called = false;
// Settable so sw_reset's firing-refusal test (this pass, owner request
// 2026-09-08) can report a firing in progress; defaults to PROFILE_EXEC_IDLE
// (0) so every existing test in this file that never touches this global is
// completely unaffected.
static profile_exec_state_t g_stub_profile_state = PROFILE_EXEC_IDLE;
void profile_executor_get_status(profile_exec_status_t *out)
{
    g_probe_interlock_called = true;
    if (out) {
        memset(out, 0, sizeof(*out));
        out->state = g_stub_profile_state;
    }
}

// autotune_engine.h -- settable so sw_reset's autotune-refusal test (this
// pass) can report autotune active; defaults false, unaffected otherwise.
static bool g_stub_autotune_active = false;
bool autotune_engine_is_active(void) { return g_stub_autotune_active; }

// safety_link.h -- never actually invoked by any test here (ota_http_safety is
// left NULL for every test -- ota_http_start()'s io_or_null/safety_or_null
// arguments), but must resolve.
esp_err_t safety_link_get_status(SafetyLinkClass *link, safety_link_status_t *out)
{ (void)link; if (out) memset(out, 0, sizeof(*out)); return ESP_FAIL; }
esp_err_t safety_link_get_peer_version_status(SafetyLinkClass *link, bool *out_known, bool *out_compatible,
                                              uint16_t *out_protocol, uint16_t *out_min_compatible)
{
    (void)link;
    if (out_known) *out_known = false;
    if (out_compatible) *out_compatible = false;
    if (out_protocol) *out_protocol = 0;
    if (out_min_compatible) *out_min_compatible = 0;
    return ESP_FAIL;
}
esp_err_t safety_link_send_announce_reboot(SafetyLinkClass *link) { (void)link; return ESP_OK; }

// safety_link.h -- SAFETY_CMD_REBOOT (0x29). Never actually invoked by any
// test here: sw_reset_http.c's own s_safety stays NULL (sw_reset_http_start()
// is never called in this suite), so the handler takes its NO_LINK branch.
// The ESP-side SEQUENCING this stub would otherwise be needed for is covered
// without it, by testing sw_reset_classify_pico_outcome() directly -- see
// test_sw_reset_pico_outcome_mapping(). Same "must resolve, never called"
// role as the safety_link_get_status() stub above.
esp_err_t safety_link_send_reboot(SafetyLinkClass *link, safety_link_reboot_outcome_t *out_outcome,
                                   uint8_t *out_reason_code)
{
    (void)link;
    (void)out_reason_code;
    if (out_outcome) *out_outcome = SAFETY_LINK_REBOOT_OUTCOME_NO_REPLY;
    return ESP_OK;
}
esp_err_t safety_link_send_rollback_ex(SafetyLinkClass *link, safety_link_rollback_outcome_t *out_outcome,
                                        uint8_t *out_reason_code)
{
    (void)link;
    (void)out_reason_code;
    // Never actually invoked by any test in this file -- ota_pico_rollback_
    // post_handler() is not exercised here (ota_http_safety stays NULL, same "must
    // resolve, never called" role as the safety_link_get_status() stub
    // above), so any fixed outcome is fine as a link-time stub.
    if (out_outcome) *out_outcome = SAFETY_LINK_ROLLBACK_OUTCOME_LINK_DOWN;
    return ESP_OK;
}

// run_state.h
bool run_state_boot_record_interrupted(void) { return false; }

// zones_http.h -- thermo_count=0 keeps ota_http_check_interlocks()'s per-zone
// loop a no-op for every test in this file (none of them cares about zone
// temperature/relay state -- only the ORDER interlocks run in relative to
// auth, decision 3's actual subject).
uint8_t zones_config_get_thermo_count(void) { return 0; }

// B2 (opus review, 2026-08-27): ota_http_check_interlocks() now refuses
// early while a zone current sweep is active -- see
// zones_current_sweep_is_active()'s doc comment (zones_http.h). A settable
// stub (default false) so this file's existing interlock-ordering tests are
// unaffected, and test_ota_http_check_interlocks_refuses_during_zone_sweep()
// below can exercise the new refusal itself.
static bool s_test_sweep_active = false;
bool zones_current_sweep_is_active(void) { return s_test_sweep_active; }
bool zones_config_get_relay_mask(uint8_t zone_index, uint8_t *out_mask)
{ (void)zone_index; if (out_mask) *out_mask = 0; return false; }
float zones_config_apply_cal(uint8_t zone_index, float raw_c) { (void)zone_index; return raw_c; }

// MAX31856.h / kiln_io.h -- never reached (thermo_count == 0 above skips the
// loop that would call these), must resolve.
esp_err_t MAX31856_read_all(MAX31856BusClass *bus, MAX31856Reading *out, size_t max_readings, size_t *out_count)
{ (void)bus; (void)out; (void)max_readings; if (out_count) *out_count = 0; return ESP_FAIL; }
esp_err_t kiln_io_read(kiln_io_t *io, kiln_io_state_t *out)
{ (void)io; if (out) memset(out, 0, sizeof(*out)); return ESP_FAIL; }

// ota_pico_relay.h -- never called by any test here.
const esp_partition_t *ota_pico_img_partition(void) { return NULL; }
const char *ota_pico_relay_phase_str(ota_pico_relay_phase_t phase) { (void)phase; return "idle"; }
bool ota_pico_relay_start(SafetyLinkClass *link, uint32_t image_length, uint32_t image_crc32,
                          const char *version16_or_null, const uint8_t image_sha256_or_null[32])
{
    (void)link; (void)image_length; (void)image_crc32; (void)version16_or_null; (void)image_sha256_or_null;
    return false;
}
void ota_pico_relay_get_status(ota_pico_relay_status_t *out) { if (out) memset(out, 0, sizeof(*out)); }

// wifi_provision_http.h -- ota_http_start()/factory_reset_http_start() both
// call this; NULL makes both bail out (ESP_ERR_INVALID_STATE) right after
// doing whatever RAM-only setup they needed (ota_http_start()'s mutex/state
// init already ran by the time it checks this -- see that function's own
// body), before touching an httpd handle this stub cannot provide.
httpd_handle_t wifi_provision_http_get_server(void) { return NULL; }

// boot_guard.h
bool boot_guard_is_recovery_mode(void) { return false; }
void boot_guard_mark_healthy(void) {}
// Test-controllable so ota_boot_guard_reset_post_handler()'s success/failure
// reporting can be exercised without pulling in the real boot_guard.c (its
// own NVS-backed clear-and-verify sequence is covered directly by
// test_boot_guard.c) -- see run_test_ota_http()'s reset before each test.
static bool s_stub_boot_guard_reset_verified = true;
static uint32_t s_stub_boot_guard_count = 0;
bool boot_guard_reset_counter(void) { return s_stub_boot_guard_reset_verified; }
uint32_t boot_guard_get_boot_count(void) { return s_stub_boot_guard_count; }

// web_encoding.h -- only reached from page GET handlers, never called here.
bool web_client_accepts_gzip(httpd_req_t *req) { (void)req; return true; }
esp_err_t web_send_gzip_not_acceptable(httpd_req_t *req, const char *tag, const char *page_name)
{ (void)req; (void)tag; (void)page_name; return ESP_OK; }
void web_set_asset_cache_headers(httpd_req_t *r) { (void)r; }

// ota_interlock.h / heat_interlock.h -- linked for real (see
// build_host_tests.ps1's 8th step) EXCEPT heat_interlock_check(), which
// ota_http.c's ota_http_heat_blocked_by_update() calls and no test here
// reaches; stubbed trivially so the link doesn't need heat_interlock.c too.
heat_interlock_result_t heat_interlock_check(const heat_interlock_snapshot_t *snap, char *reason_out,
                                             size_t reason_cap)
{
    (void)snap;
    if (reason_out && reason_cap) reason_out[0] = '\0';
    return HEAT_INTERLOCK_OK;
}

// ---------------------------------------------------------------------------
// esp_http_server.h stub bodies -- httpd_req_get_hdr_value_len/_str are
// TEST-CONTROLLABLE (a tiny field->value map a test populates before calling
// a handler), everything else is a fixed no-op that also records the LAST
// error code/message sent, which the ordering tests (decision 3) use to
// confirm which stage of reset_post_handler actually ran.
// ---------------------------------------------------------------------------

#define STUB_HDR_MAX 4
// value[] widened from 80 to 300 (2026-09-17, Finding 3 coverage) so a test
// can stage a Cookie header >= the OLD fixed 128-byte httpd-stack buffer
// http_auth_extract_session_token() used to copy it into -- this is host-
// test fixture storage on the test binary's own stack/data, not the ESP
// httpd task's 8 KB stack, so it is not subject to that stack budget.
typedef struct { const char *field; char value[300]; bool set; } stub_hdr_t;
static stub_hdr_t s_stub_hdrs[STUB_HDR_MAX];

static void stub_headers_reset(void)
{
    memset(s_stub_hdrs, 0, sizeof(s_stub_hdrs));
}

static void stub_header_set(const char *field, const char *value)
{
    for (int i = 0; i < STUB_HDR_MAX; i++) {
        if (!s_stub_hdrs[i].set || strcmp(s_stub_hdrs[i].field, field) == 0) {
            s_stub_hdrs[i].field = field;
            strncpy(s_stub_hdrs[i].value, value, sizeof(s_stub_hdrs[i].value) - 1);
            s_stub_hdrs[i].set = true;
            return;
        }
    }
}

size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field)
{
    (void)r;
    for (int i = 0; i < STUB_HDR_MAX; i++) {
        if (s_stub_hdrs[i].set && strcmp(s_stub_hdrs[i].field, field) == 0) {
            return strlen(s_stub_hdrs[i].value);
        }
    }
    return 0;
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t val_size)
{
    (void)r;
    for (int i = 0; i < STUB_HDR_MAX; i++) {
        if (s_stub_hdrs[i].set && strcmp(s_stub_hdrs[i].field, field) == 0) {
            strncpy(val, s_stub_hdrs[i].value, val_size - 1);
            val[val_size - 1] = '\0';
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

int httpd_req_to_sockfd(httpd_req_t *r) { (void)r; return -1; }

esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler)
{ (void)handle; (void)uri_handler; return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type) { (void)r; (void)type; return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value)
{ (void)r; (void)field; (void)value; return ESP_OK; }
// opus-review finding 3's tests below (ota_pico_rollback_status_get_handler(),
// the async POST returning promptly) need to see what a handler actually
// sent, not just whether it returned ESP_OK -- s_last_err_code/_msg above
// only capture the httpd_resp_send_err() path. Same capture shape, for the
// ordinary httpd_resp_send()/httpd_resp_set_status() path.
static char s_last_resp_body[512] = "";
static char s_last_resp_status[32] = "";
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
    return ESP_OK;
}
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, size_t buf_len) { (void)r; (void)buf; (void)buf_len; return ESP_OK; }

static int s_last_err_code = 0;
static char s_last_err_msg[256] = "";
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t error, const char *msg)
{
    (void)r;
    s_last_err_code = (int)error;
    if (msg) {
        strncpy(s_last_err_msg, msg, sizeof(s_last_err_msg) - 1);
        s_last_err_msg[sizeof(s_last_err_msg) - 1] = '\0';
    } else {
        s_last_err_msg[0] = '\0';
    }
    return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status)
{
    (void)r;
    if (status) {
        strncpy(s_last_resp_status, status, sizeof(s_last_resp_status) - 1);
        s_last_resp_status[sizeof(s_last_resp_status) - 1] = '\0';
    } else {
        s_last_resp_status[0] = '\0';
    }
    return ESP_OK;
}
// Records the last success-path body so a test can assert on its WORDING,
// not just on the absence of an error code. Added 2026-09-09 for
// test_sw_reset_body_does_not_claim_trip_clearing(); every pre-existing
// caller is unaffected (this only ever writes to a file-static buffer).
static char s_last_sendstr_body[1024];
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s)
{
    (void)r;
    s_last_sendstr_body[0] = '\0';
    if (s) {
        strncpy(s_last_sendstr_body, s, sizeof(s_last_sendstr_body) - 1);
        s_last_sendstr_body[sizeof(s_last_sendstr_body) - 1] = '\0';
    }
    return ESP_OK;
}
int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len) { (void)r; (void)buf; (void)buf_len; return 0; }

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

// Matches the switch in ota_http_verify_request() -- this is the test's own
// oracle for "which context string goes with which context enum", kept
// deliberately independent of ota_http.c's internal table so a bug that
// changes one without the other is exactly what these tests would catch.
static const char *ctx_str_for(ota_http_context_t ctx)
{
    switch (ctx) {
        case OTA_HTTP_CONTEXT_ESP: return "esp";
        case OTA_HTTP_CONTEXT_PICO: return "pico";
        case OTA_HTTP_CONTEXT_ESP_ROLLBACK: return "esp-rollback";
        case OTA_HTTP_CONTEXT_RECOVERY_EXIT: return "recovery";
        case OTA_HTTP_CONTEXT_FACTORY_RESET: return "factory-reset";
        case OTA_HTTP_CONTEXT_PICO_ROLLBACK: return "pico-rollback";
        case OTA_HTTP_CONTEXT_SW_RESET: return "sw-reset";
        case OTA_HTTP_CONTEXT_BOOT_GUARD_RESET: return "boot-guard-reset";
        default: return "?";
    }
}

// Issues a fresh, valid nonce directly into ota_http.c's file-scope s_nonce
// -- the access this file #includes ota_http.c FOR (see header comment).
static void issue_nonce(uint8_t nonce_out[OTA_AUTH_NONCE_LEN])
{
    for (int i = 0; i < (int)OTA_AUTH_NONCE_LEN; i++) {
        nonce_out[i] = (uint8_t)(i * 7 + 3);
    }
    ota_auth_nonce_issue(&s_nonce, nonce_out, now_ms());
}

// Computes the MAC exactly the way ota_http.c's own ota_http_verify_request()
// does: key = fake_hmac(password, KDF context), mac = fake_hmac(key, nonce||ctx_str).
// Reuses ota_http.c's own static hmac_sha256() (accessible -- see header
// comment) so this is a genuine end-to-end check of the wiring (which key,
// which context string) rather than a reimplementation that could drift.
static void compute_mac(const char *password, const uint8_t nonce[OTA_AUTH_NONCE_LEN], const char *ctx_str,
                        uint8_t mac_out[32])
{
    uint8_t key[32];
    hmac_sha256((const uint8_t *)password, strlen(password), (const uint8_t *)OTA_HTTP_KDF_CONTEXT,
                strlen(OTA_HTTP_KDF_CONTEXT), key);
    uint8_t msg[OTA_AUTH_NONCE_LEN + 16];
    memcpy(msg, nonce, OTA_AUTH_NONCE_LEN);
    size_t ctx_len = strlen(ctx_str);
    memcpy(msg + OTA_AUTH_NONCE_LEN, ctx_str, ctx_len);
    hmac_sha256(key, sizeof(key), msg, OTA_AUTH_NONCE_LEN + ctx_len, mac_out);
}

static void reset_all_lockouts(void)
{
    memset(&s_lockout_esp, 0, sizeof(s_lockout_esp));
    memset(&s_lockout_pico, 0, sizeof(s_lockout_pico));
    memset(&s_lockout_esp_rollback, 0, sizeof(s_lockout_esp_rollback));
    memset(&s_lockout_recovery_exit, 0, sizeof(s_lockout_recovery_exit));
    memset(&s_lockout_factory_reset, 0, sizeof(s_lockout_factory_reset));
    memset(&s_lockout_pico_rollback, 0, sizeof(s_lockout_pico_rollback));
    memset(&s_lockout_sw_reset, 0, sizeof(s_lockout_sw_reset));
    memset(&s_lockout_boot_guard_reset, 0, sizeof(s_lockout_boot_guard_reset));
}

// ---------------------------------------------------------------------------
// Decision 1 -- empty AP password is refused, AFTER the BOOT-button bypass,
// BEFORE any HMAC math.
// ---------------------------------------------------------------------------

static void test_empty_password_refused_with_valid_nonce(void)
{
    TEST_SECTION("ota_http_verify_request -- empty AP password is refused (OTA_HTTP_VERIFY_NO_AP_PASSWORD)");
    reset_all_lockouts();
    g_stub_boot_button_bypass = false;
    g_stub_ap_password = ""; // open AP

    uint8_t nonce[OTA_AUTH_NONCE_LEN];
    issue_nonce(nonce); // must be valid -- proves the refusal isn't just "no nonce"

    uint8_t mac[32] = { 0 }; // irrelevant -- must never be reached
    ota_http_verify_result_t r = ota_http_verify_request(OTA_HTTP_CONTEXT_ESP, mac, "10.0.0.1");

    TEST_CHECK(r == OTA_HTTP_VERIFY_NO_AP_PASSWORD,
              "an empty AP password must be refused outright, distinctly from a bad MAC or a bad nonce");
}

static void test_boot_button_bypass_wins_even_with_empty_password(void)
{
    TEST_SECTION("ota_http_verify_request -- BOOT-button bypass is checked BEFORE the empty-password refusal");
    reset_all_lockouts();
    g_stub_boot_button_bypass = true;
    g_stub_ap_password = ""; // open AP -- bypass must still work, this is the recovery path it exists for

    // No nonce issued at all -- if the bypass check ran after the nonce/
    // password checks, this would fail for a completely different reason
    // (NO_VALID_NONCE), which would hide a reordering bug behind the wrong
    // failure mode. Requiring OK here with no nonce is itself proof the
    // bypass short-circuits everything below it.
    uint8_t mac[32] = { 0 };
    ota_http_verify_result_t r = ota_http_verify_request(OTA_HTTP_CONTEXT_ESP, mac, "10.0.0.1");

    TEST_CHECK(r == OTA_HTTP_VERIFY_OK,
              "the BOOT-button recovery bypass must keep working on an open-AP board -- an operator who "
              "lost the AP password must still be able to recover it");
    g_stub_boot_button_bypass = false;
}

static void test_nonempty_password_valid_mac_succeeds(void)
{
    TEST_SECTION("ota_http_verify_request -- positive control: a real password + correct MAC succeeds");
    reset_all_lockouts();
    g_stub_boot_button_bypass = false;
    g_stub_ap_password = "correct horse battery staple";

    uint8_t nonce[OTA_AUTH_NONCE_LEN];
    issue_nonce(nonce);
    uint8_t mac[32];
    compute_mac(g_stub_ap_password, nonce, ctx_str_for(OTA_HTTP_CONTEXT_ESP), mac);

    ota_http_verify_result_t r = ota_http_verify_request(OTA_HTTP_CONTEXT_ESP, mac, "10.0.0.1");
    TEST_CHECK(r == OTA_HTTP_VERIFY_OK, "a correctly computed MAC against a real password must succeed");
}

// ---------------------------------------------------------------------------
// Decision 2 -- distinct context strings AND distinct lockout state.
// ---------------------------------------------------------------------------

static void test_mac_for_one_context_rejected_for_another(void)
{
    TEST_SECTION("ota_http_verify_request -- a MAC computed for one context is rejected under another");
    reset_all_lockouts();
    g_stub_boot_button_bypass = false;
    g_stub_ap_password = "hunter2hunter2";

    uint8_t nonce[OTA_AUTH_NONCE_LEN];
    issue_nonce(nonce);
    // Computed for "esp", presented against OTA_HTTP_CONTEXT_FACTORY_RESET.
    uint8_t mac[32];
    compute_mac(g_stub_ap_password, nonce, ctx_str_for(OTA_HTTP_CONTEXT_ESP), mac);

    ota_http_verify_result_t r = ota_http_verify_request(OTA_HTTP_CONTEXT_FACTORY_RESET, mac, "10.0.0.1");
    TEST_CHECK(r == OTA_HTTP_VERIFY_BAD_MAC,
              "a MAC signed for pushing an ESP image must not double as authorization to factory-reset "
              "the board -- contexts must use distinct context strings");
}

// Rollback-of-the-Pico-specific isolation, both directions -- the task this
// pass was built for ("add a rollback button for the SAFETY processor")
// explicitly calls out this exact pair as security-critical: a MAC signed
// for pushing a new Pico image must not authorize rolling it back, and a
// MAC signed to roll the Pico back must not authorize pushing it a new
// image either. test_mac_for_one_context_rejected_for_another() above
// already proves the general property (esp vs factory-reset); this proves
// it specifically for the pair this feature adds, in both directions.
static void test_pico_and_pico_rollback_macs_are_not_interchangeable(void)
{
    TEST_SECTION("ota_http_verify_request -- a MAC signed for 'pico' is REJECTED for 'pico-rollback', and vice versa");
    reset_all_lockouts();
    g_stub_boot_button_bypass = false;
    g_stub_ap_password = "hunter2hunter2";

    // Direction 1: signed for PICO, presented against PICO_ROLLBACK.
    {
        uint8_t nonce[OTA_AUTH_NONCE_LEN];
        issue_nonce(nonce);
        uint8_t mac[32];
        compute_mac(g_stub_ap_password, nonce, ctx_str_for(OTA_HTTP_CONTEXT_PICO), mac);
        ota_http_verify_result_t r = ota_http_verify_request(OTA_HTTP_CONTEXT_PICO_ROLLBACK, mac, "10.0.0.1");
        TEST_CHECK(r == OTA_HTTP_VERIFY_BAD_MAC,
                  "a MAC signed to push a new Pico IMAGE must not double as authorization to ROLL IT BACK");
    }

    // Direction 2: signed for PICO_ROLLBACK, presented against PICO.
    {
        uint8_t nonce[OTA_AUTH_NONCE_LEN];
        issue_nonce(nonce);
        uint8_t mac[32];
        compute_mac(g_stub_ap_password, nonce, ctx_str_for(OTA_HTTP_CONTEXT_PICO_ROLLBACK), mac);
        ota_http_verify_result_t r = ota_http_verify_request(OTA_HTTP_CONTEXT_PICO, mac, "10.0.0.1");
        TEST_CHECK(r == OTA_HTTP_VERIFY_BAD_MAC,
                  "a MAC signed to ROLL BACK the Pico must not double as authorization to push it a new IMAGE");
    }

    // Also distinct from ESP_ROLLBACK's own context (rolling back the WRONG
    // processor) -- same "different processor, different action, different
    // context string" property PICO_ROLLBACK's own ota_http.h doc comment
    // states.
    {
        uint8_t nonce[OTA_AUTH_NONCE_LEN];
        issue_nonce(nonce);
        uint8_t mac[32];
        compute_mac(g_stub_ap_password, nonce, ctx_str_for(OTA_HTTP_CONTEXT_ESP_ROLLBACK), mac);
        ota_http_verify_result_t r = ota_http_verify_request(OTA_HTTP_CONTEXT_PICO_ROLLBACK, mac, "10.0.0.1");
        TEST_CHECK(r == OTA_HTTP_VERIFY_BAD_MAC,
                  "a MAC signed to roll back the ESP must not double as authorization to roll back the Pico");
    }

    // A CORRECT PICO_ROLLBACK-signed MAC, presented against PICO_ROLLBACK,
    // must still succeed -- proves the rejections above are about context
    // mismatch, not a broken context string breaking the route entirely.
    {
        uint8_t nonce[OTA_AUTH_NONCE_LEN];
        issue_nonce(nonce);
        uint8_t mac[32];
        compute_mac(g_stub_ap_password, nonce, ctx_str_for(OTA_HTTP_CONTEXT_PICO_ROLLBACK), mac);
        ota_http_verify_result_t r = ota_http_verify_request(OTA_HTTP_CONTEXT_PICO_ROLLBACK, mac, "10.0.0.1");
        TEST_CHECK(r == OTA_HTTP_VERIFY_OK, "a correctly-signed pico-rollback MAC against pico-rollback succeeds");
    }
}

static void test_lockout_is_per_context_not_shared(void)
{
    TEST_SECTION("ota_http_verify_request -- 3 failures against one context lock only THAT context");
    reset_all_lockouts();
    g_stub_boot_button_bypass = false;
    g_stub_ap_password = "another-real-password";

    uint8_t wrong_mac[32];
    memset(wrong_mac, 0xAB, sizeof(wrong_mac)); // never a valid MAC for anything staged here

    for (int i = 0; i < 3; i++) {
        uint8_t nonce[OTA_AUTH_NONCE_LEN];
        issue_nonce(nonce);
        ota_http_verify_result_t r = ota_http_verify_request(OTA_HTTP_CONTEXT_ESP, wrong_mac, "10.0.0.1");
        TEST_CHECK(r == OTA_HTTP_VERIFY_BAD_MAC, "each of the 3 setup failures must itself be a bad-MAC result");
    }

    // 4th attempt against ESP: must now be locked out, even with a fresh
    // valid nonce -- proves the 3 failures above actually accumulated.
    {
        uint8_t nonce[OTA_AUTH_NONCE_LEN];
        issue_nonce(nonce);
        ota_http_verify_result_t r = ota_http_verify_request(OTA_HTTP_CONTEXT_ESP, wrong_mac, "10.0.0.1");
        TEST_CHECK(r == OTA_HTTP_VERIFY_LOCKED_OUT, "the ESP context must now be locked out after 3 failures");
    }

    // A CORRECT attempt against the FACTORY_RESET context, right now, must
    // still succeed -- its lockout budget must be untouched by ESP's 3
    // failures above.
    {
        uint8_t nonce[OTA_AUTH_NONCE_LEN];
        issue_nonce(nonce);
        uint8_t mac[32];
        compute_mac(g_stub_ap_password, nonce, ctx_str_for(OTA_HTTP_CONTEXT_FACTORY_RESET), mac);
        ota_http_verify_result_t r = ota_http_verify_request(OTA_HTTP_CONTEXT_FACTORY_RESET, mac, "10.0.0.1");
        TEST_CHECK(r == OTA_HTTP_VERIFY_OK,
                  "factory-reset's own lockout state must be untouched by ESP's 3 wrong-password guesses "
                  "-- a wrong guess against one route must not burn through another route's budget");
    }
}

// ---------------------------------------------------------------------------
// Decision 3 -- POST /api/factory_reset authenticates BEFORE its interlock
// check. Exercised through the real (static, #include'd) reset_post_handler().
// ---------------------------------------------------------------------------

static void test_missing_auth_header_never_reaches_interlock(void)
{
    TEST_SECTION("reset_post_handler -- a missing X-Ota-Mac header is refused BEFORE the interlock check runs");
    reset_all_lockouts();
    stub_headers_reset(); // no X-Ota-Mac header at all
    g_probe_interlock_called = false;
    s_last_err_code = 0;
    s_last_err_msg[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = reset_post_handler(&req);

    TEST_CHECK(err == ESP_OK, "reset_post_handler must always return ESP_OK (errors go through httpd_resp_send_err)");
    TEST_CHECK(s_last_err_code == 400, "a missing X-Ota-Mac header must be refused with 400, from the auth step");
    TEST_CHECK(strstr(s_last_err_msg, "X-Ota-Mac") != NULL, "the refusal must name the missing header");
    TEST_CHECK(!g_probe_interlock_called,
              "DECISION 3: the interlock check (which can leak a live zone temperature in its refusal "
              "text) must NEVER run for an unauthenticated caller -- profile_executor_get_status() "
              "(the first call once ota_http_check_interlocks() gets past its own zone-sweep-active "
              "check, B2) must not have been reached");
}

static void test_authenticated_request_does_reach_interlock(void)
{
    TEST_SECTION("reset_post_handler -- a correctly authenticated request DOES reach the interlock check");
    reset_all_lockouts();
    g_stub_boot_button_bypass = false;
    g_stub_ap_password = "factory-reset-test-password";

    // Belt-and-suspenders: confirm the MAC this test constructs is actually
    // accepted by ota_http_authenticate_request() in isolation first (its
    // own header/hex-decode/verify sequence), consuming its own nonce.
    {
        uint8_t nonce[OTA_AUTH_NONCE_LEN];
        issue_nonce(nonce);
        uint8_t mac[32];
        compute_mac(g_stub_ap_password, nonce, ctx_str_for(OTA_HTTP_CONTEXT_FACTORY_RESET), mac);
        char hex[65];
        hex_encode(mac, sizeof(mac), hex);
        stub_headers_reset();
        stub_header_set("X-Ota-Mac", hex);
        httpd_req_t probe_req;
        memset(&probe_req, 0, sizeof(probe_req));
        char ip[46];
        bool ok = ota_http_authenticate_request(&probe_req, OTA_HTTP_CONTEXT_FACTORY_RESET, ip);
        TEST_CHECK(ok, "setup sanity: the constructed MAC must itself authenticate for OTA_HTTP_CONTEXT_FACTORY_RESET");
    }

    // Now the real call under test, with its own fresh nonce/MAC (the one
    // above already consumed its nonce).
    uint8_t nonce2[OTA_AUTH_NONCE_LEN];
    issue_nonce(nonce2);
    uint8_t mac2[32];
    compute_mac(g_stub_ap_password, nonce2, ctx_str_for(OTA_HTTP_CONTEXT_FACTORY_RESET), mac2);
    char hex2[65];
    hex_encode(mac2, sizeof(mac2), hex2);
    stub_headers_reset();
    stub_header_set("X-Ota-Mac", hex2);

    g_probe_interlock_called = false;
    s_last_err_code = 0;
    s_last_err_msg[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = reset_post_handler(&req);

    TEST_CHECK(err == ESP_OK, "reset_post_handler must always return ESP_OK");
    TEST_CHECK(s_last_err_code != 400, "a correctly authenticated request must not be refused at the auth step");
    TEST_CHECK(g_probe_interlock_called,
              "DECISION 3 (positive half): once auth succeeds, the interlock check DOES run -- proving "
              "the auth step is not a no-op ahead of an interlock check that runs unconditionally either way");
}

// ---------------------------------------------------------------------------
// B2 (opus review, 2026-08-27) -- ota_http_check_interlocks() must refuse,
// specifically because of the sweep and BEFORE ever consulting profile/
// autotune/link state, while zones_current_sweep_is_active() is true.
// ---------------------------------------------------------------------------

static void test_check_interlocks_refuses_during_zone_sweep(void)
{
    TEST_SECTION("ota_http_check_interlocks() refuses while a zone current sweep is active (B2)");
    g_probe_interlock_called = false;
    s_test_sweep_active = true;

    char reason[OTA_INTERLOCK_REASON_MAX];
    reason[0] = '\0';
    ota_interlock_result_t r = ota_http_check_interlocks(false, reason, sizeof(reason));

    TEST_CHECK(r == OTA_INTERLOCK_REFUSED, "a live zone sweep must refuse the update, not merely warn");
    TEST_CHECK(strstr(reason, "sweep") != NULL, "the refusal reason must name the sweep, not a generic message");
    TEST_CHECK(!g_probe_interlock_called,
              "the sweep-active check must short-circuit BEFORE profile_executor_get_status() runs -- "
              "cheapest and orthogonal to kiln state, same reasoning as the update-mutex check");

    s_test_sweep_active = false;
}

static void test_check_interlocks_ok_when_no_sweep(void)
{
    TEST_SECTION("ota_http_check_interlocks() is unaffected when no sweep is running (B2 control case)");
    g_probe_interlock_called = false;
    s_test_sweep_active = false;

    char reason[OTA_INTERLOCK_REASON_MAX];
    reason[0] = '\0';
    ota_http_check_interlocks(false, reason, sizeof(reason));

    TEST_CHECK(g_probe_interlock_called,
              "with no sweep active, the check must proceed past the new gate into the real interlock logic");
}

// ---------------------------------------------------------------------------
// opus-review finding 3 -- POST /api/ota/pico/rollback used to block its one
// httpd worker task for up to ~6.3s inside safety_link_send_rollback_ex().
// It now hands the whole attempt to a background task (ota_pico_rollback_
// task()) and returns a 202 "pending" immediately; the outcome is polled
// from GET /api/ota/pico/rollback/status. xTaskCreate() is stubbed (this
// file's own header comment on esp_restart()) to accept the task but never
// actually run it, so a test that reaches the async branch and observes an
// immediate 202 response -- with safety_link_send_rollback_ex() never
// having been called synchronously -- is a direct proof the handler no
// longer blocks on it.
// ---------------------------------------------------------------------------

static SafetyLinkClass s_rollback_test_safety;

// Lets a test authenticate through ota_pico_rollback_post_handler()'s full
// step 1-4 gate (mac/auth/interlock/mutex) with one call, same helper shape
// test_authenticated_request_does_reach_interlock() above builds inline.
static void set_pico_rollback_headers_for(const char *password)
{
    uint8_t nonce[OTA_AUTH_NONCE_LEN];
    issue_nonce(nonce);
    uint8_t mac[32];
    compute_mac(password, nonce, ctx_str_for(OTA_HTTP_CONTEXT_PICO_ROLLBACK), mac);
    char hex[65];
    hex_encode(mac, sizeof(mac), hex);
    stub_headers_reset();
    stub_header_set("X-Ota-Mac", hex);
    // Bypasses the "safety link is down" interlock refusal (safety_link_
    // get_status() is stubbed to always fail/return link_up=false in this
    // file) -- an operator acknowledgement, not part of authentication; see
    // ota_http_req_ack_no_safety()'s own doc comment in ota_http.c.
    stub_header_set("X-Ota-Ack-No-Safety", "1");
}

static void test_pico_rollback_post_returns_pending_without_blocking(void)
{
    TEST_SECTION("ota_pico_rollback_post_handler -- opus-review finding 3: returns 202 'pending' "
                 "immediately, WITHOUT calling safety_link_send_rollback_ex() synchronously");
    reset_all_lockouts();
    g_stub_boot_button_bypass = false;
    g_stub_ap_password = "rollback-async-test-password";
    s_test_sweep_active = false;
    s_update_claim = OTA_UPDATE_NONE; // ensure no earlier test left the mutex claimed
    memset(&s_rollback_test_safety, 0, sizeof(s_rollback_test_safety));
    ota_http_safety = &s_rollback_test_safety;
    memset(&ota_http_pico_rollback_async, 0, sizeof(ota_http_pico_rollback_async));

    set_pico_rollback_headers_for(g_stub_ap_password);
    s_last_resp_status[0] = '\0';
    s_last_resp_body[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = ota_pico_rollback_post_handler(&req);

    ota_http_safety = NULL; // restore -- every other test in this file expects ota_http_safety == NULL

    TEST_CHECK(err == ESP_OK, "the handler itself always returns ESP_OK");
    TEST_CHECK(strcmp(s_last_resp_status, "202 Accepted") == 0,
              "202 Accepted, not the old synchronous 200/409/500 -- the request was accepted for "
              "async processing, the outcome is not known yet");
    TEST_CHECK(strstr(s_last_resp_body, "\"status\":\"pending\"") != NULL,
              "the immediate body reports 'pending', not a final outcome");
    TEST_CHECK(ota_http_pico_rollback_async.state == OTA_PICO_ROLLBACK_ASYNC_IN_PROGRESS,
              "the assertion that can fail: the handler marks the async state IN_PROGRESS and hands "
              "off to the background task BEFORE returning -- if it fell back to calling safety_link_"
              "send_rollback_ex() synchronously (the old blocking behavior) this would instead already "
              "be DONE by the time the handler returns, defeating the entire point of this fix");
    TEST_CHECK(s_update_claim != OTA_UPDATE_NONE,
              "the update-claim mutex is still held across the async attempt -- it is the background "
              "task's job to release it (xTaskCreate() is stubbed to never actually run that task in "
              "this file, so the claim is expected to still be held here; a real board's task releases "
              "it once safety_link_send_rollback_ex() returns)");

    // Cleanup: this stub environment's xTaskCreate() never runs the task
    // that would normally release the claim, so release it here by hand --
    // otherwise every test after this one in the same process sees an
    // update "already in progress" that never clears.
    s_update_claim = OTA_UPDATE_NONE;
}

static void test_pico_rollback_status_reports_idle_before_any_request(void)
{
    TEST_SECTION("ota_pico_rollback_status_get_handler -- reports 'idle' before any rollback has "
                 "ever been requested this boot");
    memset(&ota_http_pico_rollback_async, 0, sizeof(ota_http_pico_rollback_async));
    ota_http_pico_rollback_async.state = OTA_PICO_ROLLBACK_ASYNC_IDLE;
    s_last_resp_body[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = ota_pico_rollback_status_get_handler(&req);

    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strstr(s_last_resp_body, "\"status\":\"idle\"") != NULL, "reports idle");
}

static void test_pico_rollback_status_reports_pending_while_in_progress(void)
{
    TEST_SECTION("ota_pico_rollback_status_get_handler -- reports 'pending' while the background "
                 "task is still running");
    memset(&ota_http_pico_rollback_async, 0, sizeof(ota_http_pico_rollback_async));
    ota_http_pico_rollback_async.state = OTA_PICO_ROLLBACK_ASYNC_IN_PROGRESS;
    s_last_resp_body[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = ota_pico_rollback_status_get_handler(&req);

    TEST_CHECK(err == ESP_OK, "handler returns ESP_OK");
    TEST_CHECK(strstr(s_last_resp_body, "\"status\":\"pending\"") != NULL, "reports pending");
}

// The mandatory "four-way outcome still reaches the operator" + "a timeout
// must never be reported as success" checks, now exercised through the
// STATUS endpoint (the outcome's new home) rather than the old synchronous
// POST response.
static void test_pico_rollback_status_reports_all_four_outcomes_honestly(void)
{
    TEST_SECTION("ota_pico_rollback_status_get_handler -- once DONE, all four honest outcomes "
                 "(refused-with-reason / accepted / link-down / unknown) reach the operator, and "
                 "UNKNOWN_TIMEOUT is never reported as success text");

    struct {
        safety_link_rollback_outcome_t outcome;
        uint8_t reason_code;
        const char *want_status;
        const char *want_ok;
    } cases[] = {
        { SAFETY_LINK_ROLLBACK_OUTCOME_LINK_DOWN, 0, "\"status\":\"link_down\"", "\"ok\":false" },
        { SAFETY_LINK_ROLLBACK_OUTCOME_SEND_FAILED, 0, "\"status\":\"send_failed\"", "\"ok\":false" },
        { SAFETY_LINK_ROLLBACK_OUTCOME_REFUSED, KILNLINK_ROLLBACK_RESULT_REASON_ARMED, "\"status\":\"refused\"",
          "\"ok\":false" },
        { SAFETY_LINK_ROLLBACK_OUTCOME_UNKNOWN_TIMEOUT, 0, "\"status\":\"unknown\"", "\"ok\":true" },
        { SAFETY_LINK_ROLLBACK_OUTCOME_ACCEPTED, 0, "\"status\":\"rebooting\"", "\"ok\":true" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        memset(&ota_http_pico_rollback_async, 0, sizeof(ota_http_pico_rollback_async));
        ota_http_pico_rollback_async.state = OTA_PICO_ROLLBACK_ASYNC_DONE;
        ota_http_pico_rollback_async.outcome = cases[i].outcome;
        ota_http_pico_rollback_async.reason_code = cases[i].reason_code;
        s_last_resp_body[0] = '\0';

        httpd_req_t req;
        memset(&req, 0, sizeof(req));
        esp_err_t err = ota_pico_rollback_status_get_handler(&req);

        TEST_CHECK(err == ESP_OK, "handler returns ESP_OK");
        TEST_CHECK(strstr(s_last_resp_body, cases[i].want_status) != NULL, "reports the right status field");
        TEST_CHECK(strstr(s_last_resp_body, cases[i].want_ok) != NULL, "reports the right ok field");
    }

    // The specific assertion this whole feature exists to guarantee: an
    // UNKNOWN_TIMEOUT body must never contain the word this code uses for a
    // real success ("rebooting"), and an ACCEPTED body must not claim
    // "unknown" -- i.e. the two are not accidentally sharing one template.
    memset(&ota_http_pico_rollback_async, 0, sizeof(ota_http_pico_rollback_async));
    ota_http_pico_rollback_async.state = OTA_PICO_ROLLBACK_ASYNC_DONE;
    ota_http_pico_rollback_async.outcome = SAFETY_LINK_ROLLBACK_OUTCOME_UNKNOWN_TIMEOUT;
    s_last_resp_body[0] = '\0';
    httpd_req_t req2;
    memset(&req2, 0, sizeof(req2));
    (void)ota_pico_rollback_status_get_handler(&req2);
    TEST_CHECK(strstr(s_last_resp_body, "\"status\":\"rebooting\"") == NULL,
              "UNKNOWN_TIMEOUT must NEVER be reported with the ACCEPTED path's 'rebooting' status");
}

// ---------------------------------------------------------------------------
// sw_reset_http.c -- owner request 2026-09-08's non-destructive reboot item.
// Exercised through the real (static, #include'd) sw_reset_post_handler(),
// same idiom as reset_post_handler() above. Every test authenticates for
// OTA_HTTP_CONTEXT_SW_RESET and sets X-Ota-Ack-No-Safety: 1 so it reaches
// PAST the "no safety link" precondition (ota_http_safety is NULL for the
// whole file -- see that stub's own comment) into the profile-state check
// that is this section's actual subject.
// ---------------------------------------------------------------------------

static bool sw_reset_authenticate(void)
{
    reset_all_lockouts();
    g_stub_boot_button_bypass = false;
    g_stub_ap_password = "sw-reset-test-password";

    uint8_t nonce[OTA_AUTH_NONCE_LEN];
    issue_nonce(nonce);
    uint8_t mac[32];
    compute_mac(g_stub_ap_password, nonce, ctx_str_for(OTA_HTTP_CONTEXT_SW_RESET), mac);
    char hex[65];
    hex_encode(mac, sizeof(mac), hex);
    stub_headers_reset();
    stub_header_set("X-Ota-Mac", hex);
    stub_header_set("X-Ota-Ack-No-Safety", "1");
    return true;
}

// THE NEGATIVE-TEST TARGET (task's "test that matters most"): a firing in
// progress must refuse the reboot. Proven RED by temporarily changing
// sw_reset_post_handler()'s interlock call in sw_reset_http.c to
// `ota_interlock_result_t gate = OTA_INTERLOCK_OK;` (skipping
// ota_http_check_interlocks() entirely) -- with that edit in place this
// test's TEST_CHECK(gate != OTA_INTERLOCK_OK) / "reboot must be refused"
// line below fails: `s_last_err_code == 0 ("reboot must be refused while a
// firing (PROFILE_EXEC_RUNNING) is in progress")`. The edit was applied
// directly to sw_reset_http.c, this test rerun to confirm the failure and
// its exact message, then reversed by hand (the file was restored to match
// this pass's original edit exactly) and this test rerun again to confirm
// GREEN -- see this pass's report for the same red/green pair quoted
// verbatim. The driver file itself carries no trace of the temporary edit.
static void test_sw_reset_refuses_during_firing(void)
{
    TEST_SECTION("sw_reset_post_handler -- refuses while a firing (PROFILE_EXEC_RUNNING) is in progress");
    sw_reset_authenticate();
    g_stub_profile_state = PROFILE_EXEC_RUNNING;
    s_last_err_code = 0;
    s_last_err_msg[0] = '\0';
    s_last_resp_status[0] = '\0';
    s_last_resp_body[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = sw_reset_post_handler(&req);

    TEST_CHECK(err == ESP_OK, "sw_reset_post_handler must always return ESP_OK");
    // Interlock refusals go through httpd_resp_set_status()+httpd_resp_send()
    // (ota_http_send_interlock_refusal()), not httpd_resp_send_err() -- see
    // that function's own doc comment on the 409/428 split -- so the
    // refusal is observed via s_last_resp_status, not s_last_err_code.
    TEST_CHECK(strstr(s_last_resp_status, "409") != NULL,
              "reboot must be refused (409) while a firing (PROFILE_EXEC_RUNNING) is in progress");
    TEST_CHECK(g_probe_interlock_called, "the interlock check must actually have run");

    g_stub_profile_state = PROFILE_EXEC_IDLE; // restore for later tests
}

// Same property, the other running-shaped state: PAUSED is still a firing
// the operator can resume, not an ended one (ota_interlock.h's own doc
// comment on check #4).
static void test_sw_reset_refuses_while_paused(void)
{
    TEST_SECTION("sw_reset_post_handler -- refuses while a firing is PAUSED");
    sw_reset_authenticate();
    g_stub_profile_state = PROFILE_EXEC_PAUSED;
    s_last_resp_status[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    (void)sw_reset_post_handler(&req);

    TEST_CHECK(strstr(s_last_resp_status, "409") != NULL,
              "reboot must be refused while a firing is PAUSED, same as RUNNING");

    g_stub_profile_state = PROFILE_EXEC_IDLE;
}

// Autotune-active refusal -- the OTHER heat-in-progress state
// ota_interlock_check() gates on, independent of profile_state.
static void test_sw_reset_refuses_during_autotune(void)
{
    TEST_SECTION("sw_reset_post_handler -- refuses while autotune is active");
    sw_reset_authenticate();
    g_stub_autotune_active = true;
    s_last_resp_status[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    (void)sw_reset_post_handler(&req);

    TEST_CHECK(strstr(s_last_resp_status, "409") != NULL, "reboot must be refused while autotune is active");

    g_stub_autotune_active = false;
}

// Control case: idle kiln, correctly authenticated -> accepted (no error
// status set; sw_reset_post_handler()'s success path only ever calls
// httpd_resp_sendstr(), which this file's stub does not record as an error).
static void test_sw_reset_ok_when_idle(void)
{
    TEST_SECTION("sw_reset_post_handler -- accepted when idle (control case for the refusal tests above)");
    sw_reset_authenticate();
    g_stub_profile_state = PROFILE_EXEC_IDLE;
    s_last_err_code = 0;

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = sw_reset_post_handler(&req);

    TEST_CHECK(err == ESP_OK, "sw_reset_post_handler must always return ESP_OK");
    TEST_CHECK(s_last_err_code == 0, "an idle kiln's reboot request must not be refused");
}

// 2026-09-09 regression guard for da506105's confirmed defect: that commit
// shipped UI copy and a commit message claiming this route clears "a stuck
// S6a trip". It does not and cannot -- S6a is unconditional in
// safety_guards.c (reboot_grace_active gates only S6b), an ESP reset floats
// GPIO6 and so reads AS mainFault at the Pico, and nothing on this path
// calls safety_link_send_clear_trip(). Pinning the accepted response's
// wording is what stops the claim drifting back in: the body must state
// plainly that a latched trip is NOT cleared, and must name the route that
// does clear one, so an operator is never told the opposite of the truth.
//
// Negative-tested by deleting the "does NOT clear" sentence from
// sw_reset_http.c's own snprintf (production code, not a copy here),
// confirming RED, and restoring it by hand.
static void test_sw_reset_body_does_not_claim_trip_clearing(void)
{
    TEST_SECTION("sw_reset_post_handler -- the accepted body denies clearing a trip and names the real clear path");
    sw_reset_authenticate();
    g_stub_profile_state = PROFILE_EXEC_IDLE;
    s_last_err_code = 0;
    s_last_sendstr_body[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    (void)sw_reset_post_handler(&req);

    TEST_CHECK(s_last_err_code == 0, "control: the idle path must be accepted so a body is actually sent");
    TEST_CHECK(strstr(s_last_sendstr_body, "WILL latch a safety trip") != NULL,
               "the body must state that this reboot CREATES a latched trip -- 2026-09-09: the "
               "earlier wording only denied clearing one, which still let an operator be "
               "surprised by the S6a latch this route reliably causes");
    TEST_CHECK(strstr(s_last_sendstr_body, "nothing on this path clears it") != NULL,
               "the body must still state that a latched safety trip is NOT cleared by this reboot");
    TEST_CHECK(strstr(s_last_sendstr_body, "/api/safety/clear_trip") != NULL
                   && strstr(s_last_sendstr_body, "Clear latched trip") != NULL,
               "the body must name the real clear path -- the route AND the button an operator "
               "can actually press (\"Clear latched trip\" on the Safety page)");
    TEST_CHECK(strstr(s_last_sendstr_body, "REQUIRED FOLLOW-UP") != NULL,
               "clearing the trip must be presented as a required follow-up before heating, not "
               "as optional background information");
}

// 2026-09-09 regression guard for the second confirmed defect in this route:
// xTaskCreatePinnedToCoreWithCaps()'s return value was DISCARDED, and the
// response then said this controller was rebooting no matter what. On a
// PSRAM allocation failure that is a lie, and -- because the Pico had
// already been commanded and may already be resetting -- a lie about a
// half-reset system. Two properties are pinned here: the failure is
// reported (not swallowed), and the report says NEITHER processor rebooted,
// which is only true because the task is now created BEFORE the Pico is
// commanded.
//
// Negative-tested by restoring the discarded-return-value form in
// sw_reset_http.c (production code, not a copy here), confirming RED, and
// reversing that edit by hand.
static void test_sw_reset_reports_task_creation_failure(void)
{
    TEST_SECTION("sw_reset_post_handler -- a failed reboot-task creation is reported, and nothing is left half-reset");
    sw_reset_authenticate();
    g_stub_profile_state = PROFILE_EXEC_IDLE;
    s_last_err_code = 0;
    s_last_sendstr_body[0] = '\0';
    g_stub_task_create_result = pdFAIL;

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = sw_reset_post_handler(&req);
    g_stub_task_create_result = pdPASS; /* leave the shared default as the other tests expect */

    TEST_CHECK(err == ESP_OK, "sw_reset_post_handler must always return ESP_OK");
    TEST_CHECK(strstr(s_last_sendstr_body, "FAILED") != NULL,
               "a failed task creation must be reported as a failure, never as \"ok -- rebooting\"");
    TEST_CHECK(strstr(s_last_sendstr_body, "NEITHER processor was rebooted") != NULL,
               "the report must say neither processor rebooted -- the task is created before the "
               "safety processor is commanded precisely so that this is true");
}

// Auth-before-interlock, same property as decision 3's factory_reset test
// above: an unauthenticated caller must never reach the interlock (or a
// firing/temperature-naming refusal reason).
static void test_sw_reset_missing_auth_never_reaches_interlock(void)
{
    TEST_SECTION("sw_reset_post_handler -- a missing X-Ota-Mac header is refused BEFORE the interlock check");
    reset_all_lockouts();
    stub_headers_reset(); // no X-Ota-Mac at all
    g_probe_interlock_called = false;
    s_last_err_code = 0;
    s_last_err_msg[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = sw_reset_post_handler(&req);

    TEST_CHECK(err == ESP_OK, "sw_reset_post_handler must always return ESP_OK");
    TEST_CHECK(s_last_err_code == 400, "a missing X-Ota-Mac header must be refused with 400");
    TEST_CHECK(!g_probe_interlock_called, "the interlock check must never run for an unauthenticated caller");
}

// THE "does not touch configuration" property (task requirement 2): runs
// the ACCEPTED path (idle kiln, authenticated) against a real fake_kv-backed
// NVS partition with a pre-existing key, and confirms that key is
// byte-for-byte unchanged afterward. sw_reset_http.c links against no
// hal_kv_*() function at all (grep its own source -- true by construction),
// so this is a belt-and-suspenders runtime confirmation of that fact using
// the exact partition/namespace factory_reset.c's "kiln" scope would erase,
// proving this is a genuinely different code path, not just an untested one.
static void test_sw_reset_does_not_touch_nvs(void)
{
    TEST_SECTION("sw_reset_post_handler -- the accepted path leaves an existing kiln_nvs key untouched");

    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_init_partition("kiln_nvs") == HAL_OK, "setup: init kiln_nvs partition");
    TEST_CHECK(hal_kv_open(&h, "probe_ns", HAL_KV_MODE_READ_WRITE, "kiln_nvs") == HAL_OK,
              "setup: open a handle on kiln_nvs");
    const uint8_t sentinel[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    TEST_CHECK(hal_kv_set_blob(&h, "probe_key", sentinel, sizeof(sentinel)) == HAL_OK,
              "setup: write a sentinel key");
    TEST_CHECK(hal_kv_commit(&h) == HAL_OK, "setup: commit the sentinel so it would survive a real erase's "
                                             "de-init/re-init, matching what a genuine wipe would have to beat");

    sw_reset_authenticate();
    g_stub_profile_state = PROFILE_EXEC_IDLE;
    s_last_err_code = 0;

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = sw_reset_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "sw_reset_post_handler must always return ESP_OK");
    TEST_CHECK(s_last_err_code == 0, "setup sanity: the accepted path must not have been refused");

    uint8_t readback[4] = { 0 };
    size_t readback_len = sizeof(readback);
    TEST_CHECK(hal_kv_get_blob(&h, "probe_key", readback, &readback_len) == HAL_OK,
              "the sentinel key must still exist after sw_reset -- an erase would have de-inited "
              "the partition out from under this handle");
    TEST_CHECK(readback_len == sizeof(sentinel) && memcmp(readback, sentinel, sizeof(sentinel)) == 0,
              "the sentinel key's bytes must be byte-for-byte unchanged -- no config was touched");

    hal_kv_close(&h);
}

// ---------------------------------------------------------------------------
// docs/WEB_AUTH_PLAN.md item 12b, factory-reset half: a credential set
// through web_auth_store_set_password()/_set_pin() must survive EVERY one
// of factory_reset.c's four scopes. Calls the real, non-static
// factory_reset_execute() directly (not through reset_post_handler()'s
// auth/interlock gate, which this file's other tests already cover and
// which is orthogonal to this property) so the real hal_kv_erase_partition()
// calls on wifi_nvs/kiln_nvs/profiles_nvs actually run against the real
// fake_kv backend. A synthetic credential is used throughout -- never a
// real password.
// ---------------------------------------------------------------------------
static const uint8_t WEBAUTH12B_SALT[WEB_AUTH_SALT_LEN] = {
    0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8,
    0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0};
#define WEBAUTH12B_SYNTHETIC_PASSWORD "Synthetic-Test-Secret-9"
#define WEBAUTH12B_SYNTHETIC_PIN "482913"

static void seed_webauth12b_credential(void)
{
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK,
              "setup: init the default nvs partition (kiln_auth's home)");
    TEST_CHECK(web_auth_store_set_password(WEB_AUTH_ROLE_ADMINISTRATOR, "synthtestuser",
                                            WEBAUTH12B_SYNTHETIC_PASSWORD, WEBAUTH12B_SALT,
                                            false) == HAL_OK,
              "setup: seed a synthetic administrator password");
    TEST_CHECK(web_auth_store_set_pin(WEB_AUTH_ROLE_USER, WEBAUTH12B_SYNTHETIC_PIN,
                                       WEBAUTH12B_SALT) == HAL_OK,
              "setup: seed a synthetic user LCD PIN");
    TEST_CHECK(web_auth_store_verify_password(WEB_AUTH_ROLE_ADMINISTRATOR,
                                               WEBAUTH12B_SYNTHETIC_PASSWORD),
              "setup sanity: the seeded password verifies before any reset scope runs");
    TEST_CHECK(web_auth_store_verify_pin(WEB_AUTH_ROLE_USER, WEBAUTH12B_SYNTHETIC_PIN),
              "setup sanity: the seeded PIN verifies before any reset scope runs");
}

static void assert_webauth12b_credential_survived(const char *scope_name)
{
    TEST_CHECK(web_auth_store_verify_password(WEB_AUTH_ROLE_ADMINISTRATOR,
                                               WEBAUTH12B_SYNTHETIC_PASSWORD),
              scope_name);
    TEST_CHECK(web_auth_store_verify_pin(WEB_AUTH_ROLE_USER, WEBAUTH12B_SYNTHETIC_PIN),
              scope_name);
}

static void test_credential_survives_factory_reset_wifi_scope(void)
{
    TEST_SECTION("factory_reset_execute(FACTORY_RESET_SCOPE_WIFI) -- a kiln_auth credential must survive");
    fake_kv_reset_all();
    TEST_CHECK(hal_kv_init_partition("wifi_nvs") == HAL_OK, "setup: init wifi_nvs");
    seed_webauth12b_credential();
    TEST_CHECK(factory_reset_execute(FACTORY_RESET_SCOPE_WIFI) == ESP_OK,
              "factory_reset_execute(WIFI) must succeed");
    assert_webauth12b_credential_survived(
        "the administrator password must still verify after a WIFI-scope factory reset");
}

static void test_credential_survives_factory_reset_kiln_scope(void)
{
    TEST_SECTION("factory_reset_execute(FACTORY_RESET_SCOPE_KILN) -- a kiln_auth credential must survive");
    fake_kv_reset_all();
    TEST_CHECK(hal_kv_init_partition("kiln_nvs") == HAL_OK, "setup: init kiln_nvs");
    seed_webauth12b_credential();
    TEST_CHECK(factory_reset_execute(FACTORY_RESET_SCOPE_KILN) == ESP_OK,
              "factory_reset_execute(KILN) must succeed");
    assert_webauth12b_credential_survived(
        "the administrator password must still verify after a KILN-scope factory reset");
}

static void test_credential_survives_factory_reset_profiles_scope(void)
{
    TEST_SECTION("factory_reset_execute(FACTORY_RESET_SCOPE_PROFILES) -- a kiln_auth credential must survive");
    fake_kv_reset_all();
    TEST_CHECK(hal_kv_init_partition("profiles_nvs") == HAL_OK, "setup: init profiles_nvs");
    seed_webauth12b_credential();
    TEST_CHECK(factory_reset_execute(FACTORY_RESET_SCOPE_PROFILES) == ESP_OK,
              "factory_reset_execute(PROFILES) must succeed");
    assert_webauth12b_credential_survived(
        "the administrator password must still verify after a PROFILES-scope factory reset");
}

static void test_credential_survives_factory_reset_all_scope(void)
{
    TEST_SECTION("factory_reset_execute(FACTORY_RESET_SCOPE_ALL) -- a kiln_auth credential must survive "
                 "even the full factory-default wipe");
    fake_kv_reset_all();
    TEST_CHECK(hal_kv_init_partition("wifi_nvs") == HAL_OK, "setup: init wifi_nvs");
    TEST_CHECK(hal_kv_init_partition("kiln_nvs") == HAL_OK, "setup: init kiln_nvs");
    TEST_CHECK(hal_kv_init_partition("profiles_nvs") == HAL_OK, "setup: init profiles_nvs");
    seed_webauth12b_credential();
    TEST_CHECK(factory_reset_execute(FACTORY_RESET_SCOPE_ALL) == ESP_OK,
              "factory_reset_execute(ALL) must succeed");
    assert_webauth12b_credential_survived(
        "the administrator password must still verify after an ALL-scope (factory-default) reset");
}

// --- ESP-side sequencing for the Pico half (SAFETY_CMD_REBOOT, 0x29) -------
//
// sw_reset_post_handler() commands the Pico on the REQUEST task and reports
// the answer in its own HTTP response. The load-bearing part of that
// sequencing is the classification: what the ESP is willing to CLAIM about
// the safety processor for each thing safety_link_send_reboot() can return.
// sw_reset_classify_pico_outcome() is the whole of that decision, and it is
// exercised here directly against the real (non-static) function in
// sw_reset_http.c -- not a copy of its logic.
//
// THE PROPERTY: only an explicit, wire-visible accepted=1 may ever produce
// SW_RESET_PICO_ACCEPTED. Silence, a dead link, a local send failure, a
// driver error, and any outcome enumerator added in the future must all read
// as UNCONFIRMED. This is the "a timeout must never be misreported as
// success" rule the rollback path documents at length, applied to the one
// place where this feature could quietly claim both processors rebooted when
// only one did.
static void test_sw_reset_pico_outcome_mapping(void)
{
    TEST_SECTION("sw_reset_classify_pico_outcome -- only an explicit accepted=1 reads as ACCEPTED, "
                 "and a REFUSED outcome maps to the reason the wire actually sent");

    TEST_CHECK(sw_reset_classify_pico_outcome(ESP_OK, SAFETY_LINK_REBOOT_OUTCOME_ACCEPTED,
                                              KILNLINK_REBOOT_RESULT_REASON_NONE) ==
                   SW_RESET_PICO_ACCEPTED,
               "an explicit ACCEPTED outcome reports the safety processor as accepted");

    // 2026-09-09 fix: the reason byte is decoded into a DISTINCT report per
    // refusal reason, not collapsed onto one fixed "relay armed" claim --
    // KILNLINK_REBOOT_RESULT_REASON_TRANSFER_ACTIVE has nothing to do with
    // relay state and must never render as if it did.
    TEST_CHECK(sw_reset_classify_pico_outcome(ESP_OK, SAFETY_LINK_REBOOT_OUTCOME_REFUSED,
                                              KILNLINK_REBOOT_RESULT_REASON_ARMED) ==
                   SW_RESET_PICO_REFUSED_ARMED,
               "REFUSED + ARMED reason reports the relay-armed refusal");
    TEST_CHECK(sw_reset_classify_pico_outcome(ESP_OK, SAFETY_LINK_REBOOT_OUTCOME_REFUSED,
                                              KILNLINK_REBOOT_RESULT_REASON_TRANSFER_ACTIVE) ==
                   SW_RESET_PICO_REFUSED_TRANSFER,
               "REFUSED + TRANSFER_ACTIVE reason reports the transfer-in-progress refusal, "
               "NOT the relay-armed one");
    TEST_CHECK(sw_reset_classify_pico_outcome(ESP_OK, SAFETY_LINK_REBOOT_OUTCOME_REFUSED,
                                              KILNLINK_REBOOT_RESULT_REASON_UNKNOWN) ==
                   SW_RESET_PICO_REFUSED_OTHER,
               "REFUSED + an explicit UNKNOWN reason reports the generic refusal, not a specific claim");
    TEST_CHECK(sw_reset_classify_pico_outcome(ESP_OK, SAFETY_LINK_REBOOT_OUTCOME_REFUSED,
                                              (uint8_t)(KILNLINK_REBOOT_RESULT_REASON_TRANSFER_ACTIVE + 1)) ==
                   SW_RESET_PICO_REFUSED_OTHER,
               "REFUSED + a reason byte this build's enum does not name (a newer peer) reports the "
               "generic refusal rather than aliasing onto ARMED or TRANSFER");

    TEST_CHECK(sw_reset_classify_pico_outcome(ESP_OK, SAFETY_LINK_REBOOT_OUTCOME_NO_REPLY, 0) ==
                   SW_RESET_PICO_UNCONFIRMED,
               "silence (a Pico predating this command, or a lost reply) is UNCONFIRMED, never accepted");
    TEST_CHECK(sw_reset_classify_pico_outcome(ESP_OK, SAFETY_LINK_REBOOT_OUTCOME_LINK_DOWN, 0) ==
                   SW_RESET_PICO_UNCONFIRMED,
               "a link that was already down is UNCONFIRMED, never accepted");
    TEST_CHECK(sw_reset_classify_pico_outcome(ESP_OK, SAFETY_LINK_REBOOT_OUTCOME_SEND_FAILED, 0) ==
                   SW_RESET_PICO_UNCONFIRMED,
               "a local send failure is UNCONFIRMED, never accepted");

    // A driver-level error must not be able to smuggle an ACCEPTED through
    // on the back of whatever happened to be left in *out_outcome.
    TEST_CHECK(sw_reset_classify_pico_outcome(ESP_ERR_INVALID_STATE,
                                              SAFETY_LINK_REBOOT_OUTCOME_ACCEPTED, 0) ==
                   SW_RESET_PICO_UNCONFIRMED,
               "a non-ESP_OK return is UNCONFIRMED even when the outcome argument says ACCEPTED");
    TEST_CHECK(sw_reset_classify_pico_outcome(ESP_ERR_INVALID_ARG,
                                              SAFETY_LINK_REBOOT_OUTCOME_REFUSED,
                                              KILNLINK_REBOOT_RESULT_REASON_ARMED) ==
                   SW_RESET_PICO_UNCONFIRMED,
               "a non-ESP_OK return is UNCONFIRMED rather than a refusal it cannot actually vouch for");

    // Any future enumerator this switch has not been taught about must fall
    // through to UNCONFIRMED, not to accepted. Cast a value past the end of
    // the enum to stand in for one.
    TEST_CHECK(sw_reset_classify_pico_outcome(
                   ESP_OK, (safety_link_reboot_outcome_t)(SAFETY_LINK_REBOOT_OUTCOME_NO_REPLY + 1), 0) ==
                   SW_RESET_PICO_UNCONFIRMED,
               "an unrecognized outcome value reads as UNCONFIRMED, so a future outcome cannot "
               "silently become success");
}

// The operator-facing half of the same property: the text this route puts in
// its HTTP response (and the settings page shows verbatim) must not claim the
// safety processor rebooted unless it did. A mapping that is right while the
// sentence lies would be exactly as harmful as the mapping being wrong.
static void test_sw_reset_pico_sentences_are_honest(void)
{
    TEST_SECTION("sw_reset_pico_sentence -- no non-accepted outcome claims the safety processor rebooted");

    const char *accepted = sw_reset_pico_sentence(SW_RESET_PICO_ACCEPTED);
    const char *refused_armed = sw_reset_pico_sentence(SW_RESET_PICO_REFUSED_ARMED);
    const char *refused_transfer = sw_reset_pico_sentence(SW_RESET_PICO_REFUSED_TRANSFER);
    const char *refused_other = sw_reset_pico_sentence(SW_RESET_PICO_REFUSED_OTHER);
    const char *unconfirmed = sw_reset_pico_sentence(SW_RESET_PICO_UNCONFIRMED);
    const char *no_link = sw_reset_pico_sentence(SW_RESET_PICO_NO_LINK);

    TEST_CHECK(strstr(accepted, "accepted") != NULL,
               "the accepted sentence says the safety processor accepted");
    TEST_CHECK(strstr(refused_armed, "REFUSED") != NULL, "the armed-refusal sentence says REFUSED");
    TEST_CHECK(strstr(refused_transfer, "REFUSED") != NULL,
               "the transfer-refusal sentence says REFUSED");
    TEST_CHECK(strstr(refused_other, "REFUSED") != NULL,
               "the generic-refusal sentence says REFUSED");
    TEST_CHECK(strstr(unconfirmed, "NOT confirmed") != NULL,
               "the unconfirmed sentence says NOT confirmed, in as many words");

    // The load-bearing property from the 2026-09-09 fix: the transfer-active
    // refusal must NOT claim the relay is armed, and the generic/unknown
    // refusal must not claim EITHER specific cause -- an unrecognized reason
    // must never render as a specific claim.
    TEST_CHECK(strstr(refused_transfer, "relay") == NULL,
               "the transfer-in-progress refusal sentence must never mention the relay -- that "
               "was the defect: every refusal used to render as \"its heating relay is armed\" "
               "regardless of the real reason");
    TEST_CHECK(strstr(refused_transfer, "transfer") != NULL,
               "the transfer-in-progress refusal sentence names the real cause");
    TEST_CHECK(strstr(refused_other, "relay") == NULL && strstr(refused_other, "transfer") == NULL,
               "the generic/unrecognized-reason refusal sentence must not claim either specific "
               "cause");

    TEST_CHECK(strstr(refused_armed, "accepted") == NULL,
               "the armed-refusal sentence must never contain the word accepted");
    TEST_CHECK(strstr(refused_transfer, "accepted") == NULL,
               "the transfer-refusal sentence must never contain the word accepted");
    TEST_CHECK(strstr(refused_other, "accepted") == NULL,
               "the generic-refusal sentence must never contain the word accepted");
    TEST_CHECK(strstr(unconfirmed, "accepted") == NULL,
               "the unconfirmed sentence must never contain the word accepted");
    TEST_CHECK(strstr(no_link, "accepted") == NULL,
               "the no-safety-processor sentence must never contain the word accepted");

    // Every outcome must have its own distinct sentence -- a duplicate would
    // mean two genuinely different results read identically to the operator.
    const char *all[] = {accepted, refused_armed, refused_transfer, refused_other, unconfirmed, no_link};
    const size_t n = sizeof(all) / sizeof(all[0]);
    bool all_distinct = true;
    for (size_t i = 0; i < n && all_distinct; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (strcmp(all[i], all[j]) == 0) {
                all_distinct = false;
                break;
            }
        }
    }
    TEST_CHECK(all_distinct, "all six outcome sentences are distinct");
}

// ---------------------------------------------------------------------------
// POST /api/ota/esp/boot_guard_reset -- docs/audits/
// boot_guard_post_flash_recovery_footgun_2026-09-08.md's tool-driven trigger.
// boot_guard_reset_counter() itself (NVS write/verify/retry) is covered by
// test_boot_guard.c against the real boot_guard.c; this file's stub
// (s_stub_boot_guard_reset_verified) exists purely to exercise the HTTP
// layer's own decisions: auth ordering and honest ok:true/false reporting.
// ---------------------------------------------------------------------------

static void test_boot_guard_reset_missing_auth_refused(void)
{
    TEST_SECTION("ota_boot_guard_reset_post_handler -- a missing X-Ota-Mac header is refused (400) "
                 "before boot_guard_reset_counter() is ever called");
    reset_all_lockouts();
    stub_headers_reset();
    s_last_err_code = 0;
    s_last_err_msg[0] = '\0';
    s_stub_boot_guard_reset_verified = true; // if this got called anyway, it would look like success

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = ota_boot_guard_reset_post_handler(&req);

    TEST_CHECK(err == ESP_OK, "handler always returns ESP_OK -- errors go through httpd_resp_send_err");
    TEST_CHECK(s_last_err_code == 400, "a missing X-Ota-Mac header is refused with 400");
    TEST_CHECK(strstr(s_last_err_msg, "X-Ota-Mac") != NULL, "the refusal names the missing header");
}

static void test_boot_guard_reset_wrong_context_mac_refused(void)
{
    TEST_SECTION("ota_boot_guard_reset_post_handler -- a MAC signed for a DIFFERENT context "
                 "(recovery_exit's) is rejected -- its own context, not interchangeable");
    reset_all_lockouts();
    g_stub_boot_button_bypass = false;
    g_stub_ap_password = "boot-guard-reset-test-password";

    uint8_t nonce[OTA_AUTH_NONCE_LEN];
    issue_nonce(nonce);
    uint8_t mac[32];
    compute_mac(g_stub_ap_password, nonce, ctx_str_for(OTA_HTTP_CONTEXT_RECOVERY_EXIT), mac);
    char hex[65];
    hex_encode(mac, sizeof(mac), hex);
    stub_headers_reset();
    stub_header_set("X-Ota-Mac", hex);
    s_last_err_code = 0;
    s_last_err_msg[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = ota_boot_guard_reset_post_handler(&req);

    TEST_CHECK(err == ESP_OK, "handler always returns ESP_OK");
    TEST_CHECK(s_last_err_code == 403, "a recovery-exit-context MAC is refused (403) against "
              "OTA_HTTP_CONTEXT_BOOT_GUARD_RESET");
}

static void test_boot_guard_reset_authenticated_reports_success(void)
{
    TEST_SECTION("ota_boot_guard_reset_post_handler -- a correctly authenticated request calls "
                 "boot_guard_reset_counter() and reports ok:true when it verifies");
    reset_all_lockouts();
    g_stub_boot_button_bypass = false;
    g_stub_ap_password = "boot-guard-reset-test-password";
    s_stub_boot_guard_reset_verified = true;
    s_stub_boot_guard_count = 0;

    uint8_t nonce[OTA_AUTH_NONCE_LEN];
    issue_nonce(nonce);
    uint8_t mac[32];
    compute_mac(g_stub_ap_password, nonce, ctx_str_for(OTA_HTTP_CONTEXT_BOOT_GUARD_RESET), mac);
    char hex[65];
    hex_encode(mac, sizeof(mac), hex);
    stub_headers_reset();
    stub_header_set("X-Ota-Mac", hex);
    s_last_err_code = 0;
    s_last_err_msg[0] = '\0';
    s_last_resp_body[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = ota_boot_guard_reset_post_handler(&req);

    TEST_CHECK(err == ESP_OK, "handler always returns ESP_OK");
    TEST_CHECK(s_last_err_code == 0, "a correctly authenticated request is not refused");
    TEST_CHECK(strstr(s_last_resp_body, "\"ok\":true") != NULL,
              "reports ok:true when boot_guard_reset_counter() verifies its clear");
}

static void test_boot_guard_reset_authenticated_reports_failure_honestly(void)
{
    TEST_SECTION("ota_boot_guard_reset_post_handler -- reports ok:false, not a bare 200 that implies "
                 "success, when boot_guard_reset_counter() could NOT verify the clear");
    reset_all_lockouts();
    g_stub_boot_button_bypass = false;
    g_stub_ap_password = "boot-guard-reset-test-password";
    s_stub_boot_guard_reset_verified = false; // the lying-write case, from the caller's side

    uint8_t nonce[OTA_AUTH_NONCE_LEN];
    issue_nonce(nonce);
    uint8_t mac[32];
    compute_mac(g_stub_ap_password, nonce, ctx_str_for(OTA_HTTP_CONTEXT_BOOT_GUARD_RESET), mac);
    char hex[65];
    hex_encode(mac, sizeof(mac), hex);
    stub_headers_reset();
    stub_header_set("X-Ota-Mac", hex);
    s_last_err_code = 0;
    s_last_err_msg[0] = '\0';
    s_last_resp_body[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = ota_boot_guard_reset_post_handler(&req);

    TEST_CHECK(err == ESP_OK, "handler always returns ESP_OK -- it still responds 200 either way, "
              "the honesty is in the body, not the HTTP status");
    TEST_CHECK(s_last_err_code == 0, "auth succeeded, so this is not an httpd_resp_send_err() path");
    TEST_CHECK(strstr(s_last_resp_body, "\"ok\":false") != NULL,
              "reports ok:false -- a caller (flash_firmware()) trusting a bare 200 as success would "
              "wrongly believe the recovery-mode counter was actually cleared");
}

// ---------------------------------------------------------------------------
// GET /api/ota/esp/status -- OPEN tier (route_tier_table.h), reachable with
// no session so pre-auth OTA clients keep working, but commit/dirty/
// build_date must be redacted to JSON null for a non-admin caller (the fix
// for the build-identity leak this route's OPEN test does not cover; see
// ota_esp_status_get_handler()'s own comment). recovery_mode/version/
// active_slot/etc. stay unredacted for every caller.
// ---------------------------------------------------------------------------

// Mints a real ADMIN or USER session in the real session table
// (http_session_table()) and returns the raw bearer token to hand to
// stub_header_set("Cookie", ...) -- exactly the path a real login would
// produce (web_auth_table_create_session() keyed by SHA-256 of the token,
// same hash http_auth_session_resolve() recomputes from the cookie it is
// given), so this exercises the real seam rather than a transcribed copy of
// it.
static const char *ota_status_test_make_session(web_auth_session_role_t role, const char *token) {
    uint8_t hash[32];
    size_t hash_len = 0;
    psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)token, strlen(token), hash, sizeof(hash), &hash_len);
    // Finding 1 fix (2026-09-17 web-auth adversarial review):
    // http_auth_session_resolve() now binds a session to the client IP it
    // was minted for, exact match. This file's ota_http_get_client_ip()
    // always falls back to the literal string "unknown" (getpeername()
    // has nothing real to answer against in a host test) -- the session
    // must be minted against that same fallback string, or resolution
    // correctly (and, before this fix, silently) fails.
    web_auth_table_create_session(http_session_table(), hash, "unknown", role, (uint32_t)0);
    return token;
}

// 2026-09-17 audit finding 6, corrected the same day: this test previously
// asserted that auth-off redacts commit/dirty/build_date (the AND-of-both-
// conjuncts gate, `is_admin = http_auth_policy_web_enabled() &&
// http_auth_caller_is_admin(req)`) -- but that redacted the build identity
// from EVERYONE on a default, never-configured board, including this
// project's own tooling (flash_firmware()'s post-flash verification has no
// session and never will). With auth off, any caller can already resolve as
// admin via http_auth_caller_is_admin() (the deliberate bootstrap behaviour
// WEB_AUTH_PLAN.md section 11 relies on for setting the first admin
// password), so redacting from that same caller protects nothing. Corrected
// gate: `may_see_build_identity = !http_auth_policy_web_enabled() ||
// http_auth_caller_is_admin(req)` -- show the real values whenever auth is
// off, and redact only once auth is on and the caller isn't an admin (the
// next three tests below).
static void test_ota_esp_status_web_auth_off_shows_build_identity(void)
{
    TEST_SECTION("ota_esp_status_get_handler -- web auth OFF (default/never-configured board): "
                 "commit/dirty/build_date show their real values -- redacting from a caller who can "
                 "already bootstrap as admin protects nothing (corrected 2026-09-17)");
    web_auth_policy_t policy = { .web_enabled = false, .lcd_enabled = false,
                                  .web_timeout_s = -1, .lcd_timeout_s = -1 };
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init default nvs partition");
    TEST_CHECK(web_auth_store_set_policy(&policy) == HAL_OK, "setup: policy persisted");
    stub_headers_reset();
    s_last_resp_body[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = ota_esp_status_get_handler(&req);

    TEST_CHECK(err == ESP_OK, "handler always returns ESP_OK");
    TEST_CHECK(strstr(s_last_resp_body, "\"commit\":\"stub\"") != NULL,
              "auth off (default board) -- real commit value present (stub build_info.h)");
    TEST_CHECK(strstr(s_last_resp_body, "\"dirty\":false") != NULL,
              "auth off (default board) -- real dirty value present (FW_GIT_DIRTY stubbed to 0)");
    TEST_CHECK(strstr(s_last_resp_body, "\"build_date\":\"1970-01-01 00:00:00\"") != NULL,
              "auth off (default board) -- real build_date value present (stub build_info.h)");
    TEST_CHECK(strstr(s_last_resp_body, "\"recovery_mode\":false") != NULL,
              "recovery_mode still NOT redacted regardless of auth state");
    TEST_CHECK(strstr(s_last_resp_body, "\"version\":") != NULL,
              "version is still NOT redacted regardless of auth state");
}

static void test_ota_esp_status_unauthenticated_redacts_build_identity(void)
{
    TEST_SECTION("ota_esp_status_get_handler -- web auth ON, no cookie: commit/dirty/build_date "
                 "redacted to null, recovery_mode still present");
    web_auth_policy_t policy = { .web_enabled = true, .lcd_enabled = false,
                                  .web_timeout_s = -1, .lcd_timeout_s = -1 };
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init default nvs partition");
    TEST_CHECK(web_auth_store_set_policy(&policy) == HAL_OK, "setup: policy persisted");
    stub_headers_reset();
    s_last_resp_body[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = ota_esp_status_get_handler(&req);

    TEST_CHECK(err == ESP_OK, "handler always returns ESP_OK");
    TEST_CHECK(strstr(s_last_resp_body, "\"commit\":null") != NULL,
              "no session -- commit redacted to null");
    TEST_CHECK(strstr(s_last_resp_body, "\"dirty\":null") != NULL,
              "no session -- dirty redacted to null");
    TEST_CHECK(strstr(s_last_resp_body, "\"build_date\":null") != NULL,
              "no session -- build_date redacted to null");
    TEST_CHECK(strstr(s_last_resp_body, "\"recovery_mode\":false") != NULL,
              "recovery_mode is NOT redacted -- app.js's OPEN-tier dashboard banner needs it "
              "pre-auth (2026-09-08 incident fix)");
    TEST_CHECK(strstr(s_last_resp_body, "\"version\":") != NULL, "version is NOT redacted");
}

static void test_ota_esp_status_user_session_redacts_build_identity(void)
{
    TEST_SECTION("ota_esp_status_get_handler -- web auth ON, USER-role session: still redacted, "
                 "identical to no session");
    web_auth_policy_t policy = { .web_enabled = true, .lcd_enabled = false,
                                  .web_timeout_s = -1, .lcd_timeout_s = -1 };
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init default nvs partition");
    TEST_CHECK(web_auth_store_set_policy(&policy) == HAL_OK, "setup: policy persisted");
    const char *token = ota_status_test_make_session(WEB_AUTH_SESSION_ROLE_USER, "user-token-1");
    stub_headers_reset();
    char cookie[64];
    snprintf(cookie, sizeof(cookie), HTTP_SESSION_COOKIE_NAME "=%s", token);
    stub_header_set("Cookie", cookie);
    s_last_resp_body[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = ota_esp_status_get_handler(&req);

    TEST_CHECK(err == ESP_OK, "handler always returns ESP_OK");
    TEST_CHECK(strstr(s_last_resp_body, "\"commit\":null") != NULL,
              "USER role -- commit still redacted, not just NONE");
    TEST_CHECK(strstr(s_last_resp_body, "\"dirty\":null") != NULL,
              "USER role -- dirty still redacted");
    TEST_CHECK(strstr(s_last_resp_body, "\"build_date\":null") != NULL,
              "USER role -- build_date still redacted");
}

static void test_ota_esp_status_admin_session_gets_full_payload(void)
{
    TEST_SECTION("ota_esp_status_get_handler -- web auth ON, ADMIN-role session: real "
                 "commit/dirty/build_date values");
    web_auth_policy_t policy = { .web_enabled = true, .lcd_enabled = false,
                                  .web_timeout_s = -1, .lcd_timeout_s = -1 };
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init default nvs partition");
    TEST_CHECK(web_auth_store_set_policy(&policy) == HAL_OK, "setup: policy persisted");
    const char *token = ota_status_test_make_session(WEB_AUTH_SESSION_ROLE_ADMIN, "admin-token-1");
    stub_headers_reset();
    char cookie[64];
    snprintf(cookie, sizeof(cookie), HTTP_SESSION_COOKIE_NAME "=%s", token);
    stub_header_set("Cookie", cookie);
    s_last_resp_body[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = ota_esp_status_get_handler(&req);

    TEST_CHECK(err == ESP_OK, "handler always returns ESP_OK");
    TEST_CHECK(strstr(s_last_resp_body, "\"commit\":\"stub\"") != NULL,
              "ADMIN role -- real commit value present");
    TEST_CHECK(strstr(s_last_resp_body, "\"dirty\":false") != NULL,
              "ADMIN role -- real dirty value present");
    TEST_CHECK(strstr(s_last_resp_body, "\"build_date\":\"1970-01-01 00:00:00\"") != NULL,
              "ADMIN role -- real build_date value present");
}

// Item 3 fix (2026-09-17 adversarial review, 1179e2d3): resolve_timeout_s()
// (http_session_iface.c) used to return WEB_AUTH_TIMEOUT_NEVER_S (0) whenever
// the policy record failed to load, and web_auth_session_is_valid() treats 0
// as "never expires" -- so a policy record that goes UNREADABLE (a bad CRC
// from flash corruption, say) failed CLOSED on the enable flag but OPEN on
// expiry: an admin session minted while the policy was healthy would stay
// valid forever once the record turned unreadable, instead of being treated
// as expired/denied like every other failure of that record. This test
// mints a real ADMIN session while the policy is healthy, then corrupts the
// LAST byte of the persisted policy blob (falls inside its trailing crc32
// field -- confirmed by reading web_auth_store.c's web_auth_policy_blob_t
// layout: {version, web_enabled, lcd_enabled, reserved[2], web_timeout_s,
// web_timeout_s, crc32}) via the public hal_kv_get_blob/hal_kv_set_blob API
// only -- this file links web_auth_store.c as a compiled object, not
// #include, so it has no access to that struct's definition or to
// load_blob()/set_blob_verified(), unlike test_web_auth_store.c's own
// unreadable-record test. A previously-valid ADMIN session must then be
// denied full payload, exactly as if it had expired.
static void test_ota_esp_status_admin_session_denied_when_policy_unreadable(void)
{
    TEST_SECTION("ota_esp_status_get_handler -- policy record UNREADABLE: a previously-valid ADMIN "
                 "session is denied (fail closed on expiry, item 3), not treated as never-expiring");
    web_auth_policy_t policy = { .web_enabled = true, .lcd_enabled = false,
                                  .web_timeout_s = -1, .lcd_timeout_s = -1 };
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init default nvs partition");
    TEST_CHECK(web_auth_store_set_policy(&policy) == HAL_OK, "setup: policy persisted while healthy");
    const char *token = ota_status_test_make_session(WEB_AUTH_SESSION_ROLE_ADMIN, "admin-token-unreadable");

    // Corrupt the persisted policy blob's CRC via the public hal_kv API only.
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, "kiln_auth", HAL_KV_MODE_READ_WRITE, NULL) == HAL_OK,
              "setup: open kiln_auth namespace for corruption");
    uint8_t blob[64];
    size_t blob_len = sizeof(blob);
    TEST_CHECK(hal_kv_get_blob(&h, "auth_policy", blob, &blob_len) == HAL_OK,
              "setup: read back the persisted policy blob");
    TEST_CHECK(blob_len > 0 && blob_len <= sizeof(blob), "setup: policy blob length sane");
    blob[blob_len - 1] ^= 0xFFu; // flip the last byte -- inside the trailing crc32 field
    TEST_CHECK(hal_kv_set_blob(&h, "auth_policy", blob, blob_len) == HAL_OK,
              "setup: write back the corrupted policy blob");
    TEST_CHECK(hal_kv_commit(&h) == HAL_OK, "setup: commit the corruption");
    hal_kv_close(&h);

    stub_headers_reset();
    char cookie[64];
    snprintf(cookie, sizeof(cookie), HTTP_SESSION_COOKIE_NAME "=%s", token);
    stub_header_set("Cookie", cookie);
    s_last_resp_body[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = ota_esp_status_get_handler(&req);

    TEST_CHECK(err == ESP_OK, "handler always returns ESP_OK");
    TEST_CHECK(strstr(s_last_resp_body, "\"commit\":null") != NULL,
              "policy unreadable -- previously-valid ADMIN session now denied, commit redacted");
    TEST_CHECK(strstr(s_last_resp_body, "\"dirty\":null") != NULL,
              "policy unreadable -- dirty still redacted");
    TEST_CHECK(strstr(s_last_resp_body, "\"build_date\":null") != NULL,
              "policy unreadable -- build_date still redacted");
}

// 2026-09-17 review of the section 6 login route (WEB_AUTH_PLAN.md section
// 2b): resolve_role_for_request() (http_auth_http.c) used to SHA-256 the
// WHOLE raw Cookie header instead of parsing the named session cookie
// (HTTP_SESSION_COOKIE_NAME), so a second cookie on the same path -- legal
// per RFC 6265, and exactly what a real browser sends once anything else
// ever sets one -- silently changed the resolved session. These two tests
// exercise the real fix (extract_named_cookie() inside http_auth_http.c,
// exercised only indirectly here since it is file-static) via the same
// public handler the tests above use, with decoy cookies before and after
// the real one.
static void test_ota_esp_status_admin_session_survives_decoy_cookies(void)
{
    TEST_SECTION("ota_esp_status_get_handler -- a Cookie header carrying OTHER cookies around "
                 "kiln_sid must still resolve the real session (the header-hashing bug this "
                 "fixes would have made this depend on which decoys are present)");
    web_auth_policy_t policy = { .web_enabled = true, .lcd_enabled = false,
                                  .web_timeout_s = -1, .lcd_timeout_s = -1 };
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init default nvs partition");
    TEST_CHECK(web_auth_store_set_policy(&policy) == HAL_OK, "setup: policy persisted");
    const char *token = ota_status_test_make_session(WEB_AUTH_SESSION_ROLE_ADMIN, "admin-token-2");
    stub_headers_reset();
    char cookie[96];
    snprintf(cookie, sizeof(cookie), "a=1; " HTTP_SESSION_COOKIE_NAME "=%s; theme=dark", token);
    stub_header_set("Cookie", cookie);
    s_last_resp_body[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = ota_esp_status_get_handler(&req);

    TEST_CHECK(err == ESP_OK, "handler always returns ESP_OK");
    TEST_CHECK(strstr(s_last_resp_body, "\"commit\":\"stub\"") != NULL,
              "decoy cookies before/after kiln_sid do not prevent ADMIN resolution");
}

static void test_ota_esp_status_unnamed_cookie_value_is_not_a_session(void)
{
    TEST_SECTION("ota_esp_status_get_handler -- a Cookie header with NO kiln_sid= name (the "
                 "pre-fix format, a bare token) must resolve to no session, not accidentally "
                 "match by matching the whole header's hash");
    web_auth_policy_t policy = { .web_enabled = true, .lcd_enabled = false,
                                  .web_timeout_s = -1, .lcd_timeout_s = -1 };
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init default nvs partition");
    TEST_CHECK(web_auth_store_set_policy(&policy) == HAL_OK, "setup: policy persisted");
    const char *token = ota_status_test_make_session(WEB_AUTH_SESSION_ROLE_ADMIN, "admin-token-3");
    stub_headers_reset();
    char cookie[64];
    snprintf(cookie, sizeof(cookie), "%s", token); // bare token, no "kiln_sid=" name
    stub_header_set("Cookie", cookie);
    s_last_resp_body[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = ota_esp_status_get_handler(&req);

    TEST_CHECK(err == ESP_OK, "handler always returns ESP_OK");
    TEST_CHECK(strstr(s_last_resp_body, "\"commit\":null") != NULL,
              "no named kiln_sid cookie -- redacted exactly like no session at all");
}

// ---------------------------------------------------------------------------
// GET /api/boot_guard -- unauthenticated diagnostics, same exposure level as
// GET /api/status. Reports whatever boot_guard reports, honestly, with no
// auth gate to bypass first.
// ---------------------------------------------------------------------------

static void test_boot_guard_status_reports_count_and_recovery_mode(void)
{
    TEST_SECTION("ota_boot_guard_status_get_handler -- unauthenticated, reports the real count and "
                 "recovery-mode flag with no auth gate");
    s_stub_boot_guard_count = 7;
    s_last_resp_body[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = ota_boot_guard_status_get_handler(&req);

    TEST_CHECK(err == ESP_OK, "handler always returns ESP_OK");
    TEST_CHECK(strstr(s_last_resp_body, "\"boot_count\":7") != NULL, "reports the real boot count");
    TEST_CHECK(strstr(s_last_resp_body, "\"recovery_mode\":false") != NULL,
              "reports the real recovery-mode flag (this file's boot_guard_is_recovery_mode() stub "
              "always returns false)");
    s_stub_boot_guard_count = 0;
}

// ---------------------------------------------------------------------------
// Finding 2 / Finding 3 coverage (2026-09-17 web-auth session review) --
// http_auth_extract_session_token()/extract_named_cookie() (http_auth_http.c)
// are linked into this executable for real (see the file header above), and
// this file's own stub_header_set()/stub_headers_reset() (defined further up
// for the httpd_req_get_hdr_value_len/_str stubs) give exactly the
// test-controllable "Cookie" header those functions need -- no separate
// executable required.

static void test_extract_session_token_skips_malformed_occurrence(void)
{
    // Finding 2: a malformed (empty-value) kiln_sid occurrence used to abort
    // the whole scan; the real cookie must still be found afterward.
    TEST_SECTION("http_auth_extract_session_token -- an earlier empty kiln_sid= does not hide a later valid one "
                 "(Finding 2)");
    stub_headers_reset();
    stub_header_set("Cookie", "kiln_sid=; other=1; kiln_sid=deadbeefcafef00d");

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    char out[128];
    bool ok = http_auth_extract_session_token(&req, out, sizeof(out));

    TEST_CHECK(ok, "scan continues past the malformed occurrence instead of failing outright");
    TEST_CHECK(strcmp(out, "deadbeefcafef00d") == 0, "the later, valid kiln_sid value is the one returned");
}

static void test_extract_session_token_large_cookie_header(void)
{
    // Finding 3: a Cookie header >= the old fixed 128-byte stack buffer must
    // still be parsed correctly (not silently dropped) now that the buffer
    // is heap-allocated to the header's real length.
    TEST_SECTION("http_auth_extract_session_token -- a >=128 byte Cookie header is still parsed (Finding 3)");
    stub_headers_reset();
    char big_cookie[300];
    // Several unrelated, longish cookies followed by a real kiln_sid --
    // total length deliberately well past 128 bytes.
    snprintf(big_cookie, sizeof(big_cookie),
             "unrelated_a=%s; unrelated_b=%s; kiln_sid=%s",
             "1111111111111111111111111111", "2222222222222222222222222222",
             "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcd");
    TEST_CHECK(strlen(big_cookie) >= 128, "test fixture is actually >= 128 bytes (sanity check on the fixture itself)");
    stub_header_set("Cookie", big_cookie);

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    char out[128];
    bool ok = http_auth_extract_session_token(&req, out, sizeof(out));

    TEST_CHECK(ok, "a Cookie header >= 128 bytes is still parsed, not silently dropped");
    TEST_CHECK(strcmp(out, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcd") == 0,
               "the real kiln_sid value at the end of the long header is extracted correctly");
}

// ---------------------------------------------------------------------------

void run_test_ota_http(void)
{
    esp_err_t start_err = ota_http_start(NULL, NULL, NULL);
    // ESP_ERR_INVALID_STATE is expected (wifi_provision_http_get_server()
    // stubbed to NULL) -- s_ota_lock and every lockout/nonce state are
    // already initialized by the time ota_http_start() reaches that check
    // (see its own body), which is all these tests need.
    TEST_CHECK(start_err == ESP_ERR_INVALID_STATE || start_err == ESP_OK,
              "ota_http_start setup must reach the point of initializing s_ota_lock");

    test_empty_password_refused_with_valid_nonce();
    test_boot_button_bypass_wins_even_with_empty_password();
    test_nonempty_password_valid_mac_succeeds();

    test_mac_for_one_context_rejected_for_another();
    test_pico_and_pico_rollback_macs_are_not_interchangeable();
    test_lockout_is_per_context_not_shared();

    test_missing_auth_header_never_reaches_interlock();
    test_authenticated_request_does_reach_interlock();

    test_check_interlocks_refuses_during_zone_sweep();
    test_check_interlocks_ok_when_no_sweep();

    test_boot_guard_reset_missing_auth_refused();
    test_boot_guard_reset_wrong_context_mac_refused();
    test_boot_guard_reset_authenticated_reports_success();
    test_boot_guard_reset_authenticated_reports_failure_honestly();
    test_boot_guard_status_reports_count_and_recovery_mode();

    test_ota_esp_status_web_auth_off_shows_build_identity();
    test_ota_esp_status_unauthenticated_redacts_build_identity();
    test_ota_esp_status_user_session_redacts_build_identity();
    test_ota_esp_status_admin_session_gets_full_payload();
    test_ota_esp_status_admin_session_denied_when_policy_unreadable();
    test_ota_esp_status_admin_session_survives_decoy_cookies();
    test_ota_esp_status_unnamed_cookie_value_is_not_a_session();
    {
        // Leave web auth off for every test after this one -- matches the
        // state every other pre-existing test in this file assumed
        // (unauthenticated handler calls throughout) before these four ran.
        web_auth_policy_t policy = { .web_enabled = false, .lcd_enabled = false,
                                      .web_timeout_s = -1, .lcd_timeout_s = -1 };
        TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init default nvs partition");
    TEST_CHECK(web_auth_store_set_policy(&policy) == HAL_OK, "setup: policy persisted");
        stub_headers_reset();
    }

    test_pico_rollback_post_returns_pending_without_blocking();
    test_pico_rollback_status_reports_idle_before_any_request();
    test_pico_rollback_status_reports_pending_while_in_progress();
    test_pico_rollback_status_reports_all_four_outcomes_honestly();

    /* sw_reset_post_handler() now REFUSES the whole route if it cannot create
     * its reboot task, so the accepted-path tests below need the stub to
     * report success; test_sw_reset_reports_task_creation_failure() flips it
     * back to pdFAIL for its own duration. */
    g_stub_task_create_result = pdPASS;
    test_sw_reset_missing_auth_never_reaches_interlock();
    test_sw_reset_refuses_during_firing();
    test_sw_reset_refuses_while_paused();
    test_sw_reset_refuses_during_autotune();
    test_sw_reset_ok_when_idle();
    test_sw_reset_body_does_not_claim_trip_clearing();
    test_sw_reset_reports_task_creation_failure();
    test_sw_reset_does_not_touch_nvs();
    test_sw_reset_pico_outcome_mapping();
    test_sw_reset_pico_sentences_are_honest();
    test_credential_survives_factory_reset_wifi_scope();
    test_credential_survives_factory_reset_kiln_scope();
    test_credential_survives_factory_reset_profiles_scope();
    test_credential_survives_factory_reset_all_scope();
    test_extract_session_token_skips_malformed_occurrence();
    test_extract_session_token_large_cookie_header();
}

int main(void)
{
    run_test_ota_http();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
