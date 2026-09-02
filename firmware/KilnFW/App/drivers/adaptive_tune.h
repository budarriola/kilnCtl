// adaptive_tune.h -- PID_EXPANSION_PLAN.md Phase 7d: refine a zone's
// steady-state gain (K_dc) from the settled DWELLS every ordinary firing
// already produces, and, if the operator has opted in, quietly recompute
// that zone's PID gains from the refined model through the SAME SIMC path
// autotune's Accept uses (pid_autotune_tune_from_fopdt()).
//
// Scope, deliberately less than the plan's four layers (see
// PID_EXPANSION_PLAN.md Phase 7d-2 for the full design; this is Layer 1 +
// a diagonal-only slice of Layer 2/3):
//   - Per-zone DIAGONAL gain only -- K_dc[zone], not the coupled A matrix.
//     zones_config's existing model is already diagonal-only at this same
//     granularity (zones_config_get_model()/set_model()), so this refines
//     exactly what the feedforward term already consumes; a full coupled
//     solve is left for a future pass (needs the same rank/conditioning
//     machinery the plan calls out, on top of everything here).
//   - No integral (Ki) diagnosis, no dynamics (tau/L) re-fit -- both need
//     ramp/transition data this pass does not collect.
//   - No Layer 4 (iterative IAE-scored tuning) and no one-click revert --
//     both independent of this pass and left for later.
//
// Safety: every write this module makes happens at profile_executor.c's
// run-end (adaptive_tune_run_end()), AFTER the firing's relays are already
// off and the zone is no longer active -- never mid-firing, so there is no
// live control loop for a gain change to bump. See that function's own
// comment for why this makes seed_bumpless_with_ff() unnecessary here (it
// remains the right tool if a future pass ever needs a mid-run apply).
//
// Persistence split, and why: the LEARNED MODEL (K_dc) and the PID gains
// derived from it are written through the existing, already-validated
// setters (zones_config_set_model()/set_pid()) into the ordinary zone
// config blob -- the same place autotune's Accept path writes, so a reader
// cannot tell a learned gain from a hand-tuned or autotuned one, which is
// the point. The per-zone OPT-IN FLAG and this module's own observation
// bookkeeping live in a SEPARATE NVS namespace owned by this file
// (ADAPTIVE_TUNE_NVS_NAMESPACE below), NOT as a new field on the zone
// config blob: zones_http.c/zones_config_accessors.c (the zone config
// blob's owner) were off-limits for this pass (another agent holds them
// live), so a field there was not an option. Consolidating the opt-in flag
// into the zone blob proper, if wanted, is future work for whoever next
// touches that file.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "profile_executor.h" // profile_firing_run_record_t, MAX31856_CHANNEL_COUNT

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     enabled;                  // this zone's opt-in, default OFF -- see adaptive_tune_get_enabled()
    uint32_t ring_count;                // dwell observations currently held for this zone (<= capacity)
    uint32_t observations_lifetime;     // total observations ever folded in, this boot (ring evicts, this doesn't)
    // True once at least one refinement has been applied. RAM-only, so this
    // latch is per-BOOT, not lifetime: it clears on power cycle while the
    // gains it describes persist in NVS. The zones page gates its "last
    // applied change" column on this, so that column goes blank after a
    // reboot even though the change is still in effect.
    bool     has_applied;
    float    prior_k_dc;                // model K_dc immediately before the last applied refinement
    float    applied_k_dc;              // model K_dc immediately after it
    float    last_delta_pct;            // (applied-prior)/prior*100 -- signed
    uint8_t  last_applied_profile_id;   // which firing produced the last applied change
    uint32_t last_applied_unix_s;       // time(NULL) at that apply, 0 if clock never synced
    char     last_refusal_reason[96];   // empty if the most recent run-end attempt applied cleanly,
                                        // or none has been attempted yet; otherwise why it did not

    // -- Full coupled identification (PID_EXPANSION_PLAN.md 3.3, layer 2) --
    uint32_t joint_observations;        // joint (all-zone) dwell rows currently held, module-wide, not per-zone
    bool     coupled_attempted;         // a coupled solve for THIS zone's row was attempted at the last run_end
    bool     coupled_applied;           // it was attempted AND at least one off-diagonal cell was written
    uint8_t  coupled_cells_changed;     // how many coupling_coeff[this][*] cells were actually written
    char     coupled_refusal_reason[96];// why the coupled solve was refused/not applied for this zone, if it was

    // -- Integral (Ki) diagnosis from dwells (PID_EXPANSION_PLAN.md 3.3, layer 2) --
    uint8_t  ki_verdict;                // adaptive_tune_ki_verdict_t, cached as uint8_t so this header does not
                                        // need the enum's definition to compile against
    float    ki_correction_pct;         // signed suggested Ki move, before the per-run cap
    bool     ki_applied;                // true once a Ki correction was actually written this run
    char     ki_refusal_reason[96];     // why a nonzero diagnosis was not applied, if it was not
} adaptive_tune_zone_status_t;

