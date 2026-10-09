#include "ui_page_touch_cal.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"

#include "kiln_ui.h"
#include "lvgl_port.h"
#include "touch_cal_store.h"
#include "ui_lcd_lock.h"
#include "ui_theme.h"

// 2026-08-19 no-scroll audit (UI_PLAN.md section 3, LCD item 4): this page
// was previously unreviewed against the same no-page-scroll rule the other 8
// pages carry a budget comment for. Checked here rather than left silent.
// `scr` clears LV_OBJ_FLAG_SCROLLABLE (ui_page_touch_cal_build() below), same
// as every other page. Unlike the ~264px-budget pages, this page's content
// isn't sized against that fixed constant at all -- compute_targets() reads
// the *real* lv_display_get_horizontal/vertical_resolution() at build time
// and places every target dot, the progress label, and the full-screen
// overlay strictly within [0, width) x [0, height), by construction, rather
// than stacking a fixed set of rows that could overflow a fixed budget.
// There is nothing here that could grow past the screen regardless of
// resolution -- confirmed by reading compute_targets()/show_point() below,
// not just by the presence of the SCROLLABLE clear.
static const char *TAG = "ui_page_touch_cal";

/* 3x3 grid rather than the old 3-point corner/center/edge layout -- more
 * points means the least-squares affine fit (touch_cal_store.h) averages
 * out per-tap noise instead of being fully determined by three samples that
 * might each be slightly off-center. GRID_COLS/GRID_ROWS must multiply out
 * to POINT_COUNT. */
#define GRID_COLS 3
#define GRID_ROWS 3
#define POINT_COUNT (GRID_COLS * GRID_ROWS)

/* Repeat the whole grid this many times, averaging each point's raw sample
 * across cycles -- a single tap is noisy (finger contact area, pressure
 * variation), and this page's whole job is producing a calibration accurate
 * enough that every other page's buttons work reliably. */
#define CAL_CYCLES 3

#define TARGET_DIAMETER_PX 36

static lv_obj_t *s_target_circle;
static lv_obj_t *s_progress_label;
static lv_obj_t *s_cancel_btn;

#define CANCEL_BTN_DRAWN_PX 28

static int32_t s_target_x[POINT_COUNT];
static int32_t s_target_y[POINT_COUNT];

/* Every cycle's raw sample kept individually (not summed) -- see
 * finish_calibration()'s median comment for why: a dead/flaky patch of
 * panel can make one cycle's tap on a given point land nowhere near the
 * other two, and a running sum has no way to tell that outlier apart from
 * a good reading afterward. */
static uint16_t s_sample_raw_x[POINT_COUNT][CAL_CYCLES];
static uint16_t s_sample_raw_y[POINT_COUNT][CAL_CYCLES];
static uint16_t s_sample_fit_x[POINT_COUNT];
static uint16_t s_sample_fit_y[POINT_COUNT];
static int s_current_point;
static int s_current_cycle;

/* Computed once per build() from the real display resolution -- not
 * hardcoded against the "480x320 landscape" assumption every other page's
 * layout comment names, since this page's whole job is finding out how raw
 * touch maps onto whatever that resolution actually is. Targets sit right
 * at the screen edges/corners (margin = just the circle's own radius, so
 * the drawn dot touches the edge without being clipped by it) rather than
 * inset -- the affine fit is only as good as its extrapolation is short,
 * and a press near the physical edge of the panel is exactly where this
 * board's raw touch response has been least linear so far. */
static void compute_targets(int32_t width, int32_t height)
{
    int32_t side_margin = TARGET_DIAMETER_PX / 2;
    int32_t top = UI_THEME_STATUS_BAR_HEIGHT_PX + side_margin;
    int32_t usable_w = width - 2 * side_margin;
    int32_t usable_h = height - top - side_margin;

    for (int row = 0; row < GRID_ROWS; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            int i = row * GRID_COLS + col;
            s_target_x[i] = side_margin + (usable_w * col) / (GRID_COLS - 1);
            s_target_y[i] = top + (usable_h * row) / (GRID_ROWS - 1);
        }
    }
}

