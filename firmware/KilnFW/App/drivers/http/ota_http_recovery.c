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
#include "boot_guard.h"
#include "kilnlink/kilnlink_rollback_result.h" /* KILNLINK_ROLLBACK_RESULT_REASON_* -- ota_pico_rollback_post_handler()'s response mapping */
#include "kiln_io.h"
#include "MAX31856.h"
#include "ota_auth.h"
#include "ota_pico_relay.h"
#include "ota_record.h"
#include "hal_sysinfo.h" /* hal_sysinfo_get_build_info() */
#include "hal_time.h" /* hal_time_now_us() */
#include "recovery_switch.h" /* recovery_switch_select_boot()/_restore_running() */
#include "relay_authority.h" /* relay_authority_heat_run_active() -- system_mode_gate below */
#include "system_mode_gate.h" /* SYS_ACTION_RECOVERY_BOOT */
#include "system_mode_gate_http.h" /* system_mode_gate_http_send_refusal() */
#include "profile_executor.h" /* PROFILE_EXEC_* enum only, not its live state -- see below */
#include "run_state.h"
#include "stack_margin.h"
#include "dram_watch.h"
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
// ROUTE_TIER_ADMIN, same as every other mutating route in this file (esp/pico
// update, esp rollback) -- this used to be the one deliberately
// unauthenticated exception (the reasoning was "it does nothing an attacker
// could not already do by power-cycling the board", plus the cost of adding
// a fourth ota_http_context_t for a button whose only job is "reboot this
// board"). The owner reviewed that tradeoff and chose authentication, first
// as its own AP-password HMAC context (OTA_HTTP_CONTEXT_RECOVERY_EXIT) and,
// since 2026-09-29, as plain ADMIN-tier login once that whole HMAC scheme
// was retired -- a forced, unauthenticated reboot reachable from anywhere on
// the LAN stays closed either way.
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
    ota_http_get_client_ip(req, ip, sizeof(ip)); /* logging only -- ADMIN tier (route_tier_table.h) is the only gate, AP-password HMAC retired 2026-09-29 */

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
        /* 2026-09-08 recovery-loop audit, campaign 9b LOW: the clear did not
         * verify, so a reboot would land straight back in recovery mode.
         * Report it (500) instead of "ok / rebooting" and do NOT reboot; the
         * background confirm task (main_network_http.c) keeps retrying. */
        ESP_LOGW(OTA_HTTP_TAG, "recovery_exit: boot-guard clear did not verify this call -- NOT rebooting; "
                      "the background confirm task keeps retrying, call again once it has cleared");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"boot-guard clear did not verify; not rebooting\"}");
        return ESP_OK;
    }
    /* Plain xTaskCreate -- an INTERNAL-RAM stack, deliberately, exactly like
     * ota_rollback_reboot_task() above. This task calls esp_restart(), which
     * goes through spi_flash_disable_interrupts_caches_and_other_cpu(); a
     * task whose stack lives in PSRAM cannot run with the flash cache
     * disabled and trips esp_task_stack_is_sane_cache_disabled(). Putting
     * this stack in PSRAM to save 2 KB of internal DRAM would mean the
     * recovery-mode escape hatch panics the board instead of rebooting it.
     * (Same trap that produced a real crash in profile_executor.c earlier
     * the same day; see its task-creation comment.)
     *
     * The reply is sent only AFTER the task exists (campaign 9b LOW); the
     * task delays before rebooting, so the response still goes out first. */
    dram_watch_log_task("recovery_exit", "before-create");
    bool task_started = dram_watch_task_after("recovery_exit",
                                              xTaskCreate(ota_recovery_exit_reboot_task, "recovery_exit_reboot", 2048, NULL,
                                                          tskIDLE_PRIORITY + 1, &s_recovery_exit_reboot_task)) == pdPASS;
    /* Registered unconditionally, success or not -- stack_margin_register()
     * reads *task_handle_slot fresh at report time, so a creation failure
     * just reads back alive=false rather than needing a second branch here.
     * 2048 must match the xTaskCreate() literal above. Label shortened to
     * "recovery_exit" (not the full "recovery_exit_reboot" FreeRTOS task
     * name, unchanged above) -- the full name is 20 chars and
     * STACK_MARGIN_NAME_MAX (20) leaves only 19 usable, which would
     * silently truncate it to "recovery_exit_rebo". */
    stack_margin_register("recovery_exit", &s_recovery_exit_reboot_task, 2048);
    if (!task_started) {
        ESP_LOGE(OTA_HTTP_TAG, "recovery-mode exit: failed to start the reboot task -- board will NOT "
                      "reboot; the counter is already cleared, retry or power-cycle");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"failed to start the reboot task; the board was not rebooted\"}");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"status\":\"rebooting\"}");
    return ESP_OK;
}


