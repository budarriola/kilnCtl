// Host tests for App/drivers/http/ota_http.c and factory_reset.c.
//
// The AP-password HMAC scheme this file used to test (empty-password
// refusal, per-context MAC/lockout isolation, auth-before-interlock via a
// challenge nonce) was retired 2026-09-29 -- WEB_AUTH_PLAN.md item 2b, owner
// decision "Retire; open when login off". route_tier_table.h's ADMIN tier is
// now the sole gate on these routes, same as every other ADMIN route,
// whether web auth is on or off. What remains here is the genuinely
// unrelated coverage: the interlock/system-mode-gate/zone-sweep refusal
// logic, factory-reset scope/credential-survival behavior, sw_reset's
// firing/pause/autotune refusals, pico-rollback async status reporting, and
// admin/user session-based redaction on GET /api/ota/esp/status.
//
// APPROACH: this file #include's ota_http.c and factory_reset.c directly
// (rather than linking them as objects) to reach their `static` internals
// (reset_post_handler(), etc.) -- same convention test_zones_http.c/
// test_profiles_http.c use.
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
// NEGATIVE-TEST NOTE (repo rule: every check must be provable to fail): the
// remaining interlock/mode-gate tests were previously confirmed RED by
// temporarily editing ota_http.c/factory_reset.c, rebuilding, and rerunning
// this file's corresponding test, then restoring the file exactly and
// re-confirming GREEN -- see the commits that introduced each test for the
// specific edit used. The edits are not left in this file.
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
// that blunt). This file is different: ota_http.c's interlock/update-claim
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
// one extern web_auth_store.c's psa/crypto.h stub needs (ota_http.c itself
// no longer uses PSA crypto -- its AP-password HMAC scheme was retired
// 2026-09-29).
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
// esp_wifi.h -- declared extern there (stubs/esp_wifi.h, added 2026-09-21 for
// factory_reset.c's new esp_wifi_set_storage()/esp_wifi_restore() calls --
// docs/audits/wifi_factory_reset_driver_storage_2026-09-21.md), defined once
// here since factory_reset.c is compiled directly into this file.
// ---------------------------------------------------------------------------
int g_stub_wifi_set_storage_calls = 0;
wifi_storage_t g_stub_wifi_last_storage = WIFI_STORAGE_FLASH;
int g_stub_wifi_restore_calls = 0;

// ---------------------------------------------------------------------------
// lwip/sockets.h -- declared extern there for wifi_prov.c's host tests;
// referenced by that header's static inline getsockname()/inet_ntop(), which
// MSVC still needs a body for even though neither is ever actually called by
// any test in this file (get_client_ip()'s getpeername() always fails first).
// ---------------------------------------------------------------------------
int g_stub_getsockname_result = -1;
// 2026-09-29: widened 16 -> 48 to match stubs/lwip/sockets.h's extern
// declaration (bumped for wifi_prov_request_arrived_on_ap()'s IPv4-mapped
// AF_INET6 strings) -- unused by this file's own tests either way.
char g_stub_local_ip[48] = "";

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
// "profiles"/"all" scopes. Corrected 2026-09-21 (audit
// wifi_factory_reset_driver_storage_2026-09-21.md section 4): this symbol
// must resolve unconditionally because factory_reset.c is compiled directly
// into this file, but it is NOT true that no test here reaches a scope that
// executes -- the four test_credential_survives_factory_reset_*_scope()
// cases below call factory_reset_execute() directly (bypassing the HTTP
// handler and its interlock check) and do run execute_scope_job(), including
// the "profiles"/"all" scopes that hit this stub.
// ---------------------------------------------------------------------------
esp_err_t profiles_builtin_restore_all(void) { return ESP_OK; }
// ---------------------------------------------------------------------------
// profile_executor.h -- factory_reset.c's execute_scope_job() drops the
// last-run-started RAM cache after erasing profiles_nvs (that erase destroys
// "fs_<id>" without going through firing_stats_erase()). Same "never reached
// here, but must resolve" note as the stub above: profile_executor_*.c is not
// part of this host test build. The real function is covered by
// test_profile_executor_prestart.c.
void firing_stats_cache_invalidate_all(void) {}

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
// ---------------------------------------------------------------------------
// esp_partition.h / esp_ota_ops.h -- declared in those stub headers, defined
// here. None of these is ever invoked by this file's tests (only the
// interlock/mode-gate/reset_post_handler() paths are exercised, never the
// transfer handlers), but the whole translation unit must still link.
// ---------------------------------------------------------------------------
esp_err_t esp_partition_write(const esp_partition_t *partition, size_t dst_offset, const void *src, size_t size)
{ (void)partition; (void)dst_offset; (void)src; (void)size; return ESP_OK; }
esp_err_t esp_partition_erase_range(const esp_partition_t *partition, size_t offset, size_t size)
{ (void)partition; (void)offset; (void)size; return ESP_OK; }
uint32_t esp_partition_get_main_flash_sector_size(void) { return 4096u; }
// Referenced by net/pico_image_source.c, linked in for real (see
// build_host_tests.ps1's cmd8) because ota_pico_do_stage() now calls
// pico_image_source_describe() for KilnFW TODO.md 9.4's protocol-version
// warning. Never reached by a test in this file -- ota_pico_do_stage() is not
// exercised here -- but zero-fills rather than leaving the caller's buffer
// untouched, so the scan it feeds stays deterministic if that ever changes.
esp_err_t esp_partition_read(const esp_partition_t *partition, size_t src_offset, void *dst, size_t size)
{ (void)partition; (void)src_offset; if (dst) memset(dst, 0, size); return ESP_OK; }

