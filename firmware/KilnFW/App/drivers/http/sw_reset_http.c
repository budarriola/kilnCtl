#include "sw_reset_http.h"
#include "http_auth_http.h" // kiln_http_register() -- WEB_AUTH_PLAN.md section 5

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "hal_esp_common.h"
#include "hal_wdt.h"
#include "ota_http.h" /* interlocks + challenge/response auth -- same idiom as
                        * factory_reset.c's reset_post_handler(), see there for
                        * the full rationale on auth-before-interlock ordering */
#include "kilnlink/kilnlink_reboot_result.h" /* kilnlink_reboot_result_reason_t --
                                                * decoding the Pico's REFUSED reason so
                                                * the operator-facing sentence names the
                                                * REAL refusal (relay armed vs. a firmware
                                                * transfer in flight), not a fixed guess */
#include "safety_link.h" /* safety_link_send_reboot() -- the real Pico
                           * reboot-in-place command (SAFETY_CMD_REBOOT,
                           * 0x29); safety_link_send_announce_reboot() is
                           * still sent alongside it, see the reboot task */
#include "wifi_provision_http.h"

static const char *TAG = "sw_reset";

// Single cap for the heap-allocated response body below -- used at every
// site that sizes, fills, or bounds-checks that buffer (the malloc, the
// snprintf, and the truncation check), so the four sites can never drift
// apart. check_httpd_task_stack_budget.py: this used to be a plain local
// directly on the httpd_worker stack; see the malloc call below.
#define SW_RESET_BODY_CAP 960u

// The short fixed fallback sent when the heap body can't be allocated or
// would be truncated -- both callers below need the exact same sentence.
static const char kSwResetFallbackBody[] =
    "ok -- rebooting this controller now. No configuration was changed. "
    "This reboot WILL latch an S6a main-fault trip: clear it with POST "
    "/api/safety/clear_trip (Safety page) before heating. See the log "
    "for the safety processor's own outcome.";

// --- What this route deliberately does NOT do ----------------------------
//
// 1. It does not clear a latched safety trip, and must not be described as
//    if it did. da506105's UI copy and commit message both claimed it
//    cleared "a stuck S6a trip"; that was false, and settings_page.html's
//    own 2026-09-09 CORRECTION comment carries the full verification (S6a
//    is unconditional in safety_guards.c -- reboot_grace_active gates only
//    S6b -- and an ESP reset floats GPIO6, which the Pico reads AS
//    mainFault, so this route reliably CAUSES an S6a latch rather than
//    clearing one -- and now that the Pico reboots too, the Pico's own
//    RAM-only latch clearing does not rescue it either: the RP2040 is back
//    watching the still-floating line long before the ESP is. The response
//    body therefore states plainly that the reboot WILL latch S6a and that
//    POST /api/safety/clear_trip is a REQUIRED follow-up before heating.
//    Auto-clearing it here was considered and rejected outright: the trip is
//    correct -- the main processor really was absent -- and clearing it from
//    the same request that caused it would make S6a unable to report the one
//    event it exists to report.
//
//    Extending the grace over S6a was considered and rejected too: S6a's
//    evidence is a present positive assertion rather than S6b's absence of
//    information, so suppression would discard it rather than defer it, and
//    a grace cannot un-latch the already-latched trip anyway. The real
//    clear path is POST /api/safety/clear_trip
//    (safety_link_send_clear_trip()), reachable without JTAG from the
//    Safety page -- not from here.
//
// 2. It does not call boot_guard_reset_counter(). That escape hatch exists
//    for a TOOL that knows it just deliberately replaced the firmware
//    (docs/audits/boot_guard_post_flash_recovery_footgun_2026-09-08.md),
//    and an operator clicking Reboot is NOT the same fact. All a click
//    proves is that this board reached the HTTP stack on this boot --
//    exactly the evidence boot_confirm_is_healthy() already tries to use,
//    not the independent "the firmware just changed" knowledge that
//    justifies bypassing the counter. Wiring it in here would mask a board
//    that boots fine, serves this page, and then dies minutes later: an
//    operator rebooting it each time would clear the counter every cycle
//    and recovery mode would never arm. That is the same failure the
//    audit's negative test proved when the call was placed in
//    boot_guard_init(), just triggered by a human instead of by every boot.
//
//    The residual is accepted and real: because the counter is NOT cleared
//    here, and because boot_confirm's one-shot NVS snapshot can miss, a
//    run of back-to-back reboots from this button can still walk a healthy
//    board toward recovery mode. Reported rather than papered over -- the
//    fix belongs in boot_confirm's snapshot (retry it) or in flash_firmware()
//    calling boot_guard_reset_counter(), not in weakening the counter from a
//    web button.

