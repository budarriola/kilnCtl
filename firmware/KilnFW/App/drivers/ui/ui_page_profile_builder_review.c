#include "ui_page_profile_builder_review.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "MAX31856.h"
#include "kiln_ui.h"
#include "profile_feasibility.h"
#include "profiles_http.h"
#include "ui_confirm.h"
#include "ui_page_profile_builder_zones.h"
#include "ui_page_profiles.h"
#include "ui_theme.h"
#include "ui_topbar.h"
#include "unit_pref.h"
#include "zones_config_accessors.h"

static const char *TAG = "ui_page_profile_builder_review";

/* Arithmetic (same style as every other page in this pass), against the real
 * ~267px content budget:
 *
 *     title ....................................... ~20px
 *     gap ..........................................  4px
 *     summary card (name/zones/segments/peak, 4      ~80px
 *       lines @ ~20px, feasibility-coloured border)
 *     gap ..........................................  4px
 *     action row (Save @ 72px) ..................... 72px
 *                                                   ------
 *                                                    180px  <= 267px  OK
 *
 * Back moved into the shared top bar (ui_topbar.c) in the 2026-08-21
 * icon-topbar pass; action_row's height is unchanged (Save still sets it at
 * 72px) so no vertical budget was freed here, only Back's share of
 * action_row's horizontal width, which Save now claims alone.
 *
 * The slot picker (8 cells) is NOT part of this budget -- see
 * build_slot_picker() below: it's a full-screen overlay attached to `scr`
 * with LV_OBJ_FLAG_IGNORE_LAYOUT, the same pattern ui_page_network.c's
 * connect modal already uses, so it never joins this screen's flex column
 * (the FLEX TRAP ui_page_home.c's header comment warns about) and never
 * competes with the summary/action-row content above for the 267px budget.
 * Its own arithmetic, against the full 320px physical screen height (an
 * overlay is not bound by the *page* content budget, only by the panel
 * itself):
 *
 *     title ("Choose Slot") ....................... ~20px
 *     gap ..........................................  4px
 *     grid: 2 rows x 4 cols x 72px + 1 row gap ..... 148px
 *     gap ..........................................  4px
 *     Cancel row ................................... 44px
 *                                                   ------
 *                                                    220px  <= ~304px (320
 *                                                    minus this overlay's
 *                                                    own UI_THEME_PADDING_PX
 *                                                    top/bottom) OK
 */
#define SLOT_GRID_HEIGHT_PX (UI_THEME_MIN_TOUCH_TARGET_PX * 2 + UI_THEME_PADDING_PX / 2)

static lv_obj_t *s_summary_card;
static lv_obj_t *s_name_label;
static lv_obj_t *s_zones_label;
static lv_obj_t *s_segments_label;
static lv_obj_t *s_peak_label;

static lv_obj_t *s_slot_modal;
static lv_obj_t *s_slot_grid;

static profile_t *draft(void)
{
    return ui_page_profile_builder_draft();
}

static void do_save(uint8_t slot)
{
    profile_t *d = draft();
    char err_msg[64] = "";
    uint8_t out_id = 0;
    uint8_t warn_count = 0;

    /* profiles_http_save() writes NVS on the calling task. That is safe from
     * here: lvgl_port.c gives the LVGL task (this callback runs on it) an
     * internal-SRAM stack, same HAZARD comment ui_page_profile_detail.c's
     * confirm_start_cb() already documents for profile_executor_run() (and
     * uart_bridge_ext.c documents for its own NVS-writing call) -- an
     * external-PSRAM stack is what NVS's flash operations cannot tolerate,
     * and that isn't this task's stack. */
    bool ok = profiles_http_save(slot, d, &out_id, &warn_count, err_msg, sizeof(err_msg));

    if (!ok) {
        ESP_LOGW(TAG, "profiles_http_save(%u) refused: %s", (unsigned)slot, err_msg);
        ui_confirm_params_t params = {
            .title = "Cannot Save",
            .body = err_msg[0] ? err_msg : "Refused for an unknown reason.",
            .confirm_label = "OK",
            .confirm_color = UI_THEME_COLOR_CARD,
            .on_confirm = NULL,
            .user_data = NULL,
        };
        ui_confirm_show(&params);
        return;
    }

    lv_obj_add_flag(s_slot_modal, LV_OBJ_FLAG_HIDDEN);

    char body[48];
    snprintf(body, sizeof(body), "Saved to slot %u.", (unsigned)out_id);
    ui_confirm_params_t params = {
        .title = "Profile Saved",
        .body = body,
        .confirm_label = "OK",
        .confirm_color = UI_THEME_ACCENT_4,
        .on_confirm = NULL,
        .user_data = NULL,
    };
    ui_confirm_show(&params);
    /* "profiles_mine" was deleted (UI_PLAN.md 6.2 -- the unified list at
     * "profiles" now owns this slot). Refresh before showing: "profiles" is
     * cached after its first build, so a stale render would otherwise still
     * show the pre-save list. */
    ui_page_profiles_refresh();
    kiln_ui_show("profiles");
}

