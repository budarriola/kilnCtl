#include "ui_page_profiles.h"

#include "ui_page_profile_picker.h"

/* This page is now a thin alias for the unified profile list/picker's
 * MANAGE mode (UI_PLAN.md Section 6.2) -- the old four-cell hub
 * (My Profiles/Built-ins/Restore hidden/New Profile) and its two sub-pages
 * (ui_page_profiles_mine.c, ui_page_profiles_family.c) are gone. "Restore
 * hidden" is web-only per the plan's owner decision; New moved to a topbar
 * icon (ui_page_profile_picker.c's add_cb); Built-ins/My Profiles collapsed
 * into the one combined, favorites-first list. */
lv_obj_t *ui_page_profiles_build(void)
{
    return ui_page_profile_picker_build_manage();
}

void ui_page_profiles_refresh(void)
{
    ui_page_profile_picker_manage_refresh();
}
