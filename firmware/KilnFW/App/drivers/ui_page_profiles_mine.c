#include "ui_page_profiles_mine.h"

#include <stdio.h>

#include "kiln_ui.h"
#include "profile_feasibility.h"
#include "profiles_http.h"
#include "ui_page_profile_detail.h"
#include "ui_theme.h"
#include "ui_topbar.h"

/* Paged 2-column grid, identical arithmetic to ui_page_config.c's hub (see
 * that file's header comment for the full derivation against the real
 * ~267px content budget):
 *
 *     "N of M" indicator row ..................... 20px
 *     gap ......................................... 4px
 *     one page: 2 rows x 72px + 1 gap ........... 148px
 *                                                ------
 *                                                 172px  <= 267px  OK
 *
 * Back/Prev/Next used to be a fourth in-content nav row (44px + 4px gap);
 * they moved into the shared top bar (ui_topbar.c) in the 2026-08-21
 * icon-topbar pass -- Back and Home are ui_topbar's ordinary icons, and
 * Prev/Next are wired through cfg.prev_cb/next_cb with s_tb kept static so
 * render_page() can call ui_topbar_set_prev_enabled()/set_next_enabled() to
 * grey out whichever end is clamped. Only the "N of M" indicator, which has
 * no home in ui_topbar_t, still lives in content.
 *
 * 8 user slots at 4 per page needs exactly 2 pages. */
#define SLOTS_PER_PAGE 4
#define PAGE_COUNT ((PROFILES_MAX_COUNT + SLOTS_PER_PAGE - 1) / SLOTS_PER_PAGE)
#define GRID_HEIGHT_PX (UI_THEME_MIN_TOUCH_TARGET_PX * 2 + UI_THEME_PADDING_PX / 2)

static lv_obj_t *s_grid;
static lv_obj_t *s_indicator;
static uint8_t s_page;
static ui_topbar_t s_tb;

static void slot_clicked_cb(lv_event_t *e)
{
    uint8_t id = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    ui_page_profile_detail_set_id(id, "profiles_mine");
    kiln_ui_show("profile_detail");
}

static void render_page(void)
{
    if (!s_grid) {
        return;
    }
    lv_obj_clean(s_grid);

    uint8_t start = (uint8_t)(s_page * SLOTS_PER_PAGE);
    for (uint8_t id = start; id < PROFILES_MAX_COUNT && id < (uint8_t)(start + SLOTS_PER_PAGE); id++) {
        profile_t prof;
        bool used = profiles_http_get(id, &prof);

        lv_obj_t *cell = lv_button_create(s_grid);
        lv_obj_set_width(cell, lv_pct(48));
        lv_obj_set_height(cell, UI_THEME_MIN_TOUCH_TARGET_PX);
        lv_obj_set_style_bg_color(cell, UI_THEME_COLOR_CARD, 0);
        lv_obj_set_style_radius(cell, UI_THEME_CORNER_RADIUS_PX, 0);

        if (used) {
            /* Feasibility colouring, matching the web dashboard: too_fast/
             * unreachable get a dark-red border, unknown gets no marking, ok
             * is ordinary. */
            /* Review fix: a user-slot profile with zone_mask == 0 targets no
             * zones -- the executor refuses it outright ("targets no
             * zones") -- so feasibility() would get an optimistic coupled
             * verdict for a profile that can never run. Report unknown
             * instead. */
            profile_seg_verdict_t v = (prof.zone_mask == 0)
                ? PROFILE_SEG_UNKNOWN
                : profile_feasibility_profile_mask(prof.zone_mask, &prof, NULL, 0);
            if (v == PROFILE_SEG_TOO_FAST || v == PROFILE_SEG_UNREACHABLE) {
                lv_obj_set_style_border_width(cell, 3, 0);
                lv_obj_set_style_border_color(cell, UI_THEME_ACCENT_5, 0);
            }
            lv_obj_add_event_cb(cell, slot_clicked_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)id);
        } else {
            lv_obj_remove_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        }

        lv_obj_t *label = lv_label_create(cell);
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(label, used ? UI_THEME_COLOR_TEXT_PRIMARY : UI_THEME_COLOR_TEXT_SECONDARY, 0);
        char buf[40];
        if (used) {
            snprintf(buf, sizeof(buf), "%u: %s", (unsigned)id, prof.name);
        } else {
            snprintf(buf, sizeof(buf), "%u: (empty)", (unsigned)id);
        }
        lv_label_set_text(label, buf);
        lv_obj_center(label);

        if (used) {
            lv_obj_update_layout(cell);
            ui_theme_apply_touch_area(cell, true);
        }
    }

    if (s_indicator) {
        lv_label_set_text_fmt(s_indicator, "%u of %u", (unsigned)(s_page + 1), (unsigned)PAGE_COUNT);
    }

    ui_topbar_set_prev_enabled(&s_tb, s_page > 0);
    ui_topbar_set_next_enabled(&s_tb, (uint8_t)(s_page + 1) < PAGE_COUNT);
}

void ui_page_profiles_mine_refresh(void)
{
    s_page = 0;
    render_page();
}

static void prev_cb(lv_event_t *e)
{
    (void)e;
    if (s_page > 0) {
        s_page--;
        render_page();
    }
}

static void next_cb(lv_event_t *e)
{
    (void)e;
    if (s_page + 1 < PAGE_COUNT) {
        s_page++;
        render_page();
    }
}

lv_obj_t *ui_page_profiles_mine_build(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    ui_topbar_create(scr, &(ui_topbar_cfg_t){
        .title = "My Profiles",
        .back_page = "profiles",
        .show_home = true,
        .prev_cb = prev_cb,
        .next_cb = next_cb,
    }, &s_tb);

    s_indicator = lv_label_create(scr);
    lv_obj_set_style_text_color(s_indicator, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_indicator, "");

    s_grid = lv_obj_create(scr);
    lv_obj_set_width(s_grid, lv_pct(100));
    lv_obj_set_height(s_grid, GRID_HEIGHT_PX);
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_grid, 0, 0);
    lv_obj_set_style_pad_all(s_grid, 0, 0);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(s_grid, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);

    ui_topbar_raise(&s_tb);

    s_page = 0;
    render_page();
    return scr;
}