static void confirm_overwrite_cb(void *user_data)
{
    uint8_t slot = (uint8_t)(uintptr_t)user_data;
    do_save(slot);
}

static void slot_clicked_cb(lv_event_t *e)
{
    uint8_t slot = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    profile_t existing;
    bool occupied = profiles_http_get(slot, &existing);
    if (!occupied) {
        do_save(slot);
        return;
    }

    char body[64];
    snprintf(body, sizeof(body), "Overwrite \"%s\" in slot %u?", existing.name, (unsigned)slot);
    ui_confirm_params_t params = {
        .title = "Overwrite Profile",
        .body = body,
        .confirm_label = "Overwrite",
        .confirm_color = UI_THEME_ACCENT_5,
        .on_confirm = confirm_overwrite_cb,
        .user_data = (void *)(uintptr_t)slot,
    };
    ui_confirm_show(&params);
}

static void slot_cancel_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_slot_modal, LV_OBJ_FLAG_HIDDEN);
}

static void render_slot_grid(void)
{
    lv_obj_clean(s_slot_grid);
    for (uint8_t id = 0; id < PROFILES_MAX_COUNT; id++) {
        profile_t existing;
        bool used = profiles_http_get(id, &existing);

        lv_obj_t *cell = lv_button_create(s_slot_grid);
        lv_obj_set_width(cell, lv_pct(23));
        lv_obj_set_height(cell, UI_THEME_MIN_TOUCH_TARGET_PX);
        lv_obj_set_style_bg_color(cell, UI_THEME_COLOR_CARD, 0);
        lv_obj_set_style_radius(cell, UI_THEME_CORNER_RADIUS_PX, 0);
        lv_obj_add_event_cb(cell, slot_clicked_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)id);

        lv_obj_t *label = lv_label_create(cell);
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(label, used ? UI_THEME_COLOR_TEXT_PRIMARY : UI_THEME_COLOR_TEXT_SECONDARY, 0);
        char buf[32];
        if (used) {
            snprintf(buf, sizeof(buf), "%u - %s", (unsigned)id, existing.name);
        } else {
            snprintf(buf, sizeof(buf), "%u - free", (unsigned)id);
        }
        lv_label_set_text(label, buf);
        lv_obj_center(label);

        lv_obj_update_layout(cell);
        ui_theme_apply_touch_area(cell, true);
    }
}

static void save_btn_cb(lv_event_t *e)
{
    (void)e;
    render_slot_grid();
    lv_obj_remove_flag(s_slot_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_slot_modal);
}

