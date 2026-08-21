// ui_page_profiles_mine -- "My Profiles": the 8 user slots (ids 0..7),
// paginated 4-per-page same as ui_page_config.c's hub. An empty slot shows
// as a dimmed, non-tappable placeholder rather than being skipped, so the
// operator can see there IS room for more without guessing which ids are
// free (matches profiles_page.html's web listing, which shows all 8 slots
// too).
#ifndef UI_PAGE_PROFILES_MINE_H
#define UI_PAGE_PROFILES_MINE_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *ui_page_profiles_mine_build(void);

/* Resets paging to the first page and re-reads every slot from NVS. Call
 * before kiln_ui_show("profiles_mine") -- this screen is built once and
 * cached (kiln_ui.c), so without this a slot saved or deleted (from the web
 * dashboard, say) while the LCD was elsewhere would not show up until a
 * reboot. Safe to call before the screen has been built. */
void ui_page_profiles_mine_refresh(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_PROFILES_MINE_H
