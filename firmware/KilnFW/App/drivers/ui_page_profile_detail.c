#include "ui_page_profile_detail.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "MAX31856.h"
#include "kiln_ui.h"
#include "profile_executor.h"
#include "profile_feasibility.h"
#include "profiles_builtin.h"
#include "profiles_http.h"
#include "ui_confirm.h"
#include "ui_page_profile_segments.h"
#include "ui_theme.h"
#include "zones_http.h"

static const char *TAG = "ui_page_profile_detail";

/* Arithmetic (same style as every other page in this pass), against the real
 * ~267px content budget:
 *
 *     title line (LV_SIZE_CONTENT, ~20px) ....... ~20px
 *     gap ......................................... 4px
 *     info card (name/family/segments, ~64px) ... ~64px
 *     gap ......................................... 4px
 *     action row: Segments + Start @ 72px ....... 72px
 *     gap ......................................... 4px
 *     nav row (Back) .............................. 44px
 *                                                 ------
 *                                                 ~208px  <= 267px  OK
 *
 * No paging needed -- one profile's summary fits a single screen. */

static uint8_t s_profile_id;
static const char *s_back_page = "profiles_mine";

static lv_obj_t *s_title_label;
static lv_obj_t *s_info_card;
static lv_obj_t *s_name_label;
static lv_obj_t *s_segcount_label;
static lv_obj_t *s_family_label;

static bool load_current(profile_t *out, const builtin_profile_t **out_builtin)
{
    *out_builtin = (s_profile_id >= PROFILE_BUILTIN_ID_BASE) ? profiles_builtin_entry(s_profile_id) : NULL;
    return profiles_http_get(s_profile_id, out);
}

static profile_seg_verdict_t current_verdict(const profile_t *prof)
{
    return profile_feasibility_profile_mask(prof->zone_mask, prof, NULL, 0);
}

/* Refreshes the on-screen labels/card colour from current state. Called both
 * right after the page is first built (with whatever id/back_page was set
 * before the very first navigation here) and by
 * ui_page_profile_detail_set_id() on every subsequent navigation -- this
 * page is built once and cached (kiln_ui.c), so re-navigating here never
 * calls build() again; the labels must be repainted some other way. */
static void refresh(void)
{
    if (!s_title_label) {
        return; /* not built yet */
    }

    profile_t prof;
    const builtin_profile_t *b = NULL;
    if (!load_current(&prof, &b)) {
        lv_label_set_text(s_title_label, "Profile Detail");
        lv_label_set_text(s_name_label, "(not found)");
        lv_label_set_text(s_segcount_label, "");
        lv_label_set_text(s_family_label, "");
        lv_obj_set_style_border_width(s_info_card, 0, 0);
        return;
    }

    const char *title = b ? b->title : prof.name;
    lv_label_set_text(s_title_label, title);
    lv_label_set_text(s_name_label, prof.name);

    char segbuf[32];
    snprintf(segbuf, sizeof(segbuf), "%u segments", (unsigned)prof.segment_count);
    lv_label_set_text(s_segcount_label, segbuf);

    if (b) {
        char fambuf[48];
        snprintf(fambuf, sizeof(fambuf), "%s (builtin, read-only)", b->family ? b->family : "?");
        lv_label_set_text(s_family_label, fambuf);
    } else {
        lv_label_set_text(s_family_label, "User profile");
    }

    /* Feasibility colouring -- match the web dashboard exactly (TODO's own
     * instruction): too_fast/unreachable get a dark-red treatment
     * (UI_THEME_ACCENT_5 family), unknown gets NO marking at all (it means
     * autotune has never run, not that the schedule is bad), ok is ordinary.
     */
    profile_seg_verdict_t v = current_verdict(&prof);
    if (v == PROFILE_SEG_TOO_FAST || v == PROFILE_SEG_UNREACHABLE) {
        lv_obj_set_style_border_width(s_info_card, 3, 0);
        lv_obj_set_style_border_color(s_info_card, UI_THEME_ACCENT_5, 0);
    } else {
        lv_obj_set_style_border_width(s_info_card, 0, 0);
    }
}

void ui_page_profile_detail_set_id(uint8_t profile_id, const char *back_page)
{
    s_profile_id = profile_id;
    s_back_page = back_page ? back_page : "profiles_mine";
    refresh();
}

uint8_t ui_page_profile_detail_get_id(void)
{
    return s_profile_id;
}

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show(s_back_page);
}

static void segments_nav_cb(lv_event_t *e)
{
    (void)e;
    ui_page_profile_segments_prepare();
    kiln_ui_show("profile_segments");
}

