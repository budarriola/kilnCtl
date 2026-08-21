#include "ui_page_profiles_builtin_list.h"

#include <stdint.h>
#include <string.h>

#include "kiln_ui.h"
#include "profile_feasibility.h"
#include "profiles_builtin.h"
#include "ui_page_profile_detail.h"
#include "ui_theme.h"

/* Paged 2-column grid, identical arithmetic to ui_page_profiles_mine.c (see
 * that file's header comment for the full derivation): 4 cells/page, up to
 * 2 pages for a family's at-most-8 entries. */
#define ENTRIES_PER_PAGE 4
#define MAX_FAMILY_ENTRIES 8 /* largest family in this pass's FAMILIES split (Plainsman/Crystalline) */
#define GRID_HEIGHT_PX (UI_THEME_MIN_TOUCH_TARGET_PX * 2 + UI_THEME_PADDING_PX / 2)

static const char *s_family = "Bartlett";
static uint8_t s_ids[MAX_FAMILY_ENTRIES];
static uint8_t s_id_count;
static uint8_t s_page;
static uint8_t s_page_count;

static lv_obj_t *s_grid;
static lv_obj_t *s_indicator;
static lv_obj_t *s_title_label;

static void render_page(void); /* forward decl -- set_family() below needs it */

static void reload_ids(void)
{
    s_id_count = 0;
    for (size_t i = 0; i < g_builtin_profile_count && s_id_count < MAX_FAMILY_ENTRIES; i++) {
        uint8_t id = (uint8_t)(PROFILE_BUILTIN_ID_BASE + i);
        const builtin_profile_t *b = profiles_builtin_entry(id);
        if (!b || !b->family || strcmp(b->family, s_family) != 0) {
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
    s_page_count = (uint8_t)((s_id_count + ENTRIES_PER_PAGE - 1) / ENTRIES_PER_PAGE);
    if (s_page_count == 0) {
        s_page_count = 1;
    }
}

void ui_page_profiles_builtin_list_set_family(const char *family)
{
    s_family = family ? family : "Bartlett";
    s_page = 0;
    reload_ids();
    if (s_title_label) {
        lv_label_set_text(s_title_label, s_family);
    }
    /* Grid may not exist yet on the very first call (screen not built) --
     * render_page() itself already guards on s_grid being non-NULL, and
     * ui_page_profiles_builtin_list_build() calls reload_ids()+render_page()
     * again at build time regardless, so this is safe either order. */
    render_page();
}

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    kiln_ui_show("profiles_family");
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
        lv_label_set_text(label, b ? b->code : "?");
        lv_obj_center(label);

        lv_obj_update_layout(cell);
        ui_theme_apply_touch_area(cell, true);
    }

    if (s_indicator) {
        lv_label_set_text_fmt(s_indicator, "%u of %u", (unsigned)(s_page + 1), (unsigned)s_page_count);
    }
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

static lv_obj_t *build_nav_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, UI_THEME_MIN_TOUCH_TARGET_PX + UI_THEME_PADDING_PX * 2, 44);
    lv_obj_set_style_bg_color(btn, UI_THEME_COLOR_CARD, 0);
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

lv_obj_t *ui_page_profiles_builtin_list_build(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_title_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_title_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(s_title_label, s_family);

    s_grid = lv_obj_create(scr);
    lv_obj_set_width(s_grid, lv_pct(100));
    lv_obj_set_height(s_grid, GRID_HEIGHT_PX);
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_grid, 0, 0);
    lv_obj_set_style_pad_all(s_grid, 0, 0);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(s_grid, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *nav_row = lv_obj_create(scr);
    lv_obj_set_width(nav_row, lv_pct(100));
    lv_obj_set_height(nav_row, 44);
    lv_obj_set_style_bg_opa(nav_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(nav_row, 0, 0);
    lv_obj_set_style_pad_all(nav_row, 0, 0);
    lv_obj_set_flex_flow(nav_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(nav_row, LV_OBJ_FLAG_SCROLLABLE);

    build_nav_button(nav_row, "Back", back_btn_cb);
    build_nav_button(nav_row, "< Prev", prev_cb);
    s_indicator = lv_label_create(nav_row);
    lv_obj_set_style_text_color(s_indicator, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_indicator, "");
    build_nav_button(nav_row, "Next >", next_cb);

    render_page();
    return scr;
}
