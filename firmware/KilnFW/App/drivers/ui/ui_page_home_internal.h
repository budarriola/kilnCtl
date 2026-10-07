#ifndef UI_PAGE_HOME_INTERNAL_H
#define UI_PAGE_HOME_INTERNAL_H

/* Internal seams for the ui_page_home.c split (2026-09-04, ROADMAP.md M15's
 * 1500-line item -- ui_page_home.c had grown to 2279 lines). This header is
 * NOT public API -- ui_page_home.h stays that (just ui_page_home_build()) --
 * it exists purely so pieces that used to be one translation unit (and could
 * reach each other's `static` state and helpers for free) can still do so
 * now that they are four:
 *
 *   ui_page_home.c         -- includes/header story, all shared statics,
 *                              ui_page_home_build() (the public entry point)
 *   ui_page_home_actions.c -- Start/Stop/Pause action handlers, confirm
 *                              dialogs, the merged fire button, topbar menu
 *                              nav, and the small build_button() helper
 *   ui_page_home_chart.c   -- planned-curve lookup, the dashed-line/tick-mark
 *                              LVGL draw-event hooks, and the Y/X-tick and
 *                              legend layout helpers
 *   ui_page_home_refresh.c -- ui_home_refresh_cb(), the 1 Hz timer callback
 *                              that repaints every live number on the page
 *
 * Every symbol declared below was `static` in the original single file and
 * is widened to file-scope-internal linkage ONLY because a sibling .c file
 * in this split now calls or reads it directly. Every one of them is
 * prefixed `ui_home_`/`s_ui_home_` (functions and statics alike), including
 * several that audited clean against the rest of App/drivers/ at split time
 * -- generic names (`TAG`, `refresh_cb`, `menu_nav_cb`, `plan_lookup`,
 * `chart_draw_event_cb`, `s_status_label`, `s_topbar`, `s_progress_label`,
 * `s_zone_count`) are exactly the ones likeliest to collide with another
 * driver file's own `static` of the same name once either side stops being
 * `static`, so the split renames unconditionally rather than only where a
 * collision was found this time. See the split's commit message for the
 * full "every symbol whose linkage changed, and why" accounting. */

#include "ui_page_home.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "esp_log.h"

#include "MAX31856.h"
#include "dashboard_http.h"
#include "kiln_io.h"
#include "kiln_ui.h"
#include "ui_confirm.h"
#include "watchdog_cfg.h"
#include "ui_page_config.h"
#include "safety_trip_words.h"
#include "profile_executor.h"
#include "profile_feasibility.h"
#include "profiles_http.h"
#include "ramp_assist_cfg.h"
#include "auth_reset_gesture.h" /* docs/WEB_AUTH_PLAN.md item 10 -- the four-corner
                                   * physical credential-reset gesture; this page owns
                                   * the corner hit zones and confirm dialog per that
                                   * item's "LCD-owning agent" split. */
#include "heat_enable.h" /* heat_enable_is_granted() -- one of the gesture's three
                            * live preconditions. */
#include "ui_page_home_graph.h"
#include "ui_page_home_rail.h"
#include "run_state.h"
#include "ui_theme.h"
#include "ui_topbar.h"
#include "wifi_status_ui.h"
#include "zones_config_accessors.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- shared log tag ---------------------------------------------------- */
extern const char *UI_HOME_TAG;

/* ---- chart geometry / point-count constants, shared by build + chart +
 * refresh ------------------------------------------------------------- */
#define UI_PAGE_HOME_REFRESH_MS 1000
#define UI_PAGE_HOME_CHART_POINTS 30
#define UI_PAGE_HOME_Y_TICK_COUNT 6
#define UI_PAGE_HOME_X_TICK_COUNT 4
#define UI_PAGE_HOME_LEGEND_ROWS 2
#define UI_PAGE_HOME_PLAN_COLOR_HEX 0x9966cc
#define UI_PAGE_HOME_PLAN_DASH_WIDTH_PX 6
#define UI_PAGE_HOME_PLAN_DASH_GAP_PX   4

/* ---- shared widget/state statics, built once in ui_page_home_build() and
 * read/written by refresh.c and (for a few) actions.c / chart.c ---------- */
