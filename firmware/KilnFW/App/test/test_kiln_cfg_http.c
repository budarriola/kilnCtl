// Host test for App/drivers/http/kiln_cfg_http.c's apply_post_handler() --
// POST /api/kiln_configs/apply. Added to close a known test gap named in
// docs/SYSTEM_MODE_GATE.md section 3.6 slice 4: this handler's
// system_mode_gate wiring (owner decision Q2, 2026-09-25) was verified only
// by code-pattern review and an ESP-IDF target build, never a host-test
// assertion. kiln_cfg_http.c has NO other host-test coverage at all today
// (test_kiln_cfg_store.c/test_kiln_cfg_swap.c cover the lower-level store/
// swap-transaction modules, never this HTTP handler file).
//
// apply_post_handler() is `static` with no public seam -- same convention as
// zones_http.c/backup_http.c's own handlers (test_zones_http.c/
// test_backup_import.c's header comments) -- so this file #includes
// kiln_cfg_http.c directly. Every OTHER module it calls into is faked here:
// this test proves ONLY the refusal ORDER the plan's slice-4 note documents
// (404 existence check, then the system_mode_gate, then the OTA interlock,
// then http_async_job_busy()) and that an idle, existing, non-conflicting
// request reaches the real dispatch call (kiln_cfg_swap_worker_submit()).
// It does not exercise kiln_cfg_store.c's/kiln_cfg_swap.c's own real
// behaviour, which already has separate host-test coverage elsewhere.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

// Ahead of kiln_cfg_http.c's own #includes, purely for the TYPES the stub
// bodies below need (same convention test_zones_http.c/test_backup_import.c
// use for esp_err.h/esp_http_server.h).
#include "esp_err.h"
#include "esp_http_server.h"
#include "../drivers/persist/kiln_cfg_store.h"
#include "../drivers/persist/kiln_cfg_swap_worker.h"
#include "../drivers/net/ota_interlock.h"

// asm("_binary_...") is a GCC/binutils extension (EMBED_TXTFILES) with no
// MSVC equivalent -- #define it away, same convention as every other file
// that #includes a *_http.c with an embedded page (test_zones_http.c/
// test_backup_import.c's own header comments).
#define asm(x)
extern const uint8_t kiln_configs_page_html_gz_start[];
extern const uint8_t kiln_configs_page_html_gz_end[];
const uint8_t kiln_configs_page_html_gz_start[] = { 0 };
const uint8_t kiln_configs_page_html_gz_end[] = { 0 };

