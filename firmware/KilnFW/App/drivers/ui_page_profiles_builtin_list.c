#include "ui_page_profiles_builtin_list.h"

#include <stdint.h>
#include <stdio.h>

#include "kiln_ui.h"
#include "profile_feasibility.h"
#include "profiles_builtin.h"
#include "ui_page_profile_detail.h"
#include "ui_theme.h"
#include "ui_topbar.h"

/* Paged 2-column grid, identical arithmetic to ui_page_profiles_mine.c (see
 * that file's header comment for the full derivation): 4 cells/page.
 * Back/Prev/Next used to be a fourth in-content nav row (44px + 4px gap);
 * they moved into the shared top bar (ui_topbar.c) in the 2026-08-21
 * icon-topbar pass -- see ui_page_profiles_mine.c's header comment for the
 * identical rationale. Only the "N of M" indicator still lives in content.
 *
 * Unlike the old 4-way family split (at most 8 entries each, so at most 2
 * pages), the 3-way firing-type split is uneven -- Glaze alone holds up to
 * 23 of the 28 entries as of 2026-09-04, i.e. up to 6 pages. The per-page
 * content is unchanged (still 4 cells at the full touch-target size), so the
 * existing GRID_HEIGHT_PX arithmetic still fits the 267px no-scroll budget;
 * only the page COUNT grows, which the "N of M" indicator and Prev/Next
 * already handle generically. Bound the id array at the full catalogue size
 * rather than a per-category constant, since one category can hold nearly
 * all of it. */
#define ENTRIES_PER_PAGE 4
#define MAX_ENTRIES 32 /* >= g_builtin_profile_count (28); one static array covers every category */
#define GRID_HEIGHT_PX (UI_THEME_MIN_TOUCH_TARGET_PX * 2 + UI_THEME_PADDING_PX / 2)

static profile_firing_type_t s_type = PROFILE_FIRING_BISQUE;
static uint8_t s_ids[MAX_ENTRIES];
static uint8_t s_id_count;
static uint8_t s_page;
static uint8_t s_page_count;

static lv_obj_t *s_grid;
static lv_obj_t *s_indicator;
static ui_topbar_t s_tb;

static void render_page(void); /* forward decl -- set_firing_type() below needs it */

static void reload_ids(void)
{
    s_id_count = 0;
    for (size_t i = 0; i < g_builtin_profile_count && s_id_count < MAX_ENTRIES; i++) {
        uint8_t id = (uint8_t)(PROFILE_BUILTIN_ID_BASE + i);
        const builtin_profile_t *b = profiles_builtin_entry(id);
        if (!b || b->firing_type != s_type) {
            continue;
        }
        if (profiles_builtin_is_hidden(id)) {
            /* Hidden is a listing preference (see profiles_builtin.h) -- an
             * operator who hid a schedule does not expect it to keep
             * appearing in a browse list; profiles_builtin_restore_all() on
             * the Profiles hub is how it comes back. */
            continue;
        }
        s_ids[s_id_count++] = id;
    }

    /* Sort ascending by cone (see builtin_profile_t.cone's comment in
     * profiles_builtin.h for why plain signed comparison already matches
     * ascending heat-work), with PROFILES_BUILTIN_CONE_UNRATED entries last
     * regardless of its raw INT8_MIN value -- profiles_builtin_cone_sort_key()
     * maps the sentinel to INT16_MAX so it never sorts ahead of a real cone (a plain
     * int8_t compare would have put it first, since INT8_MIN is already the
     * lowest possible value). Small n (<= 28), insertion sort is plenty. */
    for (uint8_t i = 1; i < s_id_count; i++) {
        uint8_t key_id = s_ids[i];
        const builtin_profile_t *key = profiles_builtin_entry(key_id);
        int16_t key_cone = key ? profiles_builtin_cone_sort_key(key->cone) : 0;
        int8_t j = (int8_t)(i - 1);
        while (j >= 0) {
            const builtin_profile_t *cur = profiles_builtin_entry(s_ids[j]);
            int16_t cur_cone = cur ? profiles_builtin_cone_sort_key(cur->cone) : 0;
            if (cur_cone <= key_cone) {
                break;
            }
            s_ids[j + 1] = s_ids[j];
            j--;
        }
        s_ids[j + 1] = key_id;
    }

    s_page_count = (uint8_t)((s_id_count + ENTRIES_PER_PAGE - 1) / ENTRIES_PER_PAGE);
    if (s_page_count == 0) {
        s_page_count = 1;
    }
}

