// ui_page_profile_builder_segment -- Step 2 of the LCD profile CREATE/EDIT
// flow: one segment (Target C / Ramp C/hr / Dwell min) per screen, up to
// PROFILE_MAX_SEGMENTS (12) -- a flat list does not fit this panel's
// no-scroll budget the way ui_page_profile_segments.c's READ-ONLY list does
// (that page never has to show input controls per row), so this page pages
// through segments one at a time instead, editing the same shared draft
// ui_page_profile_builder_zones.c owns.
#ifndef UI_PAGE_PROFILE_BUILDER_SEGMENT_H
#define UI_PAGE_PROFILE_BUILDER_SEGMENT_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called right before navigating here (from Step 1's Next, or Step 3's
 * Back). Ensures the draft has at least one segment (a brand new profile
 * starts at segment_count 0; this seeds one blank segment so there is always
 * something on screen to edit) and resets the on-screen cursor to segment 0. */
void ui_page_profile_builder_segment_prepare(void);

lv_obj_t *ui_page_profile_builder_segment_build(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_PROFILE_BUILDER_SEGMENT_H
