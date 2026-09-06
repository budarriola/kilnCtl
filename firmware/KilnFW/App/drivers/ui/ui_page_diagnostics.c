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
//   the new Thermocouple Faults pages (UI_PAGE_DIAGNOSTICS_PAGE_THERMO_FAULTS(ch),
//   one channel per page as of the 2026-08-28 P4-C split -- see that macro's
//   own header comment), same seen[]/not-seen split, same by-.channel
//   indexing, unchanged.
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
//
// 2026-09-04 SECOND CONSOLIDATION (owner request: combine the three
// Thermocouple Faults pages into one; combine Safety Processor and Board
// Health into one, using Board Health's visual treatment where they
// differ). Down from 9 paged screens to 6:
//
//   - Thermocouple Faults (UI_PAGE_DIAGNOSTICS_PAGE_THERMO_FAULTS): the
//     three per-channel pages the 2026-08-28 P4-C split created are back to
//     ONE page, three compact rows -- but not by undoing P4-C's fix. What
//     made three-cards-on-one-page overflow before was the ~123-char prose
//     remedy sentence per channel (~60px) plus a separate "Channel N" title
//     row (~20px). Both are cut here: the remedy collapses to a fixed short
//     phrase ("check wiring/power" / "check TC wiring", each well under one
///    wrapped line), and the title folds into the fault line itself ("Ch0:
//     <faults>") instead of its own row -- exactly the "one header, three
//     rows, not three headers stacked" shape the owner asked for (the
//     topbar's own title is the page's one header; these three rows don't
//     need a second one each). Worst case per row is now the fault line (up
//     to the same 100-char all-8-bits-asserted string this file has always
//     used, plus a 3-char "ChN:" prefix, ~2 wrapped lines/~40px) plus the
//     short status line (bounded ~45 chars, 1 line/~20px) plus the row's own
//     8px pad = ~68px; three rows plus two 4px gaps = ~212px, comfortably
//     under the ~267px budget even before allowing margin for a channel
//     whose fault line wraps to a pessimistic 3rd line (that case: ~248px,
//     still under budget -- see the arithmetic this consolidation's own
//     commit message/report carries in full). The ABSENT-vs-FAULTED
//     distinction and the by-.channel indexing are untouched.
//
//   - Safety & Board Health (UI_PAGE_DIAGNOSTICS_PAGE_SAFETY_BOARD_HEALTH):
//     the Safety Processor page's 5 rows and the Board Health page's
//     cold-junction rows now share one page. Two things changed to make
//     that fit and to give the owner's stated preference (Board Health's
//     look) real effect where the two pages actually differed:
//       1. Board Health's own "ESP32-S3 die temp" row is dropped from this
//          merged page -- it was always a duplicate of the PSRAM & storage
//          page's s_esp32_temp_label (see this file's original 2026-08-27
//          comment above, which already chose not to show that fact twice);
//          folding two more pages together is exactly the moment to stop
//          carrying that duplication into a third place.
//       2. Where the two source pages' presentation differed, Board
//          Health's wins: it used build_stat_row()'s colored left-border
//          accent per row, where the old Safety Processor page's
//          build_full_text_row() rows had no accent at all (see that
//          function's own header comment -- "reused here verbatim" from
//          ui_page_safety.c, plain sentences with no color cue). The
//          Safety rows are still free-text sentences, not clean name/value
//          pairs, so they can't become build_stat_row()s outright -- instead
//          build_full_text_row() grew an `accent` parameter and now paints
//          the same 3px colored left border Board Health's rows always had.
//     The Link Version and State sentences were also shortened (state drops
//     its "last trip, age ago" tail -- that history is not lost, it is the
//     Trip Detail page's whole job, one page over) so this page's worst-case
//     height (5 accent rows + 3 cold-junction rows, one inter-block gap)
//     comes to ~237-257px depending on whether the Link row's borderline
//     43-50 char sentence wraps to one line or two -- either way under the
//     ~267px budget. See the consolidation's own report for the full
//     character-count arithmetic.
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

