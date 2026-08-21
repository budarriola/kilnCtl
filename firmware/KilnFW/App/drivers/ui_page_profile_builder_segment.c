#include "ui_page_profile_builder_segment.h"

#include <stdio.h>
#include <string.h>

#include "kiln_ui.h"
#include "profile_feasibility.h"
#include "profiles_http.h"
#include "ui_num_pad.h"
#include "ui_page_profile_builder_review.h"
#include "ui_page_profile_builder_zones.h"
#include "ui_theme.h"
#include "ui_topbar.h"

/* Arithmetic (same style as every other page in this pass), against the real
 * ~267px content budget:
 *
 *     title ("Segment N of M") ................... ~20px
 *     gap ..........................................  4px
 *     three value cards, side by side, 72px tall .. 72px
 *     gap ..........................................  4px
 *     nav row (Add / Del) ......................... 44px
 *     gap ..........................................  4px
 *     caption (Add/Del limit, reserved even blank)  18px
 *                                                   ------
 *                                                    162px  <= 267px  OK
 *
 * Three cards LAID OUT HORIZONTALLY, not stacked -- three 72px rows stacked
 * plus a nav row would be 3*72 + 44 = 260px on their own, no room left for a
 * second nav row or the caption; side by side they cost only one 72px row
 * total, same trick this task's brief calls out explicitly.
 *
 * 2026-08-21 icon-topbar pass: the old nav row 1 (Back / Prev / N of M /
 * Next) moved into the shared top bar (ui_topbar.c), freeing 44px + 4px of
 * gap. Prev/Next are wired through cfg.prev_cb/next_cb -- Next here is a
 * real page-style control (advances s_cur_seg like ui_page_profiles_mine.c's
 * Prev/Next), with one twist ui_topbar_set_next_enabled() is deliberately
 * NOT used for: at the last segment, Next does not clamp/disable, it
 * advances to Step 3 (profile_builder_review) -- see next_cb()'s own
 * comment. Prev DOES clamp (set_prev_enabled(s_cur_seg > 0)), same as every
 * other paged page. The standalone "N of M" indicator label was dropped
 * entirely rather than moved: s_title already renders "Segment N of M" via
 * ui_topbar_set_title(), so a second copy of the same count would be pure
 * duplication. */
#define CARD_HEIGHT_PX 72

static uint8_t s_cur_seg;

static lv_obj_t *s_target_val_label;
static lv_obj_t *s_ramp_val_label;
static lv_obj_t *s_dwell_val_label;
static lv_obj_t *s_cards_row;
static lv_obj_t *s_caption;
static lv_obj_t *s_add_btn;
static lv_obj_t *s_del_btn;
static ui_topbar_t s_tb;

static profile_t *draft(void)
{
    return ui_page_profile_builder_draft();
}

