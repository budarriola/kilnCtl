/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/* ROADMAP.md M15 1500-line item: main.c split by boot phase, 2026-09-04.
 * This file is phase 1 of app_main(): from entry through the
 * "heap stage display+touch" checkpoint -- reset-reason/coredump
 * diagnostics, the board die-temp sensor, time_sync, Wi-Fi + mDNS, the I2C
 * bus and the SX1509/kiln_io board layer, boot_guard/rtc_watchdog/
 * watchdog_cfg, the i2c_scan, the shared SPI bus, the MAX31856
 * thermocouple channels, and the display/touch/screen_idle/backlight-pwm
 * bring-up. See main_internal.h for the shared context struct and
 * main.c for app_main() itself, which calls main_boot_early() first. */

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_core_dump.h" /* esp_core_dump_image_check() -- kept direct, see the call site's comment on why hal_sysinfo's presence-only bool can't replace it here */
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h" /* esp_get_free_heap_size() -- reset reason itself now comes from
                          * hal_sysinfo_reset_reason() below, but this call site still needs
                          * the vendor heap query directly */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hal_sysinfo.h" /* hal_sysinfo_reset_reason() */
#include "board_temps.h"
#include "boot_guard.h"
#include "recovery_switch.h"
#include "cfg_fs_mount.h"
#include "watchdog_cfg.h"
#include "crash_report.h"
#include "estop_verification.h"
#include "MAX31856.h"
#include "hal_spi_esp_owner.h" /* ILI9488_start() still takes a raw spi_owner_t*
                                 * -- see that header's comment for why this
                                 * bridge exists post-HAL-Phase-1b. */
#include "NS2009.h"
#include "FT6336U.h"
#include "hal_i2c.h"
#include "hal_i2c_esp_owner.h"
#include "stack_margin.h"
#include "touch_dev.h"
/* ROADMAP.md M15 A1: this is one of the SX1509 write/config owners
 * (SX1509_start()) -- see SX1509_internal.h's top comment. */
#define SX1509_OWNER_BUILD
#include "SX1509_internal.h"
#include "screen_idle.h"
#include "backlight_pwm.h"
#include "display_power_cfg.h"
#include "i2c_scan.h"
#include "thermo_owner.h"
#include "mdns.h"
#include "rtc_watchdog.h"
#include "time_sync.h"
#include "wifi_prov.h"
#include "settings.h"

#include "main_internal.h"
#include "startup_faults.h"

/* The three MAX31856 ~DRDY lines land on the SX1509 (IO8/IO9/IO10), not on
 * ESP32 GPIOs, so the thermocouple driver cannot see them without owning an
 * I2C device it has no business owning. It takes this callback instead --
 * see MAX31856_set_drdy_provider() -- and falls back to an elapsed-time
 * heuristic whenever it returns false. */
static bool main_kiln_drdy_provider(uint8_t channel, bool *out_asserted, void *ctx)
{
    kiln_io_t *io = (kiln_io_t *)ctx;
    return kiln_io_get_drdy(io, channel, out_asserted) == ESP_OK;
}

/* backlight_pwm.h's backlight_pwm_query_fn adapter (HW_ABSTRACTION.md
 * "drivers/ layering" item 5): backlight_pwm.c is a hw-layer driver and must
 * not itself include screen_idle.h (ui) or display_power_cfg.h (persist), so
 * this boot-glue file -- which already knows both -- reads them on its
 * behalf. Same shape as main_kiln_drdy_provider() just above: a small
 * adapter that closes a plain-C-typed `ctx` back over its real type. `ctx`
 * is always `&ctx->screen_idle` here (see backlight_pwm_init()'s call site
 * below), matching what backlight_pwm.c used to cast `bl->idle` back to
 * directly before this inversion. */
static esp_err_t backlight_pwm_query_screen_and_brightness(void *ctx, bool *out_screen_on,
                                                            uint32_t *out_idle_ms,
                                                            uint8_t *out_brightness_pct)
{
    const screen_idle_t *idle = (const screen_idle_t *)ctx;
    esp_err_t err = screen_idle_get_state(idle, out_screen_on, out_idle_ms);
    if (err != ESP_OK) {
        return err; /* lock timeout on screen_idle's side -- caller retries next poll */
    }
    *out_brightness_pct = display_power_cfg_brightness_percent();
    return ESP_OK;
}

/* TODO.md section 14: the HTTP-connection-reset investigation has a
 * reproducer (8 parallel /app.js, ~1 in 8 reset) and a fragmentation
 * hypothesis (largest_free_block 8704B vs. the 8589-byte gzip payload) that
 * was reasoned from two numbers, not from watching an allocation actually
 * fail. This is that watch: heap_caps_register_failed_alloc_callback() fires
 * synchronously, in the failing task's own context, on ANY heap_caps
 * allocation failure anywhere in the system (MALLOC_CAP_INTERNAL or
 * otherwise) -- so a reproducer run either lands a line here naming the
 * task, size and caps of the allocation that actually returned NULL, or it
 * does not, which is itself evidence: no line here during a reproduced reset
 * means the reset is not caused by any heap_caps_* allocation failing, and
 * the fragmentation hypothesis above needs to be dropped rather than
 * refined. Logged at ERROR for the same reason heap_stage()'s regression
 * line is ERROR, not WARNING -- see uart_log_bridge.c's priority note a few
 * lines up in this file: ERROR lines survive the boot-burst queue, WARNING
 * lines can be dropped, and this is exactly the kind of one-shot event that
 * must not be lost to that queue filling during a concurrent-request burst. */