// ---------------------------------------------------------------------------
// wifi_provision_http.h / http_auth_http.h -- only reached from
// kiln_cfg_http_start(), which these tests never call.
// ---------------------------------------------------------------------------
httpd_handle_t wifi_provision_http_get_server(void)
{
    return NULL;
}
esp_err_t kiln_http_register(httpd_handle_t server, const httpd_uri_t *uri)
{
    (void)server;
    (void)uri;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// web_encoding.h -- only reached from kiln_configs_page_get_handler(), never
// called by these tests.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// config_divergence.h / safety_ceiling_sync.h -- only reached from
// list_get_handler(), never called by these tests.
// ---------------------------------------------------------------------------
bool safety_ceiling_sync_is_diverged(char *reason_out, size_t reason_cap)
{
    if (reason_out && reason_cap) reason_out[0] = '\0';
    return false;
}
bool safety_ceiling_sync_is_standing_diverged(char *reason_out, size_t reason_cap)
{
    if (reason_out && reason_cap) reason_out[0] = '\0';
    return false;
}

// ---------------------------------------------------------------------------
// kiln_cfg_store.h -- controllable existence/state for the tests below;
// every other entry point is trivial since no test here calls it.
// ---------------------------------------------------------------------------
static bool s_test_id_exists = true;
static char s_test_existing_name[KILN_CFG_NAME_MAX_LEN + 1] = "test-config";
bool g_stub_hardware_differs = false;

uint8_t kiln_cfg_store_max_count(void) { return 8; }
bool kiln_cfg_store_pico_half_recapture_pending(void) { return false; }
bool safety_cfg_store_has_data(void) { return true; }
/* kiln_cfg_http.c's persist-failure branch asks cfg_fs_is_available() (via
 * cfg_fs_refusal_http.h); the store is stubbed here, so cfg counts as mounted. */
bool cfg_fs_is_available(void) { return true; }
bool cfg_fs_skipped_for_recovery(void) { return false; }
uint8_t kiln_cfg_store_list(kiln_cfg_summary_t *out, uint8_t out_cap) { (void)out; (void)out_cap; return 0; }
int32_t kiln_cfg_store_get_active_id(void) { return KILN_CFG_NO_ACTIVE_ID; }
bool kiln_cfg_store_get_name(int32_t id, char *out, size_t out_cap)
{
    (void)id;
    if (!s_test_id_exists) return false;
    if (out && out_cap) {
        strncpy(out, s_test_existing_name, out_cap - 1);
        out[out_cap - 1] = '\0';
    }
    return true;
}
bool kiln_cfg_store_slot_hardware_differs(int32_t id, char *msg, size_t msg_cap)
{
    (void)id;
    if (msg && msg_cap) snprintf(msg, msg_cap, "hardware differs (stub)");
    return g_stub_hardware_differs;
}
bool kiln_cfg_store_save_current(const char *name, int32_t id_or_negative, int32_t *out_id,
                                 char *reason_out, size_t reason_cap)
{
    (void)name; (void)id_or_negative;
    if (out_id) *out_id = 1;
    if (reason_out && reason_cap) reason_out[0] = '\0';
    return true;
}
bool kiln_cfg_store_clone(int32_t src_id, const char *name, int32_t *out_id, char *reason_out,
                          size_t reason_cap)
{
    (void)src_id; (void)name;
    if (out_id) *out_id = 2;
    if (reason_out && reason_cap) reason_out[0] = '\0';
    return true;
}
bool kiln_cfg_store_apply(int32_t id, bool ack_no_safety_processor, bool ack_hardware_differs,
                          char *reason_out, size_t reason_cap)
{
    (void)id; (void)ack_no_safety_processor; (void)ack_hardware_differs;
    if (reason_out && reason_cap) reason_out[0] = '\0';
    return true;
}
bool kiln_cfg_store_delete(int32_t id, bool ack_no_safety_processor, char *reason_out, size_t reason_cap)
{
    (void)id; (void)ack_no_safety_processor;
    if (reason_out && reason_cap) reason_out[0] = '\0';
    return true;
}
bool kiln_cfg_store_rename(int32_t id, const char *name) { (void)id; (void)name; return true; }
bool kiln_cfg_store_rename_ex(int32_t id, const char *name, char *reason_out, size_t reason_cap)
{
    (void)id; (void)name;
    if (reason_out && reason_cap) reason_out[0] = '\0';
    return true;
}
bool kiln_cfg_store_reason_is_persist_failure(const char *reason)
{
    return reason != NULL && strstr(reason, KILN_CFG_PERSIST_FAIL_TEXT) != NULL;
}
bool kiln_cfg_store_name_would_collide(const char *name, int32_t exclude_id)
{
    (void)name; (void)exclude_id;
    return false;
}
bool kiln_cfg_store_is_quarantined(char *reason_out, size_t reason_cap)
{
    if (reason_out && reason_cap) reason_out[0] = '\0';
    return false;
}
bool kiln_cfg_store_quarantine_clear(bool confirm_discard, char *reason_out, size_t reason_cap)
{
    (void)confirm_discard;
    if (reason_out && reason_cap) reason_out[0] = '\0';
    return true;
}
bool kiln_cfg_store_export_package_json(int32_t id, char *out, size_t out_cap, size_t *out_len,
                                        char *reason_out, size_t reason_cap)
{
    (void)id;
    if (out && out_cap) out[0] = '\0';
    if (out_len) *out_len = 0;
    if (reason_out && reason_cap) reason_out[0] = '\0';
    return true;
}
bool kiln_cfg_store_import_package_json(const char *json, int32_t *out_id, char *reason_out,
                                        size_t reason_cap)
{
    (void)json;
    if (out_id) *out_id = 3;
    if (reason_out && reason_cap) reason_out[0] = '\0';
    return true;
}

// ---------------------------------------------------------------------------
// kiln_cfg_swap_worker.h -- the dispatch target of apply_post_handler(),
// past every refusal this file tests. Records whether it was reached at
// all -- the whole point of the ordering tests below is that it must NOT be
// reached once an earlier gate has already refused.
// ---------------------------------------------------------------------------
int g_stub_swap_submit_calls = 0;
bool kiln_cfg_swap_worker_submit(int32_t target_id, bool ack_no_safety_processor, char *reason_out,
                                 size_t reason_cap)
{
    (void)target_id; (void)ack_no_safety_processor;
    g_stub_swap_submit_calls++;
    if (reason_out && reason_cap) reason_out[0] = '\0';
    return true;
}
void kiln_cfg_swap_worker_get_status(kiln_cfg_swap_job_state_t *out_state, int32_t *out_target_id,
                                     bool *out_diverged, char *reason_out, size_t reason_cap)
{
    if (out_state) *out_state = KILN_CFG_SWAP_JOB_IDLE;
    if (out_target_id) *out_target_id = -1;
    if (out_diverged) *out_diverged = false;
    if (reason_out && reason_cap) reason_out[0] = '\0';
}

// ---------------------------------------------------------------------------
// relay_authority.h -- system_mode_gate snapshot input, same convention as
// test_zones_http.c/test_backup_import.c's own copy of this stub.
// ---------------------------------------------------------------------------
static bool s_test_profile_running = false;
static bool s_test_autotune_running = false;
void relay_authority_heat_run_active(bool *profile_running_out, bool *autotune_running_out)
{
    if (profile_running_out) *profile_running_out = s_test_profile_running;
    if (autotune_running_out) *autotune_running_out = s_test_autotune_running;
}

// ---------------------------------------------------------------------------
// ota_http.h -- the interlock, controllable and call-counted so the ordering
// tests can assert it was (or was not) reached, same convention as
// test_zones_http.c's g_probe_interlock_called.
// ---------------------------------------------------------------------------
int g_probe_interlock_called = 0;
ota_interlock_result_t g_stub_ota_interlock_result = OTA_INTERLOCK_OK;
char g_stub_ota_interlock_reason[OTA_INTERLOCK_REASON_MAX] = "";

ota_interlock_result_t ota_http_check_interlocks(bool ack_no_safety_processor, char *reason_out,
                                                 size_t reason_cap)
{
    (void)ack_no_safety_processor;
    g_probe_interlock_called++;
    if (reason_out && reason_cap) {
        strncpy(reason_out, g_stub_ota_interlock_reason, reason_cap - 1);
        reason_out[reason_cap - 1] = '\0';
    }
    return g_stub_ota_interlock_result;
}
bool ota_http_req_ack_no_safety(httpd_req_t *req)
{
    (void)req;
    return false;
}
static bool s_test_err_called = false;
static bool s_test_interlock_refusal_called = false;
esp_err_t ota_http_send_interlock_refusal(httpd_req_t *req, ota_interlock_result_t r, const char *reason)
{
    (void)req; (void)r; (void)reason;
    s_test_interlock_refusal_called = true;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// http_async_job.h -- the last refusal in the chain.
// ---------------------------------------------------------------------------
static bool s_test_async_job_busy = false;
bool http_async_job_busy(void)
{
    return s_test_async_job_busy;
}

// ---------------------------------------------------------------------------
// esp_http_server.h leaf calls -- header extraction (unused by apply, still
// linked since the whole translation unit compiles) and the plain request
// helpers.
// ---------------------------------------------------------------------------
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t val_size)
{
    (void)r; (void)field;
    if (val && val_size) val[0] = '\0';
    return ESP_FAIL; // "no such header" -- ack_hardware_differs defaults to false
}

static char s_test_post_body[256];
int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len)
{
    (void)r;
    size_t n = strlen(s_test_post_body);
    if (n > buf_len) n = buf_len;
    memcpy(buf, s_test_post_body, n);
    return (int)n;
}

