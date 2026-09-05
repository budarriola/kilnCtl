#include "ui_page_profile_builder_zones.h"

#include <stdio.h>
#include <string.h>

#include "MAX31856.h"
#include "kiln_ui.h"
#include "profiles_builtin.h"
#include "ui_num_pad.h"
#include "ui_page_profile_builder_segment.h"
#include "ui_theme.h"
#include "ui_topbar.h"
#include "zones_config_accessors.h"

/* Arithmetic (same style as every other page in this pass), against the real
 * ~267px content budget:
 *
 *     title line ................................. ~20px
 *     gap ..........................................  4px
 *     name row (tap-to-edit, 44px drawn/72px hit) .. 44px
 *     gap ..........................................  4px
 *     "Zones:" label .............................. ~18px
 *     gap ..........................................  4px
 *     zone chip row (up to MAX31856_CHANNEL_COUNT) . 44px
 *     gap ..........................................  4px
 *     validation caption (reserved even when blank) 18px
 *     gap ..........................................  4px
 *     nav row (Next) ............................... 44px
 *                                                   ------
 *                                                    208px  <= 267px  OK
 *
 * Name/chip rows are drawn at 44px, not the full 72px touch minimum -- same
 * "draw small, extend the hit area" trick ui_page_network.c's mode toggle
 * already uses (see that file's 2026-08-18 no-scroll-pass comment), applied
 * here via ui_theme_apply_touch_area()'s non-compact path, which extends a
 * sub-72px widget's effective click area up to the real minimum without
 * growing what's drawn.
 *
 * 2026-08-21 icon-topbar pass: Back moved into the shared top bar
 * (ui_topbar.c) -- freeing its 44px-wide slot inside nav_row, not a whole
 * row, since nav_row's HEIGHT was already set by Next and stays 44px either
 * way (no vertical budget freed here). Next stays an in-content button,
 * deliberately NOT moved to ui_topbar_cfg_t::next_cb: that slot is for a
 * page that PAGES (clamps at a first/last boundary, N-of-M style, e.g.
 * ui_page_profiles_mine.c), not for a wizard's "advance to the next step"
 * action -- this page has no Prev counterpart and Next's enabled state is
 * validation-gated (has_zone), not a page-boundary clamp. Using the shared
 * media-skip Next icon for a semantically different action would blur the
 * one thing this pass is trying to make consistent. */

static profile_t s_draft;
static lv_obj_t *s_name_btn_label;
static lv_obj_t *s_zone_row;
static lv_obj_t *s_caption;
static lv_obj_t *s_next_btn;
static lv_obj_t *s_zone_chips[MAX31856_CHANNEL_COUNT];

void ui_page_profile_builder_start_new(void)
{
    memset(&s_draft, 0, sizeof(s_draft));
}

void ui_page_profile_builder_start_edit(uint8_t source_id)
{
    memset(&s_draft, 0, sizeof(s_draft));
    bool ok = (source_id >= PROFILE_BUILTIN_ID_BASE) ? profiles_builtin_get(source_id, &s_draft)
                                                     : profiles_http_get(source_id, &s_draft);
    (void)ok; /* leave a blank draft if the source vanished -- Step 1 still works, just empty */
}

profile_t *ui_page_profile_builder_draft(void)
{
    return &s_draft;
}

static void refresh_validation(void)
{
    bool has_zone = s_draft.zone_mask != 0;
    if (s_caption) {
        lv_label_set_text(s_caption, has_zone ? "" : "Select at least one zone to continue");
    }
    if (s_next_btn) {
        if (has_zone) {
            lv_obj_add_flag(s_next_btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_opa(s_next_btn, LV_OPA_COVER, 0);
        } else {
            lv_obj_remove_flag(s_next_btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_opa(s_next_btn, LV_OPA_50, 0);
        }
    }
}

static void next_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_draft.zone_mask == 0) {
        return; /* belt-and-suspenders -- button is non-clickable in this state already */
    }
    ui_page_profile_builder_segment_prepare();
    kiln_ui_show("profile_builder_segment");
}

static void name_done_cb(bool accepted, const char *text, float value, void *user_data)
{
    (void)value;
    (void)user_data;
    if (!accepted) {
        return;
    }
    snprintf(s_draft.name, sizeof(s_draft.name), "%s", text);
    if (s_name_btn_label) {
        lv_label_set_text(s_name_btn_label, s_draft.name[0] ? s_draft.name : "(tap to name)");
    }
}

static void name_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_num_pad_params_t params = {
        .caption = "Profile Name",
        .mode = UI_NUM_PAD_MODE_TEXT,
        .initial_text = s_draft.name,
        .max_len = PROFILE_NAME_MAX_LEN,
        .on_done = name_done_cb,
        .user_data = NULL,
    };
    ui_num_pad_show(&params);
}

