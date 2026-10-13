#pragma once
// firing_shadow.h -- ITER_TUNE_REDESIGN.md step 8, "shadow mode".
//
// Scores every real firing the same way the redesigned iter_tune decision
// core would (firing_score.c/firing_compare.c, steps 1-2), records what the
// mechanism WOULD have proposed, and WRITES NOTHING to a zone's gains --
// ever. This is the on-hardware analogue of sec 3.1's simulated null
// experiment: comparing two consecutive, already-finished firings run under
// UNCHANGED gains is exactly what a "does the noise floor hold up on real
// hardware" measurement needs, and it is the same pattern already exercised
// by kilnctl_sim_strength_pct_adapt.exe's own consecutive-firing comparison.
//
// NO OPT-IN, NO NEW HTTP ROUTE (deliberate deviation from an earlier,
// uncoded draft of this design, which proposed a set_enabled action on
// POST /api/iter_tune/restore_commissioned). The plan's own step 8 row says
// shadow mode "scores every real firing" unconditionally -- there is no
// operator action to gate, shadow mode makes no destructive write to gate,
// and the URI handler cap (docs/CLAUDE.md, ~10 slots left as of this
// writing) is better spent on a route that actually writes something. If a
// kill switch is ever needed it belongs on iter_tune's own machinery, not a
// second, parallel enable flag here.
//
// NEVER calls any iter_tune_* function (this module's own contract, checked
// mechanically alongside iter_tune.c/.h by
// tools/PcTools/scripts/iter_tune_write_surface_check.py -- a new file under
// firmware/KilnFW/App is scanned automatically, no script change needed).
//
// PERSISTENCE: a compact verdict-summary counter, own NVS namespace
// "shadow_tune" (kiln_nvs partition) -- counts only, never the score sets
// themselves. The IN-PROGRESS firing_score_set_t (current firing) and the
// "previous finished firing" reference used for comparison are RAM-ONLY and
// do NOT survive a reboot: the first firing scored after any reset simply
// becomes the new reference with no verdict recorded, exactly like the first
// firing ever. This is deliberate (task instruction) -- there is no
// persisted-baseline hazard class to reason about here because nothing this
// module measures is ever acted on.
//
// SIMPLIFICATION vs. firing_score.h's general contract, both documented
// here rather than left implicit:
//   1. Segment boundaries are taken directly from the executor's own
//      per-tick `segment_index` (profile_executor_firing_stats.c already
//      tracks it) rather than re-derived independently -- a segment change
//      is anything that changes segment_index.
//   2. The commanded rate for firing_score_seg_begin() is estimated as the
//      instantaneous target-temperature slope at the FIRST tick of the new
//      segment (this segment's target_c minus the previous tick's target_c,
//      over that one tick's dt_s) rather than a rate read from the profile
//      step definition directly -- profile_executor_firing_stats.c has no
//      access to the profile step table, only the live target_c stream.
//      This is an approximation of the true commanded rate, not a defect in
//      firing_score itself.
//   3. saturated_high is always passed as false (the infeasibility
//      exclusion never fires here) -- profile_executor_firing_stats.c's
//      per-tick hook has no visibility into duty-cycle saturation. A
//      documented limitation, not a silent one.
//   4. The capture-transient latch (zone_captured) is reset once per
//      firing (at firing_shadow_finish_firing(), not mid-firing), matching
//      firing_score.h's own "whole firing, not per segment" contract.
//
// PURE MEASUREMENT, no write surface: builds on firing_score.h/
// firing_compare.h (both pure) plus one small, self-contained NVS store
// modeled directly on iter_tune_store.c's pattern.

#include <stdbool.h>
#include <stdint.h>

#include "firing_compare.h"
#include "firing_score.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t firings_scored;          // firings with a previous reference to compare against
    uint32_t accept_count;
    uint32_t reject_count;
    uint32_t insufficient_count;
    uint32_t no_matched_pairs_count;
    // A malloc() failure inside firing_compare() itself (FIRING_COMPARE_ALLOC_
    // FAILED, 2026-09-24 review advisory) -- kept distinct from
    // no_matched_pairs_count so a low-memory streak can never masquerade as a
    // genuine "nothing to compare" streak. Store version 2 (see .c).
    uint32_t alloc_failed_count;
    uint8_t  last_verdict;            // firing_compare_verdict_t narrowed to a byte
    float    last_composite_normalised;
} firing_shadow_status_t;

// Per-tick hook. Called from firing_stats_zone_tick() for EVERY tick of
// EVERY zone (valid or not) -- an invalid (actual_valid == false) tick is
// simply not scored, same exclusion firing_score.h itself would apply, and
// does not advance the open segment's own tracking.
void firing_shadow_zone_tick(uint8_t zone_index, bool actual_valid, float actual_c, float target_c,
                              bool dwelling, uint8_t segment_index, float dt_s);

// End-of-firing hook. Called once from inside firing_stats_persist(), which
// this task's other hook already guarantees runs on an internal-DRAM-stacked
// task (see that function's own caller_stack_is_external() guard, checked
// BEFORE this is reached) -- this function relies on that guarantee rather
// than re-checking it. Finishes any still-open per-zone segment, compares
// this firing's score set against the RAM-only previous-firing reference
// (floors == NULL, i.e. Bar 2 always skipped -- there is no measured noise-
// floor artifact wired to this module), updates and persists the compact
// verdict-summary counters, then resets every zone's per-firing state ready
// for the next run. Writes NOTHING outside this module's own NVS namespace.
void firing_shadow_finish_firing(void);

// Loads the persisted verdict-summary counters into RAM. Non-fatal on any
// failure (missing key, wrong version, wrong size) -- starts at all-zero,
// same convention as iter_tune_store_start(). Safe to call more than once.
void firing_shadow_store_start(void);

// True (and fills *out) once firing_shadow_store_start() has run; false
// (and *out untouched) before that -- never loads lazily, since the caller is
// the httpd task and a load there would race firing_shadow_finish_firing() on
// the executor task. iter_tune_http_start() calls firing_shadow_store_start()
// once at boot. *out reads all-zero if nothing has ever been persisted.
bool firing_shadow_get_status(firing_shadow_status_t *out);

// Test-only: resets every piece of RAM state (in-progress segments, the
// previous-firing reference, the cached status counters) without touching
// NVS, so host tests get a clean slate between cases.
void firing_shadow_reset_for_test(void);

// Discards the CURRENT, still-in-progress firing's per-zone segment state
// (s_current_set/s_zone) without finishing or scoring it, and WITHOUT
// touching NVS -- RAM only, so this is safe to call from any task's stack,
// including a PSRAM-stacked one. Does not touch the RAM-only previous-firing
// reference (s_previous_set/s_have_previous) or the persisted verdict-summary
// counters, so a future firing still compares against the last one that
// actually finished, exactly as if this abandoned firing had never started.
//
// Call site: firing_stats_persist()'s caller_stack_is_external() guard
// (2026-09-24 review advisory, "reset one side of a pair" class) -- when that
// guard refuses (an operator halt landed on a PSRAM-stacked task),
// firing_shadow_finish_firing() is never reached, and without this call the
// next firing's first ticks would silently append onto this abandoned
// firing's stale per-zone state instead of starting clean.
void firing_shadow_abandon_firing(void);

#ifdef __cplusplus
}
#endif
