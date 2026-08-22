/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include <stdlib.h>

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_core_dump.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_temps.h"
#include "dashboard_http.h"
#include "diagnostics_http.h"
#include "backup_http.h"
#include "settings_http.h"
#include "factory_reset.h"
#include "ILI9488.h"
#include "lvgl_port.h"
#include "MAX31856.h"
#include "NS2009.h"
#include "SX1509.h"
#include "screen_idle.h"
#include "autotune_engine.h"
#include "i2c_scan.h"
#include "kiln_io.h"
#include "kiln_io_owner.h"
#include "thermo_owner.h"
#include "mdns.h"
#include "monitor_task.h"
#include "nvs_report.h"
#include "ota_http.h"
#include "profile_executor.h"
#include "profiles_builtin.h"
#include "unit_pref.h"
#include "profiles_http.h"
#include "readiness_http.h"
#include "relay_cycles.h"
#include "rules_http.h"
#include "safety_link.h"
#include "settings.h"
#include "sim_backend.h"
#include "gpio_probe.h"
#include "uart_bridge.h"
#include "uart_log_bridge.h"
#include "uart_owner.h"
#include "uart_protocol.h"
#include "wifi_prov.h"
#include "kiln_cfg_http.h"
#include "kiln_cfg_store.h"
#include "zones_http.h"

static const char *TAG = "app_main";

/* The three MAX31856 ~DRDY lines land on the SX1509 (IO8/IO9/IO10), not on
 * ESP32 GPIOs, so the thermocouple driver cannot see them without owning an
 * I2C device it has no business owning. It takes this callback instead --
 * see MAX31856_set_drdy_provider() -- and falls back to an elapsed-time
 * heuristic whenever it returns false. */
static bool kiln_drdy_provider(uint8_t channel, bool *out_asserted, void *ctx)
{
    kiln_io_t *io = (kiln_io_t *)ctx;
    return kiln_io_get_drdy(io, channel, out_asserted) == ESP_OK;
}

/* The state app_main must leave the board in on *any* path that stops short of
 * a working PC link: relays down, isolated fault line up.
 *
 * app_main returning is not a crash -- FreeRTOS keeps running and so do every
 * task started before the failure -- so "we gave up here" has to be an
 * explicit, positive action rather than the absence of one. Both steps are
 * best-effort by nature (the expander may be the thing that failed), which is
 * why each is reported separately instead of short-circuiting. */
static void kiln_enter_safe_state(kiln_io_t *io, SafetyLinkClass *safety, bool safety_ok,
                                  uint32_t fault_sources, const char *why)
{
    ESP_LOGE(TAG, "entering safe state: %s", why);

    if (io) {
        esp_err_t err = kiln_io_all_relays_off(io);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "could not drop the relays: %s -- RELAY STATE IS UNKNOWN",
                     esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "no expander: relays cannot be commanded and their state is UNKNOWN");
    }

    if (safety_ok) {
        esp_err_t err = safety_link_set_fault_source(safety, fault_sources, true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "could not assert the isolated fault line: %s", esp_err_to_name(err));
        }
    } else {
        /* Worst case on this board: no relay control *and* no way to tell the
         * safety processor. Nothing here can fix it; the log line is so the
         * operator does not have to infer it from silence. */
        ESP_LOGE(TAG, "no safety link: the RP2040 cannot be told the main controller has faulted");
    }
}

/* --- OTA rollback confirmation (UPDATE_PROTOCOL.md sec 3, TODO.md 9.2) ----
 *
 * With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y, an image that just got OTA'd
 * in boots as PENDING_VERIFY and the bootloader will revert to the previous
 * slot on the next boot unless esp_ota_mark_app_valid_cancel_rollback() has
 * been called. UPDATE_PROTOCOL.md is emphatic, twice, that this must NOT be
 * called at the end of app_main() -- reaching the last line of main proves
 * nothing about whether the things that matter actually came up. An image
 * that boots but cannot reach the safety processor is exactly the image that
 * must roll back, and it would sail past a naive "we got to the end" check.
 *
 * So this runs as its own low-priority task, polling until all three of
 * UPDATE_PROTOCOL.md's preconditions are independently true -- NVS readable,
 * the safety link exchanging real frames, the web server up -- rather than
 * being invoked inline from app_main() at a fixed point. NVS and the web
 * server are booleans captured once, from state app_main already
 * established (nvs_report_get()'s mounted flags, dashboard_http_start()'s
 * return); the safety link is the one condition that can only become true
 * some number of poll periods AFTER boot, so it is checked live via
 * safety_link_get_status()->link_up on every pass -- that field is already
 * exactly "a valid status within SAFETY_LINK_UP_PERIODS polls", i.e. frames
 * are currently being exchanged, not just were once. No new safety_link.h
 * getter was needed for this.
 *
 * If nvs_ok or web_ok is false, it was false at boot and stays false for the
 * life of this boot, so the loop never confirms and the image is correctly
 * left PENDING_VERIFY -- that is the rollback doing its job, not a bug. If
 * safety was NULL (safety_link_start() itself failed), the link condition
 * can never become true either, for the same reason. */
typedef struct {
    SafetyLinkClass *safety; /* NULL if safety_link_start() failed this boot --
                              * the link condition can then never be satisfied */
    bool nvs_ok;
    bool web_ok;
} ota_confirm_ctx_t;

#define OTA_CONFIRM_POLL_MS   500
#define OTA_CONFIRM_WARN_MS   10000

static void ota_rollback_confirm_task(void *arg)
{
    ota_confirm_ctx_t ctx = *(ota_confirm_ctx_t *)arg;
    free(arg);

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

        if (ctx.nvs_ok && ctx.web_ok && link_up) {
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "OTA rollback confirmed: NVS readable, safety link exchanging "
                              "frames, web server up -- this image is no longer PENDING_VERIFY");
            } else {
                ESP_LOGE(TAG, "esp_ota_mark_app_valid_cancel_rollback failed: %s "
                              "(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE off, or not an OTA slot?)",
                         esp_err_to_name(err));
            }
            vTaskDelete(NULL);
            return;
        }

        if (!warned && (xTaskGetTickCount() - start) > pdMS_TO_TICKS(OTA_CONFIRM_WARN_MS)) {
            ESP_LOGW(TAG, "OTA rollback not yet confirmed %lu ms after boot: nvs_ok=%d web_ok=%d "
                          "safety_link_up=%d -- image stays PENDING_VERIFY until all three are true",
                     (unsigned long)OTA_CONFIRM_WARN_MS, (int)ctx.nvs_ok, (int)ctx.web_ok,
                     (int)link_up);
            warned = true;
        }

        vTaskDelay(pdMS_TO_TICKS(OTA_CONFIRM_POLL_MS));
    }
}

