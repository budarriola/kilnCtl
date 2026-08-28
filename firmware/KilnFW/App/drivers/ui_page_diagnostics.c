#include "ui_page_diagnostics.h"

#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

#include "MAX31856.h"
#include "board_temps.h"
#include "dashboard_http.h"
#include "safety_link.h" /* SAFETY_LINK_DIAG_STATE_*, SAFETY_LINK_STALE_MS */
#include "safety_trip_words.h"
#include "thermo_owner.h"
#include "unit_pref.h"
#include "kiln_ui.h"
#include "ui_theme.h"
#include "ui_topbar.h"

// TODO.md "Diagnostics / System info page" -- "One place to look before
// reaching for a serial console." That item names two halves: safety-link
// stats (uptime/RX-TX counts/last-seen age) and "the ESP-only half
// (heap/flash/IC temps)". The safety-link half stays out of scope here,
// same as it does for every other page that has run into it (see
// ui_page_safety.c's own header comment) -- it needs M5's link-stats
// surface, which this pass doesn't touch.
//
// 2026-08-21 memory-page expansion: the user asked for "other memorys...
// even if they do not dynamicly update", pointing specifically at the free
// PSRAM row this page already had. That's a lot more numbers than 7 stat
// rows (this page's whole prior content) had room for, so this follows
// ui_page_config.c's own "grow past the no-scroll budget -> add pages, don't
// scroll" precedent (see that file's 2026-08-20 header comment for the
// arithmetic pattern being copied): three stacked page containers, exactly
// one visible via LV_OBJ_FLAG_HIDDEN, stepped by the shared ui_topbar.c
// Prev/Next icons instead of a hand-rolled nav row.
//
// Adopting ui_topbar.c also deletes the old in-content "Back" text button
// (48px: a 44px button plus its UI_THEME_PADDING_PX/2 gap) -- see
// ui_topbar.h's own header comment for why that 48px is the whole reason the
// module exists. That frees the full ~267px content budget (480x320
// landscape; see ui_page_home.c's header comment for that number's
// derivation) for stat rows on every page, no nav row subtracted from any of
// them.
//
// PER-PAGE ROW ARITHMETIC. A stat row (build_stat_row()) renders at a
// measured 23px tall (see the pre-2026-08-21 revision of this file's
// UI_PAGE_DIAGNOSTICS_LIST_HEIGHT_PX comment, which measured this exact
// number on hardware via kiln_ui.c's tap-target dump), with a
// UI_THEME_PADDING_PX/2 = 4px gap between rows. For n rows in one page:
//     n*23 + (n-1)*4 <= 267
// Solving: n=10 gives 230 + 36 = 266px, the largest n that fits; n=11 gives
// 253 + 40 = 293px, over budget. Every page below uses well under that
// ceiling (max 8 rows / 239px) so there is slack against any per-row
// measurement error, rather than running the arithmetic right up to the
// wire the way the old single-page layout eventually did.
//
// "Flash-free" (a single bytes-free number) was in the original TODO wording
// but is still deliberately left out here even though this pass does surface
// partition detail: this board's OTA-partitioned layout (TODO.md 9's dual
// app slots + several dedicated NVS partitions, see
// firmware/KilnFW/partitions.csv and ROADMAP.md M8) doesn't reduce to one
// meaningful "bytes free" figure the way a single-partition device's would.
// What IS shown instead -- the running partition's own label/size and the
// OTA image state -- is honest because each is a well-defined per-partition
// fact, not an invented rollup across partitions that don't share a purpose.
//
// 2026-08-27 FOLD (owner request, verbatim: "the diagnostics pages should
// also contain the safty processor page, the board health, and thermocouple
// fault page's info. remove the other 3 lcd pages when you combine the
// info"). Three more pages join Firmware/Internal RAM/PSRAM & storage above,
// using the SAME Prev/Next-paged, one-topbar-per-screen pattern this file
// already had -- there was no way to fit three more pages' worth of content
// onto the three that existed (see UI_THEME_PAGE_CONTENT_BUDGET_PX's ~267px
// ceiling and this file's own per-page row arithmetic above) without either
// silently dropping content or silently scrolling, both of which this
// codebase's standing no-scroll rule forbids. Six paged screens under one
// "Diagnostics" nav item, Prev/Next-reachable exactly like the first three
// already were, is the honest way to combine three pages' worth of
// content into "the diagnostics pages" without losing any of it.
//
// PER-PAGE CONTENT INVENTORY -- what each removed page showed, and where it
// landed:
//
//   ui_page_safety.c ("Safety Processor", 5 rows): safety temp, enclosure
//   temp, power (W), link version (ESP/Pico, compatible or not), and a
//   combined current-state + last-trip line (2026-08-27: "State: TRIPPED NOW
//   -- ..." or "State: ARMED -- last <reason>, <age>s ago"). ALL FIVE rows
//   carried over verbatim to the new Safety Processor diagnostics page
//   (UI_PAGE_DIAGNOSTICS_PAGE_SAFETY) -- refresh_cb()'s safety block below is
//   a straight copy of that file's own refresh_cb(), same
//   dashboard_get_status() read, same staleness/trip-word handling. Nothing
//   was unique-but-dropped; nothing here already existed on the old
//   Firmware/RAM/PSRAM pages.
//
//   ui_page_board_health.c ("Board Health", 1 + MAX31856_CHANNEL_COUNT
//   rows): ESP32-S3 die temperature, plus one cold-junction row per
//   MAX31856 channel. ALL rows carried over to the new Board Health page
//   (UI_PAGE_DIAGNOSTICS_PAGE_BOARD_HEALTH). The ESP32 die temp is the one
//   piece of overlap with content this file already had: the PSRAM & storage
//   page's own "ESP32-S3 die temp" row (s_esp32_temp_label, unchanged) --
//   that pre-existing row is left as-is rather than duplicated a second
//   time; Board Health's row is the MAX31856 cold-junction detail that page
//   never had.
//
//   ui_page_thermo_faults.c ("Thermocouple Faults", MAX31856_CHANNEL_COUNT
//   channel cards): per-channel SR fault-bit summary (OPEN/OVUV/TCLOW/
//   TCHIGH/CJLOW/CJHIGH/TCRANGE/CJRANGE, or "OK") and a FAULT-pin/SPI status
//   line, sourced from thermo_owner_command_read_all() and indexed by each
//   reading's OWN .channel field -- never by array position, because
//   MAX31856_read_all() (thermo_owner_command_read_all()'s underlying call)
//   packs its output array over failed channels, so position and channel
//   number diverge exactly when a channel is unhealthy. A channel this page
//   never saw in that readback (thermo_bus down, or never initialized) is
//   shown as "no data" / "channel not initialized", DISTINCT from a channel
//   that reported and is faulted -- this distinction is the one a parallel
//   web-side fold of the same three pages found was "the whole point of the
//   page" (see this file's own comment on the refresh loop below): losing it
//   would let an ABSENT channel read as a healthy one. Carried over intact to
//   the new Thermocouple Faults page (UI_PAGE_DIAGNOSTICS_PAGE_THERMO_FAULTS),
//   same seen[]/not-seen split, same by-.channel indexing, unchanged.
//
// FACTS THIS FOLD MUST NOT UNDO (both fixed on the web side earlier the same
// day this LCD fold was done, both re-verified true here):
//   - Cold-junction readings are indexed by MAX31856Reading::channel, never
//     by array position -- MAX31856_read_all() compacts its output over
//     failed channels, so position != channel number the moment any channel
//     is unhealthy. thermo_faults_refresh_cb() below reads readings[i].channel
//     exactly as ui_page_thermo_faults.c's own refresh_cb() did.
//   - A CJRANGE fault invalidates the HOT junction too, not just the
//     cold-junction reading -- board_temps.h's board_temps_get()/
//     board_temps_get_live() already bake this into thermo_cj_valid[], which
//     board_health_refresh_cb() below reads through unchanged (same getter,
//     same validity bit ui_page_board_health.c always used); this page does
//     not re-derive validity from the raw fault bits a second time.
static const char *TAG __attribute__((unused)) = "ui_page_diagnostics";

