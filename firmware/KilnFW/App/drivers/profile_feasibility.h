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

/* autotune_method_t's AUTOTUNE_METHOD_STEP (autotune_engine.h) as a literal,
 * so profile_feasibility.c does not have to pull in that header's whole
 * state-machine/FreeRTOS-handle surface for one enumerator. Pinned to the
 * real enumerator by a portable compile-time assert in
 * autotune_engine_guard.c (the one .c that already includes both
 * autotune_engine.h, transitively via autotune_engine_internal.h, and this
 * header) -- if the two ever drift the build fails instead of
 * get_persisted_ambient_c() silently misreading RELAY-method records as
 * STEP or vice versa. */
#define FEASIBILITY_TUNING_METHOD_STEP 0u

typedef enum {
    PROFILE_SEG_OK = 0,      /* the model says this segment is achievable */
    PROFILE_SEG_UNKNOWN,     /* no model for this zone -- cannot answer, do not guess */
    PROFILE_SEG_TOO_FAST,    /* target is reachable, but not at the commanded rate */
    PROFILE_SEG_UNREACHABLE, /* target is above the kiln's steady-state ceiling at full power */
} profile_seg_verdict_t;

/* Verdict for one segment, starting from start_c (the temperature the kiln is
 * at when this segment begins -- for segment 0 of a profile that is ambient,
 * thereafter it is the previous segment's target).
 *
 * Solo-zone entry point: judges zone_index as if it alone were firing, with
 * no coupling help from any neighbour. Production code (profiles_http.c,
 * dashboard_http.c) goes through profile_feasibility_segment_in_mask()/
 * profile_feasibility_profile_mask() instead, since a real run always has a
 * zone_mask naming every zone actually driven; this entry point remains for
 * tests that want the pre-coupling solo behaviour directly. */
profile_seg_verdict_t profile_feasibility_segment(uint8_t zone_index, float start_c,
                                                  const profile_segment_t *seg);

/* Same, but judged as part of a run that drives every zone in zone_mask at
 * once. The zones share a chamber, so a zone's reachable ceiling with its
 * neighbours firing alongside it is higher than the solo k_dc autotune fitted
 * for it: this variant adds the persisted coupling matrix's contribution from
 * the OTHER zones in the mask (see effective_k_dc() in the .c) and uses that
 * effective gain for both the ceiling test and the heating-headroom rate
 * test. Cooling is unaffected -- no zone helps another cool. A mask naming
 * only zone_index is exactly profile_feasibility_segment() above; an absent
 * or zero coupling matrix likewise reduces to it. If zone_mask does not even
 * include zone_index, that zone is not being driven at all and the verdict
 * is UNKNOWN. */
profile_seg_verdict_t profile_feasibility_segment_in_mask(uint8_t zone_index, uint8_t zone_mask,
                                                          float start_c,
                                                          const profile_segment_t *seg);

/* Whole-profile roll-up for one zone: walks the segments in order carrying the
 * running start temperature, and returns the WORST verdict seen, ordered
 * UNREACHABLE > TOO_FAST > UNKNOWN > OK. UNKNOWN outranks OK so that a profile
 * containing even one unanswerable segment never reports a clean bill of
 * health, but ranks below the two real failures so that a genuine problem is
 * not masked by an untuned neighbour. Also writes each segment's own verdict
 * to out_segments[0..segment_count-1] if non-NULL.
 *
 * Solo-zone entry point, same relationship to profile_feasibility_profile_in_mask()
 * as profile_feasibility_segment() has to profile_feasibility_segment_in_mask()
 * above: production goes through the _in_mask/_mask variants, this one is
 * kept for tests. */
profile_seg_verdict_t profile_feasibility_profile(uint8_t zone_index, const profile_t *p,
                                                  profile_seg_verdict_t *out_segments,
                                                  size_t out_cap);

/* Whole-profile roll-up for one zone, judged as part of a multi-zone run --
 * profile_feasibility_segment_in_mask() applied over the segment walk. */
