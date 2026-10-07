#ifndef PROFILES_HTTP_INTERNAL_H
#define PROFILES_HTTP_INTERNAL_H

/* Internal seams for the profiles_http.c split (2026-09-04, ROADMAP.md M15
 * "files over 1500 lines should be broken up where it makes sense" --
 * profiles_http.c had grown to 2097 lines). This header is NOT public API --
 * profiles_http.h stays that -- it exists purely so pieces that used to be
 * one translation unit (and could reach each other's `static` state and
 * helpers for free) can still do so now that they are three:
 *
 *   profiles_http.c        -- s_profiles storage, on-flash blob versioning/
 *                              decode, NVS load/save/erase/migrate,
 *                              PROFILES_TAG, the plain-C API
 *                              (profiles_http_get/save/delete/
 *                              get_bounds()) profile_executor.c and
 *                              uart_bridge_ext.c call directly, and
 *                              profiles_http_start() (registers every
 *                              handler defined in the other two files)
 *   profiles_catalog_http.c -- read side: GET /profiles (page),
 *                              GET /api/profiles, GET /api/profile,
 *                              GET /api/profiles/builtin
 *   profiles_edit_http.c    -- write side: POST /api/profile,
 *                              POST /api/profile/delete,
 *                              POST /api/profile/builtin/{hide,restore}
 *
 * Every symbol declared below was `static` in the original single file and
 * is widened to file-scope-internal linkage ONLY because a sibling .c file
 * in this split now calls or reads it directly. `TAG` is renamed
 * `PROFILES_TAG` (not just widened) on the same rule dashboard_http.c's
 * `DASH_TAG` follows -- every other driver file in App/drivers has its own
 * `static const char *TAG`, so a bare widened `TAG` would collide with the
 * first sibling that also widens its own. `page_get_handler` is likewise
 * renamed `profiles_page_get_handler` -- it is not just generically risky,
 * it ALREADY collides today: zones_http_handlers.c defines a non-static
 * `esp_err_t page_get_handler(httpd_req_t *req)`, so widening this file's
 * copy under the same bare name would be an immediate link-time multiple-
 * definition error, not a latent one. Every other widened symbol here
 * (`nvs_save_slot`, `nvs_erase_slot`, `profile_exceeds_zone_ceiling`,
 * `validate_io_segment`, `profiles_list_get_handler`,
 * `profile_detail_get_handler`, `builtin_list_get_handler`,
 * `profile_post_handler`, `profile_delete_post_handler`,
 * `builtin_hide_post_handler`, `builtin_restore_post_handler`) was grepped
 * repo-wide for both a non-static definition and a same-named `static` one
 * before this split landed -- none exist, so these keep their original
 * names to match the original file's own vocabulary. */

#include "profiles_http.h"

#include <stdio.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "profiles_slot_bitmap.h"

/* ---- shared JSON escaping --------------------------------------------------
 * Opus review of 5dd23944, finding 3: the per-file copies of this helper
 * (profiles_edit_http.c, profiles_catalog_http.c, profiles_export_http.c --
 * see their own PROFILES_HTTP_JSON_ESCAPE_DEFINED history) only escaped `"`
 * and `\`, so an operator-supplied profile name containing a raw control
 * byte (e.g. a literal newline, decoded from a form/JSON body) landed
 * verbatim in a JSON string body and broke the response -- a client's
 * r.json() throws instead of surfacing the real refusal reason. `static
 * inline` here (this header, not a .c file) is deliberate: `static` alone in
 * a header included by multiple TUs would just mean a silent per-TU
 * duplicate (harmless but wasteful); `inline` on a `static` function is what
 * actually lets multiple ordinary per-file TUs, AND test_profiles_http.c's
 * single combined TU (which #includes profiles_http.c/profiles_catalog_http.c/
 * profiles_edit_http.c together, all three reaching this same header once
 * thanks to its own `#ifndef` guard), share one definition with zero
 * duplicate-symbol risk either way -- no macro guard needed here, unlike the
 * three now-removed per-file copies. backup_export.c, kiln_cfg_http.c and
 * diagnostics_http.c keep their own separate copies -- they sit outside this
 * split's three-file TU and widening into this header is out of this fix's
 * scope.
 *
 * NAMED profiles_http_json_escape(), NOT json_escape(): dashboard_json.h
 * (included by profiles_live_http.c, among others) already declares a
 * project-wide non-static `void json_escape(...)` with the OLDER, narrower
 * escaping (quote/backslash only -- the same gap this fix closes, but out of
 * scope to widen here per the review). Reusing the bare name `json_escape`
 * for this header's `static inline` triggered "static declaration follows
 * non-static declaration" the moment any TU pulled in both headers
 * (profiles_live_http.c does) -- a real conflict discovered while building
 * target KilnFW, not a hypothetical. A distinct name sidesteps it entirely
 * and is more honest anyway: this is not a drop-in replacement for the
 * dashboard-wide helper, just the shared escaper for this split's three
 * profiles_*_http.c files. */
