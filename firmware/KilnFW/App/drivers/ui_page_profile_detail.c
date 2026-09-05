#include "ui_page_profile_detail.h"

#include <math.h>
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
#include "ui_page_home_graph.h"
#include "ui_page_profile_builder_zones.h"
#include "ui_page_profile_segments.h"
#include "ui_theme.h"
#include "ui_topbar.h"
#include "unit_pref.h"
#include "zones_http.h"

static const char *TAG = "ui_page_profile_detail";

/* Arithmetic (same style as every other page in this pass), against the real
 * ~267px content budget (UI_THEME_PAGE_CONTENT_BUDGET_PX, computed to 268 --
 * see ui_theme.h's own derivation comment): the top bar (title + back/home
 * icons) is ui_topbar.c's own object, already outside this budget, so
 * scr's flex children here are only the three listed below, joined by
 * scr's own UI_THEME_PADDING_PX/2 (4px) pad_gap:
 *
 *     info card (name/family/segments, ~64px) ... ~64px
 *     gap ......................................... 4px
 *     planned-curve preview chart: lv_obj_set_flex_
 *       grow(s_plan_chart, 1) -- absorbs whatever is
 *       left after the fixed-height siblings below,
 *       rather than a hardcoded height that rots when
 *       the info card or action row change ........ (flex-grow, absorbs the remainder)
 *     gap ......................................... 4px
 *     action row: Segments + Edit + Start, each a
 *       UI_PAGE_PROFILE_DETAIL_BUTTON_HEIGHT_PX
 *       (40px) button, flex_grow(1) horizontally .. 40px
 *                                                 ------
 *                                          ~108px fixed, chart takes the rest
 *                                          108px <= 267px  OK with room to
 *                                          spare for the chart itself
 *
 * Button touch area: shrinking the DRAWN button from the old
 * UI_THEME_MIN_TOUCH_TARGET_PX (72px) to 40px still goes through
 * ui_theme_apply_touch_area(btn, false) (see build_action_button() below),
 * the sanctioned way to keep the EFFECTIVE touch square at/above the 72px
 * minimum without growing the visible button -- see that function's own
 * comment in ui_theme.h for why compact_layout=false is right here (this is
 * a sparse 3-button row, not a dense grid). ui_theme.c's non-compact branch
 * always extends by the "generous" margin (UI_THEME_PADDING_PX * 3 = 24px)
 * whenever the button's smaller edge is >= 24px, which 40px clears, so all
 * three buttons get a uniform 24px click-area extension on all four sides
 * via lv_obj_set_ext_click_area() (LVGL 9 has no per-axis extension -- one
 * scalar, all four sides). That extension is symmetric, so it grows
 * SIDEWAYS into the gap between adjacent buttons exactly as much as it
 * grows up/down into empty space -- unlike up/down (nothing else touchable
 * lives above the row (the chart isn't clickable) or below it (screen
 * edge)), sideways is where two buttons' extended boxes can actually
 * collide, and LVGL's hit-test is z-order-first-match, not
 * nearest-center, when that happens (ui_theme.h's own touch-group-
 * arbitration comment names this exact hazard). Non-overlap requires
 * action_row's pad_gap >= 2 * 24 = 48px, so this row's gap is raised from
 * scr's usual UI_THEME_PADDING_PX/2 (4px) to
 * UI_PAGE_PROFILE_DETAIL_ACTION_ROW_GAP_PX (48px) -- see that constant's own
 * comment. At 48px gap the two boxes' extended edges exactly touch (share a
 * boundary) rather than cross it, which is the safe, non-overlapping case.
 * Effective per-button touch box: 40 + 2*24 = 88px tall (comfortably over
 * the 72px minimum) by (visual width + 48px) wide.
 *
 * The nav row's Back button moved into the shared top bar (ui_topbar.c) in
 * the 2026-08-21 icon-topbar pass, freeing the 44px + 4px gap it used to
 * cost here.
 *
 * 2026-08-27: added the preview chart (see build_plan_chart()/refresh_plan_
 * chart() below) so a SELECTED-but-not-running profile can be previewed on
 * the LCD the way main_page.html already lets the web owner preview one --
 * ui_page_home.c:837's IDLE guard on profile_feasibility_plan_curve() stays
 * exactly as written; this page never reads st.segments/st.run_start_c, it
 * reads the profile struct already loaded from flash/NVS by load_current()
 * and (only if this SAME id happens to be the one actually running) the
 * executor's live run_start_c, matching dashboard_http.c's
 * profile_plan_get_handler() precedent exactly.
 *
 * No paging needed -- one profile's summary + a small preview chart still
 * fits a single screen. */

