#include "ui_page_touch_test.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "kiln_ui.h"
#include "ui_theme.h"

static const char *TAG = "ui_page_touch_test";

#define SQUARE_MARGIN_PX 24
#define LINE_WIDTH_PX 3
#define GUIDE_WIDTH_PX 2

static lv_obj_t *s_canvas;
static void *s_canvas_buf;     /* PSRAM, sized once for the real display resolution */
static int32_t s_canvas_w, s_canvas_h;

static lv_point_precise_t s_last_point;
static bool s_has_last_point;

/* Redraws the plain background and the square guide, discarding any drawn
 * trace -- shared by both the initial build and the "Clear" button. */
static void draw_guide(void)
{
    lv_canvas_fill_bg(s_canvas, UI_THEME_COLOR_BG, LV_OPA_COVER);

    lv_layer_t layer;
    lv_canvas_init_layer(s_canvas, &layer);

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = UI_THEME_COLOR_TEXT_SECONDARY;
    dsc.width = GUIDE_WIDTH_PX;
    dsc.round_start = 1;
    dsc.round_end = 1;

    int32_t x0 = SQUARE_MARGIN_PX;
    int32_t y0 = SQUARE_MARGIN_PX;
    int32_t x1 = s_canvas_w - 1 - SQUARE_MARGIN_PX;
    int32_t y1 = s_canvas_h - 1 - SQUARE_MARGIN_PX;

    lv_point_precise_t corners[5] = {
        { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 }, { x0, y0 },
    };
    for (int i = 0; i < 4; i++) {
        dsc.p1 = corners[i];
        dsc.p2 = corners[i + 1];
        lv_draw_line(&layer, &dsc);
    }

    lv_canvas_finish_layer(s_canvas, &layer);
    s_has_last_point = false;
}

/* Every point along a drag is drawn through touch_read_cb's ALREADY-
 * CALIBRATED point (lv_indev_get_point() reads back exactly the
 * data->point that callback set from touch_cal_apply()) -- so a wobbly or
 * offset trace against the square IS the calibration reading back
 * inaccurate, not an artifact of a separate/simplified test path. */
static void canvas_touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    if (code == LV_EVENT_RELEASED) {
        s_has_last_point = false;
        return;
    }

    lv_point_precise_t here = { point.x, point.y };

    if (s_has_last_point) {
        lv_layer_t layer;
        lv_canvas_init_layer(s_canvas, &layer);

        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = UI_THEME_ACCENT_3;
        dsc.width = LINE_WIDTH_PX;
        dsc.round_start = 1;
        dsc.round_end = 1;
        dsc.p1 = s_last_point;
        dsc.p2 = here;
        lv_draw_line(&layer, &dsc);

        lv_canvas_finish_layer(s_canvas, &layer);
    } else {
        /* First point of a new stroke -- nothing to connect to yet, just a
         * dot so a tap-without-drag still leaves a visible mark. */
        lv_canvas_set_px(s_canvas, here.x, here.y, UI_THEME_ACCENT_3, LV_OPA_COVER);
    }

    s_last_point = here;
    s_has_last_point = true;
}

static void clear_btn_cb(lv_event_t *e)
{
    (void)e;
    draw_guide();
}

static void done_btn_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("home");
}

lv_obj_t *ui_page_touch_test_build(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_color(title, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(title, "Trace the square to check calibration");

    s_canvas_w = lv_display_get_horizontal_resolution(NULL);
    s_canvas_h = lv_display_get_vertical_resolution(NULL) - UI_THEME_STATUS_BAR_HEIGHT_PX -
                 UI_THEME_MIN_TOUCH_TARGET_PX - 3 * UI_THEME_PADDING_PX;
    if (s_canvas_h < 60) s_canvas_h = 60; /* pathological-resolution floor, never expected in practice */

    size_t buf_bytes = (size_t)s_canvas_w * (size_t)s_canvas_h * 2u; /* RGB565 */
    s_canvas_buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    if (!s_canvas_buf) {
        ESP_LOGE(TAG, "PSRAM canvas buffer allocation failed (%u bytes)", (unsigned)buf_bytes);
        return scr;
    }

    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, s_canvas_w, s_canvas_h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_width(s_canvas, s_canvas_w);
    lv_obj_set_height(s_canvas, s_canvas_h);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_canvas, canvas_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_canvas, canvas_touch_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_canvas, canvas_touch_cb, LV_EVENT_RELEASED, NULL);

    draw_guide();

    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(row, UI_THEME_PADDING_PX, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *clear_btn = lv_button_create(row);
    lv_obj_set_size(clear_btn, UI_THEME_MIN_TOUCH_TARGET_PX * 2, UI_THEME_MIN_TOUCH_TARGET_PX);
    lv_obj_set_style_bg_color(clear_btn, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(clear_btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(clear_btn, clear_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clear_label = lv_label_create(clear_btn);
    lv_obj_set_style_text_color(clear_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(clear_label, "Clear");
    lv_obj_center(clear_label);

    lv_obj_t *done_btn = lv_button_create(row);
    lv_obj_set_size(done_btn, UI_THEME_MIN_TOUCH_TARGET_PX * 2, UI_THEME_MIN_TOUCH_TARGET_PX);
    lv_obj_set_style_bg_color(done_btn, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(done_btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(done_btn, done_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *done_label = lv_label_create(done_btn);
    lv_obj_set_style_text_color(done_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(done_label, "Done");
    lv_obj_center(done_label);

    return scr;
}