static void show_point(int point, int cycle)
{
    lv_obj_set_pos(s_target_circle, s_target_x[point] - TARGET_DIAMETER_PX / 2,
                    s_target_y[point] - TARGET_DIAMETER_PX / 2);
    lv_label_set_text_fmt(s_progress_label, "Touch Calibration -- tap the dot (%d/%d, pass %d/%d)",
                           point + 1, POINT_COUNT, cycle + 1, CAL_CYCLES);
}

/* Sorts a small local copy and returns the middle value. Only ever called
 * with CAL_CYCLES (3) elements -- insertion sort, not because 3 elements
 * needs an algorithm, but so this still does the right thing if CAL_CYCLES
 * is ever raised. */
static uint16_t median_of(const uint16_t *values, int n)
{
    uint16_t sorted[CAL_CYCLES];
    memcpy(sorted, values, (size_t)n * sizeof(uint16_t));
    for (int i = 1; i < n; i++) {
        uint16_t key = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > key) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }
    return sorted[n / 2];
}

/* A raw reading more than this many ADC counts from its point's median is
 * logged as a likely dead/flaky-panel-patch outlier -- purely diagnostic
 * (the median already excludes it from the fit on its own), so this can be
 * read back later to see which screen regions this board's touch panel is
 * least reliable in, without needing to reproduce the problem live. ~10% of
 * the 0..NS2009_ADC_MAX span. */
#define OUTLIER_WARN_THRESHOLD_COUNTS 400

static void finish_calibration(void)
{
    for (int i = 0; i < POINT_COUNT; i++) {
        /* Median rather than mean across the CAL_CYCLES samples for this
         * point: a mean lets one bad cycle (a tap that landed on a
         * dead/inconsistent patch of panel, so its raw reading has nothing
         * to do with where the finger actually was) drag the whole point
         * toward a wrong raw value. The median of 3 simply ignores whichever
         * single reading is the outlier, so one bad cycle out of three costs
         * nothing as long as the other two agree. */
        s_sample_fit_x[i] = median_of(s_sample_raw_x[i], CAL_CYCLES);
        s_sample_fit_y[i] = median_of(s_sample_raw_y[i], CAL_CYCLES);

        for (int c = 0; c < CAL_CYCLES; c++) {
            int dx = (int)s_sample_raw_x[i][c] - (int)s_sample_fit_x[i];
            int dy = (int)s_sample_raw_y[i][c] - (int)s_sample_fit_y[i];
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx > OUTLIER_WARN_THRESHOLD_COUNTS || dy > OUTLIER_WARN_THRESHOLD_COUNTS) {
                ESP_LOGW(TAG,
                         "point=%d/%d target_px=(%" PRId32 ",%" PRId32
                         ") cycle=%d raw=(%u,%u) is %d/%d counts off this point's median "
                         "(%u,%u) -- possible dead/flaky panel patch near this screen "
                         "location; median used it anyway, worth a look if the trace test "
                         "looks wrong nearby",
                         i + 1, POINT_COUNT, s_target_x[i], s_target_y[i], c + 1,
                         s_sample_raw_x[i][c], s_sample_raw_y[i][c], dx, dy, s_sample_fit_x[i],
                         s_sample_fit_y[i]);
            }
        }
    }

    touch_cal_t cal;
    cal.calibrated = true;
    esp_err_t err = touch_cal_fit(s_sample_fit_x, s_sample_fit_y, s_target_x, s_target_y,
                                   POINT_COUNT, &cal);
    if (err != ESP_OK) {
        /* Degenerate fit (e.g. every tap landed in the same spot) -- restart
         * the sequence rather than saving garbage that would make every
         * other page's buttons unreachable, which is exactly the bug this
         * page exists to fix. */
        ESP_LOGW(TAG, "calibration fit failed (%s) -- restarting", esp_err_to_name(err));
        s_current_point = 0;
        s_current_cycle = 0;
        memset(s_sample_raw_x, 0, sizeof(s_sample_raw_x));
        memset(s_sample_raw_y, 0, sizeof(s_sample_raw_y));
        show_point(0, 0);
        return;
    }

    /* L11: overwriting an existing calibration is an admin write; a first-boot
     * (never calibrated) save stays open since the PIN keypad needs touch. */
    if (touch_cal_store_is_calibrated() && !ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN)) {
        ESP_LOGW(TAG, "calibration not saved: admin session required");
        kiln_ui_show("config");
        return;
    }
    err = touch_cal_store_save(&cal);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "calibration computed but not saved (%s) -- will need to be redone after reboot",
                 esp_err_to_name(err));
    }
    lvgl_port_reload_touch_cal();
    /* Not straight to "home" -- ui_page_touch_test.c lets the new mapping be
     * checked by eye (trace a square, see if the drawn line follows the
     * finger) before trusting every other page's buttons to it. Its own
     * "Done" button goes to "home" from there. */
    kiln_ui_show("touch_test");
}