static uint8_t s_profile_id;
/* Back destination is NOT fixed like every other page in this pass -- this
 * page is reached from both ui_page_profiles_mine.c and
 * ui_page_profiles_builtin_list.c, and ui_page_profile_detail_set_id() is how
 * the caller says which one to return to. ui_topbar_cfg_t::back_page is a
 * plain `const char *` handed to nav_cb as event user_data and read again at
 * TAP time, not copied at build time (ui_topbar.h's own comment: "The string
 * is not copied, so pass a literal") -- so a mutable static buffer works just
 * as well as a literal as long as the pointer itself never moves and always
 * holds a valid page name by the time a tap can happen. This buffer is that:
 * ui_topbar_create() is handed its address once, at build time, and
 * ui_page_profile_detail_set_id() rewrites its CONTENTS (never reallocates
 * it) on every navigation here. */
static char s_back_target[32] = "profiles_mine";

static lv_obj_t *s_info_card;
static lv_obj_t *s_name_label;
static lv_obj_t *s_segcount_label;
static lv_obj_t *s_family_label;
static ui_topbar_t s_tb;

/* ---- Planned-curve preview chart --------------------------------------
 * A small, planned-only version of ui_page_home.c's compact chart: no
 * "actual" series (there is nothing running to have recorded actual
 * readings from), no now-dot, no live refresh timer -- refresh() (called
 * once per navigation, from ui_page_profile_detail_set_id()) is all this
 * needs, since the underlying profile only changes via the Edit flow, which
 * always returns here through set_id() again. Same dashed-purple styling
 * and axis-label placement as the home chart, reusing ui_page_home_graph.h's
 * host-tested pure-math helpers -- NOT ui_page_home.c's own static plan_
 * lookup()/chart_draw_event_cb(), which are file-local there; small enough
 * to keep a second copy here rather than promote them to a shared header for
 * two call sites. */
#define UI_PAGE_PROFILE_DETAIL_CHART_POINTS 20
#define UI_PAGE_PROFILE_DETAIL_PLAN_COLOR_HEX 0x9966cc /* matches UI_PAGE_HOME_PLAN_COLOR_HEX / main_page.html */
#define UI_PAGE_PROFILE_DETAIL_PLAN_DASH_WIDTH_PX 6
#define UI_PAGE_PROFILE_DETAIL_PLAN_DASH_GAP_PX   4
/* Idle-preview starting temperature -- matches dashboard_http.c's
 * PROFILE_PLAN_PREVIEW_AMBIENT_C and profile_feasibility.c's
 * FEASIBILITY_AMBIENT_C exactly (20 C is the shared "idle room" answer);
 * kept as its own constant per that file's own comment on why each caller
 * owns its copy rather than sharing one #include. */
#define UI_PAGE_PROFILE_DETAIL_PLAN_PREVIEW_AMBIENT_C 20.0f

/* ---- Action-row button sizing -- see the file header comment's arithmetic
 * block for the full derivation of both of these. */
/* Drawn button height. Smaller than UI_THEME_MIN_TOUCH_TARGET_PX on purpose
 * -- ui_theme_apply_touch_area() below is what keeps the EFFECTIVE touch
 * target at/above that minimum without the button looking oversized. */
#define UI_PAGE_PROFILE_DETAIL_BUTTON_HEIGHT_PX 40
/* Gap between the three action-row buttons. Wider than scr's usual
 * UI_THEME_PADDING_PX/2 specifically so the buttons' extended (post-
 * ui_theme_apply_touch_area()) click areas cannot overlap each other: that
 * helper's non-compact branch extends every side by a fixed "generous"
 * margin (ui_theme.c: UI_THEME_PADDING_PX * 3 = 24px) whenever the button's
 * smaller edge is >= 24px, and LVGL 9's lv_obj_set_ext_click_area() takes
 * one scalar for all four sides -- there is no way to ask for a smaller
 * sideways extension than up/down. Two neighbouring buttons' extended boxes
 * therefore need a gap of at least 2 * 24 = 48px between their VISUAL edges
 * to avoid colliding in the gap (LVGL's hit-test is z-order-first-match on
 * overlap, not nearest-center -- see ui_theme.h's touch-group-arbitration
 * comment). 48px makes the two extended edges exactly meet rather than
 * cross. */
#define UI_PAGE_PROFILE_DETAIL_ACTION_ROW_GAP_PX 48

