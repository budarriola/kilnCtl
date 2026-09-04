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

#include "esp_err.h"
#include "esp_http_server.h"

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

/* ---- shared profile-slot storage (profiles_http.c) ------------------------
 * All 8 slots kept resident -- see profiles_http.c's own doc comment on
 * profiles_state_t for why. Read by profiles_catalog_http.c's listing/detail
 * handlers and mutated by profiles_edit_http.c's post/delete handlers. */
typedef struct {
    profile_t profiles[PROFILES_MAX_COUNT];
    uint8_t used_bitmap; /* bit N = slot N in use */
} profiles_state_t;

extern profiles_state_t s_profiles;

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

/* Validates one RELAY_IO segment's target/flags -- shared by
 * profiles_http.c's own profiles_http_save() (the UART-bridge entry point)
 * and profiles_edit_http.c's parse_profile_fields() (the HTTP POST entry
 * point). See its definition in profiles_http.c for the owner's IO-target
 * design rule. */
bool validate_io_segment(const profile_segment_t *seg, uint8_t seg_num, char *err_msg, size_t err_cap);

/* ---- profiles_catalog_http.c ----------------------------------------------
 * Registered by profiles_http_start() in profiles_http.c. */
esp_err_t profiles_page_get_handler(httpd_req_t *req);
esp_err_t profiles_list_get_handler(httpd_req_t *req);
esp_err_t profile_detail_get_handler(httpd_req_t *req);
esp_err_t builtin_list_get_handler(httpd_req_t *req);

/* ---- profiles_edit_http.c --------------------------------------------------
 * Registered by profiles_http_start() in profiles_http.c. */
esp_err_t profile_post_handler(httpd_req_t *req);
esp_err_t profile_delete_post_handler(httpd_req_t *req);
esp_err_t builtin_hide_post_handler(httpd_req_t *req);
esp_err_t builtin_restore_post_handler(httpd_req_t *req);

#endif /* PROFILES_HTTP_INTERNAL_H */