static int s_last_status = 200;
static char s_last_resp_body[512];
static bool s_test_ok_called = false;

esp_err_t httpd_resp_send_err(httpd_req_t *req, httpd_err_code_t error, const char *msg)
{
    (void)req; (void)error;
    s_test_err_called = true;
    strncpy(s_last_resp_body, msg ? msg : "", sizeof(s_last_resp_body) - 1);
    s_last_resp_body[sizeof(s_last_resp_body) - 1] = '\0';
    return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status)
{
    (void)req;
    s_last_status = atoi(status);
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type)
{
    (void)req; (void)type;
    return ESP_OK;
}
esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *field, const char *value)
{
    (void)req; (void)field; (void)value;
    return ESP_OK;
}
// web_encoding.h -- only reached from the page/export GET handlers' gzip
// asset-cache header, never called by these tests.
void web_set_asset_cache_headers(httpd_req_t *req)
{
    (void)req;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *s)
{
    (void)req;
    strncpy(s_last_resp_body, s ? s : "", sizeof(s_last_resp_body) - 1);
    s_last_resp_body[sizeof(s_last_resp_body) - 1] = '\0';
    if (s_last_status == 200 || s_last_status == 202) s_test_ok_called = true;
    return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t *req, const char *buf, ssize_t len)
{
    (void)req; (void)len;
    if (buf) {
        strncpy(s_last_resp_body, buf, sizeof(s_last_resp_body) - 1);
        s_last_resp_body[sizeof(s_last_resp_body) - 1] = '\0';
    }
    if (s_last_status == 200 || s_last_status == 202) s_test_ok_called = true;
    return ESP_OK;
}