static void main_alloc_fail_trace_cb(size_t size, uint32_t caps, const char *function_name)
{
    ESP_LOGE(MAIN_TAG, "ALLOC FAILED: task=%s size=%u caps=0x%08lx fn=%s largest_int=%u free_int=%u",
             pcTaskGetName(NULL), (unsigned)size, (unsigned long)caps, function_name,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

void main_boot_early(main_boot_ctx_t *ctx)
{
    // TODO.md section 14 instrumentation: catch the actual failing
    // allocation (see main_alloc_fail_trace_cb() above) rather than
    // continuing to reason from largest_free_block/dram_free snapshots that
    // don't move under the failing load. Registered before any driver
    // bring-up so a failure during boot itself is caught too, not just
    // during the HTTP reproducer.
    heap_caps_register_failed_alloc_callback(main_alloc_fail_trace_cb);

    // --- Boot-time log budget, 2026-08-20 -------------------------------
    // The ESP-IDF Wi-Fi driver emits ~40 INFO lines during init ("Init
    // dynamic tx buffer num", "Init static rx buffer size", and so on), all
    // of it fixed configuration this project never varies and can read from
    // sdkconfig any time. That burst lands in exactly the window where the
    // interesting diagnostics are (reset reason, task/queue creation
    // failures, the first page's tap-target dump) and overruns
    // uart_log_bridge's queue, which then reports "log line(s) dropped
    // (queue full)" and silently discards them.
    //
    // That is not hypothetical: this flood has now destroyed evidence four
    // separate times during bench work -- it hid the boot-time
    // task-creation diagnostics that made the UART registration bug
    // undiagnosable for days, it swallowed the esp_reset_reason() line added
    // specifically to explain a spontaneous reboot, and it ate the LCD
    // tap-target dump twice. Dropping the driver's own chatter to WARN keeps
    // every genuine Wi-Fi problem (auth failures, disconnect reasons, the
    // "wifi:" state transitions that matter) while giving that queue budget
    // back to this firmware's own diagnostics.
    //
    // Deliberately set here, before wifi_prov_start() runs, so it applies to
    // the init burst itself rather than only to whatever comes after it.
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);

    // --- Reset-reason instrumentation (TODO.md 6089 open item, 2026-08-20) --
    // The board has been seen rebooting spontaneously on the bench with no
    // panic text visible over the log-bridge channel (the panic handler
    // writes straight to the USB-Serial-JTAG console, bypassing
    // esp_log_set_vprintf/uart_log_bridge entirely). This line survives
    // that: whatever the cause, the NEXT boot logs it here, queued by
    // uart_log_bridge_early_init() (main.c) so it reaches the PC link even
    // before Wi-Fi/UART bring-up finishes. Logged at ERROR so it is never
    // filtered out by a lower default log level, and unconditionally (not
    // just on unexpected reasons) so a normal power-cycle/deliberate reset
    // boot establishes the "this is what a clean boot's line looks like"
    // baseline for comparison.
    {
        hal_reset_reason_t rr = hal_sysinfo_reset_reason();
        const char *rr_name = "UNKNOWN";
        switch (rr) {
            case HAL_RESET_UNKNOWN:    rr_name = "UNKNOWN"; break;
            case HAL_RESET_POWERON:    rr_name = "POWERON"; break;
            case HAL_RESET_EXT:        rr_name = "EXT"; break;
            case HAL_RESET_SW:         rr_name = "SW"; break;
            case HAL_RESET_PANIC:      rr_name = "PANIC"; break;
            case HAL_RESET_INT_WDT:    rr_name = "INT_WDT"; break;
            case HAL_RESET_TASK_WDT:   rr_name = "TASK_WDT"; break;
            case HAL_RESET_WDT:        rr_name = "WDT"; break;
            case HAL_RESET_DEEPSLEEP:  rr_name = "DEEPSLEEP"; break;
            case HAL_RESET_BROWNOUT:   rr_name = "BROWNOUT"; break;
            case HAL_RESET_SDIO:       rr_name = "SDIO"; break;
            default: break;
        }
        /* MALLOC_CAP_INTERNAL on its own is a misleading number on this chip
         * and it misled this project for most of a day. It counts every
         * internal region including IRAM, which is 32-bit-access-only: a task
         * stack, a queue, or any byte-addressable buffer needs
         * MALLOC_CAP_8BIT, so a large "free_internal" can sit alongside a DRAM
         * pool that is effectively full. Reporting both, plus the largest
         * contiguous 8-bit block, is what actually explains an ESP_ERR_NO_MEM
         * here: a multi-KB allocation fails on the largest-block figure, not
         * on any of the free totals. */
        ESP_LOGE(MAIN_TAG, "esp_reset_reason=%d (%s); free_heap=%u free_spiram=%u", (int)rr, rr_name,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        ESP_LOGE(MAIN_TAG,
                 "internal heap: total_free=%u dram_free(8BIT)=%u dram_largest_block=%u "
                 "dram_min_ever=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

        // CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y (sdkconfig.defaults, added
        // the same 2026-08-20 diagnosability pass as this reset-reason line)
        // writes a panic's backtrace/task-state dump to the new `coredump`
        // flash partition (partitions.csv) instead of only to the
        // USB-Serial-JTAG console, which re-enumerates across the reset and
        // is exactly why earlier attempts to catch this board's spontaneous
        // reboots lost the panic text. esp_core_dump_image_check() reports
        // whether a dump is sitting there waiting to be read out with
        // espcoredump.py -- logged right next to the reset reason so a
        // glance at the boot log answers both "did it reset unexpectedly"
        // and "is there a dump for that reset" in one place, without having
        // to separately run the readback tool speculatively every time.
        // ESP_ERR_NOT_FOUND from this call just means no dump is present
        // (the common case, including every normal boot) -- not a fault.
        //
        // PROVEN END TO END 2026-08-20 with a deliberate crash: dump written
        // (82080 bytes), found and checksum-verified on the next boot, and
        // decoded back to the exact crashing function/line. It did NOT work
        // before that test -- the partition was 64K, too small for an 82KB
        // dump, so every panic silently wrote nothing. See partitions.csv.
        //
        // ALWAYS reports its outcome, including "no dump present". Silence used
        // to mean this check ran and found nothing -- but it equally meant the
        // line was lost to uart_log_bridge's queue, which does drop lines
        // during the boot burst and says so ("N log line(s) dropped (queue
        // full)"). Found 2026-08-20 while trying to prove this path works after
        // a deliberate crash: the reboot reported esp_reset_reason=4 (PANIC),
        // printed no coredump line at all, and also reported 9 dropped lines --
        // leaving no way to tell "no dump was written" from "the dump was found
        // and the news got dropped". An absent log line is not evidence, and a
        // crash diagnostic nobody can trust is worse than none, so this now
        // states the negative result explicitly and prints the error name in
        // every branch.
        esp_err_t cd_err = esp_core_dump_image_check();
        if (cd_err == ESP_OK) {
            ESP_LOGE(MAIN_TAG, "COREDUMP PRESENT in flash from a previous crash -- decode with "
                          "espcoredump.py info_corefile/dbg_corefile against build/KilnCtrl.elf "
                          "(see firmware/KilnFW/TODO.md for the exact command), then erase it "
                          "with esp_core_dump_image_erase() or it will keep reporting here");
        } else if (cd_err == ESP_ERR_NOT_FOUND || cd_err == ESP_ERR_INVALID_STATE) {
            /* The ordinary case on a clean boot, and the expected result after
             * a normal restart. Logged rather than passed over so that seeing
             * nothing here means the check itself did not run. */
            ESP_LOGE(MAIN_TAG, "no coredump in flash (esp_core_dump_image_check: %s)",
                     esp_err_to_name(cd_err));
        } else {
            ESP_LOGE(MAIN_TAG, "coredump check FAILED: %s -- a dump may exist but be unreadable",
                     esp_err_to_name(cd_err));
        }

        // crash_report.c: captures a small NVS-persisted summary (task,
        // cause, PC, backtrace) of the SAME coredump image just checked
        // above, so the diagnostics web page can show "what the last crash
        // was" without anyone plugging in a laptop to run espcoredump.py.
        // Own module, own init call -- see crash_report.h's header comment.
        // Never fails app_main; every error is logged and swallowed inside
        // crash_report_init() itself.
        crash_report_init();

        // estop_verification.c: brings up the same KILN_NVS_PARTITION
        // namespace to read/persist the operator's E-stop bench-verification
        // confirmation (readiness item "estop_verified"). Own module, own
        // init call, same reasoning as crash_report_init() just above --
        // never fails app_main, every error logged inside the call itself.
        estop_verification_init();
    }

    // --- ESP32-S3 internal die-temperature sensor (TODO.md 10.7) -----------
    // Independent of every other peripheral here -- no bus, no GPIO, nothing
    // to share or serialize against -- so it comes up this early rather than
    // waiting on I2C/SPI below. Non-fatal like everything else in app_main:
    // board_temps_get() reports esp32_valid=false if this fails, same
    // NULL-tolerant convention as a missing thermo_bus/kiln_io elsewhere in
    // this file. NOT YET VERIFIED AGAINST THE INSTALLED TOOLCHAIN OR REAL
    // HARDWARE THIS PASS -- see board_temps.h's doc comment.
    main_heap_stage("entry");

    esp_err_t board_temps_err = board_temps_start();
    if (board_temps_err != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "board_temps_start failed: %s -- no ESP32-S3 die temp this boot",
                 esp_err_to_name(board_temps_err));
    }

    // time_sync_start() (SNTP + persisted TZ, time_sync.h) must run before
    // wifi_prov_start() below: wifi_prov.c's do_ev_got_ip() calls
    // time_sync_notify_got_ip() the moment station join reaches GOT_IP,
    // which can happen on the Wi-Fi driver's own event-loop task almost
    // immediately after wifi_prov_start() returns -- time_sync_start() has
    // to have already loaded/applied the TZ and prepared (unstarted) the
    // SNTP client before that race is even possible. Non-fatal like every
    // other _start() here: a failure here just means no network time this
    // boot, same "board still boots" convention as unit_pref_start() later.
    esp_err_t time_sync_err = time_sync_start();
    if (time_sync_err != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "time_sync_start failed: %s -- no network time this boot",
                 esp_err_to_name(time_sync_err));
        startup_fault_note(STARTUP_FAULT_TIME_SYNC);
    }

    // --- Wi-Fi (station "home" mode / AP mode, see wifi_prov.h) -------------
    // A third client alongside the UART PC link and the safety processor
    // link, not a dependency of either -- started here, independent of and
    // not gating anything below, so a Wi-Fi failure can never delay or block
    // relay-safety-relevant bring-up. wifi_prov_start() is non-blocking: the
    // station join (if any) happens on the Wi-Fi driver's own event loop.
    esp_err_t wifi_err = wifi_prov_start();
    if (wifi_err != ESP_OK) {
        ESP_LOGE(MAIN_TAG, "wifi_prov_start failed: %s -- no Wi-Fi this boot, PC link and safety link unaffected",
                 esp_err_to_name(wifi_err));
    }

    // Advertises this board as "kilnctl.local" over mDNS so the web UI/pc_tools
    // don't need the station's raw IP -- reachable from Wi-Fi's AP fallback
    // and station modes alike, same as the HTTP server itself. Needs the
    // netif/event loop wifi_prov_start() just brought up, but not a joined
    // network, so it runs unconditionally rather than gating on wifi_err:
    // the AP fallback case still wants kilnctl.local to resolve. Non-fatal like
    // everything else here -- no name resolution is not a reason to fail
    // app_main.
    main_heap_stage("wifi_prov");

    esp_err_t mdns_err = mdns_init();
    if (mdns_err == ESP_OK) {
        // Two names on purpose. "kilnctl" is the primary because it is the
        // name a user has already been told twice by the time they type it --
        // the AP SSID is kilnCtl and so is the mDNS instance name below -- and
        // reported on the bench 2026-08-20: kilnctl.local was tried first and
        // failed to resolve while the board was up and serving, because the
        // hostname was "kiln" and only the hostname resolves. An instance name
        // labels the _http service in a service browser; it never answers an
        // A record.
        //
        // The old name was "kiln". There is deliberately no alias for it: a
        // delegated hostname only answers A records for an address list the
        // caller supplies and keeps up to date across every IP change, so
        // mdns_delegate_hostname_add("kiln", NULL) registers a name that
        // resolves to nothing -- tried on the bench 2026-08-20 and confirmed
        // silent. Carrying a second name correctly would mean mirroring the
        // station's address on every GOT_IP, which is real machinery for a
        // convenience alias. Every in-repo reference was renamed instead; if
        // an outside bookmark breaks, the raw IP and the LCD's second QR both
        // still work.
        mdns_hostname_set("kilnctl");
        mdns_instance_name_set("kilnCtl");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    } else {
        ESP_LOGW(MAIN_TAG, "mdns_init failed: %s -- no kilnctl.local this boot", esp_err_to_name(mdns_err));
    }

    // --- I2C bus -----------------------------------------------------------
    // ESP-IDF allows exactly one i2c_master_bus_handle_t per physical bus, so
    // it is created here and handed to whoever needs it, rather than by the
    // first driver that happens to want it.
    main_heap_stage("mdns");

    ctx->i2c_bus = NULL;
    i2c_master_bus_config_t i2c_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t i2c_err = i2c_new_master_bus(&i2c_config, &ctx->i2c_bus);
    if (i2c_err != ESP_OK) {
        ESP_LOGE(MAIN_TAG, "i2c_new_master_bus failed: %s -- no expander, no relays, no display",
                 esp_err_to_name(i2c_err));
        ctx->i2c_bus = NULL;
    }

    // --- SX1509 expander + the board layer over it -------------------------
    // First on the bus and first of the devices, because it owns the relay
    // drives. kiln_io_init loads every relay bit LOW into the data latch
    // *before* it turns those pins into outputs, so the coils never see the
    // expander's power-on latch default of 1 -- see kiln_io.h.
    ctx->io_ready = false;
    if (ctx->i2c_bus) {
        esp_err_t exp_err = SX1509_start(&ctx->expander, ctx->i2c_bus);
        if (exp_err != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "SX1509 bring-up failed: %s", esp_err_to_name(exp_err));
        } else {
            esp_err_t io_err = kiln_io_init(&ctx->kio, &ctx->expander);
            if (io_err != ESP_OK) {
                ESP_LOGE(MAIN_TAG, "kiln_io_init failed: %s -- relay state is not guaranteed",
                         esp_err_to_name(io_err));
            } else {
                ctx->io_ready = true;
                ESP_LOGI(MAIN_TAG, "board I/O up: all relays off");
            }
        }
    }

    // --- boot_guard / RTC watchdog: the earliest point it is safe to arm
    // any watchdog machinery ---------------------------------------------
    // Relays are latched off above (kiln_io_init(), whether or not it
    // succeeded -- io_ready reflects that, and boot_guard doesn't need it
    // to be true; a boot with no expander is still a boot worth counting).
    // Nothing before this point can plausibly run long enough to trip
    // either mechanism, and everything after it can, so this is the
    // earliest and latest-safe place for both:
    //   - boot_guard_init() increments the persisted "unconfirmed boot"
    //     counter and decides THIS boot's recovery_mode -- see boot_guard.h.
    //     Read once into ctx->recovery_mode, consulted further down (this
    //     file) and by main_control_bringup.c's decision of whether to start
    //     profile_executor/autotune.
    //   - rtc_watchdog_start() arms the hardware RTC watchdog as the
    //     independent-of-the-scheduler last resort -- see rtc_watchdog.h.
    //     Fed from monitor_task.c's existing heartbeat cadence.
    boot_guard_init();
    // Threshold reached and the recovery image verifies: switch partitions and
    // reboot into it (does not return). Otherwise falls through to today's
    // degraded in-app recovery mode. See recovery_switch.h.
    recovery_switch_at_boot_threshold();
    ctx->recovery_mode = boot_guard_is_recovery_mode();
    rtc_watchdog_start();

    // cfg_fs_mount_device() (persist/cfg_fs_mount.h): mounts the `cfg`
    // LittleFS partition that zones_config_cfg_fs.c/profiles_cfg_fs.c/
    // pref_cfg_fs.c bridge onto. Placed HERE -- right after recovery_mode
    // is known and before anything else in this file or a later phase runs
    // -- for two reasons: (1) cfg_fs_mount_device() re-derives
    // boot_guard_is_recovery_mode() itself and skips the mount entirely in
    // recovery mode, matching the "RECOVERY MODE... skipped entirely" gate
    // main_control_bringup.c already applies to profile_executor/autotune
    // (same signal, applied to the filesystem too, before either of those
    // subsystems -- or anything else -- gets a chance to read a cfg_fs-
    // backed item off NVS and cache it as though no newer file copy could
    // exist); (2) it is the earliest point after that decision, so no
    // consumer initialized by a later phase (main_control_bringup.c
    // onward) can read its config before the mount has had a chance to
    // land. Never blocks boot: a missing/corrupt/UNFORMATTED `cfg` partition
    // (today's real state on the bench board -- the partition itself is
    // present in partitions.csv and was flashed at c4b4e65d, but has never
    // been through the format step a separate agent owns, so the mount
    // fails until that lands) logs loudly and leaves cfg_fs_is_available()
    // false; every bridge above it degrades to NVS-only, same as before
    // this call existed.
    {
        UBaseType_t stack_words_before = uxTaskGetStackHighWaterMark(NULL);
        esp_err_t cfg_fs_err = cfg_fs_mount_device();
        UBaseType_t stack_words_after = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGI(MAIN_TAG,
                 "cfg_fs_mount_device(): %s -- main task stack high-water mark "
                 "before=%u after=%u words (%u/%u bytes free)",
                 (cfg_fs_err == ESP_OK) ? "mounted" : esp_err_to_name(cfg_fs_err),
                 (unsigned)stack_words_before, (unsigned)stack_words_after,
                 (unsigned)(stack_words_before * sizeof(StackType_t)),
                 (unsigned)(stack_words_after * sizeof(StackType_t)));
        if (cfg_fs_err != ESP_OK && cfg_fs_mount_format_in_progress()) {
            ESP_LOGW(MAIN_TAG, "cfg_fs: a deferred format is scheduled/running (self-healing), not a hard mount failure");
        }

        /* Loud, easy-to-grep boot-log banner for the "found evidence of
         * content, refused to auto-format" outcome -- same "make the
         * refusal visible, not silent" reasoning as get_heap_status()'s
         * UNACKNOWLEDGED CRASH REPORT banner. The web UI's own banner
         * (settings_page.html, GET /api/cfgfs/format_pending) is the
         * operator-facing half; this is the one a bench session tailing the
         * serial log sees immediately, before any browser is even open. */
        if (cfg_fs_mount_format_confirmation_pending()) {
            ESP_LOGE(MAIN_TAG,
                     "**** CFG PARTITION AWAITING FORMAT CONFIRMATION **** reason: %s -- the config "
                     "filesystem is UNAVAILABLE this boot; every cfg_fs-backed setting falls back to "
                     "firmware defaults until an operator confirms via POST /api/cfgfs/format_confirm "
                     "or the Settings page's danger-zone banner",
                     cfg_fs_mount_format_pending_reason());
        }

        /* A blank partition auto-formats in the background now, not here --
         * see cfg_fs_mount.h's doc comment on the deferred-format getters
         * (docs/audits/boot_hang_2026-09-08.md). This boot proceeds with
         * cfg_fs unavailable regardless of which branch above ran; this
         * banner is just the boot-log-visible half of that ("in progress",
         * not "hung") -- GET /api/cfgfs is the live/ongoing half. */
        if (cfg_fs_mount_format_ever_started()) {
            ESP_LOGW(MAIN_TAG,
                     "cfg partition auto-format started in the BACKGROUND (deferred off the boot path) -- "
                     "cfg filesystem unavailable until it completes; poll GET /api/cfgfs (\"format\" section) "
                     "for progress/duration, never assume a hang");
        }
    }

    // watchdog_cfg_init(): the task watchdog itself already exists by this
    // point (CONFIG_ESP_TASK_WDT_EN=y creates it automatically before
    // app_main() runs -- no call in this codebase creates it explicitly), so
    // this is a safe place to re-apply a persisted "panic disabled"
    // dev-mode setting via esp_task_wdt_reconfigure(). See watchdog_cfg.h.
    watchdog_cfg_init();

    // boot_button.h (the "I lost the AP password" long-press recovery hatch)
    // was deleted 2026-09-29 along with the AP-password HMAC scheme it
    // existed to bypass -- ROUTE_TIER_ADMIN is the only gate on OTA routes
    // now and there is nothing left for a BOOT-button window to suspend.

    // Runs once the expander is in its safe state (relays off) but before
    // anything else starts talking on the bus, so the results reflect what is
    // actually wired up. Logged like any other ESP_LOGx and so forwarded to
    // the GUI's Device Console -- which is where a missing SX1509 shows up.
    if (ctx->i2c_bus) {
        i2c_scan_bus(ctx->i2c_bus);
    }

    // --- Shared SPI bus ----------------------------------------------------
    // Initialized here rather than by either of its two drivers, because only
    // the FIRST hal_spi_bus_init() on a host takes effect and the two want
    // incompatible buses: MAX31856 wants SPI_DMA_DISABLED with max_transfer_sz
    // 17 (fine for a 17-byte register burst, fatal for the display, whose
    // scratch buffer is 1440 bytes and needs DMA), while the ILI9488 driver
    // never initializes a bus at all -- it only adds a device. Doing it once,
    // here, with the display's requirements, and having MAX31856_start_all()
    // ADOPT this bus (MAX31856_bus_adopt()) rather than calling
    // hal_spi_bus_init() itself, is the fix for a 2026-09-06 hardware bug:
    // hal_spi_bus_init() used to have an ALREADY_INIT recovery for exactly
    // this sharing case (whichever driver ran second would find
    // ESP_ERR_INVALID_STATE from spi_bus_initialize() and log-and-continue),
    // but that recovery still let the second caller create its OWN
    // spi_owner_t/task -- a second independent single-writer arbiter on one
    // physical bus, the same class of bug hal_i2c_esp_owner.h's own
    // 2026-09-06 fix documents for I2C (see that header's comment). It was
    // reliably hit on this board's every boot (visible in the log as
    // `spi_common: spi_bus_initialize(...) SPI bus already initialized` right
    // before `hal_spi_esp: spi host N already initialized; treating as OK`),
    // and is removed -- hal_spi_bus_init() on an already-open host is now a
    // plain, loud error; a second consumer must adopt instead. Doing it here,
    // ordered before MAX31856_start_all() below (whichever driver runs first
    // no longer matters for correctness, only for which caller must be the
    // real hal_spi_bus_init()), is the only order that cannot depend on which
    // driver happens to start first.
    hal_spi_bus_cfg_t spi_config = {
        .sck_pin = KILN_SPI_SCLK_IO,
        .mosi_pin = KILN_SPI_MOSI_IO,
        .miso_pin = KILN_SPI_MISO_IO,
        .queue_len = 8,
        .task_priority = 5,
        .stack_depth = 4096,
        .core_id = HAL_CORE_ANY,
        .dma_use_psram = KILNCTL_SPI_DMA_USE_PSRAM ? true : false,
        .async_flush = KILNCTL_SPI_ASYNC_FLUSH ? true : false,
        /* DISPLAY_ST7796_PLAN.md 9.2: sized to one full LVGL draw buffer
         * (KILNCTL_LVGL_BUF_ROWS default 40 rows x 480px x 2B/px RGB565 =
         * 38400B), not to ILI9488_SCRATCH_BYTES (1440B) -- this is the SPI
         * host's per-transaction CEILING, not a buffer it allocates. Per
         * §9's "facts established" section, raising it only grows two small
         * DMA descriptor arrays (~24B per 4092B of ceiling, so ~230B total
         * here), not a 38 KB allocation -- cheap against the ~1.6 kB
         * internal-DRAM headroom in §10. Today's ILI9488 codec still chunks
         * every flush at ILI9488_SCRATCH_BYTES (panel_spi.c's
         * disp->chunk_bytes), so this alone changes nothing observable on
         * the currently-attached panel; it is the precondition a future
         * ST7796 zero-copy flush (9.7) needs to DMA a whole LVGL buffer in
         * one transaction instead of 27 chunked ones. */
        .max_transfer_sz = KILNCTL_SPI_MAX_TRANSFER_SZ,
        .dma_chan = HAL_SPI_DMA_AUTO,
    };
    hal_status_t spi_hal_st = hal_spi_bus_init(&ctx->shared_spi_bus, KILN_SPI_HOST, &spi_config);
    esp_err_t spi_err = (spi_hal_st == HAL_OK) ? ESP_OK : ESP_FAIL;
    ctx->spi_err = spi_err;
    if (spi_hal_st != HAL_OK) {
        /* No ALREADY_INIT tolerance any more -- see the comment above this
         * block. thermo_bus.initialized right below is the real safety net
         * regardless: it is checked unconditionally and independently
         * asserts SAFETY_FAULT_SRC_THERMO if no channel actually answers, on
         * top of the boot_fault_sources bit this failure sets just below. */
        ESP_LOGE(MAIN_TAG, "hal_spi_bus_init failed: %s -- thermocouples and display are out",
                 hal_status_to_name(spi_hal_st));
    }

    /* Accumulated across the rest of bring-up and applied to the isolated fault
     * line as soon as the safety link is up (main_control_bringup.c -- it
     * cannot be asserted before the driver that owns the GPIO exists).
     *
     * "Logged and carried on" is the right policy for a board that should still
     * appear on the link with a dead panel; it is NOT the right policy for
     * anything that leaves the kiln unmonitored or uncontrolled. Those failures
     * get a bit here, and the safety processor is told. */
    ctx->boot_fault_sources = 0;

    if (!ctx->io_ready) {
        /* No expander means no relay control at all: the four coils are
         * wherever power-on left them and nothing in this firmware can move
         * them. That is the single worst state this board can boot into, so it
         * is a fault regardless of what else came up. */
        ESP_LOGE(MAIN_TAG, "expander did not come up -- RELAY STATE IS UNKNOWN and uncommandable");
#if CONFIG_KILNCTL_SIM_PLANT
        /* ...except in a simulated-plant build, where the model IS the
         * actuator and there are no coils to be uncertain about. Asserting
         * here would block relay-on globally through relay_authority and make
         * the sim incapable of ever heating, which defeats the entire point
         * of the build (TODO.md 6A.8: provoking each guard on real silicon).
         * Scoped to CONFIG_KILNCTL_SIM_PLANT precisely because suppressing
         * this fault on a board wired to a kiln would be indefensible. */
        ESP_LOGW(MAIN_TAG, "SIM BUILD: not asserting a boot fault for the missing expander -- "
                      "the simulated plant is the actuator");
#else
        ctx->boot_fault_sources |= SAFETY_FAULT_SRC_APP;
#endif
    }
    if (spi_err != ESP_OK) {
        /* No SPI bus means no MAX31856 can be read: the kiln has no temperature
         * measurement on this side of the barrier. Reported as a thermocouple
         * fault because that is exactly what it is from the safety
         * processor's point of view. No ESP_ERR_INVALID_STATE exemption any
         * more -- hal_spi_bus_init() above no longer has an ALREADY_INIT
         * recovery to be benign about (see that call site's comment); a
         * real failure here really does mean no bus. The real coverage for
         * "no channel actually came up" (as opposed to no bus at all) is the
         * thermo_bus.initialized check right below. */
        ctx->boot_fault_sources |= SAFETY_FAULT_SRC_THERMO;
    }

    // --- MAX31856 thermocouple channels (J6) -------------------------------
    // Adopts the shared bus brought up above (MAX31856_bus_adopt(), via
    // MAX31856_start_all()) rather than creating its own -- it does not own
    // the underlying host/owner and will not tear either down. Channels that
    // fail are logged and left out; losing one thermocouple is not a reason
    // to have no thermocouples.
    esp_err_t thermo_err = MAX31856_start_all(&ctx->thermo_bus, ctx->thermo_ch, &ctx->shared_spi_bus);
    if (thermo_err != ESP_OK) {
        ESP_LOGE(MAIN_TAG, "thermocouple bring-up incomplete: %s", esp_err_to_name(thermo_err));
    }
    if (!ctx->thermo_bus.initialized) {
        /* Not one channel answered. A kiln with no readable thermocouple is
         * one the safety processor must know about -- it is the condition its
         * own independent thermocouple exists to cover for. A *partial*
         * failure is deliberately not flagged here: the THERMO READ response
         * reports the dead channels explicitly (spi_failed, NaN), which is
         * finer-grained information than this one wire can carry. */
        ESP_LOGE(MAIN_TAG, "no thermocouple channel came up -- no temperature measurement on this side");
        ctx->boot_fault_sources |= SAFETY_FAULT_SRC_THERMO;
    }

    // ~DRDY is an expander pin, so this is the only thing that can turn the
    // thermocouple driver's "is this reading stale" flag from an elapsed-time
    // guess into the truth.
    if (ctx->io_ready && ctx->thermo_bus.initialized) {
        esp_err_t drdy_err = MAX31856_set_drdy_provider(&ctx->thermo_bus, main_kiln_drdy_provider, &ctx->kio);
        if (drdy_err != ESP_OK) {
            ESP_LOGW(MAIN_TAG, "MAX31856_set_drdy_provider failed: %s", esp_err_to_name(drdy_err));
        }
    }

    // thermo_owner (TODO.md 10.14 Phase 2): the single task that touches the
    // MAX31856 SPI API from here on -- must start before anything that can
    // issue a thermocouple command does (the UART THERMO bridge task, and
    // safety_link's context broadcast, both started later). Started
    // unconditionally on &ctx->thermo_bus regardless of thermo_bus.initialized:
    // the thermocouple daughterboard is not physically attached in this
    // environment, so MAX31856_start_all() above is expected to leave every
    // channel un-initialized here, but thermo_owner_start() only needs a bus
    // pointer to own -- see thermo_owner.h's doc comment. Every per-channel
    // producer fails closed (ESP_ERR_NOT_FOUND, via MAX31856_bus_channel()
    // returning NULL inside the owner task) rather than crashing when a
    // channel is missing, same fail-closed spirit as kiln_io_owner_start().
    esp_err_t thermo_owner_err = thermo_owner_start(&ctx->thermo_bus);
    if (thermo_owner_err != ESP_OK) {
        ESP_LOGE(MAIN_TAG, "thermo_owner_start failed: %s -- no thermocouple commands routed through "
                      "thermo_owner will be reachable this boot",
                 esp_err_to_name(thermo_owner_err));
        startup_fault_note(STARTUP_FAULT_THERMO_OWNER);
    }

    main_heap_stage("i2c+spi+thermo");

    // --- ILI9488 display (J2) ----------------------------------------------
    // Borrows the thermocouple bus's spi_owner_t, which is what keeps a
    // 1440-byte pixel push from being interleaved into the middle of a
    // register burst at a different clock and mode. Normally needs kiln_io
    // for D/C and ~RESET, both of which are expander pins -- except under
    // KILNCTL_DISPLAY_DC_RESET_DIRECT_GPIO bench wiring, where ILI9488_start
    // drives them from bare GPIOs instead (see settings.h/panel_spi.c) and the
    // expander isn't needed for the display at all, so the io_ready gate
    // below is relaxed in that case specifically.
    ctx->display_ready = false;
    // Only D/C strictly requires the expander (ILI9488_init fails without io
    // when dc_gpio < 0 -- there'd be no way to send even a command). ~RESET
    // going through a missing expander is already tolerated: it just falls
    // back to a software reset, same as a genuinely absent reset line today.
    bool display_needs_expander = (DISPLAY_DC_GPIO < 0);
    // HAL Phase 1b (2026-09-06): thermo_bus.owner_initialized/owner (raw
    // spi_owner_t) folded into thermo_bus.hal_bus (hal_spi_bus_t) when
    // MAX31856.c migrated to interface/hal_spi.h -- see MAX31856.h's own
    // comment. `initialized` is the same "bus is up" signal owner_initialized
    // used to be (MAX31856_bus_init() sets it last, only on success).
    // ILI9488_start() below still wants the raw spi_owner_t*, since the
    // display has not migrated to hal_spi.h yet -- hal_spi_esp_get_owner()
    // (hal_spi_esp_owner.h) is the ESP-only bridge back to it, returning NULL
    // on an uninitialized bus.
    spi_owner_t *thermo_owner_for_display =
        ctx->thermo_bus.initialized ? hal_spi_esp_get_owner(&ctx->thermo_bus.hal_bus) : NULL;
    if ((ctx->io_ready || !display_needs_expander) && thermo_owner_for_display) {
        // INVARIANT, deliberate: ILI9488_start() draws the boot splash from
        // this task (app_main), and it must run strictly before
        // lvgl_port_start() (main_bridges_bringup.c) hands the display's SPI
        // device off to the LVGL task. That handoff is where the display
        // gets its single legal draw-call owner (DISPLAY_ST7796_PLAN.md
        // section 8); nothing may draw to the panel from any other task once
        // lvgl_port_start() has run. Before that point app_main is still the
        // only task in the picture, so it drawing the splash here is not a
        // violation -- but do not "fix" this ordering by moving
        // ILI9488_start() later, or by moving lvgl_port_start() earlier:
        // either change puts two tasks in a position to draw at once.
        // i2c_bus (may be NULL if the bus itself failed to come up, see
        // above) is passed through for DISPLAY_ST7796_PLAN.md Sec.6 Step 3's
        // touch-address corroboration -- only read from when
        // KILNCTL_DISPLAY_PANEL_AUTO is selected (not the default); every
        // other build ignores it entirely.
        esp_err_t disp_err = ILI9488_start(&ctx->display, thermo_owner_for_display, KILN_SPI_HOST,
                                            ctx->io_ready ? &ctx->kio : NULL, ctx->i2c_bus);
        if (disp_err != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "ILI9488 bring-up failed: %s", esp_err_to_name(disp_err));
        } else {
            ctx->display_ready = true;
        }
    } else {
        ESP_LOGW(MAIN_TAG, "display skipped: %s",
                 ctx->io_ready ? "SPI bus unavailable"
                          : "expander unavailable (D/C and/or ~RESET still routed through it)");
    }

    // --- Touch controller (same J2 panel as the display above) -------------
    // Shares the I2C bus with the SX1509 expander -- see docs/HARDWARE.md.
    // Which physical controller is on the bus is KILNCTL_TOUCH_FT6336U, a
    // Kconfig symbol DELIBERATELY INDEPENDENT of KILNCTL_DISPLAY_PANEL
    // (2026-09-04, split apart mid color-regression bisect -- see that
    // symbol's own Kconfig help for why one choice used to drive both and
    // why that broke). *_start() logs and returns an error rather than
    // failing app_main if the chip doesn't answer; screen_idle below works
    // fine with touch_ready = false, using only injected (UART/MCP)
    // touches. touch_dev wraps whichever one came up, INCLUDING its
    // swap/invert mapping (touch_dev.h), so lvgl_port_start() never has to
    // know which controller it is or reach into a display descriptor to
    // find its mapping.
