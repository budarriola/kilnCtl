#pragma once
// iter_tune.h -- PID_EXPANSION_PLAN.md 3.3, "Iterative tuning": the layer
// that scores each firing with its own already-recorded normalized IAE
// (profile_executor_firing_stats.c's firing_stats_snapshot(), read there,
// never recomputed here), perturbs a zone's gains slightly, and keeps the
// change only if the NEXT comparable firing scores better.
//
// WHY THIS LAYER MATTERS MORE THAN THE IDENTIFICATION LAYERS (see the
// plan's 3.3 entry on "Dynamics from ramps", SHELVED): every identification
// fit in this codebase needs an informative signal -- a held step, an
// excited ramp -- and every one of those has turned out to be fakeable by
// something that isn't plant dynamics (the ramp fit reduced to a function
// of the commanded rate alone). This layer needs none of that: it only
// asks "did the whole firing track better than last time," which is true
// or false regardless of what shape the profile commanded. It is much
// harder to fool BUT it is trivially fooled by noise if the accept
// threshold isn't respected -- see the noise-floor section below, which is
// the load-bearing part of this file.
//
// THIS MODULE IS PURE DECISION LOGIC. No ESP-IDF, no NVS, no lock, no
// FreeRTOS -- same posture as max31856_codec.c/panel_codec.c. The caller
// (integration point documented at the bottom of this file) owns:
//   - persisting iter_tune_zone_state_t (its own NVS namespace, own opt-in
//     flag storage -- deliberately NOT zones_http.c/adaptive_tune's own
//     "adap_tune" namespace; those are owned by other in-flight work this
//     session, see PID_EXPANSION_PLAN.md 3.3's other two open bullets)
//   - actually writing gains into the zone's live PID config before a run
//     and reading profile_exec_firing_stats_t back out after one
//   - calling iter_tune_process_firing() exactly once per completed run,
//     at the same run-boundary-only call site profile_executor_firing_
//     stats.c already finalizes a profile_firing_run_record_t (see
//     firing_stats_maybe_finalize() in that file) -- NEVER mid-run, or a
//     perturbation could be judged against telemetry it didn't produce.
//
// This file does NOT touch adaptive_tune.c/.h, adaptive_tune_ki.c,
// adaptive_tune_model.c, adaptive_tune_internal.h, zones_http.c, or any
// display file, and is not wired into profile_executor.c by this change --
// wiring it in is the caller's job, at the integration point documented at
// the end of this header.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------
// Bounds -- mirror the limits the other tuning layers already respect.
//
// ITER_TUNE_GAIN_CEIL_C mirrors ZONE_PID_GAIN_MAX (zones_http.h, 1000.0f)
// by VALUE, not by #include -- this file must stay free of zones_http.h's
// dependency chain (that header is owned by other in-flight work this
// session). test_iter_tune.c pins both values with an explicit comment so
// the day one changes without the other, the test goes red, not silent.
#define ITER_TUNE_GAIN_FLOOR_C 0.0f
#define ITER_TUNE_GAIN_CEIL_C 1000.0f

// Bounded, revertible nudge per trial -- same posture as adaptive_tune's
// ADAPTIVE_TUNE_MAX_FRACTIONAL_MOVE / ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE
// (both 20%, adaptive_tune_internal.h), but iterative tuning has no fitted
// model to bound the move against -- only the immediately prior accepted
// gains -- so a materially smaller step is used: 20% of a gain, applied
// blind (no plant model informing direction or size), would be a much
// larger single-firing swing than a blended, model-checked 20% move.
#define ITER_TUNE_PERTURB_FRACTION 0.05f

