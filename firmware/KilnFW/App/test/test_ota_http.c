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

#undef xSemaphoreTake
#undef asm

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
// esp_random.h
// ---------------------------------------------------------------------------
void esp_fill_random(void *buf, size_t len)
{
    // Not real randomness (host tests must be deterministic) -- never
    // actually reached by any test in this file (ota_challenge_get_handler()
    // is never called), just deterministic filler so the file links.
    uint8_t *b = (uint8_t *)buf;
    for (size_t i = 0; i < len; i++) b[i] = (uint8_t)(i * 37u + 11u);
}

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
void profile_executor_get_status(profile_exec_status_t *out)
{
    g_probe_interlock_called = true;
    if (out) memset(out, 0, sizeof(*out)); // PROFILE_EXEC_IDLE == 0
}

// autotune_engine.h
bool autotune_engine_is_active(void) { return false; }

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
typedef struct { const char *field; char value[80]; bool set; } stub_hdr_t;
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
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s) { (void)r; (void)s; return ESP_OK; }
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

    test_pico_rollback_post_returns_pending_without_blocking();
    test_pico_rollback_status_reports_idle_before_any_request();
    test_pico_rollback_status_reports_pending_while_in_progress();
    test_pico_rollback_status_reports_all_four_outcomes_honestly();
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