static inline void profiles_http_json_escape(const char *src, char *out, size_t out_cap)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 1 < out_cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            if (o + 2 >= out_cap) break;
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            if (o + 6 >= out_cap) break;
            int n = snprintf(&out[o], 7, "\\u%04x", c);
            if (n != 6) break;
            o += 6;
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

/* Opus review nit N5: profiles_http_save() (profiles_http.c) and
 * backup_import.c's pass-1 commit simulation each used to scan for the
 * lowest unused slot id with their own independent copy of the same
 * "for id in 0..PROFILES_MAX_COUNT, first !used wins" loop -- correct in
 * both places today, but two independent copies of one allocation policy
 * are exactly the shape that drifts silently (one gets fixed, edited, or
 * widened -- e.g. a future "prefer the id most recently freed" policy --
 * and the other doesn't). Factored into one shared helper so they cannot
 * diverge.
 *
 * Takes a predicate callback, not a caller-materialized `bool[PROFILES_MAX_
 * COUNT]` array: an earlier version of this helper took the array, but
 * profiles_http_save() is reached from bx_flash_worker's dispatch
 * (check_all_task_stack_budgets.ps1), and that extra PROFILES_MAX_COUNT-byte
 * local pushed it 32 B over that task's ceiling. A callback lets
 * profiles_http.c answer straight from profiles_slot_used() with no local
 * buffer at all, while backup_import.c's caller still answers from its own
 * SIMULATED table (pass 2's commit order marks ids used as it goes, not the
 * board's live state) via a small adapter closure. Returns the lowest id
 * for which is_used(ctx, id) is false, or -1 if every slot is taken. */
static inline int profiles_http_first_free_slot(bool (*is_used)(void *ctx, uint8_t id), void *ctx)
{
    for (uint8_t id = 0; id < PROFILES_MAX_COUNT; id++) {
        if (!is_used(ctx, id)) {
            return (int)id;
        }
    }
    return -1;
}

/* ---- shared log tag ------------------------------------------------------ */
extern const char *PROFILES_TAG;

/* ---- validation bounds (profiles_http.c) ---------------------------------
 * Compile-time constants, not linkage -- duplicated here (rather than left
 * `static`/file-local) purely so profiles_edit_http.c's parse_profile_
 * fields()/profile_post_handler() can enforce the exact same bounds
 * profiles_http.c's own profiles_http_save()/profiles_http_get_bounds()
 * answer. Values must never diverge between the two -- see
 * PROFILE_TARGET_C_MAX's own doc comment in profiles_http.c for why 2015.0f
 * is exact, not rounded. */
#define PROFILE_TARGET_C_MIN 0.0f
#define PROFILE_TARGET_C_MAX 2015.0f
#define PROFILE_RAMP_C_PER_HR_MIN 0.0f
#define PROFILE_RAMP_C_PER_HR_MAX 1000.0f
#define PROFILE_DWELL_MIN_MAX 1440u /* 24h */
#define PROFILE_RAMP_WARN_FRACTION 0.8f

/* application/x-www-form-urlencoded whole-profile submit body cap --
 * profiles_edit_http.c's profile_post_handler() is the only reader. */
#define PROFILE_BODY_MAX 2048

/* Per-on/off-rule JSON object byte budget, shared by every writer that
 * builds one of these objects into a fixed-capacity buffer
 * (profiles_catalog_http.c's PROFILE_DETAIL_JSON_CAP and
 * profiles_export_http.c's export cap) -- a rule object with temp_source
 * measured 218 bytes worst case (widened 128 -> 224, Opus review pass).
 * Distinct from PROFILE_LIST_ENTRY_MAX (profiles_catalog_http.c), which
 * bounds a whole profile-list entry, not one rule object; both currently
 * happen to be 224 but for unrelated reasons and must not be assumed to
 * track each other. */
#define PROFILE_ON_OFF_RULE_JSON_MAX 224