/* P4-C (opus review, 2026-08-28): a single Thermocouple Faults page holding
 * all MAX31856_CHANNEL_COUNT (3) channel cards, sharing the ~267px content
 * budget via flex_grow(1), no longer fits once the per-channel status label
 * grew from ~48 to up to ~123 chars of prose (dashboard_http.c/
 * ui_page_diagnostics.c's safety_trip_words_cause_numbered() wording pass
 * was a different string, but thermo_faults_refresh_cb()'s own status_buf
 * grew independently the same day) -- worst case per row is title (~20px) +
 * fault_buf up to ~100 chars (~2 wrapped lines, ~40px) + status_buf up to
 * ~123 chars (~3 wrapped lines, ~60px) = ~120px, against the ~89px a 3-way
 * equal flex_grow split actually gives each row (267/3). flex_grow does NOT
 * grow the SHARE with content, only how leftover space divides among
 * children of equal weight, so a row whose content exceeds its share is
 * silently CLIPPED (LVGL's default overflow behavior with LV_OBJ_FLAG_
 * SCROLLABLE removed) -- worse than scrolling, because a clipped fault
 * message can hide exactly the wiring detail an operator needs. Fixed the
 * same way every other over-budget page in this file was fixed: one channel
 * per page instead of three channels sharing one, so each card gets the
 * full ~267px budget to itself. */
#define UI_PAGE_DIAGNOSTICS_PAGE_FIRMWARE 0
#define UI_PAGE_DIAGNOSTICS_PAGE_INTERNAL_RAM 1
#define UI_PAGE_DIAGNOSTICS_PAGE_PSRAM_STORAGE 2
/* 2026-09-04 second consolidation -- see this file's header comment for the
 * arithmetic. Safety Processor and Board Health now share one page; all
 * MAX31856_CHANNEL_COUNT Thermocouple Faults channels are back on one page
 * (P4-C's one-channel-per-page split is no longer needed once the per-row
 * content is trimmed -- see that comment). */
#define UI_PAGE_DIAGNOSTICS_PAGE_SAFETY_BOARD_HEALTH 3
#define UI_PAGE_DIAGNOSTICS_PAGE_THERMO_FAULTS 4
#define UI_PAGE_DIAGNOSTICS_PAGE_TRIP_DETAIL 5
#define UI_PAGE_DIAGNOSTICS_PAGE_COUNT (UI_PAGE_DIAGNOSTICS_PAGE_TRIP_DETAIL + 1)

/* ---- No-scroll budget proofs --------------------------------------------
 * Compile-time mirrors of this file's own header-comment arithmetic, same
 * style ui_page_temperature.c/ui_page_network.c use (check_ui_budget_asserts.ps1
 * pins these exact assertion texts -- see that script's own header comment).
 *
 * Safety & Board Health: a single-line stat/full-text row's height is its own
 * top+bottom pad_all (UI_THEME_PADDING_PX/2, twice) plus one font line --
 * build_stat_row() and build_full_text_row_accent() both use exactly this
 * shape, so one constant covers both row kinds. 5 Safety Processor sentence
 * rows plus MAX31856_CHANNEL_COUNT cold-junction rows, joined by
 * build_page()'s own UI_THEME_PADDING_PX/2 inter-row gap. */
#define UI_PAGE_DIAGNOSTICS_STAT_ROW_HEIGHT_PX \
    (((UI_THEME_PADDING_PX / 2) * 2) + UI_THEME_FONT_LINE_HEIGHT_PX)

#define UI_PAGE_DIAGNOSTICS_SAFETY_BH_TEXT_ROW_COUNT 5
#define UI_PAGE_DIAGNOSTICS_SAFETY_BH_ROW_COUNT \
    (UI_PAGE_DIAGNOSTICS_SAFETY_BH_TEXT_ROW_COUNT + MAX31856_CHANNEL_COUNT)

#define UI_PAGE_DIAGNOSTICS_SAFETY_BH_WORST_CASE_HEIGHT_PX \
    ((UI_PAGE_DIAGNOSTICS_SAFETY_BH_ROW_COUNT * UI_PAGE_DIAGNOSTICS_STAT_ROW_HEIGHT_PX) + \
     ((UI_PAGE_DIAGNOSTICS_SAFETY_BH_ROW_COUNT - 1) * (UI_THEME_PADDING_PX / 2)))

_Static_assert(UI_PAGE_DIAGNOSTICS_SAFETY_BH_WORST_CASE_HEIGHT_PX <= UI_THEME_PAGE_CONTENT_BUDGET_PX,
               "ui_page_diagnostics.c: 5 Safety Processor rows plus MAX31856_CHANNEL_COUNT "
               "cold-junction rows exceed UI_THEME_PAGE_CONTENT_BUDGET_PX (ui_theme.h) -- shrink "
               "a row or move content to another paged screen, don't widen the budget to match.");

