#include "ota_http.h"
#include "http_auth_http.h" // kiln_http_register() -- WEB_AUTH_PLAN.md section 5
#include "ota_http_internal.h"
#include "ota_http_util.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "build_info.h" /* FW_GIT_COMMIT/FW_GIT_DIRTY/FW_BUILD_DATE/FW_BUILD_TIME -- TODO.md 9.6's
                          * per-processor build-identity fields for the ESP side, same header
                          * safety_link.c already includes for the ANNOUNCE_VERSION payload */
#include "esp_app_desc.h"
#include "esp_app_format.h" /* esp_image_header_t, ESP_IMAGE_HEADER_MAGIC -- section 3's pre-esp_ota_begin() check */
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_rom_crc.h" /* esp_rom_crc32_le() -- section 4's Pico-image running CRC32, see ota_pico_do_stage() */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <math.h>

#include "autotune_engine.h"
#include "boot_guard.h"
#include "kilnlink/kilnlink_rollback_result.h" /* KILNLINK_ROLLBACK_RESULT_REASON_* -- ota_pico_rollback_post_handler()'s response mapping */
#include "kiln_io.h"
#include "kiln_io_owner.h" /* kiln_io_owner_command_read() -- see the pre-OTA interlock snapshot below */
#include "MAX31856.h"
#include "ota_pico_relay.h"
#include "ota_record.h"
#include "profile_executor.h" /* PROFILE_EXEC_* enum only, not its live state -- see below */
#include "run_state.h"
#include "stack_margin.h"
#include "thermo_owner.h" /* thermo_owner_command_read_all() -- see the pre-OTA interlock snapshot below */
#include "web_encoding.h"
#include "sim_backend.h"
#include "wifi_prov.h"
#include "wifi_provision_http.h"
#include "zones_config_accessors.h"

const char *OTA_HTTP_TAG = "ota_http";

// GET /ota page (TODO.md 9.6) -- gzipped at configure time by
// App/drivers/CMakeLists.txt's KILNCTL_GZIP_ASSETS list, same
// EMBED_TXTFILES + Content-Encoding: gzip convention every other page in
// this component uses (rules_page.html/profiles_page.html/etc. -- see
// rules_http.c's page_get_handler()/client_accepts_gzip() for the precedent
// this mirrors).
extern const uint8_t ota_page_html_gz_start[] asm("_binary_ota_page_html_gz_start");
extern const uint8_t ota_page_html_gz_end[] asm("_binary_ota_page_html_gz_end");

// AP-password HMAC (nonce, per-context lockout, and the KDF context string
// that used to live here) was retired 2026-09-29 -- WEB_AUTH_PLAN.md item
// 2b, owner decision "Retire; open when login off": these routes' ADMIN
// tier (route_tier_table.h) is now the only gate, on or off, same as every
// other ADMIN route. s_ota_lock is kept -- it also guards the update-claim
// mutex below, which is unrelated to authentication.
static SemaphoreHandle_t s_ota_lock;

// opus-review finding 3: safety_link_send_rollback_ex() blocks its caller
// for up to ~6.3s (4 sends * 250ms + one reply window + the 5s boot_id
// watch). esp_http_server here runs with exactly ONE worker task
// (wifi_provision_http.c's own config), so a handler that blocks inside it
// for that long queues up EVERY other request behind it -- including the
// dashboard's ~1Hz /api/status poll and this very OTA page's own UI, both
// of which have client-side timeouts well under 6.3s (wifi_provision_http.c
// lines 695-735 document a previously live-tested wedge in exactly this
// area). Rather than shorten the watch (which trades a wedged HTTP worker
// for reporting UNKNOWN_TIMEOUT on a rollback that was still genuinely in
// flight -- see safety_link.h's SAFETY_LINK_ROLLBACK_BOOT_WATCH_MS comment
// for why 5s is already a rough estimate, not a measured floor), the whole
// safety_link_send_rollback_ex() call now runs on its own short-lived task,
// same "task owns the mutex release, handler returns promptly" shape
// ota_pico_do_stage()/the Pico relay task already use for the plain Pico
// update path just above. ota_pico_rollback_post_handler() below starts the
// task and returns a 202 immediately; the OTA page polls GET /api/ota/pico/
// rollback/status (ota_pico_rollback_status_get_handler()) for the outcome,
// the same poll-for-progress shape /api/ota/pico/status already establishes
// for the plain update.
//
// ota_pico_rollback_async_state_t/ota_pico_rollback_async_t moved to
// ota_http_internal.h -- ota_http_pico.c (the reader/writer of the state
// below) needs the type too.

// Guarded by its own small mutex, deliberately separate from s_ota_lock
// (nonce/lockout bookkeeping) and from the update-claim mechanism
// (ota_http_update_try_begin()/_end(), a different file's own mutex) --
// this state is written by a background task while the handler that started
// it may already have returned and moved on to a different request, so it
// cannot share either of those locks' lifetimes.
SemaphoreHandle_t ota_http_pico_rollback_async_lock;
ota_pico_rollback_async_t ota_http_pico_rollback_async;

