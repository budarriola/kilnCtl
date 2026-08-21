// profile_feasibility -- "can this kiln actually do that?" for a firing
// schedule, answered from the FOPDT plant model autotune fitted for a zone
// (zones_config_get_model()) rather than from a number the operator typed.
//
// This is the "mark the schedule red if the tuning shows it cannot be done"
// check. It is ADDITIONAL to, not a replacement for, the max_ramp_c_per_hr
// ceiling profiles_http.c enforces at save time and profile_executor.c
// re-checks at start: that ceiling is a user-entered policy limit ("do not
// ramp my kiln faster than this"), while this module answers a physics
// question ("this kiln cannot ramp that fast whatever you ask of it").
// A segment over the configured ceiling is reported TOO_FAST here as well, so
// a single verdict per segment can drive a single colour in the UI.
//
// HONESTY RULE, and the reason this enum has four values instead of a bool:
// a zone that has never been autotuned has no model (zones_http.c encodes
// that as zeros in all three model fields). The verdict for such a zone is
// UNKNOWN -- never OK, and never red. Not knowing is not the same as being
// fine, and a red mark that really meant "untuned" would appear on every
// schedule of every fresh board and teach the operator to ignore the colour
// that exists to stop a ruined firing.
#ifndef PROFILE_FEASIBILITY_H
#define PROFILE_FEASIBILITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "profiles_http.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROFILE_SEG_OK = 0,      /* the model says this segment is achievable */
    PROFILE_SEG_UNKNOWN,     /* no model for this zone -- cannot answer, do not guess */
    PROFILE_SEG_TOO_FAST,    /* target is reachable, but not at the commanded rate */
    PROFILE_SEG_UNREACHABLE, /* target is above the kiln's steady-state ceiling at full power */
} profile_seg_verdict_t;

/* Verdict for one segment, starting from start_c (the temperature the kiln is
 * at when this segment begins -- for segment 0 of a profile that is ambient,
 * thereafter it is the previous segment's target). */
profile_seg_verdict_t profile_feasibility_segment(uint8_t zone_index, float start_c,
                                                  const profile_segment_t *seg);

/* Whole-profile roll-up for one zone: walks the segments in order carrying the
 * running start temperature, and returns the WORST verdict seen, ordered
 * UNREACHABLE > TOO_FAST > UNKNOWN > OK. UNKNOWN outranks OK so that a profile
 * containing even one unanswerable segment never reports a clean bill of
 * health, but ranks below the two real failures so that a genuine problem is
 * not masked by an untuned neighbour. Also writes each segment's own verdict
 * to out_segments[0..segment_count-1] if non-NULL. */
profile_seg_verdict_t profile_feasibility_profile(uint8_t zone_index, const profile_t *p,
                                                  profile_seg_verdict_t *out_segments,
                                                  size_t out_cap);

/* Same roll-up across a zone_mask (every participating zone must be able to do
 * it, matching profiles_http.c's existing multi-zone ceiling rule). A mask of
 * 0 -- which is what a zone-agnostic builtin catalogue entry carries -- means
 * "every configured zone". */
profile_seg_verdict_t profile_feasibility_profile_mask(uint8_t zone_mask, const profile_t *p,
                                                       profile_seg_verdict_t *out_segments,
                                                       size_t out_cap);

/* Stable lowercase token for JSON/UI: "ok" | "unknown" | "too_fast" |
 * "unreachable". Never NULL. */
const char *profile_feasibility_verdict_str(profile_seg_verdict_t v);

#ifdef __cplusplus
}
#endif

#endif // PROFILE_FEASIBILITY_H
