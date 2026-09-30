#include "ota_http.h"
#include "ota_http_internal.h"
#include "ota_http_util.h"

#include <stdarg.h>
#include <string.h>

#include "psa/crypto.h"

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

#include "hal_wdt.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <math.h>

#include "autotune_engine.h"
#include "boot_button.h"
#include "boot_guard.h"
#include "kilnlink/kilnlink_rollback_result.h" /* KILNLINK_ROLLBACK_RESULT_REASON_* -- ota_pico_rollback_post_handler()'s response mapping */
#include "kiln_io.h"
#include "MAX31856.h"
#include "ota_auth.h"
#include "ota_pico_relay.h"
#include "ota_record.h"
#include "profile_executor.h" /* PROFILE_EXEC_* enum only, not its live state -- see below */
#include "run_state.h"
#include "stack_margin.h"
#include "web_encoding.h"
#include "sim_backend.h"
#include "wifi_prov.h"
#include "wifi_provision_http.h"
#include "zones_config_accessors.h"


// --- POST /api/ota/esp/recovery_exit -- boot_guard.h's "a way out of
// recovery mode that does not require a successful OTA" requirement.
//
// Recovery mode is decided ONCE per boot (boot_guard_init(), very early in
// app_main()) and cannot be un-decided for the boot that is currently
// running -- see boot_guard.h's doc comment on boot_guard_is_recovery_mode().
// What CAN happen immediately is clearing the counter that put the board
// there, so the NEXT boot comes up normal; ota_rollback_confirm_task()
// already does that automatically within OTA_CONFIRM_POLL_MS of every boot
// (recovery-mode boots included, since Wi-Fi/dashboard/OTA HTTP all still
// come up in recovery mode -- see boot_confirm_is_healthy()) -- so an
// operator who lands here by accident is never actually stuck waiting on a
// human to notice; the board self-clears and exits on its own next reboot.
// This route exists for the impatient/uncertain case: reboot right now
// instead of waiting for that to happen (or for the RTC/task watchdog to do
// it for you) and land back in normal mode this run.
//
// AUTHENTICATED, same as every other mutating route in this file (esp/pico
// update, esp rollback) -- this used to be the one deliberately
// unauthenticated exception (the reasoning was "it does nothing an attacker
// could not already do by power-cycling the board", plus the cost of adding
// a fourth ota_http_context_t for a button whose only job is "reboot this
// board"). The owner reviewed that tradeoff and chose authentication: a
// forced, unauthenticated reboot reachable from anywhere on the LAN is a
// nuisance-DoS vector worth closing even though the same effect is
// physically achievable by other means, and OTA_HTTP_CONTEXT_RECOVERY_EXIT
// (its own HMAC context string "recovery", its own lockout state, its own
// client-side signing support in ota_page.html/ota_http_client.py/
// mcp_server.py) turned out not to be disproportionate once the other three
// contexts already existed as a template to follow. See ota_http.h's doc
// comment on OTA_HTTP_CONTEXT_RECOVERY_EXIT for why it is its own context
// rather than reusing OTA_HTTP_CONTEXT_ESP.
//
// Auth runs BEFORE the recovery-mode check, not after: ota_interlock.h's doc
// comment on why POST /api/ota/esp's real ordering is "auth first, then
// interlocks" applies here too -- letting an unauthenticated caller learn
// whether this board is currently in recovery mode (via the 403 "board is
// not in recovery mode" vs. proceeding past that check) is the same class of
// live-state leak as revealing a zone temperature to someone with no admin
// session. Checking auth first (route_tier_table.h's ADMIN tier -- the
// AP-password HMAC this used to also require was retired 2026-09-29) means
// an unauthenticated caller learns nothing about recovery-mode state at all,
// before any board-state check runs.

// File-scope (not handler-local) so ota_recovery_exit_reboot_task() below
// can null it itself right before deleting -- see that task's own comment
// on why. DRAM_PSRAM_PLAN.md Phase 0 (4.2): stack_margin_register() target.
static TaskHandle_t s_recovery_exit_reboot_task;

static void ota_recovery_exit_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_LOGW(OTA_HTTP_TAG, "recovery-mode exit requested over HTTP -- rebooting now");
    hal_wdt_reboot(); /* esp_restart() under the hood on this backend; never returns -- see hal_wdt.h.
                        * Same internal-RAM-stack requirement as before: esp_restart() disables the
                        * flash cache, which a PSRAM-backed task stack cannot survive -- see this
                        * task's own stack_margin_register() call below, unchanged. */
    // Null before deleting: stack_margin_read() (stack_margin.c) reads this
    // handle fresh on every report and treats non-NULL as "alive", calling
    // uxTaskGetStackHighWaterMark() on it -- left non-NULL past this point it
    // would dangle onto a deleted task the instant the scheduler reclaims
    // this TCB. (A reader racing this assignment and seeing the handle just
    // before it's cleared is benign: uxTaskGetStackHighWaterMark() on a task
    // that is about to be deleted but not yet reclaimed is still valid.)
    s_recovery_exit_reboot_task = NULL;
    vTaskDelete(NULL); /* defensive only: hal_wdt_reboot() is not declared noreturn (the host
                         * fake deliberately returns so tests can observe the call -- see
                         * fake_wdt.c), so this guards a real backend that somehow returns
                         * instead of falling off the end of a FreeRTOS task function. */
}

