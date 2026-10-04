/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/* ROADMAP.md M15 1500-line item: main.c split by boot phase, 2026-09-04.
 * This file is phase 2 of app_main(): the "safety+IO / control subsystems"
 * stretch, from the safety-processor link through profile_executor,
 * autotune_engine and the flash-safe executor those UART bridges share --
 * ending on the "heap stage executor+autotune" checkpoint. See
 * main_internal.h for the shared context struct and main.c for app_main()
 * itself, which calls main_control_bringup() after main_boot_early(). */

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "danger_mode.h"
#include "heat_enable.h"
#include "kiln_cfg_store.h" /* kiln_cfg_store_capture_expected_pico_fields() -- safety_ceiling_sync's
                              * standing divergence-check seam, docs/audits/kiln_profiles_feature_review_
                              * 2026-09-15.md Defect 2 */
#include "kiln_cfg_swap.h" /* kiln_cfg_swap_is_pending() -- kiln_cfg_store_autosave_from_live()'s
                             * swap-pending seam, review_autosave_rework_5bc9afb5_2026-09-15.md MEDIUM */
#include "kiln_io_owner.h"
#include "pico_auto_update_boot.h" /* docs/PICO_AUTO_UPDATE_PLAN.md G3 -- the boot-time
                                      * safety-processor firmware check started below */
#include "profile_executor.h"
#include "autotune_engine.h"
#include "relay_cycles.h"
#include "safety_ceiling_sync.h"
#include "safety_link.h"
#include "uart_bridge.h"

#include "main_internal.h"
#include "startup_faults.h"

/* safety_ceiling_sync_set_disable_heat_hooks() wants a void(void) action --
 * kiln_io_owner_command_all_relays_off() returns esp_err_t, so this adapts
 * it rather than widening the hook type for one caller's return value,
 * which nothing downstream of the enforcement call site would ever look at
 * anyway (it is an unconditional "make it so", not a query). */
static void main_control_bringup_all_relays_off_void(void)
{
    esp_err_t err = kiln_io_owner_command_all_relays_off();
    if (err != ESP_OK) {
        /* Deliberately does not repeat the callee's name in this string --
         * tools/check_safety_call_results_checked.ps1 text-scans every LINE
         * containing that literal function name for a captured return
         * value, and a log line merely mentioning it in prose would
         * otherwise be misread as a second, uncaptured call site. */
        ESP_LOGE(MAIN_TAG, "config-divergence enforcement: forcing all relays off failed: %s",
                 esp_err_to_name(err));
    }
}

/* Same void(void) adaptation as the relays-off hook above, for the same
 * reason: safety_ceiling_sync_set_disable_heat_hooks() wants a void(void)
 * action, and profile_executor_fault_halt() takes a reason string that
 * nothing about the hook call site varies -- see review finding on
 * safety_ceiling_sync.c:548-549 previously installing profile_executor_
 * halt() directly here, which recorded a divergence-caused stop as
 * RUN_STATE_PHASE_HALTED, indistinguishable from a deliberate operator
 * Stop (and, since main_page.html's 8f4d2b20 change, silently hidden from
 * the "Previous firing ended" card). profile_executor_fault_halt() records
 * RUN_STATE_PHASE_FAULTED with this reason instead, same as any other
 * controller-detected fault. */
static void profile_executor_divergence_fault_halt(void)
{
    profile_executor_fault_halt("config/ceiling divergence: heat disabled (see safety_ceiling_sync log)");
}