void ui_page_profiles_builtin_list_set_firing_type(profile_firing_type_t type)
{
    s_type = type;
    s_page = 0;
    reload_ids();
    ui_topbar_set_title(&s_tb, profiles_builtin_firing_type_label(s_type));
    /* Grid may not exist yet on the very first call (screen not built) --
     * render_page() itself already guards on s_grid being non-NULL, and
     * ui_page_profiles_builtin_list_build() calls reload_ids()+render_page()
     * again at build time regardless, so this is safe either order. */
    render_page();
}

static void entry_clicked_cb(lv_event_t *e)
{
    uint8_t id = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    ui_page_profile_detail_set_id(id, "profiles_builtin_list");
    kiln_ui_show("profile_detail");
}

static void render_page(void)
{
    if (!s_grid) {
        return;
    }
    lv_obj_clean(s_grid);

    uint8_t start = (uint8_t)(s_page * ENTRIES_PER_PAGE);
    for (uint8_t i = start; i < s_id_count && i < (uint8_t)(start + ENTRIES_PER_PAGE); i++) {
        uint8_t id = s_ids[i];
        const builtin_profile_t *b = profiles_builtin_entry(id);

        lv_obj_t *cell = lv_button_create(s_grid);
        lv_obj_set_width(cell, lv_pct(48));
        lv_obj_set_height(cell, UI_THEME_MIN_TOUCH_TARGET_PX);
        lv_obj_set_style_bg_color(cell, UI_THEME_COLOR_CARD, 0);
        lv_obj_set_style_radius(cell, UI_THEME_CORNER_RADIUS_PX, 0);
        lv_obj_add_event_cb(cell, entry_clicked_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)id);

        if (b) {
            /* Zone-agnostic catalogue entry -- feasibility_profile_mask()
             * with mask 0 means "every configured zone" (profile_feasibility.h),
             * same reading profiles_http_get() itself gives a builtin's
             * zone_mask. */
            profile_t prof;
            if (profiles_builtin_get(id, &prof)) {
                profile_seg_verdict_t v = profile_feasibility_profile_mask(0, &prof, NULL, 0);
                if (v == PROFILE_SEG_TOO_FAST || v == PROFILE_SEG_UNREACHABLE) {
                    lv_obj_set_style_border_width(cell, 3, 0);
                    lv_obj_set_style_border_color(cell, UI_THEME_ACCENT_5, 0);
                }
            }
        }

        lv_obj_t *label = lv_label_create(cell);
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
        if (b) {
            /* Cone visible in the row so the ascending-cone sort is legible,
             * not just implied by list order. */
            char cone_buf[8];
            profiles_builtin_cone_label(b->cone, cone_buf, sizeof(cone_buf));
            char text[40];
            /* Unrated entries print bare ("CODE\nUnrated"), not "Cone Unrated"
             * -- there is no cone to report, so labelling it as one would be
             * the same false-number problem this sentinel exists to avoid. */
            if (b->cone == PROFILES_BUILTIN_CONE_UNRATED) {
                snprintf(text, sizeof(text), "%s\n%s", b->code, cone_buf);
            } else {
                snprintf(text, sizeof(text), "%s\nCone %s", b->code, cone_buf);
            }
            lv_label_set_text(label, text);
        } else {
            lv_label_set_text(label, "?");
        }
        lv_obj_center(label);

        lv_obj_update_layout(cell);
        ui_theme_apply_touch_area(cell, true);
    }

    if (s_indicator) {
        lv_label_set_text_fmt(s_indicator, "%u of %u", (unsigned)(s_page + 1), (unsigned)s_page_count);
    }

    ui_topbar_set_prev_enabled(&s_tb, s_page > 0);
    ui_topbar_set_next_enabled(&s_tb, (uint8_t)(s_page + 1) < s_page_count);
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
    if (s_page + 1 < s_page_count) {
        s_page++;
        render_page();
    }
}

lv_obj_t *ui_page_profiles_builtin_list_build(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    ui_topbar_create(scr, &(ui_topbar_cfg_t){
        .title = profiles_builtin_firing_type_label(s_type),
        .back_page = "profiles_family",
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

    render_page();
    return scr;
}