esp_err_t ota_recovery_exit_post_handler(httpd_req_t *req)
{
    // 1-2. Auth (ADMIN tier only, since 2026-09-29). Still runs before the
    // recovery-mode check below, not after -- see the doc comment above this
    // handler for why.
    char ip[46];
    if (!ota_http_authenticate_request(req, OTA_HTTP_CONTEXT_RECOVERY_EXIT, ip)) {
        return ESP_OK;
    }

    // 3. Only meaningful in recovery mode -- refuses (403) outside it so
    // this is not just a general-purpose authenticated reboot button on a
    // normal boot. Runs AFTER auth (see doc comment above) so a caller who
    // never proves they hold the AP password cannot use this route's
    // response to probe whether the board is currently in recovery mode.
    if (!boot_guard_is_recovery_mode()) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "board is not in recovery mode");
        return ESP_OK;
    }
    // boot_guard_mark_healthy() is very likely already a no-op here --
    // ota_rollback_confirm_task() clears the counter automatically within
    // OTA_CONFIRM_POLL_MS of boot whenever nvs/web/ota are all up, which they
    // are in recovery mode too -- but calling it again is cheap and harmless
    // (boot_guard_mark_healthy() no-ops once already cleared this boot), and
    // removes any dependency on that background task's timing for this
    // explicit, operator-requested exit.
    if (!boot_guard_mark_healthy()) {
        /* 2026-09-08 recovery-loop audit: this is no longer trusted to be a
         * harmless no-op -- an operator explicitly asking to exit recovery
         * mode deserves to know the clear did not verify, even though the
         * reboot below proceeds regardless (the background confirm task,
         * main_network_http.c, keeps retrying across this reboot's own
         * lifetime too). */
        ESP_LOGW(OTA_HTTP_TAG, "recovery_exit: boot-guard clear did not verify this call -- rebooting anyway; "
                      "the background confirm task will keep retrying on the next boot");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"status\":\"rebooting\"}");
    /* Plain xTaskCreate -- an INTERNAL-RAM stack, deliberately, exactly like
     * ota_rollback_reboot_task() above. This task calls esp_restart(), which
     * goes through spi_flash_disable_interrupts_caches_and_other_cpu(); a
     * task whose stack lives in PSRAM cannot run with the flash cache
     * disabled and trips esp_task_stack_is_sane_cache_disabled(). Putting
     * this stack in PSRAM to save 2 KB of internal DRAM would mean the
     * recovery-mode escape hatch panics the board instead of rebooting it.
     * (Same trap that produced a real crash in profile_executor.c earlier
     * the same day; see its task-creation comment.) */
    if (xTaskCreate(ota_recovery_exit_reboot_task, "recovery_exit_reboot", 2048, NULL,
                    tskIDLE_PRIORITY + 1, &s_recovery_exit_reboot_task) != pdPASS) {
        ESP_LOGE(OTA_HTTP_TAG, "recovery-mode exit: failed to start the reboot task -- board will NOT "
                      "reboot; power-cycle it, the counter is already cleared");
    }
    /* Registered unconditionally, success or not -- stack_margin_register()
     * reads *task_handle_slot fresh at report time, so a creation failure
     * just reads back alive=false rather than needing a second branch here.
     * 2048 must match the xTaskCreate() literal above. Label shortened to
     * "recovery_exit" (not the full "recovery_exit_reboot" FreeRTOS task
     * name, unchanged above) -- the full name is 20 chars and
     * STACK_MARGIN_NAME_MAX (20) leaves only 19 usable, which would
     * silently truncate it to "recovery_exit_rebo". */
    stack_margin_register("recovery_exit", &s_recovery_exit_reboot_task, 2048);
    return ESP_OK;
}