void main_control_bringup(main_boot_ctx_t *ctx)
{
    /* 2026-09-22 fix: creates safety_ceiling_sync.c's two internal mutexes
     * once, statically, single-threaded, replacing a lazy-create TOCTOU
     * race -- see safety_ceiling_sync_init()'s own doc comment.
     *
     * 2026-09-22 review fix: this MUST come before safety_link_start()
     * immediately below, not later in this function. safety_link_start()
     * creates safety_poll_task, which calls
     * safety_ceiling_sync_reconcile_on_link_up_nonblocking() the instant
     * the link comes up (see the heat-hook comment further down this file,
     * which makes the same point about that window) -- so an init placed
     * after it would leave those first ticks running with both handles
     * still NULL, i.e. unlocked, exactly the serialization gap this fix
     * exists to close. Nothing here depends on the link or on any other
     * bring-up step; it only creates two static mutexes. */
    safety_ceiling_sync_init();

    // --- Safety processor link (isolated UART1 + the opto-isolated fault line) ------
    // Comes up whether or not an RP2040 is answering; a silent far side is
    // link_up = 0, not a startup failure.
    esp_err_t safety_err = safety_link_start(&ctx->safety);
    ctx->safety_err = safety_err;
    if (safety_err != ESP_OK) {
        /* Not a silent degradation: this is the board's last line of defence
         * and the only channel that survives everything else failing. Losing
         * it means no fault can be signalled to the processor that can cut
         * power independently. */
        ESP_LOGE(MAIN_TAG, "safety_link_start failed: %s -- THE ISOLATED FAULT LINE IS UNAVAILABLE; "
                      "no main-controller fault can be signalled to the RP2040",
                 esp_err_to_name(safety_err));
    } else if (ctx->boot_fault_sources != 0) {
        /* First moment the accumulated bring-up failures can actually reach
         * the safety processor. */
        esp_err_t err = safety_link_set_fault_source(&ctx->safety, ctx->boot_fault_sources, true);
        if (err != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "could not assert boot fault sources 0x%02X: %s",
                     (unsigned)ctx->boot_fault_sources, esp_err_to_name(err));
        } else {
            ESP_LOGE(MAIN_TAG, "isolated fault line asserted at boot, sources 0x%02X",
                     (unsigned)ctx->boot_fault_sources);
        }
    } else {
        /* This boot's own bring-up found nothing wrong -- tell safety_link so
         * it can release a stale S6a latch left over from a PREVIOUS boot's
         * assertion (SaftyFW does not reboot alongside the ESP, so its own
         * trip latch outlives an ESP-only reflash/reset). See
         * safety_link_mark_boot_clean()'s doc comment (safety_link.h). */
        safety_link_mark_boot_clean();
    }

    // 2026-09-14 owner decision ("if a config doesn't land and match on both
    // sides then alarm and dissable heaters"): installs the two real
    // heaters-off actions safety_ceiling_sync.c's divergence enforcement
    // calls on every safety_poll_task tick that finds a mismatch. Installed
    // here (not inside safety_ceiling_sync.c itself) so that file stays free
    // of a kiln_io_owner.h/profile_executor.h dependency -- see safety_
    // ceiling_sync.h's own doc comment on safety_ceiling_sync_set_disable_
    // heat_hooks() for why.
    //
    // MOVED HERE 2026-09-14 (opus review, defect 2): this call used to sit
    // right before safety_link_set_context_sources() below, well after
    // safety_link_start() above -- kiln_io_owner_start(), profile_executor_
    // start() and several other steps ran in between. safety_poll_task
    // (started inside safety_link_start()) can observe link-up and run
    // safety_ceiling_sync_reconcile_on_link_up() the instant the link comes
    // up, i.e. during that whole window, and a divergence found there used
    // to log "heaters disabled (all relays forced off, any run halted)"
    // while s_disable_all_relays_off/s_disable_halt_run were still NULL --
    // both hooks a no-op. The verdict (safety_ceiling_sync_is_diverged(),
    // which readiness_gate.h/readiness_http.c actually gate on) was never
    // wrong, and the window is short and entirely pre-HTTP -- so this was a
    // truthfulness defect (logging an action not taken), not a safety hole
    // -- but it is a real instance of this repo's "logging unchecked
    // success" shape (docs -- project_safety_calls_logging_unchecked_
    // success), so it is fixed rather than merely noted.
    //
    // Fix chosen: install the hooks as early as they CAN be installed,
    // rather than teach safety_ceiling_sync.c to log accurately about an
    // absent hook. Both real actions already tolerate being called before
    // their own subsystem starts -- kiln_io_owner_command_all_relays_off()
    // (via main_control_bringup_all_relays_off_void() above) fails closed
    // through post_and_wait()'s NULL-queue check the same way every other
    // kiln_io_owner_command_*() call already does before kiln_io_owner_
    // start() runs, and profile_executor_fault_halt() (like profile_
    // executor_halt(), which it delegates to) is documented to tolerate
    // being called before/without profile_executor_start() (see its own
    // guard, and the RECOVERY MODE note near this function's profile_
    // executor_start() call below) -- so moving the install earlier costs no
    // new failure mode; it just closes the gap between "the link can
    // observe divergence" and "the hooks that act on it exist." An
    // accurate-log alternative (ESP_LOGE once on a NULL hook, per defect 2's
    // other option) was rejected: it would still let a real divergence
    // enforcement no-op during bring-up, merely honestly.
    danger_mode_init(&ctx->safety);

    // heat_enable (heat_enable.h): the shared, refcounted holder of the
    // SAFETY_CMD_REQUEST_ENABLE request that profile_executor.c and
    // autotune_engine.c now make on every real run -- the fix for both of
    // them having never made it at all, so a firing closed K1 and left K4
    // open. Same handle and the same "valid even when safety_err != ESP_OK"
    // reasoning as danger_mode_init() immediately above, and for the same
    // reason it must run before anything that can start a firing or an
    // autotune (profile_executor_start()/autotune_engine_start(), below).
    heat_enable_init(&ctx->safety);

    safety_ceiling_sync_set_disable_heat_hooks(main_control_bringup_all_relays_off_void,
                                                profile_executor_divergence_fault_halt);
    /* 2026-09-15 audit fix, Defect 2: broadens the standing divergence check
     * from abs_max_temp_c alone to the active kiln-config slot's full
     * captured Pico record -- see safety_ceiling_sync.h's doc comment on
     * this seam. */
    safety_ceiling_sync_set_expected_pico_fields_source(kiln_cfg_store_capture_expected_pico_fields);

    /* review_autosave_rework_5bc9afb5_2026-09-15.md MEDIUM: see kiln_cfg_
     * store_set_swap_pending_source()'s doc comment (kiln_cfg_store.h). */
    kiln_cfg_store_set_swap_pending_source(kiln_cfg_swap_is_pending);

    /* docs/KILN_PROFILES_PLAN.md item 5. kiln_cfg_swap.c holds the safety
     * link as a module pointer handed to it once at bring-up -- the same
     * convention safety_ceiling_sync and safety_cfg_http already use, there
     * being no global safety-link getter in this codebase. Installed HERE,
     * beside the other two seams registered just above, rather than at the
     * HTTP call site: safety_link_start() ran at the top of this function,
     * and kiln_cfg_swap_boot_recover() (called from main_network_http.c
     * after kiln_cfg_store_init()) needs the link already installed to
     * finish or roll back an interrupted swap. Passing NULL when
     * safety_link_start() failed is not a case to special-case away --
     * kiln_cfg_swap_apply()/_boot_recover() both document refusing with a
     * reason rather than dereferencing it, and a board with no safety
     * processor must never be able to swap a kiln package at all. */
    kiln_cfg_swap_set_link(safety_err == ESP_OK ? &ctx->safety : NULL);

    // kiln_io_owner (TODO.md 10.14 Phase 1): the single task that writes
    // relay/expander state from here on -- must start before anything that
    // can issue a relay/IO command does (profile_executor, autotune,
    // dashboard_http, the UART IO bridge, all later). Needs both io (just
    // brought up in main_boot_early.c) and safety (just brought up above);
    // skipped like every other hardware-dependent step here if the expander
    // itself never came up -- there is nothing for it to own.
    //
    // Deliberately does NOT clear io_ready on failure: main_kiln_enter_safe_
    // state() and uart_bridge_start_link_watchdog() (main_bridges_bringup.c)
    // both call kiln_io_all_relays_off() DIRECTLY, independent of
    // kiln_io_owner on purpose (this file's link-loss/shutdown paths must
    // still work even if the owner task itself is wedged -- see
    // kiln_io_owner.h's top comment). Clearing io_ready here would wrongly
    // take those away too, over a failure (e.g. task/queue allocation) that
    // says nothing about whether the expander itself is reachable. Every
    // kiln_io_owner_command_*() call already fails closed on its own
    // (post_and_wait() checks for a NULL queue) if this didn't succeed --
    // nothing downstream needs a second guard.
    if (ctx->io_ready) {
        esp_err_t owner_err = kiln_io_owner_start(&ctx->kio, safety_err == ESP_OK ? &ctx->safety : NULL);
        if (owner_err != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "kiln_io_owner_start failed: %s -- no relay/IO commands routed through "
                          "kiln_io_owner will be reachable this boot (direct fail-safe paths are "
                          "unaffected)",
                     esp_err_to_name(owner_err));
            startup_fault_note(STARTUP_FAULT_KILN_IO_OWNER);
        }
    }

    // --- Boot-time safety-processor firmware check (docs/PICO_AUTO_UPDATE_
    // PLAN.md G3) ---------------------------------------------------------
    // Asks, once per boot, whether the RP2040 is running the build this board
    // has staged for it, and attempts one update inside a persisted,
    // read-back-verified budget if not. Everything slow about that (a bounded
    // wait for the Pico's FW_VERSION, a scan of the staged image) happens on
    // its own task -- this call only creates it.
    //
    // Placed after kiln_io_owner_start() and heat_enable_init() above, and
    // before anything that can start a firing (profile_executor_start() /
    // autotune_engine_start() below), so the interlock it consults
    // (ota_http_check_interlocks()) sees a fully-formed picture of whether
    // heat is in play.
    //
    // RECOVERY MODE gate, same shape as profile_executor_start() and
    // autotune_engine_start() below: recovery mode deliberately skips
    // starting subsystems, and this board has been bricked into a permanent
    // recovery loop three times from this area. A board in recovery mode also
    // has no business reflashing its safety processor.
    //
    // A failed safety_link_start() means there is no channel to learn the
    // Pico's identity over, so there is nothing to decide -- skipped, not
    // escalated (the missing link is already reported, loudly, above).
    if (!ctx->recovery_mode && safety_err == ESP_OK) {
        esp_err_t pau_err = pico_auto_update_boot_start(&ctx->safety);
        if (pau_err != ESP_OK) {
            /* Not fatal and not a block: no verdict is published, so the
             * readiness gate stays open. The only thing lost is this boot's
             * automatic update check. */
            ESP_LOGW(MAIN_TAG, "pico_auto_update_boot_start failed: %s -- the safety processor's "
                          "firmware will not be checked against the staged image this boot",
                     esp_err_to_name(pau_err));
        }
    } else if (ctx->recovery_mode) {
        ESP_LOGW(MAIN_TAG, "RECOVERY MODE: pico_auto_update_boot_start() skipped");
    }