// --- POST /api/ota/esp/recovery_boot -- deliberate entry into the recovery
// image (docs/OTA_SINGLE_SLOT.md section 4, "Deliberate entry into
// recovery"; section 7's ota_rollback_esp() -> ota_recovery_boot_esp() row).
// With one OTA slot, POST /api/ota/esp/rollback has nothing to roll back to
// (esp_ota_check_rollback_is_possible() is false); this is the replacement
// way to reach the image that can take a fresh push.
//
// Mechanism is stock IDF and nothing else: esp_ota_set_boot_partition() on
// the `factory` (recovery) partition. IDF first runs a FULL image verify on
// that partition (checksum + hash) and returns ESP_ERR_OTA_VALIDATE_FAILED
// without writing anything if it is not a bootable image; only then does it
// erase the two otadata sectors, so the bootloader falls through to factory.
// No otadata blob is hand-written. Failure modes: a power cut during that
// erase leaves either both sectors blank (bootloader picks factory) or one
// still-valid sector (bootloader picks the app again) -- both bootable.
//
// Done synchronously in the handler, BEFORE the 200 is sent, so "rebooting
// into recovery" is only ever claimed once the boot target is actually set
// and verified. The reboot itself runs on a short task, same pattern as
// ota_rollback_reboot_task(), so the response can leave first.
//
// Refusals, in order: (1) ADMIN tier (route_tier_table.h) -- auth is the
// table's, nothing in here. (2) system_mode_gate SYS_ACTION_RECOVERY_BOOT:
// 409 while a firing/autotune runs (PAUSED included) or while any relay is
// energized/unreadable. (3) ota_http_check_interlocks(): the same
// idle/cool/link gate every OTA-family route uses (428 for the
// no-safety-processor acknowledgement). (4) the single update mutex, taken
// BEFORE the authoritative relay read below -- while the claim is held,
// heat_interlock refuses every heat path (profile start, autotune start,
// manual relay-on), so "relays off" cannot go false again between that
// read and the reboot. (5) the running partition must not itself be the
// factory partition (on a pre-migration table `factory` IS the full
// application, and booting "recovery" would just re-boot the same image).
// The claim is intentionally left held across the reboot, as the rollback
// route does; a fresh boot starts with it released.
//
// If the reboot task cannot be started after the boot target was set, the
// target is put back on the running partition so the board does not
// silently land in recovery on its next unrelated reset.

// File-scope (not handler-local) so the task below can null it before
// deleting. stack_margin_register() target.
static TaskHandle_t s_recovery_boot_reboot_task;

static void ota_recovery_boot_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    // Best-effort, same as ota_rollback_reboot_task(): a failed send only
    // means SaftyFW's S6b may nuisance-trip, which is the expected outcome
    // of leaving the application anyway.
    esp_err_t announce_err = safety_link_send_announce_reboot(ota_http_safety);
    if (announce_err != ESP_OK) {
        ESP_LOGW(OTA_HTTP_TAG, "recovery_boot: safety_link_send_announce_reboot failed (%s) -- rebooting anyway",
                 esp_err_to_name(announce_err));
    }
    ESP_LOGW(OTA_HTTP_TAG, "recovery_boot: rebooting into the recovery image now");
    /* Internal-RAM stack required: esp_restart() disables the flash cache --
     * see ota_recovery_exit_reboot_task() above. */
    hal_wdt_reboot();
    s_recovery_boot_reboot_task = NULL; /* see ota_recovery_exit_reboot_task() on why null-then-delete */
    vTaskDelete(NULL);
}