/* Thermocouple Faults: MAX31856_CHANNEL_COUNT flex_grow(1) rows sharing one
 * page. Each row's own worst-case CONTENT (not its rendered flex-grow share)
 * is a wrapped fault line (up to
 * UI_PAGE_DIAGNOSTICS_THERMO_FAULT_LINE_MAX_LINES lines), a one-line status
 * sentence, and the row's own top+bottom pad_all -- see
 * build_thermo_fault_row(). Summing every row's worst-case content plus the
 * inter-row gaps and checking that against the budget is the same proof
 * shape ui_page_temperature.c's relay section uses for a fixed-height list;
 * here it stands in for "does the page have enough room to give each
 * flex-grow row at least its own worst-case content", since flex-grow alone
 * provides no such guarantee -- an under-provisioned page silently CLIPS a
 * row's content instead of scrolling or erroring (see this file's P4-C
 * comment above on exactly that failure mode). */
#define UI_PAGE_DIAGNOSTICS_THERMO_FAULT_LINE_MAX_LINES 2
#define UI_PAGE_DIAGNOSTICS_THERMO_STATUS_LINE_MAX_LINES 1
#define UI_PAGE_DIAGNOSTICS_THERMO_FAULT_ROW_CONTENT_PX \
    (((UI_PAGE_DIAGNOSTICS_THERMO_FAULT_LINE_MAX_LINES + UI_PAGE_DIAGNOSTICS_THERMO_STATUS_LINE_MAX_LINES) * \
      UI_THEME_FONT_LINE_HEIGHT_PX) + ((UI_THEME_PADDING_PX / 2) * 2))

#define UI_PAGE_DIAGNOSTICS_THERMO_FAULT_WORST_CASE_HEIGHT_PX \
    ((MAX31856_CHANNEL_COUNT * UI_PAGE_DIAGNOSTICS_THERMO_FAULT_ROW_CONTENT_PX) + \
     ((MAX31856_CHANNEL_COUNT - 1) * (UI_THEME_PADDING_PX / 2)))

_Static_assert(UI_PAGE_DIAGNOSTICS_THERMO_FAULT_WORST_CASE_HEIGHT_PX <= UI_THEME_PAGE_CONTENT_BUDGET_PX,
               "ui_page_diagnostics.c: MAX31856_CHANNEL_COUNT Thermocouple Faults rows' worst-case "
               "content exceeds UI_THEME_PAGE_CONTENT_BUDGET_PX (ui_theme.h) -- a flex_grow(1) row "
               "given less than its own worst-case content silently clips it instead of scrolling; "
               "shrink the per-row content or split channels across more pages, don't widen the "
               "budget to match.");


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

/* --- Page 4: Safety & Board Health -- merged 2026-09-04 (see this file's
 * header comment). The Safety Processor rows are free-text sentences styled
 * with build_full_text_row()'s new accent border (Board Health's visual
 * treatment, per the owner's stated preference); the cold-junction rows are
 * carried over from Board Health as-is (build_stat_row(), same accent
 * style). Board Health's own "ESP32-S3 die temp" row is deliberately NOT
 * carried over -- it duplicated s_esp32_temp_label above (PSRAM & storage
 * page); see the header comment for why dropping it now is the honest call. */
static lv_obj_t *s_safety_temp_label;
static lv_obj_t *s_enclosure_temp_label;
static lv_obj_t *s_safety_power_label;
static lv_obj_t *s_link_version_label;
static lv_obj_t *s_trip_label;
static lv_obj_t *s_bh_cj_label[MAX31856_CHANNEL_COUNT];

/* --- Page 5: Thermocouple Faults -- one page again as of 2026-09-04 (see
 * this file's header comment for why the 2026-08-28 P4-C one-channel-per-
 * page split is no longer needed), still carrying the ABSENT-vs-FAULTED
 * distinction and the by-.channel indexing P4-C's own comment documents. */
static lv_obj_t *s_tf_fault_label[MAX31856_CHANNEL_COUNT];
static lv_obj_t *s_tf_status_label[MAX31856_CHANNEL_COUNT];