/* Boot-stage internal-DRAM probe.
 *
 * The largest contiguous 8-bit internal block is 163840 bytes early in boot
 * and a few KB by the time the UART bridge tasks are created -- that collapse,
 * not any free total, is what makes a multi-KB task stack or queue allocation
 * fail (see the MALLOC_CAP_INTERNAL note in the reset-reason block above).
 * Moving LVGL's allocator to PSRAM (lvgl_mem_psram.c) recovered part of it but
 * demonstrably not all, and "somewhere between those two log lines" was as
 * precise as the evidence got.
 *
 * This prints the same figure at each bring-up stage so the drop can be
 * attributed to the stage that causes it rather than inferred. It is two log
 * lines' worth of cost at boot and nothing at all afterwards, so it stays in
 * rather than being added and removed each time this question comes back --
 * it has come back three times now.
 *
 * `delta` is against the previous call, so a stage that costs a large
 * contiguous block is visible directly without subtracting timestamps by hand. */
static void heap_stage(const char *stage)
{
    static size_t s_prev_largest;
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t free8 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int delta = (s_prev_largest == 0) ? 0 : (int)largest - (int)s_prev_largest;
    s_prev_largest = largest;
    ESP_LOGW(TAG, "heap stage %-18s largest=%6u delta=%+7d dram_free=%7u", stage,
             (unsigned)largest, delta, (unsigned)free8);
}

/* Every task creation in this file is a single-line "start" call -- the
 * semaphore/queue/task choreography behind each one lives in that driver's
 * own file (MAX31856_start_all in MAX31856.c, monitor_task_start in
 * monitor_task.c, uart_bridge_start_* in uart_bridge.c, etc.), not here.
 *
 * Only two failures abort app_main: the UART owner and the protocol stack on
 * top of it. Everything else is logged and stepped over -- a board with a dead
 * panel or an absent thermocouple daughterboard is still a board worth having
 * on the link, and the log bridge is what carries that news to the PC.
 *
 * "Stepped over" is not the same as ignored, though. Three of those failures
 * mean the kiln is uncontrolled or unmonitored -- no expander (relay state
 * unknown), no SPI bus and no thermocouple channel at all (no temperature) --
 * and each raises a bit in boot_fault_sources, which is asserted on the
 * isolated fault line as soon as the safety link exists. Both abort paths call
 * kiln_enter_safe_state() first: app_main returning does not stop FreeRTOS, so
 * leaving the board safe has to be an action, not an omission. */
