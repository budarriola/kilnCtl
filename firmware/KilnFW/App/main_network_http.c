/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/* ROADMAP.md M15 1500-line item: main.c split by boot phase, 2026-09-04.
 * This file is phase 3 of app_main(): the "network+HTTP" stretch --
 * dashboard/board-temps/settings/zones/profiles/factory-reset/readiness/
 * log/adaptive-tune/diagnostics/partition-info/settings/ota/kiln-cfg/
 * backup/safety-cfg/sim HTTP registration, the monitor task, and the PC-link
 * UART owner+protocol stack -- ending on the "heap stage uart_owner+proto"
 * checkpoint. See main_internal.h for the shared context struct and main.c
 * for app_main() itself, which calls main_network_http_bringup() after
 * main_control_bringup(). */

#include <stdlib.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "boot_guard.h"
#include "dashboard_http.h"
#include "board_temps.h"
#include "board_temps_http.h"
#include "diagnostics_http.h"
#include "partition_info_http.h"
#include "backup_http.h"
#include "settings_http.h"
#include "factory_reset.h"
#include "kiln_io.h"
#include "kiln_io_owner.h"
#include "monitor_task.h"
#include "nvs_report.h"
#include "ota_http.h"
#include "profile_executor.h"
#include "profiles_builtin.h"
#include "ramp_assist_cfg.h"
#include "unit_pref.h"
#include "profiles_http.h"
#include "log_http.h"
#include "adaptive_tune.h"
#include "adaptive_tune_http.h"
#include "readiness_http.h"
#include "safety_link.h"
#include "sim_backend.h"
#include "stack_margin.h"
#include "uart_owner.h"
#include "uart_protocol.h"
#include "kiln_cfg_http.h"
#include "safety_cfg_http.h"
#include "safety_cfg_store.h"
#include "kiln_cfg_store.h"
#include "zones_http.h"

#include "main_internal.h"

/* --- OTA rollback confirmation (UPDATE_PROTOCOL.md sec 3, TODO.md 9.2) ----
 *
 * With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y, an image that just got OTA'd
 * in boots as PENDING_VERIFY and the bootloader will revert to the previous
 * slot on the next boot unless esp_ota_mark_app_valid_cancel_rollback() has
 * been called. UPDATE_PROTOCOL.md is emphatic, twice, that this must NOT be
 * called at the end of app_main() -- reaching the last line of main proves
 * nothing about whether the things that matter actually came up.
 *
 * So this runs as its own low-priority task rather than being invoked inline
 * from app_main() at a fixed point, and defers the actual "is this healthy"
 * decision to boot_confirm_is_healthy() (boot_guard.h) -- the SAME function
 * used a few lines below to decide whether to clear boot_guard's recovery
 * counter. See that function's doc comment for the full story, but the short
 * version: this used to ALSO require safety_link_get_status()->link_up, live,
 * on every poll -- and on a board with no RP2040 attached or answering (this
 * project's own bench board, right now), that condition is never true, so
 * the image never confirmed and every OTA update silently reverted on the
 * next reset. Confirmed on hardware 2026-08-22: "OTA rollback not yet
 * confirmed ... safety_link_up=0" logged forever, then rollback on the next
 * boot. A missing safety processor is a real, already-acknowledged condition
 * elsewhere in this codebase (ota_interlock.h's
 * OTA_INTERLOCK_REFUSED_NEEDS_ACK) -- it must not ALSO silently undo the
 * operator's own OTA update.
 *
 * nvs_ok/web_ok/ota_ok are booleans captured once, from state app_main
 * already established (nvs_report_get()'s mounted flags,
 * dashboard_http_start()'s and ota_http_start()'s return codes) -- all three
 * fixed at boot, so if boot_confirm_is_healthy() is false on the first poll
 * it stays false for the life of this boot and the image is correctly left
 * PENDING_VERIFY (that is the rollback doing its job, not a bug). The safety
 * link's live up/down state is still read and logged here -- loudly, when
 * down -- purely as an informational side note; it is NOT part of the
 * confirm/clear decision itself. `safety` may be NULL if safety_link_start()
 * failed this boot, same fail-closed convention as everywhere else in this
 * file; the log below tolerates that.
 *
 * FACTORY VS. OTA-SLOT BOOTS (2026-08-24) ---------------------------------
 * esp_ota_mark_app_valid_cancel_rollback() only means something when running
 * from an OTA slot (ota_0/ota_1) that the bootloader put into PENDING_VERIFY.
 * partitions.csv places `factory` at 0x810000, and the JTAG flash path this
 * project uses on the bench (flash_firmware in tools/PcTools: bootloader @0x0,
 * partition table @0x8000, app @0x810000) writes every bench-flashed build
 * into THAT partition, not an OTA slot -- there is no rollback to cancel, and
 * the call reliably returns ESP_FAIL. That used to be logged as two ERRORs on
 * every single boot ("esp_ota_ops: Running firmware is factory" from IDF
 * itself, plus this file's own ESP_LOGE on the ESP_FAIL) despite being
 * entirely expected, which is real damage on a board whose whole diagnostic
 * story is "read the device log" -- it trains the reader to ignore ERROR
 * lines. This task now checks esp_ota_get_running_partition() once and routes
 * the decision through boot_confirm_decide() (boot_guard.h): a healthy
 * factory boot logs an explanatory INFO line and skips the rollback-cancel
 * call entirely (but still calls boot_guard_mark_healthy() -- that counter's
 * job does not depend on which partition type is running, and skipping it on
 * every factory boot would walk an otherwise-healthy bench board into
 * recovery mode after RECOVERY_MODE_BOOT_THRESHOLD reboots for no real
 * reason). A healthy OTA-slot boot keeps doing exactly what this file always
 * did, byte for byte, including the safety-link caveat warning below. */