esp_err_t ota_recovery_boot_post_handler(httpd_req_t *req)
{
    char ip[46];
    ota_http_get_client_ip(req, ip, sizeof(ip)); /* logging only -- ADMIN tier (route_tier_table.h) is the only gate */

    // 2. system_mode_gate first (same order as every other wired HTTP call
    // site: this gate, then ota_http_check_interlocks()).
    {
        sys_mode_snapshot_t mode_snap = { 0 };
        relay_authority_heat_run_active(&mode_snap.profile_running, &mode_snap.autotune_running);
        mode_snap.relays_energized = ota_http_any_relay_energized();
        char mode_reason[SYSTEM_MODE_GATE_REASON_MAX];
        mode_reason[0] = '\0';
        if (system_mode_gate_check(SYS_ACTION_RECOVERY_BOOT, &mode_snap, mode_reason, sizeof(mode_reason))) {
            ESP_LOGW(OTA_HTTP_TAG, "recovery_boot from %s: refused by system mode gate: %s", ip, mode_reason);
            return system_mode_gate_http_send_refusal(req, mode_reason);
        }
    }

    // 3. Same interlock as every OTA-family route.
    char reason[OTA_INTERLOCK_REASON_MAX];
    ota_interlock_result_t gate = ota_http_check_interlocks(ota_http_req_ack_no_safety(req), reason,
                                                            sizeof(reason));
    if (gate != OTA_INTERLOCK_OK) {
        ESP_LOGW(OTA_HTTP_TAG, "recovery_boot from %s: refused by interlock: %s", ip, reason);
        return ota_http_send_interlock_refusal(req, gate, reason);
    }

    // 4. Single update mutex (claimed as ESP, like the rollback route).
    if (!ota_http_update_try_begin(OTA_HTTP_CONTEXT_ESP)) {
        ESP_LOGW(OTA_HTTP_TAG, "recovery_boot from %s: refused, an update is already in progress", ip);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "an update is already in progress", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Authoritative relay read, now that the claim blocks every heat path.
    if (ota_http_any_relay_energized()) {
        ESP_LOGW(OTA_HTTP_TAG, "recovery_boot from %s: refused, a relay is on or unreadable", ip);
        ota_http_update_end();
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "a relay is on or its state is unreadable; relays must be off before recovery",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // 5. Verify the recovery image, then point the bootloader at it. The
    // verify-then-set helper is shared with the boot_guard threshold path
    // (recovery_switch.h); it refuses a missing/factory-running layout and an
    // image that does not verify, writing nothing in either case. SET_FAILED
    // is different: esp_ota_set_boot_partition() may already have erased
    // otadata, so that case restores the running image's boot target.
    char sel_msg[96];
    recovery_switch_result_t sel = recovery_switch_select_boot(sel_msg, sizeof(sel_msg));
    if (sel == RECOVERY_SWITCH_SET_FAILED) {
        const bool restored = recovery_switch_restore_running();
        ESP_LOGE(OTA_HTTP_TAG, "recovery_boot from %s: selecting recovery failed (%s) -- boot target restore %s",
                 ip, sel_msg, restored ? "succeeded" : "FAILED (the next reset may boot recovery)");
        ota_http_update_end();
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req,
                        restored ? "selecting recovery failed part-way; the boot target was restored to the running image, NOT rebooting"
                                 : "selecting recovery failed part-way and restoring the boot target FAILED; the next reset may boot recovery",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (sel != RECOVERY_SWITCH_OK) {
        ESP_LOGW(OTA_HTTP_TAG, "recovery_boot from %s: refused: %s -- NOT rebooting", ip, sel_msg);
        ota_http_update_end();
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, sel_msg, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    hal_sysinfo_build_info_t running_build;
    hal_sysinfo_get_build_info(&running_build);
    const char *version_before = (running_build.valid && running_build.version[0]) ? running_build.version : "";
    {
        ota_record_t rec;
        ota_record_fill(&rec, (uint32_t)(hal_time_now_us() / 1000000), "esp", version_before, "", true,
                        "recovery boot requested", NULL);
        ota_record_append(&rec); // best-effort, logs its own failure
    }

    // Create the task BEFORE sending the response so a failure can still be
    // reported honestly and the boot target reverted.
    dram_watch_log_task("recovery_boot", "before-create");
    if (dram_watch_task_after("recovery_boot",
                              xTaskCreate(ota_recovery_boot_reboot_task, "recovery_boot", 3072, NULL,
                                          tskIDLE_PRIORITY + 1, &s_recovery_boot_reboot_task)) != pdPASS) {
        ESP_LOGE(OTA_HTTP_TAG, "recovery_boot from %s: failed to start the reboot task -- restoring the "
                      "boot target to the running image", ip);
        if (!recovery_switch_restore_running()) {
            ESP_LOGE(OTA_HTTP_TAG, "recovery_boot: restoring the boot target failed -- the NEXT reset "
                          "will boot recovery");
        }
        ota_http_update_end();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not start the reboot task");
        return ESP_OK;
    }
    stack_margin_register("recovery_boot", &s_recovery_boot_reboot_task, 3072); /* 3072 matches xTaskCreate above */

    ESP_LOGW(OTA_HTTP_TAG, "recovery_boot from %s: accepted, was running '%s' -- rebooting into recovery", ip,
             version_before[0] ? version_before : "(unknown version)");
    char body[112];
    int n = snprintf(body, sizeof(body),
                     "{\"ok\":true,\"status\":\"rebooting\",\"target\":\"recovery\",\"version_before\":\"%s\"}",
                     version_before);
    httpd_resp_set_type(req, "application/json");
    ota_http_send_json_clamped(req, body, n, sizeof(body));
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
    ota_http_get_client_ip(req, ip, sizeof(ip)); /* logging only -- ADMIN tier (route_tier_table.h) is the only gate, AP-password HMAC retired 2026-09-29 */

    bool verified = boot_guard_reset_counter();
    /* persisted_count is read AFTER the reset attempt so it reflects what is
     * actually in NVS now (0 on a verified clear) -- boot_count alone (this
     * boot's fixed in-RAM count, e.g. 1) never changes across this call and
     * was the whole reason this route's response used to read as a no-op
     * even when the clear worked; see boot_guard_get_persisted_count()'s doc
     * comment. */
    uint32_t persisted = 0;
    bool have_persisted = boot_guard_get_persisted_count(&persisted);
    char json[128];
    int n;
    if (have_persisted) {
        n = snprintf(json, sizeof(json), "{\"ok\":%s,\"boot_count\":%lu,\"persisted_count\":%lu}",
                      verified ? "true" : "false", (unsigned long)boot_guard_get_boot_count(),
                      (unsigned long)persisted);
    } else {
        n = snprintf(json, sizeof(json), "{\"ok\":%s,\"boot_count\":%lu}",
                      verified ? "true" : "false", (unsigned long)boot_guard_get_boot_count());
    }
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
    uint32_t persisted = 0;
    bool have_persisted = boot_guard_get_persisted_count(&persisted);
    char json[128];
    int n;
    if (have_persisted) {
        n = snprintf(json, sizeof(json),
                      "{\"boot_count\":%lu,\"recovery_mode\":%s,\"persisted_count\":%lu}",
                      (unsigned long)boot_guard_get_boot_count(),
                      boot_guard_is_recovery_mode() ? "true" : "false",
                      (unsigned long)persisted);
    } else {
        n = snprintf(json, sizeof(json), "{\"boot_count\":%lu,\"recovery_mode\":%s}",
                      (unsigned long)boot_guard_get_boot_count(),
                      boot_guard_is_recovery_mode() ? "true" : "false");
    }
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
// directly -- so gating this endpoint behind an ADMIN-tier login (which
// would force the web page to ask for a password just to show "kiln is
// running a profile" before the file picker even appears) would not close
// any exposure that isn't already open on this same LAN. Unauthenticated
// here, matching /api/status's existing exposure level, not a new one.
// Returns {"ok":true} or {"ok":false,"reason":"<why>"}.
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