const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start_from)
{ (void)start_from; return NULL; }
const esp_partition_t *esp_ota_get_running_partition(void) { return NULL; }

// recovery_switch.c is target-only (esp_image_verify); ota_http_recovery.c's
// recovery_boot handler only calls these two seams. Never reached by a test
// in this file except test_recovery_boot_set_failed_restores_boot_target().
static recovery_switch_result_t s_fake_select_result = RECOVERY_SWITCH_NOT_PRESENT;
static int s_fake_restore_calls = 0;
recovery_switch_result_t recovery_switch_select_boot(char *msg, size_t cap)
{ if (msg && cap) msg[0] = '\0'; return s_fake_select_result; }
bool recovery_switch_restore_running(void) { s_fake_restore_calls++; return true; }
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

// relay_authority.h -- factory_reset.c's system_mode_gate wiring
// (docs/SYSTEM_MODE_GATE_PLAN.md, gate-slices-2/4/5, 2026-09-25) reads this
// same profile_running/autotune_running snapshot; derive it from the SAME
// g_stub_profile_state/g_stub_autotune_active globals the fakes above use,
// so a test that sets those up for the existing sw_reset refusal checks
// exercises the identical scenario here too.
void relay_authority_heat_run_active(bool *profile_running_out, bool *autotune_running_out)
{
    if (profile_running_out) {
        *profile_running_out = (g_stub_profile_state == PROFILE_EXEC_RUNNING ||
                                 g_stub_profile_state == PROFILE_EXEC_PAUSED);
    }
    if (autotune_running_out) {
        *autotune_running_out = g_stub_autotune_active;
    }
}

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
                                   uint8_t *out_reason_code, uint8_t *out_boot_id_before,
                                   uint8_t *out_boot_id_after)
{
    (void)link;
    (void)out_reason_code;
    (void)out_boot_id_before;
    (void)out_boot_id_after;
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

// thermo_owner.h / kiln_io_owner.h -- never reached (thermo_count == 0 above
// skips the loop that would call these), must resolve.
// docs/HTTP_HANDLER_OWNERSHIP.md Batch A (2026-09-22) routed
// ota_http_check_interlocks() through these owner accessors instead of
// MAX31856_read_all()/kiln_io_read() directly -- stubs renamed to match.
esp_err_t thermo_owner_command_read_all(MAX31856Reading *out, size_t max_readings, size_t *out_count)
{ (void)out; (void)max_readings; if (out_count) *out_count = 0; return ESP_FAIL; }
static esp_err_t s_fake_io_read_result = ESP_FAIL;
esp_err_t kiln_io_owner_command_read(kiln_io_state_t *out)
{ if (out) memset(out, 0, sizeof(*out)); return s_fake_io_read_result; }

// ota_pico_relay.h -- never called by ota_http.c's own tests, but review
// finding D6 adds a direct test of pico_img_stage.c (linked in for real,
// see build_host_tests.ps1's cmd8) below, which needs a non-NULL partition
// to stage into. Sized generously (1 MiB) so ordinary test payloads never
// trip the "too large" path by accident.
static esp_partition_t s_fake_pico_img_partition = {
    .address = 0, .size = 0x100000u, .label = "pico_img", .type = 1, .subtype = 0, .encrypted = false,
};
const esp_partition_t *ota_pico_img_partition(void) { return &s_fake_pico_img_partition; }
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
// Test-controllable third state (present/value/absent) so the handlers'
// persisted_count field -- added so a caller can tell "really cleared just
// now" apart from boot_count's fixed-for-the-boot value -- can be exercised
// without the real NVS-backed boot_guard.c (covered directly by
// test_boot_guard.c's own persisted-count tests).
static bool s_stub_boot_guard_persisted_available = true;
static uint32_t s_stub_boot_guard_persisted_count = 0;
bool boot_guard_get_persisted_count(uint32_t *out_count)
{
    if (!s_stub_boot_guard_persisted_available) {
        return false;
    }
    *out_count = s_stub_boot_guard_persisted_count;
    return true;
}

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

// The AP-password HMAC scheme (context strings, nonce, per-context lockout,
// ota_http_verify_request()) was retired 2026-09-29 -- WEB_AUTH_PLAN.md item
// 2b, owner decision "Retire; open when login off". The tests that used to
// live here (context-string-length bound, empty-password refusal, BOOT-
// button bypass ordering, cross-context MAC rejection, per-context lockout
// isolation) exercised exactly that removed mechanism and are gone with it.
// route_tier_table.h's ADMIN tier is now the only gate on these routes, on
// or off, same as every other ADMIN route -- covered by the existing ADMIN-
// tier host tests (http_auth_http.c's own suite), not by this file.

static void test_authenticated_request_does_reach_interlock(void)
{
    TEST_SECTION("reset_post_handler -- a request reaches the interlock check (auth is ADMIN-tier only now)");
    stub_headers_reset(); // no X-Ota-Mac header needed any more -- ADMIN tier is the only gate
    g_probe_interlock_called = false;
    s_last_err_code = 0;
    s_last_err_msg[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = reset_post_handler(&req);

    TEST_CHECK(err == ESP_OK, "reset_post_handler must always return ESP_OK");
    TEST_CHECK(s_last_err_code != 400, "no request here is refused at a (removed) auth-header step");
    TEST_CHECK(g_probe_interlock_called,
              "the interlock check runs for an ordinary request -- proving reset_post_handler() still "
              "reaches profile_executor_get_status() now that the auth-header step is gone");
}

static void test_recovery_boot_set_failed_restores_boot_target(void)
{
    TEST_SECTION("recovery_boot -- SET_FAILED restores the running boot target and answers 500, not a clean 409");
    static int dummy_io;
    kiln_io_t *saved_io = s_io;
    s_io = (kiln_io_t *)&dummy_io;
    s_fake_io_read_result = ESP_OK; /* relay_shadow == 0: relays off */
    g_stub_profile_state = PROFILE_EXEC_IDLE;
    stub_headers_reset();
    stub_header_set("X-Ota-Ack-No-Safety", "1"); /* no safety link in this fixture */
    s_fake_select_result = RECOVERY_SWITCH_SET_FAILED;
    s_fake_restore_calls = 0;
    s_last_resp_status[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = ota_recovery_boot_post_handler(&req);

    TEST_CHECK(err == ESP_OK, "ota_recovery_boot_post_handler must always return ESP_OK");
    TEST_CHECK(s_fake_restore_calls == 1,
              "SET_FAILED may already have erased otadata -- the handler must call recovery_switch_restore_running()");
    TEST_CHECK(strncmp(s_last_resp_status, "500", 3) == 0,
              "SET_FAILED is a 500, never the 409 that claims nothing was written");

    /* A plain refusal (image invalid) wrote nothing and must NOT restore. */
    s_fake_select_result = RECOVERY_SWITCH_INVALID;
    s_fake_restore_calls = 0;
    s_last_resp_status[0] = '\0';
    (void)ota_recovery_boot_post_handler(&req);
    TEST_CHECK(s_fake_restore_calls == 0, "INVALID writes nothing, so no restore");
    TEST_CHECK(strncmp(s_last_resp_status, "409", 3) == 0, "INVALID stays a 409");

    s_fake_select_result = RECOVERY_SWITCH_NOT_PRESENT;
    s_fake_io_read_result = ESP_FAIL;
    s_io = saved_io;
}

static void test_factory_reset_refused_by_system_mode_gate_during_firing(void)
{
    TEST_SECTION("reset_post_handler -- system_mode_gate refuses with a 409 while a firing is active (owner Q3, 2026-09-25)");
    g_stub_profile_state = PROFILE_EXEC_RUNNING;
    stub_headers_reset();

    g_probe_interlock_called = false;
    s_last_resp_status[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = reset_post_handler(&req);

    TEST_CHECK(err == ESP_OK, "reset_post_handler must always return ESP_OK");
    TEST_CHECK(strcmp(s_last_resp_status, "409 Conflict") == 0,
              "system_mode_gate's refusal is a distinct 409, never the OTA interlock's 428");
    TEST_CHECK(!g_probe_interlock_called,
              "the system_mode_gate check runs BEFORE the OTA interlock check and short-circuits it -- "
              "unconditional, unlike the interlock's ack-header escape hatch");

    g_stub_profile_state = PROFILE_EXEC_IDLE;
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

// Lets a test reach ota_pico_rollback_post_handler()'s interlock/mutex gate
// with one call (ADMIN tier is the only auth gate now -- no header to build).
static void set_pico_rollback_headers_for(const char *password)
{
    (void)password;
    stub_headers_reset();
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
    stub_headers_reset();
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

// Task 1d (docs/SYSTEM_MODE_GATE_PLAN.md known gap): the UART path
// (SYSTEM_CMD_FACTORY_RESET, uart_bridge_system.c) never goes through
// reset_post_handler()'s HTTP auth/interlock gate above -- it calls
// factory_reset_execute() directly and switches on ITS return code, per
// uart_bridge_system.c's own comment: FACTORY_RESET_ERR_MODE_GATE_REFUSED is
// logged as a refusal ("nothing erased"), distinct from a genuine erase
// failure (which still reboots). This proves the return-code contract that
// mapping depends on: a direct call during an active run returns exactly
// that sentinel, and nothing is actually erased -- never ESP_ERR_INVALID_STATE
// (which the erase loop can also legitimately return, the exact ambiguity
// factory_reset.h's own doc comment warns about, see uart_bridge_system.c's
// 2026-09-25 review-fix comment on this same overlap).
static void test_factory_reset_execute_refused_by_mode_gate_during_firing(void)
{
    TEST_SECTION("factory_reset_execute() -- returns FACTORY_RESET_ERR_MODE_GATE_REFUSED directly "
                 "(the UART path's own call site, no HTTP handler involved) while a firing is active, "
                 "and erases nothing");
    fake_kv_reset_all();
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init the default nvs partition");
    seed_webauth12b_credential();
    g_stub_profile_state = PROFILE_EXEC_RUNNING;

    esp_err_t err = factory_reset_execute(FACTORY_RESET_SCOPE_ALL);

    TEST_CHECK(err == FACTORY_RESET_ERR_MODE_GATE_REFUSED,
              "must return exactly the mode-gate sentinel, not ESP_ERR_INVALID_STATE or ESP_OK -- "
              "uart_bridge_system.c's if/else chain switches on this exact value");
    assert_webauth12b_credential_survived("factory_reset_execute() refused by the mode gate must erase "
                                          "nothing at all, not even the scopes it would normally touch");

    g_stub_profile_state = PROFILE_EXEC_IDLE;
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

// --- esp_wifi_restore() is called for wifi/all scopes only, never kiln/profiles ---
//
// docs/audits/wifi_factory_reset_driver_storage_2026-09-21.md section 5 step
// 2: the IDF Wi-Fi driver keeps its own persisted copy of the STA/AP config
// in the default nvs partition (nvs.net80211), independent of wifi_nvs.
// execute_scope_job()'s new loop must clear it (esp_wifi_restore()) whenever
// the scope's partition list includes wifi_nvs, and must never touch it for
// scopes that don't -- kiln/profiles reset kiln or profile data only, and
// must not perturb Wi-Fi state at all.
static void reset_wifi_restore_stub_counters(void)
{
    g_stub_wifi_set_storage_calls = 0;
    g_stub_wifi_last_storage = WIFI_STORAGE_FLASH;
    g_stub_wifi_restore_calls = 0;
}

static void test_factory_reset_wifi_scope_calls_esp_wifi_restore(void)
{
    TEST_SECTION("factory_reset_execute(WIFI) must call esp_wifi_restore() exactly once");
    fake_kv_reset_all();
    TEST_CHECK(hal_kv_init_partition("wifi_nvs") == HAL_OK, "setup: init wifi_nvs");
    reset_wifi_restore_stub_counters();
    TEST_CHECK(factory_reset_execute(FACTORY_RESET_SCOPE_WIFI) == ESP_OK,
              "factory_reset_execute(WIFI) must succeed");
    TEST_CHECK(g_stub_wifi_restore_calls == 1,
              "esp_wifi_restore() called exactly once for the WIFI scope");
    TEST_CHECK(g_stub_wifi_last_storage == WIFI_STORAGE_RAM,
              "storage left in RAM mode after the restore's FLASH/RAM bracket");
}

static void test_factory_reset_all_scope_calls_esp_wifi_restore(void)
{
    TEST_SECTION("factory_reset_execute(ALL) must call esp_wifi_restore() exactly once");
    fake_kv_reset_all();
    TEST_CHECK(hal_kv_init_partition("wifi_nvs") == HAL_OK, "setup: init wifi_nvs");
    TEST_CHECK(hal_kv_init_partition("kiln_nvs") == HAL_OK, "setup: init kiln_nvs");
    TEST_CHECK(hal_kv_init_partition("profiles_nvs") == HAL_OK, "setup: init profiles_nvs");
    reset_wifi_restore_stub_counters();
    TEST_CHECK(factory_reset_execute(FACTORY_RESET_SCOPE_ALL) == ESP_OK,
              "factory_reset_execute(ALL) must succeed");
    TEST_CHECK(g_stub_wifi_restore_calls == 1,
              "esp_wifi_restore() called exactly once for the ALL scope");
}

static void test_factory_reset_kiln_scope_never_calls_esp_wifi_restore(void)
{
    TEST_SECTION("factory_reset_execute(KILN) must never call esp_wifi_restore()");
    fake_kv_reset_all();
    TEST_CHECK(hal_kv_init_partition("kiln_nvs") == HAL_OK, "setup: init kiln_nvs");
    reset_wifi_restore_stub_counters();
    TEST_CHECK(factory_reset_execute(FACTORY_RESET_SCOPE_KILN) == ESP_OK,
              "factory_reset_execute(KILN) must succeed");
    TEST_CHECK(g_stub_wifi_restore_calls == 0,
              "esp_wifi_restore() must not be called for a scope that never touches wifi_nvs");
}

static void test_factory_reset_profiles_scope_never_calls_esp_wifi_restore(void)
{
    TEST_SECTION("factory_reset_execute(PROFILES) must never call esp_wifi_restore()");
    fake_kv_reset_all();
    TEST_CHECK(hal_kv_init_partition("profiles_nvs") == HAL_OK, "setup: init profiles_nvs");
    reset_wifi_restore_stub_counters();
    TEST_CHECK(factory_reset_execute(FACTORY_RESET_SCOPE_PROFILES) == ESP_OK,
              "factory_reset_execute(PROFILES) must succeed");
    TEST_CHECK(g_stub_wifi_restore_calls == 0,
              "esp_wifi_restore() must not be called for a scope that never touches wifi_nvs");
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
    // 2026-09-23: NO_REPLY + 1 is now CONFIRMED_BY_BOOT_ID, a real, named
    // value with its own explicit case -- use the next slot past that
    // instead to stand in for a genuinely future/unrecognized enumerator.
    TEST_CHECK(sw_reset_classify_pico_outcome(
                   ESP_OK, (safety_link_reboot_outcome_t)(SAFETY_LINK_REBOOT_OUTCOME_CONFIRMED_BY_BOOT_ID + 1),
                   0) == SW_RESET_PICO_UNCONFIRMED,
               "an unrecognized outcome value reads as UNCONFIRMED, so a future outcome cannot "
               "silently become success");

    // 2026-09-23: no REBOOT_RESULT arrived, but the bounded fallback watch
    // saw the peer's boot_id actually change -- this is positive evidence,
    // distinct from both plain silence (UNCONFIRMED) and a wire-visible
    // accepted=1 (ACCEPTED), and must get its own report value.
    TEST_CHECK(sw_reset_classify_pico_outcome(ESP_OK, SAFETY_LINK_REBOOT_OUTCOME_CONFIRMED_BY_BOOT_ID, 0) ==
                   SW_RESET_PICO_CONFIRMED_BY_BOOT_ID,
               "a boot_id change observed within the fallback watch is CONFIRMED_BY_BOOT_ID, "
               "not UNCONFIRMED and not ACCEPTED");
    TEST_CHECK(sw_reset_classify_pico_outcome(ESP_ERR_INVALID_STATE,
                                              SAFETY_LINK_REBOOT_OUTCOME_CONFIRMED_BY_BOOT_ID, 0) ==
                   SW_RESET_PICO_UNCONFIRMED,
               "a non-ESP_OK return is UNCONFIRMED even when the outcome argument says "
               "CONFIRMED_BY_BOOT_ID");
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
    const char *confirmed_by_boot_id = sw_reset_pico_sentence(SW_RESET_PICO_CONFIRMED_BY_BOOT_ID);

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

    // 2026-09-23: the fallback-generic sentence must not claim an explicit
    // accepted=1 was ever seen -- it is inferred from a boot_id change, not
    // wire-visible acceptance.
    TEST_CHECK(strstr(confirmed_by_boot_id, "accepted") == NULL,
               "the boot_id-fallback sentence must never contain the word accepted -- no "
               "accepted=1 was ever observed on the wire");

    // Every outcome must have its own distinct sentence -- a duplicate would
    // mean two genuinely different results read identically to the operator.
    const char *all[] = {accepted,     refused_armed, refused_transfer,    refused_other,
                         unconfirmed, no_link,       confirmed_by_boot_id};
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
    // 2026-09-23: was "all six" -- confirmed_by_boot_id is a seventh outcome
    // that Opus review of 5719982c found missing from this distinctness
    // check entirely, so a collision with it would have gone undetected.
    TEST_CHECK(all_distinct, "all seven outcome sentences are distinct");
}

// The X-Ota-Mac header parse/length-check drift guard (all six mutating OTA
// routes vs. a shared helper) tested exactly the retired HMAC scheme and is
// gone with it -- see the removal note near test_authenticated_request_does_
// reach_interlock() above. Header shape no longer matters: ADMIN tier is the
// only gate.

// ---------------------------------------------------------------------------
// POST /api/ota/esp/boot_guard_reset -- docs/audits/
// boot_guard_post_flash_recovery_footgun_2026-09-08.md's tool-driven trigger.
// boot_guard_reset_counter() itself (NVS write/verify/retry) is covered by
// test_boot_guard.c against the real boot_guard.c; this file's stub
// (s_stub_boot_guard_reset_verified) exists purely to exercise the HTTP
// layer's own honest ok:true/false reporting (its former auth-ordering
// coverage is gone along with the AP-password HMAC scheme -- ADMIN tier is
// the only gate now).
// ---------------------------------------------------------------------------

static void test_boot_guard_reset_authenticated_reports_success(void)
{
    TEST_SECTION("ota_boot_guard_reset_post_handler -- calls boot_guard_reset_counter() and reports "
                 "ok:true when it verifies");
    s_stub_boot_guard_reset_verified = true;
    s_stub_boot_guard_count = 0;
    s_stub_boot_guard_persisted_available = true;
    s_stub_boot_guard_persisted_count = 0;
    stub_headers_reset();
    s_last_err_code = 0;
    s_last_err_msg[0] = '\0';
    s_last_resp_body[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = ota_boot_guard_reset_post_handler(&req);

    TEST_CHECK(err == ESP_OK, "handler always returns ESP_OK");
    TEST_CHECK(s_last_err_code == 0, "an ordinary request is not refused");
    TEST_CHECK(strstr(s_last_resp_body, "\"ok\":true") != NULL,
              "reports ok:true when boot_guard_reset_counter() verifies its clear");
    TEST_CHECK(strstr(s_last_resp_body, "\"persisted_count\":0") != NULL,
              "reports the live persisted count (re-read AFTER the reset), not just boot_count -- "
              "boot_count alone never moves across this call and used to make a genuine clear read "
              "like a no-op");
}

static void test_boot_guard_reset_omits_persisted_count_when_unavailable(void)
{
    TEST_SECTION("ota_boot_guard_reset_post_handler -- omits persisted_count rather than fabricating "
                 "a value when boot_guard_get_persisted_count() itself cannot read it back");
    s_stub_boot_guard_reset_verified = true;
    s_stub_boot_guard_count = 0;
    s_stub_boot_guard_persisted_available = false;
    stub_headers_reset();
    s_last_err_code = 0;
    s_last_err_msg[0] = '\0';
    s_last_resp_body[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = ota_boot_guard_reset_post_handler(&req);

    TEST_CHECK(err == ESP_OK, "handler always returns ESP_OK");
    TEST_CHECK(strstr(s_last_resp_body, "\"ok\":true") != NULL, "still reports ok:true from the reset itself");
    TEST_CHECK(strstr(s_last_resp_body, "persisted_count") == NULL,
              "omits the field entirely rather than printing a fabricated 0 or null when the "
              "read-back itself failed");
    s_stub_boot_guard_persisted_available = true;
}

static void test_boot_guard_reset_authenticated_reports_failure_honestly(void)
{
    TEST_SECTION("ota_boot_guard_reset_post_handler -- reports ok:false, not a bare 200 that implies "
                 "success, when boot_guard_reset_counter() could NOT verify the clear");
    s_stub_boot_guard_reset_verified = false; // the lying-write case, from the caller's side
    s_stub_boot_guard_persisted_available = true;
    s_stub_boot_guard_persisted_count = 2;
    stub_headers_reset();
    s_last_err_code = 0;
    s_last_err_msg[0] = '\0';
    s_last_resp_body[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = ota_boot_guard_reset_post_handler(&req);

    TEST_CHECK(err == ESP_OK, "handler always returns ESP_OK -- it still responds 200 either way, "
              "the honesty is in the body, not the HTTP status");
    TEST_CHECK(s_last_err_code == 0, "an ordinary request is not refused at an auth step");
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
    s_stub_boot_guard_persisted_available = true;
    s_stub_boot_guard_persisted_count = 3;
    s_last_resp_body[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = ota_boot_guard_status_get_handler(&req);

    TEST_CHECK(err == ESP_OK, "handler always returns ESP_OK");
    TEST_CHECK(strstr(s_last_resp_body, "\"boot_count\":7") != NULL, "reports the real boot count");
    TEST_CHECK(strstr(s_last_resp_body, "\"recovery_mode\":false") != NULL,
              "reports the real recovery-mode flag (this file's boot_guard_is_recovery_mode() stub "
              "always returns false)");
    TEST_CHECK(strstr(s_last_resp_body, "\"persisted_count\":3") != NULL,
              "also reports the live persisted count, distinct from boot_count");
    s_stub_boot_guard_count = 0;
}

static void test_boot_guard_status_omits_persisted_count_when_unavailable(void)
{
    TEST_SECTION("ota_boot_guard_status_get_handler -- omits persisted_count rather than fabricating "
                 "a value when the read-back itself fails");
    s_stub_boot_guard_count = 7;
    s_stub_boot_guard_persisted_available = false;
    s_last_resp_body[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    esp_err_t err = ota_boot_guard_status_get_handler(&req);

    TEST_CHECK(err == ESP_OK, "handler always returns ESP_OK");
    TEST_CHECK(strstr(s_last_resp_body, "\"boot_count\":7") != NULL, "still reports boot_count");
    TEST_CHECK(strstr(s_last_resp_body, "persisted_count") == NULL,
              "omits the field entirely rather than a fabricated value");
    s_stub_boot_guard_count = 0;
    s_stub_boot_guard_persisted_available = true;
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

// 2026-09-17 adversarial review of the Finding-1 fix (client_ip buffer
// size): ota_http_get_client_ip() used to pass inet_ntop()'s output buffer
// straight through without checking its return value. lwip's inet_ntop(),
// like BSD's, does not write to its output on failure, so that failure path
// left the caller's buffer holding whatever was already on the stack --
// undefined content then fed into a strcmp() security comparison. The fix
// pulls the "what goes in `out` on success vs. every failure" decision into
// ota_http_client_ip_finalize() (ota_http_util.h/.c), a pure function this
// test exercises directly without needing a real socket.
static void test_client_ip_finalize_writes_real_address_on_success(void)
{
    TEST_SECTION("ota_http_client_ip_finalize -- a resolved address is copied verbatim and reports true");
    char out[46];
    memset(out, 0x5A, sizeof(out)); // poison pattern -- would survive untouched if the function did nothing
    bool known = ota_http_client_ip_finalize(out, sizeof(out), "192.0.2.7");
    TEST_CHECK(known, "a real formatted address reports true (a real client IP)");
    TEST_CHECK(strcmp(out, "192.0.2.7") == 0, "the real address is copied into out verbatim");
}

static void test_client_ip_finalize_defined_on_null_formatted_addr(void)
{
    // This is the exact case the review flagged: formatted_addr == NULL
    // models httpd_req_to_sockfd() < 0, getpeername() != 0, OR inet_ntop()
    // returning NULL -- every failure path collapses to this one call.
    TEST_SECTION("ota_http_client_ip_finalize -- NULL formatted_addr (any failure path) leaves out defined, not "
                 "untouched/uninitialized, and reports false");
    char out[46];
    memset(out, 0x5A, sizeof(out)); // poison pattern
    bool known = ota_http_client_ip_finalize(out, sizeof(out), NULL);
    TEST_CHECK(!known, "an undetermined address reports false");
    TEST_CHECK(strcmp(out, "unknown") == 0, "out holds the defined sentinel \"unknown\", not leftover poison bytes");
    TEST_CHECK(out[0] != (char)0x5A, "the poison byte at out[0] was actually overwritten");
}

static void test_client_ip_finalize_terminates_with_undersized_buffer(void)
{
    // snprintf() truncates rather than overflows even when out_len is too
    // small for "unknown" (7 chars + NUL) -- confirms this function never
    // hands back a non-NUL-terminated buffer regardless of caller sizing.
    TEST_SECTION("ota_http_client_ip_finalize -- undersized out_len still NUL-terminates, never overflows");
    char out[4];
    memset(out, 0x5A, sizeof(out));
    bool known = ota_http_client_ip_finalize(out, sizeof(out), NULL);
    TEST_CHECK(!known, "still reports false (undetermined) even when the sentinel had to be truncated");
    TEST_CHECK(out[sizeof(out) - 1] == '\0', "out is NUL-terminated within the caller's buffer size");
}

// ---------------------------------------------------------------------------
// Review finding D6: pico_img_stage.c (linked in for real above) writes
// offset/CRC bookkeeping that a caller's own chunk-length arithmetic must
// not be able to break. pico_img_stage_begin() erases only
// ceil(total_len / sector_size) sectors -- writing past total_len would
// land in un-erased flash -- so pico_img_stage_write_chunk() must refuse an
// overrun rather than trust every caller. s_fake_pico_img_partition above
// backs these calls; esp_partition_write() above is faked to always
// succeed, so this exercises pico_img_stage.c's own bookkeeping, not flash
// I/O.

static void test_pico_img_stage_offset_and_crc_bookkeeping(void)
{
    TEST_SECTION("pico_img_stage -- offset/CRC bookkeeping across chunks");
    pico_img_stage_ctx_t ctx;
    char fail_reason[96];
    pico_img_stage_begin_result_t result = PICO_IMG_STAGE_BEGIN_ERASE_FAILED; // poisoned
    const uint8_t chunk1[4] = { 0x01, 0x02, 0x03, 0x04 };
    const uint8_t chunk2[3] = { 0x05, 0x06, 0x07 };
    size_t total = sizeof(chunk1) + sizeof(chunk2);

    TEST_CHECK(pico_img_stage_begin(&ctx, total, fail_reason, sizeof(fail_reason), &result),
               "begin() succeeds against the fake partition");
    TEST_CHECK(result == PICO_IMG_STAGE_BEGIN_OK, "out-result reports OK on success");
    TEST_CHECK(ctx.written == 0 && ctx.total_len == total, "ctx starts at offset 0 with the given total");

    TEST_CHECK(pico_img_stage_write_chunk(&ctx, chunk1, sizeof(chunk1), fail_reason, sizeof(fail_reason)),
               "first chunk writes clean");
    TEST_CHECK(ctx.written == sizeof(chunk1), "written advances by exactly the first chunk's length");

    TEST_CHECK(pico_img_stage_write_chunk(&ctx, chunk2, sizeof(chunk2), fail_reason, sizeof(fail_reason)),
               "second chunk (landing exactly at total_len) writes clean");
    TEST_CHECK(ctx.written == total, "written now equals the full staged total, exactly, not more");

    uint32_t crc_two_calls = ctx.crc;
    pico_img_stage_ctx_t ctx_one_call;
    result = PICO_IMG_STAGE_BEGIN_ERASE_FAILED;
    TEST_CHECK(pico_img_stage_begin(&ctx_one_call, total, fail_reason, sizeof(fail_reason), &result),
               "begin() again for the single-call comparison");
    uint8_t whole[7];
    memcpy(whole, chunk1, sizeof(chunk1));
    memcpy(whole + sizeof(chunk1), chunk2, sizeof(chunk2));
    TEST_CHECK(pico_img_stage_write_chunk(&ctx_one_call, whole, sizeof(whole), fail_reason, sizeof(fail_reason)),
               "the same bytes written in one call also write clean");
    TEST_CHECK(ctx_one_call.crc == crc_two_calls,
               "CRC is identical whether the bytes arrive in two chunks or one -- pure running fold, "
               "no per-call reset");
}

static void test_pico_img_stage_write_chunk_refuses_overrun(void)
{
    TEST_SECTION("pico_img_stage -- write_chunk refuses writing past the staged total (D6)");
    pico_img_stage_ctx_t ctx;
    char fail_reason[96];
    fail_reason[0] = '\0';
    pico_img_stage_begin_result_t result;
    const uint8_t chunk1[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
    const uint8_t overrun_chunk[4] = { 0x11, 0x22, 0x33, 0x44 }; // 4 + 4 = 8 > total_len of 6

    TEST_CHECK(pico_img_stage_begin(&ctx, 6u, fail_reason, sizeof(fail_reason), &result),
               "begin() stages a 6-byte total");
    TEST_CHECK(pico_img_stage_write_chunk(&ctx, chunk1, sizeof(chunk1), fail_reason, sizeof(fail_reason)),
               "first 4 bytes, within budget, write clean");

    size_t written_before = ctx.written;
    uint32_t crc_before = ctx.crc;
    fail_reason[0] = '\0';
    bool ok = pico_img_stage_write_chunk(&ctx, overrun_chunk, sizeof(overrun_chunk), fail_reason,
                                         sizeof(fail_reason));
    TEST_CHECK(!ok, "a chunk that would push written past total_len is refused");
    TEST_CHECK(fail_reason[0] != '\0', "a non-empty failure reason is reported for the overrun");
    TEST_CHECK(ctx.written == written_before, "written is NOT advanced by the refused chunk");
    TEST_CHECK(ctx.crc == crc_before, "the CRC is NOT folded with the refused chunk's bytes");

    // A same-size chunk that lands EXACTLY on total_len must still succeed --
    // this is the boundary the overrun check must not falsely reject.
    const uint8_t exact_chunk[2] = { 0x55, 0x66 }; // 4 + 2 = 6 == total_len
    TEST_CHECK(pico_img_stage_write_chunk(&ctx, exact_chunk, sizeof(exact_chunk), fail_reason,
                                          sizeof(fail_reason)),
               "a chunk landing exactly at total_len (not past it) still succeeds");
    TEST_CHECK(ctx.written == 6u, "written now equals total_len exactly");
}

// ---------------------------------------------------------------------------
// N5 (opus review, 2026-09-20): content_len over the fake pico_img
// partition's 1 MiB size must be refused as TOO_LARGE at the
// pico_img_stage_begin() level AND as HTTP 400 at the full
// ota_pico_do_stage() level -- not just one or the other. s_fake_pico_img_partition
// above is exactly 0x100000 (1 MiB); 0x100001 is one byte past it.

static void test_pico_img_stage_begin_refuses_oversize(void)
{
    TEST_SECTION("pico_img_stage_begin -- content_len past the partition size is TOO_LARGE (N5)");
    pico_img_stage_ctx_t ctx;
    char fail_reason[96];
    fail_reason[0] = '\0';
    pico_img_stage_begin_result_t result = PICO_IMG_STAGE_BEGIN_OK; // poisoned
    bool ok = pico_img_stage_begin(&ctx, 0x100001u, fail_reason, sizeof(fail_reason), &result);
    TEST_CHECK(!ok, "begin() refuses a total_len larger than the staging partition");
    TEST_CHECK(result == PICO_IMG_STAGE_BEGIN_TOO_LARGE, "out-result names TOO_LARGE specifically");
    TEST_CHECK(fail_reason[0] != '\0', "a non-empty failure reason is reported");
}

static void test_ota_pico_do_stage_refuses_oversize_with_http_400(void)
{
    TEST_SECTION("ota_pico_do_stage -- an oversize Content-Length is refused with HTTP 400, not 500 (N5)");
    memset(&s_rollback_test_safety, 0, sizeof(s_rollback_test_safety));
    ota_http_safety = &s_rollback_test_safety;
    s_last_err_code = 0;
    s_last_err_msg[0] = '\0';

    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.content_len = 0x100001u; // one byte past s_fake_pico_img_partition's 1 MiB

    ota_pico_do_stage(&req, "10.0.0.1");

    TEST_CHECK(s_last_err_code == 400,
              "an oversize image is a CLIENT error (400), not a board-side failure (500) -- "
              "review finding D5's TOO_LARGE-vs-everything-else distinction, exercised end to end "
              "through the real handler rather than only at pico_img_stage_begin() in isolation");

    ota_http_safety = NULL; // restore -- every other test in this file expects ota_http_safety == NULL
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

    test_authenticated_request_does_reach_interlock();
    test_factory_reset_refused_by_system_mode_gate_during_firing();
    test_recovery_boot_set_failed_restores_boot_target();
    test_factory_reset_execute_refused_by_mode_gate_during_firing();

    test_check_interlocks_refuses_during_zone_sweep();
    test_check_interlocks_ok_when_no_sweep();

    test_boot_guard_reset_authenticated_reports_success();
    test_boot_guard_reset_omits_persisted_count_when_unavailable();
    test_boot_guard_reset_authenticated_reports_failure_honestly();
    test_boot_guard_status_reports_count_and_recovery_mode();
    test_boot_guard_status_omits_persisted_count_when_unavailable();

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
    test_factory_reset_wifi_scope_calls_esp_wifi_restore();
    test_factory_reset_all_scope_calls_esp_wifi_restore();
    test_factory_reset_kiln_scope_never_calls_esp_wifi_restore();
    test_factory_reset_profiles_scope_never_calls_esp_wifi_restore();
    test_extract_session_token_skips_malformed_occurrence();
    test_extract_session_token_large_cookie_header();

    test_client_ip_finalize_writes_real_address_on_success();
    test_client_ip_finalize_defined_on_null_formatted_addr();
    test_client_ip_finalize_terminates_with_undersized_buffer();

    test_pico_img_stage_offset_and_crc_bookkeeping();
    test_pico_img_stage_write_chunk_refuses_overrun();
    test_pico_img_stage_begin_refuses_oversize();
    test_ota_pico_do_stage_refuses_oversize_with_http_400();
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
