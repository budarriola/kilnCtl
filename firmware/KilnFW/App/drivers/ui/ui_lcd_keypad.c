#include "ui_lcd_keypad.h"

#include <stdio.h>
#include <string.h>

#include "ui_theme.h"

#define UI_LCD_KEYPAD_WIDTH_PX 400

/* ---- No-scroll fit, bug found by the 2026-09-25 LCD-01 bench run
 * (logs/bench_test/20260925T150107Z_lcd) -- the old layout (default
 * lv_msgbox header ~43px + a separate dots label + a separate status label
 * + a 3*UI_THEME_MIN_TOUCH_TARGET_PX (216px) button matrix + a
 * UI_THEME_MIN_TOUCH_TARGET_PX (72px) footer, plus theme padding) summed to
 * well over DISPLAY_WIDTH (320, the landscape height -- see ui_theme.h's
 * UI_THEME_PAGE_CONTENT_BUDGET_PX comment for why DISPLAY_WIDTH is the
 * landscape HEIGHT), so the title clipped above the top edge and Cancel
 * fell below the bottom edge. This box is a full-screen lv_msgbox_create(NULL)
 * overlay, not a topbar'd page, so the applicable ceiling is the whole
 * landscape height, not UI_THEME_PAGE_CONTENT_BUDGET_PX.
 *
 * Every pad/height below is set explicitly on the relevant lv_obj (never
 * left at the default theme's lv_theme_default.c PAD_SMALL, which is
 * itself DPI-derived and not meant to be reverse-engineered here) so this
 * arithmetic is exact, not a guess about theme internals. Mirrors the real
 * lv_obj_set_* calls in build_overlay() below -- keep both in sync. */
#define UI_LCD_KEYPAD_HEADER_PAD_PX          4
#define UI_LCD_KEYPAD_HEADER_HEIGHT_PX       (2 * UI_LCD_KEYPAD_HEADER_PAD_PX + UI_THEME_FONT_LINE_HEIGHT_PX)

#define UI_LCD_KEYPAD_CONTENT_PAD_PX         4
#define UI_LCD_KEYPAD_INFO_ROW_HEIGHT_PX     UI_THEME_FONT_LINE_HEIGHT_PX
#define UI_LCD_KEYPAD_BUTTON_ROW_HEIGHT_PX   56
#define UI_LCD_KEYPAD_MATRIX_HEIGHT_PX       (3 * UI_LCD_KEYPAD_BUTTON_ROW_HEIGHT_PX)
/* content pad_top + info row + content pad_row (gap) + matrix + content pad_bottom */
#define UI_LCD_KEYPAD_CONTENT_HEIGHT_PX \
    (UI_LCD_KEYPAD_CONTENT_PAD_PX + UI_LCD_KEYPAD_INFO_ROW_HEIGHT_PX + UI_LCD_KEYPAD_CONTENT_PAD_PX + \
     UI_LCD_KEYPAD_MATRIX_HEIGHT_PX + UI_LCD_KEYPAD_CONTENT_PAD_PX)

#define UI_LCD_KEYPAD_FOOTER_HEIGHT_PX       56
#define UI_LCD_KEYPAD_FOOTER_PAD_VER_PX      4
#define UI_LCD_KEYPAD_FOOTER_BUTTON_HEIGHT_PX \
    (UI_LCD_KEYPAD_FOOTER_HEIGHT_PX - 2 * UI_LCD_KEYPAD_FOOTER_PAD_VER_PX)

/* mbox itself carries the default theme's pad_zero (0 padding, 0 gap
 * between header/content/footer) -- see lv_theme_default.c's
 * `lv_obj_check_type(obj, &lv_msgbox_class)` branch -- so the three
 * sections stack with no extra gap between them. */
#define UI_LCD_KEYPAD_TOTAL_HEIGHT_PX \
    (UI_LCD_KEYPAD_HEADER_HEIGHT_PX + UI_LCD_KEYPAD_CONTENT_HEIGHT_PX + UI_LCD_KEYPAD_FOOTER_HEIGHT_PX)

_Static_assert(UI_LCD_KEYPAD_TOTAL_HEIGHT_PX <= DISPLAY_WIDTH,
               "ui_lcd_keypad.c: the PIN keypad overlay's total height exceeds the "
               "320px landscape panel height (DISPLAY_WIDTH) -- shrink "
               "UI_LCD_KEYPAD_BUTTON_ROW_HEIGHT_PX/FOOTER_HEIGHT_PX, don't let it "
               "overflow the screen.");
