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
    /* HIGH (review, 2026-09-19): live_profile_load_working_for_origin()
     * returned LIVE_PROFILE_LOAD_NONE_FOR_ORIGIN -- no record at all, the
     * record isn't pending, or it is pending for a DIFFERENT run's origin_id
     * (MEDIUM-1). Unlike the old bool-returning API this collapsed with, all
     * three are a DEFINITIVE fact about the persisted state (there is
     * nothing for THIS run's origin_id to ever adopt right now), not a
     * "try again" answer, so this DOES consume the generation -- see
     * PROFILE_LIVE_PICKUP_POLL_LOAD_TRANSIENT just below for the one load
     * outcome that must not. Collapsing these together with that one used to
     * be the bug: a live_profile_clear() (discard/save-as/overwrite) bumps
     * the generation while leaving no pending record behind, and if that is
     * never consumed, every future tick forever re-observes the same bump as
     * "new" and re-spends a malloc + blocking NVS read on the control task
     * for an edit that will never exist to adopt. */
    PROFILE_LIVE_PICKUP_POLL_NOT_APPLICABLE,
    /* Pass-3 review fix (2026-09-19): live_profile_load_working_for_origin()
     * returned LIVE_PROFILE_LOAD_PERMANENT -- a record IS pending for this
     * run's origin_id and hal_kv_open() itself succeeded, but the blob's
     * CONTENT can never be adopted (missing, wrong length, or a decode
     * failure). Unlike LOAD_TRANSIENT just below, this will not change on
     * retry, so it DOES consume the generation -- kept as its own kind
     * (rather than folded into NOT_APPLICABLE) only so the caller can
     * log/record a refusal distinctly from the ordinary "nothing pending"
     * case. */
    PROFILE_LIVE_PICKUP_POLL_LOAD_PERMANENT,
    /* HIGH (review, 2026-09-19): live_profile_load_working_for_origin()
     * returned LIVE_PROFILE_LOAD_TRANSIENT -- a record IS pending for this
     * run's origin_id, but the working blob itself failed to load (hal_kv
     * open error, blob decode failure). This is NOT a definitive answer
     * about the edit (it may well load fine next tick), so -- unlike
     * NOT_APPLICABLE just above -- it must NOT consume the generation. */
    PROFILE_LIVE_PICKUP_POLL_LOAD_TRANSIENT,
    /* The window/HARD-validate check actually ran -- see .result for
     * PROFILE_LIVE_PICKUP_OK vs one of the two REFUSED_* reasons. Both a
     * REFUSED_* and OK are definitive: the operator's specific candidate
     * was actually looked at and answered, so the generation may safely
     * advance either way (see profile_live_pickup_should_advance_
     * generation() below and MEDIUM-3's last_refusal recording). */
    PROFILE_LIVE_PICKUP_POLL_CHECKED,
} profile_live_pickup_poll_outcome_kind_t;

/* HIGH-1 (review): true iff s_exec.live_edit_generation may be advanced to
 * the newly-observed generation this tick. PROFILE_LIVE_PICKUP_POLL_CHECKED
 * returns true unconditionally, whether the candidate was adopted
 * (result == OK) or definitively refused (a REFUSED_* result, which the
 * caller is expected to have already recorded for the operator, MEDIUM-3) --
 * since either way the operator's specific edit was actually evaluated and
 * does not need re-evaluating on an unchanged generation.
 * PROFILE_LIVE_PICKUP_POLL_NOT_APPLICABLE (HIGH, 2026-09-19) and
 * PROFILE_LIVE_PICKUP_POLL_LOAD_PERMANENT (pass-3 review fix, 2026-09-19)
 * ALSO return true: both are definitive outcomes -- nothing exists for this
 * run's origin_id to ever adopt for this generation (NOT_APPLICABLE), or
 * something exists but can never be loaded (LOAD_PERMANENT) -- so there is
 * nothing left to re-check either way.
 * NOT_RUNNING/MALLOC_FAILED/LOAD_TRANSIENT all return false: none of them
 * looked at (or could even find) the candidate, so none of them may consume
 * the generation -- doing so was the original HIGH-1 defect (an edit made
 * while PAUSED, or one that transiently failed to load, was silently marked
 * "seen" and never revisited), and collapsing LOAD_TRANSIENT together with
 * NOT_APPLICABLE under the old bool-returning API was the same defect
 * recurring one layer down (2026-09-19 fix). Pure and host-testable on its
 * own, since the real bug was in this decision's SHAPE, not in any one
 * branch's plumbing. */
bool profile_live_pickup_should_advance_generation(profile_live_pickup_poll_outcome_kind_t kind,
                                                    profile_live_pickup_result_t result);

/* Pass-3 review fix (2026-09-19): how many of s_exec.io_segs[] an adopt-time
 * dwell re-derivation pass may safely touch. profile_executor.c's reload_
 * live_profile_if_changed() reads BOTH the OLD profile's segments (for
 * elapsed_s) and the NEW candidate's segments (for the new dwell), so it
 * must be bounded by the SMALLER of the two segment_counts -- a live edit
 * that legally shortens the profile (live_edit_check_window() allows
 * segment_count down to segment_index + 1) could otherwise index the
 * candidate's segments[] array past its own segment_count. Extracted as its
 * own pure function (rather than left inline) specifically so this bound is
 * host-testable on its own, since the original defect was exactly a wrong
 * bound with no test to catch it. */
uint8_t profile_live_pickup_io_seg_rederive_count(uint8_t old_segment_count, uint8_t new_segment_count,
                                                   uint8_t max_segments);

/* Pass-3 review fix (2026-09-19): the pure arithmetic behind that same
 * adopt-time re-derivation, extracted so it is host-testable without
 * executor plumbing. Given an active NON-BLOCKING IO/relay segment's OLD
 * dwell_min and its current countdown (old_remaining_s), and the NEW
 * candidate's dwell_min for that same segment index, returns the countdown
 * that should replace old_remaining_s so the segment's elapsed time (under
 * its OLD dwell) carries over against the NEW dwell. Clamped to
 * [0, new_dwell_min*60] at both ends -- a stale/negative old_remaining_s
 * (should not happen, but this is defensive) or a new dwell shorter than
 * what has already elapsed both floor at 0. */
float profile_live_pickup_rederive_remaining_s(uint32_t old_dwell_min, float old_remaining_s, uint32_t new_dwell_min);

#ifdef __cplusplus
}
#endif

#endif /* PROFILE_EXECUTOR_LIVE_PICKUP_H */