static void build_slot_picker(lv_obj_t *scr)
{
    /* Full-screen overlay, IGNORE_LAYOUT -- same pattern
     * ui_page_network.c's build_connect_modal() uses, see this file's header
     * comment for why that keeps it out of the summary screen's own no-scroll
     * budget. */
    s_slot_modal = lv_obj_create(scr);
    lv_obj_add_flag(s_slot_modal, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(s_slot_modal, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(s_slot_modal, 0, 0);
    lv_obj_set_style_bg_color(s_slot_modal, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_slot_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_slot_modal, 0, 0);
    lv_obj_set_style_pad_all(s_slot_modal, UI_THEME_PADDING_PX, 0);
    lv_obj_set_flex_flow(s_slot_modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_slot_modal, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(s_slot_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_slot_modal);
    lv_obj_set_style_text_color(title, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(title, "Choose Slot");

    s_slot_grid = lv_obj_create(s_slot_modal);
    lv_obj_set_width(s_slot_grid, lv_pct(100));
    lv_obj_set_height(s_slot_grid, SLOT_GRID_HEIGHT_PX);
    lv_obj_set_style_bg_opa(s_slot_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_slot_grid, 0, 0);
    lv_obj_set_style_pad_all(s_slot_grid, 0, 0);
    lv_obj_set_flex_flow(s_slot_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(s_slot_grid, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(s_slot_grid, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cancel_btn = lv_button_create(s_slot_modal);
    lv_obj_set_size(cancel_btn, UI_THEME_MIN_TOUCH_TARGET_PX + UI_THEME_PADDING_PX * 2, 44);
    lv_obj_set_style_bg_color(cancel_btn, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(cancel_btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(cancel_btn, slot_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_obj_set_style_text_color(cancel_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_center(cancel_label);
    lv_obj_update_layout(cancel_btn);
    ui_theme_apply_touch_area(cancel_btn, false);
}

void ui_page_profile_builder_review_prepare(void)
{
    profile_t *d = draft();
    if (!s_name_label) {
        return; /* not built yet -- build() calls this itself once it is */
    }

    lv_label_set_text(s_name_label, d->name[0] ? d->name : "(unnamed)");

    char zones_buf[96];
    size_t zlen = 0;
    zones_buf[0] = '\0';
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT && zlen < sizeof(zones_buf) - 1; zi++) {
        if (!(d->zone_mask & (1u << zi))) {
            continue;
        }
        char name[16];
        const char *zname = (zones_config_get_name(zi, name, sizeof(name)) && name[0]) ? name : NULL;
        char piece[24];
        if (zname) {
            snprintf(piece, sizeof(piece), "%s%s", zlen ? ", " : "", zname);
        } else {
            snprintf(piece, sizeof(piece), "%sZone %u", zlen ? ", " : "", (unsigned)zi);
        }
        size_t piece_len = strlen(piece);
        if (zlen + piece_len < sizeof(zones_buf)) {
            memcpy(zones_buf + zlen, piece, piece_len + 1);
            zlen += piece_len;
        }
    }
    char zbuf[104];
    snprintf(zbuf, sizeof(zbuf), "Zones: %s", zones_buf[0] ? zones_buf : "(none)");
    lv_label_set_text(s_zones_label, zbuf);

    char segbuf[24];
    snprintf(segbuf, sizeof(segbuf), "%u segments", (unsigned)d->segment_count);
    lv_label_set_text(s_segments_label, segbuf);

    float peak = 0.0f;
    for (uint8_t i = 0; i < d->segment_count; i++) {
        if (d->segments[i].target_c > peak) {
            peak = d->segments[i].target_c;
        }
    }
    /* LCD item 2 (2026-08-21): pure read-only display of an already-Celsius
     * `peak` computed just above from segments[i].target_c -- this label
     * never feeds back into the draft profile, so converting it for display
     * is safe. ABSOLUTE kind: peak is a real temperature reading (the
     * highest segment target), not a rate. */
    unit_pref_t pref = unit_pref_get();
    char peakbuf[24];
    snprintf(peakbuf, sizeof(peakbuf), "Peak %.0f %s", (double)unit_pref_convert(peak, pref, UNIT_PREF_KIND_ABSOLUTE),
             unit_pref_suffix(pref));
    lv_label_set_text(s_peak_label, peakbuf);

    profile_seg_verdict_t v = profile_feasibility_profile_mask(d->zone_mask, d, NULL, 0);
    if (v == PROFILE_SEG_TOO_FAST || v == PROFILE_SEG_UNREACHABLE) {
        lv_obj_set_style_border_width(s_summary_card, 3, 0);
        lv_obj_set_style_border_color(s_summary_card, UI_THEME_ACCENT_5, 0);
    } else {
        lv_obj_set_style_border_width(s_summary_card, 0, 0);
    }
}

lv_obj_t *ui_page_profile_builder_review_build(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    static ui_topbar_t tb;
    ui_topbar_create(scr, &(ui_topbar_cfg_t){
        .title = "Review & Save",
        .back_page = "profile_builder_segment",
        .show_home = true,
    }, &tb);

    s_summary_card = lv_obj_create(scr);
    lv_obj_set_width(s_summary_card, lv_pct(100));
    lv_obj_set_height(s_summary_card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_summary_card, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(s_summary_card, UI_THEME_CORNER_RADIUS_PX, 0);
    /* Phase 7 theme/style pass (TODO.md 1223-1225): pure-paint shadow, see
     * ui_theme.h -- costs no page-budget height. */
    ui_theme_apply_card_shadow(s_summary_card, 1);
    lv_obj_set_style_pad_all(s_summary_card, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_set_flex_flow(s_summary_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_summary_card, UI_THEME_PADDING_PX / 4, 0);
    lv_obj_remove_flag(s_summary_card, LV_OBJ_FLAG_SCROLLABLE);

    s_name_label = lv_label_create(s_summary_card);
    lv_obj_set_style_text_color(s_name_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(s_name_label, "");

    s_zones_label = lv_label_create(s_summary_card);
    lv_obj_set_style_text_color(s_zones_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_zones_label, "");

    s_segments_label = lv_label_create(s_summary_card);
    lv_obj_set_style_text_color(s_segments_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_segments_label, "");

    s_peak_label = lv_label_create(s_summary_card);
    lv_obj_set_style_text_color(s_peak_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_peak_label, "");

    lv_obj_t *action_row = lv_obj_create(scr);
    lv_obj_set_width(action_row, lv_pct(100));
    lv_obj_set_height(action_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(action_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(action_row, 0, 0);
    lv_obj_set_style_pad_all(action_row, 0, 0);
    lv_obj_set_flex_flow(action_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(action_row, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(action_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *save = lv_button_create(action_row);
    lv_obj_set_height(save, UI_THEME_MIN_TOUCH_TARGET_PX);
    lv_obj_set_flex_grow(save, 1);
    lv_obj_set_style_bg_color(save, UI_THEME_ACCENT_4, 0);
    lv_obj_set_style_radius(save, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(save, save_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_label = lv_label_create(save);
    lv_obj_set_style_text_color(save_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(save_label, "Save");
    lv_obj_center(save_label);
    lv_obj_update_layout(save);
    ui_theme_apply_touch_area(save, false);

    /* Raise the icon proxy BEFORE the slot-picker overlay is built, same
     * reasoning as ui_page_network.c's connect modal: the overlay (hidden by
     * default, shown by save_btn_cb) must stay ABOVE the icons when shown,
     * and z-order here is child-add-order, so building it last keeps it on
     * top regardless of when raise() ran. */
    ui_topbar_raise(&tb);
    build_slot_picker(scr);

    ui_page_profile_builder_review_prepare();
    return scr;
}