_Static_assert(UI_LCD_KEYPAD_BUTTON_ROW_HEIGHT_PX >= 48,
               "ui_lcd_keypad.c: keypad button rows must stay comfortably tappable "
               "(>= ~48px tall).");
_Static_assert(UI_LCD_KEYPAD_FOOTER_BUTTON_HEIGHT_PX >= 48,
               "ui_lcd_keypad.c: the Cancel footer button must stay comfortably "
               "tappable (>= ~48px tall).");

static lv_obj_t *s_mbox;
static lv_obj_t *s_prompt_label;
static lv_obj_t *s_info_label; // dots normally; "Wrong PIN" / lockout countdown on error
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
    lv_obj_set_style_text_color(s_info_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(s_info_label, buf);
}

static void set_status(const char *text)
{
    lv_obj_set_style_text_color(s_info_label, UI_THEME_ACCENT_5, 0);
    lv_label_set_text(s_info_label, text ? text : "");
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

    if (strcmp(key, "OK") == 0) {
        /* Deliberately does NOT blank the info label first: the dots/status
         * label is now shared (see this file's header comment), and OK on a
         * too-short entry is a defined no-op (try_submit()'s can_submit()
         * guard) that must leave whatever is currently showing (dots or a
         * still-relevant error) alone rather than wiping it. */
        try_submit();
    } else if (strcmp(key, LV_SYMBOL_BACKSPACE) == 0) {
        lcd_pin_entry_backspace(&s_ks.entry);
        refresh_dots(); // clears a stale "Wrong PIN" message same as any digit edit
    } else if (key[0] >= '0' && key[0] <= '9' && key[1] == '\0') {
        lcd_pin_entry_push_digit(&s_ks.entry, key[0]);
        refresh_dots(); // clears a stale "Wrong PIN" message same as any digit edit
    }
}

static void build_overlay(void)
{
    lcd_keypad_state_init(&s_ks);

    s_mbox = lv_msgbox_create(NULL);
    lv_obj_set_width(s_mbox, UI_LCD_KEYPAD_WIDTH_PX);

    s_prompt_label = lv_msgbox_add_title(s_mbox, "");
    lv_obj_t *header = lv_msgbox_get_header(s_mbox);
    lv_obj_set_height(header, UI_LCD_KEYPAD_HEADER_HEIGHT_PX);
    lv_obj_set_style_pad_all(header, UI_LCD_KEYPAD_HEADER_PAD_PX, 0);

    lv_obj_t *content = lv_msgbox_get_content(s_mbox);
    lv_obj_set_style_pad_all(content, UI_LCD_KEYPAD_CONTENT_PAD_PX, 0);
    lv_obj_set_style_pad_row(content, UI_LCD_KEYPAD_CONTENT_PAD_PX, 0);

    /* Dots and status share one label (2026-09-25 fix -- see this file's
     * header comment): a separate always-present status line was part of
     * what pushed the overlay's total height past the 320px panel. */
    s_info_label = lv_label_create(content);
    lv_obj_set_height(s_info_label, UI_LCD_KEYPAD_INFO_ROW_HEIGHT_PX);
    lv_label_set_text(s_info_label, "");

    s_bm = lv_buttonmatrix_create(content);
    lv_buttonmatrix_set_map(s_bm, s_bm_map);
    lv_obj_set_size(s_bm, lv_pct(100), UI_LCD_KEYPAD_MATRIX_HEIGHT_PX);
    lv_obj_add_event_cb(s_bm, bm_value_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *cancel_btn = lv_msgbox_add_footer_button(s_mbox, "Cancel");
    lv_obj_set_style_bg_color(cancel_btn, UI_THEME_COLOR_CARD, 0);
    lv_obj_add_event_cb(cancel_btn, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *footer = lv_msgbox_get_footer(s_mbox);
    lv_obj_set_height(footer, UI_LCD_KEYPAD_FOOTER_HEIGHT_PX);
    lv_obj_set_style_pad_ver(footer, UI_LCD_KEYPAD_FOOTER_PAD_VER_PX, 0);

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
    refresh_dots(); // shared label -- also clears any stale status text from a prior open
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
