// ui_page_kiln_setup -- "Kiln Setup" hub, reached from ui_page_config.c's
// nav hub. Two destinations, both requested together but kept apart on
// screen and in naming (see this file's .c header comment for why):
//   - Firing Profile -- browse/pick/start a firing SCHEDULE. Rather than
//     duplicate ui_page_profiles.c's existing browse/pick/start tree (My
//     Profiles / Built-ins / detail+START), this cell just navigates there
//     -- that tree already IS the "restore the ability to choose a profile
//     without the web dashboard" capability the user asked for.
//   - Kiln Config -- the new named-hardware-snapshot picker/manager
//     (ui_page_kiln_cfg_setup.c): apply / save-current-as / clone / rename /
//     delete.
#ifndef UI_PAGE_KILN_SETUP_H
#define UI_PAGE_KILN_SETUP_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *ui_page_kiln_setup_build(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_KILN_SETUP_H
