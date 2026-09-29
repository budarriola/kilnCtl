#include "ui_page_edit_firing.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "kiln_ui.h"
#include "live_profile.h"
#include "profile_executor.h"
#include "profiles_builtin.h"
#include "profiles_http.h"
#include "profiles_http_internal.h" /* profiles_validate_candidate()/PROFILE_VALIDATE_HARD --
                                      * the SAME validator profiles_live_http.c's accept
                                      * handler calls; this page must never re-implement a
                                      * bound/window rule of its own. Cross-directory include
                                      * is the existing convention here (profile_executor.c,
                                      * live_profile.c already do this). */
#include "ui_theme.h"
#include "ui_topbar.h"
#include "unit_pref.h"

/* Layout, against the ~267px no-scroll content budget (same arithmetic
 * discipline as ui_page_profile_builder_segment.c):
 *
 *     topbar (title + Back/Home/Prev/Next) .......... owned by ui_topbar.c
 *     note line ("Segment N of M -- running" etc) ...  18px
 *     three stepper rows, 46px each ................. 138px
 *     gap x4 ..........................................16px
 *     status/refusal line ............................ 18px
 *     Apply button row ............................... 44px
 *                                                      ------
 *                                                       234px <= 267px  OK
 *
 * One row per field (Target / Ramp / Dwell), each "- value +", rather than
 * builder_segment.c's tap-to-numpad cards: the owner's own wording for this
 * task was "+/- buttons," and a stepper is also the more forgiving control
 * for nudging a value on an ALREADY RUNNING firing (no chance of a fat-
 * fingered numpad entry putting a live zone target far out of range before
 * the validator ever sees it -- the step size itself is the guard rail). */
#define ROW_HEIGHT_PX 46
#define STEP_BTN_W_PX 44

/* Sensible, unit-aware step sizes. Kept in Celsius/native units end to end
 * (never round-tripped through the display unit), same discipline and same
 * rationale as ui_page_profile_builder_segment.c's target/ramp cards: a
 * kiln setpoint that silently changed units would be a real hazard. Display
 * conversion (unit_pref_convert()) is applied ONLY to the rendered label. */
#define TARGET_STEP_C   5.0f
#define RAMP_STEP_C_HR  5.0f
#define DWELL_STEP_MIN  5u

/* note(18) + 3*row(46=138) + status(18) + bottom_row(44) + hint(18) = 236,
 * plus scr's pad_gap (UI_THEME_PADDING_PX/2 = 4px) between each of the 6
 * children = 24 -> 260px, against the 268px budget. */
#define UI_PAGE_EDIT_FIRING_WORST_CASE_HEIGHT_PX (18 + 3 * ROW_HEIGHT_PX + 18 + 44 + 18 + 6 * (UI_THEME_PADDING_PX / 2))
_Static_assert(UI_PAGE_EDIT_FIRING_WORST_CASE_HEIGHT_PX <= UI_THEME_PAGE_CONTENT_BUDGET_PX,
               "ui_page_edit_firing.c: content exceeds UI_THEME_PAGE_CONTENT_BUDGET_PX -- split across "
               "more pages, don't scroll.");

static profile_t *s_working;        /* heap, allocated once -- see build() */
static bool s_have_working;         /* s_working holds a loaded profile */
static bool s_active;               /* firing RUNNING/PAUSED/FAULTED right now */
static uint8_t s_origin_id;
static bool s_origin_is_builtin;
static uint8_t s_running_segment_index;
static uint8_t s_cur_seg;           /* segment currently selected for editing */

static lv_obj_t *s_note_label;
static lv_obj_t *s_target_val_label;
static lv_obj_t *s_ramp_val_label;
static lv_obj_t *s_dwell_val_label;
static lv_obj_t *s_target_minus, *s_target_plus;
static lv_obj_t *s_ramp_minus, *s_ramp_plus;
static lv_obj_t *s_dwell_minus, *s_dwell_plus;
static lv_obj_t *s_status_label;
static lv_obj_t *s_apply_btn;
static ui_topbar_t s_tb;