#define UI_PAGE_DIAGNOSTICS_REFRESH_MS 2000

/* MALLOC_CAP_INTERNAL's largest free block is the number that predicts the
 * failure this board actually hits on the bench: a task creation failing
 * even though esp_get_free_heap_size()'s TOTAL free number still looks
 * plenty large, because the free bytes are fragmented across blocks smaller
 * than what the allocator needs in one contiguous piece. Measured largest
 * free block on this board: 9216 bytes, against LVGL's own 8192-byte task
 * stack requirement (CONFIG_LV_MEM_... task stack size, confirmed against
 * lvgl_port.c's xTaskCreate call for the LVGL task) -- roughly 1KB of
 * margin. 8192 below is that same LVGL stack-size constant, not a guess. */
#define UI_PAGE_DIAGNOSTICS_LVGL_TASK_STACK_BYTES 8192

#define UI_PAGE_DIAGNOSTICS_PAGE_COUNT 7
#define UI_PAGE_DIAGNOSTICS_PAGE_FIRMWARE 0
#define UI_PAGE_DIAGNOSTICS_PAGE_INTERNAL_RAM 1
#define UI_PAGE_DIAGNOSTICS_PAGE_PSRAM_STORAGE 2
/* 2026-08-27 fold -- see this file's header comment for the content
 * inventory each of these three carries over. */
#define UI_PAGE_DIAGNOSTICS_PAGE_SAFETY 3
#define UI_PAGE_DIAGNOSTICS_PAGE_BOARD_HEALTH 4
#define UI_PAGE_DIAGNOSTICS_PAGE_THERMO_FAULTS 5
/* 2026-08-27, owner scope change ("all faults... come with instructions on
 * how to fix them... what was detected wrong"): a dedicated page rather than
 * growing the Safety Processor page's existing 5 rows, which already sit
 * close to the ~267px no-scroll budget and use variable-height wrapped
 * labels -- appending a multi-line cause+remedy+source block to one of them
 * risked clipping content on hardware with no way to notice from a desktop
 * build. A new page is the same "grow past budget -> add a page" rule this
 * file's own header comment already documents for the safety/board-health/
 * thermo-fault fold. */
#define UI_PAGE_DIAGNOSTICS_PAGE_TRIP_DETAIL 6

static ui_topbar_t s_topbar;
static lv_obj_t *s_pages[UI_PAGE_DIAGNOSTICS_PAGE_COUNT];
static uint8_t s_page_index;

/* --- Page 1: Firmware (mostly static -- the user explicitly said
 * non-updating values are fine) -------------------------------------------- */
static lv_obj_t *s_fw_version_label;
static lv_obj_t *s_build_label;
static lv_obj_t *s_uptime_label;
static lv_obj_t *s_reset_reason_label;
static lv_obj_t *s_partition_label;
static lv_obj_t *s_ota_state_label;
static lv_obj_t *s_chip_label;
static lv_obj_t *s_flash_size_label;

/* --- Page 2: Internal RAM (the tight resource on this board) -------------- */
static lv_obj_t *s_int_free_label;
static lv_obj_t *s_int_largest_label;
static lv_obj_t *s_int_min_label;
static lv_obj_t *s_dma_free_label;
static lv_obj_t *s_dma_largest_label;
static lv_obj_t *s_dma_min_label;
static lv_obj_t *s_int_total_label;

/* --- Page 3: PSRAM & storage ----------------------------------------------- */
static lv_obj_t *s_psram_free_label;
static lv_obj_t *s_psram_largest_label;
static lv_obj_t *s_psram_min_label;
static lv_obj_t *s_psram_total_label;
static lv_obj_t *s_nvs_stats_label;
static lv_obj_t *s_esp32_temp_label;

/* --- Page 4: Safety Processor -- carried over verbatim from
 * ui_page_safety.c (see this file's header comment's content inventory). */
static lv_obj_t *s_safety_temp_label;
static lv_obj_t *s_enclosure_temp_label;
static lv_obj_t *s_safety_power_label;
static lv_obj_t *s_link_version_label;
static lv_obj_t *s_trip_label;

/* --- Page 5: Board Health -- carried over verbatim from
 * ui_page_board_health.c. s_esp32_temp_label above (PSRAM & storage page) is
 * a DIFFERENT label showing the same underlying fact; this page's own
 * cold-junction rows are what that page never had. */
static lv_obj_t *s_bh_esp32_label;
static lv_obj_t *s_bh_cj_label[MAX31856_CHANNEL_COUNT];

