#include "ui_page_edit_firing.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "kiln_ui.h"
#include "ui_edit_firing_apply.h" /* every rule and the whole Apply sequence -- this
                                   * file owns only widgets, paging and text */
#include "ui_theme.h"
#include "ui_topbar.h"
#include "unit_pref.h"

/* Layout, against the ~267px no-scroll content budget (same arithmetic
 * discipline as ui_page_profile_builder_segment.c):
 *
 *     topbar (title + Back/Home/Prev/Next) .......... owned by ui_topbar.c
 *     note line ("Running now -- editable" etc) ..... 18px
 *     three stepper rows, 46px each ................. 138px
 *     status/refusal line ............................ 18px
 *     Apply button row ............................... 44px
 *     hint line ...................................... 18px
 *     gap x6 ......................................... 24px
 *                                                      ------
 *                                                       260px
 *
 * One row per field (Target / Ramp / Dwell), each "- value +", rather than
 * builder_segment.c's tap-to-numpad cards: the owner's own wording for this
 * task was "+/- buttons," and a stepper is also the more forgiving control
 * for nudging a value on an ALREADY RUNNING firing -- the step size itself
 * is a guard rail, and edit_firing_step() clamps to the same bounds the
 * web's form parser enforces. */
#define ROW_HEIGHT_PX 46
#define STEP_BTN_W_PX 44
#define REFRESH_MS 1000

#define UI_PAGE_EDIT_FIRING_WORST_CASE_HEIGHT_PX (18 + 3 * ROW_HEIGHT_PX + 18 + 44 + 18 + 6 * (UI_THEME_PADDING_PX / 2))
_Static_assert(UI_PAGE_EDIT_FIRING_WORST_CASE_HEIGHT_PX <= UI_THEME_PAGE_CONTENT_BUDGET_PX,
               "ui_page_edit_firing.c: content exceeds UI_THEME_PAGE_CONTENT_BUDGET_PX -- split across "
               "more pages, don't scroll.");

/* All page state lives in ONE heap block (allocated on first prepare()/build(),
 * kept for the page's lifetime since pages are never torn down) so this page
 * costs .dram0.bss one pointer rather than ~112 B of widget pointers and a
 * ui_topbar_t. The profile copy being edited (`working`) is a SEPARATE
 * allocation that is freed every time the page is left (LV_EVENT_SCREEN_
 * UNLOADED -- which also covers the relock-to-home path, since kiln_ui.c's
 * handle_lcd_relock_to_home() leaves via kiln_ui_show("home")). */
typedef struct {
    profile_t *working;       /* heap; NULL while the page is not open */
    bool active;              /* a firing is running and `working` belongs to it */
    edit_firing_ctx_t ctx;
    uint32_t applied_generation; /* last successful Apply's generation, 0 = none */
    uint8_t cur_seg;
    lv_timer_t *timer;        /* exists only while the page is the active screen */

    lv_obj_t *note_label;
    lv_obj_t *target_val_label, *ramp_val_label, *dwell_val_label;
    lv_obj_t *target_minus, *target_plus;
    lv_obj_t *ramp_minus, *ramp_plus;
    lv_obj_t *dwell_minus, *dwell_plus;
    lv_obj_t *status_label;
    lv_obj_t *apply_btn;
    ui_topbar_t tb;
} edit_firing_page_t;

static edit_firing_page_t *s_pg;

static bool ensure_pg(void)
{
    if (s_pg) {
        return true;
    }
    s_pg = heap_caps_calloc(1, sizeof(*s_pg), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pg) {
        s_pg = heap_caps_calloc(1, sizeof(*s_pg), MALLOC_CAP_8BIT);
    }
    return s_pg != NULL;
}

static void release_working(void)
{
    if (!s_pg) {
        return;
    }
    if (s_pg->working) {
        heap_caps_free(s_pg->working);
        s_pg->working = NULL;
    }
    s_pg->active = false;
    s_pg->applied_generation = 0;
}