// The hardware pointers main.c hands to ota_http_start(), same pattern (and
// same NULL-tolerant meaning) as dashboard_http.c's s_dash struct. Read-only
// after ota_http_start(), so no lock needed to read them.
static kiln_io_t *s_io;
static MAX31856BusClass *s_thermo_bus;
SafetyLinkClass *ota_http_safety;

// --- Single cross-processor update mutex (ota_http.h) ---------------------
// Guarded by s_ota_lock, same as the nonce/lockout state above -- this is
// genuinely mutated from whatever future task handles a POST
// /api/ota/{esp,pico} upload, so it needs the same discipline as everything
// else in this file that more than one httpd worker could touch at once.
typedef enum {
    OTA_UPDATE_NONE = 0,
    OTA_UPDATE_ESP,
    OTA_UPDATE_PICO,
} ota_update_claim_t;

static ota_update_claim_t s_update_claim = OTA_UPDATE_NONE;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

// Client IP for the "log every attempt with the source IP" requirement
// (UPDATE_PROTOCOL.md section 2). Standard ESP-IDF httpd pattern: the
// underlying socket is IPv4-mapped-into-IPv6 by lwip regardless of which
// family the client actually connected over, so this handles both without
// the caller needing to know which.
//
// 2026-09-17 adversarial review of the Finding-1 fix (WEB_AUTH_CLIENT_IP_LEN
// 16 -> 46) found two PRE-EXISTING weaknesses here, unrelated to that fix's
// own buffer-size bug but with a larger blast radius now that the same
// buffer feeds two more comparison sites (http_auth_session_status()/
// _touch(), http_auth_http.c):
//
//   1. inet_ntop()'s return value went unchecked. lwip's inet_ntop(), like
//      BSD's, does not touch its output buffer on failure -- so a failure
//      here used to leave `out` holding whatever was already on the
//      caller's stack (every real caller declares a plain `char ip[46];`
//      local, never zero-initialized), and that undefined content then fed
//      a security strcmp() against a stored session IP. Fixed: the actual
//      inet_ntop() call/return-check now happens here, and the decision of
//      what to leave in `out` on any failure is delegated to
//      ota_http_client_ip_finalize() (ota_http_util.h/.c), which always
//      leaves `out` NUL-terminated -- host-tested there without needing a
//      real socket (see test_ota_http.c's ota_http_client_ip_finalize
//      cases).
//   2. Every distinct failure path (this function's own two, plus a bad
//      inet_ntop()) collapses onto the shared literal "unknown" -- two
//      different clients that both hit any of these paths present the
//      IDENTICAL client_ip string, so one could resolve a session minted
//      for the other. This is real but deliberately left UNCHANGED here,
//      for two reasons: `web_auth_session.h` already documents "unknown" as
//      a deliberate, load-bearing pre-existing property (its
//      WEB_AUTH_CLIENT_IP_LEN comment), and the only call site where the
//      collision is actually exploitable -- web_auth_login_http.c's
//      login_post_handler(), the sole place a session is MINTED rather than
//      merely compared against -- was out of scope for this pass (another
//      session was concurrently editing that file). Every OTHER caller of
//      this function (http_auth_http.c, ota_http_esp.c/_pico.c/_recovery.c,
//      web_auth_session_status_http.c) only ever COMPARES `out` against an
//      already-existing session's stored client_ip, so leaving "unknown" as
//      the shared miss/undetermined value there is unchanged and still
//      correct. CLOSED 2026-09-18: login_post_handler() now refuses to mint
//      a session (fail closed) when ota_http_get_client_ip_checked() below
//      reports the address could not be determined, rather than binding a
//      real, credential-verified session to a sentinel other undetermined
//      clients also share -- see that function's own comment and
//      web_auth_login_http.c's login_post_handler().
// 2026-09-18 follow-up to the review above: the one MINTING call site
// (web_auth_login_http.c's login_post_handler()) needs the actual
// true/false outcome ota_http_client_ip_finalize() already computes
// internally, not just the (possibly-"unknown") string it wrote -- a
// caller comparing that string against the literal "unknown" would be a
// second, fragile copy of the same sentinel contract this file's own
// review comment above warns about. This checked variant is now the real
// implementation; ota_http_get_client_ip() below is a thin wrapper over it
// that keeps every existing compare-only caller's void signature (and
// every one of their test doubles) unchanged.
bool ota_http_get_client_ip_checked(httpd_req_t *req, char *out, size_t out_len)
{
    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd < 0) {
        return ota_http_client_ip_finalize(out, out_len, NULL);
    }

    struct sockaddr_in6 addr;
    socklen_t addr_size = sizeof(addr);
    if (getpeername(sockfd, (struct sockaddr *)&addr, &addr_size) != 0) {
        return ota_http_client_ip_finalize(out, out_len, NULL);
    }

    char formatted[46]; // INET6_ADDRSTRLEN -- same size as every caller's own buffer
    const char *result;
    if (addr.sin6_family == AF_INET) {
        struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr;
        result = inet_ntop(AF_INET, &addr4->sin_addr, formatted, sizeof(formatted));
    } else {
        result = inet_ntop(AF_INET6, &addr.sin6_addr, formatted, sizeof(formatted));
    }
    return ota_http_client_ip_finalize(out, out_len, result);
}

