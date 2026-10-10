/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/* ROADMAP.md M15 1500-line item: this file was 1859 lines and is now split
 * by boot phase (2026-09-04) into:
 *   - main.c (this file): app_main() itself, plus the two helpers shared
 *     across phases (main_heap_stage(), main_kiln_enter_safe_state()).
 *   - main_internal.h: main_boot_ctx_t (the struct that replaced the flat
 *     stack of `static` locals app_main() used to declare) and the phase
 *     function prototypes.
 *   - main_boot_early.c: entry through "heap stage display+touch" -- reset-
 *     reason/coredump diagnostics, board die-temp, time_sync, Wi-Fi+mDNS,
 *     the I2C bus and SX1509/kiln_io, boot_guard/watchdog/boot_button, the
 *     shared SPI bus, MAX31856 thermocouples, and display/touch/screen_idle/
 *     backlight.
 *   - main_control_bringup.c: through "heap stage executor+autotune" --
 *     safety_link, danger_mode, heat_enable, kiln_io_owner, profile_executor,
 *     autotune_engine, the flash-safe executor.
 *   - main_network_http.c: through "heap stage uart_owner+proto" -- every
 *     HTTP route registration, the monitor task, and the PC-link UART
 *     owner+protocol stack.
 *   - main_bridges_bringup.c: through "heap stage app_main_done" -- the UART
 *     bridge tasks, LVGL, and the fail-safe link watchdog.
 *
 * app_main() below still calls each phase in EXACTLY the order it used to
 * inline them -- order is semantics in this file (peripheral bring-up order,
 * the heap-stage checkpoints, and what starts before Wi-Fi/HTTP are all
 * load-bearing and were tuned against real hardware failures). This was a
 * move-only split: no phase's internal logic changed, only where its code
 * lives and how it addresses state that used to be a local variable (now a
 * field on the main_boot_ctx_t passed to it).
 */

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "dram_margin.h"
#include "flash_worker.h"
#include "kiln_io.h"
#include "pref_cfg_fs.h"
#include "cfg_fs.h"
#include "cfg_fs_mount.h"
#include "relay_authority.h"
#include "safety_link.h"
#include "uart_log_bridge.h"

#include "main_internal.h"

/* Was `static const char *TAG = "app_main";`. Every phase file logs under
 * this same tag main.c always used -- now shared rather than duplicated,
 * and renamed off the generic `TAG` because it is widened to non-static
 * (see main_internal.h and the split's symbol-audit convention). */
const char *MAIN_TAG = "app_main";

/* Boot-stage internal-DRAM probe.
 *
 * The largest contiguous 8-bit internal block is 163840 bytes early in boot
 * and a few KB by the time the UART bridge tasks are created -- that collapse,
 * not any free total, is what makes a multi-KB task stack or queue allocation
 * fail (see the MALLOC_CAP_INTERNAL note in main_boot_early.c's reset-reason
 * block). Moving LVGL's allocator to PSRAM (lvgl_mem_psram.c) recovered part
 * of it but demonstrably not all, and "somewhere between those two log
 * lines" was as precise as the evidence got.
 *
 * This prints the same figure at each bring-up stage so the drop can be
 * attributed to the stage that causes it rather than inferred. It is two log
 * lines' worth of cost at boot and nothing at all afterwards, so it stays in
 * rather than being added and removed each time this question comes back --
 * it has come back three times now.
 *
 * `delta` is against the previous call, so a stage that costs a large
 * contiguous block is visible directly without subtracting timestamps by hand. */