profile_seg_verdict_t profile_feasibility_profile_in_mask(uint8_t zone_index, uint8_t zone_mask,
                                                          const profile_t *p,
                                                          profile_seg_verdict_t *out_segments,
                                                          size_t out_cap);

/* Same roll-up across a zone_mask (every participating zone must be able to do
 * it, matching profiles_http.c's existing multi-zone ceiling rule). A mask of
 * 0 -- which is what a zone-agnostic builtin catalogue entry carries -- means
 * "every configured zone", for the coupling contribution as well as for the
 * set of zones judged. Each zone is judged with the coupling help of the
 * other zones in the (resolved) mask, since they are all being driven. */
profile_seg_verdict_t profile_feasibility_profile_mask(uint8_t zone_mask, const profile_t *p,
                                                       profile_seg_verdict_t *out_segments,
                                                       size_t out_cap);

/* Stable lowercase token for JSON/UI: "ok" | "unknown" | "too_fast" |
 * "unreachable". Never NULL. */
const char *profile_feasibility_verdict_str(profile_seg_verdict_t v);

/* ---- Planned-curve duration model ------------------------------------------
 *
 * Backs dashboard_http.c's GET /api/profile_exec (total_planned_s/elapsed_s/
 * remaining_s/remaining_is_estimate) and GET /api/profile_plan (the drawable
 * polyline) -- lives here, not in dashboard_http.c, so the one honesty rule
 * below is written and host-tested exactly once, same reasoning as this
 * module already applying to profile_feasibility_segment().
 *
 * HONESTY RULE: profile_segment_t.ramp_c_per_hr <= 0 means "no rate
 * constraint" (profiles_http.h) -- profile_executor.c's segment-stepping
 * jumps the shared setpoint straight to that segment's target in a single
 * control tick when this is true. The ACTUAL kiln does not arrive there in
 * zero seconds: ramp-lock (profile_executor.h's top comment) holds the
 * schedule right there until every active zone catches up, and how long
 * that takes depends on the plant, not on anything this function can
 * compute. So such a segment's ramp portion is UNKNOWN, not 0 -- it makes
 * the whole profile's return value -1 ("cannot know"), and in the point
 * list it is plotted as a zero-width vertical step at its start time,
 * matching what the setpoint literally does. A caller learns the timeline
 * past that point isn't reliable from the -1 return, not from the segment
 * looking any particular way in the point list -- it must never silently
 * read as "0 seconds, nothing happening here".
 *
 * The dwell portion of every segment is always known (dwell_min*60),
 * rate-limited ramp or not.
 *
 * start_c is the temperature segment 0's ramp is assumed to start from --
 * ambient for an idle/preview call, or the run's actual captured starting
 * reading when the profile in question is the one really executing; the
 * caller decides which (see dashboard_http.c's PROFILE_PLAN_PREVIEW_AMBIENT_C
 * and profile_exec_status_t.run_start_c), this function just walks segments
 * from whatever start_c it's given. */
typedef struct {
    float t; /* seconds from profile start */
    float c; /* setpoint, Celsius */
} profile_plan_point_t;

/* Walks segments[0..segment_count) from start_c. Writes up to out_cap
 * points to out_points (one per ramp start/end and dwell start/end -- a
 * straight line between consecutive points is the correct rendering) and
 * the count actually written to *out_point_count. out_points/out_cap/
 * out_point_count may all be 0/NULL to skip point generation and just get
 * the total. Returns the total planned duration in seconds, or -1 if any
 * segment's ramp duration could not be determined -- see the honesty rule
 * above. */
int64_t profile_feasibility_plan_curve(const profile_segment_t *segments, uint8_t segment_count,
                                       float start_c, profile_plan_point_t *out_points,
                                       size_t out_cap, size_t *out_point_count);

#ifdef __cplusplus
}
#endif

#endif // PROFILE_FEASIBILITY_H