void ota_http_get_client_ip(httpd_req_t *req, char *out, size_t out_len)
{
    (void)ota_http_get_client_ip_checked(req, out, out_len);
}

// TODO.md 10.6a: content negotiation lives in web_encoding.h's shared
// web_client_accepts_gzip() (the per-file duplicated helpers were folded
// into it) -- absent Accept-Encoding is legal and served gzip per RFC 9110
// s12.5.3; a header that explicitly excludes gzip gets an uncompressed 406.
static esp_err_t ota_page_get_handler(httpd_req_t *req)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, OTA_HTTP_TAG, "ota_page.html");
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)ota_page_html_gz_start,
                            (size_t)(ota_page_html_gz_end - ota_page_html_gz_start));
}

/* snprintf() returns the length it WOULD have written, not what it did. Passing
 * that straight to httpd_resp_send() as the body length means that the moment a
 * response truncates, the server reads past the end of the local buffer and
 * sends whatever is next on the stack to the client. Every JSON responder in
 * this file used to do exactly that (TODO.md: "fifteen sites use snprintf's
 * return unclamped at the high end"), and several of them interpolate strings
 * that are not this module's own -- a Pico relay's last_error, an image's
 * version -- so the length is not always something a reader can bound by
 * inspection.
 *
 * Truncating is the right answer rather than 500ing: these are status readouts,
 * a short one is still useful, and the caller has already decided the operation
 * succeeded. The log line is there so an undersized buffer surfaces rather than
 * silently shipping half a document forever. */
esp_err_t ota_http_send_json_clamped(httpd_req_t *req, const char *buf, int n, size_t cap)
{
    if (n < 0) {
        ESP_LOGE(OTA_HTTP_TAG, "response formatting failed");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "response formatting failed");
        return ESP_OK;
    }
    size_t len = (size_t)n;
    if (len >= cap) {
        ESP_LOGE(OTA_HTTP_TAG, "response did not fit in %u bytes -- truncated, raise the buffer", (unsigned)cap);
        len = cap - 1;
    }
    return httpd_resp_send(req, buf, len);
}

// GET /api/ota/challenge and ota_http_verify_request() (the AP-password
// HMAC challenge/nonce/lockout dance) were removed here 2026-09-29 -- see
// the s_ota_lock comment above. The context-string switch that used to live
// here is gone with them; ota_http_context_t itself (ota_state.h) is still
// used by the update-claim mutex below.