/* True iff s_cur_seg's target_c/ramp_c_per_hr may be changed at all.
 * live_edit_check_window() is the sole AUTHORITATIVE rule (enforced again at
 * Apply time, server-identical) -- this is only a local, best-effort mirror
 * of that function's real behavior (confirmed against live_profile.c: segments
 * strictly before the running one must stay byte-identical; the running
 * segment itself may have target_c/ramp_c_per_hr/dwell_min changed -- only
 * its seg_kind/io_* fields are frozen; segments after are unconstrained), so
 * the UI does not invite an edit Apply will certainly refuse. */
static bool target_ramp_editable(void)
{
    return s_active && s_cur_seg >= s_running_segment_index;
}

static bool dwell_editable(void)
{
    return s_active && s_cur_seg >= s_running_segment_index;
}

static void set_stepper_enabled(lv_obj_t *btn, bool enabled)
{
    if (!btn) return;
    if (enabled) {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    } else {
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(btn, LV_OPA_50, 0);
    }
}

static void refresh(void)
{
    if (!s_target_val_label) {
        return; /* not built yet -- prepare() runs before the first build(),
                  * same guard idiom as every other page in this tree. */
    }

    if (!s_active || !s_have_working) {
        lv_label_set_text(s_note_label, "No firing is running.");
        lv_label_set_text(s_target_val_label, "--");
        lv_label_set_text(s_ramp_val_label, "--");
        lv_label_set_text(s_dwell_val_label, "--");
        lv_label_set_text(s_status_label, "");
        set_stepper_enabled(s_target_minus, false);
        set_stepper_enabled(s_target_plus, false);
        set_stepper_enabled(s_ramp_minus, false);
        set_stepper_enabled(s_ramp_plus, false);
        set_stepper_enabled(s_dwell_minus, false);
        set_stepper_enabled(s_dwell_plus, false);
        lv_obj_add_flag(s_apply_btn, LV_OBJ_FLAG_HIDDEN);
        ui_topbar_set_title(&s_tb, "Edit Firing");
        ui_topbar_set_prev_enabled(&s_tb, false);
        ui_topbar_set_next_enabled(&s_tb, false);
        return;
    }
    lv_obj_remove_flag(s_apply_btn, LV_OBJ_FLAG_HIDDEN);

    if (s_cur_seg >= s_working->segment_count) {
        s_cur_seg = (uint8_t)(s_working->segment_count > 0 ? s_working->segment_count - 1 : 0);
    }
    profile_segment_t *seg = &s_working->segments[s_cur_seg];

    char title_buf[32];
    snprintf(title_buf, sizeof(title_buf), "Segment %u of %u", (unsigned)(s_cur_seg + 1),
             (unsigned)s_working->segment_count);
    ui_topbar_set_title(&s_tb, title_buf);
    ui_topbar_set_prev_enabled(&s_tb, s_cur_seg > 0);
    ui_topbar_set_next_enabled(&s_tb, (uint32_t)(s_cur_seg + 1) < s_working->segment_count);

    if (s_cur_seg < s_running_segment_index) {
        lv_label_set_text(s_note_label, "Already finished -- not editable.");
    } else if (s_cur_seg == s_running_segment_index) {
        lv_label_set_text(s_note_label, "Running now -- editable.");
    } else {
        lv_label_set_text(s_note_label, "Upcoming -- fully editable.");
    }

    unit_pref_t pref = unit_pref_get();
    char buf[24];
    snprintf(buf, sizeof(buf), "%.0f %s", (double)unit_pref_convert(seg->target_c, pref, UNIT_PREF_KIND_ABSOLUTE),
             unit_pref_suffix(pref));
    lv_label_set_text(s_target_val_label, buf);
    snprintf(buf, sizeof(buf), "%.0f %s/hr", (double)unit_pref_convert(seg->ramp_c_per_hr, pref, UNIT_PREF_KIND_RATE),
             unit_pref_suffix(pref));
    lv_label_set_text(s_ramp_val_label, buf);
    snprintf(buf, sizeof(buf), "%u min", (unsigned)seg->dwell_min);
    lv_label_set_text(s_dwell_val_label, buf);

    bool tr_editable = target_ramp_editable();
    set_stepper_enabled(s_target_minus, tr_editable);
    set_stepper_enabled(s_target_plus, tr_editable);
    set_stepper_enabled(s_ramp_minus, tr_editable);
    set_stepper_enabled(s_ramp_plus, tr_editable);
    bool dw_editable = dwell_editable();
    set_stepper_enabled(s_dwell_minus, dw_editable);
    set_stepper_enabled(s_dwell_plus, dw_editable);
}