#if CONFIG_KILNCTL_SIM_PLANT
    /* Same reasoning as the missing-expander case in main_boot_early.c: with
     * no RP2040 answering, the link's own fail-safe policy asserts
     * SAFETY_FAULT_SRC_SAFETY_LINK, which blocks relay-on everywhere and
     * leaves a simulated firing unable to command heat. A sim build has no
     * kiln to protect, so the policy is turned off here -- and ONLY here.
     * Everything else about the link (reporting, status, the fault line
     * itself) is untouched. */
    if (safety_err == ESP_OK) {
        esp_err_t policy_err = safety_link_fault_on_link_loss(&ctx->safety, false);
        ESP_LOGW(MAIN_TAG, "SIM BUILD: safety-link loss demoted to non-fault (%s) -- "
                      "a missing RP2040 must not block a simulated firing",
                 esp_err_to_name(policy_err));
    }
#endif

    // --- Flash-safe executor for the CONTROL/PROFILES/AUTOTUNE bridges -------
    // MOVED HERE 2026-09-08 (docs/audits/flash_worker_boot_race_2026-09-08.md):
    // this used to be started AFTER profile_executor_start()/autotune_engine_
    // start() below, but relay_cycles_init() (right after this block) and
    // adaptive_tune_init() (called synchronously from inside
    // profile_executor_start()) each do a bounded wait-then-migrate against
    // this worker at boot -- and since all of this runs on ONE task, a wait
    // placed before the worker's own creation call can NEVER see it start:
    // the creation is later in this same call chain, so every cold boot
    // spun the full ceiling on BOTH waits (confirmed on hardware: "still not
    // started" logged twice, ~5 s apart, matching two sequential bounded
    // waits) and both items landed NVS-only with migration_deferred=true
    // every single time, not just occasionally. Starting the worker before
    // either caller runs removes the ordering hazard outright, so this is
    // simply relocated rather than replaced.
    //
    // Its 8192-byte stack must still come from internal SRAM (see the
    // HAZARD block in uart_bridge_ext.c); internal DRAM is at its tightest
    // right after lvgl_port_start() (main_boot_early.c), which already ran
    // before main_control_bringup() -- measured 7680 B largest free block on
    // 2026-08-20 with the three MAX31856s fitted, which is under 8192 and
    // cost all three of the CONTROL/PROFILES/AUTOTUNE bridge surfaces for a
    // whole boot at THAT point. This spot is strictly earlier (and so
    // strictly roomier) than the old one -- nothing between here and the old
    // call site allocates memory this task doesn't also allocate before
    // reaching here today -- so the old measurement is a safe upper bound,
    // not a reason to move this back later.
    if (uart_bridge_ext_start_flash_worker() != ESP_OK) {
        ESP_LOGE(MAIN_TAG, "flash-safe executor failed to start (internal SRAM, largest block %u B) -- "
                      "the CONTROL, PROFILES and AUTOTUNE uart bridges (tasks 8/9/10) will NOT "
                      "start this boot: no zone PID/model reads or writes, no fire-profile list/"
                      "save/delete or profile execution over the PC link, no autotune status or "
                      "control. Kiln control from the PC GUI is unavailable; the HTTP dashboard "
                      "and the thermo/io/safety bridges are unaffected. relay_cycles/adaptive_tune "
                      "migrate-on-load will also see the worker absent and defer (see GET /api/cfgfs).",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }

    // Lifetime relay contact-cycle counts (TODO.md 6A.1), loaded before the
    // executor starts adding to them. A failure here costs the wear history,
    // not correctness, so it is logged and ignored like every other
    // non-essential subsystem in this file. Runs AFTER the flash-safe worker
    // above so its migrate-on-load bounded wait (relay_cycles.c) actually has
    // a running worker to find.
    esp_err_t cycles_err = relay_cycles_init();
    if (cycles_err != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "relay_cycles_init failed: %s -- contact-cycle history not kept this boot",
                 esp_err_to_name(cycles_err));
        startup_fault_note(STARTUP_FAULT_RELAY_CYCLES);
    }

    main_heap_stage("safety+io_owner");

    // --- Profile executor (TODO.md section 6) -------------------------------
    // Must come up before dashboard_http_start() (main_network_http.c), which
    // registers the /api/profile_exec* routes that call into this module at
    // request time. Same non-fatal, NULL-tolerant convention as everything
    // else here: with no expander/thermo bus it still runs its state machine
    // (useful for exercising the dashboard UI) but withholds heat, per
    // profile_executor.h's doc comment. NOT YET VERIFIED AGAINST REAL RELAY/
    // THERMOCOUPLE HARDWARE -- see docs/PROJECT_STATUS.md.
    // RECOVERY MODE (boot_guard.h): skipped entirely. boot_guard_is_recovery_mode()
    // was true because prior boots never reached "healthy" -- see
    // boot_confirm_is_healthy() -- so this boot deliberately withholds every
    // control-loop entry point (profile executor and autotune) and starts
    // only Wi-Fi + the OTA HTTP routes + read-only
    // pages, so an operator can flash a fix instead of the board silently
    // reset-looping under a kiln nobody is watching.
    ctx->exec_err = ESP_ERR_INVALID_STATE;
    if (!ctx->recovery_mode) {
        ctx->exec_err = profile_executor_start(ctx->io_ready ? &ctx->kio : NULL,
                                                ctx->thermo_bus.initialized ? &ctx->thermo_bus : NULL,
                                                safety_err == ESP_OK ? &ctx->safety : NULL);
        if (ctx->exec_err != ESP_OK) {
            ESP_LOGW(MAIN_TAG, "profile_executor_start failed: %s -- no profile execution this boot",
                     esp_err_to_name(ctx->exec_err));
            startup_fault_note(STARTUP_FAULT_PROFILE_EXECUTOR);
        }
    } else {
        ESP_LOGW(MAIN_TAG, "RECOVERY MODE: profile_executor_start() skipped -- no profile execution this boot");
    }

    // (safety_ceiling_sync_set_disable_heat_hooks() itself is installed much
    // earlier now, right after safety_link_start() above -- see the 2026-09-14
    // "MOVED HERE (opus review, defect 2)" comment near the top of this
    // function. profile_executor_halt() tolerates being called before/
    // without profile_executor_start() -- see that function's own guard --
    // so RECOVERY MODE above does not need to special-case it: relays must
    // still be forceable off even when no profile execution is running this
    // boot.)

    // ROADMAP.md M5 / LINK_PROTOCOL.md sec 4: gives the safety link's poll
    // task the two hardware pointers it needs to build SAFETY_CMD_PUSH_CONTEXT
    // (relay state + raw thermocouple readings) -- same non-fatal NULL-tolerant
    // wiring convention as everything else here; a board with no io/thermo_bus
    // this boot still gets the frame, just with zone_count=0/relay_now_mask=0.
    if (safety_err == ESP_OK) {
        safety_link_set_context_sources(&ctx->safety, ctx->io_ready ? &ctx->kio : NULL,
                                        ctx->thermo_bus.initialized ? &ctx->thermo_bus : NULL);
    }

    // --- Autotune engine (TODO.md 6A.4) -------------------------------------
    // Same bring-up convention and NULL-tolerance as profile_executor above;
    // must also come up before dashboard_http_start() (registers /api/autotune*).
    // RECOVERY MODE: skipped, same reasoning as profile_executor_start() above.
    ctx->autotune_err = ESP_ERR_INVALID_STATE;
    if (!ctx->recovery_mode) {
        ctx->autotune_err = autotune_engine_start(ctx->io_ready ? &ctx->kio : NULL,
                                                   ctx->thermo_bus.initialized ? &ctx->thermo_bus : NULL,
                                                   safety_err == ESP_OK ? &ctx->safety : NULL);
        if (ctx->autotune_err != ESP_OK) {
            ESP_LOGW(MAIN_TAG, "autotune_engine_start failed: %s -- no autotune this boot", esp_err_to_name(ctx->autotune_err));
            startup_fault_note(STARTUP_FAULT_AUTOTUNE_ENGINE);
        }
    } else {
        ESP_LOGW(MAIN_TAG, "RECOVERY MODE: autotune_engine_start() skipped -- no autotune this boot");
    }

    main_heap_stage("executor+autotune");
}