/* The whole point of this page's design: normal LVGL hit-testing needs an
 * already-working raw-to-screen mapping to know a tap landed on the visible
 * dot, but that mapping is exactly what hasn't been established yet (see
 * touch_cal_store.h's header comment on the old swap/invert model being
 * wrong). A full-screen transparent button sidesteps that entirely -- ANY
 * press while a given point is showing is that point's sample, wherever it
 * physically landed, per the explicit requirement that each press counts as
 * a press on the current target. */
static void overlay_press_cb(lv_event_t *e)
{
    (void)e;
    uint16_t raw_x = 0, raw_y = 0, z1 = 0;
    lvgl_port_get_last_raw_touch(&raw_x, &raw_y, &z1);

    s_sample_raw_x[s_current_point][s_current_cycle] = raw_x;
    s_sample_raw_y[s_current_point][s_current_cycle] = raw_y;
    ESP_LOGI(TAG, "point=%d/%d pass=%d/%d target_px=(%" PRId32 ",%" PRId32 ") raw_x=%u raw_y=%u z1=%u",
             s_current_point + 1, POINT_COUNT, s_current_cycle + 1, CAL_CYCLES,
             s_target_x[s_current_point], s_target_y[s_current_point], raw_x, raw_y, z1);

    s_current_point++;
    if (s_current_point >= POINT_COUNT) {
        s_current_point = 0;
        s_current_cycle++;
        if (s_current_cycle >= CAL_CYCLES) {
            finish_calibration();
            return;
        }
    }
    show_point(s_current_point, s_current_cycle);
}

/* 2026-08-19: cancel/back path, added per explicit user request (UI_PLAN.md
 * LCD work-queue item 6). Returns to "config" without calling
 * touch_cal_store_save() -- a cancelled run must not persist a partial or
 * garbage calibration. Deliberately does NOT reset s_current_point/cycle or
 * the sample buffers here: on_screen_loaded() already does that
 * unconditionally on every kiln_ui_show("touch_cal"), so a later
 * re-calibration attempt always starts clean regardless of how the previous
 * attempt ended. */
static void cancel_press_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "calibration cancelled by user at point=%d/%d pass=%d/%d",
              s_current_point + 1, POINT_COUNT, s_current_cycle + 1, CAL_CYCLES);
    kiln_ui_show("config");
}

/* Fired every time this screen is loaded (kiln_ui.c builds a page's screen
 * object once and reuses it thereafter -- see kiln_ui_show()'s doc comment
 * -- so per-run state has to be reset here rather than only in build()).
 * Restarts the sequence from point 0 whether this is the forced first-run
 * pass or a deliberate re-calibration from the config nav hub. */