// Set once by sw_reset_http_start(), read-only after -- same pattern as
// ota_http.c's own s_io/s_thermo_bus/ota_http_safety. May be NULL on a board
// with no safety processor commissioned yet; every use below tolerates that.
static SafetyLinkClass *s_safety;

// How the two halves of this reboot are reported to the operator.
//
// The distinction that matters: "the safety processor accepted and is about
// to reset" IS provable from the wire (SAFETY_CMD_REBOOT_RESULT, accepted=1).
// "The safety processor finished rebooting" is NOT provable here -- that
// reply necessarily leaves the Pico before its reset, and this ESP is about
// to reboot itself, so it will not be around to watch. Every sentence below
// therefore says "accepted"/"commanded", never "rebooted".
typedef enum {
    SW_RESET_PICO_ACCEPTED = 0,
    SW_RESET_PICO_REFUSED_ARMED,     /* relay energized -- heating permission is live */
    SW_RESET_PICO_REFUSED_TRANSFER,  /* a firmware transfer into the inactive slot is in flight */
    SW_RESET_PICO_REFUSED_OTHER,     /* refused for a reason this build does not recognize --
                                       * an older or newer peer; NEVER render this as a specific
                                       * claim about relay state */
    SW_RESET_PICO_UNCONFIRMED, /* sent but unanswered -- or link down, or send failed */
    SW_RESET_PICO_NO_LINK,     /* no safety processor configured on this boot at all */
} sw_reset_pico_report_t;

// Runs on its own short-lived task so the response already queued by the
// handler has a chance to reach the client before esp_restart() tears the
// connection down -- identical shape to factory_reset.c's reboot_task() and
// ota_http_esp.c's ota_rollback_reboot_task().
//
// The Pico's own reboot is NOT commanded here: it is commanded on the
// request task, before the response is written, so its real outcome can go
// INTO that response (2026-09-09 -- the first version of this file only ever
// sent ANNOUNCE_REBOOT from here and said so in its own comments, because no
// wire command for a Pico reboot-in-place existed yet; SAFETY_CMD_REBOOT
// (0x29, kilnlink_reboot.h) now does).
//
// ANNOUNCE_REBOOT is still sent from here, and is still a different fact:
// it announces THIS ESP's imminent absence. It matters most in exactly the
// case where the Pico refused its own reboot (relay ARMED) and therefore
// stays up -- without it, that Pico's S6b link-dead guard would nuisance-trip
// on this ESP's 10-15 second silence.

// Per-REQUEST arm flag, heap-allocated by the handler and owned by exactly
// one reboot task -- 2026-09-09 replaced what used to be a single static
// s_reboot_armed shared across every request. That single flag had a real
// two-request window: esp_http_server dispatches one request at a time, but
// a first request's reboot task is still ALIVE (delaying, then polling) while
// a second request is being handled on the request task, and the second
// handler's very first act used to be `s_reboot_armed = false` -- disarming
// the first task's flag. If task creation for that second request then
// failed, the 500 response correctly says "NEITHER processor was rebooted"
// for the SECOND request, but the FIRST task, now re-armed to false and
// waiting again, still goes on to reboot the board once its own poll window
// re-passes -- making the "NEITHER" claim false the moment it was printed.
// Giving each request its own heap-allocated flag (freed by whichever side
// finishes last: the task on the reboot path, the handler on a
// task-creation-failure path) makes that collision structurally impossible
// rather than merely unlikely.
typedef struct {
    volatile bool armed;
} sw_reset_reboot_ctx_t;