static lv_obj_t *s_plan_chart;
static lv_chart_series_t *s_plan_series;
static int32_t s_plan_pts[UI_PAGE_PROFILE_DETAIL_CHART_POINTS];
static lv_obj_t *s_plan_peak_label; /* top-left: peak setpoint, in unit_pref */
static lv_obj_t *s_plan_time_label; /* top-right: total planned duration, M:SS */

/* Same dashing trick as ui_page_home.c's chart_draw_event_cb() -- lv_chart
 * has no per-series dash flag, so this intercepts each line draw task and
 * dashes the one matching the planned-series colour. */
static void plan_chart_draw_event_cb(lv_event_t *e)
{
    lv_draw_task_t *draw_task = lv_event_get_draw_task(e);
    lv_draw_line_dsc_t *line_dsc = lv_draw_task_get_line_dsc(draw_task);
    if (line_dsc == NULL) {
        return;
    }
    lv_color_t plan_color = lv_color_hex(UI_PAGE_PROFILE_DETAIL_PLAN_COLOR_HEX);
    if (!lv_color_eq(line_dsc->color, plan_color)) {
        return;
    }
    line_dsc->dash_width = UI_PAGE_PROFILE_DETAIL_PLAN_DASH_WIDTH_PX;
    line_dsc->dash_gap = UI_PAGE_PROFILE_DETAIL_PLAN_DASH_GAP_PX;
}

/* Piecewise-linear sample of a plan point list -- same algorithm as
 * ui_page_home.c's file-local plan_lookup(), duplicated here for the same
 * reason chart_draw_event_cb() is (two small call sites, not worth a shared
 * header). Returns NAN for an empty list. */
static float plan_chart_lookup(const profile_plan_point_t *pts, size_t n, float t)
{
    if (n == 0) {
        return NAN;
    }
    if (t <= pts[0].t) {
        return pts[0].c;
    }
    for (size_t i = 1; i < n; i++) {
        if (t <= pts[i].t) {
            float t0 = pts[i - 1].t, t1 = pts[i].t;
            float c0 = pts[i - 1].c, c1 = pts[i].c;
            if (t1 <= t0) {
                return c1;
            }
            float f = (t - t0) / (t1 - t0);
            return c0 + f * (c1 - c0);
        }
    }
    return pts[n - 1].c;
}

/* Redraws s_plan_chart from `prof`'s segments. Start temperature: if the
 * executor is actually RUNNING/PAUSED/DONE/FAULTED on this SAME profile id,
 * use its real captured run_start_c so the preview matches the live numbers;
 * otherwise (the ordinary idle-preview case) fall back to
 * UI_PAGE_PROFILE_DETAIL_PLAN_PREVIEW_AMBIENT_C -- exactly
 * dashboard_http.c's profile_plan_get_handler() precedent. This performs NO
 * read of profile_executor_get_status()'s st.segments/st.run_start_c unless
 * that guard (state != IDLE && matching id) passes, so ui_page_home.c:837's
 * warning about reading invalid IDLE state is respected: the segments this
 * function plots always come from `prof` (loaded fresh from flash/NVS by the
 * caller), never from stale executor state. */