static void set_status(const char *text)
{
    if (s_pg && s_pg->status_label) {
        lv_label_set_text(s_pg->status_label, text);
    }
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

static void set_all_steppers(bool enabled)
{
    set_stepper_enabled(s_pg->target_minus, enabled);
    set_stepper_enabled(s_pg->target_plus, enabled);
    set_stepper_enabled(s_pg->ramp_minus, enabled);
    set_stepper_enabled(s_pg->ramp_plus, enabled);
    set_stepper_enabled(s_pg->dwell_minus, enabled);
    set_stepper_enabled(s_pg->dwell_plus, enabled);
}

static void refresh(void)
{
    if (!s_pg || !s_pg->target_val_label) {
        return; /* not built yet -- prepare() runs before the first build(),
                  * same guard idiom as every other page in this tree. */
    }

    if (!s_pg->active || !s_pg->working) {
        lv_label_set_text(s_pg->note_label, "No firing is running.");
        lv_label_set_text(s_pg->target_val_label, "--");
        lv_label_set_text(s_pg->ramp_val_label, "--");
        lv_label_set_text(s_pg->dwell_val_label, "--");
        set_all_steppers(false);
        lv_obj_add_flag(s_pg->apply_btn, LV_OBJ_FLAG_HIDDEN);
        ui_topbar_set_title(&s_pg->tb, "Edit Firing");
        ui_topbar_set_prev_enabled(&s_pg->tb, false);
        ui_topbar_set_next_enabled(&s_pg->tb, false);
        return;
    }
    lv_obj_remove_flag(s_pg->apply_btn, LV_OBJ_FLAG_HIDDEN);

    const profile_t *p = s_pg->working;
    if (s_pg->cur_seg >= p->segment_count) {
        s_pg->cur_seg = (uint8_t)(p->segment_count > 0 ? p->segment_count - 1 : 0);
    }
    const profile_segment_t *seg = &p->segments[s_pg->cur_seg];
    uint8_t running = s_pg->ctx.running_seg;

    char title_buf[32];
    snprintf(title_buf, sizeof(title_buf), "Segment %u of %u", (unsigned)(s_pg->cur_seg + 1),
             (unsigned)p->segment_count);
    ui_topbar_set_title(&s_pg->tb, title_buf);
    ui_topbar_set_prev_enabled(&s_pg->tb, s_pg->cur_seg > 0);
    ui_topbar_set_next_enabled(&s_pg->tb, (uint32_t)(s_pg->cur_seg + 1) < p->segment_count);

    bool is_zone_ramp = (seg->seg_kind == PROFILE_SEG_KIND_ZONE_RAMP);
    switch (edit_firing_seg_phase(s_pg->cur_seg, running)) {
    case EDIT_FIRING_SEG_FINISHED:
        lv_label_set_text(s_pg->note_label, "Already run -- locked.");
        break;
    case EDIT_FIRING_SEG_RUNNING:
        lv_label_set_text(s_pg->note_label,
                          is_zone_ramp ? "Running now -- editable." : "Relay/IO step -- edit on the web.");
        break;
    default:
        lv_label_set_text(s_pg->note_label,
                          is_zone_ramp ? "Not started yet -- editable." : "Relay/IO step -- edit on the web.");
        break;
    }

    char buf[24];
    if (is_zone_ramp) {
        unit_pref_t pref = unit_pref_get();
        snprintf(buf, sizeof(buf), "%.0f %s",
                 (double)unit_pref_convert(seg->target_c, pref, UNIT_PREF_KIND_ABSOLUTE), unit_pref_suffix(pref));
        lv_label_set_text(s_pg->target_val_label, buf);
        /* 0 = no ramp-rate constraint (profiles_types.h) -- the web labels the
         * field "Ramp (0=none)"; show the meaning rather than "0 C/hr". */
        if (seg->ramp_c_per_hr <= 0.0f) {
            lv_label_set_text(s_pg->ramp_val_label, "none");
        } else {
            snprintf(buf, sizeof(buf), "%.0f %s/hr",
                     (double)unit_pref_convert(seg->ramp_c_per_hr, pref, UNIT_PREF_KIND_RATE),
                     unit_pref_suffix(pref));
            lv_label_set_text(s_pg->ramp_val_label, buf);
        }
    } else {
        lv_label_set_text(s_pg->target_val_label, "--");
        lv_label_set_text(s_pg->ramp_val_label, "--");
    }
    snprintf(buf, sizeof(buf), "%u min", (unsigned)seg->dwell_min);
    lv_label_set_text(s_pg->dwell_val_label, buf);

    set_all_steppers(edit_firing_seg_editable(p, s_pg->cur_seg, running));
}

void ui_page_edit_firing_prepare(void)
{
    if (!ensure_pg()) {
        return; /* build() will fail the same way; kiln_ui_show() handles a NULL screen */
    }
    release_working();
    s_pg->cur_seg = 0;
    set_status("");

    s_pg->working = heap_caps_malloc(sizeof(profile_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pg->working) {
        s_pg->working = heap_caps_malloc(sizeof(profile_t), MALLOC_CAP_8BIT);
    }
    if (s_pg->working && edit_firing_load(s_pg->working, &s_pg->ctx)) {
        s_pg->active = true;
        s_pg->cur_seg = s_pg->ctx.running_seg;
    } else {
        if (!s_pg->working) {
            set_status("Out of memory");
        }
        release_working();
    }
    refresh();
}

static void poll_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_pg) {
        return;
    }
    if (!s_pg->active) {
        /* The page was opened (or the previous firing ended) with nothing
         * running -- without this, the page would only ever notice a firing
         * that starts later by being closed and reopened (review follow-up
         * (c)). Reuses ui_page_edit_firing_prepare()'s own load path
         * (edit_firing_load()) rather than duplicating it, and leaves the
         * working allocation in place across a failed attempt so an idle
         * page isn't malloc'ing/freeing every REFRESH_MS tick; unload's
         * release_working() still frees it exactly as before, and the PIN
         * gate this page sits behind is unaffected -- this timer only ever
         * runs once the page is already the loaded screen. */
        if (!s_pg->working) {
            s_pg->working = heap_caps_malloc(sizeof(profile_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s_pg->working) {
                s_pg->working = heap_caps_malloc(sizeof(profile_t), MALLOC_CAP_8BIT);
            }
        }
        if (s_pg->working && edit_firing_load(s_pg->working, &s_pg->ctx)) {
            s_pg->active = true;
            s_pg->cur_seg = s_pg->ctx.running_seg;
            s_pg->applied_generation = 0;
            /* Clear a leftover "Firing ended." / "A different firing is
             * running -- reopen." from the ENDED/OTHER_FIRING branches below:
             * the page has just loaded the new firing itself, so either
             * message would now be stale or flatly wrong. */
            set_status("");
            refresh();
        }
        return;
    }
    edit_firing_poll_t pr;
    edit_firing_poll(&s_pg->ctx, s_pg->applied_generation, &pr);
    switch (pr.state) {
    case EDIT_FIRING_POLL_ENDED:
        release_working();
        set_status("Firing ended.");
        break;
    case EDIT_FIRING_POLL_OTHER_FIRING:
        release_working();
        set_status("A different firing is running -- reopen.");
        break;
    case EDIT_FIRING_POLL_EDITED_ELSEWHERE:
        s_pg->ctx.running_seg = pr.running_seg;
        set_status("Edited elsewhere -- reopen to reload.");
        break;
    default:
        s_pg->ctx.running_seg = pr.running_seg;
        if (pr.refused) {
            char msg[160];
            snprintf(msg, sizeof(msg), "Firing refused edit: %s", pr.refusal_msg);
            set_status(msg);
            s_pg->applied_generation = 0; /* report it once */
        }
        break;
    }
    refresh();
}

static void screen_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (!s_pg) {
        return;
    }
    if (code == LV_EVENT_SCREEN_LOADED) {
        if (!s_pg->working) {
            ui_page_edit_firing_prepare(); /* reached without the home button's prepare() */
        }
        if (!s_pg->timer) {
            s_pg->timer = lv_timer_create(poll_timer_cb, REFRESH_MS, NULL);
        }
    } else if (code == LV_EVENT_SCREEN_UNLOADED) {
        if (s_pg->timer) {
            lv_timer_delete(s_pg->timer);
            s_pg->timer = NULL;
        }
        release_working();
        refresh();
    }
}