static void sw_reset_reboot_task(void *arg)
{
    sw_reset_reboot_ctx_t *ctx = (sw_reset_reboot_ctx_t *)arg;
    vTaskDelay(pdMS_TO_TICKS(500));

    // Wait for the handler to finish (bounded). This is NOT merely
    // defensive: arming happens in the handler AFTER httpd_resp_sendstr()
    // returns (see the handler below), and sendstr() can block on a slow or
    // stalled TCP client for longer than this task's own 500 ms initial
    // delay. Without this wait, such a client could hold the send past that
    // delay and this task would reboot the board mid-response -- the very
    // divergence (no response actually delivered, a report nobody reads)
    // this ordering exists to prevent. 5 s is chosen as generously longer
    // than the Pico exchange (~345 ms worst case) plus any ordinary response
    // write; expiring the wait reboots anyway rather than leaking a task
    // that never dies, on the theory that a client stalled 5 s past a 345 ms
    // exchange is not coming back for the response either way.
    const int kArmPollMs = 20;
    const int kArmWaitMs = 5000;
    for (int waited = 0; !ctx->armed && waited < kArmWaitMs; waited += kArmPollMs) {
        vTaskDelay(pdMS_TO_TICKS(kArmPollMs));
    }
    if (!ctx->armed) {
        ESP_LOGW(TAG, "sw_reset: reboot task armed-flag never set after %d ms -- rebooting anyway",
                 kArmWaitMs);
    }

    if (s_safety) {
        esp_err_t announce_err = safety_link_send_announce_reboot(s_safety);
        if (announce_err != ESP_OK) {
            ESP_LOGW(TAG, "sw_reset: safety_link_send_announce_reboot failed (%s) -- "
                          "rebooting anyway, S6b may nuisance-trip on the safety processor",
                     esp_err_to_name(announce_err));
        }
    } else {
        ESP_LOGW(TAG, "sw_reset: no safety link configured this boot -- announce-reboot not sent");
    }

    ESP_LOGW(TAG, "sw_reset: rebooting ESP now (no configuration touched)");
    free(ctx);
    hal_wdt_reboot(); /* esp_restart() under the hood -- never returns on real hardware */
    vTaskDelete(NULL); /* defensive only, see factory_reset.c's identical comment */
}

// Translates safety_link_send_reboot()'s outcome into this file's reporting
// vocabulary. Pure mapping, no policy -- and in particular it NEVER upgrades
// an unconfirmed outcome to accepted. Non-static so the host test can pin
// that property without an httpd_req_t.
//
// reason_code is the wire byte from SAFETY_CMD_REBOOT_RESULT, meaningful only
// when outcome is REFUSED (kilnlink_reboot_result.h). It is decoded here into
// a DISTINCT report per refusal reason so the operator-facing sentence can
// name the real cause instead of a single fixed guess -- 2026-09-09 fixed a
// defect where every refusal, including the new
// KILNLINK_REBOOT_RESULT_REASON_TRANSFER_ACTIVE (a firmware transfer in
// flight, nothing to do with relay state), rendered as "its heating relay is
// armed": a heating-permission claim that was simply false for that reason,
// and dangerous precisely because it is a heating-permission claim -- an
// operator who believes the relay is armed while the board is idle hunts a
// stuck interlock, or dismisses a genuinely armed relay next time. Any reason
// byte this build does not recognize (an older or newer peer,
// KILNLINK_REBOOT_RESULT_REASON_UNKNOWN, or a future enumerator) maps to the
// generic OTHER report, which never claims a specific cause -- never let an
// unknown reason render as a specific claim.
sw_reset_pico_report_t sw_reset_classify_pico_outcome(esp_err_t err,
                                                      safety_link_reboot_outcome_t outcome,
                                                      uint8_t reason_code)
{
    if (err != ESP_OK) {
        // A local/driver error, not an answer from the peer. Not knowing is
        // not the same as being told no: unconfirmed, never refused.
        return SW_RESET_PICO_UNCONFIRMED;
    }
    switch (outcome) {
    case SAFETY_LINK_REBOOT_OUTCOME_ACCEPTED:
        return SW_RESET_PICO_ACCEPTED;
    case SAFETY_LINK_REBOOT_OUTCOME_REFUSED:
        switch ((kilnlink_reboot_result_reason_t)reason_code) {
        case KILNLINK_REBOOT_RESULT_REASON_ARMED:
            return SW_RESET_PICO_REFUSED_ARMED;
        case KILNLINK_REBOOT_RESULT_REASON_TRANSFER_ACTIVE:
            return SW_RESET_PICO_REFUSED_TRANSFER;
        case KILNLINK_REBOOT_RESULT_REASON_NONE:
        case KILNLINK_REBOOT_RESULT_REASON_UNKNOWN:
        default:
            // Includes NONE (should not pair with a refusal at all) and any
            // reason byte this build's enum does not name -- an older or
            // newer peer. Never render as a specific claim.
            return SW_RESET_PICO_REFUSED_OTHER;
        }
    case SAFETY_LINK_REBOOT_OUTCOME_LINK_DOWN:
    case SAFETY_LINK_REBOOT_OUTCOME_SEND_FAILED:
    case SAFETY_LINK_REBOOT_OUTCOME_NO_REPLY:
    default:
        // Every remaining case -- including any future enumerator this
        // switch has not been taught about -- is "we do not know".
        // Deliberately the default, so a new outcome can never silently
        // read as success.
        return SW_RESET_PICO_UNCONFIRMED;
    }
}