/* ---- shared profile-slot storage (profiles_http.c) ------------------------
 * All 8 slots kept resident -- see profiles_http.c's own doc comment on
 * profiles_state_t for why. Read by profiles_catalog_http.c's listing/detail
 * handlers and mutated by profiles_edit_http.c's post/delete handlers.
 *
 * `used_bitmap` widened uint8_t -> profiles_slot_bitmap_t (docs/
 * PROFILE_SLOTS_100.md section 7 task 1) so an id past 7 (up to the
 * 128-id ceiling section 2 of that plan documents) can be addressed once
 * PROFILES_MAX_COUNT is later raised -- still 8 today, so behavior and the
 * persisted NVS byte are unchanged. Every reader/writer goes through
 * profiles_slot_used()/profiles_slot_set()/profiles_slot_clear() below
 * rather than testing `.words[]` directly. */
typedef struct {
    profile_t profiles[PROFILES_MAX_COUNT];
    profiles_slot_bitmap_t used_bitmap;
} profiles_state_t;

/* docs/PROFILE_SLOTS_100.md section 7 task 3: this struct (dominated by
 * profiles[PROFILES_MAX_COUNT], and growing further once task 6 raises
 * PROFILES_MAX_COUNT) is lazily allocated from PSRAM rather than reserved as
 * a .bss global -- see profiles_storage_ensure()'s doc comment in
 * profiles_http.c (which owns the allocation and its internal-RAM/empty-
 * fallback path) for the full rationale. `s_profiles` stays usable exactly
 * as before EVERYWHERE it already appears (`s_profiles.foo`,
 * `&s_profiles`, `memset(&s_profiles, 0, sizeof(s_profiles))`) because this
 * macro expands to a dereference of the lazily-allocated pointer, not to a
 * plain global -- it is intentionally NOT `extern profiles_state_t
 * s_profiles;` any more. */
profiles_state_t *profiles_storage_ensure(void);
#define s_profiles (*profiles_storage_ensure())

/* Accessors for s_profiles.used_bitmap -- the ONLY sanctioned way to test/
 * set/clear a slot's used bit. Defined in profiles_http.c (which owns
 * s_profiles); declared here so profiles_catalog_http.c and
 * profiles_edit_http.c (the other two files this header seams together)
 * use the exact same bit logic rather than re-deriving it. `id` may be
 * anything up to PROFILES_SLOT_BITMAP_MAX_ID -- an id at or past
 * PROFILES_MAX_COUNT is simply never set, not a caller error, matching the
 * old scalar's behavior of silently reading 0 past its own width. */
bool profiles_slot_used(uint8_t id);
void profiles_slot_set(uint8_t id);
void profiles_slot_clear(uint8_t id);

/* ---- shared on-flash blob encode/decode (profiles_http.c) -----------------
 * Widened non-static (2026-09-07, docs/FILESYSTEM_USER_DATA_PLAN.md section 5
 * step 4, user-profiles filesystem move) so profiles_cfg_fs.c's read-through/
 * dual-write bridge can validate and produce a `cfg`-partition file using the
 * EXACT SAME version-1/2/3 migration chain and CRC check the NVS path already
 * uses -- one decoder, two storage backends, never two parsers to keep in
 * sync. See profiles_http.c's decode_profile_blob()/PROFILE_VERSION comment
 * for the full version history and the "growing profile_segment_t itself"
 * hazard this discipline exists to avoid repeating. */
typedef enum {
    PROFILE_DECODE_OK,      /* *out is a valid, current-format profile_t, ready to adopt */
    PROFILE_DECODE_CORRUPT, /* reject outright: version 0, wrong length for the claimed
                             * version, unknown version, or CRC mismatch -- *out is
                             * zeroed, nothing is adopted */
    PROFILE_DECODE_NEWER,   /* version > PROFILE_VERSION -- refuse without guessing;
                             * *out is zeroed, but the caller must leave the SOURCE
                             * bytes untouched */
} profile_decode_result_t;

/* Generous upper bound on the encoded (version-prefixed, CRC-tailed) blob
 * size for ANY version this build knows about, current version included --
 * sized off sizeof(profile_t) rather than the private profile_persisted_t
 * struct (defined in profiles_http.c only) so a caller outside that file
 * never needs to know the on-flash wrapper's exact layout, only a safe
 * buffer size to allocate. 32 bytes of headroom covers the version + crc32
 * tail (5 bytes) with room to spare for a future small header field. */
