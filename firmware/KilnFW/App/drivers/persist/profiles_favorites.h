#ifndef PROFILES_FAVORITES_H
#define PROFILES_FAVORITES_H

/* profiles_favorites -- the operator's "favorite" marks on firing profiles,
 * persisted across reboot.
 *
 * A favorite is a SHORTCUT, never a move. Marking a profile favorite does not
 * remove it from the combined profile list and does not relocate its storage;
 * it only sets a bit that the UI uses to also show that profile in its
 * Favorites section. A profile therefore cannot be lost by favoriting it, and
 * ANY profile can be favorited -- both the user's saved slots and the shipped
 * Digital Fire catalogue entries.
 *
 * IDENTIFIER: the existing one-byte profile id already distinguishes the two
 * namespaces (user slots are 0..PROFILES_MAX_COUNT-1, builtins are
 * PROFILE_BUILTIN_ID_BASE+index, see profiles_builtin.h), so no new
 * identifier scheme is invented here. Internally that single id space is
 * stored as TWO masks, one per namespace, because the two ranges are far
 * apart (0..7 and 128..155) and a single 256-bit mask would be mostly holes.
 *
 * STORAGE: same partition and namespace as the user slots and the builtin
 * hidden-mask -- this data belongs to the same feature and has the same
 * lifetime, so it migrates, factory-resets and rolls back with it rather
 * than acquiring a second place to look. Key names deliberately collide with
 * neither profiles_http.c's "prof0".."prof7"/"prof_used" nor
 * profiles_builtin.c's "prof_bihid", and both are within NVS's 15-character
 * key limit (enforced by NVS_KEY_LEN_CHECK in the .c -- this project has
 * already silently lost data once to a 16-character key).
 *
 * LIFECYCLE, decided deliberately so no favorite is ever left dangling:
 *
 *   - DELETING a saved profile clears that slot's favorite bit
 *     (profile_delete_post_handler calls profiles_favorites_set(id, false)).
 *     The slot becomes empty, so a favorite pointing at it would reference
 *     nothing.
 *
 *   - IMPORTING over an occupied slot KEEPS the favorite. The favorite marks
 *     the slot, the slot still holds a profile afterward, and the import is
 *     an edit of that slot's contents rather than its disappearance. This is
 *     the deliberate opposite of the delete case, and the distinction is
 *     exactly "does a profile still exist at this id afterward".
 *
 *   - HIDING a builtin does not clear its favorite: hiding is reversible and
 *     the entry still exists in flash (see profiles_builtin.h), so the mark
 *     is still meaningful when it is unhidden.
 *
 * PERSISTENCE (owner decision 2026-10-07, docs/CONFIG_FILESYSTEM.md): the
 * masks live in one cfg file ("prof_fav.bin", pref_cfg_fs rev-prefixed
 * record) and saves go there ONLY. The legacy profiles_nvs keys are read once
 * at boot as a fallback and migrated into the file; nothing writes them any
 * more. A save while cfg is unmounted fails (the HTTP route refuses with 503
 * first); the change still applies live and reverts on reboot.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "profiles_slot_bitmap.h"

/* cfg_fs relative path of the favorites file (profiles-scope reset names it). */
#define PROFILES_FAVORITES_FILE_PATH "prof_fav.bin"

/* Loads the persisted favorite masks. Call once at boot, BEFORE
 * profiles_http_start() registers the read paths that report them. A missing
 * key or a never-written namespace is not an error -- it means "nothing
 * favorited yet", which is the shipped default. */
esp_err_t profiles_favorites_start(void);

/* True if `id` (either namespace) is currently marked favorite. An id that is
 * in neither namespace answers false rather than failing. */
bool profiles_favorites_is(uint8_t id);

/* Marks or unmarks `id`. Returns ESP_OK when the change was also persisted;
 * a non-OK return (ESP_ERR_INVALID_STATE when cfg is not mounted) means the
 * change IS live but did not reach flash. An id in neither namespace returns
 * ESP_ERR_INVALID_ARG and changes nothing. */
esp_err_t profiles_favorites_set(uint8_t id, bool favorite);

/* Read-only dual-write status for GET /api/cfgfs; same contract as
 * unit_pref_get_dualwrite_status(). Any output pointer may be NULL. */
void profiles_favorites_get_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid, uint32_t *nvs_rev,
                                             bool *diverged);

/* Raw masks, for the JSON listing. Bit i of *out_user is user slot i; bit i
 * of *out_builtin is builtin catalogue index i (that is, id
 * PROFILE_BUILTIN_ID_BASE+i). Either pointer may be NULL.
 *
 * `out_user` widened uint32_t* -> profiles_slot_bitmap_t* (docs/
 * PROFILE_SLOTS_100.md section 7 task 1) -- the plan's Status section
 * names the old `user_mask & (1u << i)` scalar test as undefined behavior
 * once `i` reaches 32, which the 100-slot raise (task 6) would do.
 * `out_builtin` stays a plain uint32_t: the builtin catalogue is a fixed
 * .rodata table (28 entries today, see profiles_builtin.h) nowhere near 32
 * and is not affected by the user-slot count. */
void profiles_favorites_masks(profiles_slot_bitmap_t *out_user, uint32_t *out_builtin);

#endif /* PROFILES_FAVORITES_H */