// The operator-facing sentence for each outcome. Kept next to the enum so a
// new outcome cannot be added without a sentence for it.
const char *sw_reset_pico_sentence(sw_reset_pico_report_t report)
{
    switch (report) {
    case SW_RESET_PICO_ACCEPTED:
        return "The safety processor accepted the reboot and is resetting into the same firmware; "
               "its configuration was not touched.";
    case SW_RESET_PICO_REFUSED_ARMED:
        return "The safety processor REFUSED to reboot (its heating relay is armed). It stays "
               "running; only this controller is rebooting.";
    case SW_RESET_PICO_REFUSED_TRANSFER:
        return "The safety processor REFUSED to reboot (a firmware transfer into it is in "
               "progress). It stays running; only this controller is rebooting. Retry after the "
               "transfer finishes.";
    case SW_RESET_PICO_REFUSED_OTHER:
        return "The safety processor REFUSED to reboot for a reason this controller's firmware "
               "does not recognize (it may be running older or newer firmware). It stays running; "
               "only this controller is rebooting.";
    case SW_RESET_PICO_NO_LINK:
        return "No safety processor is configured on this boot, so nothing was sent to one. Only "
               "this controller is rebooting.";
    case SW_RESET_PICO_UNCONFIRMED:
    default:
        return "The reboot was sent to the safety processor but it did not confirm, so its reboot "
               "is NOT confirmed -- it may be running firmware that predates this command, or the "
               "link dropped. Only this controller's reboot is certain.";
    }
}

