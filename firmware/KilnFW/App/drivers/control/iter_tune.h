#pragma once
// iter_tune.h -- ITER_TUNE_REDESIGN.md step 5: the redesigned
// iterative-tuning decision core.
//
// WHAT CHANGED, AND WHY THE OLD CORE IS GONE RATHER THAN KEPT ALONGSIDE.
// The previous version of this module scored a whole firing with one scalar
// (profile_executor_firing_stats.c's iae_normalized) and refused to compare
// two firings whose start temperatures differed by more than 2 degC. That
// made it correct and nearly always idle -- the only two full captures this
// repo has of the same profile and build differ by 4.8 degC at the first
// sample. The owner's requirement (2026-09-08) was precisely that the
// start-point dependence go away and that the objective be "how well it
// tracks the target temperature". So the scalar, the comparability window,
// and the relative/absolute IAE thresholds are DELETED, not deprecated --
// the plan's step 5 gate is "the old whole-firing path fully removed, not
// left dual". Scoring now lives in firing_score.c (per matched segment) and
// the accept rule in firing_compare.c (matched-pair, non-dominance).
//
// WHAT IS KEPT VERBATIM from the old module, because it was right:
//   - the EXACT-REVERT posture: gains are never recomputed on revert;
//     iter_tune_active_gains() returns the same float bits that were
//     accepted. Revert is a flag clear, never an arithmetic undo.
//   - pure decision logic: no ESP-IDF, no NVS, no lock, no FreeRTOS. The
//     caller owns persistence and the one run-boundary call site.
//   - opt-in per zone, default OFF (a zeroed struct is a valid disabled
//     state).
//   - kd is never touched.
//
// THE CAGE IS ANCHORED TO A PERSISTED COMMISSIONING SNAPSHOT, not to the
// rolling baseline. A baseline-relative cage ratchets -- this repo has been
// bitten by exactly that ("Bound relative to persisted state"). Per the
// plan's settled owner decision 9.1, the anchor is captured AUTOMATICALLY
// the first time iter_tune is enabled for a zone, and can be deliberately
// moved later by an explicit re-anchor action, which is a different action
// from the "restore commissioned gains" revert.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "firing_compare.h"

