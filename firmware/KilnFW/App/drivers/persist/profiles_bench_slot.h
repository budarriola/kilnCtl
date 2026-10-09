/* profiles_bench_slot.h -- the hidden bench-harness profile slot.
 *
 * docs/PROFILE_SLOTS_100.md section 7, "Owner decision, 2026-09-19
 * (post phase-A review)": beyond the 100 user slots (ids 0..99) and the 1
 * reserved live-edit slot (id 100, LIVE_EDIT_WORKING_SLOT_ID, live_profile.h),
 * there is one additional hidden slot reserved for
 * docs/BENCH_TEST_SYSTEM_PLAN.md's bench test system to use. It is never
 * listed in the catalogue, never counted in the favorites masks, never
 * exported/backed up, and never shown on any web page or the LCD -- the
 * same visibility exclusion LIVE_EDIT_WORKING_SLOT_ID already gets.
 *
 * This id is deliberately NOT part of the 0..PROFILES_MAX_COUNT-1 loop range
 * every ordinary catalogue/favorites/LCD-picker loop already uses, so the
 * exclusion is structural rather than an extra check that could be missed
 * at a new call site: any code that iterates
 * `for (id = 0; id < PROFILES_MAX_COUNT; id++)` (profiles_catalog_http.c's
 * profiles_list_get_handler()/favorites_list_get_handler(),
 * profiles_export_http.c, backup export/import, ui_page_profile_picker.c's
 * catalogue build) never reaches this id at all. profiles_favorites.c's
 * fav_locate() also rejects it: it is neither `< PROFILES_MAX_COUNT` (user)
 * nor `profiles_builtin_id_valid()` (builtin), so
 * profiles_favorites_is()/_set() answer false/ESP_ERR_INVALID_ARG for it,
 * exactly like any other out-of-range id today.
 *
 * No profile storage backs this id yet -- the bench test system that will
 * actually read/write it is future work (BENCH_TEST_SYSTEM_PLAN.md). This
 * header exists now, at the same time PROFILES_MAX_COUNT and
 * LIVE_EDIT_WORKING_SLOT_ID move to their 100-slot layout, so the id is
 * reserved and the exclusion tests below are correct starting from this
 * commit rather than something a later change might silently invalidate. */
#ifndef PROFILES_BENCH_SLOT_H
#define PROFILES_BENCH_SLOT_H

#include "live_profile.h" /* LIVE_EDIT_WORKING_SLOT_ID */

#ifdef __cplusplus
extern "C" {
#endif

/* One past the live-edit working slot -- id 101 at today's PROFILES_MAX_COUNT
 * (100). Never compared against anything that assumes PROFILES_MAX_COUNT or
 * LIVE_EDIT_WORKING_SLOT_ID stay fixed -- if either moves, this id moves
 * with them automatically, same convention LIVE_EDIT_WORKING_SLOT_ID itself
 * uses. profiles_types.h's _Static_assert(PROFILES_MAX_COUNT + 2 <= 128)
 * covers this id too (the "+2" is the live-edit slot and this one). */
#define PROFILE_BENCH_SLOT_ID (LIVE_EDIT_WORKING_SLOT_ID + 1)

#ifdef __cplusplus
}
#endif

#endif /* PROFILES_BENCH_SLOT_H */