void app_main(void)
{
    // Installed before anything else touches ESP_LOGx, so every line from
    // here on -- including failures during the driver bring-up immediately
    // below -- is captured and queued. Nothing is actually sent yet (there's
    // no uart_protocol_t until further down); see uart_log_bridge_start()
    // below for when the backlog actually flushes.
    uart_log_bridge_early_init();

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
    // uart_log_bridge_early_init() above so it reaches the PC link even
    // before Wi-Fi/UART bring-up finishes. Logged at ERROR so it is never
    // filtered out by a lower default log level, and unconditionally (not
    // just on unexpected reasons) so a normal power-cycle/deliberate reset
    // boot establishes the "this is what a clean boot's line looks like"
    // baseline for comparison.
    {
        esp_reset_reason_t rr = esp_reset_reason();
        const char *rr_name = "UNKNOWN";
        switch (rr) {
            case ESP_RST_UNKNOWN:    rr_name = "UNKNOWN"; break;
            case ESP_RST_POWERON:    rr_name = "POWERON"; break;
            case ESP_RST_EXT:        rr_name = "EXT"; break;
            case ESP_RST_SW:         rr_name = "SW"; break;
            case ESP_RST_PANIC:      rr_name = "PANIC"; break;
            case ESP_RST_INT_WDT:    rr_name = "INT_WDT"; break;
            case ESP_RST_TASK_WDT:   rr_name = "TASK_WDT"; break;
            case ESP_RST_WDT:        rr_name = "WDT"; break;
            case ESP_RST_DEEPSLEEP:  rr_name = "DEEPSLEEP"; break;
            case ESP_RST_BROWNOUT:   rr_name = "BROWNOUT"; break;
            case ESP_RST_SDIO:       rr_name = "SDIO"; break;
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
        ESP_LOGE(TAG, "esp_reset_reason=%d (%s); free_heap=%u free_spiram=%u", (int)rr, rr_name,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        ESP_LOGE(TAG,
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
            ESP_LOGE(TAG, "COREDUMP PRESENT in flash from a previous crash -- decode with "
                          "espcoredump.py info_corefile/dbg_corefile against build/KilnCtrl.elf "
                          "(see firmware/KilnFW/TODO.md for the exact command), then erase it "
                          "with esp_core_dump_image_erase() or it will keep reporting here");
        } else if (cd_err == ESP_ERR_NOT_FOUND || cd_err == ESP_ERR_INVALID_STATE) {
            /* The ordinary case on a clean boot, and the expected result after
             * a normal restart. Logged rather than passed over so that seeing
             * nothing here means the check itself did not run. */
            ESP_LOGE(TAG, "no coredump in flash (esp_core_dump_image_check: %s)",
                     esp_err_to_name(cd_err));
        } else {
            ESP_LOGE(TAG, "coredump check FAILED: %s -- a dump may exist but be unreadable",
                     esp_err_to_name(cd_err));
        }
    }

    // --- ESP32-S3 internal die-temperature sensor (TODO.md 10.7) -----------
    // Independent of every other peripheral here -- no bus, no GPIO, nothing
    // to share or serialize against -- so it comes up this early rather than
    // waiting on I2C/SPI below. Non-fatal like everything else in app_main:
    // board_temps_get() reports esp32_valid=false if this fails, same
    // NULL-tolerant convention as a missing thermo_bus/kiln_io elsewhere in
    // this file. NOT YET VERIFIED AGAINST THE INSTALLED TOOLCHAIN OR REAL
    // HARDWARE THIS PASS -- see board_temps.h's doc comment.
    heap_stage("entry");

    esp_err_t board_temps_err = board_temps_start();
    if (board_temps_err != ESP_OK) {
        ESP_LOGW(TAG, "board_temps_start failed: %s -- no ESP32-S3 die temp this boot",
                 esp_err_to_name(board_temps_err));
    }

    // --- Wi-Fi (station "home" mode / AP mode, see wifi_prov.h) -------------
    // A third client alongside the UART PC link and the safety processor
    // link, not a dependency of either -- started here, independent of and
    // not gating anything below, so a Wi-Fi failure can never delay or block
    // relay-safety-relevant bring-up. wifi_prov_start() is non-blocking: the
    // station join (if any) happens on the Wi-Fi driver's own event loop.
    esp_err_t wifi_err = wifi_prov_start();
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_prov_start failed: %s -- no Wi-Fi this boot, PC link and safety link unaffected",
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
    heap_stage("wifi_prov");

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
        ESP_LOGW(TAG, "mdns_init failed: %s -- no kilnctl.local this boot", esp_err_to_name(mdns_err));
    }

    // --- I2C bus -----------------------------------------------------------
    // ESP-IDF allows exactly one i2c_master_bus_handle_t per physical bus, so
    // it is created here and handed to whoever needs it, rather than by the
    // first driver that happens to want it.
    heap_stage("mdns");

    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t i2c_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t i2c_err = i2c_new_master_bus(&i2c_config, &i2c_bus);
    if (i2c_err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s -- no expander, no relays, no display",
                 esp_err_to_name(i2c_err));
        i2c_bus = NULL;
    }

    // --- SX1509 expander + the board layer over it -------------------------
    // First on the bus and first of the devices, because it owns the relay
    // drives. kiln_io_init loads every relay bit LOW into the data latch
    // *before* it turns those pins into outputs, so the coils never see the
    // expander's power-on latch default of 1 -- see kiln_io.h.
    static SX1509Class expander;
    static kiln_io_t kio;
    bool io_ready = false;
    if (i2c_bus) {
        esp_err_t exp_err = SX1509_start(&expander, i2c_bus);
        if (exp_err != ESP_OK) {
            ESP_LOGE(TAG, "SX1509 bring-up failed: %s", esp_err_to_name(exp_err));
        } else {
            esp_err_t io_err = kiln_io_init(&kio, &expander);
            if (io_err != ESP_OK) {
                ESP_LOGE(TAG, "kiln_io_init failed: %s -- relay state is not guaranteed",
                         esp_err_to_name(io_err));
            } else {
                io_ready = true;
                ESP_LOGI(TAG, "board I/O up: all relays off");
            }
        }
    }

    // Runs once the expander is in its safe state (relays off) but before
    // anything else starts talking on the bus, so the results reflect what is
    // actually wired up. Logged like any other ESP_LOGx and so forwarded to
    // the GUI's Device Console -- which is where a missing SX1509 shows up.
    if (i2c_bus) {
        i2c_scan_bus(i2c_bus);
    }

    // --- Shared SPI bus ----------------------------------------------------
    // Initialized here rather than by either of its two drivers, because only
    // the FIRST spi_bus_initialize() on a host takes effect and the two want
    // incompatible buses: MAX31856_bus_init asks for SPI_DMA_DISABLED with
    // max_transfer_sz 17 (fine for a 17-byte register burst, fatal for the
    // display, whose scratch buffer is 1440 bytes and needs DMA), while the
    // ILI9488 driver never initializes a bus at all -- it only adds a device.
    // Doing it once, here, with the display's requirements is the only order
    // that cannot depend on which driver happens to start first: both then
    // find the host already up, which each handles.
    spi_bus_config_t spi_config = {
        .mosi_io_num = KILN_SPI_MOSI_IO,
        .miso_io_num = KILN_SPI_MISO_IO,
        .sclk_io_num = KILN_SPI_SCLK_IO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = ILI9488_SCRATCH_BYTES,
    };
    esp_err_t spi_err = spi_bus_initialize(KILN_SPI_HOST, &spi_config, SPI_DMA_CH_AUTO);
    if (spi_err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s -- thermocouples and display are out",
                 esp_err_to_name(spi_err));
    }

    /* Accumulated across the rest of bring-up and applied to the isolated fault
     * line as soon as the safety link is up (a few lines below -- it cannot be
     * asserted before the driver that owns the GPIO exists).
     *
     * "Logged and carried on" is the right policy for a board that should still
     * appear on the link with a dead panel; it is NOT the right policy for
     * anything that leaves the kiln unmonitored or uncontrolled. Those failures
     * get a bit here, and the safety processor is told. */
    uint32_t boot_fault_sources = 0;

    if (!io_ready) {
        /* No expander means no relay control at all: the four coils are
         * wherever power-on left them and nothing in this firmware can move
         * them. That is the single worst state this board can boot into, so it
         * is a fault regardless of what else came up. */
        ESP_LOGE(TAG, "expander did not come up -- RELAY STATE IS UNKNOWN and uncommandable");
#if CONFIG_KILNCTL_SIM_PLANT
        /* ...except in a simulated-plant build, where the model IS the
         * actuator and there are no coils to be uncertain about. Asserting
         * here would block relay-on globally through relay_authority and make
         * the sim incapable of ever heating, which defeats the entire point
         * of the build (TODO.md 6A.8: provoking each guard on real silicon).
         * Scoped to CONFIG_KILNCTL_SIM_PLANT precisely because suppressing
         * this fault on a board wired to a kiln would be indefensible. */
        ESP_LOGW(TAG, "SIM BUILD: not asserting a boot fault for the missing expander -- "
                      "the simulated plant is the actuator");
#else
        boot_fault_sources |= SAFETY_FAULT_SRC_APP;
#endif
    }
    if (spi_err != ESP_OK) {
        /* No SPI bus means no MAX31856 can be read: the kiln has no temperature
         * measurement on this side of the barrier. Reported as a thermocouple
         * fault because that is exactly what it is from the safety
         * processor's point of view. */
        boot_fault_sources |= SAFETY_FAULT_SRC_THERMO;
    }

    // --- MAX31856 thermocouple channels (J6) -------------------------------
    // Finds the bus already up and shares it (and, having not created it, will
    // not free it). Channels that fail are logged and left out; losing one
    // thermocouple is not a reason to have no thermocouples.
    static MAX31856BusClass thermo_bus;
    static MAX31856Class thermo_ch[MAX31856_CHANNEL_COUNT];
    esp_err_t thermo_err = MAX31856_start_all(&thermo_bus, thermo_ch);
    if (thermo_err != ESP_OK) {
        ESP_LOGE(TAG, "thermocouple bring-up incomplete: %s", esp_err_to_name(thermo_err));
    }
    if (!thermo_bus.initialized) {
        /* Not one channel answered. A kiln with no readable thermocouple is
         * one the safety processor must know about -- it is the condition its
         * own independent thermocouple exists to cover for. A *partial*
         * failure is deliberately not flagged here: the THERMO READ response
         * reports the dead channels explicitly (spi_failed, NaN), which is
         * finer-grained information than this one wire can carry. */
        ESP_LOGE(TAG, "no thermocouple channel came up -- no temperature measurement on this side");
        boot_fault_sources |= SAFETY_FAULT_SRC_THERMO;
    }

    // ~DRDY is an expander pin, so this is the only thing that can turn the
    // thermocouple driver's "is this reading stale" flag from an elapsed-time
    // guess into the truth.
    if (io_ready && thermo_bus.initialized) {
        esp_err_t drdy_err = MAX31856_set_drdy_provider(&thermo_bus, kiln_drdy_provider, &kio);
        if (drdy_err != ESP_OK) {
            ESP_LOGW(TAG, "MAX31856_set_drdy_provider failed: %s", esp_err_to_name(drdy_err));
        }
    }

    // thermo_owner (TODO.md 10.14 Phase 2): the single task that touches the
    // MAX31856 SPI API from here on -- must start before anything that can
    // issue a thermocouple command does (the UART THERMO bridge task, and
    // safety_link's context broadcast, both started later below). Started
    // unconditionally on &thermo_bus regardless of thermo_bus.initialized:
    // the thermocouple daughterboard is not physically attached in this
    // environment, so MAX31856_start_all() above is expected to leave every
    // channel un-initialized here, but thermo_owner_start() only needs a bus
    // pointer to own -- see thermo_owner.h's doc comment. Every per-channel
    // producer fails closed (ESP_ERR_NOT_FOUND, via MAX31856_bus_channel()
    // returning NULL inside the owner task) rather than crashing when a
    // channel is missing, same fail-closed spirit as kiln_io_owner_start()
    // above.
    esp_err_t thermo_owner_err = thermo_owner_start(&thermo_bus);
    if (thermo_owner_err != ESP_OK) {
        ESP_LOGE(TAG, "thermo_owner_start failed: %s -- no thermocouple commands routed through "
                      "thermo_owner will be reachable this boot",
                 esp_err_to_name(thermo_owner_err));
    }

    heap_stage("i2c+spi+thermo");

    // --- ILI9488 display (J2) ----------------------------------------------
    // Borrows the thermocouple bus's spi_owner_t, which is what keeps a
    // 1440-byte pixel push from being interleaved into the middle of a
    // register burst at a different clock and mode. Normally needs kiln_io
    // for D/C and ~RESET, both of which are expander pins -- except under
    // KILNCTL_DISPLAY_DC_RESET_DIRECT_GPIO bench wiring, where ILI9488_start
    // drives them from bare GPIOs instead (see settings.h/ILI9488.c) and the
    // expander isn't needed for the display at all, so the io_ready gate
    // below is relaxed in that case specifically.
    static ILI9488Class display;
    bool display_ready = false;
    // Only D/C strictly requires the expander (ILI9488_init fails without io
    // when dc_gpio < 0 -- there'd be no way to send even a command). ~RESET
    // going through a missing expander is already tolerated: it just falls
    // back to a software reset, same as a genuinely absent reset line today.
    bool display_needs_expander = (DISPLAY_DC_GPIO < 0);
    if ((io_ready || !display_needs_expander) && thermo_bus.owner_initialized) {
        esp_err_t disp_err =
            ILI9488_start(&display, &thermo_bus.owner, KILN_SPI_HOST, io_ready ? &kio : NULL);
        if (disp_err != ESP_OK) {
            ESP_LOGE(TAG, "ILI9488 bring-up failed: %s", esp_err_to_name(disp_err));
        } else {
            display_ready = true;
        }
    } else {
        ESP_LOGW(TAG, "display skipped: %s",
                 io_ready ? "SPI bus unavailable"
                          : "expander unavailable (D/C and/or ~RESET still routed through it)");
    }

    // --- NS2009 touch controller (same J2 panel as the display above) ------
    // Shares the I2C bus with the SX1509 expander -- see docs/HARDWARE.md,
    // which also has the two still-open pinout questions (D/C-vs-touch-IRQ
    // on pin 1, and a possible SDA/SCL swap on pins 2/3) that leave whether
    // this chip answers at all unsettled on this board revision. NS2009_start
    // logs and returns ESP_ERR_NOT_FOUND rather than failing app_main if it
    // doesn't; screen_idle below works fine with touch_ready = false, using
    // only injected (UART/MCP) touches.
    static NS2009Class touch;
    bool touch_ready = false;
    if (i2c_bus) {
        esp_err_t touch_err = NS2009_start(&touch, i2c_bus);
        touch_ready = (touch_err == ESP_OK);
        if (!touch_ready) {
            ESP_LOGW(TAG, "NS2009 bring-up failed: %s -- touch input unavailable, synthetic "
                          "injection over the UART bridge still works",
                     esp_err_to_name(touch_err));
        }
    }

    // --- Screen idle/blank state machine ------------------------------------
    // TODO.md: auto-blank the panel after CONFIG_KILNCTL_TOUCH_IDLE_TIMEOUT_MS
    // to save its useful life (no backlight control line to switch instead --
    // see docs/HARDWARE.md). Needs only the display; touch_ready gates
    // whether it also sees real presses, not whether it runs at all.
    static screen_idle_t screen_idle;
    bool screen_idle_ready = false;
    if (display_ready) {
        // Always NULL here: once lvgl_port_start() runs below, it becomes the
        // sole NS2009 reader and forwards real presses into
        // screen_idle_inject_touch() -- screen_idle must not also poll touch
        // itself, or the two would race the same I2C device.
        esp_err_t idle_err = screen_idle_init(&screen_idle, &display, NULL);
        if (idle_err != ESP_OK) {
            ESP_LOGE(TAG, "screen_idle_init failed: %s -- no auto-blank this boot",
                     esp_err_to_name(idle_err));
        } else if (screen_idle_start(&screen_idle) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start screen_idle task -- no auto-blank this boot");
        } else {
            screen_idle_ready = true;
        }
    }

    heap_stage("display+touch");

    // --- Safety processor link (opto-isolated UART1 + the fault line) ------
    // Comes up whether or not an RP2040 is answering; a silent far side is
    // link_up = 0, not a startup failure.
    static SafetyLinkClass safety;
    esp_err_t safety_err = safety_link_start(&safety);
    if (safety_err != ESP_OK) {
        /* Not a silent degradation: this is the board's last line of defence
         * and the only channel that survives everything else failing. Losing
         * it means no fault can be signalled to the processor that can cut
         * power independently. */
        ESP_LOGE(TAG, "safety_link_start failed: %s -- THE ISOLATED FAULT LINE IS UNAVAILABLE; "
                      "no main-controller fault can be signalled to the RP2040",
                 esp_err_to_name(safety_err));
    } else if (boot_fault_sources != 0) {
        /* First moment the accumulated bring-up failures can actually reach
         * the safety processor. */
        esp_err_t err = safety_link_set_fault_source(&safety, boot_fault_sources, true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "could not assert boot fault sources 0x%02X: %s",
                     (unsigned)boot_fault_sources, esp_err_to_name(err));
        } else {
            ESP_LOGE(TAG, "isolated fault line asserted at boot, sources 0x%02X",
                     (unsigned)boot_fault_sources);
        }
    }

    // kiln_io_owner (TODO.md 10.14 Phase 1): the single task that writes
    // relay/expander state from here on -- must start before anything that
    // can issue a relay/IO command does (profile_executor, autotune,
    // dashboard_http, the UART IO bridge, all below). Needs both io (just
    // brought up above) and safety (just brought up above); skipped like
    // every other hardware-dependent step here if the expander itself never
    // came up -- there is nothing for it to own.
    //
    // Deliberately does NOT clear io_ready on failure: kiln_enter_safe_state()
    // and uart_bridge_start_link_watchdog() below both call
    // kiln_io_all_relays_off() DIRECTLY, independent of kiln_io_owner on
    // purpose (this file's link-loss/shutdown paths must still work even if
    // the owner task itself is wedged -- see kiln_io_owner.h's top comment).
    // Clearing io_ready here would wrongly take those away too, over a
    // failure (e.g. task/queue allocation) that says nothing about whether
    // the expander itself is reachable. Every kiln_io_owner_command_*()
    // call already fails closed on its own (post_and_wait() checks for a
    // NULL queue) if this didn't succeed -- nothing downstream needs a
    // second guard.
    if (io_ready) {
        esp_err_t owner_err = kiln_io_owner_start(&kio, safety_err == ESP_OK ? &safety : NULL);
        if (owner_err != ESP_OK) {
            ESP_LOGE(TAG, "kiln_io_owner_start failed: %s -- no relay/IO commands routed through "
                          "kiln_io_owner will be reachable this boot (direct fail-safe paths are "
                          "unaffected)",
                     esp_err_to_name(owner_err));
        }
    }