// --- POST /api/ota/esp/boot_guard_reset --
// docs/audits/boot_guard_post_flash_recovery_footgun_2026-09-08.md's "not
// yet implemented" follow-up, now wired up: a TOOL that just performed a
// deliberate flash and independently confirmed (its own verify step, e.g.
// tools/PcTools' flash_firmware()) that the NEW build is actually running
// calls this to bypass boot_confirm_is_healthy()'s flaky one-shot NVS
// snapshot and clear boot_guard's recovery-mode counter directly.
//
// Deliberately does NOT gate on boot_guard_is_recovery_mode() the way
// ota_recovery_exit_post_handler() does -- see this route's own context
// doc comment (ota_state.h, OTA_HTTP_CONTEXT_BOOT_GUARD_RESET): the common
// case this exists for is an ORDINARY, non-recovery-mode board being
// reflashed during normal iteration, specifically so it never has to reach
// recovery mode in the first place. Refusing outside recovery mode, the
// way recovery_exit does, would defeat the entire point.
//
// Does NOT reboot the board -- unlike recovery_exit/sw_reset/rollback, this
// route's only effect is the NVS clear; the board just flashed is already
// mid-boot into the new image by the time the tool's verify step (and
// therefore this call) runs.
esp_err_t ota_boot_guard_reset_post_handler(httpd_req_t *req)
{
    // 1-2. Auth (ADMIN tier only, since 2026-09-29). No recovery-mode check
    // after this, unlike recovery_exit -- see this handler's own doc comment
    // above for why.
    char ip[46];
    if (!ota_http_authenticate_request(req, OTA_HTTP_CONTEXT_BOOT_GUARD_RESET, ip)) {
        return ESP_OK;
    }

    bool verified = boot_guard_reset_counter();
    char json[96];
    int n = snprintf(json, sizeof(json), "{\"ok\":%s,\"boot_count\":%lu}",
                      verified ? "true" : "false", (unsigned long)boot_guard_get_boot_count());
    if (!verified) {
        // Same reasoning as ota_recovery_exit_post_handler()'s equivalent
        // log line: the caller (a tool that just flashed the board and is
        // relying on this call to keep the counter from climbing) deserves
        // to know the clear did not verify, not a silent 200 that implies
        // it worked.
        ESP_LOGW(OTA_HTTP_TAG, "boot_guard_reset from %s: boot-guard clear did not verify -- the "
                      "persisted counter may still be nonzero; caller should treat this flash as "
                      "NOT having reset the recovery-mode counter", ip);
    } else {
        ESP_LOGI(OTA_HTTP_TAG, "boot_guard_reset from %s: boot-guard counter cleared and verified "
                      "-- recorded as a deliberate flash", ip);
    }
    httpd_resp_set_type(req, "application/json");
    ota_http_send_json_clamped(req, json, n, sizeof(json));
    return ESP_OK;
}

// GET /api/boot_guard -- diagnostics follow-up flagged by the same audit
// ("a /api/boot_guard diagnostics route is a reasonable follow-up"): before
// this, confirming the fix above actually worked required a JTAG memory
// read of s_bg. Unauthenticated, matching GET /api/status's existing
// exposure level (a boot count and a recovery-mode boolean are no more
// sensitive than the live zone temperatures that route already exposes
// without auth) -- see ota_interlock_get_handler()'s own doc comment just
// below for the same reasoning applied to that route.
esp_err_t ota_boot_guard_status_get_handler(httpd_req_t *req)
{
    char json[96];
    int n = snprintf(json, sizeof(json), "{\"boot_count\":%lu,\"recovery_mode\":%s}",
                      (unsigned long)boot_guard_get_boot_count(),
                      boot_guard_is_recovery_mode() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    ota_http_send_json_clamped(req, json, n, sizeof(json));
    return ESP_OK;
}


// ota_pico_rollback_format_body()/ota_pico_rollback_reason_str() moved to
// ota_http_util.c -- see the #define aliases above and ota_http_util.h's
// header comment.

// GET /api/ota/interlock -- TODO.md 9.6: "interlock state shown BEFORE the
// file picker, with the blocker named." ota_http_check_interlocks() itself
// is only ever called from inside the authenticated POST /api/ota/{esp,pico}
// handlers (see ota_http.h's doc comment above that function: an
// unauthenticated caller would learn live kiln telemetry, e.g. "zone 2 is at
// 340 C", folded into the refusal reason string). That reasoning is sound in
// isolation, but this codebase's own GET /api/status (dashboard_http.c) is
// ALREADY unauthenticated and already returns every zone's live temperature
// directly -- so gating this endpoint behind the OTA challenge/HMAC dance
// (which would force the web page to ask for the Wi-Fi AP password just to
// show "kiln is running a profile" before the file picker even appears)
// would not close any exposure that isn't already open on this same LAN.
// Unauthenticated here, matching /api/status's existing exposure level, not
// a new one. Returns {"ok":true} or {"ok":false,"reason":"<why>"}.
esp_err_t ota_interlock_get_handler(httpd_req_t *req)
{
    /* Asked WITHOUT the acknowledgement on purpose: this endpoint reports
     * the board's actual state so the page can decide what to show, and
     * passing the ack here would hide the very condition the page needs to
     * warn about. `needs_ack` tells the page that this particular refusal
     * is the overridable one, so it can offer the warning dialog instead of
     * greying the control out. */
    char reason[OTA_INTERLOCK_REASON_MAX];
    ota_interlock_result_t r = ota_http_check_interlocks(false, reason, sizeof(reason));

    char body[OTA_INTERLOCK_REASON_MAX + 64];
    int n;
    if (r == OTA_INTERLOCK_OK) {
        n = snprintf(body, sizeof(body), "{\"ok\":true}");
    } else {
        n = snprintf(body, sizeof(body), "{\"ok\":false,\"reason\":\"%s\",\"needs_ack\":%s}", reason,
                     r == OTA_INTERLOCK_REFUSED_NEEDS_ACK ? "true" : "false");
    }
    httpd_resp_set_type(req, "application/json");
    ota_http_send_json_clamped(req, body, n, sizeof(body));
    return ESP_OK;
}