static void prev_cb(lv_event_t *e)
{
    (void)e;
    if (s_pg && s_pg->cur_seg > 0) {
        s_pg->cur_seg--;
        refresh();
    }
}

static void next_cb(lv_event_t *e)
{
    (void)e;
    if (s_pg && s_pg->working && (uint32_t)(s_pg->cur_seg + 1) < s_pg->working->segment_count) {
        s_pg->cur_seg++;
        refresh();
    }
}

static void step(edit_firing_field_t field, int dir)
{
    if (!s_pg || !s_pg->active || !s_pg->working) return;
    if (edit_firing_step(s_pg->working, s_pg->cur_seg, s_pg->ctx.running_seg, field, dir)) {
        set_status("");
    }
    refresh();
}

static void target_minus_cb(lv_event_t *e) { (void)e; step(EDIT_FIRING_FIELD_TARGET, -1); }
static void target_plus_cb(lv_event_t *e) { (void)e; step(EDIT_FIRING_FIELD_TARGET, +1); }
static void ramp_minus_cb(lv_event_t *e) { (void)e; step(EDIT_FIRING_FIELD_RAMP, -1); }
static void ramp_plus_cb(lv_event_t *e) { (void)e; step(EDIT_FIRING_FIELD_RAMP, +1); }
static void dwell_minus_cb(lv_event_t *e) { (void)e; step(EDIT_FIRING_FIELD_DWELL, -1); }
static void dwell_plus_cb(lv_event_t *e) { (void)e; step(EDIT_FIRING_FIELD_DWELL, +1); }

