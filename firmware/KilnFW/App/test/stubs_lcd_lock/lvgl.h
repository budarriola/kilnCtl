// Private shim of lvgl.h for test_ui_lcd_lock.c only (HOST_TEST_COVERAGE_GAPS
// round 2, R2-10). This directory is placed on the include path of that ONE
// executable's command line; it declares just the handful of lv_* symbols
// ui_lcd_lock.c and the real ui_theme.h/ui_confirm.h/ui_lcd_keypad.h headers
// touch. Behaviour (recording what ui_lcd_lock.c does to the "widgets") lives
// in the test file, so the state machine in ui_lcd_lock.c runs unmodified.
#ifndef TEST_STUB_LVGL_H
#define TEST_STUB_LVGL_H

#include <stdbool.h>
#include <stdint.h>

typedef struct lv_obj_t { int id; } lv_obj_t;
typedef struct lv_timer_t { int id; } lv_timer_t;
typedef struct lv_indev_t { int id; } lv_indev_t;
typedef struct lv_event_t { int id; } lv_event_t;
typedef struct { uint32_t full; } lv_color_t;
typedef int lv_event_code_t;
typedef struct { int32_t x; int32_t y; } lv_point_t;

#define LV_EVENT_CLICKED 1
#define LV_EVENT_PRESSED 2
#define LV_OBJ_FLAG_HIDDEN 1u
#define LV_OBJ_FLAG_CLICKABLE 2u

typedef void (*lv_event_cb_t)(lv_event_t *e);
typedef void (*lv_timer_cb_t)(lv_timer_t *t);

static inline lv_color_t lv_color_hex(uint32_t c)
{
    lv_color_t r = {c};
    return r;
}

lv_obj_t *lv_layer_top(void);
lv_obj_t *lv_msgbox_create(lv_obj_t *parent);
void lv_obj_set_width(lv_obj_t *o, int w);
void lv_obj_set_height(lv_obj_t *o, int h);
void lv_msgbox_add_title(lv_obj_t *mbox, const char *title);
lv_obj_t *lv_msgbox_add_text(lv_obj_t *mbox, const char *text);
lv_obj_t *lv_msgbox_add_footer_button(lv_obj_t *mbox, const char *text);
lv_obj_t *lv_msgbox_get_footer(lv_obj_t *mbox);
void lv_obj_set_style_bg_color(lv_obj_t *o, lv_color_t c, int sel);
void lv_obj_add_event_cb(lv_obj_t *o, lv_event_cb_t cb, lv_event_code_t code, void *ud);
lv_obj_t *lv_obj_get_parent(const lv_obj_t *o);
void lv_obj_add_flag(lv_obj_t *o, uint32_t f);
void lv_obj_remove_flag(lv_obj_t *o, uint32_t f);
bool lv_obj_has_flag(const lv_obj_t *o, uint32_t f);
void lv_obj_move_foreground(lv_obj_t *o);
void lv_label_set_text_fmt(lv_obj_t *label, const char *fmt, ...);
uint32_t lv_tick_get(void);
lv_timer_t *lv_timer_create(lv_timer_cb_t cb, uint32_t period, void *ud);
void lv_indev_add_event_cb(lv_indev_t *indev, lv_event_cb_t cb, lv_event_code_t code, void *ud);

#endif