void main_heap_stage(const char *stage)
{
    static size_t s_prev_largest;
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t free8 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int delta = (s_prev_largest == 0) ? 0 : (int)largest - (int)s_prev_largest;
    s_prev_largest = largest;
    ESP_LOGW(MAIN_TAG, "heap stage %-18s largest=%6u delta=%+7d dram_free=%7u", stage,
             (unsigned)largest, delta, (unsigned)free8);

    /* See dram_margin.h for exactly why these two thresholds and not some
     * other pair -- they are the documented figures from the one time this
     * exact failure (HTTP sockets resetting, /app.js truncated, "Loading..."
     * forever) was caught with numbers attached, not an invented margin.
     * Logged at ERROR, not just returned, because heap_stage() callers never
     * check a return value today and this alarm exists precisely so a
     * regression announces itself in the boot log instead of only showing up
     * later as an unrelated-looking front-end bug report. */
    dram_margin_result_t margin = dram_margin_check(largest, free8);
    /* The owner's 20 kB floor. Checked before the two below and reported on
     * its own line, because it answers a different question: not "are we in
     * the zone where the failure was observed" (the standing alarm) nor "is
     * this build worse than any before it" (the regression line), but "have
     * we spent the margin we said we would keep". It is the early warning for
     * the other two, so it must not be folded into either. */
    if (margin.below_floor) {
        ESP_LOGE(MAIN_TAG,
                 "heap stage %-18s BELOW THE 20K DRAM FLOOR: dram_free=%u (floor %u) -- "
                 "still above the measured HTTP-failure figure (%u), so this is the warning "
                 "shot, not the failure. Find what grew before it reaches it",
                 stage, (unsigned)free8, (unsigned)KILN_DRAM_FREE_FLOOR_BYTES,
                 (unsigned)KILN_DRAM_FREE_ALARM_BYTES);
    }
    if (margin.regressed) {
        /* The line that is actually news: worse than this firmware has ever
         * measured. Distinct wording from the standing alarm below on purpose
         * -- grep for "DRAM REGRESSION" to find only real changes. */
        ESP_LOGE(MAIN_TAG,
                 "heap stage %-18s DRAM REGRESSION: largest=%u (was %u%s) "
                 "dram_free=%u (was %u%s) -- this build uses MORE internal DRAM "
                 "than any measured before it. Do not raise the thresholds in "
                 "dram_margin.h to silence this; find what grew",
                 stage, (unsigned)largest, (unsigned)KILN_DRAM_LARGEST_KNOWN_BYTES,
                 margin.largest_regressed ? ", WORSE" : "", (unsigned)free8,
                 (unsigned)KILN_DRAM_FREE_KNOWN_BYTES,
                 margin.free_regressed ? ", WORSE" : "");
    } else if (margin.tripped) {
        /* No longer a standing condition since 2026-08-27 (dram_margin.h):
         * that pass moved four task stacks off internal DRAM and pushed
         * `largest` above KILN_DRAM_LARGEST_ALARM_BYTES for the first time, so
         * this branch firing today is a real, non-recurring event, not the
         * every-boot noise it used to be. Deliberately WARNING, not ERROR
         * even so -- it is real and must stay visible,
         * but an ERROR that fires every single boot is one everybody learns to
         * scroll past, and then the DRAM REGRESSION line above would arrive
         * inside a message that has been ignored for months. See
         * dram_margin.h's comment on why the two are separated.
         *
         * The level split has a second, useful consequence on this board:
         * uart_log_bridge.c prioritises ERROR lines so they survive the
         * boot-burst log queue (commit f183b96), while WARN lines can be
         * dropped when that queue fills -- and it does fill during boot. So
         * the regression line, which is news, is the one guaranteed to reach
         * the host, and the standing line, which says only "still true", is
         * the one allowed to be dropped. That is the right way round; do not
         * "fix" the standing line by promoting it to ERROR. */
        ESP_LOGW(MAIN_TAG,
                 "heap stage %-18s DRAM margin (standing, expected): largest=%u "
                 "(failure-zone <%u: %s) dram_free=%u (failure-zone <%u: %s) -- at "
                 "or below the documented HTTP-socket-reset figures (dram_margin.h); "
                 "a single allocation bigger than the largest figure will fail even "
                 "though dram_free looks nonzero",
                 stage, (unsigned)largest, (unsigned)KILN_DRAM_LARGEST_ALARM_BYTES,
                 margin.largest_low ? "yes" : "no", (unsigned)free8,
                 (unsigned)KILN_DRAM_FREE_ALARM_BYTES, margin.free_low ? "yes" : "no");
    }
}

