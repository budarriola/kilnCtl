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
    bool     has_applied;               // true once at least one refinement has actually been applied
    float    prior_k_dc;                // model K_dc immediately before the last applied refinement
    float    applied_k_dc;              // model K_dc immediately after it
    float    last_delta_pct;            // (applied-prior)/prior*100 -- signed
    uint8_t  last_applied_profile_id;   // which firing produced the last applied change
    uint32_t last_applied_unix_s;       // time(NULL) at that apply, 0 if clock never synced
    char     last_refusal_reason[96];   // empty if the most recent run-end attempt applied cleanly,
                                        // or none has been attempted yet; otherwise why it did not
} adaptive_tune_zone_status_t;

// Loads the per-zone opt-in flags from this module's own NVS namespace and
// registers its two HTTP endpoints on the shared httpd server
// (wifi_provision_http_get_server()). Call once at boot; profile_executor.c
// does this from profile_executor_start(), since this file owns no init
// entry point of its own in main.c. Safe to call from app_main's task (not
// PSRAM-stacked) -- see adaptive_tune.c's top comment for the read-vs-write
// stack-safety split.
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