#ifdef __cplusplus
extern "C" {
#endif

// Absolute bounds. ITER_TUNE_GAIN_CEIL_C mirrors ZONE_PID_GAIN_MAX
// (zones_http.h, 1000.0f) by VALUE, not by #include -- this file must stay
// free of that header's dependency chain. test_iter_tune.c pins both values
// with an explicit comment so the day one changes without the other, the
// test goes red, not silent.
#define ITER_TUNE_GAIN_FLOOR_C 0.0f
#define ITER_TUNE_GAIN_CEIL_C 1000.0f

// The cage, relative to the persisted commissioned anchor (plan sec 4).
#define ITER_TUNE_CAGE_LOW_FACTOR 0.5f
#define ITER_TUNE_CAGE_HIGH_FACTOR 2.0f

// Adaptive step. The plan (sec 4) specifies: start at 10%, HALVE after two
// consecutive rejects, double (capped at 20%) after two consecutive
// accepts, converge below 3%.
//
// DELIBERATE DEVIATION, FOUND IN SIMULATION (sim_iter_tune.c, this change):
// the plan's rule halves the step after ANY two rejects, but the two reject
// kinds mean opposite things.
//   - REJECT_DEGRADED means the change WAS measurable and was bad. Halving
//     is right: take a smaller bite.
//   - INSUFFICIENT means nothing cleared the 0.5 degC floor, i.e. the change
//     was TOO SMALL TO MEASURE. Halving it makes the next signal smaller
//     still, which is exactly backwards, and the search then walks itself
//     down to the 3% convergence threshold having learned nothing.
// The simulator measured this directly: on the bench plant model a 10% step
// on kp or ki moves the sub-scores by ~0.01-0.10 degC against a 0.5 degC
// floor, while an oracle grid over the same cage shows 1.5-1.7 degC of
// tracking error genuinely available. Under the plan's own schedule the
// mechanism was structurally inert -- it refused every trial from every
// starting gain set, for the same "two mutually exclusive conditions"
// reason this repo has already recorded once for dwell credit.
// So: INSUFFICIENT now GROWS the step (doubling, capped at
// ITER_TUNE_STEP_PROBE_MAX and still hard-caged to [0.5x, 2x] of the
// anchor) until the change is large enough to be measurable at all;
// DEGRADED still halves it. Convergence is unchanged in spirit: a zone
// stops when it has probed both directions of both parameters at the probe
// cap without a measurable result, or the step is halved below 3%.
#define ITER_TUNE_STEP_START 0.20f
#define ITER_TUNE_STEP_MAX 0.20f
// Growth cap for the "too small to measure" case. 50% of the baseline is
// still inside the [0.5x, 2x] cage from the anchor, so this widens the
// STEP, never the bound.
#define ITER_TUNE_STEP_PROBE_MAX 0.50f
#define ITER_TUNE_STEP_MIN 0.03f

// Owner decision 9.2: six SCORED trials per zone, not twelve. Six is the
// smallest round number strictly above Bar 2's own n >= 5 minimum, leaving
// one trial of margin rather than landing exactly on the bar.
#define ITER_TUNE_MAX_TRIALS 6

// Three consecutive rejects while already at the minimum step is the
// "this zone has nothing left to find" stopping condition.
#define ITER_TUNE_MAX_REJECTS_AT_MIN_STEP 3

// A gain that clamps to a cage edge twice stops the zone -- the search is
// pushing at a boundary it is not allowed to cross.
#define ITER_TUNE_MAX_CAGE_EDGE_HITS 2

// A trial that scores no matched pairs stays armed and rides the next
// firing, but only this many times, so a stale trial cannot ride
// indefinitely against a moving plant (plan sec 4).
#define ITER_TUNE_MAX_CARRIES 3

typedef struct {
    float kp;
    float ki;
    float kd;
} iter_tune_gains_t;

typedef enum {
    ITER_TUNE_PARAM_KP = 0,
    ITER_TUNE_PARAM_KI = 1,
    ITER_TUNE_PARAM_COUNT = 2,
} iter_tune_param_t;

typedef enum {
    ITER_TUNE_STATUS_OFF = 0,       // never enabled
    ITER_TUNE_STATUS_TUNING = 1,    // enabled, still has budget
    ITER_TUNE_STATUS_CONVERGED = 2, // a stopping rule fired; sticky
    ITER_TUNE_STATUS_FAULTED = 3,   // a guard trip / fault / operator halt ended it; sticky
} iter_tune_status_t;

// Why a zone stopped. Persisted alongside `status` so a CONVERGED zone can
// always say WHY it stopped -- the review of 2026-09-09 found a path
// (an un-perturbable, i.e. zero-valued, parameter) that terminated the
// search with no trial, no fault and no text at all.
typedef enum {
    ITER_TUNE_STOP_NONE = 0,              // still running, or never started
    ITER_TUNE_STOP_PARAMS_EXHAUSTED = 1,  // every parameter probed out
    ITER_TUNE_STOP_TRIAL_BUDGET = 2,      // ITER_TUNE_MAX_TRIALS scored trials used
    ITER_TUNE_STOP_CAGE_EDGE = 3,         // pushed at the cage boundary too often
    ITER_TUNE_STOP_UNPERTURBABLE = 4,     // no parameter can be moved at all (see below)
    ITER_TUNE_STOP_FAULT = 5,             // iter_tune_fault(): guard trip / halt
} iter_tune_stop_reason_t;

typedef enum {
    ITER_TUNE_RESULT_DISABLED = 0,
    ITER_TUNE_RESULT_NO_TRIAL = 1,          // nothing was pending; nothing judged
    ITER_TUNE_RESULT_ACCEPTED = 2,
    ITER_TUNE_RESULT_REVERTED = 3,          // rejected or insufficient evidence -- exact revert
    ITER_TUNE_RESULT_CARRIED = 4,           // no matched pairs; trial stays armed for the next firing
    ITER_TUNE_RESULT_CARRY_EXHAUSTED = 5,   // carried too often; reverted unscored
} iter_tune_result_t;

// Persisted per-zone state. A zeroed struct is a valid, fully-disabled
// starting state -- "opt-in, default OFF, per zone" with no init call.
typedef struct {
    bool enabled;

    // Cage anchor: the gains active the FIRST time this zone was enabled
    // (owner decision 9.1). Never rewritten by tuning; moved only by an
    // explicit iter_tune_reanchor().
    bool              has_anchor;
    iter_tune_gains_t anchor;

    // Current accepted gains. Trial gains live ONLY in pending_gains, so a
    // revert is a flag clear and is bit-exact.
    bool              has_baseline;
    iter_tune_gains_t baseline;

    bool              has_pending;
    iter_tune_gains_t pending_gains;

    // Per-PARAMETER search state. The plan (sec 4) specifies coordinate
    // descent "cycling kp -> ki -> next zone", i.e. the parameter advances
    // every trial -- so each parameter must carry its OWN step size and
    // direction, or one parameter's ladder silently resets the other's.
    // Measured in simulation: with a single shared step and a parameter that
    // only advanced on failure, a six-trial budget was spent entirely on kp
    // (which the oracle grid shows barely moves tracking on this plant) and
    // ki -- which carries almost all of the available improvement -- was
    // never reached at all.
    uint8_t param;                                   // parameter to move on the NEXT proposal
    float   step_frac[ITER_TUNE_PARAM_COUNT];        // adaptive step, fraction of the baseline value
    bool    step_negative[ITER_TUNE_PARAM_COUNT];    // direction of the next proposal
    uint8_t consec_accepts[ITER_TUNE_PARAM_COUNT];
    uint8_t consec_rejects[ITER_TUNE_PARAM_COUNT];   // consecutive DEGRADED rejects -- these halve the step
    uint8_t param_done[ITER_TUNE_PARAM_COUNT];       // 1 once this parameter is exhausted
    // Bitmask per parameter: bit 0 set once the POSITIVE direction has hit
    // a cage edge with zero movement, bit 1 for NEGATIVE. 2026-09-09 fix
    // (opus review of 249ce287): a clamp-produced-no-movement used to retire
    // the parameter after trying only ONE direction -- step_negative[pi] was
    // never flipped first. A baseline sitting at the top of the cage (a
    // common resting place after a run of accepted increases) retired kp
    // forever the first time an upward step clamped to no movement, even
    // though a downward step was still entirely legal, and could stop the
    // whole zone CONVERGED / STOP_PARAMS_EXHAUSTED with real search space
    // left unexplored. Both bits set (both directions independently proven
    // immovable) is what now actually retires the parameter.
    uint8_t cage_edge_dir_tried[ITER_TUNE_PARAM_COUNT];
    uint8_t trials_scored;
    uint8_t carries;
    uint8_t cage_edge_hits;
    uint8_t status;            // iter_tune_status_t
    uint8_t stop_reason;       // iter_tune_stop_reason_t -- RAM-only, part of this
                               // in-RAM state struct like every other field here;
                               // 249ce287's commit message called it "persisted",
                               // which is not true of anything in
                               // iter_tune_zone_state_t -- nothing in this module
                               // writes NVS/LittleFS/config_store. Corrected here
                               // since the commit message itself cannot be edited.
} iter_tune_zone_state_t;

// Turns the mechanism on for a zone. The FIRST enable snapshots
// `current_gains` as the commissioned anchor and seeds the baseline from it;
// a later re-enable leaves both alone. Returns false if already enabled.
bool iter_tune_enable(iter_tune_zone_state_t *state, iter_tune_gains_t current_gains);

// Deliberately moves the cage centre (owner decision 9.1's "re-anchor"),
// e.g. after a fresh hand-tuning pass. Distinct from the revert action
// below: this moves the ANCHOR, not the gains -- the accepted baseline is
// KEPT, merely clamped into the new cage. Clears any pending trial and
// restarts the budget, because the search space has changed.
//
// IT ALSO CLEARS A STICKY CONVERGED STATUS, which is the whole point of the
// action. Before 2026-09-09 it did not: iter_tune_enable() refuses while
// status is CONVERGED, re-anchor only rewrote the status when the zone was
// still `enabled`, and a CONVERGED zone is by construction disabled -- so
// the documented escape hatch ("moving it takes a deliberate re-anchor")
// was a permanent no-op, and the only real exit from CONVERGED was
// iter_tune_restore_commissioned(), which throws every accepted gain away.
// The intended sequence is now: iter_tune_reanchor() then
// iter_tune_enable(), which resumes with the accepted gains intact.
//
// FAULTED is deliberately NOT cleared -- plan sec 5.5's "it does not
// retry". A zone stopped by a guard trip must be investigated and put back
// through iter_tune_restore_commissioned(). Returns false in that case,
// having changed nothing.
bool iter_tune_reanchor(iter_tune_zone_state_t *state, iter_tune_gains_t new_anchor);

// The single operator action of plan sec 4's Revert path: put the persisted
// commissioned gains back and disable the module for this zone. Returns the
// gains the caller must write.
iter_tune_gains_t iter_tune_restore_commissioned(iter_tune_zone_state_t *state);

// Ends tuning for this zone after a guard trip, FAULTED transition, or
// operator halt during a trial firing: the trial is discarded UNSCORED,
// gains revert, and the zone is disabled with a sticky status. It does not
// retry (plan sec 5.5).
void iter_tune_fault(iter_tune_zone_state_t *state);

// The gains to run with right now: the pending trial's if one is armed,
// otherwise the accepted baseline. A caller that always asks this gets the
// exact revert for free.
iter_tune_gains_t iter_tune_active_gains(const iter_tune_zone_state_t *state);

// Clamps `g` into the cage: [0.5x, 2x] of the anchor for kp and ki, and the
// absolute [FLOOR, CEIL] bounds, with kd passed through untouched. Pure;
// applied inside iter_tune_propose_perturbation() AND intended to be
// re-applied by the caller before writing, so a bug in one layer is caught
// by the other (plan sec 5.3). Sets *out_hit_edge when a clamp actually
// bound.
iter_tune_gains_t iter_tune_clamp_to_cage(const iter_tune_zone_state_t *state, iter_tune_gains_t g,
                                          bool *out_hit_edge);

// A parameter whose value is zero (a P-only zone with ki == 0 is entirely
// plausible) is UN-PERTURBABLE and is skipped, not probed from an absolute
// floor. Two independent reasons, and either alone settles it:
//   - the search is multiplicative (v * (1 +/- step)); zero has no scale,
//     so there is no honest step size to take from it;
//   - the cage is multiplicative too, so a zero ANCHOR collapses
//     [0.5x, 2x] to exactly {0}. Perturbing off zero would mean leaving the
//     cage, and the cage is the safety bound -- widening it to accommodate
//     an un-tunable parameter is exactly the ratchet this repo has already
//     been bitten by ("Bound relative to persisted state").
// Before 2026-09-09 this case stalled instead: the proposal collapsed back
// onto the baseline, the "no movement" guard returned false without
// advancing the parameter or setting param_done, and every later call
// returned false with status still TUNING -- no trial, no CONVERGED, no
// fault, no reason. Worse, `param` starts at KP, so a zero kp also blocked
// ki from ever being probed. A zone whose parameters are all
// un-perturbable now stops CONVERGED with stop_reason
// ITER_TUNE_STOP_UNPERTURBABLE.
//
// Proposes the next single-parameter perturbation. ONE parameter, ONE zone,
// per firing -- never two: the measured coupling matrix is large and
// asymmetric, so a simultaneous two-zone perturbation is unattributable by
// construction. kd is never touched. Returns false (state untouched) when
// disabled, unanchored, already pending, or stopped.
bool iter_tune_propose_perturbation(iter_tune_zone_state_t *state, iter_tune_gains_t *out_gains);

// The one call site, at run completion only. `cmp` is firing_compare()'s
// verdict for this firing's score set against the baseline's. Applies the
// accept/revert, the adaptive step update, the carry rule, and the stopping
// rules. `reason` is optional human text.
iter_tune_result_t iter_tune_process_comparison(iter_tune_zone_state_t *state,
                                                const firing_compare_result_t *cmp, char *reason,
                                                size_t reason_len);

const char *iter_tune_status_str(iter_tune_status_t status);
const char *iter_tune_stop_reason_str(iter_tune_stop_reason_t reason);
const char *iter_tune_result_str(iter_tune_result_t result);

#ifdef __cplusplus
}
#endif

// ---------------------------------------------------------------------
// INTEGRATION POINT (documented; deliberately not wired in by this change --
// plan step 7 owns persistence and the HTTP surface, and step 8 puts it in
// shadow mode on hardware before it ever proposes anything there).
//
// At run start: for each enabled zone, iter_tune_propose_perturbation() if
// no trial is armed, then write iter_tune_active_gains() into the zone's
// live pid_cfg via zones_config_set_pid(zone, kp, ki, kd) -- that call is
// this module's ONLY write path into board state, and kd is passed through
// unmodified.
//
// During the run: feed firing_score_seg_tick() per zone per executor tick,
// opening a segment at every profile segment boundary.
//
// At run completion (profile_executor_firing_stats.c's
// firing_stats_maybe_finalize(), the same run-boundary-only call site that
// already builds a profile_firing_run_record_t): firing_compare() this
// firing's score set against the stored baseline set, then
// iter_tune_process_comparison(). On ACCEPTED, persist the new baseline AND
// the trial's score set as the new baseline set. On REVERTED, persist
// nothing but the state flags. On a guard trip or halt, call
// iter_tune_fault() instead of comparing anything.
