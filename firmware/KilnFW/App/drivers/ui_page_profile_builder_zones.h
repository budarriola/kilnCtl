// ui_page_profile_builder_zones -- Step 1 of the LCD profile CREATE/EDIT
// flow: name + which configured zones this profile drives. Owns the shared
// working draft (a plain profile_t) that ui_page_profile_builder_segment.c
// and ui_page_profile_builder_review.c both read/write through
// ui_page_profile_builder_draft() -- one owner, same "set state, then
// navigate" pattern ui_page_profile_detail.c's set_id()/get_id() pair
// already establishes for this same page tree.
//
// Reached two ways:
//   - ui_page_profiles.c's "New Profile" cell -> start_new() -> a blank draft
//     (zone_mask 0, segment_count 0).
//   - ui_page_profile_detail.c's "Edit" action -> start_edit(profile_id) ->
//     the draft is seeded from that profile's current name/zones/segments,
//     whether the source is a user slot OR a builtin (profiles_builtin_get()
//     handles the latter). Editing a builtin therefore always starts as a
//     COPY in the draft; profiles_http_save() is never told to overwrite the
//     builtin's id (>= PROFILE_BUILTIN_ID_BASE) because Step 3's slot picker
//     only ever offers the 8 real user slots (PROFILES_MAX_COUNT), so a
//     builtin structurally cannot be the save target.
#ifndef UI_PAGE_PROFILE_BUILDER_ZONES_H
#define UI_PAGE_PROFILE_BUILDER_ZONES_H

#include <stdint.h>

#include "lvgl.h"
#include "profiles_http.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Resets the shared draft to a blank profile (empty name, zone_mask 0,
 * segment_count 0) ready for Step 1. */
void ui_page_profile_builder_start_new(void);

/* Seeds the shared draft from an existing profile (user slot or builtin) --
 * see this file's header comment on why editing a builtin is always a copy.
 * No-op (draft left blank) if source_id does not resolve to a real profile. */
void ui_page_profile_builder_start_edit(uint8_t source_id);

/* The shared working draft -- ui_page_profile_builder_segment.c and
 * ui_page_profile_builder_review.c both mutate/read through this pointer
 * rather than keeping their own copy, so the three steps can never disagree
 * about what is being built. Valid only between a start_new()/start_edit()
 * call and the next one (i.e. for the lifetime of one builder session). */
profile_t *ui_page_profile_builder_draft(void);

lv_obj_t *ui_page_profile_builder_zones_build(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_PROFILE_BUILDER_ZONES_H