// ---------------------------------------------------------------------
// The noise floor. THIS IS THE PART THAT MAKES THE MECHANISM HONEST.
//
// Two firings of the same profile, same zones, same gains do not produce
// identical normalized IAE -- ordinary sensor quantization (0.1 degC),
// ambient drift, and the executor's own tick jitter all move it. Accepting
// ANY improvement, however small, ratchets on that noise and calls it
// progress; §3.3's own adaptive-tune-Ki work independently hit exactly
// this failure mode (a vacuous decreasing-direction test) before it was
// closed.
//
// This repo has no controlled same-gain repeat-firing dataset to fit a
// true noise floor from (the five plant_sim fixture captures --
// tools/PcTools/tests/fixtures/plant_sim/*.jsonl -- are five DIFFERENT
// firmware/gain states, not five repeats of one). The two captures closest
// to a repeat -- holdfix_clean.jsonl and final.jsonl, both labeled
// climb_mode=coupled/integral_floor=ff_hold, i.e. the same shipped build --
// still are not proven to share identical gains or ambient conditions, so
// their spread is an UPPER bound on true noise (it may include real
// gain/environment differences), not a measured floor. Their whole-run
// normalized IAE, read directly from each capture's final poll row:
//
//     zone   holdfix_clean   final    relative change
//     z0     0.0275          0.0337   +22.5% (worse)
//     z1     0.0160          0.0236   +47.5% (worse)
//     z2     0.0210          0.0270   +28.6% (worse)
//
// Spreads of 20-48% between two nominally-comparable firings of the SAME
// build. That is larger than many real gain improvements this session
// measured (§2's whole-run IAE table shows the day's total improvement was
// 41-63%, achieved over MANY changes, not one). Read plainly: at this
// noise level, a single-firing A/B comparison cannot safely accept an
// improvement smaller than roughly a quarter of a gain's total possible
// benefit, which is a real limitation, not a tuning choice this file can
// paper over. ITER_TUNE_MIN_RELATIVE_IMPROVEMENT is therefore set at the
// conservative end of that observed spread rather than fit to it (fitting
// a threshold to two data points would be exactly the vacuous-test mistake
// this session has repeatedly shipped and reverted) -- 20%, matching the
// fractional-move convention every other bounded layer in this file's
// neighborhood already uses (ADAPTIVE_TUNE_MAX_FRACTIONAL_MOVE /
// ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE, adaptive_tune_internal.h). A firing
// that improves by less than this is treated as noise and reverted, not
// accepted.
//
// HONEST CONSEQUENCE: with a 20-48% observed spread and a 20% acceptance
// bar, this mechanism WILL occasionally accept a gain change that was pure
// noise (a below-floor true change can still show an above-floor
// measurement by chance), and will occasionally revert a real improvement
// that measured smaller than the noise on that particular firing. Neither
// failure compounds, because every subsequent firing is scored against
// whatever is currently accepted, not against history -- a lucky accept
// gets re-tested the very next firing under the same bar.
#define ITER_TUNE_MIN_RELATIVE_IMPROVEMENT 0.20f

// Two firings are only comparable at a "comparable starting temperature" --
// project_autotune_needs_rested_baseline (this repo's own lesson):
// residual heat from a prior firing biases the fitted/measured behavior.
// 5 degC is roughly half the smallest per-zone quantization-visible step
// this repo's IAE figures resolve at bench scale (§2's overshoot deltas
// are all >= 0.1 degC; 5 degC is two orders of magnitude looser, i.e. a
// "this zone had clearly not returned to a rested state" guard, not a
// precision claim).
#define ITER_TUNE_START_TEMP_TOLERANCE_C 5.0f

// ---------------------------------------------------------------------

typedef struct {
    float kp;
    float ki;
    float kd;
} iter_tune_gains_t;

// One firing's identity + score, as the caller reads it back from
// profile_firing_run_record_t / profile_exec_firing_stats_t after a run
// completes. iae_normalized is copied VERBATIM from
// profile_exec_firing_stats_t.iae_normalized (profile_executor_firing_
// stats.c's firing_stats_snapshot()) -- never recomputed here.
typedef struct {
    uint8_t profile_id;
    uint8_t zone_mask;     // profile.zone_mask for this run -- must match exactly to compare
    float   start_temp_c;  // this zone's actual_c at the first accumulated tick
    float   iae_normalized;// score -- lower is better
    iter_tune_gains_t gains; // the gains that PRODUCED this score
} iter_tune_firing_t;

// Persisted per-zone state. Zero-initialized is a valid, fully-disabled
// starting state (enabled == false, has_baseline == false) -- matches the
// "opt-in, default OFF, per zone" requirement without a separate init call.
typedef struct {
    bool enabled;                    // per-zone opt-in, default OFF

    bool               has_baseline; // false until the first firing under this mechanism completes
    iter_tune_firing_t baseline;     // last ACCEPTED (or seeded) firing: its gains + the score they earned

    bool               has_pending;    // a perturbation is currently on trial
    iter_tune_gains_t  pending_gains;  // gains under trial (only meaningful if has_pending)
    bool               next_perturb_negative; // alternates trial direction call to call -- see
                                               // iter_tune_propose_perturbation()'s doc comment
} iter_tune_zone_state_t;