// list_get_handler()'s ?id= query parsing -- never exercised by these tests
// (apply_post_handler() doesn't use the query string), stubbed only so the
// whole translation unit links.
esp_err_t httpd_req_get_url_query_str(httpd_req_t *req, char *buf, size_t buf_len)
{
    (void)req;
    if (buf && buf_len) buf[0] = '\0';
    return ESP_ERR_NOT_FOUND;
}
esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t val_size)
{
    (void)qry; (void)key;
    if (val && val_size) val[0] = '\0';
    return ESP_ERR_NOT_FOUND;
}

#include "../drivers/http/kiln_cfg_http.c"

#undef asm

// ---------------------------------------------------------------------------
// Test harness
// ---------------------------------------------------------------------------

static void test_reset(void)
{
    s_test_id_exists = true;
    strncpy(s_test_existing_name, "test-config", sizeof(s_test_existing_name) - 1);
    g_stub_hardware_differs = false;
    g_stub_swap_submit_calls = 0;
    s_test_profile_running = false;
    s_test_autotune_running = false;
    g_probe_interlock_called = 0;
    g_stub_ota_interlock_result = OTA_INTERLOCK_OK;
    g_stub_ota_interlock_reason[0] = '\0';
    s_test_interlock_refusal_called = false;
    s_test_async_job_busy = false;
    s_last_status = 200;
    s_last_resp_body[0] = '\0';
    s_test_err_called = false;
    s_test_ok_called = false;
}

static void run_apply(int32_t id)
{
    snprintf(s_test_post_body, sizeof(s_test_post_body), "id=%ld", (long)id);
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.content_len = (long long)strlen(s_test_post_body);
    esp_err_t err = apply_post_handler(&req);
    TEST_CHECK(err == ESP_OK, "apply_post_handler must always return ESP_OK");
}

static void test_apply_nonexistent_id_refused_before_mode_gate(void)
{
    TEST_SECTION("apply_post_handler -- a nonexistent id is refused 404 before the mode gate/interlock "
                 "are ever consulted, even while a firing is active");
    test_reset();
    s_test_id_exists = false;
    s_test_profile_running = true; // must not matter: 404 wins first
    run_apply(999);
    TEST_CHECK(s_test_err_called, "must go through httpd_resp_send_err() for the 404");
    TEST_CHECK(!s_test_ok_called, "must not report success");
    TEST_CHECK(g_probe_interlock_called == 0, "the OTA interlock must never be reached for a nonexistent id");
    TEST_CHECK(g_stub_swap_submit_calls == 0, "the swap worker must never be reached for a nonexistent id");
}

static void test_apply_refused_by_mode_gate_before_interlock(void)
{
    TEST_SECTION("apply_post_handler -- an existing id, while a firing is RUNNING, is refused by the "
                 "system mode gate BEFORE ota_http_check_interlocks() is ever called (plan slice-4 order)");
    test_reset();
    s_test_profile_running = true;
    run_apply(1);
    TEST_CHECK(g_probe_interlock_called == 0,
              "ota_http_check_interlocks() must never be reached once the mode gate has already refused");
    TEST_CHECK(g_stub_swap_submit_calls == 0, "the swap worker must never be reached");
    TEST_CHECK(!s_test_err_called, "the mode gate's refusal goes through system_mode_gate_http_send_refusal(), "
              "not httpd_resp_send_err()");
    TEST_CHECK(!s_test_ok_called, "must not report success");
    TEST_CHECK(s_last_status == 409, "system_mode_gate refusals are always 409 (plan section 5, decision 4)");
}

static void test_apply_refused_by_mode_gate_autotune_running(void)
{
    TEST_SECTION("apply_post_handler -- same refusal while autotune (not a profile) is running");
    test_reset();
    s_test_autotune_running = true;
    run_apply(1);
    TEST_CHECK(g_probe_interlock_called == 0, "the interlock must not be reached");
    TEST_CHECK(g_stub_swap_submit_calls == 0, "the swap worker must not be reached");
    TEST_CHECK(s_last_status == 409, "must be refused 409");
}

