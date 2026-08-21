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

#include "board_temps.h"
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

#define UI_PAGE_DIAGNOSTICS_PAGE_COUNT 3
#define UI_PAGE_DIAGNOSTICS_PAGE_FIRMWARE 0
#define UI_PAGE_DIAGNOSTICS_PAGE_INTERNAL_RAM 1
#define UI_PAGE_DIAGNOSTICS_PAGE_PSRAM_STORAGE 2

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

static void update_title(void)
{
    static const char *page_names[UI_PAGE_DIAGNOSTICS_PAGE_COUNT] = {
        "Firmware", "Internal RAM", "PSRAM & storage",
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
