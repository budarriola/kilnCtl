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
#include "kiln_io_owner.h"
#include "profile_executor.h"
#include "autotune_engine.h"
#include "relay_cycles.h"
#include "safety_link.h"
#include "uart_bridge.h"

#include "main_internal.h"

void main_control_bringup(main_boot_ctx_t *ctx)
{
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

    // danger_mode (diagnostics page's explicit-accept relay-override
    // section) needs the safety link handle to request/release
    // SAFETY_CMD_REQUEST_ENABLE -- see danger_mode.h. `&ctx->safety` is valid
    // even when safety_err != ESP_OK (safety_link_request_enable() itself
    // refuses cleanly on an uninitialized link); must run before
    // kiln_io_owner_start() below, whose relay_on_blocked() calls
    // danger_mode_active(). Also safe before profile_executor_start() has
    // run, same guarantee boot_button_start() (main_boot_early.c) already
    // relies on: danger_mode_request_start() only reads
    // profile_executor_get_status(), which answers cleanly pre-start.
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
        }
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

    // Lifetime relay contact-cycle counts (TODO.md 6A.1), loaded before the
    // executor starts adding to them. A failure here costs the wear history,
    // not correctness, so it is logged and ignored like every other
    // non-essential subsystem in this file.
    esp_err_t cycles_err = relay_cycles_init();
    if (cycles_err != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "relay_cycles_init failed: %s -- contact-cycle history not kept this boot",
                 esp_err_to_name(cycles_err));
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
        }
    } else {
        ESP_LOGW(MAIN_TAG, "RECOVERY MODE: profile_executor_start() skipped -- no profile execution this boot");
    }

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
        }
    } else {
        ESP_LOGW(MAIN_TAG, "RECOVERY MODE: autotune_engine_start() skipped -- no autotune this boot");
    }

    // --- Flash-safe executor for the CONTROL/PROFILES/AUTOTUNE bridges -------
    // Started HERE, and deliberately not down with the bridge tasks that use
    // it: its 8192-byte stack must come from internal SRAM (see the HAZARD
    // block in uart_bridge_ext.c) and internal DRAM is at its tightest right
    // after lvgl_port_start() (main_bridges_bringup.c) -- measured 7680
    // largest free block on 2026-08-20 with the three MAX31856s fitted,
    // which is under 8192 and cost all three of those bridge surfaces for a
    // whole boot. At this stage the largest free block is ~31744. Do not
    // move this later.
    if (uart_bridge_ext_start_flash_worker() != ESP_OK) {
        ESP_LOGE(MAIN_TAG, "flash-safe executor failed to start (internal SRAM, largest block %u B) -- "
                      "the CONTROL, PROFILES and AUTOTUNE uart bridges (tasks 8/9/10) will NOT "
                      "start this boot: no zone PID/model reads or writes, no fire-profile list/"
                      "save/delete or profile execution over the PC link, no autotune status or "
                      "control. Kiln control from the PC GUI is unavailable; the HTTP dashboard "
                      "and the thermo/io/safety bridges are unaffected.",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }

    main_heap_stage("executor+autotune");
}