#if CONFIG_KILNCTL_TOUCH_FT6336U
    static FT6336UClass ft6336u_touch;
    // FT6336U.c is now a HAL Phase 1b consumer (hal_i2c.h), not a raw
    // driver/i2c_master.h one -- see FT6336U.h's header comment and
    // docs/HW_ABSTRACTION.md. ctx->i2c_bus above is a raw
    // i2c_master_bus_handle_t shared with SX1509/ILI9488/NS2009, which have
    // not migrated yet, so this hal_i2c_bus_t ADOPTS the SAME already-
    // created I2C_NUM_0 port and the SAME i2c_owner_t the SX1509 bring-up
    // above already initialized (ctx->expander.owner), via
    // hal_i2c_esp_adopt() (hal_i2c_esp_owner.h), rather than creating a
    // second bus/owner on the port. 2026-09-06: hal_i2c_bus_init() used to
    // have an ALREADY_INIT recovery path for exactly this sharing case, but
    // IDF's cleanup after i2c_new_master_bus() FAILS on an already-open
    // port leaves the port half released ("acquire bus failed"/"Bus not
    // freed entirely"), which broke every later SX1509 transfer on the same
    // port (LCD D/C via the expander failing, SPI owner queue starving,
    // MAX31856 channel locks timing out, 79s boot). hal_i2c_bus_init() must
    // never be called on this already-open port again -- adopt is the
    // replacement. This also fixes the second latent bug that path had:
    // a second independent i2c_owner_t task on one physical port breaks the
    // single-writer invariant i2c_owner.c exists to protect; adopting keeps
    // exactly one owner task/queue for I2C_NUM_0.
    static hal_i2c_bus_t ft6336u_hal_bus;
    bool ft6336u_hal_bus_ready = false;