static esp_err_t sw_reset_post_handler(httpd_req_t *req)
{
    // Auth first, same reasoning as factory_reset.c's reset_post_handler():
    // an unauthenticated caller must not learn live kiln state from this
    // route's own refusal reason. Its own context (OTA_HTTP_CONTEXT_SW_RESET)
    // means a MAC signed for this route cannot double as authorization for
    // any other destructive or update route, and vice versa.
    char ip[46];
    if (!ota_http_authenticate_request(req, OTA_HTTP_CONTEXT_SW_RESET, ip)) {
        return ESP_OK;
    }

    // Refuse while a firing/autotune is running or any zone's heater is
    // commanded on -- the SAME gate factory_reset.c and kiln_cfg_http.c's
    // apply already use. Rebooting the ESP mid-firing drops control of live
    // heaters (the executor and its relay commands vanish with it); a
    // reboot is not gentler than a config wipe from the kiln's point of
    // view, so it gets exactly the same refusal, not a looser one.
    // ota_http_req_ack_no_safety() carries the same escape hatch factory
    // reset allows for a board with no safety processor -- a board being
    // commissioned still needs a way to clear a stuck state without JTAG.
    char interlock_reason[OTA_INTERLOCK_REASON_MAX];
    ota_interlock_result_t gate = ota_http_check_interlocks(ota_http_req_ack_no_safety(req),
                                                             interlock_reason,
                                                             sizeof(interlock_reason));
    if (gate != OTA_INTERLOCK_OK) {
        ESP_LOGW(TAG, "sw_reset from %s: refused by interlock: %s", ip, interlock_reason);
        return ota_http_send_interlock_refusal(req, gate, interlock_reason);
    }

    // No body to read: unlike /api/factory_reset, this route has no scope
    // to choose -- it always means "reboot both processors, touch nothing
    // else". POST with an empty body is intentional here.

    ESP_LOGW(TAG, "sw_reset from %s: authenticated, rebooting -- no config touched", ip);

    // THIS controller's reboot task is created FIRST, before anything is
    // commanded anywhere, and a failure to create it aborts the whole route
    // without touching the safety processor.
    //
    // Why this ordering: task creation is the one step here that can fail
    // for a local reason (PSRAM allocation), and it is the step that owns
    // the ESP half of the reboot. Created after the Pico command, a failure
    // leaves the system HALF-RESET -- the safety processor has already
    // accepted and is resetting, and there is nothing left to undo it with.
    // Created first, a failure leaves BOTH processors untouched and the
    // operator gets an error instead of a false "both rebooted", which is
    // the whole defect being fixed here (the return value used to be
    // discarded entirely, and the response claimed this controller was
    // rebooting regardless -- this repo's "logging unchecked success"
    // class).
    //
    // The created-but-not-yet-run window is closed by ctx->armed: the task
    // delays, then waits for THIS request's handler to arm it, so it cannot
    // reboot this ESP out from under the Pico exchange or the response. The
    // context is per-request (heap-allocated here, freed by whichever side
    // finishes last) precisely so that a second, concurrent-in-flight
    // request's flag can never disarm this one's task -- see the struct's
    // own comment above for the two-request window this closes.
    //
    // 2026-08-22-style PSRAM-stack note: sw_reset_reboot_task() only
    // vTaskDelay()s, sends one fire-and-forget UART frame and calls
    // hal_wdt_reboot() -- no flash access on this task's own stack, so
    // unlike factory_reset.c's erase path there is no flash-worker dispatch
    // needed here.
    sw_reset_reboot_ctx_t *ctx = (sw_reset_reboot_ctx_t *)heap_caps_malloc(
        sizeof(sw_reset_reboot_ctx_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx) {
        ESP_LOGE(TAG, "sw_reset from %s: could not allocate the reboot context -- NOTHING was "
                      "rebooted; the safety processor was deliberately not commanded",
                 ip);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req,
                           "FAILED -- could not start this controller's reboot (out of memory). "
                           "NEITHER processor was rebooted: the safety processor was deliberately "
                           "not commanded, so nothing is half-reset. No configuration was changed. "
                           "Try again; if it keeps failing, power-cycle the board.");
        return ESP_OK;
    }
    ctx->armed = false;
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(sw_reset_reboot_task, "sw_reset_reboot",
                                                         3072, ctx, tskIDLE_PRIORITY + 1, NULL,
                                                         tskNO_AFFINITY,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        free(ctx);
        ESP_LOGE(TAG, "sw_reset from %s: could not create the reboot task (%d) -- NOTHING was "
                      "rebooted; the safety processor was deliberately not commanded",
                 ip, (int)created);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req,
                           "FAILED -- could not start this controller's reboot (out of memory). "
                           "NEITHER processor was rebooted: the safety processor was deliberately "
                           "not commanded, so nothing is half-reset. No configuration was changed. "
                           "Try again; if it keeps failing, power-cycle the board.");
        return ESP_OK;
    }

    // Pico half next, on this task, while there is still an HTTP response
    // to put the answer in. safety_link_send_reboot() is bounded (one send
    // plus SAFETY_LINK_REPLY_TIMEOUT_MS, ~345 ms at 230400 baud), well
    // inside what an httpd handler may spend, and doing it here is the only
    // way this route can report honestly on BOTH processors: run from the
    // delayed reboot task instead and the answer arrives after the response
    // has already been written, so nobody ever learns it.
    sw_reset_pico_report_t pico_report = SW_RESET_PICO_NO_LINK;
    if (s_safety) {
        safety_link_reboot_outcome_t outcome = SAFETY_LINK_REBOOT_OUTCOME_NO_REPLY;
        uint8_t reason_code = 0;
        esp_err_t reboot_err = safety_link_send_reboot(s_safety, &outcome, &reason_code);
        pico_report = sw_reset_classify_pico_outcome(reboot_err, outcome, reason_code);
        ESP_LOGW(TAG, "sw_reset: safety processor reboot -> outcome=%d err=%s reason=%u: %s",
                 (int)outcome, esp_err_to_name(reboot_err), (unsigned)reason_code,
                 sw_reset_pico_sentence(pico_report));
    } else {
        ESP_LOGW(TAG, "sw_reset: no safety link configured this boot -- ESP half only");
    }

    // Reported per-processor, never as one undifferentiated "ok": the two
    // halves genuinely can disagree (a Pico holding an armed relay refuses
    // while this ESP still reboots), and an operator told "both rebooted"
    // when only one did will draw exactly the wrong conclusion about
    // whatever stuck state they were trying to clear.
    //
    // The S6a sentence says WILL, not "does not clear": this route reliably
    // CREATES a latched main-fault trip. GPIO6 floats through the ESP's
    // reset (safety_link.c calls that an undefined fault state at the
    // safety processor), safety_guards.c's S6a trips on the debounced
    // mainFault line unconditionally, and a Pico that reboots too comes back
    // first and re-latches on the still-floating line. So the follow-up is
    // not optional advice, it is required before heating -- said here rather
    // than left for the operator to discover from a refusing kiln.
    // This response buffer used to be a plain local directly on the
    // httpd_worker stack. Heap (PSRAM preferred); a malloc failure falls
    // back to the same short fixed string the truncation branch below
    // already sends, rather than refusing the whole route -- both
    // processors are already committed to rebooting by this point
    // (ctx->armed below still has to run either way), so this is a
    // response-wording fallback, not a correctness gate.
    char *body = heap_caps_malloc(SW_RESET_BODY_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        body = malloc(SW_RESET_BODY_CAP);
    }
    if (!body) {
        httpd_resp_sendstr(req, kSwResetFallbackBody);
        ctx->armed = true;
        return ESP_OK;
    }
    int n = snprintf(body, SW_RESET_BODY_CAP,
                     "ok -- rebooting this controller now; it will be unreachable for about 10-15 "
                     "seconds, then come back on the same address. No configuration was changed on "
                     "either processor. %s "
                     "IMPORTANT: this reboot WILL latch a safety trip. While this controller "
                     "resets, its fault line to the safety processor is undefined, which the "
                     "safety processor reads as a main-fault (S6a) and latches "
                     "SAFETY_TRIP_MAIN_FAULT. That is correct fail-safe behaviour, not a bug, and "
                     "nothing on this path clears it. REQUIRED FOLLOW-UP before heating: clear the "
                     "trip with POST /api/safety/clear_trip -- the \"Clear latched trip\" button on "
                     "the Safety page, or the dashboard's Clear Trip button. Heat stays blocked "
                     "until you do.",
                     sw_reset_pico_sentence(pico_report));
    if (n < 0 || (size_t)n >= SW_RESET_BODY_CAP) {
        /* Truncated (cannot happen with today's strings, but never send half
         * a sentence about which processors rebooted -- and never drop the
         * required-follow-up half either). */
        free(body);
        httpd_resp_sendstr(req, kSwResetFallbackBody);
        ctx->armed = true;
        return ESP_OK;
    }
    httpd_resp_sendstr(req, body);
    free(body);
    ctx->armed = true;
    return ESP_OK;
}

esp_err_t sw_reset_http_start(SafetyLinkClass *safety_or_null)
{
    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }
    s_safety = safety_or_null;

    static const httpd_uri_t post_uri = {
        .uri = "/api/sw_reset", .method = HTTP_POST, .handler = sw_reset_post_handler,
    };
    esp_err_t err = kiln_http_register(server, &post_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST /api/sw_reset) failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}