static void test_apply_idle_passes_gate_then_hits_interlock(void)
{
    TEST_SECTION("apply_post_handler -- idle (no firing/autotune) reaches the OTA interlock next");
    test_reset();
    g_stub_ota_interlock_result = OTA_INTERLOCK_REFUSED;
    strncpy(g_stub_ota_interlock_reason, "a profile is running", sizeof(g_stub_ota_interlock_reason) - 1);
    run_apply(1);
    TEST_CHECK(g_probe_interlock_called == 1, "idle must reach the interlock check");
    TEST_CHECK(s_test_interlock_refusal_called, "an interlock refusal must be sent via "
              "ota_http_send_interlock_refusal()");
    TEST_CHECK(g_stub_swap_submit_calls == 0, "the swap worker must not be reached when the interlock refuses");
}

static void test_apply_refused_while_async_job_busy(void)
{
    TEST_SECTION("apply_post_handler -- idle, interlock OK, but an http_async_job is running: refused 409, "
                 "swap worker never reached (interlock IS reached first, per plan slice-4 order)");
    test_reset();
    s_test_async_job_busy = true;
    run_apply(1);
    TEST_CHECK(g_probe_interlock_called == 1, "the interlock runs before the busy check, so it IS reached");
    TEST_CHECK(g_stub_swap_submit_calls == 0, "the swap worker must never be reached while another "
              "commissioning operation owns http_async_job");
    TEST_CHECK(s_last_status == 409, "the busy refusal is 409");
}

static void test_apply_idle_and_clear_reaches_swap_worker(void)
{
    TEST_SECTION("apply_post_handler -- idle, interlock OK, not busy: reaches the real dispatch "
                 "(positive control -- proves the ordering tests above are refusing for the right reason, "
                 "not because the handler never gets anywhere)");
    test_reset();
    run_apply(1);
    TEST_CHECK(g_probe_interlock_called == 1, "the interlock is checked");
    TEST_CHECK(g_stub_swap_submit_calls == 1, "an unblocked apply must reach kiln_cfg_swap_worker_submit() exactly once");
    TEST_CHECK(s_last_status == 202, "an accepted apply reports 202 Accepted");
}

// HTTP fuzz campaign part 2: hostile apply bodies never reach the interlock or the swap worker.
static void test_apply_hostile_bodies_never_reach_swap(void)
{
    TEST_SECTION("apply_post_handler fuzz -- malformed id bodies are 4xx and never reach the swap worker");
    static const char *const bodies[] = {
        "", "id", "id=", "id=abc", "id=1x", "id=-1", "id=%00", "id=1%002", "id=%zz",
        "id=0x1", "id=1.5", "id=+", "foo=1", "&&&", "id=1e2", "id=--1",
        "id=+1", "id=%2B1", "id=%201", "id=99999999999", "id=2147483648",
    };
    for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); i++) {
        test_reset();
        s_test_id_exists = true;
        snprintf(s_test_post_body, sizeof(s_test_post_body), "%s", bodies[i]);
        httpd_req_t req;
        memset(&req, 0, sizeof(req));
        req.content_len = (long long)strlen(bodies[i]);
        (void)apply_post_handler(&req);
        char msg[160];
        snprintf(msg, sizeof(msg), "body '%s' refused", bodies[i]);
        TEST_CHECK(s_test_err_called && !s_test_ok_called, msg);
        snprintf(msg, sizeof(msg), "body '%s': swap worker never reached", bodies[i]);
        TEST_CHECK(g_stub_swap_submit_calls == 0 && g_probe_interlock_called == 0, msg);
    }
    /* oversized Content-Length (past the 256 B scratch) refused without a read */
    test_reset();
    s_test_id_exists = true;
    snprintf(s_test_post_body, sizeof(s_test_post_body), "id=1");
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.content_len = 100000;
    (void)apply_post_handler(&req);
    TEST_CHECK(s_test_err_called && g_stub_swap_submit_calls == 0, "100000-byte Content-Length refused");
}

int main(void)
{
    test_apply_hostile_bodies_never_reach_swap();
    test_apply_nonexistent_id_refused_before_mode_gate();
    test_apply_refused_by_mode_gate_before_interlock();
    test_apply_refused_by_mode_gate_autotune_running();
    test_apply_idle_passes_gate_then_hits_interlock();
    test_apply_refused_while_async_job_busy();
    test_apply_idle_and_clear_reaches_swap_worker();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures == 0 ? 0 : 1;
}