/* --- Page 6: Trip Detail -- new, 2026-08-27 (owner scope change, see this
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
        "Safety & Board Health", "Thermocouple Faults", "Trip Detail",
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

/* Trip Detail's Reason row age suffix -- 2026-09-04 P2 fix: 1cf200f's
 * consolidation dropped the "last %s, %lus ago" tail from the Safety & Board
 * Health page's State row (see that comment above trip_buf) on the theory
 * that the Trip Detail page one page over already carries the full trip
 * history -- but it didn't carry the AGE, only reason/cause/remedy/source,
 * so trip_event_age_ms (safety_link.h) became dead: no LCD page showed it
 * any more (dashboard_status_http.c's JSON export is the only remaining
 * consumer). h/m/s, same style as format_uptime() above, rather than raw
 * seconds -- a trip minutes or hours old reading "3612s ago" is harder to
 * parse at a glance than "1h00m ago". */
static void format_trip_age(uint32_t age_ms, char *buf, size_t buf_len)
{
    uint32_t age_s = age_ms / 1000u;
    if (age_s < 60u) {
        snprintf(buf, buf_len, "%lus ago", (unsigned long)age_s);
    } else if (age_s < 3600u) {
        snprintf(buf, buf_len, "%lum%02lus ago", (unsigned long)(age_s / 60u), (unsigned long)(age_s % 60u));
    } else {
        snprintf(buf, buf_len, "%luh%02lum ago", (unsigned long)(age_s / 3600u), (unsigned long)((age_s % 3600u) / 60u));
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
    /* ROADMAP.md "Safety TC display audit, 2026-09-05": show this reading as
     * a distinct thermocouple unless dashboard_http.h's
     * safety_tc_is_separate_sensor says it is CONFIRMED borrowed (mirrors
     * safety_tc_is_separate_physical_sensor()/window.kcSafetyTcIsSeparate on
     * the web side, fail-to-shown) -- only a BORROWED_ZONE/BOTH
     * configuration confirmed by a V3-or-newer frame reads as reused; an
     * older Pico that has never confirmed either way still shows the
     * reading. */
    if (ds.safety_temp_valid && ds.safety_tc_is_separate_sensor) {
        snprintf(buf, sizeof(buf), "Safety temp: %.1f %s",
                 (double)unit_pref_convert(ds.safety_temp_c, ds.temp_unit, UNIT_PREF_KIND_ABSOLUTE),
                 unit_pref_suffix(ds.temp_unit));
        lv_label_set_text(s_safety_temp_label, buf);
    } else if (ds.safety_temp_valid) {
        lv_label_set_text(s_safety_temp_label, "Safety temp: (confirmed borrowed from a zone probe)");
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
    /* 2026-09-04 consolidation: shortened to a compact "ESP99/Pico99" form
     * (no spaces around the slash, no "is older" clause -- the remedy is
     * always "update ESP" regardless of which side is older, so naming the
     * older side added length without adding an actionable fact) so this
     * sentence stays a single wrapped line even at two-digit protocol
     * versions -- see this file's header comment for the character count. */
    if (!ds.link_version_known) {
        lv_label_set_text(s_link_version_label, "Link: ---");
    } else if (ds.link_version_compatible) {
        snprintf(buf, sizeof(buf), "Link: ESP %u / Pico %u (OK)",
                 (unsigned)ds.self_protocol_version, (unsigned)ds.peer_protocol_version);
        lv_label_set_text(s_link_version_label, buf);
    } else {
        char vbuf[80];
        snprintf(vbuf, sizeof(vbuf), "Link: ESP%u/Pico%u INCOMPATIBLE, update ESP",
                 (unsigned)ds.self_protocol_version, (unsigned)ds.peer_protocol_version);
        lv_label_set_text(s_link_version_label, vbuf);
    }
    /* 2026-09-04 consolidation: drops the "last <reason>, <age>s ago" tail
     * the standalone Safety Processor page used to append to every state
     * word -- that history (reason, detected cause, remedy, fault source,
     * latch warning) is the Trip Detail page's entire job one page over, so
     * repeating an abbreviated form of it here was the redundancy this fold
     * was told to drop, not information unique to this page. What remains
     * is the LIVE state word only, which is this page's own fact. */
    char trip_buf[64];
    if (!ds.diag_ever_received || ds.diag_age_ms >= SAFETY_LINK_STALE_MS) {
        snprintf(trip_buf, sizeof(trip_buf), "State: UNKNOWN (no fresh diagnostics)");
    } else {
        switch (ds.diag_state) {
        case SAFETY_LINK_DIAG_STATE_INIT:  snprintf(trip_buf, sizeof(trip_buf), "State: starting up"); break;
        case SAFETY_LINK_DIAG_STATE_GRACE: snprintf(trip_buf, sizeof(trip_buf), "State: startup grace"); break;
        case SAFETY_LINK_DIAG_STATE_ARMED: snprintf(trip_buf, sizeof(trip_buf), "State: ARMED"); break;
        case SAFETY_LINK_DIAG_STATE_WARN:  snprintf(trip_buf, sizeof(trip_buf), "State: ARMED (warning)"); break;
        case SAFETY_LINK_DIAG_STATE_TRIPPED:
            snprintf(trip_buf, sizeof(trip_buf), "State: TRIPPED NOW -- %s",
                     safety_trip_words_short(ds.diag_trip_reason));
            break;
        default: snprintf(trip_buf, sizeof(trip_buf), "State: unrecognised"); break;
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
     * currently tripped; the State row on the Safety & Board Health page is
     * the live truth for that. */
    if (!ds.trip_event_ever_received) {
        lv_label_set_text(s_td_reason_label, "Reason: no trip recorded");
        lv_label_set_text(s_td_cause_label, "Detected: --");
        lv_label_set_text(s_td_remedy_label, "To clear: --");
        lv_label_set_text(s_td_source_label, "Fault source: --");
        lv_label_set_text(s_td_latch_label, ""); /* N5 fix: no claim of a latch with no trip on record */
    } else {
        /* Worst case measured against lv_font_montserrat_14.c's adv_w table
         * (P2 fix, 2026-09-04): longest safety_trip_words_short() string
         * ("S13 borrowed TC dead") plus "Reason: " is ~217px; the longest
         * age suffix (hours case, e.g. " (1193h59m ago)") is ~112px more,
         * ~329px combined -- comfortably one line in this row's ~445px
         * width, so no wrap-budget change is needed here. */
        char age_buf[24];
        format_trip_age(ds.trip_event_age_ms, age_buf, sizeof(age_buf));
        char td_buf[96];
        snprintf(td_buf, sizeof(td_buf), "Reason: %s (%s)", safety_trip_words_short(ds.trip_reason), age_buf);
        lv_label_set_text(s_td_reason_label, td_buf);

        /* 2026-08-28: numbered cause (safety_trip_words_cause_numbered()) --
         * inner buffer is 320, not 200: the S3/S9 sentence prose is ~130
         * bytes plus four %.2f floats, and a pathological runtime magnitude
         * (%.2f of 1e38 is ~45 chars) can push a single conversion well past
         * a "normal" amps reading -- -Werror=format-truncation cannot catch
         * this because the values are runtime floats. Outer buf is 340 (9
         * bytes of "Detected: " prefix plus the 320 plus NUL, rounded up)
         * and this label is not on the tight no-scroll row grid the stat
         * rows are (it wraps within its own container, same as
         * s_td_remedy_label below), so headroom is cheap and correctness
         * against -Werror=format-truncation is not. */
        char td_cause_num_buf[320];
        char td_cause_buf[340];
        snprintf(td_cause_buf, sizeof(td_cause_buf), "Detected: %s",
                 safety_trip_words_cause_numbered(ds.trip_reason, ds.trip_safety_tc_c,
                                                   ds.trip_deciding_threshold,
                                                   ds.trip_current_a, ds.trip_context_age_100ms,
                                                   td_cause_num_buf, sizeof(td_cause_num_buf)));
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

    /* ---- Board Health (folded from ui_page_board_health.c; the ESP32-S3
     * die temp row is NOT repeated here -- see this file's header comment
     * and s_bh_cj_label's own comment for why: s_esp32_temp_label on the
     * PSRAM & storage page already shows this exact fact.) ---------------- */
    board_temps_t bt2;
    board_temps_get_live(&bt2);
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

        /* 2026-08-28: this used to print raw abbreviations (OPEN, TCLOW...)
         * -- a code, not an explanation, exactly the offender ROADMAP.md
         * M13 calls out ("what was detected wrong", not a bare flag). Words
         * match main_page.html's FAULT_BITS table verbatim (same reasoning
         * as safety_trip_words.h's own header comment: two independently
         * maintained copies, C and JS, kept in sync by hand since this
         * firmware cannot #include a C header into a web page).
         *
         * 2026-09-04: "ChN: " prefixed onto this line rather than a separate
         * title row -- with all MAX31856_CHANNEL_COUNT channels back on one
         * page (see this file's header comment), a per-channel title row is
         * exactly the repeated chrome the consolidation was told to drop in
         * favor of one header (the topbar) and compact rows. */
        char fault_buf[168];
        static const struct { uint8_t mask; const char *word; } bits[] = {
            { MAX31856_MASK_OPEN,     "open circuit" },   { MAX31856_MASK_OVUV,   "over/under voltage" },
            { MAX31856_MASK_TCLOW,    "TC low" },         { MAX31856_MASK_TCHIGH, "TC high" },
            { MAX31856_MASK_CJLOW,    "CJ low" },         { MAX31856_MASK_CJHIGH, "CJ high" },
            { MAX31856_FAULT_TCRANGE, "TC out of range" }, { MAX31856_FAULT_CJRANGE, "CJ out of range" },
        };
        uint8_t fs = readings[i].fault_status;
        size_t prefix_len = (size_t)snprintf(fault_buf, sizeof(fault_buf), "Ch%u: ", (unsigned)ch);
        if (fs == 0) {
            snprintf(fault_buf + prefix_len, sizeof(fault_buf) - prefix_len, "OK");
        } else {
            bool first = true;
            for (size_t b = 0; b < sizeof(bits) / sizeof(bits[0]); b++) {
                if (fs & bits[b].mask) {
                    size_t used = strlen(fault_buf);
                    snprintf(fault_buf + used, sizeof(fault_buf) - used, "%s%s", first ? "" : ", ", bits[b].word);
                    first = false;
                }
            }
        }
        bool faulted = (fs != 0) || readings[i].spi_failed;
        lv_label_set_text(s_tf_fault_label[ch], fault_buf);
        lv_obj_set_style_text_color(s_tf_fault_label[ch],
                                     faulted ? UI_THEME_ACCENT_5 : UI_THEME_COLOR_TEXT_PRIMARY, 0);

        /* 2026-09-04: shortened from a ~123-char instructional sentence to a
         * fixed short phrase -- see this file's header comment for the row
         * arithmetic this was required to make fit once three channels
         * shared one page again. The distinction that mattered (wiring/
         * power fault vs. thermocouple/wiring fault) is kept; the generic
         * "it should clear on its own" prose is not, since it was the same
         * sentence regardless of which specific bit fired. */
        char status_buf[64];
        if (readings[i].spi_failed) {
            /* Not a reading at all -- the SPI transaction itself failed, so
             * this is NOT the same thing as a decoded SR fault bit above
             * (ABSENT vs FAULTED, per this pass's own instructions: a
             * channel that never reported is not a healthy channel, and
             * must not be worded like one that reported and found a
             * problem). */
            snprintf(status_buf, sizeof(status_buf),
                     "FAULT pin: %s  SPI: FAILED (check wiring/power)",
                     readings[i].fault_pin_asserted ? "yes" : "no");
        } else if (fs != 0) {
            snprintf(status_buf, sizeof(status_buf),
                     "FAULT pin: %s  SPI: ok (check TC wiring)",
                     readings[i].fault_pin_asserted ? "yes" : "no");
        } else {
            snprintf(status_buf, sizeof(status_buf), "FAULT pin: %s  SPI: ok",
                     readings[i].fault_pin_asserted ? "yes" : "no");
        }
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
 * than build_stat_row()'s separate name/value pair), for a page whose rows
 * are pre-formatted sentences ("Link: ESP 3 / Pico 3 (OK)"), not a clean
 * name/value split.
 *
 * 2026-09-04: takes an `accent` color and paints the same 3px colored
 * left-border build_stat_row() has always used, where the original
 * ui_page_safety.c-derived version had none -- the owner's explicit "use
 * Board Health's theme" call when the two pages merged (see this file's
 * header comment). Trip Detail's 5 rows still want the old plain look (no
 * accent, nothing about that page changed in this pass), so they go through
 * build_full_text_row() below, which forwards UI_THEME_COLOR_CARD as the
 * border color -- same color as the row's own background, i.e. invisible. */
static lv_obj_t *build_full_text_row_accent(lv_obj_t *parent, const char *initial_text, lv_color_t accent)
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

    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(label, initial_text);
    return label;
}

static lv_obj_t *build_full_text_row(lv_obj_t *parent, const char *initial_text)
{
    return build_full_text_row_accent(parent, initial_text, UI_THEME_COLOR_CARD);
}

/* One fixed-share channel card for the merged Thermocouple Faults page --
 * same shape ui_page_thermo_faults.c's build_channel_row() used: fault
 * summary (wraps), status line, each row sharing the page's remaining
 * height via flex_grow rather than a hard-coded per-row height.
 *
 * 2026-09-04: no separate "Channel N" title label any more -- refresh_cb()
 * now prefixes the channel number directly onto the fault line ("Ch0: ...")
 * so three cards sharing one page (see this file's header comment) don't
 * each spend a row on chrome the topbar's own title already covers for the
 * page as a whole. */
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

    /* Page 4: Safety & Board Health -- merged 2026-09-04 (see this file's
     * header comment). 5 accent-bordered sentence rows (Safety Processor,
     * now using Board Health's border-accent treatment via
     * build_full_text_row_accent()) plus MAX31856_CHANNEL_COUNT
     * cold-junction stat rows (Board Health, unchanged) -- the duplicate
     * "ESP32-S3 die temp" row Board Health used to also show is dropped
     * here, see s_bh_cj_label's own comment. Worst case ~237-257px,
     * comfortably inside the ~267px budget -- see the header comment and
     * this consolidation's report for the full character-count arithmetic. */
    lv_obj_t *safety_bh_page = s_pages[UI_PAGE_DIAGNOSTICS_PAGE_SAFETY_BOARD_HEALTH];
    s_safety_temp_label = build_full_text_row_accent(safety_bh_page, "Safety temp: ---", UI_THEME_ACCENT_1);
    s_enclosure_temp_label = build_full_text_row_accent(safety_bh_page, "Enclosure temp: ---", UI_THEME_ACCENT_2);
    s_safety_power_label = build_full_text_row_accent(safety_bh_page, "Power: ---", UI_THEME_ACCENT_3);
    s_link_version_label = build_full_text_row_accent(safety_bh_page, "Link: ---", UI_THEME_ACCENT_4);
    s_trip_label = build_full_text_row_accent(safety_bh_page, "State: ---", UI_THEME_ACCENT_5);
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
        s_bh_cj_label[ch] = build_stat_row(safety_bh_page, name, accent);
    }

    /* Page 6: Trip Detail -- new, 2026-08-27 (see this file's
     * UI_PAGE_DIAGNOSTICS_PAGE_TRIP_DETAIL comment). 5 wrapped full-text
     * rows, same shape as the Safety Processor page's own rows. */
    lv_obj_t *trip_detail_page = s_pages[UI_PAGE_DIAGNOSTICS_PAGE_TRIP_DETAIL];
    s_td_reason_label = build_full_text_row(trip_detail_page, "Reason: --");
    s_td_cause_label = build_full_text_row(trip_detail_page, "Detected: --");
    s_td_remedy_label = build_full_text_row(trip_detail_page, "To clear: --");
    s_td_source_label = build_full_text_row(trip_detail_page, "Fault source: --");
    s_td_latch_label = build_full_text_row(trip_detail_page, "--");

    /* Page 5: Thermocouple Faults -- back to ONE page for all
     * MAX31856_CHANNEL_COUNT channels as of 2026-09-04 (see this file's
     * header comment for why the 2026-08-28 P4-C one-channel-per-page split
     * is no longer needed: the per-row content that overflowed a shared
     * page is trimmed now, not the row count). Each card still gets an
     * equal flex_grow(1) share of the page, same as P4-C's per-page cards
     * did within their own page -- the only change is three shares of one
     * page instead of one share each of three pages. */
    lv_color_t tf_accents[3] = { UI_THEME_ACCENT_1, UI_THEME_ACCENT_2, UI_THEME_ACCENT_3 };
    lv_obj_t *thermo_fault_page = s_pages[UI_PAGE_DIAGNOSTICS_PAGE_THERMO_FAULTS];
    for (uint8_t ch = 0; ch < MAX31856_CHANNEL_COUNT; ch++) {
        build_thermo_fault_row(thermo_fault_page, ch, tf_accents[ch % 3]);
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