typedef struct {
    SafetyLinkClass *safety; /* NULL if safety_link_start() failed this boot -- logged, not gating */
    bool nvs_ok;
    bool web_ok;
    bool ota_ok; /* ota_http_start() succeeded -- the OTA routes this whole recovery mechanism needs exist */
} main_ota_confirm_ctx_t;

#define MAIN_OTA_CONFIRM_POLL_MS   500
#define MAIN_OTA_CONFIRM_WARN_MS   10000

static void main_ota_rollback_confirm_task(void *arg)
{
    main_ota_confirm_ctx_t ctx = *(main_ota_confirm_ctx_t *)arg;
    free(arg);

    // Fixed for the life of this boot -- the running partition cannot change
    // underneath a running image. Determined once, outside the poll loop, so
    // every iteration reuses the same answer rather than re-querying it.
    const esp_partition_t *running    = esp_ota_get_running_partition();
    bool                   is_factory = running && running->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY;
    if (running) {
        ESP_LOGI(MAIN_TAG, "running partition: '%s' (subtype 0x%02x)", running->label, running->subtype);
    } else {
        // esp_ota_get_running_partition() returning NULL is not documented to
        // happen in practice, but treat it the same as "not factory" rather
        // than crash on a NULL deref -- boot_confirm_decide() then takes the
        // OTA-slot branch, which is this file's pre-existing behavior.
        ESP_LOGW(MAIN_TAG, "esp_ota_get_running_partition() returned NULL -- assuming an OTA slot");
    }

    TickType_t start = xTaskGetTickCount();
    bool       warned = false;

    for (;;) {
        bool link_up = false;
        if (ctx.safety) {
            safety_link_status_t st;
            if (safety_link_get_status(ctx.safety, &st) == ESP_OK) {
                link_up = st.link_up;
            }
        }

        boot_confirm_action_t action =
            boot_confirm_decide(is_factory, ctx.nvs_ok, ctx.web_ok, ctx.ota_ok);

        if (action == BOOT_CONFIRM_SKIP_FACTORY) {
            ESP_LOGI(MAIN_TAG, "running from the factory partition -- OTA rollback confirmation does "
                          "not apply here (there is no PENDING_VERIFY slot to cancel; "
                          "esp_ota_mark_app_valid_cancel_rollback() only means something after a "
                          "real OTA into ota_0/ota_1). NVS, web server and OTA routes are healthy, "
                          "so boot_guard's counter is still cleared below.");
            boot_guard_mark_healthy();
            vTaskDelete(NULL);
            return;
        }

        if (action == BOOT_CONFIRM_CONFIRM_OTA_SLOT) {
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            if (err == ESP_OK) {
                ESP_LOGI(MAIN_TAG, "OTA rollback confirmed: NVS readable, web server and OTA routes up "
                              "-- this image is no longer PENDING_VERIFY");
            } else {
                ESP_LOGE(MAIN_TAG, "esp_ota_mark_app_valid_cancel_rollback failed: %s "
                              "(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE off, or not an OTA slot?)",
                         esp_err_to_name(err));
            }
            if (!link_up) {
                /* Degraded-but-recorded, per boot_guard.h's doc comment: this
                 * confirmation did NOT wait for a live safety-link frame
                 * exchange. Announced loudly rather than silently, so a board
                 * that is SUPPOSED to have a safety processor answering does
                 * not have that fact buried in an INFO line. */
                ESP_LOGW(MAIN_TAG, "OTA rollback confirmed WITHOUT a live safety-link check "
                              "(safety_link_up=0) -- if this board is expected to have a safety "
                              "processor answering, that is a separate problem worth investigating");
            }
            /* Same predicate as the rollback decision just above, by
             * construction (boot_confirm_is_healthy() was already
             * satisfied) -- see boot_guard.h's doc comment for why these two
             * must never be allowed to disagree again. */
            boot_guard_mark_healthy();
            vTaskDelete(NULL);
            return;
        }

        if (!warned && (xTaskGetTickCount() - start) > pdMS_TO_TICKS(MAIN_OTA_CONFIRM_WARN_MS)) {
            ESP_LOGW(MAIN_TAG, "OTA rollback not yet confirmed %lu ms after boot: nvs_ok=%d web_ok=%d "
                          "ota_ok=%d (safety_link_up=%d, informational only) -- image stays "
                          "PENDING_VERIFY until nvs/web/ota are all true",
                     (unsigned long)MAIN_OTA_CONFIRM_WARN_MS, (int)ctx.nvs_ok, (int)ctx.web_ok,
                     (int)ctx.ota_ok, (int)link_up);
            warned = true;
        }

        vTaskDelay(pdMS_TO_TICKS(MAIN_OTA_CONFIRM_POLL_MS));
    }
}