static void refresh_plan_chart(const profile_t *prof)
{
    if (!s_plan_chart) {
        return; /* not built yet */
    }

    profile_exec_status_t st;
    profile_executor_get_status(&st);
    float start_c = UI_PAGE_PROFILE_DETAIL_PLAN_PREVIEW_AMBIENT_C;
    if (st.state != PROFILE_EXEC_IDLE && st.profile_id == s_profile_id) {
        start_c = st.run_start_c;
    }

    profile_plan_point_t plan_pts[1 + 2 * PROFILE_MAX_SEGMENTS];
    size_t plan_n = 0;
    (void)profile_feasibility_plan_curve(prof->segments, prof->segment_count, start_c, plan_pts,
                                         sizeof(plan_pts) / sizeof(plan_pts[0]), &plan_n);

    if (plan_n == 0) {
        /* No segments (or curve entirely unknown-duration) -- nothing to
         * plot. Blank every point and hide the overlay labels rather than
         * show a stale/empty chart with confident-looking axis text. */
        for (uint32_t i = 0; i < UI_PAGE_PROFILE_DETAIL_CHART_POINTS; i++) {
            s_plan_pts[i] = LV_CHART_POINT_NONE;
        }
        lv_chart_refresh(s_plan_chart);
        lv_obj_add_flag(s_plan_peak_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_plan_time_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    float horizon_s = plan_pts[plan_n - 1].t;
    if (horizon_s < 1.0f) {
        horizon_s = 1.0f; /* guard div-by-zero below */
    }

    unit_pref_t unit = unit_pref_get();
    float peak_c = ui_page_home_plan_peak_c(plan_pts, plan_n);
    float peak_disp = unit_pref_convert(peak_c, unit, UNIT_PREF_KIND_ABSOLUTE);
    float zero_disp = unit_pref_convert(0.0f, unit, UNIT_PREF_KIND_ABSOLUTE);
    int32_t axis_lo = (int32_t)lroundf(zero_disp < peak_disp ? zero_disp : peak_disp - 1.0f);
    int32_t axis_hi = (int32_t)lroundf(peak_disp > zero_disp ? peak_disp : zero_disp + 1.0f);
    lv_chart_set_axis_range(s_plan_chart, LV_CHART_AXIS_PRIMARY_Y, axis_lo, axis_hi);

    for (uint32_t i = 0; i < UI_PAGE_PROFILE_DETAIL_CHART_POINTS; i++) {
        float t_i = (UI_PAGE_PROFILE_DETAIL_CHART_POINTS > 1)
                        ? (float)i * horizon_s / (float)(UI_PAGE_PROFILE_DETAIL_CHART_POINTS - 1)
                        : 0.0f;
        float c = plan_chart_lookup(plan_pts, plan_n, t_i);
        float disp = unit_pref_convert(c, unit, UNIT_PREF_KIND_ABSOLUTE);
        s_plan_pts[i] = isnan(disp) ? LV_CHART_POINT_NONE : (int32_t)lroundf(disp);
    }
    lv_chart_refresh(s_plan_chart);

    char peak_buf[16];
    snprintf(peak_buf, sizeof(peak_buf), "peak %d%s", (int)lroundf(peak_disp), unit_pref_suffix(unit));
    lv_label_set_text(s_plan_peak_label, peak_buf);
    lv_obj_remove_flag(s_plan_peak_label, LV_OBJ_FLAG_HIDDEN);

    char time_buf[16];
    ui_page_home_format_mmss((uint32_t)lroundf(horizon_s), time_buf, sizeof(time_buf));
    lv_label_set_text(s_plan_time_label, time_buf);
    lv_obj_remove_flag(s_plan_time_label, LV_OBJ_FLAG_HIDDEN);
}

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
    if (!s_info_card) {
        return; /* not built yet */
    }

    profile_t prof;
    const builtin_profile_t *b = NULL;
    if (!load_current(&prof, &b)) {
        ui_topbar_set_title(&s_tb, "Profile Detail");
        lv_label_set_text(s_name_label, "(not found)");
        lv_label_set_text(s_segcount_label, "");
        lv_label_set_text(s_family_label, "");
        lv_obj_set_style_border_width(s_info_card, 0, 0);
        if (s_plan_chart) {
            for (uint32_t i = 0; i < UI_PAGE_PROFILE_DETAIL_CHART_POINTS; i++) {
                s_plan_pts[i] = LV_CHART_POINT_NONE;
            }
            lv_chart_refresh(s_plan_chart);
            lv_obj_add_flag(s_plan_peak_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_plan_time_label, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    const char *title = b ? b->title : prof.name;
    ui_topbar_set_title(&s_tb, title);
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

    refresh_plan_chart(&prof);
}

void ui_page_profile_detail_set_id(uint8_t profile_id, const char *back_page)
{
    s_profile_id = profile_id;
    snprintf(s_back_target, sizeof(s_back_target), "%s", back_page ? back_page : "profiles_mine");
    refresh();
}

uint8_t ui_page_profile_detail_get_id(void)
{
    return s_profile_id;
}

static void segments_nav_cb(lv_event_t *e)
{
    (void)e;
    ui_page_profile_segments_prepare();
    kiln_ui_show("profile_segments");
}

static void edit_btn_cb(lv_event_t *e)
{
    (void)e;
    /* Editing a BUILT-IN must mean "save a copy" -- profiles_http_save()
     * treats requested_id >= PROFILES_MAX_COUNT as "first free slot" and
     * ui_page_profile_builder_review.c's slot picker only ever offers the 8
     * real user slots either way, so a builtin id (>= PROFILE_BUILTIN_ID_BASE)
     * structurally cannot be the save target -- see
     * ui_page_profile_builder_zones.h's header comment. */
    ui_page_profile_builder_start_edit(s_profile_id);
    kiln_ui_show("profile_builder_zones");
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
    lv_obj_set_height(btn, UI_PAGE_PROFILE_DETAIL_BUTTON_HEIGHT_PX);
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

    /* back_page points at s_back_target, a mutable static buffer, not a
     * literal -- see that buffer's own comment above for why that's safe. */
    ui_topbar_create(scr, &(ui_topbar_cfg_t){
        .title = "Profile Detail",
        .back_page = s_back_target,
        .show_home = true,
    }, &s_tb);

    s_info_card = lv_obj_create(scr);
    lv_obj_set_width(s_info_card, lv_pct(100));
    lv_obj_set_height(s_info_card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_info_card, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(s_info_card, UI_THEME_CORNER_RADIUS_PX, 0);
    /* Phase 7 theme/style pass (TODO.md 1223-1225): a resting-lift shadow,
     * pure paint (see ui_theme.h's own comment) -- does not change this
     * card's height, so the budget arithmetic below is unaffected. */
    ui_theme_apply_card_shadow(s_info_card, 1);
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

    s_plan_chart = lv_chart_create(scr);
    lv_obj_set_width(s_plan_chart, lv_pct(100));
    /* flex-grow, not a fixed height -- absorbs whatever the info card and
     * action row leave over, so the chart gets larger automatically as
     * those siblings shrink instead of a hardcoded number that has to be
     * hand-updated (and can rot) whenever they change. See the file header
     * comment's arithmetic block. */
    lv_obj_set_flex_grow(s_plan_chart, 1);
    lv_obj_set_style_bg_color(s_plan_chart, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(s_plan_chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_plan_chart, 0, 0);
    lv_obj_set_style_radius(s_plan_chart, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(s_plan_chart, 2, 0);
    lv_chart_set_type(s_plan_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(s_plan_chart, 0, 0); /* see ui_page_home.c's own comment on why 0,0 */
    lv_chart_set_point_count(s_plan_chart, UI_PAGE_PROFILE_DETAIL_CHART_POINTS);
    s_plan_series =
        lv_chart_add_series(s_plan_chart, lv_color_hex(UI_PAGE_PROFILE_DETAIL_PLAN_COLOR_HEX), LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_add_event_cb(s_plan_chart, plan_chart_draw_event_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    for (uint32_t i = 0; i < UI_PAGE_PROFILE_DETAIL_CHART_POINTS; i++) {
        s_plan_pts[i] = LV_CHART_POINT_NONE;
    }
    lv_chart_set_series_ext_y_array(s_plan_chart, s_plan_series, s_plan_pts);
    lv_chart_set_axis_range(s_plan_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_obj_remove_flag(s_plan_chart, LV_OBJ_FLAG_SCROLLABLE);

    s_plan_peak_label = lv_label_create(s_plan_chart);
    lv_obj_set_style_text_color(s_plan_peak_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_bg_color(s_plan_peak_label, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_plan_peak_label, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(s_plan_peak_label, 3, 0);
    lv_obj_set_style_radius(s_plan_peak_label, 4, 0);
    lv_obj_align(s_plan_peak_label, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_obj_add_flag(s_plan_peak_label, LV_OBJ_FLAG_HIDDEN);

    s_plan_time_label = lv_label_create(s_plan_chart);
    lv_obj_set_style_text_color(s_plan_time_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_bg_color(s_plan_time_label, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_plan_time_label, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(s_plan_time_label, 3, 0);
    lv_obj_set_style_radius(s_plan_time_label, 4, 0);
    lv_obj_align(s_plan_time_label, LV_ALIGN_TOP_RIGHT, -2, 2);
    lv_obj_add_flag(s_plan_time_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *action_row = lv_obj_create(scr);
    lv_obj_set_width(action_row, lv_pct(100));
    lv_obj_set_height(action_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(action_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(action_row, 0, 0);
    lv_obj_set_style_pad_all(action_row, 0, 0);
    lv_obj_set_flex_flow(action_row, LV_FLEX_FLOW_ROW);
    /* Wider than scr's usual gap -- required so the buttons' extended touch
     * areas cannot overlap. See UI_PAGE_PROFILE_DETAIL_ACTION_ROW_GAP_PX's
     * own comment. */
    lv_obj_set_style_pad_gap(action_row, UI_PAGE_PROFILE_DETAIL_ACTION_ROW_GAP_PX, 0);
    lv_obj_remove_flag(action_row, LV_OBJ_FLAG_SCROLLABLE);

    build_action_button(action_row, "Segments", UI_THEME_COLOR_CARD, segments_nav_cb);
    build_action_button(action_row, "Edit", UI_THEME_COLOR_CARD, edit_btn_cb);
    build_action_button(action_row, "Start", UI_THEME_ACCENT_4, start_btn_cb);

    ui_topbar_raise(&s_tb);

    refresh();
    return scr;
}