bool ota_http_update_try_begin(ota_http_context_t ctx)
{
    ota_update_claim_t want = (ctx == OTA_HTTP_CONTEXT_ESP) ? OTA_UPDATE_ESP : OTA_UPDATE_PICO;

    if (xSemaphoreTake(s_ota_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        // Cannot safely check/mutate the claim -- refuse rather than risk
        // two callers both believing they won it.
        ESP_LOGW(OTA_HTTP_TAG, "OTA update claim(%s): internal lock timeout, refused",
                 ctx == OTA_HTTP_CONTEXT_ESP ? "esp" : "pico");
        return false;
    }

    bool won = (s_update_claim == OTA_UPDATE_NONE);
    if (won) {
        s_update_claim = want;
    }
    xSemaphoreGive(s_ota_lock);

    if (won) {
        ESP_LOGI(OTA_HTTP_TAG, "OTA update claim(%s): acquired", ctx == OTA_HTTP_CONTEXT_ESP ? "esp" : "pico");
    } else {
        ESP_LOGW(OTA_HTTP_TAG, "OTA update claim(%s): refused, an update is already in progress",
                 ctx == OTA_HTTP_CONTEXT_ESP ? "esp" : "pico");
    }
    return won;
}

void ota_http_update_end(void)
{
    if (xSemaphoreTake(s_ota_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_update_claim != OTA_UPDATE_NONE) {
            ESP_LOGI(OTA_HTTP_TAG, "OTA update claim released");
        }
        s_update_claim = OTA_UPDATE_NONE;
        xSemaphoreGive(s_ota_lock);
    } else {
        // Defensive: could not take the lock to release. Logged loudly
        // because an unreleased claim permanently refuses every future
        // update until reboot -- this must never happen silently.
        ESP_LOGE(OTA_HTTP_TAG, "OTA update claim release: lock timeout -- claim may remain held");
    }
}

bool ota_http_update_in_progress(ota_http_context_t *out_ctx)
{
    bool in_progress = false;
    ota_update_claim_t claim = OTA_UPDATE_NONE;

    // s_ota_lock does not exist until ota_http_start() runs. Any caller that
    // asks before then must get a fail-safe answer rather than a crash:
    // xSemaphoreTake() on a NULL handle asserts inside FreeRTOS and panics
    // the whole system. This is not hypothetical -- rules_task called this
    // from its 1 Hz fail-safe gate while ota_http_start() was still ~120
    // lines away in app_main(), and the board boot-looped with
    // "assert_func ... xQueueSemaphoreTake" on every single boot.
    //
    // "In progress" is the safe answer here, not "idle": every caller uses
    // this to decide whether it is safe to start heating or start another
    // update, and before the OTA subsystem is even up, refusing both is
    // correct. The start-order fix in main.c is the real remedy; this guard
    // exists so a future caller that runs early degrades to "refuse" instead
    // of taking the board down.
    if (s_ota_lock == NULL) {
        ESP_LOGW(OTA_HTTP_TAG, "OTA update claim query before ota_http_start() -- reporting in-progress (fail-safe)");
        return true;
    }

    if (xSemaphoreTake(s_ota_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        claim = s_update_claim;
        xSemaphoreGive(s_ota_lock);
    } else {
        // Cannot confirm the claim is free -- treat contention as "in
        // progress" rather than risk telling a caller it's safe to start a
        // second update when we simply couldn't check.
        ESP_LOGW(OTA_HTTP_TAG, "OTA update claim query: internal lock timeout, reporting in-progress");
        return true;
    }

    in_progress = (claim != OTA_UPDATE_NONE);
    if (in_progress && out_ctx) {
        *out_ctx = (claim == OTA_UPDATE_ESP) ? OTA_HTTP_CONTEXT_ESP : OTA_HTTP_CONTEXT_PICO;
    }
    return in_progress;
}

/* Per-request acknowledgement token for the one overridable interlock
 * precondition (ota_interlock.h's OTA_INTERLOCK_REFUSED_NEEDS_ACK). A
 * header rather than a query parameter so it survives the binary-body
 * upload routes unchanged, and so it never lands in a server access log or
 * a browser history entry the way a URL would.
 *
 * NOT itself an auth check. That is a deliberate, bounded choice: every
 * route that reads this already refuses an unauthenticated caller outright
 * (route_tier_table.h's ADMIN tier), so nobody without an admin session can
 * set this header on a request that gets this far. What the
 * header relaxes is a local operator-policy check ("is a supervisor
 * watching?"), never an authentication or authorisation decision, and it
 * cannot turn a hard refusal (a running profile, a hot zone) into a pass.
 */
#define OTA_ACK_NO_SAFETY_HEADER "X-Ota-Ack-No-Safety"

bool ota_http_req_ack_no_safety(httpd_req_t *req)
{
    char val[8];
    if (httpd_req_get_hdr_value_str(req, OTA_ACK_NO_SAFETY_HEADER, val, sizeof(val)) != ESP_OK) {
        return false;
    }
    return val[0] == '1';
}

/* Emits the right refusal for an interlock result, keeping the status code
 * meaningful to the page: 428 Precondition Required means "there is a
 * precondition you can satisfy by acknowledging it, ask the operator and
 * retry with the header"; 409 Conflict keeps its old meaning of "the kiln
 * is busy or hot, there is nothing to acknowledge." A client that does not
 * know about 428 still sees a 4xx and still refuses, which is the safe
 * default. */
esp_err_t ota_http_send_interlock_refusal(httpd_req_t *req, ota_interlock_result_t r,
                                          const char *reason)
{
    if (r == OTA_INTERLOCK_REFUSED_NEEDS_ACK) {
        httpd_resp_set_status(req, "428 Precondition Required");
    } else {
        httpd_resp_set_status(req, "409 Conflict");
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, reason, HTTPD_RESP_USE_STRLEN);
}

ota_interlock_result_t ota_http_check_interlocks(bool ack_no_safety_processor, char *reason_out,
                                                 size_t reason_cap)
{
    /* B2 (opus review, 2026-08-27): a zone current sweep (zones_http.c) is
     * a relay writer too, and mid-update is one of the worse times for a
     * relay to be raced -- checked here, before the snapshot below, same
     * "cheapest and orthogonal to kiln state" reasoning
     * ota_interlock_check() itself documents for its own mutex check. Kept
     * as a standalone early return rather than a new ota_interlock_snapshot_t
     * field so ota_interlock.c -- the pure, host-tested half of this check,
     * with its own precondition-ordering doc comment and test coverage --
     * stays untouched; zones_http.h/.c are the only files this pass is
     * authorized to change. */
    if (zones_current_sweep_is_active()) {
        if (reason_out && reason_cap > 0) {
            snprintf(reason_out, reason_cap, "a zone current sweep is running");
        }
        return OTA_INTERLOCK_REFUSED;
    }

    ota_interlock_snapshot_t snap = { 0 };
    snap.operator_ack_no_safety_processor = ack_no_safety_processor;

    // Profile executor state -- one-for-one map onto ota_interlock.h's own
    // enum (see that header's comment on why it can't just reuse
    // profile_exec_state_t directly: this file CAN include profile_executor.h,
    // but ota_interlock.c must stay host-buildable and cannot). Only the
    // .state field is used here -- see the per-zone loop below for why
    // pstat.zones[] itself is the WRONG source for temperature/heater-
    // commanded data.
    /* Heap, not a stack local: this runs on the httpd task (8192-byte
     * stack, wifi_provision_http.c), and profile_exec_status_t is 1464
     * bytes -- same reasoning and pattern as safety_cfg_http.c's reads.
     * Freed right after the switch below; nothing past this point needs
     * more than the enum it already copied out. */
    profile_exec_status_t *pstat = heap_caps_malloc(sizeof(*pstat), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!pstat) {
        if (reason_out && reason_cap > 0) {
            snprintf(reason_out, reason_cap, "out of memory checking profile state");
        }
        return OTA_INTERLOCK_REFUSED;
    }
    profile_executor_get_status(pstat);
    switch (pstat->state) {
        case PROFILE_EXEC_RUNNING: snap.profile_state = OTA_INTERLOCK_PROFILE_RUNNING; break;
        case PROFILE_EXEC_PAUSED:  snap.profile_state = OTA_INTERLOCK_PROFILE_PAUSED; break;
        case PROFILE_EXEC_DONE:    snap.profile_state = OTA_INTERLOCK_PROFILE_DONE; break;
        case PROFILE_EXEC_FAULTED: snap.profile_state = OTA_INTERLOCK_PROFILE_FAULTED; break;
        case PROFILE_EXEC_IDLE:
        default:                   snap.profile_state = OTA_INTERLOCK_PROFILE_IDLE; break;
    }
    free(pstat);

    snap.autotune_active = autotune_engine_is_active();

    // Safety link: NULL (no link this boot) is treated as down, same
    // fail-safe default as every other consumer of this pointer.
    if (ota_http_safety) {
        safety_link_status_t link_status;
        snap.safety_link_up = (safety_link_get_status(ota_http_safety, &link_status) == ESP_OK)
                                   ? link_status.link_up
                                   : false;
    } else {
        snap.safety_link_up = false;
    }

    // run_state's boot-record breadcrumb -- catches a firing that survived a
    // reboot without a clean ending, which profile_state alone (always IDLE
    // on a fresh boot) cannot see. Purely informational per run_state.h's
    // own contract, which is exactly the read-only use this is.
    snap.run_state_interrupted = run_state_boot_record_interrupted();

    snap.other_update_in_progress = ota_http_update_in_progress(NULL);

    snap.temp_ceiling_c = OTA_INTERLOCK_TEMP_CEILING_C;

    // Per-zone temperature and relay-commanded state, read the SAME way
    // dashboard_http.c's /api/status does -- directly from the thermo bus
    // and kiln_io, NOT from profile_executor_get_status()'s zones[] array.
    // profile_exec_zone_status_t.active only means "this zone participated
    // in the last/current RUN" (see profile_executor.h's own doc comment);
    // when the executor is IDLE (the exact case this interlock exists to
    // catch -- "a cooling kiln is still a hot kiln" even with nothing
    // running), every zones[i].active there is false and this loop would
    // silently check NOTHING. Reading zones_config_get_thermo_count()/
    // thermo_owner_command_read_all()/kiln_io_owner_command_read() instead
    // means the ceiling and heater-commanded checks below see the kiln's actual current state
    // regardless of whether a profile happens to be running.
    ota_interlock_zone_snapshot_t zones[MAX31856_CHANNEL_COUNT];
    memset(zones, 0, sizeof(zones));

    uint8_t thermo_count = zones_config_get_thermo_count();
    if (thermo_count > MAX31856_CHANNEL_COUNT) {
        thermo_count = MAX31856_CHANNEL_COUNT; /* defensive; zones_http.c already validates this */
    }

    MAX31856Reading readings[MAX31856_CHANNEL_COUNT];
    size_t reading_count = 0;
    if (sim_backend_enabled()) {
        sim_backend_read_all(readings, MAX31856_CHANNEL_COUNT, &reading_count);
    } else if (s_thermo_bus && s_thermo_bus->initialized) {
        thermo_owner_command_read_all(readings, MAX31856_CHANNEL_COUNT, &reading_count);
    }

    kiln_io_state_t io_state;
    memset(&io_state, 0, sizeof(io_state));
    bool io_read_ok = false;
    if (s_io) {
        io_read_ok = (kiln_io_owner_command_read(&io_state) == ESP_OK);
    }

    for (uint8_t i = 0; i < thermo_count; i++) {
        zones[i].active = true;

        // Find channel i's reading, if it answered this poll -- readings[]
        // is not guaranteed to be dense/in-order once a channel is absent,
        // same reasoning as dashboard_http.c's own loop.
        const MAX31856Reading *r = NULL;
        for (size_t j = 0; j < reading_count; j++) {
            if (readings[j].channel == i) {
                r = &readings[j];
                break;
            }
        }
        if (r && !r->spi_failed && !isnan(r->tc_temperature_c)) {
            zones[i].actual_valid = true;
            zones[i].actual_c = zones_config_apply_cal(i, r->tc_temperature_c);
        } else {
            zones[i].actual_valid = false;
        }

        // heater_commanded: any relay this zone owns is currently on. NOT
        // io_read_ok (no io this boot / read failed) is treated as
        // "commanded" too -- an unreadable relay state cannot be assumed
        // off, same "no valid data -> refuse" rule the temperature check
        // above follows.
        if (!io_read_ok) {
            zones[i].heater_commanded = true;
            continue;
        }
        uint8_t relay_mask = 0;
        if (zones_config_get_relay_mask(i, &relay_mask)) {
            zones[i].heater_commanded = (io_state.relay_shadow & relay_mask) != 0;
        } else {
            zones[i].heater_commanded = false; /* zone has no relays assigned -- nothing to command */
        }
    }

    return ota_interlock_check(&snap, zones, thermo_count, reason_out, reason_cap);
}

// True when ANY relay (all four, not just the ones a configured zone owns)
// is commanded on, or when the relay state cannot be read -- an unreadable
// relay is never assumed off, same "no valid data -> refuse" rule the
// per-zone heater_commanded read above follows. Used by POST
// /api/ota/esp/recovery_boot, whose target image has no relay driver at all.
bool ota_http_any_relay_energized(void)
{
    kiln_io_state_t io_state;
    memset(&io_state, 0, sizeof(io_state));
    if (!s_io || kiln_io_owner_command_read(&io_state) != ESP_OK) {
        return true;
    }
    return (io_state.relay_shadow & 0x0F) != 0;
}

// See ota_http.h's doc comment above this function. The mirror-image glue
// to ota_http_check_interlocks() above: same s_update_claim mutex, opposite
// direction ("may heat proceed" instead of "may an update start").
static bool (*s_fetch_busy_probe)(void);

void ota_http_set_fetch_busy_probe(bool (*probe)(void))
{
    s_fetch_busy_probe = probe;
}

bool ota_http_heat_blocked_by_update(char *reason_out, size_t reason_cap)
{
    heat_interlock_snapshot_t snap = { 0 };
    ota_http_context_t ctx;

    snap.fetch_busy = (s_fetch_busy_probe != NULL) && s_fetch_busy_probe();

    snap.update_in_progress = ota_http_update_in_progress(&ctx);
    if (snap.update_in_progress) {
        // Explicit per-value mapping, not a two-way ternary -- a ternary
        // testing only "== OTA_HTTP_CONTEXT_PICO" happened to be correct
        // today only because ota_http_update_try_begin() is, in fact, never
        // called with anything but OTA_HTTP_CONTEXT_ESP or _PICO (every
        // other ota_http_context_t member -- ESP_ROLLBACK, RECOVERY_EXIT,
        // FACTORY_RESET, PICO_ROLLBACK -- is an auth context that claims the
        // mutex AS _ESP or _PICO, per each handler's own doc comment; see
        // ota_pico_rollback_post_handler() above claiming OTA_HTTP_CONTEXT_
        // PICO, not _PICO_ROLLBACK). That is an invariant of the CALL SITES,
        // not of this enum, so a future context that claims the mutex under
        // its own value would silently fall through to the ternary's "else"
        // (ESP) with no compiler warning. A switch makes every currently-
        // reachable value's mapping explicit and gives a single place to
        // extend if that invariant ever changes.
        switch (ctx) {
            case OTA_HTTP_CONTEXT_PICO:
                snap.update_context = HEAT_INTERLOCK_UPDATE_PICO;
                break;
            // opus-review finding 4 (latent nit): OTA_HTTP_CONTEXT_PICO_
            // ROLLBACK used to sit in the same case-group as the ESP-mapped
            // values below, correct only because ota_pico_rollback_post_
            // handler() claims the mutex AS OTA_HTTP_CONTEXT_PICO (see the
            // comment above this switch), never as _PICO_ROLLBACK -- so this
            // branch has never actually been reached for it. If a future
            // handler ever DID claim the mutex under its own _PICO_ROLLBACK
            // value, grouping it with ESP would misreport a Pico-side action
            // to the heat interlock as an ESP one. Given its own explicit
            // case rather than left to fall into the ESP group: it names the
            // processor it actually rolls back (the Pico), which is also the
            // semantically correct mapping even in the unreachable-today
            // case, not just the loudest one.
            case OTA_HTTP_CONTEXT_PICO_ROLLBACK:
                snap.update_context = HEAT_INTERLOCK_UPDATE_PICO;
                break;
            case OTA_HTTP_CONTEXT_ESP:
            case OTA_HTTP_CONTEXT_ESP_ROLLBACK:
            case OTA_HTTP_CONTEXT_RECOVERY_EXIT:
            case OTA_HTTP_CONTEXT_FACTORY_RESET:
            default:
                // ESP_ROLLBACK/RECOVERY_EXIT/FACTORY_RESET are listed
                // explicitly even though the claim mutex never actually
                // holds these values (see the comment above) -- collapsing
                // onto ESP here matches heat_interlock.h's own doc comment
                // on heat_interlock_update_context_t (only ESP/PICO exist on
                // that side; ESP_ROLLBACK collapses onto ESP).
                snap.update_context = HEAT_INTERLOCK_UPDATE_ESP;
                break;
        }
    }

    return heat_interlock_check(&snap, reason_out, reason_cap) != HEAT_INTERLOCK_OK;
}

// --- POST /api/ota/esp (TODO.md 9.5, ota_http.h's doc comment) ------------


esp_err_t ota_http_refusal_drain(httpd_req_t *req, uint8_t *buf, size_t cap)
{
    uint32_t start = now_ms();
    for (;;) {
        // httpd_req_recv() returns 0 once remaining_len is 0, negative on a
        // socket error or receive timeout.
        int ret = httpd_req_recv(req, (char *)buf, cap);
        ota_http_drain_verdict_t v = ota_http_drain_verdict(ret, now_ms() - start, OTA_REFUSAL_DRAIN_CAP_MS);
        if (v == OTA_DRAIN_DONE) {
            return ESP_OK;
        }
        if (v == OTA_DRAIN_FAIL) {
            return ESP_FAIL;
        }
        vTaskDelay(1); // yield so IDLE0 can feed the task watchdog during a long drain
    }
}

// Formats into a comfortably large scratch buffer, then copies (truncating
// if needed, never overflowing) into the caller's smaller `dst`. Used for
// every fail_reason assignment below instead of snprintf() directly into
// fail_reason (OTA_RECORD_REASON_MAX bytes): several of these messages
// interpolate an esp_err_to_name() string or an int whose width the
// compiler cannot bound at a small destination, which -Werror=format-
// truncation (correctly) refuses to build. Formatting into `tmp` first,
// which is sized generously enough that no realistic message here actually
// truncates, sidesteps that without shortening the messages themselves.
void ota_http_set_fail_reason(char *dst, size_t dst_cap, const char *fmt, ...)
{
    char tmp[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    strncpy(dst, tmp, dst_cap - 1);
    dst[dst_cap - 1] = '\0';
}

esp_err_t ota_http_start(kiln_io_t *io_or_null, MAX31856BusClass *thermo_bus_or_null,
                          SafetyLinkClass *safety_or_null)
{
    s_io = io_or_null;
    s_thermo_bus = thermo_bus_or_null;
    ota_http_safety = safety_or_null;

    s_ota_lock = xSemaphoreCreateMutex();
    if (!s_ota_lock) {
        return ESP_ERR_NO_MEM;
    }
    ota_http_pico_rollback_async_lock = xSemaphoreCreateMutex();
    if (!ota_http_pico_rollback_async_lock) {
        return ESP_ERR_NO_MEM;
    }
    memset(&ota_http_pico_rollback_async, 0, sizeof(ota_http_pico_rollback_async));
    ota_http_pico_rollback_async.state = OTA_PICO_ROLLBACK_ASYNC_IDLE;
    s_update_claim = OTA_UPDATE_NONE;

    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        return ESP_ERR_INVALID_STATE;
    }

    // TODO.md 9.6: the web page itself, GET /ota -- registered first among
    // this file's routes purely because it has no dependency on anything
    // below it; order does not otherwise matter to httpd_register_uri_handler().
    static const httpd_uri_t ota_page_uri = {
        .uri = "/ota", .method = HTTP_GET, .handler = ota_page_get_handler
    };
    esp_err_t err = kiln_http_register(server, &ota_page_uri);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_HTTP_TAG, "httpd_register_uri_handler(/ota) failed: %s", esp_err_to_name(err));
        return err;
    }

    // GET /api/ota/challenge (the AP-password HMAC nonce issuer) was removed
    // here 2026-09-29 along with the rest of that scheme -- see the
    // s_ota_lock comment near the top of this file.

    // TODO.md 9.5: the ESP's own self-update transfer -- see ota_http.h's
    // doc comment on ota_esp_post_handler() for the wire contract.
    static const httpd_uri_t esp_update_uri = {
        .uri = "/api/ota/esp", .method = HTTP_POST, .handler = ota_esp_post_handler
    };
    err = kiln_http_register(server, &esp_update_uri);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_HTTP_TAG, "httpd_register_uri_handler(/api/ota/esp) failed: %s", esp_err_to_name(err));
        return err;
    }

    // TODO.md 9.5: the Pico-image relay -- see ota_http.h's doc comment on
    // this pair of handlers for the wire contract and the async response
    // shape.
    static const httpd_uri_t pico_update_uri = {
        .uri = "/api/ota/pico", .method = HTTP_POST, .handler = ota_pico_post_handler
    };
    err = kiln_http_register(server, &pico_update_uri);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_HTTP_TAG, "httpd_register_uri_handler(/api/ota/pico) failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t pico_status_uri = {
        .uri = "/api/ota/pico/status", .method = HTTP_GET, .handler = ota_pico_status_get_handler
    };
    err = kiln_http_register(server, &pico_status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_HTTP_TAG, "httpd_register_uri_handler(/api/ota/pico/status) failed: %s", esp_err_to_name(err));
        return err;
    }

    // TODO.md 9.6: interlock state for the web page, before the file picker
    // -- see ota_interlock_get_handler()'s own doc comment for why this is
    // unauthenticated, same exposure level as GET /api/status.
    static const httpd_uri_t interlock_uri = {
        .uri = "/api/ota/interlock", .method = HTTP_GET, .handler = ota_interlock_get_handler
    };
    err = kiln_http_register(server, &interlock_uri);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_HTTP_TAG, "httpd_register_uri_handler(/api/ota/interlock) failed: %s", esp_err_to_name(err));
        return err;
    }

    // Closes the gap flagged in ota_http_client.py's module doc comment and
    // mcp_server.py's ota_status() doc comment -- see ota_esp_status_get_handler()'s
    // own doc comment above for the response shape.
    static const httpd_uri_t esp_status_uri = {
        .uri = "/api/ota/esp/status", .method = HTTP_GET, .handler = ota_esp_status_get_handler
    };
    err = kiln_http_register(server, &esp_status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_HTTP_TAG, "httpd_register_uri_handler(/api/ota/esp/status) failed: %s", esp_err_to_name(err));
        return err;
    }

    // Explicit revert to the previous image -- ota_http.h's doc comment
    // above this section for the full contract, and why it needs its own
    // route rather than being folded into POST /api/ota/esp.
    static const httpd_uri_t esp_rollback_uri = {
        .uri = "/api/ota/esp/rollback", .method = HTTP_POST, .handler = ota_esp_rollback_post_handler
    };
    err = kiln_http_register(server, &esp_rollback_uri);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_HTTP_TAG, "httpd_register_uri_handler(/api/ota/esp/rollback) failed: %s", esp_err_to_name(err));
        return err;
    }

    // Deliberate entry into the recovery image -- ota_http_recovery.c's
    // ota_recovery_boot_post_handler() doc comment has the contract
    // (docs/OTA_SINGLE_SLOT_PLAN.md section 4).
    static const httpd_uri_t recovery_boot_uri = {
        .uri = "/api/ota/esp/recovery_boot", .method = HTTP_POST, .handler = ota_recovery_boot_post_handler
    };
    err = kiln_http_register(server, &recovery_boot_uri);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_HTTP_TAG, "httpd_register_uri_handler(/api/ota/esp/recovery_boot) failed: %s", esp_err_to_name(err));
        return err;
    }

    // The Pico half of the same feature -- see ota_pico_rollback_post_
    // handler()'s own doc comment above for the full contract (its own auth
    // context, its own honest accept/refuse/link-down/unknown response
    // shape, built on safety_link_send_rollback_ex()).
    static const httpd_uri_t pico_rollback_uri = {
        .uri = "/api/ota/pico/rollback", .method = HTTP_POST, .handler = ota_pico_rollback_post_handler
    };
    err = kiln_http_register(server, &pico_rollback_uri);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_HTTP_TAG, "httpd_register_uri_handler(/api/ota/pico/rollback) failed: %s", esp_err_to_name(err));
        return err;
    }

    // opus-review finding 3's poll target -- see ota_pico_rollback_status_
    // get_handler()'s own doc comment above.
    static const httpd_uri_t pico_rollback_status_uri = {
        .uri = "/api/ota/pico/rollback/status", .method = HTTP_GET,
        .handler = ota_pico_rollback_status_get_handler
    };
    err = kiln_http_register(server, &pico_rollback_status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_HTTP_TAG, "httpd_register_uri_handler(/api/ota/pico/rollback/status) failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    // boot_guard.h's "a way out of recovery mode" requirement -- see
    // ota_recovery_exit_post_handler()'s own doc comment for the auth
    // (OTA_HTTP_CONTEXT_RECOVERY_EXIT) and recovery-mode-refuses-403 checks
    // it runs, and the ordering between them.
    static const httpd_uri_t recovery_exit_uri = {
        .uri = "/api/ota/esp/recovery_exit", .method = HTTP_POST, .handler = ota_recovery_exit_post_handler
    };
    err = kiln_http_register(server, &recovery_exit_uri);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_HTTP_TAG, "httpd_register_uri_handler(/api/ota/esp/recovery_exit) failed: %s", esp_err_to_name(err));
        return err;
    }

    // docs/audits/boot_guard_post_flash_recovery_footgun_2026-09-08.md's
    // follow-up: the TOOL-driven boot_guard_reset_counter() trigger, see
    // ota_boot_guard_reset_post_handler()'s own doc comment
    // (ota_http_recovery.c) for the auth (OTA_HTTP_CONTEXT_BOOT_GUARD_RESET)
    // and why -- unlike recovery_exit -- this one does NOT gate on
    // boot_guard_is_recovery_mode().
    static const httpd_uri_t boot_guard_reset_uri = {
        .uri = "/api/ota/esp/boot_guard_reset", .method = HTTP_POST, .handler = ota_boot_guard_reset_post_handler
    };
    err = kiln_http_register(server, &boot_guard_reset_uri);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_HTTP_TAG, "httpd_register_uri_handler(/api/ota/esp/boot_guard_reset) failed: %s", esp_err_to_name(err));
        return err;
    }

    // Diagnostics follow-up from the same audit -- see
    // ota_boot_guard_status_get_handler()'s own doc comment (unauthenticated,
    // same exposure level as GET /api/status).
    static const httpd_uri_t boot_guard_status_uri = {
        .uri = "/api/boot_guard", .method = HTTP_GET, .handler = ota_boot_guard_status_get_handler
    };
    err = kiln_http_register(server, &boot_guard_status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_HTTP_TAG, "httpd_register_uri_handler(/api/boot_guard) failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}