/* The state app_main must leave the board in on *any* path that stops short of
 * a working PC link: relays down, isolated fault line up.
 *
 * app_main returning is not a crash -- FreeRTOS keeps running and so do every
 * task started before the failure -- so "we gave up here" has to be an
 * explicit, positive action rather than the absence of one. Both steps are
 * best-effort by nature (the expander may be the thing that failed), which is
 * why each is reported separately instead of short-circuiting. */
void main_kiln_enter_safe_state(kiln_io_t *io, SafetyLinkClass *safety, bool safety_ok,
                                  uint32_t fault_sources, const char *why)
{
    ESP_LOGE(MAIN_TAG, "entering safe state: %s", why);

    if (io) {
        esp_err_t err = kiln_io_all_relays_off(io);
        if (err != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "could not drop the relays: %s -- RELAY STATE IS UNKNOWN",
                     esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(MAIN_TAG, "no expander: relays cannot be commanded and their state is UNKNOWN");
    }

    if (safety_ok) {
        esp_err_t err = safety_link_set_fault_source(safety, fault_sources, true);
        if (err != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "could not assert the isolated fault line: %s", esp_err_to_name(err));
        }
    } else {
        /* Worst case on this board: no relay control *and* no way to tell the
         * safety processor. Nothing here can fix it; the log line is so the
         * operator does not have to infer it from silence. */
        ESP_LOGE(MAIN_TAG, "no safety link: the RP2040 cannot be told the main controller has faulted");
    }
}

/* Every task creation across the four phase functions below is a single-line
 * "start" call -- the semaphore/queue/task choreography behind each one
 * lives in that driver's own file (MAX31856_start_all in MAX31856.c,
 * monitor_task_start in monitor_task.c, uart_bridge_start_* in
 * uart_bridge.c, etc.), not here.
 *
 * Only two failures abort app_main: the UART owner and the protocol stack on
 * top of it. Everything else is logged and stepped over -- a board with a dead
 * panel or an absent thermocouple daughterboard is still a board worth having
 * on the link, and the log bridge is what carries that news to the PC.
 *
 * "Stepped over" is not the same as ignored, though. Three of those failures
 * mean the kiln is uncontrolled or unmonitored -- no expander (relay state
 * unknown), no SPI bus and no thermocouple channel at all (no temperature) --
 * and each raises a bit in boot_fault_sources (main_boot_ctx_t), which is
 * asserted on the isolated fault line as soon as the safety link exists.
 * Both abort paths call main_kiln_enter_safe_state() first: app_main
 * returning does not stop FreeRTOS, so leaving the board safe has to be an
 * action, not an omission. */
void app_main(void)
{
    // Installed before anything else touches ESP_LOGx, so every line from
    // here on -- including failures during the driver bring-up in
    // main_boot_early.c -- is captured and queued. Nothing is actually sent
    // yet (there's no uart_protocol_t until main_network_http_bringup());
    // see uart_log_bridge_start() (main_bridges_bringup.c) for when the
    // backlog actually flushes.
    uart_log_bridge_early_init();

    // Before any task that can enter a cfg save section exists (LVGL, httpd,
    // the bridges, main_boot_early's deferred tasks): makes every save section
    // reserve the flash worker even if it begins before main_control_bringup()
    // starts that worker. Only creates a static mutex and installs two hooks,
    // so it is safe in recovery mode too. See uart_bridge_ext.c.
    uart_bridge_ext_save_reservation_init();
    // Factory-reset writer fence (pref_cfg_fs.h): refuse cfg saves while the reset mark is set.
    pref_cfg_fs_set_reset_refuse_hook(relay_authority_reset_refuses_writer);
    cfg_fs_mount_set_write_refuse_hook(relay_authority_reset_refuses_writer);
    cfg_fs_set_write_refuse_hook(relay_authority_reset_refuses_writer);

    static main_boot_ctx_t ctx;

    main_boot_early(&ctx);
    main_control_bringup(&ctx);
    main_network_http_bringup(&ctx);
    main_bridges_bringup(&ctx);
}
