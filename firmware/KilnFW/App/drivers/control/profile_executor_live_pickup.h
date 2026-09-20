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

/* HIGH-1 (review): what a tick's live-edit poll actually managed to do this
 * time, so the pure decision below can tell an "adopt or definitively
 * refused" outcome apart from every "nothing happened yet, try again" one.
 * profile_executor.c's reload_live_profile_if_changed() reaches exactly one
 * of these every time live_profile_generation() has moved. */
typedef enum {
    /* s_exec.state != PROFILE_EXEC_RUNNING -- an edit made while PAUSED/
     * FAULTED is left for the NEXT tick that finds RUNNING (plan owner
     * decision 4: editing is allowed while paused, it just doesn't apply
     * until resumed). Must not be consumed now, or it is lost forever the
     * moment the run resumes and this same generation is never seen as new
     * again. */
    PROFILE_LIVE_PICKUP_POLL_NOT_RUNNING,
    /* heap_caps_malloc() failed -- transient, retry next tick. */
    PROFILE_LIVE_PICKUP_POLL_MALLOC_FAILED,
    /* live_profile_load_working_for_origin() returned false: nothing
     * pending, the pending record isn't for this run (MEDIUM-1), or the
     * blob failed to decode. None of these is a definitive answer about
     * THIS run's still-outstanding edit (there may be no such edit at all),
     * so none of them may consume the generation either. */
    PROFILE_LIVE_PICKUP_POLL_NOT_APPLICABLE,
    /* The window/HARD-validate check actually ran -- see .result for
     * PROFILE_LIVE_PICKUP_OK vs one of the two REFUSED_* reasons. Both a
     * REFUSED_* and OK are definitive: the operator's specific candidate
     * was actually looked at and answered, so the generation may safely
     * advance either way (see profile_live_pickup_should_advance_
     * generation() below and MEDIUM-3's last_refusal recording). */
    PROFILE_LIVE_PICKUP_POLL_CHECKED,
} profile_live_pickup_poll_outcome_kind_t;

/* HIGH-1 (review): true iff s_exec.live_edit_generation may be advanced to
 * the newly-observed generation this tick. Only PROFILE_LIVE_PICKUP_POLL_
 * CHECKED ever returns true here -- and then unconditionally, whether the
 * candidate was adopted (result == OK) or definitively refused (a REFUSED_*
 * result, which the caller is expected to have already recorded for the
 * operator, MEDIUM-3) -- since either way the operator's specific edit was
 * actually evaluated and does not need re-evaluating on an unchanged
 * generation. NOT_RUNNING/MALLOC_FAILED/NOT_APPLICABLE all return false:
 * none of them looked at the candidate at all, so none of them may consume
 * the generation -- doing so was the HIGH-1 defect (an edit made while
 * PAUSED, or one that transiently failed to load, was silently marked
 * "seen" and never revisited). Pure and host-testable on its own, since the
 * real bug was in this decision's SHAPE, not in any one branch's plumbing. */
bool profile_live_pickup_should_advance_generation(profile_live_pickup_poll_outcome_kind_t kind,
                                                    profile_live_pickup_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* PROFILE_EXECUTOR_LIVE_PICKUP_H */