/* --- Page 6: Thermocouple Faults -- carried over verbatim from
 * ui_page_thermo_faults.c, including its ABSENT-vs-FAULTED distinction (see
 * this file's header comment). */
static lv_obj_t *s_tf_fault_label[MAX31856_CHANNEL_COUNT];
static lv_obj_t *s_tf_status_label[MAX31856_CHANNEL_COUNT];

/* --- Page 7: Trip Detail -- new, 2026-08-27 (owner scope change, see this
 * file's UI_PAGE_DIAGNOSTICS_PAGE_TRIP_DETAIL comment). What was detected,
 * what to do about it, and (S6a only) which of this board's own fault
 * sources actually caused it. */
static lv_obj_t *s_td_reason_label;
static lv_obj_t *s_td_cause_label;
static lv_obj_t *s_td_remedy_label;
static lv_obj_t *s_td_source_label;
static lv_obj_t *s_td_latch_label;

static void update_title(void)
{
    static const char *page_names[UI_PAGE_DIAGNOSTICS_PAGE_COUNT] = {
        "Firmware", "Internal RAM", "PSRAM & storage",
        "Safety Processor", "Board Health", "Thermocouple Faults", "Trip Detail",
    };
    char buf[48];
    snprintf(buf, sizeof(buf), "Diagnostics: %s  %u of %u", page_names[s_page_index],
             (unsigned)(s_page_index + 1), (unsigned)UI_PAGE_DIAGNOSTICS_PAGE_COUNT);
    ui_topbar_set_title(&s_topbar, buf);
}

/* Shows page `index`, hides the rest, and updates the title/prev-next
 * enabled state to match -- same pattern as ui_page_config.c's
 * hub_show_page(), copied because it's the sanctioned way to page a
 * no-scroll LCD screen on this project (see this file's header comment). */
static void show_page(uint8_t index)
{
    if (index >= UI_PAGE_DIAGNOSTICS_PAGE_COUNT) {
        return;
    }
    s_page_index = index;

    for (uint8_t i = 0; i < UI_PAGE_DIAGNOSTICS_PAGE_COUNT; i++) {
        if (!s_pages[i]) {
            continue;
        }
        if (i == index) {
            lv_obj_remove_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    update_title();
    /* Clamp, don't wrap -- ui_topbar.h's own doc comment on
     * ui_topbar_set_prev_enabled()/set_next_enabled() names this as the
     * expected behavior for a paging page. */
    ui_topbar_set_prev_enabled(&s_topbar, index > 0);
    ui_topbar_set_next_enabled(&s_topbar, index + 1 < UI_PAGE_DIAGNOSTICS_PAGE_COUNT);
}

static void prev_cb(lv_event_t *e)
{
    (void)e;
    if (s_page_index > 0) {
        show_page((uint8_t)(s_page_index - 1));
    }
}

static void next_cb(lv_event_t *e)
{
    (void)e;
    if (s_page_index + 1 < UI_PAGE_DIAGNOSTICS_PAGE_COUNT) {
        show_page((uint8_t)(s_page_index + 1));
    }
}

static void format_uptime(char *buf, size_t buf_len)
{
    int64_t uptime_s = esp_timer_get_time() / 1000000;
    int64_t days = uptime_s / 86400;
    int hours = (int)((uptime_s % 86400) / 3600);
    int mins = (int)((uptime_s % 3600) / 60);
    int secs = (int)(uptime_s % 60);
    if (days > 0) {
        snprintf(buf, buf_len, "%lldd %02d:%02d:%02d", (long long)days, hours, mins, secs);
    } else {
        snprintf(buf, buf_len, "%02d:%02d:%02d", hours, mins, secs);
    }
}

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:    return "Power-on";
    case ESP_RST_EXT:        return "External pin";
    case ESP_RST_SW:         return "Software (esp_restart)";
    case ESP_RST_PANIC:      return "Panic/exception";
    case ESP_RST_INT_WDT:    return "Interrupt watchdog";
    case ESP_RST_TASK_WDT:   return "Task watchdog";
    case ESP_RST_WDT:        return "Other watchdog";
    case ESP_RST_DEEPSLEEP:  return "Deep sleep wake";
    case ESP_RST_BROWNOUT:   return "Brownout";
    case ESP_RST_SDIO:       return "SDIO";
    case ESP_RST_USB:        return "USB";
    case ESP_RST_JTAG:       return "JTAG";
    case ESP_RST_EFUSE:      return "eFuse error";
    case ESP_RST_PWR_GLITCH: return "Power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
    case ESP_RST_UNKNOWN:
    default:                 return "Unknown";
    }
}

static const char *ota_state_str(esp_ota_img_states_t s)
{
    switch (s) {
    case ESP_OTA_IMG_NEW:            return "new (unverified)";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending verify";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    case ESP_OTA_IMG_UNDEFINED:
    default:                         return "undefined";
    }
}

/* Everything on the Firmware page is set once at build time -- version,
 * build date, reset reason, running partition, OTA state, and chip/flash
 * facts are all fixed for the life of the running image, so there is
 * nothing for the 2s refresh timer to do for this page (uptime is the one
 * exception and lives in refresh_cb() below like it always has). */
static void build_firmware_statics(void)
{
    char buf[64];

    const esp_app_desc_t *app_desc = esp_app_get_description();
    lv_label_set_text(s_fw_version_label, app_desc ? app_desc->version : "n/a");
    if (app_desc) {
        snprintf(buf, sizeof(buf), "%s %s", app_desc->date, app_desc->time);
        lv_label_set_text(s_build_label, buf);
    } else {
        lv_label_set_text(s_build_label, "n/a");
    }

    lv_label_set_text(s_reset_reason_label, reset_reason_str(esp_reset_reason()));

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        snprintf(buf, sizeof(buf), "%s (%lu KB)", running->label,
                 (unsigned long)(running->size / 1024));
        lv_label_set_text(s_partition_label, buf);

        esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
        if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
            lv_label_set_text(s_ota_state_label, ota_state_str(ota_state));
        } else {
            /* Genuinely can't be determined (e.g. running from `factory`,
             * which carries no OTA state record) -- "n/a" rather than
             * guessing, same rule this file already applies to the die
             * temp below. */
            lv_label_set_text(s_ota_state_label, "n/a");
        }
    } else {
        lv_label_set_text(s_partition_label, "n/a");
        lv_label_set_text(s_ota_state_label, "n/a");
    }

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    const char *model_str = (chip_info.model == CHIP_ESP32S3) ? "ESP32-S3" : "other";
    snprintf(buf, sizeof(buf), "%s rev v%u.%u, %u core%s", model_str,
             (unsigned)(chip_info.revision / 100), (unsigned)(chip_info.revision % 100),
             (unsigned)chip_info.cores, chip_info.cores == 1 ? "" : "s");
    lv_label_set_text(s_chip_label, buf);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        snprintf(buf, sizeof(buf), "%lu MB", (unsigned long)(flash_size / (1024 * 1024)));
        lv_label_set_text(s_flash_size_label, buf);
    } else {
        lv_label_set_text(s_flash_size_label, "n/a");
    }
}

