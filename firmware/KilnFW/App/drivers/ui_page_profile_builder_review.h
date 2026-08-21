// ui_page_profile_builder_review -- Step 3 of the LCD profile CREATE/EDIT
// flow: a summary of the shared draft (name / zones / segment count / peak
// C / whole-profile feasibility) and Save, which opens a slot picker overlay
// (8 cells -- see this file's .c for why that's a full-screen overlay rather
// than sharing this screen's own content budget) rather than saving straight
// to whatever slot profiles_http_save()'s "first free slot" convention would
// pick. requested_id is always explicit here, on purpose: this is the LCD's
// one call site for that function, and an operator who just built a specific
// schedule should choose (or deliberately overwrite) a specific slot, not
// have one silently assigned -- and when all 8 are already used, an explicit
// pick is the only way to still succeed at all.
#ifndef UI_PAGE_PROFILE_BUILDER_REVIEW_H
#define UI_PAGE_PROFILE_BUILDER_REVIEW_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called right before navigating here (from Step 2's Next on the last
 * segment). Refreshes the summary labels from the current draft. */
void ui_page_profile_builder_review_prepare(void);

lv_obj_t *ui_page_profile_builder_review_build(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_PROFILE_BUILDER_REVIEW_H