/* Runs on the LVGL task. Its stack is a static internal-SRAM array
 * (s_lvgl_task_stack, lvgl_port.c), not PSRAM, so the NVS write inside
 * live_profile_save_working() is legal here -- the same precedent as
 * ui_page_profile_builder_review.c's do_save(); live_profile.c refuses the
 * write itself on an external-RAM stack anyway. */
static void apply_cb(lv_event_t *e)
{
    (void)e;
    if (!s_pg || !s_pg->active || !s_pg->working) return;

    char err[128];
    if (!edit_firing_apply(s_pg->working, &s_pg->ctx, err, sizeof(err))) {
        char msg[144];
        snprintf(msg, sizeof(msg), "Refused: %s", err);
        set_status(msg);
    } else {
        s_pg->applied_generation = s_pg->ctx.generation;
        set_status("Applied.");
    }
    refresh();
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
    if (!ensure_pg()) {
        return NULL;
    }
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, screen_event_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(scr, screen_event_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

    ui_topbar_create(scr, &(ui_topbar_cfg_t){
        .title = "Edit Firing",
        .back_page = "home",
        .show_home = true,
        .prev_cb = prev_cb,
        .next_cb = next_cb,
    }, &s_pg->tb);

    s_pg->note_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_pg->note_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_pg->note_label, "");

    build_step_row(scr, "Target", &s_pg->target_val_label, &s_pg->target_minus, &s_pg->target_plus,
                   target_minus_cb, target_plus_cb);
    build_step_row(scr, "Ramp", &s_pg->ramp_val_label, &s_pg->ramp_minus, &s_pg->ramp_plus, ramp_minus_cb,
                   ramp_plus_cb);
    build_step_row(scr, "Dwell", &s_pg->dwell_val_label, &s_pg->dwell_minus, &s_pg->dwell_plus, dwell_minus_cb,
                   dwell_plus_cb);

    s_pg->status_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_pg->status_label, UI_THEME_ACCENT_5, 0);
    lv_label_set_text(s_pg->status_label, "");
    lv_label_set_long_mode(s_pg->status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_pg->status_label, lv_pct(100));

    lv_obj_t *bottom_row = lv_obj_create(scr);
    lv_obj_set_width(bottom_row, lv_pct(100));
    lv_obj_set_height(bottom_row, 44);
    lv_obj_set_style_bg_opa(bottom_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bottom_row, 0, 0);
    lv_obj_set_style_pad_all(bottom_row, 0, 0);
    lv_obj_set_flex_flow(bottom_row, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(bottom_row, LV_OBJ_FLAG_SCROLLABLE);

    s_pg->apply_btn = lv_button_create(bottom_row);
    lv_obj_set_flex_grow(s_pg->apply_btn, 1);
    lv_obj_set_height(s_pg->apply_btn, 44);
    lv_obj_set_style_bg_color(s_pg->apply_btn, UI_THEME_ACCENT_4, 0);
    lv_obj_set_style_radius(s_pg->apply_btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(s_pg->apply_btn, apply_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *apply_lbl = lv_label_create(s_pg->apply_btn);
    lv_label_set_text(apply_lbl, "Apply");
    lv_obj_center(apply_lbl);
    ui_theme_apply_touch_area(s_pg->apply_btn, false);

    /* Owner: "leave the working copy's end-of-run decision to the web". */
    lv_obj_t *hint = lv_label_create(scr);
    lv_obj_set_style_text_color(hint, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(hint, "Save/discard this edit on the web when the firing ends.");

    ui_topbar_raise(&s_pg->tb);

    refresh();
    return scr;
}