/* NVS usage is read once here too. nvs_get_stats() reflects live state in
 * principle, but the entries it counts (zones/rules/profiles/run_state) only
 * change on an explicit save from elsewhere in the UI, never as a
 * background process this page's 2s timer would need to catch -- so one
 * read at page-build time is the honest cost/benefit call here, same as the
 * rest of this page's static facts. */
static void build_nvs_statics(void)
{
    nvs_stats_t stats;
    char buf[64];
    if (nvs_get_stats(NULL, &stats) == ESP_OK) {
        snprintf(buf, sizeof(buf), "%u used / %u free / %u total",
                 (unsigned)stats.used_entries, (unsigned)stats.free_entries,
                 (unsigned)stats.total_entries);
        lv_label_set_text(s_nvs_stats_label, buf);
    } else {
        lv_label_set_text(s_nvs_stats_label, "n/a");
    }
}

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    char buf[48];

    /* Shared by the Safety Processor block below -- same plain-C getter
     * dashboard_http.c's GET /api/status handler and every other live-data
     * page call (TODO.md 10.1a). */
    dashboard_status_t ds;
    dashboard_get_status(&ds);

    /* Uptime -- Firmware page's one live value. */
    format_uptime(buf, sizeof(buf));
    lv_label_set_text(s_uptime_label, buf);

    /* Internal RAM page -- the resource that actually matters on this
     * board (see this file's header comment). */
    size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t int_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    size_t int_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);

    snprintf(buf, sizeof(buf), "%lu B", (unsigned long)int_free);
    lv_label_set_text(s_int_free_label, buf);

    snprintf(buf, sizeof(buf), "%lu B", (unsigned long)int_largest);
    lv_label_set_text(s_int_largest_label, buf);
    /* 8192 = LVGL's own task stack size (lvgl_port.c's xTaskCreate call for
     * the LVGL task) -- the number a fragmented-but-large-looking total free
     * heap can hide behind. Red below that line, same accent this page uses
     * for any other value that means "look at this now". */
    if (int_largest < UI_PAGE_DIAGNOSTICS_LVGL_TASK_STACK_BYTES) {
        lv_obj_set_style_text_color(s_int_largest_label, UI_THEME_ACCENT_5, 0);
    } else {
        lv_obj_set_style_text_color(s_int_largest_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    }

    snprintf(buf, sizeof(buf), "%lu B", (unsigned long)int_min);
    lv_label_set_text(s_int_min_label, buf);

    snprintf(buf, sizeof(buf), "%lu B", (unsigned long)int_total);
    lv_label_set_text(s_int_total_label, buf);

    size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    size_t dma_min = heap_caps_get_minimum_free_size(MALLOC_CAP_DMA);

    snprintf(buf, sizeof(buf), "%lu B", (unsigned long)dma_free);
    lv_label_set_text(s_dma_free_label, buf);
    snprintf(buf, sizeof(buf), "%lu B", (unsigned long)dma_largest);
    lv_label_set_text(s_dma_largest_label, buf);
    snprintf(buf, sizeof(buf), "%lu B", (unsigned long)dma_min);
    lv_label_set_text(s_dma_min_label, buf);

    /* PSRAM & storage page. MALLOC_CAP_SPIRAM is 0 free on a board with no
     * PSRAM populated/enabled rather than an error -- heap_caps_get_free_
     * size() doesn't distinguish "no PSRAM" from "PSRAM full" by return
     * value alone, so 0 is shown as "0 KB", not invented as "n/a" (unlike
     * board_temps_t's genuine hardware-absent case below, there is no
     * separate validity bit here to honestly report absence with). Same
     * reasoning now applies to the largest-free-block/min-free/total rows
     * added alongside it. */
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t psram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)(psram_free / 1024));
    lv_label_set_text(s_psram_free_label, buf);
    snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)(psram_largest / 1024));
    lv_label_set_text(s_psram_largest_label, buf);
    snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)(psram_min / 1024));
    lv_label_set_text(s_psram_min_label, buf);
    snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)(psram_total / 1024));
    lv_label_set_text(s_psram_total_label, buf);

    board_temps_t bt;
    board_temps_get_live(&bt);
    if (bt.esp32_valid) {
        /* ROADMAP.md 2026-08-21 shared unit preference -- ABSOLUTE
         * conversion, same reasoning as ui_page_board_health.c's identical
         * ESP32-die-temperature row. */
        unit_pref_t unit = unit_pref_get();
        snprintf(buf, sizeof(buf), "%.1f %s", (double)unit_pref_convert(bt.esp32_c, unit, UNIT_PREF_KIND_ABSOLUTE),
                 unit_pref_suffix(unit));
        lv_obj_set_style_text_color(s_esp32_temp_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    } else {
        snprintf(buf, sizeof(buf), "n/a");
        lv_obj_set_style_text_color(s_esp32_temp_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    }
    lv_label_set_text(s_esp32_temp_label, buf);

    /* ---- Safety Processor (folded from ui_page_safety.c) ---------------- */
    if (ds.safety_temp_valid) {
        snprintf(buf, sizeof(buf), "Safety temp: %.1f %s",
                 (double)unit_pref_convert(ds.safety_temp_c, ds.temp_unit, UNIT_PREF_KIND_ABSOLUTE),
                 unit_pref_suffix(ds.temp_unit));
        lv_label_set_text(s_safety_temp_label, buf);
    } else {
        lv_label_set_text(s_safety_temp_label, "Safety temp: ---");
    }
    if (ds.enclosure_temp_valid) {
        snprintf(buf, sizeof(buf), "Enclosure temp: %.1f %s",
                 (double)unit_pref_convert(ds.enclosure_temp_c, ds.temp_unit, UNIT_PREF_KIND_ABSOLUTE),
                 unit_pref_suffix(ds.temp_unit));
        lv_label_set_text(s_enclosure_temp_label, buf);
    } else {
        lv_label_set_text(s_enclosure_temp_label, "Enclosure temp: ---");
    }
    if (ds.power_valid) {
        snprintf(buf, sizeof(buf), "Power: %.0f W", (double)ds.power_w);
        lv_label_set_text(s_safety_power_label, buf);
    } else {
        lv_label_set_text(s_safety_power_label, "Power: ---");
    }
    if (!ds.link_version_known) {
        lv_label_set_text(s_link_version_label, "Link version: ---");
    } else if (ds.link_version_compatible) {
        snprintf(buf, sizeof(buf), "Link version: ESP %u / Pico %u (OK)",
                 (unsigned)ds.self_protocol_version, (unsigned)ds.peer_protocol_version);
        lv_label_set_text(s_link_version_label, buf);
    } else {
        char vbuf[112];
        const char *older = (ds.peer_protocol_version < ds.self_protocol_version) ? "Pico"
                            : (ds.peer_protocol_version > ds.self_protocol_version) ? "ESP"
                                                                                     : "neither";
        snprintf(vbuf, sizeof(vbuf),
                 "Link version: ESP %u / Pico %u -- INCOMPATIBLE, %s is older. Update ESP first.",
                 (unsigned)ds.self_protocol_version, (unsigned)ds.peer_protocol_version, older);
        lv_label_set_text(s_link_version_label, vbuf);
    }
    char trip_tail[40];
    if (!ds.trip_event_ever_received) {
        snprintf(trip_tail, sizeof(trip_tail), "no trip recorded");
    } else {
        snprintf(trip_tail, sizeof(trip_tail), "last %s, %lus ago",
                 safety_trip_words_short(ds.trip_reason),
                 (unsigned long)(ds.trip_event_age_ms / 1000u));
    }
    char trip_buf[96];
    if (!ds.diag_ever_received || ds.diag_age_ms >= SAFETY_LINK_STALE_MS) {
        snprintf(trip_buf, sizeof(trip_buf), "State: UNKNOWN (no fresh diagnostics) -- %s", trip_tail);
    } else {
        const char *state_word;
        switch (ds.diag_state) {
        case SAFETY_LINK_DIAG_STATE_INIT:    state_word = "starting up"; break;
        case SAFETY_LINK_DIAG_STATE_GRACE:   state_word = "startup grace"; break;
        case SAFETY_LINK_DIAG_STATE_ARMED:   state_word = "ARMED"; break;
        case SAFETY_LINK_DIAG_STATE_WARN:    state_word = "ARMED (warning)"; break;
        case SAFETY_LINK_DIAG_STATE_TRIPPED: state_word = "TRIPPED"; break;
        default:                             state_word = "unrecognised"; break;
        }
        if (ds.diag_state == SAFETY_LINK_DIAG_STATE_TRIPPED) {
            snprintf(trip_buf, sizeof(trip_buf), "State: TRIPPED NOW -- %s",
                     safety_trip_words_short(ds.diag_trip_reason));
        } else {
            snprintf(trip_buf, sizeof(trip_buf), "State: %s -- %s", state_word, trip_tail);
        }
    }
    lv_label_set_text(s_trip_label, trip_buf);

    /* ---- Trip Detail (new, 2026-08-27) -----------------------------------
     * Shows the LATCHED trip's cause/remedy/source, not the live diag state
     * above -- deliberately: this is "why did it trip and how do I clear
     * it", answerable long after the underlying condition went away (same
     * "evidence survives" reasoning as safety_link.h's trip_event_* fields
     * themselves). "No trip recorded" only means this ESP has never received
     * a TRIP_EVENT frame this boot's cache lifetime -- NOT that nothing is
     * currently tripped; the State row on the Safety Processor page is the
     * live truth for that. */
    if (!ds.trip_event_ever_received) {
        lv_label_set_text(s_td_reason_label, "Reason: no trip recorded");
        lv_label_set_text(s_td_cause_label, "Detected: --");
        lv_label_set_text(s_td_remedy_label, "To clear: --");
        lv_label_set_text(s_td_source_label, "Fault source: --");
        lv_label_set_text(s_td_latch_label, ""); /* N5 fix: no claim of a latch with no trip on record */
    } else {
        char td_buf[64];
        snprintf(td_buf, sizeof(td_buf), "Reason: %s", safety_trip_words_short(ds.trip_reason));
        lv_label_set_text(s_td_reason_label, td_buf);

        char td_cause_buf[112];
        snprintf(td_cause_buf, sizeof(td_cause_buf), "Detected: %s",
                 safety_trip_words_cause(ds.trip_reason));
        lv_label_set_text(s_td_cause_label, td_cause_buf);

        /* 144, not 112: the longest safety_fault_source_remedy_one() string
         * is 131 bytes and the longest safety_trip_words_remedy() is 109, so
         * "To clear: " + either overflows 112 and the target build refuses it
         * (-Werror=format-truncation). Sized against the tables rather than
         * rounded up by eye -- if a remedy sentence grows past this the build
         * fails again, which is the desired outcome: a silently truncated
         * remedy is a half-instruction to an operator standing at a kiln,
         * and this whole page exists to stop faults being under-explained.
         * The MSVC host tests do NOT run -Wformat-truncation; only the
         * xtensa/arm target builds catch this class. */
        char td_remedy_buf[144];
        /* trip_fault_sources_valid is checked here as well as on the "Fault
         * source (at trip)" line below, and it has to be: without it, an
         * unwitnessed reboot resend carrying a non-zero but untrustworthy
         * mask would print one source's specific remedy directly above a
         * line reading "not captured". Two adjacent labels contradicting
         * each other is worse than either alone -- an operator acts on the
         * specific one. Gate both on the same condition or neither. */
        if (ds.trip_reason == 6u && ds.trip_fault_sources_valid &&
            ds.trip_fault_sources != 0u) {
            /* S6a with a captured source: name the FIRST asserted source's
             * own remedy (safety_fault_source_remedy_one()) rather than the
             * generic "resolve the fault source named below" -- a multi-bit
             * mask still shows the rest via s_td_source_label above; row
             * space here is the LCD no-scroll budget, not a place for a
             * full per-bit list. */
            uint32_t first_bit = ds.trip_fault_sources & (~(ds.trip_fault_sources - 1u));
            snprintf(td_remedy_buf, sizeof(td_remedy_buf), "To clear: %s",
                     safety_fault_source_remedy_one(first_bit));
        } else {
            snprintf(td_remedy_buf, sizeof(td_remedy_buf), "To clear: %s",
                     safety_trip_words_remedy(ds.trip_reason));
        }
        lv_label_set_text(s_td_remedy_label, td_remedy_buf);

        /* S6a only -- every other guard's cause IS the reason text above;
         * safety_link.h's trip_fault_sources field comment explains why only
         * S6a needs this second layer of decode.
         *
         * 2026-08-28 audit fix (N3/N4): mask==0 here is NEVER a genuine
         * "none" for a captured S6a trip -- the ESP must have asserted the
         * isolated fault line to cause the trip at all, so a zero mask means
         * "released before the frame arrived" or "not captured" (this being
         * an unwitnessed reboot resend, safety_link.h's trip_fault_sources_
         * valid), never "no cause". Render that explicitly instead of
         * falling through to safety_fault_source_words()'s "none", which is
         * reserved for the LIVE heat_block_sources rendering elsewhere on
         * this page, where zero genuinely does mean none. */
        if (ds.trip_reason == 6u) {
            /* 160, not 80 -- 2026-08-28 audit fix (N6): all six
             * safety_fault_source_words() strings comma-joined are 141
             * bytes; three sources alone is already ~70. No overflow (the
             * helper is runtime-bounded, not a format string), so this class
             * of truncation is invisible to -Werror=format-truncation --
             * sizing generously here is the only guard. */
            char src_words[160];
            char td_src_buf[192];
            if (ds.trip_fault_sources_valid && ds.trip_fault_sources != 0u) {
                safety_fault_source_words(ds.trip_fault_sources, src_words, sizeof(src_words));
                snprintf(td_src_buf, sizeof(td_src_buf), "Fault source (at trip): %s", src_words);
            } else {
                snprintf(td_src_buf, sizeof(td_src_buf),
                         "Fault source (at trip): not captured -- the source cleared before "
                         "the trip was reported");
            }
            lv_label_set_text(s_td_source_label, td_src_buf);
        } else {
            lv_label_set_text(s_td_source_label, "Fault source: n/a for this guard");
        }

        /* 2026-08-28 audit fix (N5): this used to be set OUTSIDE this
         * else-branch, so it rendered next to "Reason: no trip recorded"
         * above -- claiming a latched trip on a board that has never
         * reported one this boot. Only meaningful once a trip is actually
         * on record. */
        lv_label_set_text(s_td_latch_label,
                           "This trip is LATCHED -- it does not clear on its own, and starting "
                           "a new firing will NOT clear it. Only Clear Trip does, and it is "
                           "refused while the cause is still present.");
    }

    /* ---- Board Health (folded from ui_page_board_health.c) -------------- */
    board_temps_t bt2;
    board_temps_get_live(&bt2);
    if (bt2.esp32_valid) {
        snprintf(buf, sizeof(buf), "%.1f %s", (double)unit_pref_convert(bt2.esp32_c, unit_pref_get(), UNIT_PREF_KIND_ABSOLUTE),
                 unit_pref_suffix(unit_pref_get()));
        lv_obj_set_style_text_color(s_bh_esp32_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    } else {
        snprintf(buf, sizeof(buf), "n/a");
        lv_obj_set_style_text_color(s_bh_esp32_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    }
    lv_label_set_text(s_bh_esp32_label, buf);
    for (uint8_t ch = 0; ch < MAX31856_CHANNEL_COUNT; ch++) {
        bool valid = (ch < bt2.thermo_count) && bt2.thermo_cj_valid[ch];
        if (valid) {
            snprintf(buf, sizeof(buf), "%.1f %s",
                     (double)unit_pref_convert(bt2.thermo_cj_c[ch], unit_pref_get(), UNIT_PREF_KIND_ABSOLUTE),
                     unit_pref_suffix(unit_pref_get()));
            lv_obj_set_style_text_color(s_bh_cj_label[ch], UI_THEME_COLOR_TEXT_PRIMARY, 0);
        } else {
            snprintf(buf, sizeof(buf), "n/a");
            lv_obj_set_style_text_color(s_bh_cj_label[ch], UI_THEME_COLOR_TEXT_SECONDARY, 0);
        }
        lv_label_set_text(s_bh_cj_label[ch], buf);
    }

    /* ---- Thermocouple Faults (folded from ui_page_thermo_faults.c) -----
     * Not-present-in-this-read-back default: a channel that never came up
     * is left as "no data" rather than silently showing stale/zeroed OK
     * text -- see this file's header comment for why this distinction
     * (ABSENT vs FAULTED) must survive the fold intact.
     *
     * MAX31856_read_all() (thermo_owner_command_read_all()'s underlying
     * call) packs readings[] by POSITION over only the channels that
     * actually initialized -- it compacts over failed channels, so index i
     * is not channel i the moment any channel is unhealthy. Every reading
     * is therefore filed by its own readings[i].channel field below, never
     * by array position -- the exact bug this comment exists to prevent. */
    MAX31856Reading readings[MAX31856_CHANNEL_COUNT];
    size_t reading_count = 0;
    memset(readings, 0, sizeof(readings));
    (void)thermo_owner_command_read_all(readings, MAX31856_CHANNEL_COUNT, &reading_count);

    bool seen[MAX31856_CHANNEL_COUNT] = { false };
    for (size_t i = 0; i < reading_count && i < MAX31856_CHANNEL_COUNT; i++) {
        uint8_t ch = readings[i].channel;
        if (ch >= MAX31856_CHANNEL_COUNT) {
            continue; /* defensive; channel is always in range on this board */
        }
        seen[ch] = true;

        char fault_buf[80];
        static const struct { uint8_t mask; const char *name; } bits[] = {
            { MAX31856_MASK_OPEN,    "OPEN" },   { MAX31856_MASK_OVUV,   "OVUV" },
            { MAX31856_MASK_TCLOW,   "TCLOW" },  { MAX31856_MASK_TCHIGH, "TCHIGH" },
            { MAX31856_MASK_CJLOW,   "CJLOW" },  { MAX31856_MASK_CJHIGH, "CJHIGH" },
            { MAX31856_FAULT_TCRANGE, "TCRANGE" }, { MAX31856_FAULT_CJRANGE, "CJRANGE" },
        };
        uint8_t fs = readings[i].fault_status;
        if (fs == 0) {
            snprintf(fault_buf, sizeof(fault_buf), "OK");
        } else {
            fault_buf[0] = '\0';
            bool first = true;
            for (size_t b = 0; b < sizeof(bits) / sizeof(bits[0]); b++) {
                if (fs & bits[b].mask) {
                    size_t used = strlen(fault_buf);
                    snprintf(fault_buf + used, sizeof(fault_buf) - used, "%s%s", first ? "" : ", ", bits[b].name);
                    first = false;
                }
            }
        }
        bool faulted = (fs != 0) || readings[i].spi_failed;
        lv_label_set_text(s_tf_fault_label[ch], fault_buf);
        lv_obj_set_style_text_color(s_tf_fault_label[ch],
                                     faulted ? UI_THEME_ACCENT_5 : UI_THEME_COLOR_TEXT_PRIMARY, 0);

        char status_buf[48];
        snprintf(status_buf, sizeof(status_buf), "FAULT pin: %s   SPI: %s",
                 readings[i].fault_pin_asserted ? "yes" : "no",
                 readings[i].spi_failed ? "FAILED" : "ok");
        lv_label_set_text(s_tf_status_label[ch], status_buf);
        lv_obj_set_style_text_color(s_tf_status_label[ch],
                                     readings[i].spi_failed ? UI_THEME_ACCENT_5 : UI_THEME_COLOR_TEXT_SECONDARY, 0);
    }
    for (uint8_t ch = 0; ch < MAX31856_CHANNEL_COUNT; ch++) {
        if (!seen[ch]) {
            lv_label_set_text(s_tf_fault_label[ch], "no data");
            lv_obj_set_style_text_color(s_tf_fault_label[ch], UI_THEME_COLOR_TEXT_SECONDARY, 0);
            lv_label_set_text(s_tf_status_label[ch], "channel not initialized");
            lv_obj_set_style_text_color(s_tf_status_label[ch], UI_THEME_COLOR_TEXT_SECONDARY, 0);
        }
    }
}

static lv_obj_t *build_stat_row(lv_obj_t *parent, const char *name, lv_color_t accent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(row, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(row, 3, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(row, accent, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(label, name);

    lv_obj_t *value = lv_label_create(row);
    lv_obj_set_style_text_color(value, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(value, "--");

    return value;
}

/* Full-width single-label row -- same shape ui_page_safety.c's
 * build_stat_label() used (one combined "name: value" string per row rather
 * than build_stat_row()'s separate name/value pair), reused here verbatim
 * for the folded Safety Processor page since its rows are already
 * pre-formatted sentences ("Link version: ESP 3 / Pico 3 (OK)"), not a
 * clean name/value split. */
static lv_obj_t *build_full_text_row(lv_obj_t *parent, const char *initial_text)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(row, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(label, initial_text);
    return label;
}

/* One fixed-share channel card for the folded Thermocouple Faults page --
 * same shape ui_page_thermo_faults.c's build_channel_row() used: title,
 * fault summary (wraps), status line, each row sharing the page's remaining
 * height via flex_grow rather than a hard-coded per-row height (that file's
 * own header comment: three fixed 72px rows once overflowed and hid the
 * third channel). */
static void build_thermo_fault_row(lv_obj_t *parent, uint8_t channel, lv_color_t accent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_flex_grow(row, 1);
    lv_obj_set_style_bg_color(row, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(row, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(row, 3, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(row, accent, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *title = lv_label_create(row);
    lv_obj_set_style_text_color(title, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    char title_buf[24];
    snprintf(title_buf, sizeof(title_buf), "Channel %u", (unsigned)channel);
    lv_label_set_text(title, title_buf);

    lv_obj_t *fault_label = lv_label_create(row);
    lv_obj_set_width(fault_label, lv_pct(100));
    lv_label_set_long_mode(fault_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(fault_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(fault_label, "--");
    s_tf_fault_label[channel] = fault_label;

    lv_obj_t *status_label = lv_label_create(row);
    lv_obj_set_width(status_label, lv_pct(100));
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(status_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(status_label, "--");
    s_tf_status_label[channel] = status_label;
}

/* One page: a non-scrollable flex column of stat rows, sized to fill
 * whatever `content` has left. Same "create once, toggle HIDDEN" pattern as
 * ui_page_config.c's build_hub_page() -- see this file's header comment for
 * why paging replaced the single-page internally-scrolling layout. */
static lv_obj_t *build_page(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_width(page, lv_pct(100));
    lv_obj_set_flex_grow(page, 1);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(page, UI_THEME_PADDING_PX / 2, 0);
    return page;
}

lv_obj_t *ui_page_diagnostics_build(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    ui_topbar_create(scr, &(ui_topbar_cfg_t){
        .title = "Diagnostics",
        .back_page = "config",
        .show_home = true,
        .prev_cb = prev_cb,
        .next_cb = next_cb,
    }, &s_topbar);

    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(content, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    for (uint8_t i = 0; i < UI_PAGE_DIAGNOSTICS_PAGE_COUNT; i++) {
        s_pages[i] = build_page(content);
    }

    /* Page 1: Firmware -- 8 rows (8*23 + 7*4 = 212px, well inside the ~267px
     * budget; see this file's header comment for the general arithmetic). */
    lv_obj_t *fw_page = s_pages[UI_PAGE_DIAGNOSTICS_PAGE_FIRMWARE];
    s_fw_version_label = build_stat_row(fw_page, "Firmware version", UI_THEME_ACCENT_1);
    s_build_label = build_stat_row(fw_page, "Build date/time", UI_THEME_ACCENT_2);
    s_uptime_label = build_stat_row(fw_page, "Uptime", UI_THEME_ACCENT_3);
    s_reset_reason_label = build_stat_row(fw_page, "Reset reason", UI_THEME_ACCENT_4);
    s_partition_label = build_stat_row(fw_page, "Running partition", UI_THEME_ACCENT_1);
    s_ota_state_label = build_stat_row(fw_page, "OTA boot state", UI_THEME_ACCENT_2);
    s_chip_label = build_stat_row(fw_page, "Chip", UI_THEME_ACCENT_3);
    s_flash_size_label = build_stat_row(fw_page, "Flash size", UI_THEME_ACCENT_4);

    /* Page 2: Internal RAM -- 7 rows (7*23 + 6*4 = 185px). */
    lv_obj_t *ram_page = s_pages[UI_PAGE_DIAGNOSTICS_PAGE_INTERNAL_RAM];
    s_int_largest_label = build_stat_row(ram_page, "Internal RAM largest free block", UI_THEME_ACCENT_5);
    s_int_free_label = build_stat_row(ram_page, "Internal RAM free", UI_THEME_ACCENT_1);
    s_int_min_label = build_stat_row(ram_page, "Internal RAM free (worst-case)", UI_THEME_ACCENT_2);
    s_int_total_label = build_stat_row(ram_page, "Internal RAM total", UI_THEME_ACCENT_3);
    s_dma_free_label = build_stat_row(ram_page, "DMA-capable free", UI_THEME_ACCENT_4);
    s_dma_largest_label = build_stat_row(ram_page, "DMA-capable largest free block", UI_THEME_ACCENT_1);
    s_dma_min_label = build_stat_row(ram_page, "DMA-capable free (worst-case)", UI_THEME_ACCENT_2);

    /* Page 3: PSRAM & storage -- 6 rows (6*23 + 5*4 = 158px). */
    lv_obj_t *psram_page = s_pages[UI_PAGE_DIAGNOSTICS_PAGE_PSRAM_STORAGE];
    s_psram_free_label = build_stat_row(psram_page, "Free PSRAM", UI_THEME_ACCENT_1);
    s_psram_largest_label = build_stat_row(psram_page, "PSRAM largest free block", UI_THEME_ACCENT_2);
    s_psram_min_label = build_stat_row(psram_page, "PSRAM free (worst-case)", UI_THEME_ACCENT_3);
    s_psram_total_label = build_stat_row(psram_page, "PSRAM total", UI_THEME_ACCENT_4);
    s_nvs_stats_label = build_stat_row(psram_page, "NVS entries (used/free/total)", UI_THEME_ACCENT_1);
    s_esp32_temp_label = build_stat_row(psram_page, "ESP32-S3 die temp", UI_THEME_ACCENT_2);

    /* Page 4: Safety Processor -- folded from ui_page_safety.c (2026-08-27,
     * see this file's header comment's content inventory). 5 rows, same
     * count/shape that page always had, well inside the ~267px budget. */
    lv_obj_t *safety_page = s_pages[UI_PAGE_DIAGNOSTICS_PAGE_SAFETY];
    s_safety_temp_label = build_full_text_row(safety_page, "Safety temp: ---");
    s_enclosure_temp_label = build_full_text_row(safety_page, "Enclosure temp: ---");
    s_safety_power_label = build_full_text_row(safety_page, "Power: ---");
    s_link_version_label = build_full_text_row(safety_page, "Link version: ---");
    s_trip_label = build_full_text_row(safety_page, "State: ---");

    /* Page 5: Board Health -- folded from ui_page_board_health.c. 1 +
     * MAX31856_CHANNEL_COUNT rows (4 on this board), same as that page. */
    lv_obj_t *board_health_page = s_pages[UI_PAGE_DIAGNOSTICS_PAGE_BOARD_HEALTH];
    s_bh_esp32_label = build_stat_row(board_health_page, "ESP32-S3 die temp", UI_THEME_ACCENT_1);
    for (uint8_t ch = 0; ch < MAX31856_CHANNEL_COUNT; ch++) {
        char name[40];
        snprintf(name, sizeof(name), "MAX31856 ch %u cold-junction", (unsigned)ch);
        lv_color_t accent;
        switch (ch % 5) {
        case 0: accent = UI_THEME_ACCENT_2; break;
        case 1: accent = UI_THEME_ACCENT_3; break;
        case 2: accent = UI_THEME_ACCENT_4; break;
        case 3: accent = UI_THEME_ACCENT_1; break;
        default: accent = UI_THEME_ACCENT_2; break;
        }
        s_bh_cj_label[ch] = build_stat_row(board_health_page, name, accent);
    }

    /* Page 7: Trip Detail -- new, 2026-08-27 (see this file's
     * UI_PAGE_DIAGNOSTICS_PAGE_TRIP_DETAIL comment). 5 wrapped full-text
     * rows, same shape as the Safety Processor page's own rows. */
    lv_obj_t *trip_detail_page = s_pages[UI_PAGE_DIAGNOSTICS_PAGE_TRIP_DETAIL];
    s_td_reason_label = build_full_text_row(trip_detail_page, "Reason: --");
    s_td_cause_label = build_full_text_row(trip_detail_page, "Detected: --");
    s_td_remedy_label = build_full_text_row(trip_detail_page, "To clear: --");
    s_td_source_label = build_full_text_row(trip_detail_page, "Fault source: --");
    s_td_latch_label = build_full_text_row(trip_detail_page, "--");

    /* Page 6: Thermocouple Faults -- folded from ui_page_thermo_faults.c.
     * MAX31856_CHANNEL_COUNT channel cards sharing the page's remaining
     * height via flex_grow, same as that page's own `list`. */
    lv_obj_t *thermo_faults_page = s_pages[UI_PAGE_DIAGNOSTICS_PAGE_THERMO_FAULTS];
    lv_color_t tf_accents[3] = { UI_THEME_ACCENT_1, UI_THEME_ACCENT_2, UI_THEME_ACCENT_3 };
    for (uint8_t ch = 0; ch < MAX31856_CHANNEL_COUNT; ch++) {
        build_thermo_fault_row(thermo_faults_page, ch, tf_accents[ch % 3]);
    }

    /* MUST come after content exists -- ui_topbar.h's own usage note: the
     * icon proxy overlaps whatever's beneath it, and LVGL resolves
     * overlapping hit tests by child order. */
    ui_topbar_raise(&s_topbar);

    build_firmware_statics();
    build_nvs_statics();
    show_page(0);

    /* Pages are never torn down (kiln_ui.h's header comment) -- same
     * "create once, keep refreshing forever" timer lifetime as every other
     * live-data page. */
    lv_timer_create(refresh_cb, UI_PAGE_DIAGNOSTICS_REFRESH_MS, NULL);
    refresh_cb(NULL); /* paint real numbers immediately instead of waiting one tick */

    return scr;
}