typedef enum {
    ITER_TUNE_RESULT_DISABLED,          // state->enabled was false; nothing touched
    ITER_TUNE_RESULT_SEEDED_BASELINE,   // no prior baseline -- this firing became one, no comparison made
    ITER_TUNE_RESULT_BASELINE_REFRESHED,// firing used baseline gains (no trial pending) -- baseline updated
    ITER_TUNE_RESULT_REFUSED_NOT_COMPARABLE, // different profile/zone_mask/start temp -- trial left pending, unscored
    ITER_TUNE_RESULT_ACCEPTED,          // trial scored >= MIN_RELATIVE_IMPROVEMENT better -- new baseline
    ITER_TUNE_RESULT_REVERTED,          // trial scored worse, or not enough better -- baseline gains restored
} iter_tune_result_t;

// True (and reason left empty) only if `b` may be legitimately compared
// against `a` -- same profile_id, same zone_mask, and a starting
// temperature within ITER_TUNE_START_TEMP_TOLERANCE_C. Pure, no state
// mutation; exposed separately from iter_tune_process_firing() so it is
// independently host-testable per REQUIREMENT.
bool iter_tune_comparable(const iter_tune_firing_t *a, const iter_tune_firing_t *b, char *reason,
                           size_t reason_len);

// Returns the gains that should be applied to this zone's live PID config
// RIGHT NOW -- the pending trial's gains if one is in flight, otherwise the
// current accepted baseline. A caller that always asks this function for
// "what gains do I run with" gets an exact revert for free: REVERTED never
// recomputes anything, it just clears has_pending, so this function starts
// returning the untouched baseline.gains again -- the same float bits that
// were accepted last time, not a recomputation of them.
iter_tune_gains_t iter_tune_active_gains(const iter_tune_zone_state_t *state);

// Proposes a new bounded, revertible perturbation of the current baseline
// gains (kp and ki move by +/-ITER_TUNE_PERTURB_FRACTION, alternating sign
// call to call so both directions get explored over time rather than
// walking off in whichever direction the first nudge happened to try; kd
// is left untouched -- derivative gain is the noise-sensitive term and is
// out of scope for a blind perturbation with no plant model backing it).
// Clamped to [ITER_TUNE_GAIN_FLOOR_C, ITER_TUNE_GAIN_CEIL_C]. Requires
// state->enabled && state->has_baseline && !state->has_pending; returns
// false (state untouched) otherwise -- callers should not propose a second
// trial while one is already outstanding. On success, writes
// state->pending_gains, sets has_pending, flips next_perturb_negative, and
// returns true.
bool iter_tune_propose_perturbation(iter_tune_zone_state_t *state, iter_tune_gains_t *out_gains);

// The one call site this whole module funnels through, at run-completion
// time only (see this header's top comment). `firing` is the just-
// completed run's identity/score, scored under iter_tune_active_gains()'s
// gains as of when that run STARTED (the caller is responsible for that
// invariant -- this function has no way to check it).
iter_tune_result_t iter_tune_process_firing(iter_tune_zone_state_t *state, const iter_tune_firing_t *firing,
                                             char *reason, size_t reason_len);

#ifdef __cplusplus
}
#endif

// ---------------------------------------------------------------------
// INTEGRATION POINT (documented, not wired in by this change -- see this
// file's top comment for why: avoiding profile_executor.c during a session
// where other agents are mid-flight on adjacent work in that area).
//
// At run start (profile_executor_run(), before the executor task begins
// driving a PID-mode zone): for each zone with iter_tune enabled, call
// iter_tune_active_gains() and write the result into that zone's live
// pid_cfg (zones_config_set_pid() or equivalent) before the run begins.
//
// At run completion (profile_executor_firing_stats.c's firing_stats_
// maybe_finalize(), the same call site that already builds a
// profile_firing_run_record_t): for each zone with iter_tune enabled,
// build an iter_tune_firing_t from that record's zr->stats.iae_normalized,
// zr->kp/ki/kd, rec->profile_id, rec->zone_mask, and the zone's actual_c
// at the run's first accumulated tick (not currently captured anywhere --
// a caller wiring this in needs to add that one field, e.g. to zone_
// runtime_t, alongside fs_target_min_c/fs_target_max_c's existing
// first-tick capture pattern), then call iter_tune_process_firing(). If
// the result is ITER_TUNE_RESULT_ACCEPTED or ITER_TUNE_RESULT_REVERTED,
// persist the (possibly reverted) state and call
// iter_tune_propose_perturbation() to arm the next run's trial.
