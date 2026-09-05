// Shipped-in-flash firing schedules -- the published Digital Fire catalogue
// (https://digitalfire.com/schedule), imported so a new board has real,
// known-good programs to fire without anyone typing a schedule in by hand.
//
// WHY THESE ARE NOT USER PROFILES. profiles_http.c's storage has exactly 8
// slots, addressed by a uint8_t used_bitmap, and that 0..7 id space runs
// through NVS, the UART protocol, the web API and the LCD. 28 catalogue
// entries do not fit in it and must not evict a user's own programs, so the
// catalogue is a separate, read-only table in .rodata with its own id range
// (PROFILE_BUILTIN_ID_BASE + index). Nothing here is copied into RAM or NVS
// unless the user explicitly saves one into a slot to edit it.
//
// "Removable by the user" is therefore a hide, not a delete: you cannot
// erase a const table in flash. profiles_builtin_set_hidden() records the
// choice in a persisted bitmask so a hidden schedule stops appearing in
// listings, and profiles_builtin_restore_all() brings them back. That is
// also why the mask is 32-bit -- one bit per catalogue entry, with room to
// spare, unlike the 8-bit user bitmap that forced this design in the first
// place.
#ifndef PROFILES_BUILTIN_H
#define PROFILES_BUILTIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "profiles_http.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Builtin ids start well above PROFILES_MAX_COUNT so that any id is
 * unambiguously either a user slot or a catalogue entry, with no overlap to
 * get wrong at a protocol boundary. Fits a uint8_t, which every existing id
 * field already is. */
#define PROFILE_BUILTIN_ID_BASE 128

/* Sentinel for "the source page states no cone number at all" (as opposed to
 * a low/unusual-but-real cone). Chosen as INT8_MIN so it can never collide
 * with a real cone (the printed Orton range is roughly 022..42, i.e. -22..42
 * in this field's encoding -- see the .cone comment below). Owner decision
 * 2026-09-05: the ten entries this repo's original pass marked UNRESOLVED
 * (a memory-derived, unverified guess standing in for a number the source
 * simply does not publish) get this sentinel instead of that guess.
 * profiles_builtin_cone_label() prints it as "Unrated", and the browse list
 * sorts it after every real cone within its firing-type group. */
#define PROFILES_BUILTIN_CONE_UNRATED INT8_MIN

/* What a potter actually picks a schedule by (see ui_page_profiles_family.c,
 * which despite its filename is now a firing-TYPE picker, not a publisher
 * picker). "Other" is deliberately not folded into Bisque or Glaze -- a
 * decal firing or a quartz-inversion cracking-avoidance schedule is neither,
 * and forcing one into "Glaze" would be a false classification, not a
 * simplification. */
typedef enum {
    PROFILE_FIRING_BISQUE = 0,
    PROFILE_FIRING_GLAZE,
    PROFILE_FIRING_OTHER,
} profile_firing_type_t;

/* Catalogue entry name limits: `code` reuses profile_t's name field verbatim
 * (so a builtin can be copied into a user slot without truncation), while
 * `title` is the human-readable name that field is too short to hold. */
typedef struct {
    const char       code[PROFILE_NAME_MAX_LEN + 1]; /* e.g. "C6DHSC" */
    const char      *title;                          /* e.g. "Plainsman Cone 6 Drop-and-hold, Slow Cool" */
    const char      *slug;                           /* digitalfire.com/schedule/<slug> -- the attribution link */
    /* Publisher attribution ("Bartlett", "Plainsman", "Crystalline",
     * "General") -- still shown on the profile detail page ("Plainsman
     * (builtin, read-only)"), but no longer the LCD browse axis. Browsing is
     * now by firing_type then cone (see below); this field is real
     * attribution for a published schedule, kept for that reason alone. */
    const char      *family;
    /* Firing-type browse axis. Has no universal derivation (unlike cone) --
     * hand-classified per entry; see profiles_builtin_table.inc's header
     * comment for the per-entry reasoning. */
    profile_firing_type_t firing_type;
    /* Orton cone number: cones below "1" are encoded negative, matching the
     * "0N" printed form (cone 04 -> -4, cone 6 -> +6). This is NOT the same
     * ordering as the printed digit -- it is chosen so that plain ascending
     * signed-integer sort already matches ascending heat-work, e.g.
     * -4 < -1 < 1 < 6 corresponds to cone04 < cone01 < cone1 < cone6, which
     * is the real Orton progression. Derived from this entry's peak segment
     * target_c and the ramp_c_per_hr of the segment that reaches it, read
     * off the standard two-speed Orton cone chart (60 C/hr slow column,
     * 108 C/hr fast column) -- see profiles_builtin_table.inc's header
     * comment for the per-entry rate-column choice and any conflict with a
     * cone number stated in the title. */
    int8_t           cone;
    uint8_t          segment_count;
    profile_segment_t segments[PROFILE_MAX_SEGMENTS];
} builtin_profile_t;

extern const builtin_profile_t g_builtin_profiles[];
extern const size_t            g_builtin_profile_count;

/* Loads the persisted hidden-mask. Call once at boot, after nvs_flash_init().
 * A missing key is not an error -- it means nothing has been hidden yet. */
esp_err_t profiles_builtin_start(void);

/* True if `id` is in the builtin range AND that entry exists. Does not
 * consider the hidden mask: a hidden schedule is still readable by direct id,
 * so an existing reference to one does not dangle. */
bool profiles_builtin_id_valid(uint8_t id);

/* Fills *out from the catalogue. Returns false for a non-builtin or
 * out-of-range id. zone_mask is left 0 -- the catalogue is zone-agnostic and
 * the caller supplies the zones when running or saving it. */
bool profiles_builtin_get(uint8_t id, profile_t *out);

/* The catalogue entry itself (title/slug/code), for listings and attribution.
 * NULL for a non-builtin id. */
const builtin_profile_t *profiles_builtin_entry(uint8_t id);

/* Display helpers for the two new browse fields. */
const char *profiles_builtin_firing_type_label(profile_firing_type_t type); /* "Bisque" / "Glaze" / "Other" */
/* Formats a cone field into its printed form ("04", "6", "10") into buf, or
 * "Unrated" for PROFILES_BUILTIN_CONE_UNRATED. buf must be at least 8 bytes
 * (strlen("Unrated") + NUL). */
void profiles_builtin_cone_label(int8_t cone, char *buf, size_t buf_len);

/* Hidden mask -- "removed by the user", persisted. Hiding is per entry and
 * reversible; see this file's header comment for why it is not a delete. */
bool      profiles_builtin_is_hidden(uint8_t id);
esp_err_t profiles_builtin_set_hidden(uint8_t id, bool hidden);
esp_err_t profiles_builtin_restore_all(void);

#ifdef __cplusplus
}
#endif

#endif // PROFILES_BUILTIN_H
