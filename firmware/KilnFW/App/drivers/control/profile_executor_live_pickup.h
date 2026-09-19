/* profile_executor_live_pickup.h -- pure pickup logic for
 * docs/LIVE_PROFILE_EDIT_PLAN.md pass 1, section 3/7/11.
 *
 * Deliberately free of FreeRTOS/s_exec so host tests can call it directly
 * (plan section 11: "written pure ... so it is directly callable"). The
 * caller (profile_executor.c's tick, under s_exec.lock, mirroring
 * reload_config_if_changed()'s own placement/locking) is responsible for:
 *   - polling live_profile_generation() once per tick and only calling this
 *     when it has moved;
 *   - loading the candidate via live_profile_load_working();
 *   - supplying the RUNNING profile's own copy (s_exec.profile) and
 *     s_exec.segment_index unchanged;
 *   - on PROFILE_LIVE_PICKUP_OK, replacing s_exec.profile's CONTENT with the
 *     candidate and leaving segment_index/segment_elapsed_s/io_segs/dwelling/
 *     every firing-stats accumulator untouched -- continuity is the whole
 *     point (plan section 1's "the running segment keeps running").
 */
#ifndef PROFILE_EXECUTOR_LIVE_PICKUP_H
#define PROFILE_EXECUTOR_LIVE_PICKUP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "profiles_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROFILE_LIVE_PICKUP_OK = 0,
    PROFILE_LIVE_PICKUP_REFUSED_WINDOW,  /* live_edit_check_window() refused */
    PROFILE_LIVE_PICKUP_REFUSED_INVALID, /* HARD re-validation refused (plan section 7) */
} profile_live_pickup_result_t;

/* HARD-mode re-validates `candidate` against the zone config exactly as
 * profiles_validate_candidate(..., PROFILE_VALIDATE_HARD, ...) would (the
 * SAME validator new-profile creation uses -- plan's "one implementation"
 * rule), via the caller-supplied `validate_hard` seam so this stays pure and
 * host-testable without pulling in zones_config_accessors.h. Runs the
 * edit-window check FIRST (cheaper, no config lookups) so a structurally
 * illegal edit is refused before spending a validation pass on it. */
profile_live_pickup_result_t profile_executor_live_pickup_check(
    const profile_t *running, const profile_t *candidate, uint8_t segment_index,
    bool (*validate_hard)(void *ctx, const profile_t *candidate, char *err_msg, size_t err_cap), void *validate_ctx,
    char *err_msg, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* PROFILE_EXECUTOR_LIVE_PICKUP_H */
