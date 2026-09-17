#include "ui_lcd_keypad.h"

#include <stdio.h>
#include <string.h>

#include "ui_theme.h"

#define UI_LCD_KEYPAD_WIDTH_PX 400

static lv_obj_t *s_mbox;
static lv_obj_t *s_prompt_label;
static lv_obj_t *s_dots_label;
static lv_obj_t *s_status_label; // "Wrong PIN" / lockout countdown -- empty otherwise
static lv_obj_t *s_bm;

static lcd_keypad_state_t s_ks;
static ui_lcd_keypad_done_cb_t s_on_done;
static void *s_user_data;

static const char *const s_bm_map[] = {
    "1", "2", "3", "\n",
    "4", "5", "6", "\n",
    "7", "8", "9", "\n",
    LV_SYMBOL_BACKSPACE, "0", "OK", "",
};

static void refresh_dots(void)
{
    char buf[LCD_PIN_MAX_DIGITS + 1];
    uint8_t n = s_ks.entry.len;
    for (uint8_t i = 0; i < n; i++) {
        buf[i] = '*';
    }
    buf[n] = '\0';
    lv_label_set_text(s_dots_label, buf);
}

static void set_status(const char *text)
{
    lv_label_set_text(s_status_label, text ? text : "");
}

static void close_overlay(void)
{
    if (s_mbox) {
        lv_obj_add_flag(s_mbox, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *backdrop = lv_obj_get_parent(s_mbox);
        if (backdrop) {
            lv_obj_add_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void finish(bool granted, lcd_pin_role_t role)
{
    ui_lcd_keypad_done_cb_t cb = s_on_done;
    void *ud = s_user_data;
    close_overlay();
    lcd_pin_entry_reset(&s_ks.entry);
    if (cb) {
        cb(granted, role, ud);
    }
}

static void cancel_cb(lv_event_t *e)
{
    (void)e;
    finish(false, LCD_PIN_ROLE_NONE);
}

static void backdrop_click_cb(lv_event_t *e)
{
    /* "a tap on the backdrop ... here it cancels" -- section 7. Distinct
     * from ui_confirm.c's/ui_num_pad.c's backdrop, which deliberately
     * swallows a stray tap and does nothing; a PIN overlay is cheap to
     * re-open and an operator tapping outside it clearly wants out. */
    (void)e;
    finish(false, LCD_PIN_ROLE_NONE);
}

static void try_submit(void)
{
    if (!lcd_pin_entry_can_submit(&s_ks.entry)) {
        /* "OK is inert below 4 digits" -- do nothing, not even a status
         * message, since the digit dots already show the count. */
        return;
    }

    uint32_t now_ms = (uint32_t)lv_tick_get();
    lcd_pin_role_t role = LCD_PIN_ROLE_NONE;
    lcd_keypad_submit_result_t result = lcd_keypad_state_submit(&s_ks, now_ms, &role);

    switch (result) {
        case LCD_KEYPAD_SUBMIT_GRANTED:
            finish(true, role);
            break;
        case LCD_KEYPAD_SUBMIT_LOCKED_OUT:
            lcd_pin_entry_reset(&s_ks.entry);
            refresh_dots();
            set_status("Too many attempts -- panel locked, try again later");
            break;
        case LCD_KEYPAD_SUBMIT_DENIED:
            lcd_pin_entry_reset(&s_ks.entry);
            refresh_dots();
            set_status("Wrong PIN");
            break;
        case LCD_KEYPAD_SUBMIT_TOO_SHORT:
        default:
            /* Defensive-only path (see lcd_auth_state.h) -- can't happen
             * given the can_submit() guard just above. */
            break;
    }
}

static void bm_value_changed_cb(lv_event_t *e)
{
    lv_obj_t *bm = (lv_obj_t *)lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(bm);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) {
        return;
    }
    const char *key = lv_buttonmatrix_get_button_text(bm, id);
    if (!key) {
        return;
    }

    set_status(""); // any keypress clears a stale "Wrong PIN" message

    if (strcmp(key, "OK") == 0) {
        try_submit();
    } else if (strcmp(key, LV_SYMBOL_BACKSPACE) == 0) {
        lcd_pin_entry_backspace(&s_ks.entry);
        refresh_dots();
    } else if (key[0] >= '0' && key[0] <= '9' && key[1] == '\0') {
        lcd_pin_entry_push_digit(&s_ks.entry, key[0]);
        refresh_dots();
    }
}

static void build_overlay(void)
{
    lcd_keypad_state_init(&s_ks);

    s_mbox = lv_msgbox_create(NULL);
    lv_obj_set_width(s_mbox, UI_LCD_KEYPAD_WIDTH_PX);

    s_prompt_label = lv_msgbox_add_title(s_mbox, "");

    lv_obj_t *content = lv_msgbox_get_content(s_mbox);

    s_dots_label = lv_label_create(content);
    lv_obj_set_style_text_color(s_dots_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(s_dots_label, "");

    s_status_label = lv_label_create(content);
    lv_obj_set_style_text_color(s_status_label, UI_THEME_ACCENT_5, 0);
    lv_label_set_text(s_status_label, "");

    s_bm = lv_buttonmatrix_create(content);
    lv_buttonmatrix_set_map(s_bm, s_bm_map);
    lv_obj_set_size(s_bm, lv_pct(100), 3 * UI_THEME_MIN_TOUCH_TARGET_PX);
    lv_obj_add_event_cb(s_bm, bm_value_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *cancel_btn = lv_msgbox_add_footer_button(s_mbox, "Cancel");
    lv_obj_set_style_bg_color(cancel_btn, UI_THEME_COLOR_CARD, 0);
    lv_obj_add_event_cb(cancel_btn, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *footer = lv_msgbox_get_footer(s_mbox);
    lv_obj_set_height(footer, UI_THEME_MIN_TOUCH_TARGET_PX);

    lv_obj_t *backdrop = lv_obj_get_parent(s_mbox);
    if (backdrop) {
        lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(backdrop, backdrop_click_cb, LV_EVENT_CLICKED, NULL);
    }
}

void ui_lcd_keypad_show(const char *prompt, ui_lcd_keypad_done_cb_t on_done, void *user_data)
{
    if (!s_mbox) {
        build_overlay();
    }

    lcd_pin_entry_reset(&s_ks.entry);
    refresh_dots();
    set_status("");
    lv_label_set_text(s_prompt_label, prompt ? prompt : "Enter PIN");
    s_on_done = on_done;
    s_user_data = user_data;

    lv_obj_t *backdrop = lv_obj_get_parent(s_mbox);
    if (backdrop) {
        lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(backdrop);
    }
    lv_obj_remove_flag(s_mbox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_mbox);
}

bool ui_lcd_keypad_is_open(void)
{
    return s_mbox != NULL && !lv_obj_has_flag(s_mbox, LV_OBJ_FLAG_HIDDEN);
}

void ui_lcd_keypad_force_close(void)
{
    if (!ui_lcd_keypad_is_open()) {
        return;
    }
    finish(false, LCD_PIN_ROLE_NONE);
}