void main_network_http_bringup(main_boot_ctx_t *ctx)
{
    // --- Dashboard HTTP API (live status + manual relay control) -----------
    // Registers on the httpd instance wifi_prov_start() already brought up
    // (main_boot_early.c) -- if that failed (no Wi-Fi this boot), this just
    // logs and is skipped like every other bring-up step here; a dashboard
    // nobody can reach over Wi-Fi is not a reason to fail app_main. Reads the
    // same kio/thermo_bus this file owns rather than duplicating them, and
    // reuses the safety pointer for the same relay_authority gate the UART
    // bridge uses below.
    ctx->dash_err = dashboard_http_start(ctx->io_ready ? &ctx->kio : NULL,
                                          ctx->thermo_bus.initialized ? &ctx->thermo_bus : NULL,
                                          ctx->safety_err == ESP_OK ? &ctx->safety : NULL);
    if (ctx->dash_err != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "dashboard_http_start failed: %s -- no dashboard this boot",
                 esp_err_to_name(ctx->dash_err));
    }

    // TODO.md 10.7: board-health IC temperature JSON, deliberately its own
    // route rather than folded into /api/status above -- kiln-process temp
    // and board-health temp are different audiences and mixing them was
    // exactly what 10.7 said to avoid. Same thermo_bus pointer as
    // dashboard_http_start() just above (this module borrows it to call
    // MAX31856_read_all() itself per request rather than owning the bus);
    // esp32_c is independent of thermo_bus and already came up in
    // board_temps_start() (main_boot_early.c).
    esp_err_t board_temps_http_err = board_temps_http_start(ctx->thermo_bus.initialized ? &ctx->thermo_bus : NULL);
    if (board_temps_http_err != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "board_temps_http_start failed: %s -- no /api/board_temps or /board_temps "
                      "page this boot", esp_err_to_name(board_temps_http_err));
    }

    // --- Settings/Profiles HTTP pages (TODO.md sections 3 and 5) -----------
    // Pure config CRUD -- none of these take hardware pointers, they only
    // need the shared httpd instance above. Each failure is logged and
    // skipped, same non-fatal convention as dashboard_http_start: a missing
    // settings page is never a reason to fail app_main or touch the
    // control/safety path.
    // 2026-08-21, ROADMAP.md "a real shared temperature-unit setting":
    // dashboard_http_start() was already called above this block, but it
    // only registers the handler here -- it does not read unit_pref_get()
    // until the first request actually arrives (dashboard_get_status()),
    // and the LCD's first redraw tick is likewise well after app_main
    // returns, so loading the preference here (before either can be polled)
    // is still in time for both. Same non-fatal convention as every module
    // in this block: a failed load means Celsius for this boot only, never a
    // reason to fail app_main.
    esp_err_t unit_pref_err = unit_pref_start();
    if (unit_pref_err != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "unit_pref_start failed: %s -- defaulting to Celsius this boot",
                 esp_err_to_name(unit_pref_err));
    }

    // 2026-09-02, forthcoming "ramp assist" feature: same non-fatal, load-
    // before-first-poll placement as unit_pref_start() just above -- a
    // failed load leaves ramp_assist_cfg_enabled() at its safe default
    // (disabled) for this boot only, never a reason to fail app_main.
    esp_err_t ramp_assist_err = ramp_assist_cfg_start();
    if (ramp_assist_err != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "ramp_assist_cfg_start failed: %s -- ramp assist stays disabled this boot",
                 esp_err_to_name(ramp_assist_err));
    }

    esp_err_t zones_err = zones_http_start();
    if (zones_err != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "zones_http_start failed: %s -- no Thermocouples & Zones page this boot",
                 esp_err_to_name(zones_err));
    }
    // PID_EXPANSION_PLAN.md 3.3, U2: adaptive_tune.c's per-zone opt-in flags
    // now live in the zone config blob zones_http_start() just loaded (or
    // failed to -- either way, s_zones.cfg is in a defined, safe-default
    // state by the time this returns, same as every other module's non-
    // fatal load above). MUST run after zones_http_start(), never before --
    // adaptive_tune_init() (profile_executor_start(), main_control_bringup.c)
    // runs too early for it; see adaptive_tune_load_enable_flags()'s own
    // comment for the ordering evidence. Non-fatal like every settings-load
    // call in this block: this function has no failure mode that stops
    // app_main, only zones left at their struct-zero (opted-out) default if
    // something upstream never loaded.
    adaptive_tune_load_enable_flags();
    // 2026-08-27+2 (Tasks 1/2/3): the per-zone current sweep, the runtime
    // CT-to-zone mapping check, and the read-only safety-processor wiring
    // display all need real hardware, which zones_http_start() itself
    // deliberately does not take (pure config CRUD -- see its own comment).
    // Same io/thermo_bus/safety pointers as dashboard_http_start()/
    // ota_http_start() above, same NULL-tolerant convention: called
    // unconditionally, even if zones_http_start() itself failed to register
    // its HTTP routes, so the underlying state (sweep refusal, safety
    // wiring) is still correct for whichever caller (MCP, a future retry)
    // reaches it.
    zones_http_set_hw(ctx->io_ready ? &ctx->kio : NULL, ctx->thermo_bus.initialized ? &ctx->thermo_bus : NULL,
                      ctx->safety_err == ESP_OK ? &ctx->safety : NULL);
    // The shipped Digital Fire schedule catalogue's persisted hidden-mask.
    // Must load before profiles_http_start() registers the read paths that
    // consult it, or the first listing after boot would show hidden entries.
    // Non-fatal like every settings module here: a failed load means the
    // catalogue simply shows everything this boot.
    esp_err_t builtin_err = profiles_builtin_start();
    if (builtin_err != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "profiles_builtin_start failed: %s -- built-in schedules shown unfiltered this boot",
                 esp_err_to_name(builtin_err));
    }
    esp_err_t profiles_err = profiles_http_start();
    if (profiles_err != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "profiles_http_start failed: %s -- no Profiles page this boot",
                 esp_err_to_name(profiles_err));
    }

    // TODO.md 8.1: the explicit-scope reset/factory-default endpoint. Only
    // needs the shared httpd instance -- no hardware pointers, same as the
    // settings pages just above -- and is likewise never a reason to fail
    // app_main.
    esp_err_t factory_reset_err = factory_reset_http_start();
    if (factory_reset_err != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "factory_reset_http_start failed: %s -- no reset/factory-default endpoint this boot",
                 esp_err_to_name(factory_reset_err));
    }

    // TODO.md 8.2's boot-time report: capture AFTER every module above that
    // owns an NVS partition (wifi_prov_start() in main_boot_early.c,
    // relay_cycles_init(), zones/profiles_http_start() just above) has
    // already run its own nvs_partition_init() -- this only observes what
    // those calls established, it does not itself mount or erase anything.
    nvs_report_capture();

    // TODO.md 8.2's boot-time report -- kept here (its original spot); the
    // OTA rollback confirmation task that used to be kicked off right after
    // it now starts further below, after ota_http_start(), because it needs
    // that call's result too (see boot_confirm_is_healthy()/ota_ok).

    // TODO.md 8.3: the "is this kiln ready to fire?" status page. Read-only
    // aggregator over the getters every module above already exposes --
    // registered last among the settings pages so every fact it can report
    // (zones_config_is_valid(), nvs_report_get(), dashboard_http_get_hw_ready(),
    // etc.) reflects this boot's actual state rather than a partial one.
    esp_err_t readiness_err = readiness_http_start();
    if (readiness_err != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "readiness_http_start failed: %s -- no readiness page this boot",
                 esp_err_to_name(readiness_err));
    }

    // log_http.c: read-back for the firing/autotune logs log_store_mount.c
    // persists to flash (GET /api/logs/firing, GET /api/logs/autotune).
    // Registered here alongside the other read-only status pages -- no
    // ordering dependency on anything else, and it lives in its own file
    // rather than dashboard_http.c (owned by parallel work at the time this
    // was added).
    esp_err_t log_http_err = log_http_start();
    if (log_http_err != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "log_http_start failed: %s -- no /api/logs/* endpoints this boot",
                 esp_err_to_name(log_http_err));
    }

    // adaptive_tune_http.c: GET /api/adaptive_tune (status, all zones), POST
    // /api/adaptive_tune/enable -- the operator's only way to reach Phase 7d
    // continuous tuning (adaptive_tune.c). Registered here, after profile_
    // executor_start() (main_control_bringup.c) has already called
    // adaptive_tune_init() to load the persisted opt-in mask, and alongside
    // the other read-mostly status APIs -- no ordering dependency beyond the
    // shared httpd server already being up, same as log_http_start() just
    // above.
    esp_err_t adaptive_tune_http_err = adaptive_tune_http_start();
    if (adaptive_tune_http_err != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "adaptive_tune_http_start failed: %s -- no /api/adaptive_tune* endpoints this boot",
                 esp_err_to_name(adaptive_tune_http_err));
    }

    // UI_PLAN.md "page structure rework" section: /diagnostics, /diagnostics/
    // thermo and /safety -- the four LCD pages (diagnostics + board health
    // merged into one web page, thermo faults, safety) that had no web
    // equivalent at all until this pass. Takes no hardware pointers -- these
    // are static pages that poll the existing GET /api/status and
    // GET /api/board_temps endpoints client-side, same "pure page, no
    // server-side data gathering of its own" shape as readiness_http_start()
    // just above. Registered right after readiness for the same reason: no
    // ordering dependency on anything below it, so placement only matters for
    // reading this boot sequence top-to-bottom.
    esp_err_t diagnostics_err = diagnostics_http_start();
    if (diagnostics_err != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "diagnostics_http_start failed: %s -- no /diagnostics, /diagnostics/thermo "
                      "or /safety page this boot", esp_err_to_name(diagnostics_err));
    }

    // FLASH_BUDGET_PLAN.md section 8 item 3: GET /api/partitions reports
    // the live partition table this running app is actually using (see
    // partition_info_http.h's header comment for why this replaced a JTAG
    // flash read at 0x8000, which does not work on this chip). No hardware
    // pointers needed, registered right after diagnostics for the same
    // "no ordering dependency" reason as everything else in this block.
    esp_err_t partition_info_err = partition_info_http_start();
    if (partition_info_err != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "partition_info_http_start failed: %s -- no /api/partitions this boot",
                 esp_err_to_name(partition_info_err));
    }

    // UI_PLAN.md "web page structure" section, items 2-4: the settings hub
    // and the manual-relay page that finally split main_page.html apart --
    // that file kept its own inline Settings block, Danger zone, and
    // per-relay toggles even after the diagnostics/safety pages above
    // shipped. Same "pure page, no server-side data gathering of its own"
    // shape as diagnostics_http_start() just above, registered right after
    // it for the same reason (no ordering dependency on anything below).
    esp_err_t settings_err = settings_http_start();
    if (settings_err != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "settings_http_start failed: %s -- no /settings or "
                      "/settings/display page this boot", esp_err_to_name(settings_err));
    }

    // CommonFW/docs/UPDATE_PROTOCOL.md section 2 + section 1 / TODO.md 9.4:
    // GET /api/ota/challenge, plus the interlock check and update mutex
    // (ota_interlock.{h,c}, ota_http_check_interlocks()/_update_try_begin()/
    // _update_end()). Auth logic (nonce lifecycle, lockout) and the
    // interlock precondition logic are both real and host-tested
    // (App/test/test_ota_auth.c, App/test/test_ota_interlock.c); the
    // streamed OTA upload handlers themselves (POST /api/ota/esp,
    // POST /api/ota/pico) are not built yet -- ota_http_verify_request()
    // and ota_http_check_interlocks() are exposed for whichever future pass
    // adds them. Same io/thermo_bus/safety pointers as
    // dashboard_http_start() just above, for the same reason: the
    // interlock's per-zone checks read hardware state directly rather than
    // through profile_executor, so they see the truth whether or not a
    // profile happens to be running.
    esp_err_t ota_http_err = ota_http_start(ctx->io_ready ? &ctx->kio : NULL,
                                            ctx->thermo_bus.initialized ? &ctx->thermo_bus : NULL,
                                            ctx->safety_err == ESP_OK ? &ctx->safety : NULL);
    if (ota_http_err != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "ota_http_start failed: %s -- no /api/ota/challenge this boot",
                 esp_err_to_name(ota_http_err));
    }

    // TODO.md 9.2 / UPDATE_PROTOCOL.md sec 3: kick off OTA rollback
    // confirmation now that NVS's mounted state, the web server's start
    // result, AND ota_http_start()'s own result all exist to capture. See
    // main_ota_rollback_confirm_task() above for why this is a background
    // poller and not an inline call here, and boot_confirm_is_healthy()
    // (boot_guard.h) for what "healthy" means as of this pass. Moved here
    // (from right after nvs_report_capture(), a few hundred lines above)
    // specifically because ota_ok needs ota_http_err, which does not exist
    // until this line.
    {
        size_t                       nvs_section_count = 0;
        const nvs_report_section_t *nvs_sections = nvs_report_get(&nvs_section_count);
        bool                         nvs_ok = (nvs_section_count > 0);
        for (size_t i = 0; i < nvs_section_count; i++) {
            if (!nvs_sections[i].mounted) {
                nvs_ok = false;
            }
        }

        main_ota_confirm_ctx_t *ota_ctx = calloc(1, sizeof(*ota_ctx));
        if (ota_ctx) {
            ota_ctx->safety = (ctx->safety_err == ESP_OK) ? &ctx->safety : NULL;
            ota_ctx->nvs_ok = nvs_ok;
            ota_ctx->web_ok = (ctx->dash_err == ESP_OK);
            ota_ctx->ota_ok = (ota_http_err == ESP_OK);
            if (xTaskCreate(main_ota_rollback_confirm_task, "ota_confirm", 3072, ota_ctx,
                             tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
                ESP_LOGE(MAIN_TAG, "Failed to start OTA rollback confirmation task -- this image "
                              "will stay PENDING_VERIFY for the rest of this boot, and boot_guard's "
                              "counter will not be cleared this boot either");
                free(ota_ctx);
            }
        } else {
            ESP_LOGE(MAIN_TAG, "OTA rollback confirmation context alloc failed -- this image "
                          "will stay PENDING_VERIFY for the rest of this boot, and boot_guard's "
                          "counter will not be cleared this boot either");
        }
    }

    // Rule evaluator (rules_task.c/rules_http.c) was deleted 2026-08-27:
    // relay/IO control moved into firing profiles as segments (see
    // profile_executor.c's io_seg_* machinery and its guard-9 watchdog,
    // which now covers the stale-tick force-off this task used to do) per
    // the owner's "instead of the relays and rules section I want them to be
    // part of the profile" request.

    // Owner-report (2026-08-21 follow-up): saved "kiln config" slots --
    // whole-zones-config snapshots, named, cloneable, switchable -- that
    // must survive a programming cycle like every other user-set parameter
    // here. kiln_cfg_store_init() must run AFTER zones_http_start() (already
    // called above): its boot-time active-config restore falls back to
    // whatever zones_http_start() already loaded on its own if nothing (or
    // an invalid something) is marked active, and that fallback is only
    // correct if a real zones config load already happened. kiln_cfg_http_start()
    // must run AFTER ota_http_start() just above: its apply handler calls
    // ota_http_check_interlocks(), which needs the io/thermo_bus/safety
    // pointers ota_http_start() just stashed -- same ordering reasoning
    // backup_http_start()'s own comment below gives for the identical call.
    esp_err_t kiln_cfg_store_err = kiln_cfg_store_init();
    if (kiln_cfg_store_err != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "kiln_cfg_store_init failed: %s -- saved kiln configs unavailable this boot",
                 esp_err_to_name(kiln_cfg_store_err));
    }
    esp_err_t kiln_cfg_http_err = kiln_cfg_http_start();
    if (kiln_cfg_http_err != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "kiln_cfg_http_start failed: %s -- no /api/kiln_configs this boot",
                 esp_err_to_name(kiln_cfg_http_err));
    }

    // TODO.md 0.5 / UI_PLAN.md's settings+profile import/export, unified into
    // one Backup & Restore page (backup_http.c). Registered AFTER
    // ota_http_start() just above, deliberately: its import handler calls
    // ota_http_check_interlocks() before writing anything, which reads the
    // io/thermo_bus/safety pointers ota_http_start() just stashed -- calling
    // it before that point would see stale/unset state.
    esp_err_t backup_err = backup_http_start();
    if (backup_err != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "backup_http_start failed: %s -- no /settings/backup page this boot",
                 esp_err_to_name(backup_err));
    }

    /* Safety-processor commissioning (SaftyFW/docs/COMMISSIONING.md). The
     * store is the ESP's CACHE of parameters that live on the Pico; the Pico's
     * own config_store is the source of truth, and the config_crc carried on
     * every FW_VERSION frame is what decides whether this cache is current.
     * Init before the HTTP half so a request arriving immediately after
     * registration finds a loaded (or honestly-empty) cache rather than
     * uninitialised state.
     *
     * `safety` may be NULL here -- safety_link_start() is allowed to fail this
     * boot (see its own error path above, which logs that the isolated fault
     * line is unavailable and carries on). safety_cfg_http_start() takes it as
     * link_or_null precisely so the page still serves in that case, showing
     * link_up=false and its cached values marked stale, instead of the route
     * silently not existing. A missing page and a dead link look identical
     * from a browser, and only one of them is the truth. */
    esp_err_t safety_cfg_store_err = safety_cfg_store_init();
    if (safety_cfg_store_err != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "safety_cfg_store_init failed: %s -- safety commissioning cache "
                      "unavailable this boot (values will refetch from the Pico)",
                 esp_err_to_name(safety_cfg_store_err));
    }
    esp_err_t safety_cfg_http_err =
        safety_cfg_http_start(ctx->safety_err == ESP_OK ? &ctx->safety : NULL);
    if (safety_cfg_http_err != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "safety_cfg_http_start failed: %s -- no /safety/commissioning this boot",
                 esp_err_to_name(safety_cfg_http_err));
    }

    // Development-only /api/sim (fault injection into the simulated plant).
    // Compiles to a no-op returning ESP_OK unless CONFIG_KILNCTL_SIM_PLANT --
    // there is no way to reach this endpoint from a production image.
    esp_err_t sim_err = sim_backend_register_http();
    if (sim_err != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "sim_backend_register_http failed: %s -- no /api/sim this boot",
                 esp_err_to_name(sim_err));
    }

    main_heap_stage("http_handlers");

    static monitor_task_t monitor;
    monitor_task_init(&monitor, &ctx->expander.owner.task_handle);
    if (monitor_task_start(&monitor) != pdPASS) {
        ESP_LOGE(MAIN_TAG, "Failed to start heartbeat monitor task");
    }

    // --- PC link -----------------------------------------------------------
    // NOTE (2026-08-19): this block used to `return;` from app_main on
    // either failure below, which silently skipped EVERY subsequent line in
    // this function -- including lvgl_port_start(). On a bench where the
    // PC<->ESP USB-serial UART is the thing that's actually broken (a known,
    // separate, already-documented fault -- see ROADMAP.md M1's bench-state
    // notes), that meant a dead PC cable also took down the local LCD UI,
    // which has no dependency on the PC link at all. A board that can't be
    // driven from a PC should still show its own screen; per this file's own
    // "non-fatal like everything else in app_main" convention (see the
    // Wi-Fi/mDNS/I2C comments in main_boot_early.c), a missing PC link is
    // exactly that kind of independent, non-fatal peripheral, not a reason
    // to abort boot. pc_link_ready gates only the uart_proto-dependent
    // bridge tasks in main_bridges_bringup.c; lvgl_port_start() and the
    // fail-safe link watchdog there do not take uart_proto and are
    // unaffected either way.
    ctx->pc_link_ready = false;
    esp_err_t uart_err = uart_owner_init(&ctx->uart_owner, UART_OWNER_PORT_NUM, UART_OWNER_TX_IO,
                                          UART_OWNER_RX_IO, UART_OWNER_BAUD_RATE,
                                          UART_OWNER_QUEUE_LEN, UART_OWNER_TASK_PRIORITY,
                                          UART_OWNER_STACK_SIZE, tskNO_AFFINITY);
    if (uart_err != ESP_OK) {
        ESP_LOGE(MAIN_TAG, "uart_owner_init failed: %s", esp_err_to_name(uart_err));
        /* No PC link will ever come up. Relays go to their safe state right
         * here rather than waiting on the link watchdog further down (which
         * needs a link to watch and will never get one) -- otherwise the
         * board sits with whatever the expander's power-on latch left
         * energized. Local UI/display bring-up continues below regardless. */
        main_kiln_enter_safe_state(ctx->io_ready ? &ctx->kio : NULL, &ctx->safety, ctx->safety_err == ESP_OK,
                              SAFETY_FAULT_SRC_PC_LINK | SAFETY_FAULT_SRC_APP,
                              "the PC link UART could not be opened");
    } else {
        /* TODO.md section 13: internal-only (plain xTaskCreatePinnedToCore(),
         * no MALLOC_CAP_SPIRAM) and a named candidate for the DRAM-trough
         * investigation. Registering here, against the SAME
         * &ctx->uart_owner.event_task_handle field uart_owner_init() just
         * filled in, not a copy -- see stack_margin.h's registration comment
         * for why that indirection matters. (2026-09-06 uart collapse:
         * uart_owner_task()/task_handle -- the request-queue worker -- were
         * deleted, since uart_owner_transfer() had zero real callers; only
         * the event task remains.) The safety-link UART also runs this same
         * uart_owner.c code (safety_link.c's uart_owner_init() call) but is
         * deliberately NOT registered here: same task name, different
         * instance, and disambiguating them needs its own naming scheme --
         * left for whoever picks that up next rather than silently
         * conflated with the PC-link pair below. */
        stack_margin_register("uart_owner_evt_task", &ctx->uart_owner.event_task_handle, UART_OWNER_STACK_SIZE);
    }

    if (uart_err == ESP_OK) {
        uart_err = uart_protocol_init(&ctx->uart_proto, &ctx->uart_owner, UART_PROTO_DEVICE_ESP,
                                       UART_PROTOCOL_TASK_PRIORITY, UART_PROTOCOL_STACK_SIZE,
                                       tskNO_AFFINITY);
        if (uart_err != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "uart_protocol_init failed: %s", esp_err_to_name(uart_err));
            /* Same reasoning as the uart_owner_init failure above: the port
             * exists but nothing can be addressed over it, so no host will
             * ever command these relays. */
            main_kiln_enter_safe_state(ctx->io_ready ? &ctx->kio : NULL, &ctx->safety, ctx->safety_err == ESP_OK,
                                  SAFETY_FAULT_SRC_PC_LINK | SAFETY_FAULT_SRC_APP,
                                  "the PC link protocol stack could not be started");
        } else {
            ctx->pc_link_ready = true;
            /* TODO.md section 13's third PC-link-side entry -- same
             * internal-only, plain xTaskCreatePinnedToCore() stack, same
             * "PC-link instance only, not the safety-link one" caveat as
             * uart_owner above. */
            stack_margin_register("uart_proto_rx", &ctx->uart_proto.rx_task_handle, UART_PROTOCOL_STACK_SIZE);
        }
    }

    main_heap_stage("uart_owner+proto");
}