static void on_screen_loaded(lv_event_t *e)
{
    (void)e;
    s_current_point = 0;
    s_current_cycle = 0;
    memset(s_sample_raw_x, 0, sizeof(s_sample_raw_x));
    memset(s_sample_raw_y, 0, sizeof(s_sample_raw_y));
    show_point(0, 0);

    /* Cancel only makes sense when a working touch mapping already exists to
     * fall back to (kiln_ui.c's forced-first-boot comment: an uncalibrated
     * board's other pages' buttons don't reliably work yet, so "cancel" would
     * strand the user on a screen they can't navigate). On a forced first
     * run there is nowhere safe to cancel to, so the button stays hidden and
     * non-clickable -- overlay still owns the whole screen in that case,
     * unchanged from before this pass. */
    bool can_cancel = touch_cal_store_is_calibrated();
    if (can_cancel) {
        lv_obj_remove_flag(s_cancel_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_cancel_btn, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_add_flag(s_cancel_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_cancel_btn, LV_OBJ_FLAG_CLICKABLE);
    }
}

/* THE REFUSAL PATH. Every surface that OFFERS calibration hides the option
 * when the wired-in controller does not support it, but hiding a button in
 * front of a live page is the same lie one layer down -- this page is
 * registered by name in kiln_ui.c's page table, so any present or future
 * kiln_ui_show("touch_cal") reaches it regardless of which buttons exist.
 * So the page itself refuses, and says which of the two unsupported cases
 * it is rather than presenting one message for both:
 *
 *  - SELF_CALIBRATING: a self-calibrating controller (touch_dev.h;
 *    FT6336U.h -- the bench unit's actual panel) never populates
 *    touch_cal_store; it bypasses that fit entirely, by design. The 3x3 grid
 *    below could never be completed on one: every tap would append to a
 *    calibration nothing ever reads back (touch_read_cb() does not even
 *    check s_touch_cal.calibrated for a self_calibrating device -- see
 *    lvgl_port.c).
 *  - NO_TOUCH: no controller came up at all. Presenting a grid that cannot
 *    register a single press would leave the operator stuck on a page with
 *    no working way off it. This case is stated as a DETECTION FAILURE, not
 *    as "none needed" -- the part that would be there may well be one
 *    calibration is meaningful for; nothing answered this boot. An unknown
 *    must not read as a confident "not supported". */
static lv_obj_t *build_unsupported_notice(touch_cal_support_t support)
{
    const char *body =
        (support == TOUCH_CAL_SUPPORT_NO_TOUCH)
            ? "No touch controller was detected at startup.\n"
              "Touch calibration is unavailable until one responds."
            : "This touch controller self-calibrates.\n"
              "No calibration step is needed or available.";

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, body);
    lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, lv_pct(80));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);

    lv_obj_t *back_btn = lv_button_create(scr);
    lv_obj_set_size(back_btn, UI_THEME_MIN_TOUCH_TARGET_PX * 2, UI_THEME_MIN_TOUCH_TARGET_PX);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -UI_THEME_PADDING_PX);
    lv_obj_set_style_bg_color(back_btn, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(back_btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(back_btn, cancel_press_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_set_style_text_color(back_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_center(back_label);

    return scr;
}

lv_obj_t *ui_page_touch_cal_build(void)
{
    /* Refuse rather than pretend -- see build_unsupported_notice(). The
     * shared predicate, not a locally re-derived condition: this page, the
     * config hub's nav cell, kiln_ui.c's boot gate and /api/status all read
     * the same touch_dev_cal_support() answer. */
    const touch_cal_support_t support = lvgl_port_touch_cal_support();
    if (!touch_cal_support_is_offerable(support)) {
        return build_unsupported_notice(support);
    }

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_screen_loaded, LV_EVENT_SCREEN_LOADED, NULL);

    int32_t width = lv_display_get_horizontal_resolution(NULL);
    int32_t height = lv_display_get_vertical_resolution(NULL);
    compute_targets(width, height);

    s_progress_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_progress_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_align(s_progress_label, LV_ALIGN_TOP_MID, 0, UI_THEME_PADDING_PX);

    s_target_circle = lv_obj_create(scr);
    lv_obj_set_size(s_target_circle, TARGET_DIAMETER_PX, TARGET_DIAMETER_PX);
    lv_obj_set_style_radius(s_target_circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_target_circle, UI_THEME_ACCENT_3, 0);
    lv_obj_set_style_bg_opa(s_target_circle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_target_circle, 0, 0);
    lv_obj_remove_flag(s_target_circle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_target_circle, LV_OBJ_FLAG_CLICKABLE); /* the overlay owns hit-testing */

    /* Full-screen transparent button. s_target_circle already has
     * LV_OBJ_FLAG_CLICKABLE removed above, so it's invisible to hit-testing
     * regardless of z-order -- this overlay is the only clickable object on
     * the page, and its transparent background leaves the dot fully
     * visible underneath. */
    lv_obj_t *overlay = lv_obj_create(scr);
    lv_obj_set_size(overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(overlay, overlay_press_cb, LV_EVENT_CLICKED, NULL);

    /* Cancel button, created after `overlay` so it draws (and hit-tests) on
     * top of it -- lv_indev_search_obj() checks highest z-order first, so a
     * later sibling always wins a point that lands inside both. Sits in the
     * top-left status-bar strip: compute_targets() already reserves
     * UI_THEME_STATUS_BAR_HEIGHT_PX at the top before placing any grid
     * target there, so this can't overlap a calibration point or shrink
     * compute_targets()'s usable area (targets stay at the true screen edge,
     * which compute_targets()'s own comment explains matters for the affine
     * fit's extrapolation accuracy). Drawn small (28px, below
     * UI_THEME_MIN_TOUCH_TARGET_PX) to fit the 32px-tall status-bar strip --
     * same "invisible extended hit area" pattern UI_PLAN.md already names
     * for ui_page_network.c's/ui_page_temperature.c's other shrunk buttons,
     * restored toward 72px via ui_theme_apply_touch_area()'s
     * compact_layout=false path below. Starts hidden; on_screen_loaded()
     * decides visibility every time this page is shown, based on whether a
     * working calibration already exists to cancel back to. */
    s_cancel_btn = lv_button_create(scr);
    lv_obj_set_size(s_cancel_btn, 64, CANCEL_BTN_DRAWN_PX);
    lv_obj_set_pos(s_cancel_btn, UI_THEME_PADDING_PX / 2, UI_THEME_PADDING_PX / 2);
    lv_obj_set_style_radius(s_cancel_btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_bg_color(s_cancel_btn, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(s_cancel_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_cancel_btn, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_border_opa(s_cancel_btn, LV_OPA_40, 0);
    lv_obj_set_style_border_width(s_cancel_btn, 1, 0);
    lv_obj_remove_flag(s_cancel_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_cancel_btn, LV_OBJ_FLAG_HIDDEN);
    /* lv_button_create() objects are clickable by default -- explicitly
     * remove it here too so a hidden-but-not-yet-shown button can never eat
     * a touch meant for the overlay beneath it before on_screen_loaded()
     * decides visibility for the first time. */
    lv_obj_remove_flag(s_cancel_btn, LV_OBJ_FLAG_CLICKABLE);
    ui_theme_apply_touch_area(s_cancel_btn, false);
    lv_obj_add_event_cb(s_cancel_btn, cancel_press_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel_label = lv_label_create(s_cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_center(cancel_label);

    /* Also done in on_screen_loaded() (fired by kiln_ui_show()'s
     * lv_screen_load() right after this returns) -- set here too, so the
     * very first frame drawn already shows point 1 rather than a blank
     * label for however long LVGL takes to dispatch that event. */
    s_current_point = 0;
    s_current_cycle = 0;
    memset(s_sample_raw_x, 0, sizeof(s_sample_raw_x));
    memset(s_sample_raw_y, 0, sizeof(s_sample_raw_y));
    show_point(0, 0);

    return scr;
}
