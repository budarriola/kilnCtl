#include "ui_page_profile_segments.h"

#include <stdio.h>

#include "kiln_ui.h"
#include "profile_feasibility.h"
#include "profiles_builtin.h"
#include "profiles_http.h"
#include "ui_page_profile_detail.h"
#include "ui_theme.h"

/* Arithmetic (same style as every other page in this pass), against the real
 * ~267px content budget:
 *
 *     row list: 4 rows x 48px + 3 gaps (4px each) ... 204px
 *     gap ............................................  4px
 *     nav row (Back / Prev / N of M / Next) .........  44px
 *                                                     ------
 *                                                      252px  <= 267px  OK
 *
 * Read-only rows (spec explicitly allows shorter than
 * UI_THEME_MIN_TOUCH_TARGET_PX for a non-tap-target row -- these are not
 * clickable) so 4 fit per page; up to PROFILE_MAX_SEGMENTS (12) needs at
 * most 3 pages. */
#define ROWS_PER_PAGE 4
#define ROW_HEIGHT_PX 48
#define SEGMENTS_LIST_HEIGHT_PX (ROW_HEIGHT_PX * ROWS_PER_PAGE + (UI_THEME_PADDING_PX / 2) * (ROWS_PER_PAGE - 1))

static profile_t s_prof;
static bool s_prof_valid;
static profile_seg_verdict_t s_seg_verdict[PROFILE_MAX_SEGMENTS];
static uint8_t s_page_count;
static uint8_t s_page;

static lv_obj_t *s_list;
static lv_obj_t *s_indicator;
static lv_obj_t *s_title_label;

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("profile_detail");
}

static void render_page(void)
{
    if (!s_list) {
        return;
    }
    lv_obj_clean(s_list);

    if (!s_prof_valid) {
        lv_obj_t *none = lv_label_create(s_list);
        lv_obj_set_style_text_color(none, UI_THEME_COLOR_TEXT_SECONDARY, 0);
        lv_label_set_text(none, "Profile not found");
        return;
    }

    uint8_t start = (uint8_t)(s_page * ROWS_PER_PAGE);
    for (uint8_t i = start; i < s_prof.segment_count && i < (uint8_t)(start + ROWS_PER_PAGE); i++) {
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, ROW_HEIGHT_PX);
        lv_obj_set_style_bg_color(row, UI_THEME_COLOR_CARD, 0);
        lv_obj_set_style_radius(row, UI_THEME_CORNER_RADIUS_PX, 0);
        lv_obj_set_style_pad_all(row, UI_THEME_PADDING_PX / 4, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE); /* read-only row, not a tap target */
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        /* Feasibility colouring, per-segment, same rule as
         * ui_page_profile_detail.c's whole-profile card: too_fast/unreachable
         * -> dark red left border, unknown -> no marking, ok -> ordinary. */
        profile_seg_verdict_t v = s_seg_verdict[i];
        if (v == PROFILE_SEG_TOO_FAST || v == PROFILE_SEG_UNREACHABLE) {
            lv_obj_set_style_border_width(row, 3, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_color(row, UI_THEME_ACCENT_5, 0);
            lv_obj_set_style_pad_left(row, UI_THEME_PADDING_PX, 0);
        }

        lv_obj_t *label = lv_label_create(row);
        lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
        char buf[64];
        snprintf(buf, sizeof(buf), "Seg %u: %.0fC @ %.0fC/hr, dwell %umin", (unsigned)(i + 1),
                 (double)s_prof.segments[i].target_c, (double)s_prof.segments[i].ramp_c_per_hr,
                 (unsigned)s_prof.segments[i].dwell_min);
        lv_label_set_text(label, buf);
        lv_obj_center(label);
    }

    if (s_indicator) {
        lv_label_set_text_fmt(s_indicator, "%u of %u", (unsigned)(s_page + 1), (unsigned)(s_page_count ? s_page_count : 1));
    }
}

static void prev_cb(lv_event_t *e)
{
    (void)e;
    if (s_page > 0) {
        s_page--;
        render_page();
    }
}

static void next_cb(lv_event_t *e)
{
    (void)e;
    if (s_page + 1 < s_page_count) {
        s_page++;
        render_page();
    }
}

void ui_page_profile_segments_prepare(void)
{
    uint8_t id = ui_page_profile_detail_get_id();
    s_prof_valid = profiles_http_get(id, &s_prof);
    s_page = 0;

    if (s_prof_valid) {
        profile_feasibility_profile_mask(s_prof.zone_mask, &s_prof, s_seg_verdict, PROFILE_MAX_SEGMENTS);
        s_page_count = (uint8_t)((s_prof.segment_count + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE);
        if (s_page_count == 0) {
            s_page_count = 1;
        }
    } else {
        s_page_count = 1;
    }

    if (s_title_label) {
        const builtin_profile_t *b = (id >= PROFILE_BUILTIN_ID_BASE) ? profiles_builtin_entry(id) : NULL;
        char buf[64];
        snprintf(buf, sizeof(buf), "Segments -- %s", s_prof_valid ? (b ? b->title : s_prof.name) : "?");
        lv_label_set_text(s_title_label, buf);
    }

    render_page();
}

static lv_obj_t *build_nav_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, UI_THEME_MIN_TOUCH_TARGET_PX + UI_THEME_PADDING_PX * 2, 44);
    lv_obj_set_style_bg_color(btn, UI_THEME_COLOR_CARD, 0);
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

lv_obj_t *ui_page_profile_segments_build(void)
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
    lv_label_set_text(s_title_label, "Segments");

    s_list = lv_obj_create(scr);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_height(s_list, SEGMENTS_LIST_HEIGHT_PX);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_list, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *nav_row = lv_obj_create(scr);
    lv_obj_set_width(nav_row, lv_pct(100));
    lv_obj_set_height(nav_row, 44);
    lv_obj_set_style_bg_opa(nav_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(nav_row, 0, 0);
    lv_obj_set_style_pad_all(nav_row, 0, 0);
    lv_obj_set_flex_flow(nav_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(nav_row, LV_OBJ_FLAG_SCROLLABLE);

    build_nav_button(nav_row, "Back", back_btn_cb);
    build_nav_button(nav_row, "< Prev", prev_cb);
    s_indicator = lv_label_create(nav_row);
    lv_obj_set_style_text_color(s_indicator, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_indicator, "");
    build_nav_button(nav_row, "Next >", next_cb);

    render_page();
    return scr;
}