#define PROFILE_BLOB_MAX_SIZE (sizeof(profile_t) + 32u)

/* The one place a stored profile blob (NVS OR a `cfg`-partition file) is
 * turned into a trustworthy, current-format profile_t. `blob` must point at
 * the version byte (offset 0 of the on-flash wrapper); a `cfg` file caller
 * (profiles_cfg_fs.c) passes its own bytes starting after its 4-byte rev
 * prefix. See profiles_http.c's definition for the full per-version decode
 * path (CRC check for v2/v3, typed field-by-field conversion for v1/v2). */
profile_decode_result_t profile_decode_blob(const void *blob, size_t len, profile_t *out, const char **err_reason);

/* Encodes `profile` into the CURRENT-version on-flash wrapper (version tag +
 * fields + freshly-recomputed CRC) into `out` (capacity `cap`). Returns the
 * number of bytes written, or 0 if `cap` is too small (nothing is written in
 * that case) -- mirrors zones_config_json_encode-style helpers' "0 means
 * failed" convention used elsewhere in this codebase. */
size_t profile_encode_current_blob(const profile_t *profile, void *out, size_t cap);

/* ---- persisted per-slot rev counters (profiles_http.c) --------------------
 * PROFILES_MAX_COUNT uint32_t revs, bumped on BOTH nvs_save_slot() and
 * nvs_erase_slot() (a delete is a mutation too) and persisted under
 * NVS_KEY_PROFILE_REV so profiles_cfg_fs_resolve() can tell "the file is
 * ahead because the NVS write half of a save failed" apart from "this slot
 * was legitimately deleted after the file was written" -- see
 * profiles_cfg_fs.h's header comment. Exposed here (rather than kept purely
 * static) only because test_profiles_cfg_fs.c inspects it directly to set up
 * fixtures without going through the full NVS load path. */
extern uint32_t s_profile_rev[PROFILES_MAX_COUNT];

/* ---- profiles_http.c -------------------------------------------------------
 * NVS persistence primitives -- profiles_catalog_http.c never calls these
 * (read-only), profiles_edit_http.c's post/delete/builtin-hide handlers do. */
esp_err_t nvs_save_slot(uint8_t id);
esp_err_t nvs_erase_slot(uint8_t id);

/* True iff some ZONE_RAMP segment's target_c exceeds the CURRENTLY
 * configured max_temp_c of one of its zone_mask zones -- advisory-only
 * check shared by the list/detail JSON (profiles_catalog_http.c) and the
 * POST /api/profile response (profiles_edit_http.c). See its definition in
 * profiles_http.c for the full "why this warns instead of refusing"
 * rationale. */
bool profile_exceeds_zone_ceiling(const profile_t *p, char *note, size_t note_cap);

/* docs/ON_OFF_ZONE_PLAN.md plan step 5 -- rejects a candidate profile whose
 * on/off rules reference a nonexistent segment or a zone not typed
 * ZONE_TYPE_ON_OFF (the dangerous direction: a HEATER zone driven by on/off
 * logic), or whose numeric fields are out of bounds. Called by
 * profiles_http_save() for both the HTTP POST and UART-bridge entry points;
 * exposed here so profiles_edit_http.c/profiles_export_http.c can also
 * validate a freshly-parsed candidate before it ever reaches save (same
 * "fail fast, at the edge" shape validate_io_segment() already follows). */
bool validate_on_off_rules(const profile_t *candidate, char *err_msg, size_t err_cap);

/* Validates one RELAY_IO segment's target/flags -- shared by
 * profiles_http.c's own profiles_http_save() (the UART-bridge entry point)
 * and profiles_edit_http.c's parse_profile_fields() (the HTTP POST entry
 * point). See its definition in profiles_http.c for the owner's IO-target
 * design rule. */
bool validate_io_segment(const profile_segment_t *seg, uint8_t seg_num, char *err_msg, size_t err_cap);

/* Widened non-`static` (docs/LIVE_PROFILE_EDIT_PLAN.md section 8 item 1) so
 * the live-edit handler decodes the identical x-www-form-urlencoded shape
 * profiles_edit_http.c's own POST /api/profile does -- two decoders would
 * drift on the first new field. Renamed from the original `static
 * parse_profile_fields` per the CLAUDE.md rule on widening a `static` (a
 * prefix rename even when the repo-wide grep for the bare name is clean). */
bool profiles_parse_profile_fields(const char *body, profile_t *p, char *err_msg, size_t err_cap);