#else
    static NS2009Class touch;
#endif
    ctx->touch_ready = false;
    ctx->touch_dev = (touch_dev_t){0};
    if (ctx->i2c_bus) {
#if CONFIG_KILNCTL_TOUCH_FT6336U
        // Adopt the SX1509's already-initialized owner/bus (SX1509_start()
        // above already ran i2c_owner_init() on this exact ctx->i2c_bus
        // handle) instead of creating a second bus/owner on the same port --
        // see the header comment above and hal_i2c_esp_owner.h. The owner
        // task is already registered for stack-margin reporting by
        // SX1509.c ("i2c_owner_sx1509"); nothing new to register here since
        // no new task is created.
        hal_status_t hal_bus_err =
            hal_i2c_esp_adopt(&ft6336u_hal_bus, ctx->i2c_bus, &ctx->expander.owner);
        ft6336u_hal_bus_ready = (hal_bus_err == HAL_OK);
        if (!ft6336u_hal_bus_ready) {
            ESP_LOGE(MAIN_TAG, "hal_i2c_esp_adopt for FT6336U failed: %s",
                     hal_status_to_name(hal_bus_err));
        }
        esp_err_t touch_err = ft6336u_hal_bus_ready
                                   ? FT6336U_start(&ft6336u_touch, &ft6336u_hal_bus)
                                   : ESP_FAIL;
        ctx->touch_ready = (touch_err == ESP_OK);
        if (ctx->touch_ready) {
            ctx->touch_dev.ctx = &ft6336u_touch;
            ctx->touch_dev.read = FT6336U_touch_dev_read;
            ctx->touch_dev.self_calibrating = true;
            ctx->touch_dev.swap_xy = TOUCH_CAP_SWAP_XY;
            ctx->touch_dev.invert_x = TOUCH_CAP_INVERT_X;
            ctx->touch_dev.invert_y = TOUCH_CAP_INVERT_Y;
        } else {
            ESP_LOGW(MAIN_TAG, "FT6336U bring-up failed: %s -- touch input unavailable, synthetic "
                          "injection over the UART bridge still works",
                     esp_err_to_name(touch_err));
            if (touch_err != ESP_ERR_NOT_FOUND) { /* absent controller = board config, not a fault */
                startup_fault_note(STARTUP_FAULT_TOUCH);
            }
            // No FT6336U on the bus (or it failed identity check). Unlike
            // the pre-adopt code, this bus_t is adopted (owner_owned=false,
            // bus_owned=false, see hal_i2c_esp_owner.h) -- deinit here only
            // clears ft6336u_hal_bus's own local storage; it does NOT tear
            // down the shared SX1509 owner task/queue/bus handle, which
            // stays alive for the SX1509/relay path regardless of whether a
            // touch controller answered on the bus.
            if (ft6336u_hal_bus_ready) {
                hal_i2c_bus_deinit(&ft6336u_hal_bus);
                ft6336u_hal_bus_ready = false;
            }
        }
#else
        esp_err_t touch_err = NS2009_start(&touch, ctx->i2c_bus);
        ctx->touch_ready = (touch_err == ESP_OK);
        if (ctx->touch_ready) {
            ctx->touch_dev.ctx = &touch;
            ctx->touch_dev.read = NS2009_touch_dev_read;
            ctx->touch_dev.self_calibrating = false;
            ctx->touch_dev.swap_xy = TOUCH_CAL_SWAP_XY;
            ctx->touch_dev.invert_x = TOUCH_CAL_INVERT_X;
            ctx->touch_dev.invert_y = TOUCH_CAL_INVERT_Y;
        } else {
            ESP_LOGW(MAIN_TAG, "NS2009 bring-up failed: %s -- touch input unavailable, synthetic "
                          "injection over the UART bridge still works",
                     esp_err_to_name(touch_err));
            if (touch_err != ESP_ERR_NOT_FOUND) { /* absent controller = board config, not a fault */
                startup_fault_note(STARTUP_FAULT_TOUCH);
            }
        }
#endif
    }

    // --- Screen idle/blank state machine ------------------------------------
    // TODO.md: auto-blank the panel after CONFIG_KILNCTL_TOUCH_IDLE_TIMEOUT_MS
    // to save its useful life (no backlight control line to switch instead --
    // see docs/HARDWARE.md). Needs only the display; touch_ready gates
    // whether it also sees real presses, not whether it runs at all.
    ctx->screen_idle_ready = false;
    if (ctx->display_ready) {
        // Always NULL here: once lvgl_port_start() runs later, it becomes the
        // sole NS2009 reader and forwards real presses into
        // screen_idle_inject_touch() -- screen_idle must not also poll touch
        // itself, or the two would race the same I2C device.
        esp_err_t idle_err = screen_idle_init(&ctx->screen_idle, &ctx->display, NULL, ctx->recovery_mode);
        if (idle_err != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "screen_idle_init failed: %s -- no auto-blank this boot",
                     esp_err_to_name(idle_err));
            startup_fault_note(STARTUP_FAULT_LCD_BACKLIGHT);
        } else if (screen_idle_start(&ctx->screen_idle) != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "Failed to start screen_idle task -- no auto-blank this boot");
            startup_fault_note(STARTUP_FAULT_LCD_BACKLIGHT);
        } else {
            ctx->screen_idle_ready = true;
        }
    }

    // --- Backlight PWM (DISPLAY_ST7796_PLAN.md 3.4.1) -----------------------
    // Default-OFF (CONFIG_KILNCTL_BACKLIGHT_PWM_ENABLE) -- no-op with the
    // flying wire not fitted. Reads screen_idle's screen_on flag; needs
    // screen_idle_ready, not just display_ready.
    if (ctx->screen_idle_ready) {
        esp_err_t bl_err = backlight_pwm_init(&ctx->backlight, backlight_pwm_query_screen_and_brightness,
                                               &ctx->screen_idle);
        if (bl_err == ESP_OK) {
            if (backlight_pwm_start(&ctx->backlight) != ESP_OK) {
                ESP_LOGE(MAIN_TAG, "Failed to start backlight_pwm task -- backlight stays as bring-up left it");
                startup_fault_note(STARTUP_FAULT_LCD_BACKLIGHT);
            }
        } else if (bl_err != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGE(MAIN_TAG, "backlight_pwm_init failed: %s -- backlight stays as bring-up left it",
                     esp_err_to_name(bl_err));
            startup_fault_note(STARTUP_FAULT_LCD_BACKLIGHT);
        } /* ESP_ERR_NOT_SUPPORTED: flag off, expected, nothing to log */
    }

    main_heap_stage("display+touch");
}
