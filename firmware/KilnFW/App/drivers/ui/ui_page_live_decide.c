#include "ui_page_live_decide.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "kiln_ui.h"
#include "profiles_live_http.h" /* profiles_live_decide_apply()/_status()/_default_name() -- the ONLY decision logic */
#include "ui_confirm.h"
#include "ui_theme.h"
#include "ui_topbar.h"

/* Layout against the no-scroll content budget:
 *     topbar ........................................ owned by ui_topbar.c
 *     info line (origin / built-in note) ............ 1 text line
 *     three action buttons, 52px each ............... 156px
 *     status line ................................... 1 text line
 *     hint line ..................................... 1 text line
 *     gap x5 ........................................ 5 * (padding / 2)
 *     (a text line is UI_THEME_FONT_LINE_HEIGHT_PX) */
#define BTN_HEIGHT_PX 52
#define UI_PAGE_LIVE_DECIDE_WORST_CASE_HEIGHT_PX (3 * UI_THEME_FONT_LINE_HEIGHT_PX + 3 * BTN_HEIGHT_PX + 5 * (UI_THEME_PADDING_PX / 2))
_Static_assert(UI_PAGE_LIVE_DECIDE_WORST_CASE_HEIGHT_PX <= UI_THEME_PAGE_CONTENT_BUDGET_PX,
               "ui_page_live_decide.c: content exceeds UI_THEME_PAGE_CONTENT_BUDGET_PX -- split across "
               "more pages, do not scroll.");

/* One heap block (PSRAM first) so the page costs .dram0.bss one pointer. */
typedef struct {
    profiles_live_decide_status_t st;
    lv_obj_t *info_label;
    lv_obj_t *discard_btn, *save_btn, *overwrite_btn;
    lv_obj_t *status_label;
    ui_topbar_t tb;
} live_decide_page_t;

static live_decide_page_t *s_pg;

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

static void set_status(const char *text, bool ok)
{
    if (s_pg && s_pg->status_label) {
        lv_obj_set_style_text_color(s_pg->status_label, ok ? UI_THEME_ACCENT_4 : UI_THEME_ACCENT_5, 0);
        lv_label_set_text(s_pg->status_label, text);
    }
}

static void set_btn_enabled(lv_obj_t *btn, bool enabled)
{
    if (!btn) {
        return;
    }
    if (enabled) {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    } else {
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(btn, LV_OPA_40, 0);
    }
}

static void refresh(void)
{
    if (!s_pg || !s_pg->info_label) {
        return;
    }
    char buf[64];
    if (!s_pg->st.record_pending) {
        lv_label_set_text(s_pg->info_label, "Nothing to decide.");
        set_btn_enabled(s_pg->discard_btn, false);
        set_btn_enabled(s_pg->save_btn, false);
        set_btn_enabled(s_pg->overwrite_btn, false);
        return;
    }
    if (s_pg->st.origin_is_builtin) {
        snprintf(buf, sizeof(buf), "Edited %s (built-in: cannot overwrite)", s_pg->st.origin_name);
    } else {
        snprintf(buf, sizeof(buf), "Edited %s", s_pg->st.origin_name);
    }
    lv_label_set_text(s_pg->info_label, buf);
    /* A decision is owed only once the firing is over (pending_decision, the
     * same flag GET /api/profile/live reports); while it is RUNNING/PAUSED/
     * FAULTED the record exists but the buttons stay off. */
    bool can_decide = s_pg->st.pending_decision;
    if (!can_decide) {
        set_status("Decision available when the firing ends.", true);
    }
    set_btn_enabled(s_pg->discard_btn, can_decide);
    set_btn_enabled(s_pg->save_btn, can_decide);
    /* Mirrors the web's 403: disabled, not hidden, so the reason stays visible
     * in the info line. profiles_live_decide_apply() refuses it regardless. */
    set_btn_enabled(s_pg->overwrite_btn, can_decide && !s_pg->st.origin_is_builtin);
}

void ui_page_live_decide_prepare(void)
{
    if (!ensure_pg()) {
        return;
    }
    profiles_live_decide_status(&s_pg->st);
    set_status("", true);
    refresh();
}

/* All three run on the LVGL task. Its stack is a static internal-SRAM array
 * (s_lvgl_task_stack, lvgl_port.c), so the NVS writes inside
 * profiles_live_decide_apply() are legal here -- the same precedent as
 * ui_page_profile_builder_review.c's do_save() and ui_page_edit_firing.c's
 * apply_cb(). No new task is created, so nothing to register for stack
 * margin. */