// ---------------------------------------------------------------------
// Full coupled identification (Layer 2) -- pure fit math, host-tested
// directly (test_adaptive_tune.c), same seam convention as adaptive_tune_
// fit_gain() above.
// ---------------------------------------------------------------------

typedef enum {
    ADAPTIVE_TUNE_COUPLED_OK = 0,
    ADAPTIVE_TUNE_COUPLED_TOO_FEW_OBSERVATIONS,
    ADAPTIVE_TUNE_COUPLED_ILL_CONDITIONED,
} adaptive_tune_coupled_result_t;

// Solves, for EVERY affected zone i in 0..n-1, the least-squares row
// coupling_coeff[i][*] such that duty_obs[k][*] . coupling_coeff[i][*] ~=
// rise_obs[k][i] across the m joint observations (rise_obs[k][i] ==
// actual_c - ambient_c for zone i at joint observation k). This is
// zones_http.h's stored convention exactly: out_C[affected][stepped] --
// the SAME row/column orientation as zones_config_get/set_coupling(), NOT
// the transposed wire-report form /api/autotune/matrix serves. See adaptive_
// tune.c's own comment at this function's definition for the normal-
// equations derivation and why it is safe to reuse zone_coupling_gauss_
// solve_partial_pivot_vec() (zone_coupling_solve.c, not edited by this
// file) as the actual linear solve.
//
// m is the number of joint observations, n the number of zones/unknowns per
// row (<= MAX31856_CHANNEL_COUNT). Refuses (returns TOO_FEW_OBSERVATIONS)
// below m == n + ADAPTIVE_TUNE_COUPLED_OBS_MARGIN joint observations -- see
// the .c file for that margin's numeric value -- and refuses (ILL_
// CONDITIONED) if the shared design matrix fails the same pivot-floor
// conditioning check zone_coupling_gauss_solve_partial_pivot_vec() already
// applies at runtime (COUPLING_SOLVE_PIVOT_REL_EPS, admitting condition
// numbers up to ~1e4). out_C is left untouched unless ADAPTIVE_TUNE_COUPLED_
// OK is returned.
adaptive_tune_coupled_result_t adaptive_tune_coupled_fit(
    const float duty_obs[][MAX31856_CHANNEL_COUNT], const float rise_obs[][MAX31856_CHANNEL_COUNT], uint32_t m,
    uint8_t n, float out_C[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT]);

// ---------------------------------------------------------------------
// Integral (Ki) diagnosis from a within-dwell trace -- pure math, host-
// tested directly.
// ---------------------------------------------------------------------

typedef enum {
    ADAPTIVE_TUNE_KI_INSUFFICIENT = 0, // trace too short to say anything
    ADAPTIVE_TUNE_KI_OK,               // no diagnosis -- tracking looks fine
    ADAPTIVE_TUNE_KI_OFFSET_TOO_SMALL, // steady non-zero error, not oscillating -> Ki too small
    ADAPTIVE_TUNE_KI_FLOORED,          // looks like the OFFSET case, but duty is pinned near a rail the whole
                                        // window -- the integral is very likely sitting on its -ff_hold floor,
                                        // not under-correcting for lack of gain. NEVER produces a Ki correction.
    ADAPTIVE_TUNE_KI_OSCILLATING,      // drift or irregular hunting -> Ki too large
    ADAPTIVE_TUNE_KI_LIMIT_CYCLE,      // regular, sustained oscillation -> Ki too large, AND the same trace
                                        // hands over Ku/Tu (pid_autotune_fit_relay()) for free
} adaptive_tune_ki_verdict_t;

typedef struct {
    adaptive_tune_ki_verdict_t verdict;
    float    ki_correction_pct;  // signed, BEFORE any per-run cap: +N% means "Ki should grow", -N% "shrink".
                                  // Always 0 for INSUFFICIENT/OK/FLOORED.
    float    ku_estimate;        // valid (nonzero) only for LIMIT_CYCLE
    float    tu_estimate_s;      // valid (nonzero) only for LIMIT_CYCLE
    uint32_t zero_crossings;     // diagnostic -- surfaced so a test can assert the *reason*, not just the verdict
} adaptive_tune_ki_diag_t;

