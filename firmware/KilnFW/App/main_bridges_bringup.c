/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/* ROADMAP.md M15 1500-line item: main.c split by boot phase, 2026-09-04.
 * This file is phase 4 (the last) of app_main(): the "late/bridges" stretch
 * -- the PC-link UART bridge tasks, the LVGL display task, the second wave
 * of UART bridges (CONTROL/PROFILES/AUTOTUNE/WIFI + log store mount +
 * telemetry + gpio_probe), and the fail-safe link watchdog -- ending on the
 * "heap stage app_main_done" checkpoint. See main_internal.h for the shared
 * context struct and main.c for app_main() itself, which calls
 * main_bridges_bringup() last. */

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"

#include "gpio_probe.h"
#include "kiln_io.h"
#include "log_store_mount.h"
#include "lvgl_port.h"
#include "safety_link.h"
#include "telemetry_log.h"
#include "uart_bridge.h"
#include "uart_log_bridge.h"

#include "main_internal.h"
#include "startup_faults.h"

void main_bridges_bringup(main_boot_ctx_t *ctx)
{
    // Everything below through the control/profiles/autotune/wifi/gpio_probe
    // block takes &ctx->uart_proto and is only valid to call once
    // ctx->pc_link_ready is true (see the PC link block's own NOTE in
    // main_network_http.c). lvgl_port_start() is the one exception in this
    // stretch -- it takes &ctx->display, not &ctx->uart_proto -- so it stays
    // outside this gate, exactly as unaffected by a dead PC link as
    // Wi-Fi/mDNS/I2C bring-up already were in main_boot_early.c.
    if (ctx->pc_link_ready) {
        // Starts draining the backlog (everything logged since app_main
        // started) over the wire. Started before the other bridge tasks below
        // purely so buffered boot-time log lines -- e.g. an SX1509 or panel
        // failure -- reach the PC as early as possible; registration order
        // otherwise doesn't matter between these tasks.
        if (uart_log_bridge_start(&ctx->uart_proto) != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "Failed to start log uart bridge task");
        }

        if (uart_bridge_start_info_task(&ctx->uart_proto) != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "Failed to start info uart bridge task");
            startup_fault_note(STARTUP_FAULT_PC_BRIDGES);
        }
        if (uart_bridge_start_system_task(&ctx->uart_proto, &ctx->uart_owner) != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "Failed to start system uart bridge task");
            startup_fault_note(STARTUP_FAULT_PC_BRIDGES);
        }
        if (ctx->thermo_bus.initialized) {
            /* Reports the actual error and the free heap: this failure was hit
             * on the bench 2026-08-12 and the old message ("Failed to start
             * thermo uart bridge task") could not distinguish a
             * task-registration refusal (ESP_ERR_INVALID_STATE / task id
             * already taken) from ESP_ERR_NO_MEM, which is the difference
             * between a logic bug and a memory-pressure problem. */
            esp_err_t thermo_task_err = uart_bridge_start_thermo_task(&ctx->uart_proto, &ctx->thermo_bus);
            if (thermo_task_err != ESP_OK) {
                ESP_LOGE(MAIN_TAG, "Failed to start thermo uart bridge task: %s (free heap %lu B, largest block %u B)",
                         esp_err_to_name(thermo_task_err), (unsigned long)esp_get_free_heap_size(),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
                startup_fault_note(STARTUP_FAULT_PC_BRIDGES);
            }
        }
        if (ctx->io_ready && uart_bridge_start_io_task(&ctx->uart_proto, &ctx->kio) != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "Failed to start io uart bridge task");
            startup_fault_note(STARTUP_FAULT_PC_BRIDGES);
        }
    }
    main_heap_stage("uart_bridges_1");

    // Replaces the UART DISPLAY_CMD_* remote-draw path -- LVGL owns the panel
    // now (TODO.md 10.1). uart_bridge_start_display_task() was confirmed dead
    // (nothing ever called it) and removed 2026-08-27, along with the MCP
    // display_* tools on the PC side; see TODO.md 10.1.
    // RECOVERY MODE (boot_guard.h): the LCD UI is NOT started. Its pages are
    // built against the control modules recovery mode deliberately skips --
    // ui_page_home.c reads the profile executor and autotune engine as it
    // builds the home screen -- and with those never started it takes a mutex
    // that was never created:
    //
    //   I (6419) ui_page_home: status label width 424 of bar 464 ...
    //   assert failed: xQueueSemaphoreTake queue.c:1709 (( pxQueue ))
    //
    // observed on the bench 2026-08-22 by forcing recovery_mode true. That
    // panicked ~6.4 s into every boot, so the mode whose entire purpose is to
    // make an unbootable board recoverable over Wi-Fi was itself a boot loop.
    //
    // Skipping the UI rather than teaching every page to tolerate a
    // half-initialised system: recovery mode is defined as "Wi-Fi and the OTA
    // routes, nothing else", the panel has nothing useful to show when no
    // control module is running, and every additional page taught to handle
    // NULL is another path that only ever executes on a board already in
    // trouble. The blank panel is not silent -- boot_guard_init() prints the
    // RECOVERY MODE banner at ERROR level, GET /api/ota/esp/status reports
    // recovery_mode, and the OTA page shows it.
    if (ctx->recovery_mode) {
        ESP_LOGW(MAIN_TAG, "RECOVERY MODE: LVGL/LCD UI skipped -- the panel stays blank this boot; "
                      "recover over Wi-Fi (GET /ota) or POST /api/ota/esp/recovery_exit to reboot "
                      "back into normal mode");
    } else if (ctx->display_ready &&
               lvgl_port_start(&ctx->display, ctx->touch_ready ? &ctx->touch_dev : NULL,
                               ctx->screen_idle_ready ? &ctx->screen_idle : NULL) != ESP_OK) {
        ESP_LOGE(MAIN_TAG, "Failed to start LVGL display task");
        startup_fault_note(STARTUP_FAULT_LCD_UI);
    }
    if (ctx->pc_link_ready) {
        if (ctx->screen_idle_ready && uart_bridge_start_touch_task(&ctx->uart_proto, &ctx->screen_idle) != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "Failed to start touch uart bridge task");
        }
        /* Gated the same way lvgl_port_start() above is (display_ready,
         * !recovery_mode): every kiln_ui_* call this task makes walks the
         * live LVGL widget tree, which only exists once that call has
         * actually succeeded. */
        if (!ctx->recovery_mode && ctx->display_ready && uart_bridge_start_ui_test_task(&ctx->uart_proto) != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "Failed to start UI test uart bridge task");
        }
        if (ctx->safety_err == ESP_OK &&
            uart_bridge_start_safety_task(&ctx->uart_proto, &ctx->safety) != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "Failed to start safety uart bridge task");
            startup_fault_note(STARTUP_FAULT_PC_BRIDGES);
        }
    }

    main_heap_stage("lvgl_start");

    // CONTROL/PROFILES/AUTOTUNE/WIFI (tasks 8-11): additive UART coverage for
    // everything the HTTP dashboard offers (TODO.md "UART parity with
    // HTTP"), so the PC-side GUI can drive the board without Wi-Fi. None of
    // these own hardware directly -- they route through the same
    // zones_http.c/profiles_http.c/profile_executor.c/autotune_engine.c/
    // wifi_prov.c getters and setters the HTTP handlers already call in
    // main_network_http.c, so starting them unconditionally (no io_ready/
    // thermo_bus.initialized gate) matches those modules' own "safe with
    // nothing attached" design.
    if (ctx->pc_link_ready) {
        // The three below share the flash-safe executor started at the
        // "executor+autotune" stage (main_control_bringup.c); ESP_ERR_NO_MEM
        // from any of them means that worker is not up (see
        // uart_bridge_ext.c). Named individually and spelled out, because a
        // dead bridge here is silent from the board's point of view -- it
        // only shows up on the PC as "destination task not registered on the
        // peer".
        esp_err_t control_task_err = uart_bridge_start_control_task(&ctx->uart_proto);
        if (control_task_err != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "Failed to start control uart bridge task: %s%s", esp_err_to_name(control_task_err),
                     control_task_err == ESP_ERR_NO_MEM
                         ? " -- flash-safe executor unavailable; NO zone PID/model reads or writes over the PC link this boot"
                         : "");
            startup_fault_note(STARTUP_FAULT_PC_BRIDGES);
        }
        esp_err_t profiles_task_err = uart_bridge_start_profiles_task(&ctx->uart_proto);
        if (profiles_task_err != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "Failed to start profiles uart bridge task: %s%s", esp_err_to_name(profiles_task_err),
                     profiles_task_err == ESP_ERR_NO_MEM
                         ? " -- flash-safe executor unavailable; NO fire-profile list/save/delete or profile execution over the PC link this boot"
                         : "");
            startup_fault_note(STARTUP_FAULT_PC_BRIDGES);
        }
        esp_err_t autotune_task_err = uart_bridge_start_autotune_task(&ctx->uart_proto);
        if (autotune_task_err != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "Failed to start autotune uart bridge task: %s%s", esp_err_to_name(autotune_task_err),
                     autotune_task_err == ESP_ERR_NO_MEM
                         ? " -- flash-safe executor unavailable; NO autotune status or control over the PC link this boot"
                         : "");
            startup_fault_note(STARTUP_FAULT_PC_BRIDGES);
        }
        if (uart_bridge_start_wifi_task(&ctx->uart_proto) != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "Failed to start wifi uart bridge task");
            startup_fault_note(STARTUP_FAULT_PC_BRIDGES);
        }
        // log_store_mount.c: mounts the `logs` SPIFFS partition (partitions.csv,
        // 0xCF0000, 3072K) at "/logs" -- the persistent home for firing/
        // autotune logs (2026-09-01 audit: "logs are kept in external
        // flash" had nothing behind it before this). Must run before
        // telemetry_log_start() below so the telemetry task's very first
        // tick can persist rather than silently no-op against an unmounted
        // store; a failure here is logged and left non-fatal -- every
        // subsequent log_store_write_*() call just returns
        // ESP_ERR_INVALID_STATE and drops the line, same "degrade, don't
        // wedge" contract log_store.c documents.
        if (log_store_mount() != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "log_store_mount failed -- firing/autotune logs will NOT be persisted "
                          "to flash this boot (live debug-UART telemetry is unaffected)");
            startup_fault_note(STARTUP_FAULT_LOG_STORE);
        }
        // telemetry_log.c: firing/autotune telemetry over the same debug
        // UART every ESP_LOGx call already rides (uart_log_bridge.c), PLUS
        // (unconditionally, regardless of the UART feed's opt-in enable
        // flag) persistence to the log store just mounted above. Started
        // here (after both profile_executor_start()/autotune_engine_start(),
        // main_control_bringup.c, are up). The UART feed itself does
        // nothing until an operator calls telemetry_log_set_enabled() -- see
        // telemetry_log.h's file banner for why THAT default is OFF; the
        // flash persistence path has no such gate.
        if (telemetry_log_start() != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "Failed to start telemetry_log task -- no firing/autotune UART telemetry this boot");
            startup_fault_note(STARTUP_FAULT_TELEMETRY_LOG);
        }
        // ESP_ERR_NOT_SUPPORTED here just means CONFIG_KILNCTL_ENABLE_GPIO_PROBE
        // is off (the default) -- not a failure worth an ESP_LOGE. See
        // gpio_probe.h.
        esp_err_t gpio_probe_err = uart_bridge_start_gpio_probe_task(&ctx->uart_proto);
        if (gpio_probe_err != ESP_OK && gpio_probe_err != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGE(MAIN_TAG, "Failed to start gpio_probe uart bridge task: %s",
                     esp_err_to_name(gpio_probe_err));
        }
    }

    main_heap_stage("uart_bridges_2");

    // --- Fail-safe on loss of the PC link -----------------------------------
    // Started last, so it is watching a link every bridge above can already
    // feed. Until the host's first frame or ACK it holds the board in the
    // link-lost state -- relays off, fault line asserted -- which is the same
    // state a pulled USB cable produces, and the correct one for a controller
    // nobody is currently controlling. See uart_bridge.h for the timing and
    // exactly what "the link went away" means here.
    esp_err_t wd_err = uart_bridge_start_link_watchdog(ctx->io_ready ? &ctx->kio : NULL,
                                                       ctx->safety_err == ESP_OK ? &ctx->safety : NULL);
    if (wd_err != ESP_OK) {
        // Nothing left that drops the relays on link loss. That is the one
        // failure in this file worth shouting about: a kiln whose controller
        // has gone away will stay exactly as hot as it was.
        ESP_LOGE(MAIN_TAG, "PC link watchdog did not start (%s) -- RELAYS WILL NOT DROP ON LINK LOSS",
                 esp_err_to_name(wd_err));
        startup_fault_note(STARTUP_FAULT_PC_LINK_WATCHDOG);
        main_kiln_enter_safe_state(ctx->io_ready ? &ctx->kio : NULL, &ctx->safety, ctx->safety_err == ESP_OK,
                              SAFETY_FAULT_SRC_APP,
                              "no link watchdog, so relays cannot be guaranteed to drop");
    }

    main_heap_stage("app_main_done");
}