static void run_decision(live_edit_decision_kind_t kind, const char *name)
{
    char err[96] = {0};
    uint8_t id = 0;
    profiles_live_decide_result_t r =
        profiles_live_decide_apply(kind, name, kind == LIVE_EDIT_DECISION_OVERWRITE, &id, err, sizeof(err));
    char msg[128];
    if (r == LIVE_DECIDE_OK) {
        if (kind == LIVE_EDIT_DECISION_DISCARD) {
            snprintf(msg, sizeof(msg), "Edit discarded.");
        } else if (kind == LIVE_EDIT_DECISION_OVERWRITE) {
            snprintf(msg, sizeof(msg), "Original overwritten.");
        } else {
            snprintf(msg, sizeof(msg), "Saved as %s.", name ? name : "new profile");
        }
    } else {
        snprintf(msg, sizeof(msg), "Refused: %s", err[0] ? err : "failed");
    }
    profiles_live_decide_status(&s_pg->st);
    set_status(msg, r == LIVE_DECIDE_OK);
    refresh();
}

static void discard_confirmed_cb(void *ud)
{
    (void)ud;
    if (s_pg) {
        run_decision(LIVE_EDIT_DECISION_DISCARD, NULL);
    }
}

static void overwrite_confirmed_cb(void *ud)
{
    (void)ud;
    if (s_pg) {
        run_decision(LIVE_EDIT_DECISION_OVERWRITE, NULL);
    }
}

static void discard_cb(lv_event_t *e)
{
    (void)e;
    ui_confirm_show(&(ui_confirm_params_t){
        .title = "Discard edit",
        .body = "Throw away the changes made during this firing?",
        .confirm_label = "Discard",
        .confirm_color = UI_THEME_ACCENT_5,
        .on_confirm = discard_confirmed_cb,
    });
}

static void overwrite_cb(lv_event_t *e)
{
    (void)e;
    if (!s_pg || s_pg->st.origin_is_builtin) {
        return;
    }
    char body[96];
    snprintf(body, sizeof(body), "Replace saved profile %s with the edited version?", s_pg->st.origin_name);
    ui_confirm_show(&(ui_confirm_params_t){
        .title = "Overwrite original",
        .body = body, /* lv_msgbox_add_text copies it */
        .confirm_label = "Overwrite",
        .confirm_color = UI_THEME_ACCENT_5,
        .on_confirm = overwrite_confirmed_cb,
    });
}

/* No text entry on the LCD, so the name is generated and shown in the status
 * line afterwards. Non-destructive (adds a profile), so no confirm dialog. */
static void save_cb(lv_event_t *e)
{
    (void)e;
    if (!s_pg) {
        return;
    }
    char name[PROFILE_NAME_MAX_LEN + 1];
    if (!profiles_live_decide_default_name(&s_pg->st, name, sizeof(name))) {
        set_status("No free auto-name -- save on the web.", false);
        return;
    }
    run_decision(LIVE_EDIT_DECISION_SAVE_AS, name);
}

static void screen_event_cb(lv_event_t *e)
{
    if (s_pg && lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED) {
        profiles_live_decide_status(&s_pg->st); /* reached without prepare() */
        refresh();
    }
}

static lv_obj_t *build_action_btn(lv_obj_t *parent, const char *text, lv_color_t color, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, BTN_HEIGHT_PX);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_radius(btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    ui_theme_apply_touch_area(btn, false);
    return btn;
}

lv_obj_t *ui_page_live_decide_build(void)
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

    ui_topbar_create(scr, &(ui_topbar_cfg_t){
        .title = "Keep Edit?",
        .back_page = "home",
        .show_home = true,
    }, &s_pg->tb);

    s_pg->info_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_pg->info_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_long_mode(s_pg->info_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_pg->info_label, lv_pct(100));
    lv_label_set_text(s_pg->info_label, "");

    s_pg->discard_btn = build_action_btn(scr, "Discard edit", UI_THEME_ACCENT_5, discard_cb);
    s_pg->save_btn = build_action_btn(scr, "Save as new (auto-named)", UI_THEME_ACCENT_4, save_cb);
    s_pg->overwrite_btn = build_action_btn(scr, "Overwrite original", UI_THEME_ACCENT_1, overwrite_cb);

    s_pg->status_label = lv_label_create(scr);
    lv_label_set_long_mode(s_pg->status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_pg->status_label, lv_pct(100));
    lv_label_set_text(s_pg->status_label, "");

    lv_obj_t *hint = lv_label_create(scr);
    lv_obj_set_style_text_color(hint, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_text(hint, "Rename a saved copy on the web.");

    ui_topbar_raise(&s_pg->tb);
    refresh();
    return scr;
}