// actual_c/duty are a trailing, TIME-ORDERED trace of ONE dwell (dt_s
// constant spacing, same convention as adaptive_tune_zone_tick's dt_s).
// dwell_err_mean_c/dwell_err_max_c are profile_exec_firing_stats_t's own
// dwell-only error figures (mean/max |actual-target|, SETPOINT-aware --
// this function never receives setpoint_c itself; see adaptive_tune.c's
// top comment on why) for the SAME dwell/run. Returns false only if out is
// NULL; a too-short trace is reported as ADAPTIVE_TUNE_KI_INSUFFICIENT in
// *out, not a false return, so callers do not have to special-case it.
bool adaptive_tune_diagnose_ki(const float *actual_c, const float *duty, uint32_t n, float dt_s,
                               float dwell_err_mean_c, float dwell_err_max_c, adaptive_tune_ki_diag_t *out);

// Loads the per-zone opt-in flags from this module's own NVS namespace.
// Call once at boot; profile_executor.c does this from profile_executor_
// start(), since this file owns no init entry point of its own in main.c.
// Safe to call from app_main's task (not PSRAM-stacked) -- see adaptive_
// tune.c's top comment for the read-vs-write stack-safety split. Registers
// NO HTTP endpoints itself -- see adaptive_tune_http.h's adaptive_tune_http_
// start() for those (GET /api/adaptive_tune, POST /api/adaptive_tune/enable),
// called separately from main.c once the shared httpd server is up.
void adaptive_tune_init(void);

// Called once per control tick, per active zone, from profile_executor.c's
// tick loop, WITH s_exec.lock held (this module keeps its own internal
// lock, taken only from inside this call and adaptive_tune_run_end() below
// -- never the reverse of s_exec.lock -> this module's lock, so there is no
// new deadlock ordering to reason about against s_at.lock). actual_c is
// calibration-corrected; ambient_c is the run's captured cold-junction
// ambient (profile_executor.c's s_exec.ambient_c). dt_s is the same
// measured tick interval firing_stats_zone_tick() uses. Pure bookkeeping:
// detects a settled dwell (reusing autotune_engine.c's slope-floor
// approach, not autotune's own state) and records ONE (duty, rise-over-
// ambient) observation per dwell into this zone's ring. Never touches NVS
// or the network.
void adaptive_tune_zone_tick(uint8_t zone_index, float actual_c, bool actual_valid, float duty, bool dwelling,
                              float ambient_c, float dt_s);

// Called once per finished run, per active zone, from profile_executor.c
// AFTER s_exec.lock has been released (matching firing_stats_persist()'s
// own call site) -- this is the run-end, safe-boundary hook: the zone that
// just fired is no longer active, so any gain change applied here cannot
// bump a live control loop. `clean` is the caller's verdict on the whole
// run (profile completed normally, not faulted or operator-stopped early);
// a per-zone excluded_sample_count check happens here too. Attempts a
// refinement for every ACTIVE, opted-in zone in rec; every guard this can
// refuse on is documented on the static helpers in adaptive_tune.c.
void adaptive_tune_run_end(const profile_firing_run_record_t *rec, bool clean);

// Opt-in setter/getter -- the only thing an operator directly controls.
// Persists via this module's own flash-worker-routed NVS write (see
// adaptive_tune.c's top comment); returns false if the write failed (the
// live flag still takes effect either way, same "applied live, logged if
// the save failed" convention as time_sync_set_tz()).
bool adaptive_tune_set_enabled(uint8_t zone_index, bool enabled);
bool adaptive_tune_get_enabled(uint8_t zone_index);

void adaptive_tune_get_status(uint8_t zone_index, adaptive_tune_zone_status_t *out);

// ---- pure helpers, exposed for host tests (adaptive_tune.c has no other
// seam into this math -- see test_adaptive_tune.c) --------------------------

// Ordinary-least-squares fit of K through the origin: K = sum(u*dT) /
// sum(u*u), dT = rise over ambient. Physically this is exactly Phase 7d's
// "K_effective = (T_dwell - T_ambient) / u_steady" generalized to many
// dwells instead of trusting any single one. Returns false (does not write
// *out_k) if the duty values carry too little energy to divide by
// (sum(u*u) below ADAPTIVE_TUNE_FIT_MIN_DENOM) -- see the .c file for that
// bound's numeric value and reasoning.
bool adaptive_tune_fit_gain(const float *duty, const float *rise_c, uint32_t n, float *out_k);

#ifdef __cplusplus
}
#endif