#if CONFIG_KILNCTL_SIM_PLANT
    /* Same reasoning as the missing-expander case above: with no RP2040
     * answering, the link's own fail-safe policy asserts
     * SAFETY_FAULT_SRC_SAFETY_LINK, which blocks relay-on everywhere and
     * leaves a simulated firing unable to command heat. A sim build has no
     * kiln to protect, so the policy is turned off here -- and ONLY here.
     * Everything else about the link (reporting, status, the fault line
     * itself) is untouched. */
    if (safety_err == ESP_OK) {
        esp_err_t policy_err = safety_link_fault_on_link_loss(&safety, false);
        ESP_LOGW(TAG, "SIM BUILD: safety-link loss demoted to non-fault (%s) -- "
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
        ESP_LOGW(TAG, "relay_cycles_init failed: %s -- contact-cycle history not kept this boot",
                 esp_err_to_name(cycles_err));
    }

    heap_stage("safety+io_owner");

    // --- Profile executor (TODO.md section 6) -------------------------------
    // Must come up before dashboard_http_start() below, which registers the
    // /api/profile_exec* routes that call into this module at request time.
    // Same non-fatal, NULL-tolerant convention as everything else here: with
    // no expander/thermo bus it still runs its state machine (useful for
    // exercising the dashboard UI) but withholds heat, per
    // profile_executor.h's doc comment. NOT YET VERIFIED AGAINST REAL RELAY/
    // THERMOCOUPLE HARDWARE -- see docs/PROJECT_STATUS.md.
    esp_err_t exec_err = profile_executor_start(io_ready ? &kio : NULL,
                                                thermo_bus.initialized ? &thermo_bus : NULL,
                                                safety_err == ESP_OK ? &safety : NULL);
    if (exec_err != ESP_OK) {
        ESP_LOGW(TAG, "profile_executor_start failed: %s -- no profile execution this boot",
                 esp_err_to_name(exec_err));
    }

    // ROADMAP.md M5 / LINK_PROTOCOL.md sec 4: gives the safety link's poll
    // task the two hardware pointers it needs to build SAFETY_CMD_PUSH_CONTEXT
    // (relay state + raw thermocouple readings) -- same non-fatal NULL-tolerant
    // wiring convention as everything else here; a board with no io/thermo_bus
    // this boot still gets the frame, just with zone_count=0/relay_now_mask=0.
    if (safety_err == ESP_OK) {
        safety_link_set_context_sources(&safety, io_ready ? &kio : NULL,
                                        thermo_bus.initialized ? &thermo_bus : NULL);
    }

    // --- Autotune engine (TODO.md 6A.4) -------------------------------------
    // Same bring-up convention and NULL-tolerance as profile_executor above;
    // must also come up before dashboard_http_start() (registers /api/autotune*).
    esp_err_t autotune_err = autotune_engine_start(io_ready ? &kio : NULL,
                                                   thermo_bus.initialized ? &thermo_bus : NULL,
                                                   safety_err == ESP_OK ? &safety : NULL);
    if (autotune_err != ESP_OK) {
        ESP_LOGW(TAG, "autotune_engine_start failed: %s -- no autotune this boot", esp_err_to_name(autotune_err));
    }

    // --- Flash-safe executor for the CONTROL/PROFILES/AUTOTUNE bridges -------
    // Started HERE, and deliberately not down with the bridge tasks that use
    // it: its 8192-byte stack must come from internal SRAM (see the HAZARD
    // block in uart_bridge_ext.c) and internal DRAM is at its tightest right
    // after lvgl_port_start() below -- measured 7680 largest free block on
    // 2026-08-20 with the three MAX31856s fitted, which is under 8192 and cost
    // all three of those bridge surfaces for a whole boot. At this stage the
    // largest free block is ~31744. Do not move this later.
    if (uart_bridge_ext_start_flash_worker() != ESP_OK) {
        ESP_LOGE(TAG, "flash-safe executor failed to start (internal SRAM, largest block %u B) -- "
                      "the CONTROL, PROFILES and AUTOTUNE uart bridges (tasks 8/9/10) will NOT "
                      "start this boot: no zone PID/model reads or writes, no fire-profile list/"
                      "save/delete or profile execution over the PC link, no autotune status or "
                      "control. Kiln control from the PC GUI is unavailable; the HTTP dashboard "
                      "and the thermo/io/safety bridges are unaffected.",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }

    heap_stage("executor+autotune");

    // --- Dashboard HTTP API (live status + manual relay control) -----------
    // Registers on the httpd instance wifi_prov_start() already brought up
    // -- if that failed (no Wi-Fi this boot), this just logs and is skipped
    // like every other bring-up step here; a dashboard nobody can reach
    // over Wi-Fi is not a reason to fail app_main. Reads the same kio/
    // thermo_bus this file owns rather than duplicating them, and reuses
    // the safety pointer for the same relay_authority gate the UART bridge
    // uses below.
    esp_err_t dash_err = dashboard_http_start(io_ready ? &kio : NULL,
                                              thermo_bus.initialized ? &thermo_bus : NULL,
                                              safety_err == ESP_OK ? &safety : NULL);
    if (dash_err != ESP_OK) {
        ESP_LOGW(TAG, "dashboard_http_start failed: %s -- no dashboard this boot",
                 esp_err_to_name(dash_err));
    }

    // TODO.md 10.7: board-health IC temperature JSON, deliberately its own
    // route rather than folded into /api/status above -- kiln-process temp
    // and board-health temp are different audiences and mixing them was
    // exactly what 10.7 said to avoid. Same thermo_bus pointer as
    // dashboard_http_start() just above (this module borrows it to call
    // MAX31856_read_all() itself per request rather than owning the bus);
    // esp32_c is independent of thermo_bus and already came up in
    // board_temps_start() near the top of app_main.
    esp_err_t board_temps_http_err = board_temps_http_start(thermo_bus.initialized ? &thermo_bus : NULL);
    if (board_temps_http_err != ESP_OK) {
        ESP_LOGW(TAG, "board_temps_http_start failed: %s -- no /api/board_temps or /board_temps "
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
        ESP_LOGW(TAG, "unit_pref_start failed: %s -- defaulting to Celsius this boot",
                 esp_err_to_name(unit_pref_err));
    }

    esp_err_t zones_err = zones_http_start();
    if (zones_err != ESP_OK) {
        ESP_LOGW(TAG, "zones_http_start failed: %s -- no Thermocouples & Zones page this boot",
                 esp_err_to_name(zones_err));
    }
    esp_err_t rules_err = rules_http_start();
    if (rules_err != ESP_OK) {
        ESP_LOGW(TAG, "rules_http_start failed: %s -- no Relays & Rules page this boot",
                 esp_err_to_name(rules_err));
    }
    // The shipped Digital Fire schedule catalogue's persisted hidden-mask.
    // Must load before profiles_http_start() registers the read paths that
    // consult it, or the first listing after boot would show hidden entries.
    // Non-fatal like every settings module here: a failed load means the
    // catalogue simply shows everything this boot.
    esp_err_t builtin_err = profiles_builtin_start();
    if (builtin_err != ESP_OK) {
        ESP_LOGW(TAG, "profiles_builtin_start failed: %s -- built-in schedules shown unfiltered this boot",
                 esp_err_to_name(builtin_err));
    }
    esp_err_t profiles_err = profiles_http_start();
    if (profiles_err != ESP_OK) {
        ESP_LOGW(TAG, "profiles_http_start failed: %s -- no Profiles page this boot",
                 esp_err_to_name(profiles_err));
    }

    // TODO.md 8.1: the explicit-scope reset/factory-default endpoint. Only
    // needs the shared httpd instance -- no hardware pointers, same as the
    // settings pages just above -- and is likewise never a reason to fail
    // app_main.
    esp_err_t factory_reset_err = factory_reset_http_start();
    if (factory_reset_err != ESP_OK) {
        ESP_LOGW(TAG, "factory_reset_http_start failed: %s -- no reset/factory-default endpoint this boot",
                 esp_err_to_name(factory_reset_err));
    }

    // TODO.md 8.2's boot-time report: capture AFTER every module above that
    // owns an NVS partition (wifi_prov_start() far above, relay_cycles_init(),
    // zones/rules/profiles_http_start() just above) has already run its own
    // nvs_partition_init() -- this only observes what those calls established,
    // it does not itself mount or erase anything.
    nvs_report_capture();

    // TODO.md 9.2 / UPDATE_PROTOCOL.md sec 3: kick off OTA rollback
    // confirmation now that NVS's mounted state and the web server's start
    // result both exist to capture. See ota_rollback_confirm_task() above
    // for why this is a background poller and not an inline call here.
    {
        size_t                       nvs_section_count = 0;
        const nvs_report_section_t *nvs_sections = nvs_report_get(&nvs_section_count);
        bool                         nvs_ok = (nvs_section_count > 0);
        for (size_t i = 0; i < nvs_section_count; i++) {
            if (!nvs_sections[i].mounted) {
                nvs_ok = false;
            }
        }

        ota_confirm_ctx_t *ota_ctx = calloc(1, sizeof(*ota_ctx));
        if (ota_ctx) {
            ota_ctx->safety = (safety_err == ESP_OK) ? &safety : NULL;
            ota_ctx->nvs_ok = nvs_ok;
            ota_ctx->web_ok = (dash_err == ESP_OK);
            if (xTaskCreate(ota_rollback_confirm_task, "ota_confirm", 3072, ota_ctx,
                             tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
                ESP_LOGE(TAG, "Failed to start OTA rollback confirmation task -- this image "
                              "will stay PENDING_VERIFY for the rest of this boot");
                free(ota_ctx);
            }
        } else {
            ESP_LOGE(TAG, "OTA rollback confirmation context alloc failed -- this image "
                          "will stay PENDING_VERIFY for the rest of this boot");
        }
    }

    // TODO.md 8.3: the "is this kiln ready to fire?" status page. Read-only
    // aggregator over the getters every module above already exposes --
    // registered last among the settings pages so every fact it can report
    // (zones_config_is_valid(), nvs_report_get(), dashboard_http_get_hw_ready(),
    // etc.) reflects this boot's actual state rather than a partial one.
    esp_err_t readiness_err = readiness_http_start();
    if (readiness_err != ESP_OK) {
        ESP_LOGW(TAG, "readiness_http_start failed: %s -- no readiness page this boot",
                 esp_err_to_name(readiness_err));
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
        ESP_LOGW(TAG, "diagnostics_http_start failed: %s -- no /diagnostics, /diagnostics/thermo "
                      "or /safety page this boot", esp_err_to_name(diagnostics_err));
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
        ESP_LOGW(TAG, "settings_http_start failed: %s -- no /settings or /settings/manual page "
                      "this boot", esp_err_to_name(settings_err));
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
    esp_err_t ota_http_err = ota_http_start(io_ready ? &kio : NULL,
                                            thermo_bus.initialized ? &thermo_bus : NULL,
                                            safety_err == ESP_OK ? &safety : NULL);
    if (ota_http_err != ESP_OK) {
        ESP_LOGW(TAG, "ota_http_start failed: %s -- no /api/ota/challenge this boot",
                 esp_err_to_name(ota_http_err));
    }

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
        ESP_LOGW(TAG, "kiln_cfg_store_init failed: %s -- saved kiln configs unavailable this boot",
                 esp_err_to_name(kiln_cfg_store_err));
    }
    esp_err_t kiln_cfg_http_err = kiln_cfg_http_start();
    if (kiln_cfg_http_err != ESP_OK) {
        ESP_LOGW(TAG, "kiln_cfg_http_start failed: %s -- no /api/kiln_configs this boot",
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
        ESP_LOGW(TAG, "backup_http_start failed: %s -- no /settings/backup page this boot",
                 esp_err_to_name(backup_err));
    }

    // Development-only /api/sim (fault injection into the simulated plant).
    // Compiles to a no-op returning ESP_OK unless CONFIG_KILNCTL_SIM_PLANT --
    // there is no way to reach this endpoint from a production image.
    esp_err_t sim_err = sim_backend_register_http();
    if (sim_err != ESP_OK) {
        ESP_LOGW(TAG, "sim_backend_register_http failed: %s -- no /api/sim this boot",
                 esp_err_to_name(sim_err));
    }

    heap_stage("http_handlers");

    static monitor_task_t monitor;
    monitor_task_init(&monitor, &expander.owner.task_handle);
    if (monitor_task_start(&monitor) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start heartbeat monitor task");
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
    // Wi-Fi/mDNS/I2C comments above), a missing PC link is exactly that kind
    // of independent, non-fatal peripheral, not a reason to abort boot.
    // pc_link_ready gates only the uart_proto-dependent bridge tasks below;
    // lvgl_port_start() and the fail-safe link watchdog do not take
    // uart_proto and are unaffected either way.
    bool pc_link_ready = false;
    static uart_owner_t uart_owner;
    esp_err_t uart_err = uart_owner_init(&uart_owner, UART_OWNER_PORT_NUM, UART_OWNER_TX_IO,
                                          UART_OWNER_RX_IO, UART_OWNER_BAUD_RATE,
                                          UART_OWNER_QUEUE_LEN, UART_OWNER_TASK_PRIORITY,
                                          UART_OWNER_STACK_SIZE, tskNO_AFFINITY);
    if (uart_err != ESP_OK) {
        ESP_LOGE(TAG, "uart_owner_init failed: %s", esp_err_to_name(uart_err));
        /* No PC link will ever come up. Relays go to their safe state right
         * here rather than waiting on the link watchdog further down (which
         * needs a link to watch and will never get one) -- otherwise the
         * board sits with whatever the expander's power-on latch left
         * energized. Local UI/display bring-up continues below regardless. */
        kiln_enter_safe_state(io_ready ? &kio : NULL, &safety, safety_err == ESP_OK,
                              SAFETY_FAULT_SRC_PC_LINK | SAFETY_FAULT_SRC_APP,
                              "the PC link UART could not be opened");
    }

    static uart_protocol_t uart_proto;
    if (uart_err == ESP_OK) {
        uart_err = uart_protocol_init(&uart_proto, &uart_owner, UART_PROTO_DEVICE_ESP,
                                       UART_PROTOCOL_TASK_PRIORITY, UART_PROTOCOL_STACK_SIZE,
                                       tskNO_AFFINITY);
        if (uart_err != ESP_OK) {
            ESP_LOGE(TAG, "uart_protocol_init failed: %s", esp_err_to_name(uart_err));
            /* Same reasoning as the uart_owner_init failure above: the port
             * exists but nothing can be addressed over it, so no host will
             * ever command these relays. */
            kiln_enter_safe_state(io_ready ? &kio : NULL, &safety, safety_err == ESP_OK,
                                  SAFETY_FAULT_SRC_PC_LINK | SAFETY_FAULT_SRC_APP,
                                  "the PC link protocol stack could not be started");
        } else {
            pc_link_ready = true;
        }
    }

    heap_stage("uart_owner+proto");

    // Everything below through the control/profiles/autotune/wifi/gpio_probe
    // block takes &uart_proto and is only valid to call once pc_link_ready is
    // true (see the PC link block's own NOTE above). lvgl_port_start() is the
    // one exception in this stretch -- it takes &display, not &uart_proto --
    // so it stays outside this gate, exactly as unaffected by a dead PC link
    // as Wi-Fi/mDNS/I2C bring-up already were above.
    if (pc_link_ready) {
        // Starts draining the backlog (everything logged since app_main
        // started) over the wire. Started before the other bridge tasks below
        // purely so buffered boot-time log lines -- e.g. an SX1509 or panel
        // failure -- reach the PC as early as possible; registration order
        // otherwise doesn't matter between these tasks.
        if (uart_log_bridge_start(&uart_proto) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start log uart bridge task");
        }

        if (uart_bridge_start_info_task(&uart_proto) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start info uart bridge task");
        }
        if (uart_bridge_start_system_task(&uart_proto, &uart_owner) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start system uart bridge task");
        }
        if (thermo_bus.initialized) {
            /* Reports the actual error and the free heap: this failure was hit
             * on the bench 2026-08-12 and the old message ("Failed to start
             * thermo uart bridge task") could not distinguish a
             * task-registration refusal (ESP_ERR_INVALID_STATE / task id
             * already taken) from ESP_ERR_NO_MEM, which is the difference
             * between a logic bug and a memory-pressure problem. */
            esp_err_t thermo_task_err = uart_bridge_start_thermo_task(&uart_proto, &thermo_bus);
            if (thermo_task_err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start thermo uart bridge task: %s (free heap %lu B, largest block %u B)",
                         esp_err_to_name(thermo_task_err), (unsigned long)esp_get_free_heap_size(),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
            }
        }
        if (io_ready && uart_bridge_start_io_task(&uart_proto, &kio) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start io uart bridge task");
        }
    }
    heap_stage("uart_bridges_1");

    // Replaces the UART DISPLAY_CMD_* remote-draw path -- LVGL owns the panel
    // now (TODO.md 10.1). uart_bridge_start_display_task() is no longer
    // called here; it stays in uart_bridge.c as dead code for now.
    if (display_ready &&
        lvgl_port_start(&display, touch_ready ? &touch : NULL,
                        screen_idle_ready ? &screen_idle : NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start LVGL display task");
    }
    if (pc_link_ready) {
        if (screen_idle_ready && uart_bridge_start_touch_task(&uart_proto, &screen_idle) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start touch uart bridge task");
        }
        if (safety_err == ESP_OK &&
            uart_bridge_start_safety_task(&uart_proto, &safety) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start safety uart bridge task");
        }
    }

    heap_stage("lvgl_start");

    // CONTROL/PROFILES/AUTOTUNE/WIFI (tasks 8-11): additive UART coverage for
    // everything the HTTP dashboard offers (TODO.md "UART parity with
    // HTTP"), so the PC-side GUI can drive the board without Wi-Fi. None of
    // these own hardware directly -- they route through the same
    // zones_http.c/profiles_http.c/profile_executor.c/autotune_engine.c/
    // wifi_prov.c getters and setters the HTTP handlers already call above,
    // so starting them unconditionally (no io_ready/thermo_bus.initialized
    // gate) matches those modules' own "safe with nothing attached" design.
    if (pc_link_ready) {
        // The three below share the flash-safe executor started at the
        // "executor+autotune" stage above; ESP_ERR_NO_MEM from any of them
        // means that worker is not up (see uart_bridge_ext.c). Named
        // individually and spelled out, because a dead bridge here is silent
        // from the board's point of view -- it only shows up on the PC as
        // "destination task not registered on the peer".
        esp_err_t control_task_err = uart_bridge_start_control_task(&uart_proto);
        if (control_task_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start control uart bridge task: %s%s", esp_err_to_name(control_task_err),
                     control_task_err == ESP_ERR_NO_MEM
                         ? " -- flash-safe executor unavailable; NO zone PID/model reads or writes over the PC link this boot"
                         : "");
        }
        esp_err_t profiles_task_err = uart_bridge_start_profiles_task(&uart_proto);
        if (profiles_task_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start profiles uart bridge task: %s%s", esp_err_to_name(profiles_task_err),
                     profiles_task_err == ESP_ERR_NO_MEM
                         ? " -- flash-safe executor unavailable; NO fire-profile list/save/delete or profile execution over the PC link this boot"
                         : "");
        }
        esp_err_t autotune_task_err = uart_bridge_start_autotune_task(&uart_proto);
        if (autotune_task_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start autotune uart bridge task: %s%s", esp_err_to_name(autotune_task_err),
                     autotune_task_err == ESP_ERR_NO_MEM
                         ? " -- flash-safe executor unavailable; NO autotune status or control over the PC link this boot"
                         : "");
        }
        if (uart_bridge_start_wifi_task(&uart_proto) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start wifi uart bridge task");
        }
        // ESP_ERR_NOT_SUPPORTED here just means CONFIG_KILNCTL_ENABLE_GPIO_PROBE
        // is off (the default) -- not a failure worth an ESP_LOGE. See
        // gpio_probe.h.
        esp_err_t gpio_probe_err = uart_bridge_start_gpio_probe_task(&uart_proto);
        if (gpio_probe_err != ESP_OK && gpio_probe_err != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGE(TAG, "Failed to start gpio_probe uart bridge task: %s",
                     esp_err_to_name(gpio_probe_err));
        }
    }

    heap_stage("uart_bridges_2");

    // --- Fail-safe on loss of the PC link -----------------------------------
    // Started last, so it is watching a link every bridge above can already
    // feed. Until the host's first frame or ACK it holds the board in the
    // link-lost state -- relays off, fault line asserted -- which is the same
    // state a pulled USB cable produces, and the correct one for a controller
    // nobody is currently controlling. See uart_bridge.h for the timing and
    // exactly what "the link went away" means here.
    esp_err_t wd_err = uart_bridge_start_link_watchdog(io_ready ? &kio : NULL,
                                                       safety_err == ESP_OK ? &safety : NULL);
    if (wd_err != ESP_OK) {
        // Nothing left that drops the relays on link loss. That is the one
        // failure in this file worth shouting about: a kiln whose controller
        // has gone away will stay exactly as hot as it was.
        ESP_LOGE(TAG, "PC link watchdog did not start (%s) -- RELAYS WILL NOT DROP ON LINK LOSS",
                 esp_err_to_name(wd_err));
        kiln_enter_safe_state(io_ready ? &kio : NULL, &safety, safety_err == ESP_OK,
                              SAFETY_FAULT_SRC_APP,
                              "no link watchdog, so relays cannot be guaranteed to drop");
    }

    heap_stage("app_main_done");
}
