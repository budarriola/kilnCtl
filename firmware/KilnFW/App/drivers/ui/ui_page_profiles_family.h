// ui_page_profiles_family -- despite the filename (kept to avoid an
// unrelated page-id/registration churn), this is now the builtin-catalogue
// FIRING-TYPE picker: Bisque / Glaze / Other. It used to pick a publisher
// family (Bartlett/Plainsman/Crystalline/General); that axis was not what a
// potter actually chooses a schedule on, so it moved to firing type first,
// then cone (ui_page_profiles_builtin_list.c sorts by builtin_profile_t.cone
// ascending within the chosen type). Publisher family is still real
// attribution and still shown on the profile detail page -- see
// builtin_profile_t.family in profiles_builtin.h -- it just is not the
// browse axis any more.
#ifndef UI_PAGE_PROFILES_FAMILY_H
#define UI_PAGE_PROFILES_FAMILY_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *ui_page_profiles_family_build(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_PROFILES_FAMILY_H