static void confirm_start_cb(void *user_data)
{
    (void)user_data;
    char err_msg[64] = "";
    /* profile_executor_run() writes NVS on the calling task (run_state.h's
     * breadcrumb). That is safe from here: lvgl_port.c gives the LVGL task
     * (this callback runs on it) an internal-SRAM stack, same HAZARD comment
     * uart_bridge_ext.c documents for its own NVS-writing call from a
     * non-default task -- an external-PSRAM stack is what NVS's flash
     * operations cannot tolerate, and that isn't this task's stack. */
    if (!profile_executor_run(s_profile_id, err_msg, sizeof(err_msg))) {
        ESP_LOGW(TAG, "profile_executor_run(%u) refused: %s", (unsigned)s_profile_id, err_msg);
        ui_confirm_params_t err_params = {
            .title = "Cannot Start",
            .body = err_msg[0] ? err_msg : "Refused for an unknown reason.",
            .confirm_label = "OK",
            .confirm_color = UI_THEME_COLOR_CARD,
            .on_confirm = NULL,
            .user_data = NULL,
        };
        ui_confirm_show(&err_params);
        return;
    }
    kiln_ui_show("home");
}

static void start_btn_cb(lv_event_t *e)
{
    (void)e;
    profile_t prof;
    const builtin_profile_t *b = NULL;
    if (!load_current(&prof, &b)) {
        return;
    }

    char zones_buf[96];
    size_t zlen = 0;
    zones_buf[0] = '\0';
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT && zlen < sizeof(zones_buf) - 1; zi++) {
        if (prof.zone_mask != 0 && !(prof.zone_mask & (1u << zi))) {
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

    char body[256];
    snprintf(body, sizeof(body), "Start \"%s\" now? This will energise %s for the duration of the "
                                  "firing, which can be hours.",
             prof.name, zones_buf[0] ? zones_buf : "no zones");

    ui_confirm_params_t params = {
        .title = "Confirm Start",
        .body = body,
        .confirm_label = "Start",
        .confirm_color = UI_THEME_ACCENT_4,
        .on_confirm = confirm_start_cb,
        .user_data = NULL,
    };
    ui_confirm_show(&params);
}

static lv_obj_t *build_action_button(lv_obj_t *parent, const char *text, lv_color_t bg, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, UI_THEME_MIN_TOUCH_TARGET_PX);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_radius(btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);

    lv_obj_update_layout(btn);
    ui_theme_apply_touch_area(btn, false);
    return btn;
}

lv_obj_t *ui_page_profile_detail_build(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_title_label = lv_label_create(scr);
    lv_obj_set_width(s_title_label, lv_pct(100));
    lv_label_set_long_mode(s_title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_title_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(s_title_label, "Profile Detail");

    s_info_card = lv_obj_create(scr);
    lv_obj_set_width(s_info_card, lv_pct(100));
    lv_obj_set_height(s_info_card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_info_card, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(s_info_card, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(s_info_card, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_set_flex_flow(s_info_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_info_card, UI_THEME_PADDING_PX / 4, 0);
    lv_obj_remove_flag(s_info_card, LV_OBJ_FLAG_SCROLLABLE);

    s_name_label = lv_label_create(s_info_card);
    lv_obj_set_style_text_color(s_name_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(s_name_label, "");

    s_segcount_label = lv_label_create(s_info_card);
    lv_obj_set_style_text_color(s_segcount_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_segcount_label, "");

    s_family_label = lv_label_create(s_info_card);
    lv_obj_set_style_text_color(s_family_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_family_label, "");

    lv_obj_t *action_row = lv_obj_create(scr);
    lv_obj_set_width(action_row, lv_pct(100));
    lv_obj_set_height(action_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(action_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(action_row, 0, 0);
    lv_obj_set_style_pad_all(action_row, 0, 0);
    lv_obj_set_flex_flow(action_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(action_row, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(action_row, LV_OBJ_FLAG_SCROLLABLE);

    build_action_button(action_row, "Segments", UI_THEME_COLOR_CARD, segments_nav_cb);
    build_action_button(action_row, "Start", UI_THEME_ACCENT_4, start_btn_cb);

    lv_obj_t *nav_row = lv_obj_create(scr);
    lv_obj_set_width(nav_row, lv_pct(100));
    lv_obj_set_height(nav_row, 44);
    lv_obj_set_style_bg_opa(nav_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(nav_row, 0, 0);
    lv_obj_set_style_pad_all(nav_row, 0, 0);
    lv_obj_remove_flag(nav_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(nav_row);
    lv_obj_set_size(back, UI_THEME_MIN_TOUCH_TARGET_PX + UI_THEME_PADDING_PX * 2, 44);
    lv_obj_set_style_bg_color(back, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(back, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back);
    lv_obj_set_style_text_color(back_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
    lv_obj_update_layout(back);
    ui_theme_apply_touch_area(back, false);

    refresh();
    return scr;
}