extern uint8_t s_ui_home_zone_count;
extern lv_obj_t *s_ui_home_trip_strip;
/* True only while the strip is showing a LIVE safety trip (set every refresh
 * from ui_safety_view_derive(), never latched). A tap on the strip opens the
 * Safety page only then; viewing needs no PIN (owner 2026-10-07). */
extern bool s_ui_home_trip_strip_is_safety;
extern lv_obj_t *s_ui_home_lag_notice;
extern uint32_t s_ui_home_lag_notice_ticks;
extern lv_obj_t *s_ui_home_chart;
extern lv_chart_series_t *s_ui_home_chart_actual_series;
extern lv_chart_series_t *s_ui_home_chart_planned_series;
extern int32_t s_ui_home_chart_actual_pts[UI_PAGE_HOME_CHART_POINTS];
extern int32_t s_ui_home_chart_planned_pts[UI_PAGE_HOME_CHART_POINTS];
extern lv_obj_t *s_ui_home_chart_y_tick_labels[UI_PAGE_HOME_Y_TICK_COUNT];
extern int32_t s_ui_home_chart_axis_lo, s_ui_home_chart_axis_hi;
extern bool s_ui_home_chart_y_ticks_visible;
extern bool s_ui_home_axis_hold_have;
extern int32_t s_ui_home_axis_hold_lo, s_ui_home_axis_hold_hi;
extern bool s_ui_home_axis_hold_prev_active;
extern uint32_t s_ui_home_axis_hold_prev_elapsed_s;
extern lv_obj_t *s_ui_home_chart_x_tick_labels[UI_PAGE_HOME_X_TICK_COUNT];
extern lv_obj_t *s_ui_home_chart_legend_row[UI_PAGE_HOME_LEGEND_ROWS];
extern lv_obj_t *s_ui_home_chart_legend_swatch[UI_PAGE_HOME_LEGEND_ROWS];
extern lv_obj_t *s_ui_home_chart_legend_label[UI_PAGE_HOME_LEGEND_ROWS];
extern lv_obj_t *s_ui_home_chart_now_dot;
extern lv_obj_t *s_ui_home_progress_wrap;
extern lv_obj_t *s_ui_home_progress_track;
extern lv_obj_t *s_ui_home_progress_fill;
extern lv_obj_t *s_ui_home_progress_label;
extern lv_obj_t *s_ui_home_fire_btn;
extern lv_obj_t *s_ui_home_fire_btn_label;
extern lv_obj_t *s_ui_home_pause_btn;
extern lv_obj_t *s_ui_home_pause_btn_label;

/* Owner request 2026-09-28 ("make a simple screen for the lcd that also
 * allows modifying the current firing like the web does") -- the LCD
 * equivalent of the web's "Edit firing" button (d484e51a). Same visibility
 * rule as Pause/Resume (RUNNING/PAUSED only), built hidden. */
extern lv_obj_t *s_ui_home_edit_btn;
extern lv_obj_t *s_ui_home_edit_btn_label;
extern ui_topbar_t s_ui_home_topbar;
extern lv_obj_t *s_ui_home_status_label;

/* UI_PLAN.md 6.1 -- selected-profile name left of Start, tap opens the
 * profile picker page ("profile_picker", pick mode). Grows to absorb
 * action_row's leftover width; Start/Pause are pinned to their existing
 * ~96px drawn size (see ui_home_build_button()'s grow parameter). */
extern lv_obj_t *s_ui_home_profile_btn;
extern lv_obj_t *s_ui_home_profile_label;

/* UI_PLAN.md 6.5 -- right-quarter rail widgets, built once in
 * ui_page_home_build(), filled every tick by ui_home_rail_refresh() below. */
extern lv_obj_t *s_ui_home_rail;
extern lv_obj_t *s_ui_home_rail_relay_pill[KILN_IO_RELAY_COUNT];
extern lv_obj_t *s_ui_home_rail_zone_row[MAX31856_CHANNEL_COUNT];
extern lv_obj_t *s_ui_home_rail_zone_name[MAX31856_CHANNEL_COUNT];
extern lv_obj_t *s_ui_home_rail_zone_temp[MAX31856_CHANNEL_COUNT];
extern lv_obj_t *s_ui_home_rail_zone_bar[MAX31856_CHANNEL_COUNT];
extern lv_obj_t *s_ui_home_rail_watts_label;

