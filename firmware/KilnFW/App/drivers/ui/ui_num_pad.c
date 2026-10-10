#include "ui_num_pad.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_theme.h"

#define UI_NUM_PAD_WIDTH_PX 440

static lv_obj_t *s_modal;
static lv_obj_t *s_caption;
static lv_obj_t *s_ta;
static lv_obj_t *s_range_label;
static lv_obj_t *s_kb;

static ui_num_pad_mode_t s_mode;
static float s_min;
static float s_max;
static int s_decimals;
static ui_num_pad_done_cb_t s_on_done;
static void *s_user_data;

static void close_modal(void)
{
    if (s_modal) {
        lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

static void cancel_cb(lv_event_t *e)
{
    (void)e;
    ui_num_pad_done_cb_t cb = s_on_done;
    void *ud = s_user_data;
    close_modal();
    if (cb) {
        cb(false, "", 0.0f, ud);
    }
}

static void done_cb(lv_event_t *e)
{
    (void)e;
    ui_num_pad_done_cb_t cb = s_on_done;
    void *ud = s_user_data;
    const char *raw = lv_textarea_get_text(s_ta);

    char text[64];
    float value = 0.0f;
    if (s_mode == UI_NUM_PAD_MODE_NUMBER) {
        value = (float)atof(raw);
        if (s_min < s_max) {
            if (value < s_min) value = s_min;
            if (value > s_max) value = s_max;
        }
        snprintf(text, sizeof(text), "%.*f", s_decimals, (double)value);
    } else {
        snprintf(text, sizeof(text), "%s", raw);
    }

    close_modal();
    if (cb) {
        cb(true, text, value, ud);
    }
}

static void build_modal(void)
{
    /* Parented to lv_layer_top() -- see this file's header comment for why
     * that keeps it out of every page's own no-scroll content budget. */
    s_modal = lv_obj_create(lv_layer_top());
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(s_modal, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(s_modal, 0, 0);
    lv_obj_set_style_bg_color(s_modal, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_set_style_pad_all(s_modal, UI_THEME_PADDING_PX, 0);
    lv_obj_set_flex_flow(s_modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_modal, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);

    s_caption = lv_label_create(s_modal);
    lv_obj_set_style_text_color(s_caption, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(s_caption, "");

    s_ta = lv_textarea_create(s_modal);
    lv_obj_set_width(s_ta, UI_NUM_PAD_WIDTH_PX);
    lv_textarea_set_one_line(s_ta, true);

    s_range_label = lv_label_create(s_modal);
    lv_obj_set_style_text_color(s_range_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_range_label, "");

    lv_obj_t *btn_row = lv_obj_create(s_modal);
    lv_obj_set_width(btn_row, lv_pct(100));
    lv_obj_set_height(btn_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(btn_row, UI_THEME_PADDING_PX, 0);

    lv_obj_t *done_btn = lv_button_create(btn_row);
    lv_obj_set_height(done_btn, UI_THEME_MIN_TOUCH_TARGET_PX);
    lv_obj_set_flex_grow(done_btn, 1);
    lv_obj_set_style_bg_color(done_btn, UI_THEME_ACCENT_4, 0);
    lv_obj_set_style_radius(done_btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(done_btn, done_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *done_label = lv_label_create(done_btn);
    lv_label_set_text(done_label, "Done");
    lv_obj_center(done_label);
    lv_obj_update_layout(done_btn);
    ui_theme_apply_touch_area(done_btn, false);

    lv_obj_t *cancel_btn = lv_button_create(btn_row);
    lv_obj_set_height(cancel_btn, UI_THEME_MIN_TOUCH_TARGET_PX);
    lv_obj_set_flex_grow(cancel_btn, 1);
    lv_obj_set_style_bg_color(cancel_btn, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(cancel_btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(cancel_btn, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_center(cancel_label);
    lv_obj_update_layout(cancel_btn);
    ui_theme_apply_touch_area(cancel_btn, false);

    /* Same lv_keyboard_create()+lv_keyboard_set_textarea() pair
     * ui_page_network.c's connect modal already uses -- see this file's
     * header comment on why that means near-zero incremental flash cost. Its
     * default size fills whatever vertical space this column has left below
     * the widgets above, same as that modal. */
    s_kb = lv_keyboard_create(s_modal);
    lv_keyboard_set_textarea(s_kb, s_ta);
}

void ui_num_pad_show(const ui_num_pad_params_t *params)
{
    if (!params) {
        return;
    }
    if (!s_modal) {
        build_modal();
    }

    s_mode = params->mode;
    s_min = params->min;
    s_max = params->max;
    s_decimals = params->decimals;
    s_on_done = params->on_done;
    s_user_data = params->user_data;

    lv_label_set_text(s_caption, params->caption ? params->caption : "");

    if (params->mode == UI_NUM_PAD_MODE_NUMBER) {
        lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_NUMBER);
        lv_textarea_set_password_mode(s_ta, false);
        lv_textarea_set_max_length(s_ta, 32);
        char buf[32];
        snprintf(buf, sizeof(buf), "%.*f", params->decimals, (double)params->initial_value);
        lv_textarea_set_text(s_ta, buf);
        if (params->min < params->max) {
            lv_label_set_text_fmt(s_range_label, "Range: %.*f to %.*f", params->decimals, (double)params->min,
                                  params->decimals, (double)params->max);
        } else {
            lv_label_set_text(s_range_label, "");
        }
    } else {
        lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_textarea_set_password_mode(s_ta, false);
        lv_textarea_set_max_length(s_ta, params->max_len ? params->max_len : 63);
        lv_textarea_set_text(s_ta, params->initial_text ? params->initial_text : "");
        lv_label_set_text(s_range_label, "");
    }

    lv_obj_remove_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
}

void ui_num_pad_close(void)
{
    /* Relock/teardown: hide without firing the Done callback, and drop the
     * callback so nothing can write into a page that is no longer shown. */
    s_on_done = NULL;
    s_user_data = NULL;
    close_modal();
}