void ui_page_edit_firing_prepare(void)
{
    s_cur_seg = 0;
    s_have_working = false;

    profile_executor_live_status_t st;
    profile_executor_get_live_status(&st);
    s_active = st.active;
    if (!s_active) {
        refresh();
        return;
    }
    s_origin_id = st.profile_id;
    s_running_segment_index = st.segment_index;
    s_origin_is_builtin = (st.profile_id >= PROFILES_MAX_COUNT);

    if (!s_working) {
        /* Allocated once, kept for the page's lifetime (pages are never torn
         * down, kiln_ui.h's header comment) -- heap rather than a static
         * profile_t so this page's slice of .dram0.bss is one pointer, not
         * sizeof(profile_t), per this task's budget instruction. */
        s_working = heap_caps_malloc(sizeof(profile_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_working) {
        s_active = false; /* out of memory -- degrade to "no firing" view */
        refresh();
        return;
    }

    bool loaded = false;
    if (live_profile_has_pending_for_origin(s_origin_id)) {
        loaded = (live_profile_load_working_for_origin(s_origin_id, s_working) == LIVE_PROFILE_LOAD_OK);
    }
    if (!loaded) {
        loaded = s_origin_is_builtin ? profiles_builtin_get(s_origin_id, s_working)
                                      : profiles_http_get(s_origin_id, s_working);
    }
    s_have_working = loaded;
    s_cur_seg = s_running_segment_index;
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
    if (s_have_working && (uint32_t)(s_cur_seg + 1) < s_working->segment_count) {
        s_cur_seg++;
        refresh();
    }
}

static void target_minus_cb(lv_event_t *e)
{
    (void)e;
    if (!s_have_working || !target_ramp_editable()) return;
    s_working->segments[s_cur_seg].target_c -= TARGET_STEP_C;
    lv_label_set_text(s_status_label, "");
    refresh();
}

static void target_plus_cb(lv_event_t *e)
{
    (void)e;
    if (!s_have_working || !target_ramp_editable()) return;
    s_working->segments[s_cur_seg].target_c += TARGET_STEP_C;
    lv_label_set_text(s_status_label, "");
    refresh();
}

static void ramp_minus_cb(lv_event_t *e)
{
    (void)e;
    if (!s_have_working || !target_ramp_editable()) return;
    float v = s_working->segments[s_cur_seg].ramp_c_per_hr - RAMP_STEP_C_HR;
    s_working->segments[s_cur_seg].ramp_c_per_hr = (v < 0.0f) ? 0.0f : v;
    lv_label_set_text(s_status_label, "");
    refresh();
}

static void ramp_plus_cb(lv_event_t *e)
{
    (void)e;
    if (!s_have_working || !target_ramp_editable()) return;
    s_working->segments[s_cur_seg].ramp_c_per_hr += RAMP_STEP_C_HR;
    lv_label_set_text(s_status_label, "");
    refresh();
}

static void dwell_minus_cb(lv_event_t *e)
{
    (void)e;
    if (!s_have_working || !dwell_editable()) return;
    uint32_t v = s_working->segments[s_cur_seg].dwell_min;
    s_working->segments[s_cur_seg].dwell_min = (v > DWELL_STEP_MIN) ? (v - DWELL_STEP_MIN) : 0;
    lv_label_set_text(s_status_label, "");
    refresh();
}

static void dwell_plus_cb(lv_event_t *e)
{
    (void)e;
    if (!s_have_working || !dwell_editable()) return;
    s_working->segments[s_cur_seg].dwell_min += DWELL_STEP_MIN;
    lv_label_set_text(s_status_label, "");
    refresh();
}

/* Same three-call sequence api_profile_live_post_handler() in
 * profiles_live_http.c makes (fork-if-needed -> validate HARD -> window
 * check -> save), reusing the SAME functions, never a copy: this page has
 * no C API of its own for any of these rules. Runs on the LVGL task; the
 * LVGL task's own stack is a static internal-SRAM array (s_lvgl_task_stack,
 * lvgl_port.c), not PSRAM, so writing through to NVS from here is the same
 * established, precedented pattern ui_page_profile_builder_review.c's
 * do_save() already uses -- see that file's header comment. */
static void apply_cb(lv_event_t *e)
{
    (void)e;
    if (!s_have_working) return;

    profile_executor_live_status_t st;
    profile_executor_get_live_status(&st);
    if (!st.active) {
        lv_label_set_text(s_status_label, "Refused: no active firing");
        s_active = false;
        refresh();
        return;
    }
    bool origin_is_builtin = (st.profile_id >= PROFILES_MAX_COUNT);
    char err[128] = {0};

    if (!live_profile_has_pending_for_origin(st.profile_id)) {
        profile_t *origin = heap_caps_malloc(sizeof(profile_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!origin) {
            lv_label_set_text(s_status_label, "Refused: out of memory");
            return;
        }
        bool have_origin = origin_is_builtin ? profiles_builtin_get(st.profile_id, origin)
                                              : profiles_http_get(st.profile_id, origin);
        if (!have_origin) {
            heap_caps_free(origin);
            lv_label_set_text(s_status_label, "Refused: origin profile not readable");
            return;
        }
        char origin_name[PROFILE_NAME_MAX_LEN + 1];
        snprintf(origin_name, sizeof(origin_name), "%s", origin->name);
        profile_t *fork_out = heap_caps_malloc(sizeof(profile_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        live_edit_record_t rec;
        bool forked = fork_out && live_profile_fork(st.profile_id, origin_is_builtin, origin_name, origin, fork_out,
                                                     &rec, err, sizeof(err));
        heap_caps_free(origin);
        if (fork_out) heap_caps_free(fork_out);
        if (!forked) {
            lv_label_set_text(s_status_label, err[0] ? err : "Refused: fork failed");
            return;
        }
    }

    char warn_json[16] = {0}; /* discarded -- this page shows no warnings list, only refusals */
    if (!profiles_validate_candidate(s_working, PROFILE_VALIDATE_HARD, warn_json, sizeof(warn_json), err,
                                      sizeof(err))) {
        lv_label_set_text(s_status_label, err[0] ? err : "Refused: invalid");
        return;
    }

    profile_t *running = heap_caps_malloc(sizeof(profile_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (running) {
        bool have_running =
            origin_is_builtin ? profiles_builtin_get(st.profile_id, running) : profiles_http_get(st.profile_id, running);
        if (have_running && live_edit_check_window(running, s_working, st.segment_index, err, sizeof(err))) {
            heap_caps_free(running);
            lv_label_set_text(s_status_label, err[0] ? err : "Refused: window violation");
            return;
        }
        heap_caps_free(running);
    }

    if (!live_profile_save_working(s_working, err, sizeof(err))) {
        lv_label_set_text(s_status_label, err[0] ? err : "Refused: save failed");
        return;
    }

    lv_label_set_text(s_status_label, "Applied.");
}

static lv_obj_t *build_step_row(lv_obj_t *parent, const char *caption, lv_obj_t **out_val_label,
                                 lv_obj_t **out_minus, lv_obj_t **out_plus, lv_event_cb_t minus_cb,
                                 lv_event_cb_t plus_cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, ROW_HEIGHT_PX);
    lv_obj_set_style_bg_color(row, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(row, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_hor(row, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_set_style_pad_ver(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap = lv_label_create(row);
    lv_obj_set_style_text_color(cap, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(cap, caption);

    lv_obj_t *minus = lv_button_create(row);
    lv_obj_set_size(minus, STEP_BTN_W_PX, ROW_HEIGHT_PX - 8);
    lv_obj_set_style_bg_color(minus, UI_THEME_ACCENT_1, 0);
    lv_obj_set_style_radius(minus, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(minus, minus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *minus_lbl = lv_label_create(minus);
    lv_label_set_text(minus_lbl, LV_SYMBOL_MINUS);
    lv_obj_center(minus_lbl);
    ui_theme_apply_touch_area(minus, false);
    *out_minus = minus;

    lv_obj_t *val = lv_label_create(row);
    lv_obj_set_style_text_color(val, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(val, "--");
    lv_obj_set_flex_grow(val, 1);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_CENTER, 0);
    *out_val_label = val;

    lv_obj_t *plus = lv_button_create(row);
    lv_obj_set_size(plus, STEP_BTN_W_PX, ROW_HEIGHT_PX - 8);
    lv_obj_set_style_bg_color(plus, UI_THEME_ACCENT_1, 0);
    lv_obj_set_style_radius(plus, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(plus, plus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *plus_lbl = lv_label_create(plus);
    lv_label_set_text(plus_lbl, LV_SYMBOL_PLUS);
    lv_obj_center(plus_lbl);
    ui_theme_apply_touch_area(plus, false);
    *out_plus = plus;

    return row;
}

lv_obj_t *ui_page_edit_firing_build(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    ui_topbar_create(scr, &(ui_topbar_cfg_t){
        .title = "Edit Firing",
        .back_page = "home",
        .show_home = true,
        .prev_cb = prev_cb,
        .next_cb = next_cb,
    }, &s_tb);

    s_note_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_note_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_note_label, "");

    build_step_row(scr, "Target", &s_target_val_label, &s_target_minus, &s_target_plus, target_minus_cb,
                   target_plus_cb);
    build_step_row(scr, "Ramp", &s_ramp_val_label, &s_ramp_minus, &s_ramp_plus, ramp_minus_cb, ramp_plus_cb);
    build_step_row(scr, "Dwell", &s_dwell_val_label, &s_dwell_minus, &s_dwell_plus, dwell_minus_cb, dwell_plus_cb);

    s_status_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_status_label, UI_THEME_ACCENT_5, 0);
    lv_label_set_text(s_status_label, "");
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_status_label, lv_pct(100));

    lv_obj_t *bottom_row = lv_obj_create(scr);
    lv_obj_set_width(bottom_row, lv_pct(100));
    lv_obj_set_height(bottom_row, 44);
    lv_obj_set_style_bg_opa(bottom_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bottom_row, 0, 0);
    lv_obj_set_style_pad_all(bottom_row, 0, 0);
    lv_obj_set_flex_flow(bottom_row, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(bottom_row, LV_OBJ_FLAG_SCROLLABLE);

    s_apply_btn = lv_button_create(bottom_row);
    lv_obj_set_flex_grow(s_apply_btn, 1);
    lv_obj_set_height(s_apply_btn, 44);
    lv_obj_set_style_bg_color(s_apply_btn, UI_THEME_ACCENT_4, 0);
    lv_obj_set_style_radius(s_apply_btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(s_apply_btn, apply_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *apply_lbl = lv_label_create(s_apply_btn);
    lv_label_set_text(apply_lbl, "Apply");
    lv_obj_center(apply_lbl);
    ui_theme_apply_touch_area(s_apply_btn, false);

    /* Owner: "leave the working copy's end-of-run decision to the web" --
     * one short line only, and only if it fits (it does: this row is empty
     * apart from the caption below, well under the 267px budget). */
    lv_obj_t *hint = lv_label_create(scr);
    lv_obj_set_style_text_color(hint, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(hint, "Save/discard this edit on the web when the firing ends.");

    ui_topbar_raise(&s_tb);

    refresh();
    return scr;
}