static void zone_chip_cb(lv_event_t *e)
{
    uint8_t zi = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    s_draft.zone_mask ^= (uint8_t)(1u << zi);
    bool active = (s_draft.zone_mask & (1u << zi)) != 0;
    lv_obj_set_style_bg_color(s_zone_chips[zi], active ? UI_THEME_ACCENT_3 : UI_THEME_COLOR_CARD, 0);
    refresh_validation();
}

static void render_zone_chips(void)
{
    lv_obj_clean(s_zone_row);
    uint8_t count = zones_config_get_thermo_count();
    if (count > MAX31856_CHANNEL_COUNT) {
        count = MAX31856_CHANNEL_COUNT; /* real hardware ceiling -- see MAX31856.h */
    }
    for (uint8_t zi = 0; zi < count; zi++) {
        lv_obj_t *chip = lv_button_create(s_zone_row);
        lv_obj_set_height(chip, 44);
        lv_obj_set_flex_grow(chip, 1);
        bool active = (s_draft.zone_mask & (1u << zi)) != 0;
        lv_obj_set_style_bg_color(chip, active ? UI_THEME_ACCENT_3 : UI_THEME_COLOR_CARD, 0);
        lv_obj_set_style_radius(chip, UI_THEME_CORNER_RADIUS_PX, 0);
        lv_obj_add_event_cb(chip, zone_chip_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)zi);
        s_zone_chips[zi] = chip;

        char name[16];
        char buf[24];
        if (zones_config_get_name(zi, name, sizeof(name)) && name[0]) {
            snprintf(buf, sizeof(buf), "%s", name);
        } else {
            snprintf(buf, sizeof(buf), "Zone %u", (unsigned)zi);
        }
        lv_obj_t *label = lv_label_create(chip);
        lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
        lv_label_set_text(label, buf);
        lv_obj_center(label);
        lv_obj_update_layout(chip);
        ui_theme_apply_touch_area(chip, true);
    }
}

lv_obj_t *ui_page_profile_builder_zones_build(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    static ui_topbar_t tb;
    ui_topbar_create(scr, &(ui_topbar_cfg_t){
        .title = "New Profile -- Name & Zones",
        .back_page = "profiles",
        .show_home = true,
    }, &tb);

    lv_obj_t *name_btn = lv_button_create(scr);
    lv_obj_set_width(name_btn, lv_pct(100));
    lv_obj_set_height(name_btn, 44);
    lv_obj_set_style_bg_color(name_btn, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_radius(name_btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(name_btn, name_btn_cb, LV_EVENT_CLICKED, NULL);
    s_name_btn_label = lv_label_create(name_btn);
    lv_obj_set_style_text_color(s_name_btn_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(s_name_btn_label, "(tap to name)");
    lv_obj_center(s_name_btn_label);
    lv_obj_update_layout(name_btn);
    ui_theme_apply_touch_area(name_btn, false);

    lv_obj_t *zones_label = lv_label_create(scr);
    lv_obj_set_style_text_color(zones_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(zones_label, "Zones (tap to toggle):");

    s_zone_row = lv_obj_create(scr);
    lv_obj_set_width(s_zone_row, lv_pct(100));
    lv_obj_set_height(s_zone_row, 44);
    lv_obj_set_style_bg_opa(s_zone_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_zone_row, 0, 0);
    lv_obj_set_style_pad_all(s_zone_row, 0, 0);
    lv_obj_set_flex_flow(s_zone_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(s_zone_row, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(s_zone_row, LV_OBJ_FLAG_SCROLLABLE);

    s_caption = lv_label_create(scr);
    lv_obj_set_style_text_color(s_caption, UI_THEME_ACCENT_5, 0);
    lv_label_set_text(s_caption, "");

    lv_obj_t *nav_row = lv_obj_create(scr);
    lv_obj_set_width(nav_row, lv_pct(100));
    lv_obj_set_height(nav_row, 44);
    lv_obj_set_style_bg_opa(nav_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(nav_row, 0, 0);
    lv_obj_set_style_pad_all(nav_row, 0, 0);
    lv_obj_set_flex_flow(nav_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(nav_row, LV_OBJ_FLAG_SCROLLABLE);

    s_next_btn = lv_button_create(nav_row);
    lv_obj_set_size(s_next_btn, UI_THEME_MIN_TOUCH_TARGET_PX + UI_THEME_PADDING_PX * 2, 44);
    lv_obj_set_style_bg_color(s_next_btn, UI_THEME_ACCENT_4, 0);
    lv_obj_set_style_radius(s_next_btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(s_next_btn, next_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *next_label = lv_label_create(s_next_btn);
    lv_obj_set_style_text_color(next_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(next_label, "Next");
    lv_obj_center(next_label);
    lv_obj_update_layout(s_next_btn);
    ui_theme_apply_touch_area(s_next_btn, false);

    ui_topbar_raise(&tb);

    lv_label_set_text(s_name_btn_label, s_draft.name[0] ? s_draft.name : "(tap to name)");
    render_zone_chips();
    refresh_validation();
    return scr;
}