static void refresh(void)
{
    profile_t *d = draft();
    if (d->segment_count == 0) {
        return; /* prepare() always seeds at least one -- defensive only */
    }
    if (s_cur_seg >= d->segment_count) {
        s_cur_seg = (uint8_t)(d->segment_count - 1);
    }
    profile_segment_t *seg = &d->segments[s_cur_seg];

    char buf[24];
    snprintf(buf, sizeof(buf), "%.0f C", (double)seg->target_c);
    lv_label_set_text(s_target_val_label, buf);
    snprintf(buf, sizeof(buf), "%.0f C/hr", (double)seg->ramp_c_per_hr);
    lv_label_set_text(s_ramp_val_label, buf);
    snprintf(buf, sizeof(buf), "%u min", (unsigned)seg->dwell_min);
    lv_label_set_text(s_dwell_val_label, buf);

    char title_buf[32];
    snprintf(title_buf, sizeof(title_buf), "Segment %u of %u", (unsigned)(s_cur_seg + 1),
             (unsigned)d->segment_count);
    ui_topbar_set_title(&s_tb, title_buf);
    ui_topbar_set_prev_enabled(&s_tb, s_cur_seg > 0);
    /* Next is deliberately always enabled -- see this file's header comment:
     * at the last segment it advances to Step 3 rather than clamping. */

    /* Live feasibility verdict -- profile_feasibility_profile_mask() is pure
     * math over the in-memory model (no NVS touched), safe to call on every
     * value change/page render, same call ui_page_profiles_mine.c and
     * ui_page_profile_detail.c already make for their own read-only cards.
     * Colouring matches every other page in this pass exactly: red for
     * too_fast/unreachable, NOTHING for unknown (an untuned zone is not the
     * same as a bad schedule), ordinary for ok. */
    profile_seg_verdict_t verdicts[PROFILE_MAX_SEGMENTS];
    profile_feasibility_profile_mask(d->zone_mask, d, verdicts, PROFILE_MAX_SEGMENTS);
    profile_seg_verdict_t v = verdicts[s_cur_seg];
    if (v == PROFILE_SEG_TOO_FAST || v == PROFILE_SEG_UNREACHABLE) {
        lv_obj_set_style_border_width(s_cards_row, 3, 0);
        lv_obj_set_style_border_color(s_cards_row, UI_THEME_ACCENT_5, 0);
    } else {
        lv_obj_set_style_border_width(s_cards_row, 0, 0);
    }

    if (d->segment_count >= PROFILE_MAX_SEGMENTS) {
        lv_obj_remove_flag(s_add_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(s_add_btn, LV_OPA_50, 0);
        lv_label_set_text(s_caption, "Maximum 12 segments reached");
    } else {
        lv_obj_add_flag(s_add_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(s_add_btn, LV_OPA_COVER, 0);
        if (d->segment_count <= 1) {
            lv_label_set_text(s_caption, "At least one segment is required");
        } else {
            lv_label_set_text(s_caption, "");
        }
    }

    if (d->segment_count <= 1) {
        lv_obj_remove_flag(s_del_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(s_del_btn, LV_OPA_50, 0);
    } else {
        lv_obj_add_flag(s_del_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(s_del_btn, LV_OPA_COVER, 0);
    }
}

void ui_page_profile_builder_segment_prepare(void)
{
    profile_t *d = draft();
    if (d->segment_count == 0) {
        d->segment_count = 1;
        memset(&d->segments[0], 0, sizeof(d->segments[0]));
    }
    s_cur_seg = 0;
    refresh();
}

static void prev_cb(lv_event_t *e)
{
    (void)e;
    if (s_cur_seg > 0) {
        s_cur_seg--;
        refresh();
    }
}

static void next_cb(lv_event_t *e)
{
    (void)e;
    profile_t *d = draft();
    if (s_cur_seg + 1 < d->segment_count) {
        s_cur_seg++;
        refresh();
        return;
    }
    /* Already on the last segment -- Next advances to Step 3. */
    ui_page_profile_builder_review_prepare();
    kiln_ui_show("profile_builder_review");
}

static void add_cb(lv_event_t *e)
{
    (void)e;
    profile_t *d = draft();
    if (d->segment_count >= PROFILE_MAX_SEGMENTS) {
        return; /* button is non-clickable in this state already */
    }
    memset(&d->segments[d->segment_count], 0, sizeof(d->segments[0]));
    d->segment_count++;
    s_cur_seg = (uint8_t)(d->segment_count - 1);
    refresh();
}

static void del_cb(lv_event_t *e)
{
    (void)e;
    profile_t *d = draft();
    if (d->segment_count <= 1) {
        return; /* button is non-clickable in this state already */
    }
    for (uint8_t i = s_cur_seg; i + 1 < d->segment_count; i++) {
        d->segments[i] = d->segments[i + 1];
    }
    d->segment_count--;
    if (s_cur_seg >= d->segment_count) {
        s_cur_seg = (uint8_t)(d->segment_count - 1);
    }
    refresh();
}

static void target_done_cb(bool accepted, const char *text, float value, void *user_data)
{
    (void)text;
    (void)user_data;
    if (!accepted) return;
    draft()->segments[s_cur_seg].target_c = value;
    refresh();
}

static void ramp_done_cb(bool accepted, const char *text, float value, void *user_data)
{
    (void)text;
    (void)user_data;
    if (!accepted) return;
    draft()->segments[s_cur_seg].ramp_c_per_hr = value;
    refresh();
}

static void dwell_done_cb(bool accepted, const char *text, float value, void *user_data)
{
    (void)text;
    (void)user_data;
    if (!accepted) return;
    draft()->segments[s_cur_seg].dwell_min = (uint32_t)value;
    refresh();
}

static void target_card_cb(lv_event_t *e)
{
    (void)e;
    float min_c, max_c;
    profiles_http_get_bounds(&min_c, &max_c, NULL, NULL, NULL);
    ui_num_pad_params_t params = {
        .caption = "Target C",
        .mode = UI_NUM_PAD_MODE_NUMBER,
        .initial_value = draft()->segments[s_cur_seg].target_c,
        .min = min_c,
        .max = max_c,
        .decimals = 0,
        .on_done = target_done_cb,
    };
    ui_num_pad_show(&params);
}

static void ramp_card_cb(lv_event_t *e)
{
    (void)e;
    float min_r, max_r;
    profiles_http_get_bounds(NULL, NULL, &min_r, &max_r, NULL);
    ui_num_pad_params_t params = {
        .caption = "Ramp C/hr",
        .mode = UI_NUM_PAD_MODE_NUMBER,
        .initial_value = draft()->segments[s_cur_seg].ramp_c_per_hr,
        .min = min_r,
        .max = max_r,
        .decimals = 0,
        .on_done = ramp_done_cb,
    };
    ui_num_pad_show(&params);
}

static void dwell_card_cb(lv_event_t *e)
{
    (void)e;
    uint32_t dwell_max = 0;
    profiles_http_get_bounds(NULL, NULL, NULL, NULL, &dwell_max);
    ui_num_pad_params_t params = {
        .caption = "Dwell min",
        .mode = UI_NUM_PAD_MODE_NUMBER,
        .initial_value = (float)draft()->segments[s_cur_seg].dwell_min,
        .min = 0.0f,
        .max = (float)dwell_max,
        .decimals = 0,
        .on_done = dwell_done_cb,
    };
    ui_num_pad_show(&params);
}

static lv_obj_t *build_card(lv_obj_t *parent, const char *caption, lv_obj_t **out_val_label, lv_event_cb_t cb)
{
    lv_obj_t *card = lv_button_create(parent);
    lv_obj_set_height(card, CARD_HEIGHT_PX);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_style_bg_color(card, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(card, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *cap = lv_label_create(card);
    lv_obj_set_style_text_color(cap, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(cap, caption);

    lv_obj_t *val = lv_label_create(card);
    lv_obj_set_style_text_color(val, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(val, "");
    *out_val_label = val;

    lv_obj_update_layout(card);
    ui_theme_apply_touch_area(card, false);
    return card;
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

lv_obj_t *ui_page_profile_builder_segment_build(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    ui_topbar_create(scr, &(ui_topbar_cfg_t){
        .title = "Segment",
        .back_page = "profile_builder_zones",
        .show_home = true,
        .prev_cb = prev_cb,
        .next_cb = next_cb,
    }, &s_tb);

    s_cards_row = lv_obj_create(scr);
    lv_obj_set_width(s_cards_row, lv_pct(100));
    lv_obj_set_height(s_cards_row, CARD_HEIGHT_PX);
    lv_obj_set_style_bg_opa(s_cards_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_cards_row, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(s_cards_row, 0, 0);
    lv_obj_set_flex_flow(s_cards_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(s_cards_row, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(s_cards_row, LV_OBJ_FLAG_SCROLLABLE);

    build_card(s_cards_row, "Target C", &s_target_val_label, target_card_cb);
    build_card(s_cards_row, "Ramp C/hr", &s_ramp_val_label, ramp_card_cb);
    build_card(s_cards_row, "Dwell min", &s_dwell_val_label, dwell_card_cb);

    lv_obj_t *nav_row2 = lv_obj_create(scr);
    lv_obj_set_width(nav_row2, lv_pct(100));
    lv_obj_set_height(nav_row2, 44);
    lv_obj_set_style_bg_opa(nav_row2, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(nav_row2, 0, 0);
    lv_obj_set_style_pad_all(nav_row2, 0, 0);
    lv_obj_set_flex_flow(nav_row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(nav_row2, UI_THEME_PADDING_PX, 0);
    lv_obj_remove_flag(nav_row2, LV_OBJ_FLAG_SCROLLABLE);

    s_add_btn = build_nav_button(nav_row2, "+ Add", add_cb);
    s_del_btn = build_nav_button(nav_row2, "Del", del_cb);

    s_caption = lv_label_create(scr);
    lv_obj_set_style_text_color(s_caption, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_caption, "");

    ui_topbar_raise(&s_tb);

    refresh();
    return scr;
}