/* UI_PLAN.md 6.1 -- see ui_page_home_refresh.c for the full doc comment. */
void ui_home_profile_label_refresh(const profile_exec_status_t *st);

/* UI_PLAN.md 6.1 -- both defined in ui_page_home_actions.c. The resolver is
 * THE definition of "which profile would Start run"; the dashboard label and
 * the Start button must both go through it or they drift apart. */
bool ui_home_resolve_profile_id(const profile_exec_status_t *st, uint8_t *out_id);
bool ui_home_profile_name_for_id(uint8_t id, char *out, size_t out_cap);
void ui_home_profile_picked_cb(uint8_t profile_id);

/* ---- helpers, defined in ui_page_home.c or ui_page_home_actions.c, used
 * from another file in the split -------------------------------------- */
void ui_home_format_duration(uint32_t seconds, char *out, size_t out_cap);
float ui_home_freezing_point_disp(unit_pref_t unit);

/* ---- actions.c: Start/Stop/Pause handling, confirm dialogs, topbar menu
 * nav, and the small button-builder used by ui_page_home_build() -------- */
void ui_home_menu_nav_cb(lv_event_t *e);
lv_obj_t *ui_home_build_button(lv_obj_t *parent, const char *text, lv_color_t bg, lv_event_cb_t cb,
                                int32_t height, lv_obj_t **out_label);
void ui_home_fire_btn_cb(lv_event_t *e);
void ui_home_profile_btn_cb(lv_event_t *e);
void ui_home_pause_resume_btn_cb(lv_event_t *e);
void ui_home_edit_btn_cb(lv_event_t *e);

/* Cached "a live-edit decision is owed" read (ui_page_home_refresh.c). */
bool ui_home_live_edit_decision_owed(void);

/* WEB_AUTH_PLAN.md item 10 -- one shared corner-tap handler for all four
 * hit zones, discriminated by the corner baked into the event's user_data
 * at lv_obj_add_event_cb() time (auth_reset_gesture_corner_t cast through
 * (void*)(intptr_t)). See ui_page_home.c's build() for where the four
 * zones are created and ui_page_home_refresh.c for the armed-banner /
 * cancel-or-expiry logging that reads auth_reset_gesture_singleton() on
 * the existing 1 Hz tick. */
void ui_home_auth_reset_corner_tap_cb(lv_event_t *e);

/* ---- chart.c: planned-curve lookup, draw-event hooks, tick/legend layout,
 * used from ui_page_home_build() and ui_page_home_refresh.c ------------- */
float ui_home_plan_lookup(const profile_plan_point_t *pts, size_t n, float t);
void ui_home_chart_draw_event_cb(lv_event_t *e);
void ui_home_chart_y_tick_draw_event_cb(lv_event_t *e);
void ui_home_chart_set_y_ticks(int32_t axis_lo, int32_t axis_hi, unit_pref_t unit);
void ui_home_chart_hide_y_ticks(void);
void ui_home_chart_set_x_ticks(float horizon_s, bool has_span);
void ui_home_chart_set_legend(bool has_span, bool has_actual_multi, bool has_planned);

/* ---- refresh.c: the 1 Hz timer callback, called directly once from
 * ui_page_home_build() to paint real numbers before the first tick ------ */
void ui_home_refresh_cb(lv_timer_t *timer);

/* UI_PLAN.md 6.5 -- fills the right-quarter rail from the SAME ds/st
 * snapshot ui_home_refresh_cb() already holds (no new producer call, no
 * new lock). Kept out-of-line from ui_home_refresh_cb()'s own frame on
 * purpose -- see its definition's header comment for the stack-budget
 * reasoning (lvgl task, check_all_task_stack_budgets.py). */
void ui_home_rail_refresh(const dashboard_status_t *ds, const profile_exec_status_t *st);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_HOME_INTERNAL_H