/* Selects which rule profiles_validate_candidate() applies to a ZONE_RAMP
 * segment's target_c against its zone's CURRENTLY configured max_temp_c
 * (docs/LIVE_PROFILE_EDIT_PLAN.md section 7). PROFILE_VALIDATE_ADVISORY is
 * today's save-time behavior (profile_exceeds_zone_ceiling(): a profile
 * exceeding the live ceiling still saves, with a warning -- profiles are
 * portable between kilns, and only *starting* one enforces the ceiling).
 * PROFILE_VALIDATE_HARD is the live-edit rule: a target above the zone's
 * max_temp_c, or a ramp above the zone's max_ramp_c_per_hr, or a zone with
 * max_temp_c == 0 (uncommissioned -- no ceiling to check against, and a
 * missing ceiling must never read as an infinite one), is refused outright.
 * The two modes share every other check (segment_count, target/ramp/dwell
 * bounds, IO-segment validation, on/off-rule validation) identically. */
typedef enum {
    PROFILE_VALIDATE_ADVISORY = 0, /* save-time: over-ceiling warns, does not refuse */
    PROFILE_VALIDATE_HARD = 1,     /* live-edit accept/pickup: over-ceiling refuses */
} profile_validate_mode_t;

/* LOW (review, 2026-09-19): profile_executor.c cannot #include this header
 * (it pulls in esp_http_server.h, which is not host-test-safe, and
 * test_profile_executor_prestart.c #includes profile_executor.c directly as
 * a host-only translation unit) -- it declares its own host-safe
 * profile_validate_mode_local_t/extern instead, matching this real
 * enum/prototype by hand. That duplication cannot be reached by the
 * compiler across the two TUs, so pin the one thing that actually has to
 * stay in sync -- the numeric values -- with a static assert here, in the
 * TU that owns the real enum. If this ever fails, profile_executor.c's
 * PROFILE_VALIDATE_HARD_LOCAL must be updated to match. */
_Static_assert(PROFILE_VALIDATE_ADVISORY == 0, "profile_executor.c's local profile_validate_mode_local_t duplicate is out of sync");
_Static_assert(PROFILE_VALIDATE_HARD == 1, "profile_executor.c's local profile_validate_mode_local_t duplicate is out of sync");

/* The one place a candidate profile_t is checked before it is written
 * anywhere. Called by profile_post_handler() (mode ADVISORY, unchanged
 * behavior), the live-edit POST handler (mode HARD, docs/LIVE_PROFILE_EDIT_PLAN.md
 * section 7), and the executor's pickup re-check (mode HARD, against the
 * *live* zone config, under s_exec.lock) -- three call sites, one rule.
 * Reads zone config; touches no httpd state, so it host-tests directly
 * without a request context. `warnings_json` (may be NULL to skip) receives
 * a JSON array of advisory warning strings (ramp within 20% of a zone
 * ceiling, or -- ADVISORY mode only -- a target above the current zone
 * ceiling); `err_msg`/`err_cap` receive the refusal reason on a `false`
 * return. Returns false on the first hard violation encountered (segment
 * index named in `err_msg`); a HARD-mode over-ceiling target/ramp is such a
 * violation, an ADVISORY-mode one is not (it becomes a warning instead). */
bool profiles_validate_candidate(const profile_t *candidate, profile_validate_mode_t mode, char *warnings_json,
                                  size_t warnings_json_cap, char *err_msg, size_t err_cap);

/* ---- profiles_catalog_http.c ----------------------------------------------
 * Registered by profiles_http_start() in profiles_http.c. */
esp_err_t profiles_page_get_handler(httpd_req_t *req);
esp_err_t profiles_list_get_handler(httpd_req_t *req);
esp_err_t profile_detail_get_handler(httpd_req_t *req);
esp_err_t builtin_list_get_handler(httpd_req_t *req);
esp_err_t favorites_list_get_handler(httpd_req_t *req);

/* ---- profiles_edit_http.c --------------------------------------------------
 * Registered by profiles_http_start() in profiles_http.c. */
esp_err_t profile_post_handler(httpd_req_t *req);
esp_err_t profile_delete_post_handler(httpd_req_t *req);
esp_err_t builtin_hide_post_handler(httpd_req_t *req);
esp_err_t builtin_restore_post_handler(httpd_req_t *req);
esp_err_t profile_favorite_post_handler(httpd_req_t *req);

#endif /* PROFILES_HTTP_INTERNAL_H */
