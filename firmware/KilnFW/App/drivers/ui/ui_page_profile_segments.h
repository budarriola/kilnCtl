// ui_page_profile_segments -- read-only, paginated segment list (target C /
// ramp C-per-hr / dwell min) for whichever profile ui_page_profile_detail.c
// last set (ui_page_profile_detail_get_id()). Reached from that screen's
// "Segments" button; Back always returns to "profile_detail" -- there is
// only one caller, unlike ui_page_profile_detail.c's two.
#ifndef UI_PAGE_PROFILE_SEGMENTS_H
#define UI_PAGE_PROFILE_SEGMENTS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *ui_page_profile_segments_build(void);

/* Reloads this screen's segment data from ui_page_profile_detail_get_id()
 * and resets paging to the first page. Call BEFORE kiln_ui_show
 * ("profile_segments") -- this screen is built once and cached (kiln_ui.c),
 * so without this a second visit for a different profile would keep showing
 * the first profile's segments. Safe to call before the screen has been
 * built (it only updates static data + widgets that may not exist yet;
 * ui_page_profile_segments_build() reads the same data on first build). */
void ui_page_profile_segments_prepare(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_PROFILE_SEGMENTS_H
